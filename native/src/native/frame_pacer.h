// pgr4-native PGR4 native renderer: frame pacing on the display's clock (primary mode).
//
// Why. The game paces itself on guest vblanks: KickOffEx (sub_826CACA0) hands its presentation
// interval to the GPU, and ReXGlue raises the vblanks from a timer at --video_mode_refresh_rate,
// a clock beside the display's. The two drift apart: a frame that misses a tick by a little waits
// a whole tick (a doubled frame at any frame rate), and under a frame limit (Steam's, which is
// gamescope's) the game's time steps no longer match the intervals its frames are shown at. While
// the pacer runs the guest presents immediately (Pgr4PresentationInterval) and the display is the
// only clock.
//
// The gate. Frame F + 1 begins once the display has shown frame F + 1 - L
// (--pgr4_native_frame_latency). Frames then begin in step with the display whatever its refresh
// rate, frame limit or VRR, and the game's time steps follow the intervals shown. L frames let the
// game thread, the render thread and the GPU each work on one; fewer leave the GPU idle between
// frames, and the Deck's GPU then lowers its clock.
//
// The delay (--pgr4_native_low_latency). The gate alone puts L display intervals between the game
// reading its input and the display showing the result, however short the pipeline: 67 ms under
// a 45 fps limit at L = 3 (td-111). So frame F + 1 begins a delay D after its gate opened, D being
// the frames' headroom less a margin (--pgr4_native_latency_margin_us): 31 ms then, with no frame
// shown late (td-118). This is DXVK's latency sleep (dxvk_latency_builtin.cpp) measured against
// the display, as the game cannot know the frame interval under gamescope's limiter.
// - A shown frame's headroom is the time from the GPU finishing it (the frame fence) to the
//   display showing it, plus the delay it began with. A frame shown n intervals late counts
//   against the slot it missed, n intervals earlier, and none counts below zero. A frame shown
//   more than kStallSlots slots late is a stall (a loading screen), not the pipeline's time: the
//   estimate starts over.
// - The estimate is held: the least headroom of the last kHeadroomWindowS seconds (in one-second
//   buckets), since the slowest frames come seconds apart (in the race at 90 fps, single frames
//   with twice the usual GPU time every 3-7 s, td-114). Released at a fixed rate instead (1 ms/s,
//   td-113 to td-119), the estimate took over a minute to recover from one hitch at 30 fps, where
//   the headroom spans three 33 ms intervals (td-121).
// - D follows it by at most kDelayUpNs per frame up and kDelayDownNs down, and only down for
//   kHeadroomFrames frames after the estimate started over, so the game's time steps stay within
//   a fraction of a millisecond of the intervals shown.
// - The interval is the shortest the display showed recently: the least median of three of the
//   last kIntervalFrames. Frames begun just late enough to fill the display's slots cannot tell
//   a frame limit from a pace they set themselves, so D probes: it eases down by half an interval,
//   kProbeStepNs per frame, and holds there for kProbeHoldFrames frames. Under a limit only the
//   latency changes, for a moment (20 ms for 2 s at 30 fps, td-122); without one the frames are
//   shown at the faster pace, and the interval follows it. D probes kProbePeriodNs after the pace
//   was set, then at twice the period before, up to kProbePeriodMaxNs; and at once when a frame
//   was shown sooner than the pace allows, as the first frames are once a limit is lifted.
//
// "Shown" is the swapchain's frame latency waitable object: vkd3d-proton releases it once per
// present when vkWaitForPresentKHR returns, but only in FIFO (with other present modes once the
// frame's blit finished, which is why the renderer presents with vsync while the pacer paces);
// DXGI when the frame leaves the present queue. A thread here waits on it and on the frame fence.
// A frame the render thread does not present (held for pipelines, or while ReXGlue's presenter
// has the window: a XAM dialog, the Xenos plugin) counts as shown at once (RetireUnshown).
//
// The swapchain comes and goes with the game window's owner (renderer.h, OwnGameWindow): the
// pacer counts across swapchains, each one's waitable bound from Attach to Detach.
#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "async_file.h"
#include "win_handle.h"  // windows.h

struct ID3D12Fence;

namespace pgr4::native {

class FramePacer {
 public:
  static constexpr uint32_t kMaxFrameLatency = 3;

  struct Options {
    // L above, at most kMaxFrameLatency; 0 only observes (the log), and the game keeps its
    // vblank pacing.
    uint32_t frame_latency = kMaxFrameLatency;
    bool low_latency = true;  // the delay D above
    uint64_t latency_margin_ns = 4'000'000;
    // When not null: one "present,frame,ready_ms,shown_ms,delay_ms" row per present the display
    // showed (NowNs clock).
    std::unique_ptr<AsyncFile> log;
  };

  explicit FramePacer(Options options);
  ~FramePacer();
  FramePacer(const FramePacer&) = delete;
  FramePacer& operator=(const FramePacer&) = delete;

  uint32_t frame_latency() const { return latency_; }
  // Whether frames are paced: a swapchain is attached and L > 0.
  bool pacing() const { return waitable_ && latency_ > 0; }

  // Render thread. Attach: the swapchain the frames are presented to from here on, by its frame
  // latency waitable (D3D12Device::TakeFrameLatencyWaitable before its first Present; owned by
  // the pacer from here), and the fence the frames signal. Detach, once that swapchain was
  // released: its presents not counted yet are shown, and the waitable is closed.
  void Attach(HANDLE waitable, ID3D12Fence* fence);
  void Detach();
  // Render thread, once per Swap: the frame was presented and signals `fence_value` once the GPU
  // finished it, or will not be presented (RetireUnshown).
  void Presented(uint64_t fence_value);
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
  // Rings indexed by frame or present number: more than the frames begun but not retired (L) and
  // the presents not shown (at most L).
  static constexpr uint32_t kRing = 16;
  static constexpr uint32_t kIntervalFrames = 32;
  static constexpr uint32_t kIntervalMinFrames = 8;  // before IntervalLocked has an answer
  static constexpr uint32_t kHeadroomFrames = 64;
  static constexpr uint32_t kHeadroomWindowS = 10;
  static constexpr int64_t kStallSlots = 3;
  static constexpr int64_t kDelayUpNs = 500'000;
  static constexpr int64_t kDelayDownNs = 1'000'000;
  static constexpr uint64_t kProbePeriodNs = 10'000'000'000;
  static constexpr uint64_t kProbePeriodMaxNs = 80'000'000'000;
  static constexpr int64_t kProbeStepNs = 500'000;  // D's step down per frame while probing
  static constexpr uint64_t kProbeHoldFrames = 8;   // for the median of three to see a faster pace
  static constexpr uint64_t kNoLag = ~0ull;

  struct Present {
    uint64_t frame = 0;  // the frame's number (Swaps since the pacer started, from 1)
    uint64_t fence = 0;
    uint64_t ready_ns = 0;  // when the pacer saw the fence reach `fence`; 0: not yet
  };

  void Run();
  // mutex_ held. Frames `retired_` + 1 to `retired_` + n were shown or will not be.
  void RetireLocked(uint64_t n, uint64_t now_ns);
  void OnFenceLocked(uint64_t now_ns);
  // mutex_ held, per shown present: updates delay_ns_.
  void ControlLocked(const Present& p, uint64_t shown_ns);
  int64_t IntervalLocked() const;
  void ResetControlLocked();
  // Sleeps until `deadline_ns` (NowNs clock).
  void SleepUntil(uint64_t deadline_ns) const;

  const uint32_t latency_;
  const bool low_latency_;
  const int64_t margin_ns_;
  const std::unique_ptr<AsyncFile> log_;
  UniqueHandle waitable_;           // render thread: the attached swapchain's
  ID3D12Fence* fence_ = nullptr;    // render thread: with waitable_
  const UniqueHandle thread_stop_;  // Detach ends the waiting thread with it
  const UniqueHandle fence_event_;  // set when the GPU finished the oldest present not seen ready
  std::thread thread_;              // waits on waitable_ and fence_event_ from Attach to Detach
  uint64_t swaps_ = 0;              // render thread: Presented and RetireUnshown calls
  uint64_t queued_ = 0;             // game thread: frames WaitToBegin was called for

  // Sleeps with sub-millisecond precision: Wine's NtDelayExecution sleeps in the process (a
  // waitable timer is a wineserver object there); on Windows it rounds up to the timer period
  // and a high-resolution waitable timer does not.
  using NtDelayExecutionFn = LONG(WINAPI*)(BOOLEAN alertable, const LARGE_INTEGER* timeout);
  NtDelayExecutionFn delay_execution_ = nullptr;
  UniqueHandle sleep_timer_;

  std::mutex mutex_;
  std::condition_variable retired_cv_;
  // Presents [shown_, presented_) are not shown yet, [ready_, presented_) not seen finished.
  std::array<Present, kRing> presents_{};
  uint64_t presented_ = 0, ready_ = 0, shown_ = 0;
  uint64_t armed_ = 0;    // the fence value fence_event_ was last set to wait for
  uint64_t retired_ = 0;  // frames shown or retired unshown
  std::array<uint64_t, kRing> retired_ns_{};  // by frame number: when retired_ reached it
  // By frame number: how long after its gate opened the frame began (kNoLag: no gate).
  std::array<uint64_t, kRing> lag_ns_{};
  bool stopped_ = false;
  bool timed_out_ = false;
  // Latency control, per shown present.
  int64_t delay_ns_ = 0;                         // D
  uint64_t last_frame_ = 0, last_shown_ns_ = 0;  // the last shown present's
  std::array<int64_t, kIntervalFrames> intervals_{};
  uint64_t interval_count_ = 0;
  int64_t interval_ns_ = 0;      // IntervalLocked when the headroom was last reset
  int64_t headroom_ns_ = 0;      // the held headroom
  uint64_t headroom_count_ = 0;  // frames since it was reset
  // The least headroom shown in each of the last kHeadroomWindowS seconds, by second.
  std::array<int64_t, kHeadroomWindowS> headroom_by_s_{};
  uint64_t headroom_s_ = 0;  // the second the last headroom was shown in
  uint64_t probe_at_ns_ = 0, probe_end_frame_ = 0;
  uint64_t probe_period_ns_ = kProbePeriodNs;
  int64_t probe_delay_ns_ = 0;  // D's ceiling until probe_end_frame_
};

}  // namespace pgr4::native
