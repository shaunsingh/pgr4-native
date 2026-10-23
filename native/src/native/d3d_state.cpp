// pgr4-native PGR4 native renderer: Xenos register state per draw from the guest D3D device.
// See d3d_state.h for the model; the evidence (disassembly of the base-disc D3D library in the
// generated code, M1 trace comparisons) is in the development notes, "Renderer decision".
#include "d3d_state.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <xxhash.h>

#include <emmintrin.h>

#include <cstring>

REXCVAR_DEFINE_BOOL(pgr4_native_track_verify, false, "PGR4 native",
                    "After each game draw, compare the guest register shadow with the tracked "
                    "registers and log the ones that differ (state tracker check)");

namespace pgr4::native {
namespace {

// Guest virtual memory as the generated code addresses it (the 0xE0000000 view is 4 KiB higher).
inline const uint8_t* Host(const uint8_t* base, uint32_t a) {
  return base + a + (a >= 0xE0000000u ? 0x1000u : 0u);
}
inline uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Host(base, a), 4);
  return __builtin_bswap32(v);
}
inline uint16_t Load16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, Host(base, a), 2);
  return __builtin_bswap16(v);
}
inline uint64_t Load64(const uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, Host(base, a), 8);
  return __builtin_bswap64(v);
}
// Virtual -> physical the way the D3D computes GPU addresses (e.g. sub_8269ADD8's index base).
inline uint32_t GpuAddress(uint32_t a) {
  return (a & 0x1FFFFFFFu) + ((((a >> 20) & 0xFFFu) + 0x200u) & 0x1000u);
}

// Register blocks the draw functions flush with sub_826A8720, (device offset, first register,
// registers, dirty mask (device + 8 * mask), the mask bit of the first register, registers per
// bit). The counts are the dirty-mask widths in sub_8269ADD8 (the shadow areas between the
// offsets are longer for 0x2000: 19 dwords, of which only 16 are ever flushed). The flush
// functions shift a block's bits to the top of the word and walk them with count-leading-zeros,
// so bit `top - k` covers the k-th group of `per` registers.
struct Block {
  uint32_t offset, reg, count;
  uint8_t mask, top, per;
};
constexpr Block kRegisterBlocks[] = {
    {10368, 0x2000, 16, 2, 57, 1},                  // mask +16 bits 57..42
    {10444, 0x2100, 21, 2, 41, 1},                  // mask +16 bits 41..21
    {10528, 0x2180, 5, 2, 16, 1},                   // mask +16 bits 16..12
    {10548, 0x2200, 12, 2, 11, 1},                  // mask +16 bits 11..0
    {10596, 0x2280, 21, 3, 54, 1},                  // mask +24 bits 54..34
    {10680, 0x2300, 38, 4, 37, 1},                  // mask +32 bits 37..0
    {10832, 0x2380, 8, 4, 45, 1},                   // mask +32 bits 45..38
    {dev::kClipPlanes, 0x2388, 24, 4, 54, 4},       // mask +32 bits 54..49, sub_826A87E0
    {dev::kFetchConstants, 0x4800, 192, 3, 31, 6},  // mask +24 bits 31..0, sub_826A8960
    {dev::kBoolLoop, 0x4900, 40, 4, 56, 40},        // mask +32 bit 56
};

// Mask +16 bits 17..20 make a draw call the shader load sub_826A9858 (bit 19 = vertex shader,
// 20 = pixel shader). Besides the stream, it writes registers of the 0x2000, 0x2180 and 0x2200
// blocks into the shadow (0x2001, 0x2180-0x2182 and, through sub_826A8188, 0x2203, 0x2208)
// and the draw flushes them in the same call, so their dirty bits are not set at its entry.
constexpr uint64_t kShaderLoadBits = 0x1E0000ull;
constexpr uint64_t kShaderLoadBlocks = (0xFFFFull << 42) | (0x1Full << 12) | 0xFFFull;  // mask +16
// Vertex fetch constant 31 (mask +24 bit 0): BeginVertices' inline vertex buffer.
constexpr uint64_t kFetch31 = 1ull;

constexpr uint32_t kFnBeginVertices = 0x8269A4F8, kFnDrawVerticesUP = 0x8269A9A8,
                   kFnDrawVertices = 0x8269A9F0, kFnDrawIndexed = 0x8269ADD8,
                   kFnDrawIndx2Inline = 0x8269B230, kFnDrawIndx2Split = 0x826A7318,
                   kFnClearF = 0x8269C378, kFnClear = 0x8269C4A0, kFnResolve = 0x82696978,
                   kFnSwap = 0x82695BE0, kFn10A8 = 0x826A10A8;

// Shader-rebind dirty bits in the mask at +16 (sub_826A9858): 19 vertex shader, 20 pixel shader.
constexpr uint64_t kVsRebind = 1ull << 19, kPsRebind = 1ull << 20;

}  // namespace

void GuestStateTracker::Copy(uint32_t reg, const uint8_t* base, uint32_t src, uint32_t count) {
  const uint8_t* p = Host(base, src);
  auto put_raw = [&](uint32_t i) {
    uint32_t v;
    std::memcpy(&v, p + i * 4, 4);
    if (v == raw_[reg + i]) return;
    raw_[reg + i] = v;
    regs_[reg + i] = __builtin_bswap32(v);
    Mark(reg + i);
  };
  // Most of what the flushing calls copy is unchanged: compare 16 registers per branch.
  auto eq4 = [&](uint32_t i) {
    const __m128i g = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i * 4));
    const __m128i m = _mm_loadu_si128(reinterpret_cast<const __m128i*>(raw_ + reg + i));
    return _mm_cmpeq_epi32(g, m);
  };
  uint32_t i = 0;
  for (; i + 16 <= count; i += 16) {
    const __m128i e = _mm_and_si128(_mm_and_si128(eq4(i), eq4(i + 4)),
                                    _mm_and_si128(eq4(i + 8), eq4(i + 12)));
    if (_mm_movemask_epi8(e) == 0xFFFF) continue;
    for (uint32_t k = 0; k < 16; ++k) put_raw(i + k);
  }
  for (; i + 4 <= count; i += 4) {
    if (_mm_movemask_epi8(eq4(i)) == 0xFFFF) continue;
    for (uint32_t k = 0; k < 4; ++k) put_raw(i + k);
  }
  for (; i < count; ++i) put_raw(i);
}

void GuestStateTracker::CopyDirty(const uint8_t* base, uint32_t device, const uint64_t dirty[6]) {
  for (const Block& b : kRegisterBlocks) {
    const uint32_t groups = b.count / b.per;
    uint64_t m = (dirty[b.mask] << (63 - b.top)) & ~(~0ull >> groups);  // group g at bit 63 - g
    uint32_t g = 0;
    while (m) {
      const uint32_t skip = uint32_t(__builtin_clzll(m));
      m <<= skip;
      g += skip;
      const uint32_t run = ~m ? uint32_t(__builtin_clzll(~m)) : 64;
      Copy(b.reg + g * b.per, base, device + b.offset + g * b.per * 4, run * b.per);
      g += run;
      m = run < 64 ? m << run : 0;
    }
  }
}

void GuestStateTracker::Verify(const uint8_t* base, uint32_t device, uint32_t fn_addr) {
  static uint64_t checks = 0, differing = 0;
  static uint32_t reported = 0;
  static bool seen[kGuestRegisterCount] = {};
  ++checks;
  for (const Block& b : kRegisterBlocks) {
    const uint8_t* p = Host(base, device + b.offset);
    for (uint32_t i = 0; i < b.count; ++i) {
      const uint32_t reg = b.reg + i;
      uint32_t v;
      std::memcpy(&v, p + i * 4, 4);
      if (v == raw_[reg] || reg == 0x2102) continue;  // VGT_INDX_OFFSET: per draw, not the shadow
      ++differing;
      if (!seen[reg] && reported < 64) {
        seen[reg] = true;
        ++reported;
        REXLOG_INFO("PGR4 native state verify: register {:04X} shadow {:08X} tracked {:08X} after {:08X}",
                    reg, __builtin_bswap32(v), regs_[reg], fn_addr);
      }
    }
  }
  if (checks >= 1024 && (checks & (checks - 1)) == 0)
    REXLOG_INFO("PGR4 native state verify: {} draws checked, {} differing register values", checks,
                differing);
}

void GuestStateTracker::TakeChanges(std::vector<RegWrite>* out) {
  for (uint32_t s = 0; s < kChangedWordGroups; ++s) {
    uint64_t words = changed_words_[s];
    changed_words_[s] = 0;
    while (words) {
      const uint32_t w = s * 64 + uint32_t(__builtin_ctzll(words));
      words &= words - 1;
      uint64_t bits = changed_[w];
      changed_[w] = 0;
      while (bits) {
        const uint32_t reg = w * 64 + uint32_t(__builtin_ctzll(bits));
        bits &= bits - 1;
        out->push_back({reg, regs_[reg]});
      }
    }
  }
}

// sub_826A9668: a shader object's literal constants, LOAD_ALU_CONSTANT'd from guest memory into
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
  e.vertex_stride_dwords = *Host(base, device + 11824);
  return e;
}

bool IsStateFlushingFn(uint32_t addr) {
  switch (addr) {
    case kFnBeginVertices: case kFnDrawVerticesUP: case kFnDrawVertices: case kFnDrawIndexed:
    case kFnDrawIndx2Inline: case kFnDrawIndx2Split: case kFnClearF: case kFnClear:
    case kFnResolve: case kFnSwap: case kFn10A8:
      return true;
    default:
      return false;
  }
}

GuestStateTracker& GuestState() {
  static GuestStateTracker* tracker = new GuestStateTracker();  // 80 KB, never freed
  return *tracker;
}

void GuestStateTracker::Attach(const uint8_t* base, uint32_t device) {
  // A new device (the game creates a second one at the title screen): start from its shadow.
  static uint32_t attaches = 0;
  if (++attaches <= 8) REXLOG_INFO("PGR4 native state: tracking device {:08X} (was {:08X})", device, device_);
  device_ = device;
  Copy(0x4000, base, device + dev::kVsConstants, 1024);
  Copy(0x4400, base, device + dev::kPsConstants, 1024);
  for (const Block& b : kRegisterBlocks) Copy(b.reg, base, device + b.offset, b.count);
  vs_ = ps_ = {};
  inline_vertices_ = inline_bytes_ = 0;
}

void GuestStateTracker::AfterCall(const uint8_t* base, uint32_t fn_addr, const CallEntry& e,
                                  const uint32_t r[8], uint64_t ret) {
  const uint32_t device = r[0];
  if (!device) return;
  if (device != device_) Attach(base, device);
  // DrawVerticesUP is BeginVertices + a copy + RestorePush; the nested BeginVertices call
  // already reported its flush and its inline vertex buffer.
  if (fn_addr == kFnDrawVerticesUP) return;
  // An immediate VS load belongs to the draw it was made in; one made by Clear/Resolve's own
  // draws (DrawIndx2Inline) never reaches a game draw (the D3D re-dirties the VS after them).
  const bool vs_imm_new = vs_imm_pending_valid_;
  vs_imm_pending_valid_ = false;

  // Float constants: only the 4-vec4 groups whose dirty bit the call cleared reached the GPU
  // (sub_826A8AC0; bit 63 - k = constants 4k..4k+3). The rest of the shadow may hold values
  // the game wrote without a dirty bit (c247-c255 in the M1 traces).
  for (uint32_t m = 0; m < 2; ++m) {
    uint64_t now = Load64(base, device + dev::kDirtyMasks + m * 8);
    uint64_t flushed = e.dirty[m] & ~now;
    const uint32_t reg = m ? 0x4400 : 0x4000, off = m ? dev::kPsConstants : dev::kVsConstants;
    while (flushed) {
      uint32_t k = uint32_t(__builtin_clzll(flushed));
      flushed &= ~(1ull << (63 - k));
      Copy(reg + k * 16, base, device + off + k * 64, 16);
    }
  }

  // Register blocks, clip planes, fetch and bool/loop constants: at the end of every flushing
  // call the shadow equals what the GPU has for these (the M1 traces show no other
  // differences: the draw flushes whatever is dirty, and Clear/Resolve re-dirty what they
  // overwrite in the stream, so the next draw restores it). A game draw changes only what was
  // dirty at its entry and what its shader load wrote; Clear, Resolve, Swap and the D3D's own
  // draws may write the shadow themselves, so after them every block is compared.
  const bool game_draw =
      fn_addr == kFnDrawIndexed || fn_addr == kFnDrawVertices || fn_addr == kFnBeginVertices;
  const bool stride_changed =
      fn_addr == kFnBeginVertices && ((r[3] >> 2) & 0xFF) != e.vertex_stride_dwords;
  if (game_draw) {
    uint64_t dirty[6];
    std::memcpy(dirty, e.dirty, sizeof(dirty));
    if ((dirty[2] & kShaderLoadBits) || stride_changed) dirty[2] |= kShaderLoadBlocks;
    if (inline_fetch_) dirty[3] |= kFetch31;
    CopyDirty(base, device, dirty);
    if (REXCVAR_GET(pgr4_native_track_verify)) Verify(base, device, fn_addr);
  } else {
    for (const Block& b : kRegisterBlocks) Copy(b.reg, base, device + b.offset, b.count);
  }
  inline_fetch_ = false;

  // Window scissor: sub_8268DEB0 writes PA_SC_WINDOW_OFFSET/SCISSOR_TL/BR straight into the
  // stream from the rectangle SetScissorRect (fn_826904C8) keeps at device+10436/+10440 (15-bit
  // signed x | y << 16); Resolve, Clear and the others write their own and then restore it
  // from there. Untiled form (offset 0); with predicated tiling the D3D writes one clamped
  // copy per tile instead.
  {
    const uint32_t tl = Load32(base, device + dev::kScissorTL), br = Load32(base, device + dev::kScissorBR);
    auto field = [](uint32_t v) { return uint32_t(int32_t(v << 17) >> 17); };
    Put(0x2080, 0);
    Put(0x2081, ((field(tl >> 16) << 16) & 0x3FFF0000u) | (field(tl) & 0x3FFFu));
    Put(0x2082, ((field(br >> 16) << 16) & 0x3FFF0000u) | (field(br) & 0x3FFFu));
  }

  const bool draw = fn_addr == kFnBeginVertices || fn_addr == kFnDrawVertices ||
                    fn_addr == kFnDrawIndexed || fn_addr == kFn10A8;
  if (draw) {
    // Bound shaders and the literal constants sub_826A9858 loads when they are rebound (after
    // the constant flush, so they win over the shadow).
    uint32_t vs = Load32(base, device + dev::kVertexShader);
    uint32_t ps = Load32(base, device + dev::kPixelShader);
    uint64_t rebind = e.dirty[2];
    if (stride_changed && vs) rebind |= kVsRebind;  // BeginVertices sets it itself
    if (vs) {
      if (vs_imm_new) {
        // sub_826A9508 ran inside this draw: the GPU now holds the patched copy.
        vs_imm_.swap(vs_imm_pending_);
        vs_ = vs_imm_pending_ref_;
        vs_.immediate = vs_imm_.data();
      } else if (!vs_.immediate || (rebind & kVsRebind)) {
        // IM_LOAD of the object's variant, or nothing loaded (same shader as before). After an
        // immediate load the D3D reloads the VS only under the VS rebind bit (sub_826A9858).
        // Variant 1 is used without a pixel shader when the object has one (flag 0x20).
        uint32_t variant = (!ps && (Load32(base, vs + 872) & 0x20)) ? 1 : 0;
        uint32_t sub = vs + Load32(base, vs + 896 + variant * 8);
        vs_ = {vs, GpuAddress(Load32(base, vs + 32) + Load32(base, sub + 872)), Load32(base, sub + 876) >> 2};
        vs_.binding = XXH3_64bits(Host(base, vs + 416 * variant + 40), 24);
      }
      if (rebind & (kVsRebind | kPsRebind)) LoadLiterals(base, vs + 872, Load32(base, vs + 32));
    } else {
      vs_ = {};
    }
    if (ps) {
      uint32_t sub = ps + Load32(base, ps + 64);
      ps_ = {ps, GpuAddress(Load32(base, ps + 24) + Load32(base, sub + 40)), Load32(base, sub + 44) >> 2};
      if (rebind & kPsRebind) LoadLiterals(base, ps + 40, Load32(base, ps + 24));
    } else {
      ps_ = {};
    }
  }

  // Registers the draw functions write straight into the stream (not into the shadow).
  switch (fn_addr) {
    case kFnDrawIndexed:   // VGT_INDX_OFFSET = BaseVertexIndex
    case kFnDrawVertices:  // VGT_INDX_OFFSET = StartVertex
      Put(0x2102, r[2]);
      inline_vertices_ = inline_bytes_ = 0;
      break;
    case kFnDrawIndx2Split:  // Swap's internal draw
      Put(0x2102, 0);
      break;
    case kFnBeginVertices: {
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
      inline_bytes_ = bytes;
      break;
    }
    default:
      break;
  }
}

void GuestStateTracker::AfterImmediateVs(const uint8_t* base, const uint32_t r[8]) {
  // sub_826A9508(device, patch, vs object, ..., variant): reserves 5 + n dwords, writes
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
    if (++misses <= 4) REXLOG_WARN("PGR4 native state: no IM_LOAD_IMMEDIATE before push cursor {:08X} (vs {:08X})",
                                   start, vs);
    return;
  }
  vs_imm_pending_.resize(dwords);
  std::memcpy(vs_imm_pending_.data(), Host(base, start), size_t(dwords) * 4);
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
      uint32_t ib = Load32(base, device + dev::kIndexBuffer);
      s->index_buffer = ib;
      if (ib) {
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
    case HookFn::kBeginVertices:  // (dev, prim, vertex count, stride)
    case HookFn::kDrawVerticesUP:  // (dev, prim, vertex count, data, stride)
      s->primitive = a.r[1] & 0x3F;
      s->count = a.r[2];
      s->inline_vertices = inline_vertices_;
      s->inline_bytes = inline_bytes_;
      break;
    default:
      return false;
  }
  s->vs = vs_;
  s->ps = ps_;
  s->vertex_decl = Load32(base, device + dev::kVertexDecl);
  for (uint32_t i = 0; i < 4; ++i) s->color_surface[i] = Load32(base, device + dev::kRenderTargets + i * 4);
  s->depth_surface = Load32(base, device + dev::kDepthStencil);
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

bool ReadDrawState(const uint8_t* guest_base, uint32_t device_addr, const DrawArgs& args, DrawState* out) {
  return GuestState().ReadDrawState(guest_base, device_addr, args, out);
}

}  // namespace pgr4::native
