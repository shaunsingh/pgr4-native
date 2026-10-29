// pgr4-native PGR4 native renderer: frame generation with AMD FidelityFX Frame Generation, through
// the FidelityFX API (fidelityfx.h; amd_fidelityfx_framegeneration_dx12.dll beside the loader, or
// what OptiScaler puts behind it). --pgr4_native_frame_generation, with the upscaler and the frame
// pacer.
//
// Each frame with the scene's inputs is presented after a frame generated between it and the one
// before. The upscaler's Upscale records Prepare with the scene's depth and motion at the render
// size; the frame then draws into image() in place of the back buffer (D3D12Device::SetFrameImage),
// Generate records the generation of the frame before it from the last image and this one, and
// EndFrame presents that frame, then the image (D3D12Device::SetGeneratedFrame). Both presents are
// FIFO and back to back, so the display shows them on consecutive refreshes, and the pacer, which
// counts a frame shown with its second present (frame_pacer.h), begins the frames at half the
// refresh rate, in step with it.
//
// Not FidelityFX's frame generation swapchain: that presents from threads of its own on its own
// clock, signals its frame latency waitable before the display shows a frame, and waits for its
// fences by spinning, a core each, and on the Deck the CPU's power is the GPU's (td-308 to td-310:
// 28 to 38 frames per second with the GPU at 336 to 610 MHz, against 87 at 1275 MHz without frame
// generation, td-311). FidelityFX's context alone generates frames for any presenter
// (FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY).
//
// The HUD is not interpolated: drawn over the scene after its motion was taken, it would smear
// between two frames (td-314). FidelityFX takes it from the frame's image where the image and a
// HUD-less copy of it differ (HUDLessColor), which needs the two to match exactly elsewhere: the
// copy is the front buffer's resolve (the last resolve before the previous Swap) run early
// (Resolver::ResolveCopy), before the frame's first blended draw into its surface after an
// opaque one, the final composite (BeforeDraw; draw logs of td-265: pixel shader
// 6B06B8CDADD9CCBF, then 40 blended quads of HUD), and the Presenter draws it into hudless() as
// it draws the front buffer into the image (SwapCopy). The generation reads the
// scene's part of both (Generate's rect). A frame without the scene's inputs (the upscaler off, a
// loading screen) is presented alone, and the next one starts the generation over: a frame ID not
// one more than the last resets it.
//
// Threading: the thread that records the frames (NativeRenderer::Execute*).
#pragma once

#include "d3d12_device.h"
#include "fidelityfx.h"
#include "resolve.h"

#include <ffx_api_types.h>

#include <cstdint>
#include <optional>

namespace pgr4::native {

class FrameGeneration {
 public:
  // Whether --pgr4_native_frame_generation asks for it.
  static bool Requested();

  FrameGeneration(D3D12Device& device, Resolver& resolver) : device_(device), resolver_(resolver) {}
  ~FrameGeneration();
  FrameGeneration(const FrameGeneration&) = delete;
  FrameGeneration& operator=(const FrameGeneration&) = delete;

  // Loads the FidelityFX API; false (logged) without it.
  bool Initialize();

  // Upscaler::Upscale, after the upscaling of frame `frame`: records the preparation of its depth
  // and motion (in COMPUTE_READ) into `list`. The context and images follow the back buffers'
  // size and format first, with the GPU idle.
  void Prepare(uint64_t frame, ID3D12GraphicsCommandList* list, const FfxApiResource& depth,
               const FfxApiResource& motion, FfxApiDimensions2D render_size,
               FfxApiFloatCoords2D jitter, FfxApiFloatCoords2D motion_scale,
               const FfxCamera& camera, float frame_ms, bool reset);
  // Whether Prepare recorded frame `frame`, which is then drawn into image() and generated.
  bool prepared(uint64_t frame) const { return context_ && prepared_frame_ == frame; }
  // The frame's image: the back buffers' size and format, in PRESENT outside the frame's draws.
  ID3D12Resource* image() const { return image_.Get(); }
  D3D12_CPU_DESCRIPTOR_HANDLE image_rtv() const { return Rtv(0); }
  // The image without its HUD, of the same kind.
  ID3D12Resource* hudless() const { return hudless_.Get(); }
  D3D12_CPU_DESCRIPTOR_HANDLE hudless_rtv() const { return Rtv(1); }

  // The HUD-less copy. OnResolve: every resolve. OnSwap, `front_base` the Swap's front buffer (its
  // guest physical address).
  void OnResolve(const ResolveParams& p) { last_resolve_ = p; }
  void OnSwap(uint32_t front_base);
  // Before each draw of frame `frame` (`regs` its register file): when the draw is the HUD's first,
  // records the copy into `list` and returns true (the list's compute state changed).
  bool BeforeDraw(ID3D12GraphicsCommandList* list, uint64_t frame, const uint32_t* regs) {
    if (!front_ || prepared_frame_ != frame || copy_frame_ == frame) [[likely]]
      return false;
    return CopyAtHud(list, frame, regs);
  }
  // Frame `frame`'s copy (ResolvedImage of the front buffer's format and size), nullptr without.
  const ResolvedImage* hudless_source(uint64_t frame) const {
    return copy_frame_ == frame ? copy_ : nullptr;
  }

  // After frame `frame` was drawn into image() and, with `hudless`, into hudless(), `rect` the
  // scene's part of it: records the generation of the frame before it into `list`. Returns that
  // frame (image()'s size, format and state), nullptr (logged once) on failure.
  ID3D12Resource* Generate(ID3D12GraphicsCommandList* list, uint64_t frame, FfxApiRect2D rect,
                           bool hudless);

 private:
  // The context and images for back buffers of this size and format, the context with FfxDebug's
  // watermark or not (the driver reads its variable at creation); false (logged) on failure.
  bool Resize(uint32_t width, uint32_t height, bool hdr, bool debug);
  void Destroy();
  D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t i) const;
  bool CopyAtHud(ID3D12GraphicsCommandList* list, uint64_t frame, const uint32_t* regs);

  D3D12Device& device_;
  Resolver& resolver_;
  const FfxApi* api_ = nullptr;
  void* context_ = nullptr;  // ffxContext
  ComPtr<ID3D12Resource> image_, hudless_;
  ComPtr<ID3D12Resource> output_;          // the generated frame
  ComPtr<ID3D12DescriptorHeap> rtv_heap_;  // image_'s RTV, hudless_'s
  uint32_t rtv_stride_ = 0;
  uint32_t width_ = 0, height_ = 0;
  bool hdr_ = false;
  bool debug_ = false;                    // FidelityFX's debug view and the watermark
  uint64_t prepared_frame_ = UINT64_MAX;  // the last frame Prepare recorded
  ResolveParams last_resolve_;
  std::optional<ResolveParams> front_;      // the front buffer's resolve
  uint64_t composited_frame_ = UINT64_MAX;  // the last frame drawn opaque into front_'s surface
  uint64_t copy_frame_ = UINT64_MAX;        // the last frame CopyAtHud ran in
  const ResolvedImage* copy_ = nullptr;     // its copy
};

}  // namespace pgr4::native
