// pgr4-native PGR4 native renderer: host render targets for the guest's EDRAM
// surfaces, and the guest Clear calls.
//
// No EDRAM emulation and no tiles: every guest surface (RB_SURFACE_INFO pitch and MSAA,
// RB_COLOR_INFO / RB_DEPTH_INFO base and format) gets one host render target at full size,
// whatever predicated tiling the guest D3D does behind the API.
//
// Aliasing (tools/rt_passes.py over tap traces of the menus and the race): PGR4 reuses EDRAM base 0
// for most passes of a frame (shadow-map depth, the 128x128 mip chain, the 1280x720 2x scene,
// the 1x post chain, 80-pitch luminance reductions, the UI), and the 256x256 4x cube faces
// sit at color base 224 over depth base 0. Every surface use starts with a Clear/ClearF of
// that surface (BeginTiling's ClearF for the scene), a Resolve with clear flags on the same
// surface, or a full-screen draw that overwrites its color, so surfaces at the same base are
// separate host targets and rendered data never moves between them. Cleared values do: the
// post chain's first 1x pass writes RGB only and keeps the alpha EndTiling's resolve clears
// left in the EDRAM (the motion blur mask: every tile is rendered at the base, so the 1x
// surface's rows 0-479 hold the last tile's clear, alpha 0, and rows 480-719 the first
// tile's, alpha 1). Clears
// are therefore also recorded as EDRAM fills that other surfaces over the same tiles take
// over when they are next used (EdramFill below).
//
// Targets are keyed by the host view format, not the guest format: guest formats with one host
// format (2_10_10_10_FLOAT, its _AS_16_16_16_16 variant and 16_16_16_16_FLOAT, all
// R16G16B16A16_FLOAT; 8_8_8_8 and its _GAMMA variant; 2_10_10_10 and its _AS_10_10_10_10
// variant) share one target at the same base, pitch and MSAA, and so its contents. The pair
// that must share storage is 2_10_10_10_FLOAT and 2_10_10_10_FLOAT_AS_16_16_16_16 (PGR4 flips
// between them inside one pass for blending precision), as in ReXGlue's host RT path. It is the
// only pair PGR4's traces show on one surface, so sharing the others' targets is harmless here.
// The D3D's own clear quads at half pitch with 4x MSAA (inside Clear/ClearF) are not game
// draws and never reach this module.
//
// Threading: DecodeClear runs on the guest thread; everything else on the thread that records
// the frames (NativeRenderer::Execute*: the render thread by default, else the guest thread).
#pragma once

#include "d3d12_device.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

namespace pgr4::native {

struct HookArgs;

// Raw EDRAM surface registers of the current render targets.
struct SurfaceRegs {
  uint32_t surface_info = 0;    // RB_SURFACE_INFO (0x2000)
  uint32_t color_info[4] = {};  // RB_COLOR_INFO (0x2001), RB_COLOR1..3_INFO (0x2003..0x2005)
  uint32_t depth_info = 0;      // RB_DEPTH_INFO (0x2002)
};

// From a register file indexed by register number (DrawState::regs).
SurfaceRegs SurfaceRegsFromRegisters(const uint32_t* regs);
// From the guest device's register shadow (block 0x2000 at device+10368).
SurfaceRegs SurfaceRegsFromDevice(const uint8_t* base, uint32_t device);

// Samples per pixel of RB_SURFACE_INFO's msaa_samples (bits 16-17): 1, 2 or 4.
inline uint32_t SurfaceSamples(uint32_t surface_info) {
  const uint32_t msaa = (surface_info >> 16) & 3;
  return msaa >= 2 ? 4 : msaa == 1 ? 2 : 1;
}

// Guest D3D device fields this module reads (tools/scan_d3d_surface.py, d3d_state.h).
constexpr uint32_t kDevShadow2000 = 10368;     // registers 0x2000..0x2012
constexpr uint32_t kDevShadow2100 = 10444;     // registers 0x2100..0x2114
constexpr uint32_t kDevShadow2300 = 10680;     // registers from 0x2300 (the RB_COPY_* block)
constexpr uint32_t kDevColorSurfaces = 12432;  // 4 x guest D3D surface object
constexpr uint32_t kDevDepthSurface = 12448;
// Predicated tiling, as BeginTiling (fn_826D1C38) stores it: the tile count, each tile's
// rectangle in surface pixels {x1, y1, x2, y2} (16 bytes), and each tile's origin rounded down
// to 32 pixels {x, y} (8 bytes), the surface pixel the guest renders at the EDRAM base.
constexpr uint32_t kDevTileCount = 12740;
constexpr uint32_t kDevTileRects = 12744;
constexpr uint32_t kDevTileOrigins = 12984;
constexpr uint32_t kMaxTiles = 15;

// One host render target.
struct RenderTarget {
  bool depth = false;
  uint32_t edram_base = 0;  // tiles
  uint32_t pitch = 0;       // pixels (RB_SURFACE_INFO surface_pitch)
  uint32_t samples = 1;     // 1, 2 or 4
  uint32_t width = 0, height = 0;
  uint32_t guest_format = 0;  // last xenos::ColorRenderTargetFormat / DepthRenderTargetFormat
  DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;         // RTV or DSV
  DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;          // color, or the depth plane
  DXGI_FORMAT stencil_srv_format = DXGI_FORMAT_UNKNOWN;  // depth only: the stencil plane
  ComPtr<ID3D12Resource> resource;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_CPU_DESCRIPTOR_HANDLE view{};  // RTV or DSV
  uint32_t view_index = 0;
  uint64_t last_frame = 0;  // frame of the last use, for eviction
  uint64_t fill_seq = 0;    // EDRAM fills up to this sequence are applied (SyncEdram)
};

// Host targets for one draw.
struct BoundTargets {
  RenderTarget* color[4] = {};
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[4] = {};
  uint32_t color_count = 0;  // highest bound color index + 1 (null RTVs below it)
  RenderTarget* depth = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
  uint32_t samples = 1;
  uint32_t width = 0, height = 0;  // smallest bound target (the draw's maximum extent)
};

// Decoded Clear / ClearF arguments.
struct ClearParams {
  uint32_t flags = 0;   // D3DCLEAR_TARGET0..3 = 1..8, ZBUFFER = 0x10, STENCIL = 0x20
  float color[4] = {};  // RGBA
  float depth = 1.0f;
  uint8_t stencil = 0;
  std::vector<D3D12_RECT> rects;  // surface pixels; empty: whole surface
  // The EDRAM the guest clears, in pixels from the surface's EDRAM base, where it is not
  // `rects`: under predicated tiling every tile is rendered at the base, so BeginTiling's
  // ClearF clears the largest tile's extent there once (and `rects` holds every tile), and
  // EndTiling's resolve clears land at the base whatever the tile's position.
  std::vector<D3D12_RECT> edram_rects;
};

// One Clear / ClearF call, decoded from its arguments and the device (guest memory).
struct ClearCall {
  ClearParams p;
  SurfaceRegs regs;
  uint32_t bound_colors = 0;  // bit i: a surface is bound as render target i
  bool depth_bound = false;
  uint32_t height = 0;
};

class RenderTargets {
 public:
  RenderTargets();
  ~RenderTargets();

  bool Initialize(ID3D12Device* device);

  // Host targets for a draw. color_mask bit i: guest RT i is used (its RB_COLOR_MASK nibble is
  // non-zero); depth_used: depth or stencil test/write enabled. height: rows the draw can
  // touch (bottom of its scissor/viewport, clamped to 2048). Records the barriers to
  // RENDER_TARGET / DEPTH_WRITE into `list`; the caller sets the RTVs/DSV from `out`.
  bool Bind(ID3D12GraphicsCommandList* list, const SurfaceRegs& regs, uint32_t color_mask,
            bool depth_used, uint32_t height, BoundTargets* out);

  // Guest Clear / ClearF (HookFn kClear / kClearF), after the guest call returned: the surfaces
  // bound in the device's register shadow and the clear values (reads guest memory only);
  // false: nothing to clear. Clear then clears their host targets.
  static bool DecodeClear(const uint8_t* base, const HookArgs& args, ClearCall* out);
  void Clear(ID3D12GraphicsCommandList* list, const SurfaceRegs& regs, uint32_t bound_colors,
             bool depth_bound, uint32_t height, const ClearParams& p);

  // The host target of guest color RT `index` (0..3) or of the depth surface, created if
  // missing (resolve sources). nullptr for an invalid format.
  RenderTarget* GetColor(const SurfaceRegs& regs, uint32_t index, uint32_t height);
  RenderTarget* GetDepth(const SurfaceRegs& regs, uint32_t height);

  // Clears color target `rt` to the EDRAM fills recorded over its tiles since its last use.
  // Bind and Clear do this; other readers of a target (resolve sources) call it first.
  void SyncEdram(ID3D12GraphicsCommandList* list, RenderTarget* rt);

  static void Transition(ID3D12GraphicsCommandList* list, RenderTarget* rt,
                         D3D12_RESOURCE_STATES state);

  // Once per frame before recording it (renderer start and after each Swap): `frame_fence`
  // is the fence the frame being recorded will signal, `completed_fence` the GPU's progress.
  // Frees replaced targets the GPU is done with and drops targets unused for kMaxAge frames.
  void BeginFrame(uint64_t frame, uint64_t frame_fence, uint64_t completed_fence);

  // Forgets every recorded EDRAM fill, for clears that are not guest content (the resolve self
  // test), so no game surface over the same tiles takes them over.
  void DropAllFills() { fills_.clear(); }

  // The draw log (DrawRenderer) while one is open: target creations and EDRAM fills applied.
  void SetLog(std::FILE* log) { log_ = log; }

 private:
  static constexpr uint64_t kMaxAge = 120;

  // EDRAM residue. A color clear leaves one value in the EDRAM tiles it covers, and another
  // surface of the same storage format over those tiles reads it until something renders
  // there. A fill is one cleared rectangle in EDRAM terms: dword columns and sample rows on the
  // cleared surface's tile grid (tiles of 80 dwords x 16 sample rows, `pitch` tiles per row
  // from tile `base`). A draw or a depth clear over any of its tiles drops a fill whole.
  struct EdramFill {
    uint64_t seq = 0;
    uint32_t base = 0, pitch = 1;  // tiles
    uint32_t x0 = 0, x1 = 0;       // dword columns within a tile row
    uint32_t y0 = 0, y1 = 0;       // sample rows from base's tile row
    uint32_t format = 0;           // xenos color format as stored (the _AS_ variants folded)
    float color[4] = {};
    uint32_t first_tile() const { return base + y0 / 16 * pitch + x0 / 80; }
    uint32_t end_tile() const { return base + (y1 - 1) / 16 * pitch + (x1 - 1) / 80 + 1; }
  };
  static constexpr size_t kMaxFills = 32;
  // `r`: pixels from rt's EDRAM base.
  void RecordFill(const RenderTarget& rt, const D3D12_RECT& r, const float color[4]);
  // Drops the fills over rt's first `rows` rows of pixels.
  void DropFills(const RenderTarget& rt, uint32_t rows);

  RenderTarget* Get(bool depth, uint32_t surface_info, uint32_t info, uint32_t height);
  void Retire(ComPtr<ID3D12Resource> resource);
  bool Create(RenderTarget* rt, uint32_t height);
  uint32_t AllocView(bool depth);

  ID3D12Device* device_ = nullptr;
  ComPtr<ID3D12DescriptorHeap> rtv_heap_, dsv_heap_;
  uint32_t rtv_stride_ = 0, dsv_stride_ = 0;
  std::vector<uint32_t> free_rtv_, free_dsv_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> targets_;
  // Get's last hits, direct-mapped by key, in front of targets_ (cleared when targets are
  // dropped); and the RTV heap start (null RTVs).
  struct GetMemo {
    uint64_t key = 0;
    RenderTarget* rt = nullptr;
  };
  GetMemo get_memo_[16];
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_start_{};
  struct Retired {
    ComPtr<ID3D12Resource> resource;
    uint64_t fence;
  };
  std::vector<Retired> retired_;
  uint64_t frame_ = 0, frame_fence_ = 0;
  std::vector<EdramFill> fills_;  // oldest first
  uint64_t fill_seq_ = 0;
  std::FILE* log_ = nullptr;
};

}  // namespace pgr4::native
