// pgr4-native PGR4 native renderer: frame pacing on the display's clock (frame_pacer.h).
#include "frame_pacer.h"

#include "renderer.h"

#include <rex/logging.h>

#include <algorithm>
#include <chrono>

namespace pgr4::native {

FramePacer::FramePacer(uint32_t frame_latency, std::FILE* log)
    : latency_(std::min(frame_latency, kMaxFrameLatency)),
      log_(log),
      thread_stop_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
  if (log_) std::fprintf(log_, "present,shown_ms\n");
}

FramePacer::~FramePacer() {
  Stop();
  Detach();
  CloseHandle(thread_stop_);
  if (log_) std::fclose(log_);
}

void FramePacer::Attach(HANDLE waitable) {
  // The waitable starts with counts no present stands for (the swapchain's maximum frame
  // latency); no Present has run yet, so taking them all leaves one count per shown present.
  while (WaitForSingleObjectEx(waitable, 0, FALSE) == WAIT_OBJECT_0) {}
  waitable_ = waitable;
  thread_ = std::thread([this] { Run(); });
}

void FramePacer::Detach() {
  if (!waitable_) return;
  SetEvent(thread_stop_);
  thread_.join();
  // The released swapchain completed its presents, and may not have signalled all of them.
  while (shown_ < presented_ && WaitForSingleObjectEx(waitable_, 0, FALSE) == WAIT_OBJECT_0)
    ++shown_;
  Retire(presented_ - shown_);
  shown_ = presented_;
  CloseHandle(waitable_);
  waitable_ = nullptr;
}

void FramePacer::Run() {
  SetThreadDescription(GetCurrentThread(), L"PGR4 native pacer");
  const HANDLE handles[] = {waitable_, thread_stop_};
  while (WaitForMultipleObjectsEx(2, handles, FALSE, INFINITE, FALSE) == WAIT_OBJECT_0) {
    const uint64_t shown_ns = NowNs();
    ++shown_;
    Retire(1);
    if (log_) {
      std::fprintf(log_, "%llu,%.3f\n", (unsigned long long)shown_, shown_ns / 1e6);
      std::fflush(log_);
    }
  }
}

void FramePacer::Retire(uint64_t frames) {
  if (!frames) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    retired_ += frames;
  }
  retired_cv_.notify_one();
}

void FramePacer::RetireUnshown() { Retire(1); }

void FramePacer::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }
  retired_cv_.notify_one();
}

uint64_t FramePacer::WaitToBegin() {
  // Frame `queued_` was queued; frame queued_ + 1 begins once frame queued_ + 1 - L was shown.
  const uint64_t queued = ++queued_;
  if (latency_ == 0 || queued < latency_) return 0;
  const uint64_t needed = queued + 1 - latency_;
  std::unique_lock<std::mutex> lock(mutex_);
  if (retired_ >= needed || stopped_) return 0;
  const uint64_t t0 = NowNs();
  const bool shown = retired_cv_.wait_for(lock, std::chrono::milliseconds(kWaitTimeoutMs),
                                          [&] { return retired_ >= needed || stopped_; });
  if (!shown && !timed_out_) {
    timed_out_ = true;
    REXLOG_WARN("PGR4 native: frame {} not shown after {} ms; frames go on without the display",
                needed, kWaitTimeoutMs);
  }
  return NowNs() - t0;
}

}  // namespace pgr4::native
