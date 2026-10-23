// pgr4-native PGR4 native renderer: guest Swap -> the debug window, and captures.
//
// OnSwap reads the front buffer texture of the guest Swap (r4, a D3DBaseTexture; fetch
// constant at +28, the same object the frame's last Resolve wrote), finds its host image in the
// Resolver (RTResolve) and draws it into the debug window's back buffer with the fetch
// constant's swizzle and the display controller's gamma ramp (present.cpp). Without a
// resolved image the back buffer is cleared to black.
//
// Captures (the image check against Xenos): --pgr4_native_capture_dir (empty by default: no
// requests polled) and either the file <dir>\capture.request (deleted when taken, read every 16
// frames), a mark (F12), or --pgr4_native_capture_frame=N. A request at Swap F-1 switches the
// Xenos plugin to full readback (HoldFullReadback in resolve.h; with "fast" guest memory holds
// the previous frame's resolves) and captures frame F:
//   f<F>-native.png  the native front buffer (read back after the frame's fence)
//   f<F>-xenos.png   the same front buffer as the Xenos plugin wrote it into guest memory
//                    (read after the guest thread was held in the Swap hook for
//                    --pgr4_native_capture_hold_ms so the plugin's GPU thread finishes frame
//                    F), untiled with TextureMod's UntileTexture
// and logs the PSNR (RGB, 8 bit) of the pair and appends it to <dir>\captures.csv.
// Both images are the front buffer before the presenter's gamma ramp.
//
// Threading: the renderer's OnSwap only (guest thread). With the render thread (renderer.h)
// ReadSwap and CaptureRequested run on the guest thread and OnSwap/AfterEndFrame on the
// render thread; the renderer runs the Swaps around a capture synchronously (the guest thread
// waits in its Swap hook), so the hold and the guest memory read keep their meaning.
#pragma once

#include "d3d12_device.h"
#include "renderer.h"

#include <cstdint>
#include <memory>

namespace pgr4::native {

// --pgr4_native_luma_probe: the mean brightness (0-255, before the gamma ramp) of each quarter
// of three rows of a front buffer, at 10, 40 and 80 % of its height (left to right), read
// kFramesInFlight frames after it was drawn. Quarters, as a whole-row mean hides a flash
// confined to one object.
struct LumaProbe {
  static constexpr uint32_t kRows = 3, kSegments = 4;
  uint64_t frame = 0;  // the frame they belong to (0: none yet)
  float luma[kRows][kSegments] = {};
};

// What OnSwap needs from the guest Swap call (read at the call).
struct SwapCall {
  uint32_t texture = 0;  // r4: the front buffer D3DBaseTexture (0 = none)
  uint32_t fetch[6] = {};  // its fetch constant (+28), as stored (virtual address)
  // -1: OnSwap checks for a capture request itself; 0/1: the caller did (CaptureRequested).
  int8_t capture_request = -1;
};

class Presenter {
 public:
  explicit Presenter(NativeRenderer& renderer);
  ~Presenter();
  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;

  bool Initialize();

  // Reads the Swap's front buffer texture object from guest memory.
  static void ReadSwap(const uint8_t* base, const HookArgs& args, SwapCall* out);
  // A capture was requested (file, F12 or --pgr4_native_capture_frame == frame).
  bool CaptureRequested(uint64_t frame);
  // A capture runs its Swap and the one before with the guest thread held (to read the Xenos
  // plugin's copy of the frame, --pgr4_native_capture_xenos).
  bool capture_holds_guest() const;

  // In NativeRenderer::OnSwap before D3D12Device::EndFrame: records the blit into the back
  // buffer (PRESENT state on entry and exit) and, for a capture, the readback copy. Sets its own
  // root signature, pipeline and descriptor heap on the list. `shown`: the frame is presented
  // (false: held, no blit).
  void OnSwap(const SwapCall& swap, bool shown);

  // In NativeRenderer::OnSwap after EndFrame: finishes a pending capture (with the Xenos copy:
  // waits for the frame's fence, holds the guest thread, reads guest memory, writes the PNGs;
  // without it: hands a completed frame's image to a thread that writes its PNG).
  void AfterEndFrame();

  bool front_buffer_found() const { return last_found_; }
  const LumaProbe& luma_probe() const;

 private:
  struct Impl;
  NativeRenderer& renderer_;
  std::unique_ptr<Impl> impl_;
  bool last_found_ = false;
};

}  // namespace pgr4::native
