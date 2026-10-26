// pgr4-native PGR4 native renderer: guest Resolve -> host texture copies.
//
// A guest Resolve(Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel, DestSliceOrFace,
// pClearColor, ClearZ, ClearStencil, pParameters) copies a rectangle of a render target
// (color RT 0-3 or depth) into a texture level, converting the format, averaging MSAA samples,
// and optionally clears the source surfaces afterwards. Here every resolve destination
// (guest physical address of the level/slice) gets a host Texture2D that holds exactly the
// texels the Xenos plugin writes into guest memory, untiled and endian-unswapped, in
// the typeless family of GetHostFormat(dest format)->resource (texture_layout.h), so the
// texture cache can bind it in place of its own texture (or copy it into one) instead of the
// guest-memory upload. A compute shader (format packing as ReXGlue's resolve shaders do it)
// writes the texels straight into the image through a UINT view. Images persist per
// destination address, format and size, so their resources and views stay valid.
//
// Arguments are decoded the way the guest D3D does it (read from its generated code,
// sub_826CBEE0): NULL source rect = the destination level's size, right/bottom rounded up to
// 8; NULL dest point = (0, 0); sample select from Flags bits 4-6 (0 = by the surface's MSAA:
// 1x sample 0, 2x average of 2, 4x average of 4); exponent bias from Flags bits 26-31;
// red/blue swap when the destination's swizzle X is not R; raw copies for depth, conversion
// for color (the guest D3D never asks for a raw color copy). The guest Resolve runs first, so
// DecodeResolve also compares the decoded copy registers with the ones the D3D wrote into its
// register shadow and logs mismatches.
//
// Oracle dumps (--pgr4_native_resolve_dump=<dir>, resolve_debug.cpp, with the self test):
// creating <dir>\dump.request arms a dump of the next frame's resolves: for each, the native
// texels (`f<F>-r<i>-native.bin`, tight rows), the guest memory Xenos wrote at the same address
// in the same frame (`-guest.bin`, tiled, big-endian; read in AfterSwap with the plugin on full
// readback, see HoldFullReadback), and one JSON line in `f<F>-resolves.jsonl`. When a frame
// resolves twice into one address, every entry's guest copy is the last write.
// tools/compare_resolves.py untiles and compares them. AfterSwap's wait holds the guest only
// when frames are recorded on the guest thread, so dumps need --pgr4_native_render_thread off.
//
// Pipeline state: Resolve sets its own compute root signature, PSO and shader-visible
// descriptor heap on the command list; callers that draw afterwards must set theirs again.
#pragma once

#include "render_targets.h"

#include <rex/graphics/xenos.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace pgr4::native {

class GpuTimer;
class Upscaler;

// What the texture cache needs to use a resolve result (TextureMod contract).
// The resources live as long as the Resolver.
struct ResolvedImage {
  uint32_t guest_base = 0;                       // physical address of the destination level/slice
  rex::graphics::xenos::TextureFormat format{};  // destination format (RB_COPY_DEST_INFO)
  uint32_t width = 0, height = 0;                // texels written from (0, 0), dest point included
  ID3D12Resource* texture = nullptr;  // state: COPY_SOURCE | ALL_SHADER_RESOURCE; typeless
  // Depth resolves only: R32_FLOAT = float20e4(d >> 8) (or unorm24 for D24S8), what the
  // texture cache's k_24_8(_FLOAT) conversion gives when the game samples the k_8_8_8_8
  // destination as depth. Same size and state as `texture`.
  ID3D12Resource* depth_r32f = nullptr;
  // With linear output (Resolver::set_linear_output), the last resolve from a float target into
  // k_2_10_10_10: the copied color before the format's clamp to [0, 1], R16G16B16A16_FLOAT, same
  // size and state as `texture`; nullptr otherwise.
  ID3D12Resource* linear = nullptr;
  uint64_t seq = 0;  // increases with every write (global resolve counter)
};

// One decoded Resolve call.
struct ResolveParams {
  SurfaceRegs regs;                          // the source surfaces (register shadow at the call)
  uint32_t source = 0;                       // 0-3 color render target, 4 depth
  uint32_t sample_select = 0;                // xenos::CopySampleSelect
  uint32_t copy_command = 1;                 // xenos::CopyCommand: 0 raw (depth), 1 convert
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;    // source rectangle, surface pixels, exclusive end
  int32_t dest_x = 0, dest_y = 0;            // destination point
  uint32_t dest_base = 0;                    // physical address of the destination level/slice
  uint32_t dest_format = 0;                  // xenos::TextureFormat (copy destination format)
  uint32_t dest_width = 0, dest_height = 0;  // destination level size, texels
  uint32_t dest_endian = 0;  // xenos::Endian (dumps only; the host image is unswapped)
  uint32_t dest_number = 0;  // xenos::SurfaceNumberFormat: 0/1 unorm/snorm, 2/3 uint/sint, 7 float
  bool dest_swap = false;    // swap red and blue
  int32_t exp_bias = 0;      // color *= 2^exp_bias (convert only)
  // Guest layout of the destination (dumps only).
  bool dest_tiled = false;
  uint32_t dest_pitch_blocks = 0;  // row pitch in texels (tiled: 32-aligned)
  uint32_t dest_extent_bytes = 0;  // guest memory the level/slice occupies
  // Clears after the copy (Flags 0x100 color, 0x200 depth+stencil) over the source rect, all
  // of it on the target even where the copy is clipped to the destination level.
  // Inside EndTiling the rect is one tile, which the guest rendered with its origin (edram_x,
  // edram_y) at the surface's EDRAM base, so that is where the clear lands in the EDRAM.
  uint32_t clear_flags = 0;
  int32_t edram_x = 0, edram_y = 0;
  float clear_color[4] = {};
  float clear_depth = 1.0f;
  uint8_t clear_stencil = 0;
};

// Decodes a Resolve hook call (`args` = r3..r10, f1, sp at entry; guest memory at `base`), after
// the guest Resolve returned, and logs where its copy registers in the D3D's shadow disagree.
// Returns false (and logs once per reason) for what is not supported yet: packed mip tails,
// 3D destinations, copy commands other than raw/convert.
bool DecodeResolve(const uint8_t* base, const HookArgs& args, ResolveParams* out);

// The Xenos plugin's --readback_resolve "fast"/"some" copy the previous resolve of the same
// address range into guest memory (one frame late), so guest memory never holds the frame the
// guest just swapped. Oracles (captures, dumps) call HoldFullReadback(true) a frame ahead, which
// switches the plugin to "full" (synchronous, exact) through the cvar registry, and
// HoldFullReadback(false) after reading guest memory, which restores the previous value. Nested
// holds are counted. On the frame-recording thread only (resolve dumps and Presenter captures
// call it from Execute*: the render thread by default).
void HoldFullReadback(bool hold);

class Resolver {
 public:
  Resolver();
  ~Resolver();

  // `queue` runs SelfTest at start when --pgr4_native_resolve_selftest is set.
  bool Initialize(ID3D12Device* device, RenderTargets* targets, ID3D12CommandQueue* queue);

  // Once per frame before recording it, like RenderTargets::BeginFrame (handles dumps too).
  // On the frame-recording thread, as are AfterSwap and Resolve.
  void BeginFrame(uint64_t frame, uint64_t frame_fence, uint64_t completed_fence);

  // After the swap's EndFrame: for a dumped frame, sleeps --pgr4_native_capture_hold_ms so the
  // Xenos plugin can execute the frame, then copies the guest memory of its resolves. That
  // sleep holds the guest only when frames are recorded on the guest thread: with the render
  // thread (the default) the guest runs on and the copies may hold a later frame.
  void AfterSwap();

  bool Resolve(ID3D12GraphicsCommandList* list, const ResolveParams& p);

  // The last resolve into this guest physical address, or nullptr.
  const ResolvedImage* Find(uint32_t guest_phys_base) const;
  // Called with a guest physical address whenever its Find result may change (an image is
  // resolved into, created, replaced or erased), so a cache can recheck only the textures
  // that sample that address.
  void SetChangedCallback(std::function<void(uint32_t guest_phys_base)> f) {
    changed_ = std::move(f);
  }
  // HDR output: resolves from a float target into k_2_10_10_10 also keep their linear color
  // (ResolvedImage::linear). Takes effect at the next resolve.
  void set_linear_output(bool on) { linear_output_ = on; }
  // With the upscaler: resolves out of its scene surface copy the upscaled color (upscaler.h).
  void set_upscaler(Upscaler* upscaler) { upscaler_ = upscaler; }
  // With GPU timing: Resolve marks its phases (GpuTimer::ResolvePhase).
  void set_gpu_timer(GpuTimer* timer) { timer_ = timer; }

  // Clears host targets of the formats PGR4 resolves from (8888, 7e3, 16F, 32F, D24FS8/D24S8;
  // 1x/2x/4x) to known values, resolves them into the destination formats PGR4 uses on its own
  // command list, and checks the read-back texels against hand-computed Xenos encodings. Logs
  // each case; returns true when all pass. Initialize runs it with --pgr4_native_resolve_selftest.
  bool SelfTest(ID3D12CommandQueue* queue);

 private:
  struct Image;
  // Every image resolved into one guest address, one per format and size. Images are never
  // destroyed: the texture cache binds them in place of its own textures and the draws keep
  // descriptors of them, and an address that alternates between formats or sizes reuses its
  // images instead of recreating one per resolve.
  struct Destination {
    std::vector<std::unique_ptr<Image>> images;
    Image* current = nullptr;  // the last one written (Find)
  };
  struct Dump;
  struct DumpDeleter {
    void operator()(Dump* dump) const;
  };
  bool CreatePipelines();
  // The image at p.dest_base for p's format and size, created if missing; `*dest_out`: its
  // destination.
  Image* GetImage(const ResolveParams& p, Destination** dest_out);
  // Copies the clipped rectangle `src` of rt to (dx, dy) in the destination image.
  bool Copy(ID3D12GraphicsCommandList* list, const ResolveParams& p, RenderTarget* rt,
            const D3D12_RECT& src, int32_t dx, int32_t dy);

  // Oracle dumps and the self test (resolve_debug.cpp), and what they use of resolve.cpp.
  void InitializeOracles(ID3D12CommandQueue* queue);  // as their cvars ask
  // While a dump records, reads back `img`'s texels from (0, 0) to (width, height).
  void DumpRecord(ID3D12GraphicsCommandList* list, const ResolveParams& p, const ResolvedImage& img,
                  uint32_t width, uint32_t height);
  void DumpBeginFrame(uint64_t completed_fence);
  static DXGI_FORMAT DestResourceFormat(uint32_t dest_format);  // of the host image
  static uint32_t DestBytesPerTexel(uint32_t dest_format);
  static bool IsFloatDest(uint32_t dest_format);
  // false: the generic copy pipelines only; true: each kind's specialized one, compiled on
  // first use on the calling thread.
  void SetSpecialization(bool sync);
  void EraseDestination(uint32_t dest_base);  // and every image of it

  ID3D12Device* device_ = nullptr;
  RenderTargets* targets_ = nullptr;
  struct Pipelines;
  std::unique_ptr<Pipelines> pipelines_;
  ComPtr<ID3D12DescriptorHeap> heap_;  // shader visible: per-frame rings of view tables
  uint32_t heap_stride_ = 0;
  uint32_t heap_next_ = 0;
  bool b5g6r5_store_ = false;  // typed UAV store of B5G6R5_UNORM (k_5_6_5 / k_6_5_5 images)
  bool linear_output_ = false;
  std::unordered_map<uint32_t, Destination> destinations_;
  size_t image_count_ = 0;
  uint64_t frame_ = 0, frame_fence_ = 0;
  uint64_t seq_ = 0;
  std::function<void(uint32_t)> changed_;
  std::unique_ptr<Dump, DumpDeleter> dump_;
  GpuTimer* timer_ = nullptr;
  Upscaler* upscaler_ = nullptr;
};

}  // namespace pgr4::native
