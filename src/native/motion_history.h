// pgr4-native PGR4 native renderer: the previous frame's constants of each draw, for the motion
// vector pass of the upscaler's scene (DxbcShaderTranslator's motion_vectors).
//
// A draw is matched with the previous frame's by its identity (its vertex shader, index buffer
// and index count: the draws of a frame keep these across frames, while per-frame vertex streams
// such as instance transforms move) and, among draws of the same identity, by its order. 2520 of
// the 2522 scene draws of a race frame match (td-265 draw logs). An unmatched draw reads its own
// constants and gets no motion of its own.
//
// Flat open-addressing tables with generation stamps instead of clearing. The constants stay in
// the upload memory they were committed to, which the constant pool keeps for one more frame
// (UploadPool::set_keep_previous_frame).
//
// Threading: the thread that records the draws.
#pragma once

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <memory>
#include <utility>

namespace pgr4::native {

class MotionHistory {
 public:
  struct Constants {
    const void* vs = nullptr;  // the shader the float constants are packed for
    D3D12_GPU_VIRTUAL_ADDRESS float_constants = 0, fetch_constants = 0;
  };

  // Frame `frame`'s first draw: the last frame's draws become the previous frame's, if that was
  // frame - 1 (the constant pool keeps one frame's memory, drawn or not).
  void BeginFrame(uint64_t frame) {
    stamp_ += frame == frame_ + 1 ? 1 : 2;
    frame_ = frame;
    std::swap(current_, previous_);
    current_used_ = counts_used_ = 0;
  }

  // The key of this frame's next draw with `identity`.
  uint64_t Key(uint64_t identity) {
    const uint64_t id = Mix(identity);
    uint32_t occurrence = 0;
    if (Slot* s = Insert(*counts_, id, counts_used_)) occurrence = s->count++;
    return Mix(id + occurrence);
  }

  // The previous frame's constants of the draw with `key`, or nullptr.
  const Constants* Previous(uint64_t key) const {
    const Slot& s = (*previous_)[Find(*previous_, key, stamp_ - 1)];
    return s.stamp == stamp_ - 1 ? &s.constants : nullptr;
  }

  void Record(uint64_t key, const Constants& c) {
    if (Slot* s = Insert(*current_, key, current_used_)) s->constants = c;
  }

 private:
  static constexpr uint32_t kSlots = 8192;  // a power of two, three times a frame's draws
  static constexpr uint32_t kMaxUsed = kSlots / 2;
  struct Slot {
    uint64_t key = 0, stamp = 0;
    Constants constants;
    uint32_t count = 0;  // the identity's draws so far (counts_)
  };
  using Table = std::array<Slot, kSlots>;

  static uint64_t Mix(uint64_t x) {  // the murmur3 finalizer
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    return x ^ (x >> 33);
  }
  // The index of `key` among the slots of generation `stamp`, or of the free slot where it goes
  // (linear probing; at most kMaxUsed slots are live, so a free one is near).
  static uint32_t Find(const Table& t, uint64_t key, uint64_t stamp) {
    uint32_t i = uint32_t(key) & (kSlots - 1);
    while (t[i].stamp == stamp && t[i].key != key) i = (i + 1) & (kSlots - 1);
    return i;
  }
  // `key`'s slot of this frame, made if new; nullptr when the table is full.
  Slot* Insert(Table& t, uint64_t key, uint32_t& used) {
    Slot& s = t[Find(t, key, stamp_)];
    if (s.stamp != stamp_) {
      if (used == kMaxUsed) return nullptr;
      ++used;
      s = {key, stamp_, {}, 0};
    }
    return &s;
  }

  uint64_t stamp_ = 2;  // the tables start at generation 0: empty
  uint64_t frame_ = UINT64_MAX - 1;
  std::unique_ptr<Table> tables_[3] = {std::make_unique<Table>(), std::make_unique<Table>(),
                                       std::make_unique<Table>()};
  Table* current_ = tables_[0].get();
  Table* previous_ = tables_[1].get();
  Table* counts_ = tables_[2].get();
  uint32_t current_used_ = 0, counts_used_ = 0;
};

}  // namespace pgr4::native
