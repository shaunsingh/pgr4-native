// pgr4-native PGR4 native renderer: frame pacing on the display's clock (frame_pacer.h).
#include "frame_pacer.h"

#include "clock.h"

#include <d3d12.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <utility>

namespace pgr4::native {

FramePacer::FramePacer(Options options)
    : latency_(std::min(options.frame_latency, kMaxFrameLatency)),
      low_latency_(options.low_latency),
      margin_ns_(int64_t(options.latency_margin_ns)),
      log_(std::move(options.log)),
      thread_stop_(CreateAutoResetEvent()),
      fence_event_(CreateAutoResetEvent()) {
  lag_ns_.fill(kNoLag);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll && GetProcAddress(ntdll, "wine_get_version"))
    delay_execution_ =
        reinterpret_cast<NtDelayExecutionFn>(GetProcAddress(ntdll, "NtDelayExecution"));
  if (!delay_execution_)
    sleep_timer_.reset(CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
  if (log_) log_->Append("present,frame,ready_ms,shown_ms,delay_ms\n");
}

FramePacer::~FramePacer() {
  Stop();
  Detach();
}

void FramePacer::Attach(HANDLE waitable, ID3D12Fence* fence) {
  // The waitable starts with counts no present stands for (the swapchain's maximum frame
  // latency); no Present has run yet, so taking them all leaves one count per shown present.
  while (WaitForSingleObjectEx(waitable, 0, FALSE) == WAIT_OBJECT_0) {}
  waitable_.reset(waitable);
  fence_ = fence;
  thread_ = std::thread([this] { Run(); });
}

void FramePacer::Detach() {
  if (!waitable_) return;
  SetEvent(thread_stop_.get());
  thread_.join();
  {
    // The released swapchain completed its presents, whether it signalled them or not.
    std::lock_guard<std::mutex> lock(mutex_);
    RetireLocked(presented_ - shown_, NowNs());
    shown_ = ready_ = presented_;
    ResetControlLocked();
  }
  retired_cv_.notify_one();
  waitable_.reset();
  fence_ = nullptr;
}

void FramePacer::Presented(uint64_t fence_value) {
  std::lock_guard<std::mutex> lock(mutex_);
  presents_[presented_++ % kRing] = {++swaps_, fence_value, 0};
  if (ready_ + 1 == presented_ && fence_) {
    fence_->SetEventOnCompletion(fence_value, fence_event_.get());
    armed_ = fence_value;
  }
}

void FramePacer::RetireUnshown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++swaps_;
    RetireLocked(1, NowNs());
  }
  retired_cv_.notify_one();
}

void FramePacer::Run() {
  SetThreadDescription(GetCurrentThread(), L"PGR4 native pacer");
  // The fence first: when both are set, a frame is seen finished before it is seen shown.
  const HANDLE handles[] = {fence_event_.get(), waitable_.get(), thread_stop_.get()};
  for (;;) {
    const DWORD r = WaitForMultipleObjectsEx(3, handles, FALSE, INFINITE, FALSE);
    if (r != WAIT_OBJECT_0 && r != WAIT_OBJECT_0 + 1) break;
    const uint64_t now = NowNs();
    std::unique_lock<std::mutex> lock(mutex_);
    OnFenceLocked(now);
    if (r == WAIT_OBJECT_0 || shown_ == presented_) continue;
    Present p = presents_[shown_ % kRing];
    // Shown implies finished; the fence's value may trail the present by a wakeup.
    if (!p.ready_ns) p.ready_ns = now;
    ++shown_;
    ready_ = std::max(ready_, shown_);
    RetireLocked(1, now);
    ControlLocked(p, now);
    const uint64_t present = shown_;
    const int64_t delay = delay_ns_;
    lock.unlock();
    retired_cv_.notify_one();
    if (log_)
      log_->Printf("%llu,%llu,%.3f,%.3f,%.3f\n", (unsigned long long)present,
                   (unsigned long long)p.frame, p.ready_ns / 1e6, now / 1e6, delay / 1e6);
  }
}

void FramePacer::OnFenceLocked(uint64_t now_ns) {
  const uint64_t done = fence_->GetCompletedValue();
  while (ready_ < presented_ && presents_[ready_ % kRing].fence <= done)
    presents_[ready_++ % kRing].ready_ns = now_ns;
  if (ready_ < presented_ && armed_ != presents_[ready_ % kRing].fence) {
    armed_ = presents_[ready_ % kRing].fence;
    fence_->SetEventOnCompletion(armed_, fence_event_.get());
  }
}

void FramePacer::RetireLocked(uint64_t n, uint64_t now_ns) {
  for (; n; --n) retired_ns_[++retired_ % kRing] = now_ns;
}

int64_t FramePacer::IntervalLocked() const {
  const uint64_t n = std::min<uint64_t>(interval_count_, kIntervalFrames);
  if (n < kIntervalMinFrames) return 0;
  const auto at = [&](uint64_t i) {
    return intervals_[(interval_count_ - n + i) % kIntervalFrames];
  };
  int64_t interval = std::numeric_limits<int64_t>::max();
  for (uint64_t i = 0; i + 2 < n; ++i) {
    const int64_t a = at(i), b = at(i + 1), c = at(i + 2);
    interval = std::min(interval, std::max(std::min(a, b), std::min(std::max(a, b), c)));
  }
  return interval;
}

void FramePacer::ControlLocked(const Present& p, uint64_t shown_ns) {
  if (!low_latency_ || latency_ == 0) return;
  const bool chained = last_frame_ && p.frame == last_frame_ + 1;
  const int64_t shown_interval = int64_t(shown_ns - last_shown_ns_);
  last_frame_ = p.frame;
  last_shown_ns_ = shown_ns;
  if (!chained) return;
  intervals_[interval_count_++ % kIntervalFrames] = shown_interval;
  const uint64_t lag = lag_ns_[p.frame % kRing];
  const int64_t interval = IntervalLocked();
  if (lag == kNoLag || interval <= 0) return;
  if (!probe_at_ns_) probe_at_ns_ = shown_ns;
  // A new pace (a frame limit set or lifted, the probe finding a faster one): the headroom was
  // measured against the old one's slots.
  if (std::abs(interval - interval_ns_) * 4 > interval_ns_) {
    interval_ns_ = interval;
    headroom_count_ = 0;
    probe_period_ns_ = kProbePeriodNs;
  }
  const int64_t missed = std::max<int64_t>(0, (shown_interval + interval / 2) / interval - 1);
  // A stall (a loading screen) says nothing of the pipeline's time: the headroom starts over.
  if (missed > kStallSlots) {
    headroom_count_ = 0;
    return;
  }
  const int64_t headroom = std::max<int64_t>(
      0, int64_t(shown_ns) - missed * interval - int64_t(p.ready_ns) + int64_t(lag));
  const uint64_t second = shown_ns / 1'000'000'000;
  if (headroom_count_++ == 0) {
    headroom_by_s_.fill(std::numeric_limits<int64_t>::max());
  } else {
    for (uint64_t s = headroom_s_ + 1; s <= std::min(second, headroom_s_ + kHeadroomWindowS); ++s)
      headroom_by_s_[s % kHeadroomWindowS] = std::numeric_limits<int64_t>::max();
  }
  headroom_s_ = second;
  int64_t& least = headroom_by_s_[second % kHeadroomWindowS];
  least = std::min(least, headroom);
  headroom_ns_ = *std::min_element(headroom_by_s_.begin(), headroom_by_s_.end());
  int64_t target = std::max<int64_t>(0, headroom_ns_ - margin_ns_);
  if (headroom_count_ < kHeadroomFrames) target = std::min(target, delay_ns_);
  const bool sooner = shown_interval * 8 < interval * 7;
  if (delay_ns_ > 0 && p.frame >= probe_end_frame_ &&
      (sooner || shown_ns - probe_at_ns_ > probe_period_ns_)) {
    if (!sooner) probe_period_ns_ = std::min(probe_period_ns_ * 2, kProbePeriodMaxNs);
    probe_at_ns_ = shown_ns;
    probe_delay_ns_ = std::max<int64_t>(0, delay_ns_ - interval / 2);
    probe_end_frame_ = p.frame + kProbeHoldFrames +
                       uint64_t((delay_ns_ - probe_delay_ns_ + kProbeStepNs - 1) / kProbeStepNs);
  }
  const bool probing = p.frame < probe_end_frame_;
  if (probing) target = std::min(target, probe_delay_ns_);
  delay_ns_ += std::clamp(target - delay_ns_, probing ? -kProbeStepNs : -kDelayDownNs, kDelayUpNs);
}

void FramePacer::ResetControlLocked() {
  delay_ns_ = 0;
  last_frame_ = last_shown_ns_ = 0;
  interval_count_ = headroom_count_ = 0;
  interval_ns_ = 0;
  probe_at_ns_ = probe_end_frame_ = 0;
  probe_period_ns_ = kProbePeriodNs;
}

void FramePacer::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }
  retired_cv_.notify_one();
}

void FramePacer::SleepUntil(uint64_t deadline_ns) const {
  const uint64_t now = NowNs();
  if (deadline_ns <= now) return;
  LARGE_INTEGER due;
  due.QuadPart = -int64_t((deadline_ns - now + 99) / 100);  // relative, in 100 ns units
  if (delay_execution_) {
    delay_execution_(FALSE, &due);
  } else if (sleep_timer_ &&
             SetWaitableTimer(sleep_timer_.get(), &due, 0, nullptr, nullptr, FALSE)) {
    WaitForSingleObject(sleep_timer_.get(), INFINITE);
  } else {
    Sleep(DWORD((deadline_ns - now) / 1'000'000));
  }
}

uint64_t FramePacer::WaitToBegin() {
  // Frame `queued_` was queued; frame queued_ + 1 begins once frame queued_ + 1 - L was shown,
  // and D after that.
  const uint64_t queued = ++queued_;
  const uint64_t frame = queued + 1;
  if (latency_ == 0) return 0;
  const uint64_t t0 = NowNs();
  std::unique_lock<std::mutex> lock(mutex_);
  lag_ns_[frame % kRing] = kNoLag;
  if (queued < latency_ || stopped_) return 0;
  const uint64_t needed = queued + 1 - latency_;
  if (retired_ < needed) {
    const bool shown = retired_cv_.wait_for(lock, std::chrono::milliseconds(kWaitTimeoutMs),
                                            [&] { return retired_ >= needed || stopped_; });
    if (!shown && !timed_out_) {
      timed_out_ = true;
      REXLOG_WARN("PGR4 native: frame {} not shown after {} ms; frames go on without the display",
                  needed, kWaitTimeoutMs);
    }
    if (!shown || stopped_) return NowNs() - t0;
  }
  const uint64_t gate_ns = retired_ns_[needed % kRing];
  const uint64_t deadline = gate_ns + uint64_t(delay_ns_);
  lock.unlock();
  SleepUntil(deadline);
  const uint64_t begin = NowNs();
  lock.lock();
  lag_ns_[frame % kRing] = begin - gate_ns;
  return begin - t0;
}

}  // namespace pgr4::native
