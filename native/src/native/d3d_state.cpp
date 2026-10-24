// pgr4-native PGR4 native renderer: Xenos register state per draw from the guest D3D device.
// See d3d_state.h for the model; the evidence (disassembly of the base-disc D3D library in the
// generated code, trace comparisons with --pgr4_d3d_tap) is in the development notes, "Renderer
// decision".
#include "d3d_state.h"

#include "guest_memory.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <xxhash.h>

#include <simde/x86/sse2.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <span>
#include <utility>

REXCVAR_DEFINE_BOOL(pgr4_native_track_verify, false, "PGR4 native",
                    "After each game draw, compare the guest register shadow with the tracked "
                    "registers and log the ones that differ (state tracker check)");

namespace pgr4::native {
namespace {

// The state the flushing calls send by dirty mask: (device offset, first register, registers,
// dirty mask (device + 8 * mask), the mask bit of the first register, registers per bit). The
// float constants (sub_826DE030) come first, then the register blocks of sub_826DDC90; their
// counts are the dirty-mask widths in sub_826D0340 (the shadow areas between the offsets are
// longer for 0x2000: 19 dwords, of which only 16 are ever flushed). The flush functions shift a
// block's bits to the top of the word and walk them with count-leading-zeros, so bit `top - k`
// covers the k-th group of `per` registers.
struct Block {
  uint32_t offset, reg, count;
  uint8_t mask, top, per, groups;
  uint64_t bits;  // the block's bits in its mask
  constexpr Block(uint32_t offset, uint32_t reg, uint32_t count, uint8_t mask, uint8_t top,
                  uint8_t per)
      : offset(offset),
        reg(reg),
        count(count),
        mask(mask),
        top(top),
        per(per),
        groups(uint8_t(count / per)),
        bits((~0ull >> (64 - count / per)) << (top + 1 - count / per)) {}
};
constexpr Block kFlushBlocks[] = {
    {dev::kVsConstants, 0x4000, 1024, 0, 63, 16},   // mask +0: c0..c255, 4 per bit
    {dev::kPsConstants, 0x4400, 1024, 1, 63, 16},   // mask +8
    {10368, 0x2000, 16, 2, 57, 1},                  // mask +16 bits 57..42
    {10444, 0x2100, 21, 2, 41, 1},                  // mask +16 bits 41..21
    {10528, 0x2180, 5, 2, 16, 1},                   // mask +16 bits 16..12
    {10548, 0x2200, 12, 2, 11, 1},                  // mask +16 bits 11..0
    {10596, 0x2280, 21, 3, 54, 1},                  // mask +24 bits 54..34
    {10680, 0x2300, 38, 4, 37, 1},                  // mask +32 bits 37..0
    {10832, 0x2380, 8, 4, 45, 1},                   // mask +32 bits 45..38
    {dev::kClipPlanes, 0x2388, 24, 4, 54, 4},       // mask +32 bits 54..49, sub_826DDD50
    {dev::kFetchConstants, 0x4800, 192, 3, 31, 6},  // mask +24 bits 31..0, sub_826DDED0
    {dev::kBoolLoop, 0x4900, 40, 4, 56, 40},        // mask +32 bit 56
};
// The register blocks alone. Their shadow is what the GPU has after every flushing call; the
// float constants' shadow may also hold values the game wrote without a dirty bit.
constexpr std::span<const Block> kRegisterBlocks = std::span(kFlushBlocks).subspan<2>();

// Mask +16 bits 17..20 make a draw call the shader load sub_826DEDC8 (bit 19 = vertex shader,
// 20 = pixel shader). Besides the stream, it writes registers of the 0x2000, 0x2180 and 0x2200
// blocks into the shadow (0x2001, 0x2180-0x2182 and, through sub_826DD6F8, 0x2203, 0x2208)
// and the draw flushes them in the same call, so their dirty bits are not set at its entry.
constexpr uint64_t kShaderLoadBits = 0x1E0000ull;
constexpr uint64_t kShaderLoadBlocks = (0xFFFFull << 42) | (0x1Full << 12) | 0xFFFull;  // mask +16
// Vertex fetch constant 31 (mask +24 bit 0): BeginVertices' inline vertex buffer.
constexpr uint64_t kFetch31 = 1ull;

// The most registers one CopyFlushed takes (every block, all groups).
constexpr uint32_t kFlushedMax = [] {
  uint32_t n = 0;
  for (const Block& b : kFlushBlocks) n += b.count;
  return n;
}();
// Update<6> (fetch constants) compares 8 registers per group: the last group's two extra lanes
// are the first VS constants, in the device and in raw_.
static_assert(dev::kFetchConstants + 192 * 4 == dev::kVsConstants);
static_assert(0x4800 + 192 + 2 <= kGuestRegisterCount);

// Bit k: register k of the four at p (guest order) equals raw[k].
inline uint32_t Same4(const uint8_t* p, const uint32_t* raw) {
  const simde__m128i a = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(p));
  const simde__m128i b = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(raw));
  return uint32_t(simde_mm_movemask_ps(simde_mm_castsi128_ps(simde_mm_cmpeq_epi32(a, b))));
}

// Registers reg..reg + kN - 1: those whose value at p (guest order) differs from raw[0..kN - 1]
// are taken into raw and regs (host order) and appended to `out`. 128-bit compares: a function
// with 256-bit registers ends in VZEROUPPER, which the Zenbleed workaround makes slow on the
// Deck's Zen 2.
template <uint32_t kN>
[[gnu::always_inline]] inline RegWrite* Update(const uint8_t* p, uint32_t reg, uint32_t* raw,
                                               uint32_t* regs, RegWrite* out) {
  static_assert(kN == 1 || kN == 6 || kN % 4 == 0);
  uint64_t differ;
  if constexpr (kN == 1) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    differ = v != raw[0];
  } else {
    uint64_t same = 0;
    for (uint32_t i = 0; i < kN; i += 4) same |= uint64_t(Same4(p + i * 4, raw + i)) << i;
    differ = ~same & (~0ull >> (64 - kN));
  }
  for (; differ; differ &= differ - 1) {
    const uint32_t i = uint32_t(std::countr_zero(differ));
    uint32_t v;
    std::memcpy(&v, p + i * 4, 4);
    raw[i] = v;
    regs[i] = __builtin_bswap32(v);
    *out++ = {reg + i, regs[i]};
  }
  return out;
}

// The groups of kFlushBlocks[kI] whose bit is set in `mask`, in register order.
template <size_t kI>
[[gnu::always_inline]] inline RegWrite* FlushBlock(const uint8_t* base, uint32_t device,
                                                   uint64_t mask, uint32_t* raw, uint32_t* regs,
                                                   RegWrite* out) {
  constexpr Block b = kFlushBlocks[kI];
  uint64_t m = (mask & b.bits) << (63 - b.top);  // group g at bit 63 - g
  if (!m) return out;
  const uint8_t* const p = HostPtr(base, device + b.offset);
  do {
    const uint32_t g = uint32_t(std::countl_zero(m));
    m ^= (1ull << 63) >> g;
    const uint32_t first = b.reg + g * b.per;
    out = Update<b.per>(p + g * b.per * 4, first, raw + first, regs + first, out);
  } while (m);
  return out;
}

// Shader-rebind dirty bits in the mask at +16 (sub_826DEDC8): 19 vertex shader, 20 pixel shader.
constexpr uint64_t kVsRebind = 1ull << 19, kPsRebind = 1ull << 20;

}  // namespace

void RegLog::Grow(size_t capacity) {
  capacity = std::max({capacity, capacity_ * 2, size_t(4096)});
  auto data = std::make_unique_for_overwrite<RegWrite[]>(capacity);
  if (size_) std::memcpy(data.get(), data_.get(), size_ * sizeof(RegWrite));
  data_ = std::move(data);
  capacity_ = capacity;
}

void GuestStateTracker::Copy(uint32_t reg, const uint8_t* base, uint32_t src, uint32_t count) {
  const uint8_t* const p = HostPtr(base, src);
  RegLog& log = sink_ ? *sink_ : discard_;
  RegWrite* out = log.Reserve(count);
  uint32_t i = 0;
  for (; i + 4 <= count; i += 4)
    out = Update<4>(p + i * 4, reg + i, raw_ + reg + i, regs_ + reg + i, out);
  // One at a time: the source may end with the guest's allocation.
  for (; i < count; ++i) out = Update<1>(p + i * 4, reg + i, raw_ + reg + i, regs_ + reg + i, out);
  if (sink_) log.Commit(out);
}

void GuestStateTracker::CopyFlushed(const uint8_t* base, uint32_t device,
                                    const uint64_t flushed[6]) {
  RegLog& log = sink_ ? *sink_ : discard_;
  RegWrite* out = log.Reserve(kFlushedMax);
  [&]<size_t... kI>(std::index_sequence<kI...>) {
    ((out = FlushBlock<kI>(base, device, flushed[kFlushBlocks[kI].mask], raw_, regs_, out)), ...);
  }(std::make_index_sequence<std::size(kFlushBlocks)>());
  if (sink_) log.Commit(out);
}
void GuestStateTracker::Verify(const uint8_t* base, uint32_t device, uint32_t fn_addr) {
  static uint64_t checks = 0, differing = 0;
  static uint32_t reported = 0;
  static bool seen[kGuestRegisterCount] = {};
  ++checks;
  for (const Block& b : kRegisterBlocks) {
    const uint8_t* p = HostPtr(base, device + b.offset);
    for (uint32_t i = 0; i < b.count; ++i) {
      const uint32_t reg = b.reg + i;
      uint32_t v;
      std::memcpy(&v, p + i * 4, 4);
      if (v == raw_[reg] || reg == 0x2102) continue;  // VGT_INDX_OFFSET: per draw, not the shadow
      ++differing;
      if (!seen[reg] && reported < 64) {
        seen[reg] = true;
        ++reported;
        REXLOG_INFO(
            "PGR4 native state verify: register {:04X} shadow {:08X} tracked {:08X} after {:08X}",
            reg, __builtin_bswap32(v), regs_[reg], fn_addr);
      }
    }
  }
  if (checks >= 1024 && (checks & (checks - 1)) == 0)
    REXLOG_INFO("PGR4 native state verify: {} draws checked, {} differing register values", checks,
                differing);
}

void GuestStateTracker::RecordInto(RegLog* sink) {
  if (sink && !synced_) {
    synced_ = true;
    for (uint32_t reg = 0; reg < kGuestRegisterCount; ++reg)
      if (regs_[reg]) sink->Append({reg, regs_[reg]});
  }
  sink_ = sink;
}

// sub_826DEBD8: a shader object's literal constants, LOAD_ALU_CONSTANT'd from guest memory into
// the float constants when the shader is (re)bound. `table` is the object's constant table,
// `data` the virtual base its offsets are relative to.
void GuestStateTracker::LoadLiterals(const uint8_t* base, uint32_t table, uint32_t data) {
  uint32_t rel = Load32(base, table + 20);
  if (!rel) return;
  uint32_t head = table + rel;
  uint32_t cur = head + 20, end = cur + Load32(base, head + 16);
  while (cur < end) {
    uint32_t index = Load16(base, cur), count = Load16(base, cur + 2);
    if (!count) break;
    uint32_t src = Load32(base, cur + 4) + data;
    cur += 8;
    uint32_t first = 0x4000 + ((index * 4) & 0x7FF);
    count &= 0xFFF;
    if (first + count > 0x4800) count = 0x4800 - first;
    Copy(first, base, src, count);
  }
}

CallEntry ReadDirtyMasks(const uint8_t* base, uint32_t device) {
  CallEntry e;
  for (uint32_t i = 0; i < 6; ++i) e.dirty[i] = Load64(base, device + dev::kDirtyMasks + i * 8);
  e.vertex_stride_dwords = *HostPtr(base, device + 11824);
  return e;
}

void PrefetchIndexBuffer(const uint8_t* base, uint32_t device) {
  const uint8_t* ib = HostPtr(base, Load32(base, device + dev::kIndexBuffer));
  __builtin_prefetch(ib);       // format dword (+0)
  __builtin_prefetch(ib + 24);  // address (+24), on the next line when the object straddles one
}

void MarkAllDirty(uint8_t* base, uint32_t device) {
  uint64_t add[6] = {0, 0, kShaderLoadBits, 0, 0, 0};
  for (const Block& b : kFlushBlocks) add[b.mask] |= b.bits;
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t a = device + dev::kDirtyMasks + i * 8;
    Store64(base, a, Load64(base, a) | add[i]);
  }
}

GuestStateTracker& GuestState() {
  static GuestStateTracker* tracker = new GuestStateTracker();  // 160 KB, never freed
  return *tracker;
}

void GuestStateTracker::Attach(const uint8_t* base, uint32_t device) {
  // A new device (the game creates a second one at the title screen): start from its shadow.
  static uint32_t attaches = 0;
  if (++attaches <= 8)
    REXLOG_INFO("PGR4 native state: tracking device {:08X} (was {:08X})", device, device_);
  device_ = device;
  verify_ = REXCVAR_GET(pgr4_native_track_verify);
  constexpr uint64_t kAll[6] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
  CopyFlushed(base, device, kAll);
  vs_ = ps_ = {};
  inline_vertices_ = 0;
}

void GuestStateTracker::AfterCall(const uint8_t* base, uint32_t fn_addr, const CallEntry& e,
                                  const uint32_t r[8], uint64_t ret) {
  const uint32_t device = r[0];
  if (!device) return;
  if (device != device_) Attach(base, device);
  // DrawVerticesUP is BeginVertices + a copy + RestorePush; the nested BeginVertices call
  // already reported its flush and its inline vertex buffer.
  if (fn_addr == guest_fn::kDrawVerticesUP) return;
  // An immediate VS load belongs to the draw it was made in; one made by Clear/Resolve's own
  // draws (DrawIndx2Inline) never reaches a game draw (the D3D re-dirties the VS after them).
  const bool vs_imm_new = vs_imm_pending_valid_;
  vs_imm_pending_valid_ = false;

  // What the call sent to the GPU. Float constants: only the 4-vec4 groups whose dirty bit the
  // call cleared (sub_826DE030; bit 63 - k = constants 4k..4k+3). The rest of the shadow may
  // hold values the game wrote without a dirty bit (c247-c255 in the tap traces).
  //
  // Register blocks, clip planes, fetch and bool/loop constants: at the end of every flushing
  // call the shadow equals what the GPU has for these (the tap traces show no other
  // differences: the draw flushes whatever is dirty, and Clear/Resolve re-dirty what they
  // overwrite in the stream, so the next draw restores it). A game draw changes only what was
  // dirty at its entry and what its shader load wrote; Clear, Resolve, Swap and the D3D's own
  // draws may write the shadow themselves, so after them every block is compared.
  uint64_t flushed[6];
  for (uint32_t m = 0; m < 2; ++m)
    flushed[m] = e.dirty[m] & ~Load64(base, device + dev::kDirtyMasks + m * 8);
  const bool game_draw = fn_addr == guest_fn::kDrawIndexed || fn_addr == guest_fn::kDrawVertices ||
                         fn_addr == guest_fn::kBeginVertices;
  const bool stride_changed =
      fn_addr == guest_fn::kBeginVertices && ((r[3] >> 2) & 0xFF) != e.vertex_stride_dwords;
  if (game_draw) {
    const bool shader_load = (e.dirty[2] & kShaderLoadBits) || stride_changed;
    flushed[2] = e.dirty[2] | (shader_load ? kShaderLoadBlocks : 0);
    flushed[3] = e.dirty[3] | (inline_fetch_ ? kFetch31 : 0);
    flushed[4] = e.dirty[4];
    flushed[5] = e.dirty[5];
  } else {
    std::fill(flushed + 2, flushed + 6, ~0ull);
  }
  inline_fetch_ = false;
  CopyFlushed(base, device, flushed);
  if (game_draw && verify_) [[unlikely]]
    Verify(base, device, fn_addr);

  // Window scissor: sub_826C33C0 writes PA_SC_WINDOW_OFFSET/SCISSOR_TL/BR straight into the
  // stream from the rectangle SetScissorRect (fn_826C59D8) keeps at device+10436/+10440 (15-bit
  // signed x | y << 16); Resolve, Clear and the others write their own and then restore it
  // from there. Untiled form (offset 0); with predicated tiling the D3D writes one clamped
  // copy per tile instead.
  {
    const uint32_t tl = Load32(base, device + dev::kScissorTL),
                   br = Load32(base, device + dev::kScissorBR);
    auto field = [](uint32_t v) { return uint32_t(int32_t(v << 17) >> 17); };
    Put(0x2080, 0);
    Put(0x2081, ((field(tl >> 16) << 16) & 0x3FFF0000u) | (field(tl) & 0x3FFFu));
    Put(0x2082, ((field(br >> 16) << 16) & 0x3FFF0000u) | (field(br) & 0x3FFFu));
  }

  const bool draw = fn_addr == guest_fn::kBeginVertices || fn_addr == guest_fn::kDrawVertices ||
                    fn_addr == guest_fn::kDrawIndexed || fn_addr == guest_fn::k10A8;
  if (draw) {
    // Bound shaders and the literal constants sub_826DEDC8 loads when they are rebound (after
    // the constant flush, so they win over the shadow). That load, under the mask +16 bits
    // 17..20, is the only place the D3D changes which ucode the GPU runs and patches a variant
    // for a declaration, so the shader objects are read only then or when the object differs
    // (after an attach, which binds none).
    const uint32_t vs = Load32(base, device + dev::kVertexShader);
    const uint32_t ps = Load32(base, device + dev::kPixelShader);
    uint64_t rebind = e.dirty[2];
    if (stride_changed && vs) rebind |= kVsRebind;  // BeginVertices sets it itself
    const bool shader_load = (rebind & kShaderLoadBits) != 0;
    if (!vs) {
      vs_ = {};
    } else if (vs_imm_new) {
      // sub_826DEA78 ran inside this draw: the GPU now holds the patched copy.
      vs_imm_.swap(vs_imm_pending_);
      vs_ = vs_imm_pending_ref_;
      vs_.immediate = vs_imm_.data();
    } else if (vs_.immediate ? (rebind & kVsRebind) != 0 : shader_load || vs != vs_.object) {
      // IM_LOAD of the object's variant. After an immediate load the D3D reloads the VS only
      // under the VS rebind bit. Variant 1 is used without a pixel shader when the object has
      // one (flag 0x20).
      const uint32_t variant = (!ps && (Load32(base, vs + 872) & 0x20)) ? 1 : 0;
      const uint32_t sub = vs + Load32(base, vs + 896 + variant * 8);
      vs_ = {vs, GpuAddress(Load32(base, vs + 32) + Load32(base, sub + 872)),
             Load32(base, sub + 876) >> 2};
      vs_.binding = XXH3_64bits(HostPtr(base, vs + 416 * variant + 40), 24);
    }
    if (vs && (rebind & (kVsRebind | kPsRebind)))
      LoadLiterals(base, vs + 872, Load32(base, vs + 32));
    if (!ps) {
      ps_ = {};
    } else if (shader_load || ps != ps_.object) {
      const uint32_t sub = ps + Load32(base, ps + 64);
      ps_ = {ps, GpuAddress(Load32(base, ps + 24) + Load32(base, sub + 40)),
             Load32(base, sub + 44) >> 2};
    }
    if (ps && (rebind & kPsRebind)) LoadLiterals(base, ps + 40, Load32(base, ps + 24));
  }

  // Registers the draw functions write straight into the stream (not into the shadow).
  switch (fn_addr) {
    case guest_fn::kDrawIndexed:   // VGT_INDX_OFFSET = BaseVertexIndex
    case guest_fn::kDrawVertices:  // VGT_INDX_OFFSET = StartVertex
      Put(0x2102, r[2]);
      inline_vertices_ = 0;
      break;
    case guest_fn::kDrawIndx2Split:  // Swap's internal draw
      Put(0x2102, 0);
      break;
    case guest_fn::kBeginVertices: {
      Put(0x2102, 0);
      uint32_t p = uint32_t(ret);
      if (!p) break;  // no space: nothing was written
      // Vertex fetch 93-95 (fetch constant 31): the inline vertices behind the packets. The
      // D3D re-dirties slot 31 so the next draw restores the shadow's value.
      uint32_t bytes = r[2] * r[3];
      const uint32_t f = 0x4800 + 31 * 6;
      Put(f + 0, 1);
      Put(f + 1, 0);
      Put(f + 2, 1);
      Put(f + 3, 0);
      Put(f + 4, GpuAddress(p) | 3);
      Put(f + 5, 0x10000002u | (((bytes >> 2) & 0xFFFFFFu) << 2));
      inline_fetch_ = true;
      inline_vertices_ = GpuAddress(p);
      break;
    }
    default:
      break;
  }
}

void GuestStateTracker::AfterImmediateVs(const uint8_t* base, const uint32_t r[8]) {
  // sub_826DEA78(device, patch, vs object, ..., variant): reserves 5 + n dwords, writes
  // INVALIDATE_STATE (0xC0003B00, 0x100) and IM_LOAD_IMMEDIATE (header, 0 = vertex, n), copies
  // the n ucode dwords of variant [vs + 896 + 8 * variant], patches them in place and leaves
  // the push cursor (device+48) on the last of them. No space: nothing is written.
  const uint32_t device = r[0], vs = r[2], variant = r[7];
  if (!device || !vs || variant > 7) return;
  const uint32_t sub = vs + Load32(base, vs + 896 + variant * 8);
  const uint32_t dwords = Load32(base, sub + 876) >> 2;
  if (!dwords || dwords > 0x3FFF) return;
  const uint32_t start = Load32(base, device + dev::kPush) - (dwords - 1) * 4;
  if (Load32(base, start - 12) != (0xC0002B00u | (((dwords + 1) & 0x3FFFu) << 16)) ||
      Load32(base, start - 8) != 0 || Load32(base, start - 4) != dwords) {
    static uint32_t misses = 0;
    if (++misses <= 4)
      REXLOG_WARN("PGR4 native state: no IM_LOAD_IMMEDIATE before push cursor {:08X} (vs {:08X})",
                  start, vs);
    return;
  }
  vs_imm_pending_.resize(dwords);
  std::memcpy(vs_imm_pending_.data(), HostPtr(base, start), size_t(dwords) * 4);
  vs_imm_pending_ref_ = {vs, GpuAddress(start), dwords, nullptr, ++vs_imm_seq_};
  vs_imm_pending_valid_ = true;
}

bool GuestStateTracker::ReadDrawState(const uint8_t* base, uint32_t device, const DrawArgs& a,
                                      DrawState* s) {
  if (!device || device != device_) return false;
  *s = DrawState{};
  s->regs = regs_;
  s->fn = a.fn;
  s->device = device;
  switch (a.fn) {
    case HookFn::kDrawIndexedVertices: {  // (dev, prim, base vertex, start index, index count)
      s->primitive = a.r[1] & 0x3F;
      s->base_vertex = a.r[2];
      s->count = a.r[4];
      s->indexed = true;
      if (const uint32_t ib = Load32(base, device + dev::kIndexBuffer)) {
        uint32_t d0 = Load32(base, ib);
        s->index_32bit = d0 >> 31;
        s->index_endian = (d0 >> 29) & 3;
        s->index_address = GpuAddress(Load32(base, ib + 24) + a.r[3] * (s->index_32bit ? 4 : 2));
      }
      break;
    }
    case HookFn::kDrawVertices:  // (dev, prim, start vertex, vertex count)
      s->primitive = a.r[1] & 0x3F;
      s->base_vertex = a.r[2];
      s->count = a.r[3];
      break;
    case HookFn::kBeginVertices:   // (dev, prim, vertex count, stride)
    case HookFn::kDrawVerticesUP:  // (dev, prim, vertex count, data, stride)
      s->primitive = a.r[1] & 0x3F;
      s->count = a.r[2];
      s->inline_vertices = inline_vertices_;
      break;
    default:
      return false;
  }
  s->vs = vs_;
  s->ps = ps_;
  return true;
}

bool IsTapOnlyRegister(uint32_t reg) {
  // Written into the stream by Resolve (RB_COPY_DEST_BASE) and the occlusion queries
  // (RB_SAMPLE_COUNT_ADDR); no draw reads them, Resolve's own state comes from its arguments.
  return reg == 0x2319 || reg == 0x2325;
}

uint32_t GuestStateTracker::CompareWithTap(const uint32_t* tap_regs, const uint8_t* tap_written,
                                           uint64_t* per_reg, uint64_t* per_tile) const {
  uint32_t bad = 0;
  for (uint32_t r = 0x2000; r < kGuestRegisterCount; ++r) {
    if (!tap_written[r] || tap_regs[r] == regs_[r] || IsTapOnlyRegister(r)) continue;
    if (tap_written[r] == kTapWrittenPredicated) {
      if (per_tile) ++*per_tile;
      continue;
    }
    ++bad;
    if (per_reg) ++per_reg[r];
  }
  return bad;
}

}  // namespace pgr4::native
