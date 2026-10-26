// pgr4-native PGR4 native renderer: the upscaler's render size from the GPU's time per frame
// (--pgr4_native_upscaler_target_fps).
//
// The render size is a level out of kSteps: level n renders n / kSteps of the largest size
// (--pgr4_native_upscaler_width/height) on each axis. Each frame's GPU time
// (D3D12Device::gpu_busy_ns, which leaves out the time the GPU waited for the CPU) goes into a
// moving average, held against a budget: kBudget of the target frame time. Above the budget the
// level drops at once, by the square root of the overshoot, since the scene's GPU time grows
// with its pixels (the interface, the post-processing and the upscaler itself do not, so a drop
// undershoots and the next one follows). Below kRaise of the budget for kRaiseFrames frames, the
// level rises one step. After a change the next waits kSettleFrames frames: the GPU time read
// each frame is of a frame D3D12Device::kFramesInFlight back.
//
// Threading: the render thread's (Upscaler::BeginFrame).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pgr4::native {

class DynamicResolution {
 public:
  static constexpr uint32_t kSteps = 80;

  // `min_level` (at most kSteps) is the smallest level; `frame_ns` the target frame time.
  DynamicResolution(uint32_t min_level, uint64_t frame_ns)
      : min_level_(std::clamp<uint32_t>(min_level, 1, kSteps)),
        budget_ns_(double(frame_ns) * kBudget) {}

  uint32_t level() const { return level_; }

  // Once per frame with a recent frame's GPU time in ns (0: none measured); returns the level
  // for the next frame.
  uint32_t Update(uint64_t gpu_ns) {
    if (!gpu_ns) return level_;
    average_ns_ += (double(gpu_ns) - average_ns_) * (average_ns_ ? kAverageWeight : 1.0);
    if (settle_) {
      --settle_;
      return level_;
    }
    if (average_ns_ > budget_ns_) {
      under_ = 0;
      const auto to = uint32_t(double(level_) * std::sqrt(budget_ns_ / average_ns_));
      if (level_ > min_level_) Change(std::clamp(to, min_level_, level_ - 1));
    } else if (average_ns_ < budget_ns_ * kRaise && level_ < kSteps) {
      if (++under_ >= kRaiseFrames) Change(level_ + 1);
    } else {
      under_ = 0;
    }
    return level_;
  }

 private:
  static constexpr double kBudget = 0.9;
  static constexpr double kRaise = 0.8;
  static constexpr double kAverageWeight = 0.125;
  static constexpr uint32_t kRaiseFrames = 30;
  static constexpr uint32_t kSettleFrames = 6;

  void Change(uint32_t to) {
    // What the average should become at the new level, so that it does not undo the change.
    const double r = double(to) / double(level_);
    average_ns_ *= r * r;
    level_ = to;
    settle_ = kSettleFrames;
    under_ = 0;
  }

  const uint32_t min_level_;
  const double budget_ns_;
  uint32_t level_ = kSteps;
  double average_ns_ = 0.0;
  uint32_t settle_ = 0, under_ = 0;
};

}  // namespace pgr4::native
