// pgr4-native PGR4 native renderer: the previous frame's constants of each draw, for the motion
// vector pass of the upscaler's scene (DxbcShaderTranslator's motion_vectors).
//
// A draw is matched with the previous frame's by its identity (its vertex shader, index buffer
// and index count: the draws of a frame keep these across frames, while per-frame vertex streams
// such as instance transforms move) and its place in the frame's order. The race draws its scene
// in the same order frame after frame (2520 of 2522 draws matched in td-265's draw logs), so
// each draw looks at the previous frame's draws within kWindow on either side of the one after
// the last matched, not matched yet: draws that appear, vanish or swap places with a neighbour
// (culling, sorting) cost a few compares and leave the rest in step.
//
// A draw with no match, a mesh that came into view or changed its level of detail (1 to 3 % of
// the race's, td-529), would read its own constants and show no motion at all while the camera
// moves, a block of the road or a building that FSR smears where it stood (dark boxes over the
// road in the 20261029-205920 session). It moves with the camera instead (Camera).
//
// Many draws share an identity: the copies of one mesh along the track (barrier sections,
// chevron boards, kerbs), told apart by their constants alone. Taking the first such draw in
// order pairs every copy after one that came into or went out of view with its neighbour's
// transform, meters off (12 draws a frame in the draw logs of the 20261029-205920 session),
// and FSR smears those surfaces with the wrong motion. Among several, a draw takes the one with
// the most float constants bit for bit its own: a copy's world transform stays the same from
// frame to frame where its neighbours' differ, while the camera's constants change alike for
// all (in that session's logs every pair this picks has the transform bit for bit); ties keep
// the order.
//
// The frames' draws are kept in order, in arrays read and written front to back, which a
// hash table probed at random spots for every draw is not (td-499 perf: three cache misses a
// draw, 6 % of the render thread). The constants stay in the upload memory they were committed
// to, which the constant pool keeps for one more frame (UploadPool::set_keep_previous_frame);
// the float constants are also copied, once per upload, into an array of the frame's, as the
// upload memory is write-combined and slow to read.
//
// Threading: the thread that records the draws.
#pragma once

#include <d3d12.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace pgr4::native {

class MotionHistory {
 public:
  struct Constants {
    const void* vs = nullptr;  // the shader the float constants are packed for
    D3D12_GPU_VIRTUAL_ADDRESS float_constants = 0, fetch_constants = 0;
  };
  // How the last Match found the previous constants.
  enum class Found {
    kNone,    // not at all
    kOrder,   // the draw of its identity next in order
    kCopy,    // among copies of its mesh, the one with its constants (another than the order's)
    kCamera,  // none: camera_floats(), its own with the camera's of the previous frame
  };

  MotionHistory() {
    current_.reserve(kReserved);
    previous_.reserve(kReserved);
    current_floats_.reserve(kReservedFloats);
    previous_floats_.reserve(kReservedFloats);
  }

  // Frame `frame`'s first draw: the last frame's draws become the previous frame's, if that was
  // frame - 1 (the constant pool keeps one frame's memory, drawn or not).
  void BeginFrame(uint64_t frame) {
    std::swap(current_, previous_);
    std::swap(current_floats_, previous_floats_);
    current_.clear();
    current_floats_.clear();
    if (frame != frame_ + 1) {
      previous_.clear();
      previous_floats_.clear();
    }
    frame_ = frame;
    cursor_ = 0;
  }

  // This frame's next draw, of `identity` with the constants `now`, `floats` the `size` bytes
  // (whole registers) of float constants at now.float_constants: the previous frame's constants
  // for it, or nullptr. Valid until the next Match.
  const Constants* Match(uint64_t identity, const Constants& now, const uint8_t* floats,
                         uint32_t size) {
    Draw d = {identity, now, 0, size, kNone, false};
    if (!current_.empty() && current_.back().constants.float_constants == now.float_constants &&
        current_.back().size == size) {
      d.offset = current_.back().offset;  // the same upload
    } else {
      d.offset = uint32_t(current_floats_.size());
      current_floats_.insert(current_floats_.end(), floats, floats + size);
    }
    current_.push_back(d);

    const size_t n = previous_.size();
    size_t best = n, first = n;
    uint32_t best_same = 0;
    bool several = false;
    auto consider = [&](size_t i) {
      const Draw& p = previous_[i];
      if (p.identity != identity || p.matched) return;
      if (best == n) {
        best = first = i;
        return;
      }
      if (!several) {
        several = true;
        best_same = SameRegisters(previous_[best], floats, size);
      }
      if (const uint32_t same = SameRegisters(p, floats, size); same > best_same) {
        best = i;
        best_same = same;
      }
    };
    ForWindow(consider);
    if (best == n) return Camera(now, floats, size);
    found_ = several && best != first ? Found::kCopy : Found::kOrder;
    previous_[best].matched = true;
    current_.back().previous = uint32_t(best);
    cursor_ = best + 1;
    return &previous_[best].constants;
  }
  Found found() const { return found_; }
  // Found::kCamera: the float constants of the previous frame for the last Match's draw, its
  // size bytes. Valid until the next Match.
  const std::vector<uint8_t>& camera_floats() const { return camera_floats_; }

 private:
  static constexpr size_t kWindow = 32;
  static constexpr size_t kReserved = 8192;  // twice the most draws in a race frame
  static constexpr size_t kReservedFloats = size_t(8) << 20;
  static constexpr uint32_t kNone = UINT32_MAX;
  struct Draw {
    uint64_t identity;
    Constants constants;
    uint32_t offset, size;  // its float constants in the frame's array
    uint32_t previous;      // current_: the draw of previous_ it matched, or kNone
    bool matched;           // previous_: taken by a draw of this frame
  };

  // `f` for each draw of previous_ within kWindow of cursor_, those after it first.
  template <typename F>
  void ForWindow(F&& f) const {
    const size_t n = previous_.size();
    for (size_t i = cursor_, end = std::min(n, cursor_ + kWindow); i < end; ++i) f(i);
    for (size_t i = cursor_, end = cursor_ > kWindow ? cursor_ - kWindow : 0; i-- > end;) f(i);
  }

  // The registers (16 bytes each) of the previous frame's draw `p` the same as `floats`.
  uint32_t SameRegisters(const Draw& p, const uint8_t* floats, uint32_t size) const {
    if (p.size != size) return 0;
    const uint8_t* q = previous_floats_.data() + p.offset;
    uint32_t same = 0;
    for (uint32_t i = 0; i < size; i += 16) same += std::memcmp(q + i, floats + i, 16) == 0;
    return same;
  }

  // A draw without one of its identity in the previous frame (a mesh that came into view or
  // changed its level of detail) moves with the camera: camera_floats() is its own float
  // constants with those the last matched draw of its shader changed since the previous frame
  // (the camera's, and anything else of the frame's, such as its time) at their previous
  // values, and its own others (its transform). Exact for static geometry, which the track's
  // new draws are; a moving object's own motion is missing for its first frame. nullptr, with
  // Found::kCamera, or Found::kNone without such a draw or if that draw changed every register
  // (a moving object's, nothing of the frame's told apart from its own).
  const Constants* Camera(const Constants& now, const uint8_t* floats, uint32_t size) {
    found_ = Found::kNone;
    const Draw* ref = nullptr;
    for (size_t i = current_.size() - 1; i-- > 0;) {
      const Draw& c = current_[i];
      if (c.constants.vs == now.vs && c.size == size && c.previous != kNone) {
        ref = &c;
        break;
      }
    }
    if (!ref) return nullptr;
    const uint8_t* ref_now = current_floats_.data() + ref->offset;
    const uint8_t* ref_then = previous_floats_.data() + previous_[ref->previous].offset;
    camera_floats_.assign(floats, floats + size);
    bool kept = false;
    for (uint32_t r = 0; r < size; r += 16) {
      if (!std::memcmp(ref_now + r, ref_then + r, 16))
        kept = true;
      else
        std::memcpy(camera_floats_.data() + r, ref_then + r, 16);
    }
    if (kept) found_ = Found::kCamera;
    return nullptr;
  }
  std::vector<Draw> current_, previous_;
  std::vector<uint8_t> current_floats_, previous_floats_;
  size_t cursor_ = 0;  // in previous_: after the last draw matched
  uint64_t frame_ = UINT64_MAX - 1;
  Found found_ = Found::kNone;
  std::vector<uint8_t> camera_floats_;  // Camera's
};

}  // namespace pgr4::native
