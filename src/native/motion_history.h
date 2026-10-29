// pgr4-native PGR4 native renderer: the previous frame's constants of each draw, for the motion
// vector pass of the upscaler's scene (DxbcShaderTranslator's motion_vectors).
//
// A draw is matched with the previous frame's by its identity (its vertex shader, index buffer
// and index count: the draws of a frame keep these across frames, while per-frame vertex streams
// such as instance transforms move) and its place in the frame's order. The race draws its scene
// in the same order frame after frame (2520 of 2522 draws matched in td-265's draw logs), so
// each draw first tries the previous frame's draw after the last one matched, then those
// within kWindow on either side not matched yet: draws that appear, vanish or swap places with
// a neighbour (culling, sorting) cost a few compares and leave the rest in step. An unmatched
// draw reads its own constants and gets no motion of its own.
//
// The frames' draws are kept in order, in arrays read and written front to back, which a
// hash table probed at random spots for every draw is not (td-499 perf: three cache misses a
// draw, 6 % of the render thread). The constants stay in the upload memory they were committed
// to, which the constant pool keeps for one more frame (UploadPool::set_keep_previous_frame).
//
// Threading: the thread that records the draws.
#pragma once

#include <d3d12.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace pgr4::native {

class MotionHistory {
 public:
  struct Constants {
    const void* vs = nullptr;  // the shader the float constants are packed for
    D3D12_GPU_VIRTUAL_ADDRESS float_constants = 0, fetch_constants = 0;
  };

  MotionHistory() {
    current_.reserve(kReserved);
    previous_.reserve(kReserved);
  }

  // Frame `frame`'s first draw: the last frame's draws become the previous frame's, if that was
  // frame - 1 (the constant pool keeps one frame's memory, drawn or not).
  void BeginFrame(uint64_t frame) {
    std::swap(current_, previous_);
    current_.clear();
    if (frame != frame_ + 1) previous_.clear();
    frame_ = frame;
    cursor_ = 0;
  }

  // This frame's next draw, of `identity` with the constants `now`: the matching draw's
  // constants of the previous frame, or nullptr. Valid until the next BeginFrame.
  const Constants* Match(uint64_t identity, const Constants& now) {
    current_.push_back({identity, now, false});
    const size_t n = previous_.size();
    auto take = [&](size_t i) {
      previous_[i].matched = true;
      cursor_ = i + 1;
      return &previous_[i].constants;
    };
    for (size_t i = cursor_, end = std::min(n, cursor_ + kWindow); i < end; ++i)
      if (previous_[i].identity == identity && !previous_[i].matched) return take(i);
    for (size_t i = cursor_, end = cursor_ > kWindow ? cursor_ - kWindow : 0; i-- > end;)
      if (previous_[i].identity == identity && !previous_[i].matched) return take(i);
    return nullptr;
  }

 private:
  static constexpr size_t kWindow = 32;
  static constexpr size_t kReserved = 8192;  // twice the most draws in a race frame
  struct Draw {
    uint64_t identity;
    Constants constants;
    bool matched;  // previous_: taken by a draw of this frame
  };

  std::vector<Draw> current_, previous_;
  size_t cursor_ = 0;  // in previous_: after the last draw matched
  uint64_t frame_ = UINT64_MAX - 1;
};

}  // namespace pgr4::native
