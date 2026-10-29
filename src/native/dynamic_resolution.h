// pgr4-native PGR4 native renderer: the upscaler's render size from the GPU's time per frame
// (--pgr4_native_upscaler_target_fps).
//
// The render size is a level out of kSteps: level n renders n / kSteps of the largest size
// (--pgr4_native_upscaler_width/height) on each axis, an area of (n / kSteps)^2. The budget is
// kBudget of the frame time: the target frame rate's, or the frame rate the frames are paced at
// when that is lower (a lower frame rate setting, frame generation).
//
// Each frame's GPU time (D3D12Device::gpu_busy_ns, which leaves out the time the GPU waited for
// the CPU) is of a frame `lag` frames back, and counts toward the level that frame rendered at.
// The frames at one level make a run, whose time is a moving average over kRunFrames. Only part
// of the time grows with the area: the scene's pixels, while the post-processing, the interface,
// shadows and the upscaler itself do not (FSR 4 through OptiScaler on a Steam Deck: 13.9 ms at a
// quarter of the area, 14.8 ms at all of it). So the time at another level is predicted from the
// run's by a time per area, which each change of at least kLearnArea measures: the difference of
// the two runs' times over the difference of their areas. Until the first such change all of the
// time is taken to grow with the area.
//
// Both ways the level goes to the largest predicted within kAim of the budget, below which noise
// does not take it over the budget: at once when the run is over the budget, and when that is
// higher than the level for kRaiseFrames frames in a row. A change waits for the run at the new
// level.
//
// Threading: the render thread's (Upscaler::BeginFrame).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pgr4::native {

class DynamicResolution {
 public:
  static constexpr uint32_t kSteps = 80;
  static constexpr uint32_t kMaxLag = 3;

  // `min_level` (at most kSteps) is the least level, `target_ns` the target frame time, and the
  // GPU times Update takes are of the frame `lag` (at most kMaxLag) frames back.
  DynamicResolution(uint32_t min_level, uint64_t target_ns, uint32_t lag)
      : min_level_(std::clamp<uint32_t>(min_level, 1, kSteps)),
        target_ns_(double(target_ns)),
        lag_(std::min(lag, kMaxLag)) {
    levels_.fill(kSteps);
  }

  uint32_t level() const { return level_; }
  // Whether the last run decided on was at the least level and over the budget.
  bool out_of_reach() const { return out_of_reach_; }
  // The time per area measured so far in ns, 0 before the first measured change.
  double per_area_ns() const { return learned_ ? per_area_ns_ : 0.0; }
  // The current run's GPU time in ns.
  double run_ns() const { return run_ns_; }

  // The frames before the next one were not this one's to size (the upscaler was off): the next
  // `lag` GPU times are not counted, and the run starts over.
  void Restart() {
    frames_ = 0;
    run_frames_ = 0;
    raise_frames_ = 0;
    previous_.full = false;
  }

  // Once per frame, with the GPU time in ns of the frame `lag` frames back (0: none measured)
  // and the time in ns of a frame at the pace the frames are shown at. Returns the level of the
  // next frame.
  uint32_t Update(uint64_t gpu_ns, uint64_t paced_ns) {
    const uint32_t measured = levels_[(frames_ + kRing - lag_) % kRing];
    if (gpu_ns && frames_ >= lag_) Measure(measured, double(gpu_ns));
    const double budget = std::max(target_ns_, double(paced_ns)) * kBudget;
    if (run_level_ == level_ && run_frames_ >= kRunFrames) Decide(budget);
    levels_[frames_++ % kRing] = level_;
    return level_;
  }

 private:
  static constexpr double kBudget = 0.9;
  static constexpr double kAim = 0.95;
  static constexpr uint32_t kRunFrames = 8;
  static constexpr uint32_t kRaiseFrames = 30;
  static constexpr double kLearnArea = 0.1;
  static constexpr uint32_t kRing = kMaxLag + 1;

  static double Area(uint32_t level) {
    const double s = double(level) / kSteps;
    return s * s;
  }

  void Measure(uint32_t level, double ns) {
    if (level != run_level_) {
      // A full run before this one can be compared with this one once it is full too.
      previous_ = {run_level_, run_ns_, run_frames_ >= kRunFrames};
      run_level_ = level;
      run_frames_ = 0;
    }
    ++run_frames_;
    run_ns_ += (ns - run_ns_) / double(std::min(run_frames_, kRunFrames));
    if (run_frames_ == kRunFrames && previous_.full) {
      previous_.full = false;
      Learn(previous_.level, previous_.ns);
    }
  }

  void Learn(uint32_t level, double ns) {
    const double d_area = Area(level) - Area(run_level_);
    if (std::abs(d_area) < kLearnArea) return;
    // At most all of the time grows with the area; none of it shrinks as the area grows.
    const double most = std::max(ns / Area(level), run_ns_ / Area(run_level_));
    const double measured = std::clamp((ns - run_ns_) / d_area, 0.0, most);
    per_area_ns_ = learned_ ? (per_area_ns_ + measured) / 2 : measured;
    learned_ = true;
  }

  double PerArea() const { return learned_ ? per_area_ns_ : run_ns_ / Area(level_); }

  // The largest level predicted within `budget` from the current run, at least the least level.
  uint32_t Fit(double budget) const {
    const double per_area = PerArea();
    if (per_area <= 0.0) return kSteps;  // the area makes no difference
    const double area = Area(level_) + (budget - run_ns_) / per_area;
    if (area >= 1.0) return kSteps;
    return std::clamp(uint32_t(std::sqrt(std::max(area, 0.0)) * kSteps), min_level_, kSteps);
  }

  void Decide(double budget) {
    out_of_reach_ = level_ == min_level_ && run_ns_ > budget;
    if (run_ns_ > budget) {
      raise_frames_ = 0;
      if (const uint32_t to = Fit(budget * kAim); to < level_) level_ = to;
    } else if (const uint32_t to = Fit(budget * kAim); to > level_) {
      if (++raise_frames_ >= kRaiseFrames) {
        level_ = to;
        raise_frames_ = 0;
      }
    } else {
      raise_frames_ = 0;
    }
  }

  const uint32_t min_level_;
  const double target_ns_;
  const uint32_t lag_;
  uint32_t level_ = kSteps;
  std::array<uint32_t, kRing> levels_;  // by frame: the level it rendered at
  uint64_t frames_ = 0;
  uint32_t run_level_ = kSteps, run_frames_ = 0;
  double run_ns_ = 0.0;
  struct {
    uint32_t level;
    double ns;
    bool full;
  } previous_ = {kSteps, 0.0, false};
  double per_area_ns_ = 0.0;
  bool learned_ = false;
  bool out_of_reach_ = false;
  uint32_t raise_frames_ = 0;
};

}  // namespace pgr4::native
