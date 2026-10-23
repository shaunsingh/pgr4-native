// pgr4-native PGR4 native renderer: host of the native D3D12 path.
//
// The guest D3D hooks in pgr4_d3d_trace.cpp forward every call to the guest D3D (without
// --pgr4_native_primary the Xenos plugin keeps rendering, as the oracle) and, with
// --pgr4_native, then call the NativeRenderer entry points below with the guest device and the
// call's arguments. Only the outermost
// draw/clear/resolve/Swap call is passed on (the D3D's internal clear, copy and Swap quads are
// not game draws); a top-level BeginVertices reaches OnDraw at the entry of the next hooked
// call, once the game has written its vertices. The renderer owns its own D3D12 queue, window
// and swapchain, separate from ReXGlue's presenter (d3d12_device.h).
//
// Threading: the guest D3D device is used by one guest thread at a time; all On* entry points
// are called on that thread, after the original guest function returned, and are serialized
// by the renderer. Modules may assume single-threaded access from the On* calls.
//
// With --pgr4_native_render_thread (default on; false keeps everything on the guest thread as
// above) the On* calls only capture, on the guest thread, what each call reads from guest
// memory: the register changes since the last draw (the tracker's TakeChanges), the
// DrawState, DrawRenderer::Prepare's shader lookup and vertex/index copies,
// and the decoded Clear/Resolve/Swap arguments. They queue it in batches to the render thread
// ("PGR4 native render"), which replays it into the modules with its own copy of the register
// file. From then on the command list, the D3D12 objects and every module's state belong to
// that thread (DrawRenderer's vertex upload pool and shader lookup stay on the guest thread).
// The guest thread blocks at Swap while the render thread is more than one frame behind, and
// waits for the render thread to finish the Swaps around a capture (present.h).
#pragma once

#include <rex/cvar.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

REXCVAR_DECLARE(bool, pgr4_native);                // master switch, off by default
REXCVAR_DECLARE(std::string, pgr4_native_csv);     // per-frame CSV of the native path ("" = off)
REXCVAR_DECLARE(bool, pgr4_native_render_thread);  // record on a render thread (see above)

namespace pgr4::native {

class D3D12Device;
class DrawRenderer;
class FramePacer;
class GpuTimer;
class Presenter;
class RenderTargets;
class Resolver;
class TextureCache;
struct ClearCall;
struct DrawPrep;
struct DrawState;
struct ResolveParams;
struct SwapCall;

// Guest D3D entry points the native path receives (addresses in the base-disc default.xex;
// names from native/src/d3d_surface.inc).
enum class HookFn : uint32_t {
  kBeginVertices = 0x826CFA60,        // draw; the game writes the vertices after it returns
  kDrawVerticesUP = 0x826CFF10,       // draw
  kDrawVertices = 0x826CFF58,         // draw
  kDrawIndexedVertices = 0x826D0340,  // draw
  kDrawIndx2Inline = 0x826D0798,      // draw, called only from inside the D3D library
  kDrawIndx2Split = 0x826DC888,       // draw, called only from inside the D3D library
  kClearF = 0x826D18E0,
  kClear = 0x826D1A08,
  kResolve = 0x826CBEE0,
  kSwap = 0x826CB148,
};

// The guest D3D's predicated tiling calls a hooked call can be nested in. BeginTiling
// (fn_826D1C38) clears through ClearF with the largest tile's extent at the EDRAM base, meaning
// every tile; EndTiling (fn_826D20D0) resolves each tile, its rectangle in surface pixels and
// the EDRAM holding the tile from its origin (the tile list at device+12740, render_targets.h).
enum class TilingCall : uint8_t { kNone, kBegin, kEnd };

// Raw PPC arguments of one hooked call. Integer arguments in r3..r10 and float arguments in
// f1..f4 are captured at entry, before the guest function runs (it clobbers them).
// Argument meaning follows the XDK D3DDevice_* signatures [INFERENCE, check against the
// tap traces' `C` lines before relying on one]:
//   DrawIndexedVertices(dev, prim, base_vertex, start_index, index_count)
//   DrawVertices(dev, prim, start_vertex, vertex_count)
//   DrawVerticesUP(dev, prim, vertex_count, data_ptr, stride)
//   BeginVertices(dev, prim, vertex_count, stride) -> r3 = pointer the game writes into
//   Clear(dev, count, rects_ptr, flags, color; z in f1, stencil in r9)
//   ClearF(dev, flags, rect_ptr, color_ptr; z in f1, stencil in r8)
//   (a float argument takes the next integer register's slot)
//   Resolve(dev, flags, src_rect_ptr, dest_texture, dest_point_ptr, level, slice,
//           clear_color_ptr; clear_z in f1, clear_stencil, params_ptr)
//   Swap(dev, front_buffer_texture, scaler_params_ptr)
struct HookArgs {
  HookFn fn;
  uint32_t device;  // r3 at entry: the guest D3D device (a guest virtual address)
  uint32_t r[8];    // r3..r10 at entry; r[0] == device
  uint32_t sp;      // r1 at entry: arguments past r10 are on the guest stack (Resolve's
                    // ClearStencil and pParameters; offsets in resolve.h)
  double f[4];      // f1..f4 at entry
  uint64_t ret;     // r3 after the guest function returned
  TilingCall tiling = TilingCall::kNone;  // the tiling call this one is nested in
};

// Per-frame counters of the native path (also the frame CSV's columns).
struct FrameStats {
  uint64_t draws = 0, clears = 0, resolves = 0;
  uint64_t state_fail = 0;                             // draws ReadDrawState rejected
  uint64_t draw_ns = 0, clear_ns = 0, resolve_ns = 0;  // CPU time inside the On* calls
  uint64_t swap_ns = 0;                                // OnSwap: record, submit, present
  // Guest thread: time in the On* calls (render thread mode: capturing and queueing) and the
  // part of it spent waiting for the render thread.
  uint64_t hook_ns = 0, hook_wait_ns = 0;
  uint64_t hook_max_ns = 0;  // render thread mode: the longest single On* call, without waits
  uint64_t reg_writes = 0;   // render thread mode: register values replayed
  // Parts of swap_ns: waiting for the texture untile workers, then D3D12Device::EndFrameTimes.
  uint64_t swap_untile_ns = 0, swap_submit_ns = 0, swap_present_ns = 0, swap_fence_ns = 0;
  uint64_t frames_held = 0;  // Swaps not presented (DrawRenderer::FrameIncomplete)
  uint64_t submits = 0;      // lists submitted before the Swap's (--pgr4_native_submit_draws)
  // The guest's pad reads since the Swap before (rex_xam_take_frame_pad, taken at the Swap).
  uint64_t pad = 0;
  // Pacing (NowNs clock): when the guest thread began the frame (its previous Swap returned) and
  // reached its Swap, how long FramePacer::WaitToBegin held it before it began, and the frame's
  // present number (D3D12Device::EndFrameTimes::presents; 0: not presented).
  uint64_t guest_begin_ns = 0, guest_swap_ns = 0, pace_wait_ns = 0, present = 0;
};

class NativeRenderer {
 public:
  // The renderer, created on first use when --pgr4_native is on; nullptr when it is off or
  // when D3D12 initialization failed (logged once). Cheap enough to call per hook.
  static NativeRenderer* Get();
  // The renderer while it draws the game: Get(), except after the renderer switch handed the
  // game to the Xenos plugin (primary mode, bind_renderer_switch). The hooks use this one; while
  // it is null the guest D3D runs unchanged and nothing is tracked.
  static NativeRenderer* Live() {
    NativeRenderer* r = Get();
    return r && r->live_.load(std::memory_order_relaxed) ? r : nullptr;
  }

  // True while the renderer presents the game's frames and paces them on the display
  // (frame_pacer.h): the guest D3D then presents without waiting for a vblank
  // (Pgr4PresentationInterval in pgr4_d3d_trace.cpp). Any thread.
  static bool PacesFrames() { return paces_frames_.load(std::memory_order_relaxed); }

  // Hook entry points. `base` is the guest memory base the hook received (guest virtual
  // address a is at base + a, plus 0x1000 for a >= 0xE0000000 like the generated code).
  void OnDraw(const uint8_t* base, const HookArgs& args);
  void OnClear(const uint8_t* base, const HookArgs& args);
  void OnResolve(const uint8_t* base, const HookArgs& args);
  void OnSwap(uint8_t* base, const HookArgs& args);
  // A Swap while the Xenos plugin draws (Live() is null): the renderer switch takes the game
  // back.
  void OnSwapSuspended();

  // For modules.
  D3D12Device& device() { return *device_; }
  ID3D12Device* d3d12() const;
  ID3D12CommandQueue* queue() const;
  // The frame's direct command list: open from the previous Swap until OnSwap closes and
  // submits it. Record into it from the On* calls only (the render thread, in that mode).
  ID3D12GraphicsCommandList* command_list() const;
  // Swaps recorded so far (= index of the frame being recorded).
  uint64_t frame_index() const { return frame_index_; }
  // Fence value OnSwap will signal for the frame being recorded; a resource last used in this
  // frame may be freed once device().completed_fence() >= this.
  uint64_t frame_fence() const;
  // Modules owned by the renderer.
  TextureCache& textures() { return *textures_; }
  RenderTargets& render_targets() { return *rt_; }
  Resolver& resolver() { return *resolver_; }

  ~NativeRenderer();

 private:
  NativeRenderer() = default;
  bool Initialize();
  void WriteCsv(const FrameStats& s, uint64_t frame_ns);
  // Starts a frame's module state (RenderTargets/Resolver BeginFrame) at its first On* call.
  void BeginFrame() {
    if (!frame_begun_) BeginFrameModules();
  }
  void BeginFrameModules();
  // The recording half of the On* calls (on the guest thread, or the render thread).
  void ExecuteDraw(const DrawState& state, const DrawPrep* prep);
  void ExecuteClear(const ClearCall& clear);
  void ExecuteResolve(const ResolveParams& p);
  void ExecuteSwap(const SwapCall& swap);
  // After a draw: submits the frame so far once --pgr4_native_submit_draws draws were recorded
  // since the last submission.
  void SubmitEarly();
  // A failed submission or present: logs and turns the native path off for the rest of the run.
  void Fail();
  // Primary mode, at a Swap before anything is recorded into the back buffer: who presents into
  // the game window. The renderer while it draws the game and no XAM dialog is up; ReXGlue's
  // presenter otherwise (the dialogs are its ImGui overlay, and after the renderer switch it
  // shows the Xenos plugin's frames). A window has one swapchain at a time: the presenter's
  // surface is disconnected before ours is created, and ours released before it is connected
  // again. Returns false when the swapchain cannot be created (logged).
  bool OwnGameWindow(bool own);
  // Render thread mode.
  struct Queue;
  void QueueDraw(const uint8_t* base, const HookArgs& args);
  void QueueClear(const uint8_t* base, const HookArgs& args);
  void QueueResolve(const uint8_t* base, const HookArgs& args);
  void QueueSwap(uint8_t* base, const HookArgs& args);
  void RenderThreadMain();
  // Renderer switch. The key bind_renderer_switch (F9; a Steam Input binding can send it from a
  // back grip) requests it on the UI thread; the guest thread takes the request at its next
  // Swap. A key rather than a pad combination: the game reads every button of the pad.
  // HandOver: the Xenos plugin draws from the next frame on; TakeBack: native again.
  bool SwitchRequested() {
    return switch_requested_.load(std::memory_order_relaxed) &&
           switch_requested_.exchange(false, std::memory_order_relaxed);
  }
  void HandOver(uint8_t* base, uint32_t device);
  void TakeBack();

  static inline std::atomic<bool> paces_frames_{false};  // pacer_->pacing(), for any thread
  std::atomic<bool> live_{true};
  std::atomic<bool> switch_requested_{false};
  bool switch_bound_ = false;  // primary mode with --pgr4_native_renderer_switch

  std::unique_ptr<Queue> queue_;  // render thread mode only

  std::unique_ptr<D3D12Device> device_;
  bool primary_ = false;  // --pgr4_native_primary: presenting into the game window
  std::unique_ptr<RenderTargets> rt_;
  std::unique_ptr<Resolver> resolver_;
  std::unique_ptr<TextureCache> textures_;
  std::unique_ptr<DrawRenderer> draw_;
  std::unique_ptr<Presenter> present_;
  // Primary mode with --pgr4_native_frame_latency > 0, or observing only with --pgr4_native_csv.
  std::unique_ptr<FramePacer> pacer_;
  GpuTimer* gpu_timer_ = nullptr;  // device_'s, with --pgr4_native_gpu_timing
  bool frame_begun_ = false;
  uint64_t frame_index_ = 0;
  uint64_t frame_start_ns_ = 0;
  FrameStats stats_;
  uint64_t draws_submitted_ = 0;  // stats_.draws at the frame's last SubmitEarly
  struct Csv;
  std::unique_ptr<Csv> csv_;
};

// Monotonic CPU time in nanoseconds (RDTSC calibrated against QueryPerformanceCounter).
uint64_t NowNs();

}  // namespace pgr4::native
