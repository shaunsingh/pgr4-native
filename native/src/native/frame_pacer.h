// pgr4-native PGR4 native renderer: frame pacing on the display's clock (primary mode).
//
// The game paces itself on guest vblanks: KickOffEx (sub_826CACA0) hands its presentation
// interval to the GPU, which flips on the vblank that interval names, and ReXGlue raises the
// vblanks from a timer at --video_mode_refresh_rate. That timer is a clock of its own beside the
// display's. The game's frames begin on its ticks while the display shows them on its own, so
// the two drift apart; a frame that misses a tick by a little waits a whole tick more (a doubled
// frame at any frame rate), and under a frame limit (Steam's, which is gamescope's) the game's
// measured time steps no longer match the intervals its frames are shown at.
//
// While the pacer runs the guest presents immediately (Pgr4PresentationInterval) and only the
// display sets the pace: the game thread begins frame F + 1 once the display has shown frame
// F + 1 - L, where L is --pgr4_native_frame_latency. The frames then begin in step with the
// display whatever its refresh rate, frame limit or VRR, the game's time steps follow the
// intervals the display shows them at, and the game reads its input at most L frames before the
// display shows the result. L = 1 runs one frame at a time, from the game thread through the
// GPU to the display; each frame more lets the game thread work on the next frame while the GPU
// draws one. Too few leave the GPU idle between frames, and the Deck's GPU then lowers its clock
// (vkd3d-proton's swapchain.c on its own latency default).
//
// "Shown" is the swapchain's frame latency waitable object: vkd3d-proton releases it once per
// present when vkWaitForPresentKHR returns, but only in FIFO (with other present modes once the
// frame's blit finished, which is why the renderer presents with vsync while the pacer paces);
// DXGI when the frame leaves the present queue. A thread here waits on it and counts. A frame
// the render thread does not present (held for pipelines, or while ReXGlue's presenter has the
// window: a XAM dialog, the Xenos plugin) counts as shown at once (RetireUnshown).
//
// The swapchain comes and goes with the game window's owner (renderer.h, OwnGameWindow): the
// pacer counts across swapchains, each one's waitable bound from Attach to Detach.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

namespace pgr4::native {

class FramePacer {
 public:
  static constexpr uint32_t kMaxFrameLatency = 3;

  // `frame_latency`: L above, at most kMaxFrameLatency; 0 only observes (the log), and the game
  // keeps its vblank pacing. `log`, when not null: one "present,shown_ms" row per present the
  // display showed (NowNs clock), the pacer's own (closed with it).
  FramePacer(uint32_t frame_latency, std::FILE* log);
  ~FramePacer();
  FramePacer(const FramePacer&) = delete;
  FramePacer& operator=(const FramePacer&) = delete;

  uint32_t frame_latency() const { return latency_; }
  // Whether frames are paced: a swapchain is attached and L > 0.
  bool pacing() const { return waitable_ && latency_ > 0; }

  // Render thread. Attach: the swapchain the frames are presented to from here on, by its frame
  // latency waitable (D3D12Device::TakeFrameLatencyWaitable before its first Present; owned by
  // the pacer from here). Detach, once that swapchain was released: its presents not counted
  // yet are shown, and the waitable is closed.
  void Attach(HANDLE waitable);
  void Detach();
  // Render thread, once per Swap: the frame was presented, or will not be (RetireUnshown).
  void Presented() { ++presented_; }
  void RetireUnshown();

  // Game thread, once per frame after the Swap queued it: blocks until the next frame may
  // begin. Returns the ns it waited.
  uint64_t WaitToBegin();
  // Opens the gate for good (the native path stopped); WaitToBegin returns at once from then on.
  void Stop();

 private:
  // A present the display does not complete (a minimized or occluded window, suspend) lets the
  // game go on at this pace instead of stopping it.
  static constexpr uint32_t kWaitTimeoutMs = 100;

  void Run();
  void Retire(uint64_t frames);

  const uint32_t latency_;
  std::FILE* const log_;
  HANDLE waitable_ = nullptr;  // render thread: the attached swapchain's
  const HANDLE thread_stop_;   // Detach ends the waiting thread with it
  std::thread thread_;         // waits on waitable_ from Attach to Detach
  uint64_t presented_ = 0;     // render thread: presents over all swapchains
  uint64_t shown_ = 0;         // waitable releases taken (thread_, render thread once joined)
  uint64_t queued_ = 0;        // game thread: frames WaitToBegin was called for
  std::mutex mutex_;
  std::condition_variable retired_cv_;
  uint64_t retired_ = 0;  // frames shown or retired unshown (mutex_)
  bool stopped_ = false;  // (mutex_)
  bool timed_out_ = false;
};

}  // namespace pgr4::native
