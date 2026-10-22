// pgr4-native PGR4 native renderer (design A, milestone 2): Xenos register state per draw, read
// from the guest D3D device instead of the push buffer.
//
// The XDK D3D device keeps a shadow of the GPU registers (offsets below, from the flush calls
// in DrawIndexedVertices sub_8269ADD8) and six 64-bit dirty masks at device+0..+40. The draw
// functions (and Clear/Resolve for the register blocks) flush the dirty parts of the shadow
// into the push buffer and clear the masks. What the GPU sees therefore differs from the
// shadow in three ways (the development notes, "Renderer decision", M1 shadow mismatches):
//   1. float constants written to the shadow without their dirty bit are never sent; the
//      bound shaders' literal constants are loaded straight from the shader objects
//      (LOAD_ALU_CONSTANT in sub_826A9668) when a shader is rebound, never into the shadow;
//   2. the draw functions write some registers straight into the stream: VGT_INDX_OFFSET
//      (base vertex / start vertex), vertex fetch slots 93-95 (BeginVertices' inline vertex
//      buffer); Resolve writes RB_COPY_* and queries write RB_SAMPLE_COUNT_ADDR (not draw
//      state, not tracked here);
//   3. the user clip planes PA_CL_UCP_0..5 (0x2388-0x239F) are not in the 0x2380 block (that
//      block is 8 registers, 0x2380-0x2387); they are flushed from device+10272, 16 bytes per
//      plane, by sub_826A87E0 under dirty bits 49..54 of the mask at +32.
//
// GuestState keeps the GPU's view of the register file: the hook dispatch captures the dirty
// masks at the entry of every flushing D3D call (ReadDirtyMasks) and reports the call's exit
// (AfterCall); the tracker then copies what the call flushed. ReadDrawState is cheap after
// that (no copy of the register file; DrawState points at the tracker's array).
//
// Owner: StateReader (M2 owner #2). Ask before changing this header.
#pragma once

#include <cstdint>
#include <vector>

#include "renderer.h"

namespace pgr4::native {

using DrawArgs = HookArgs;

constexpr uint32_t kGuestRegisterCount = 0x5000;

// Device layout (byte offsets from the guest D3D device pointer).
namespace dev {
constexpr uint32_t kDirtyMasks = 0;         // 6 x u64, big-endian
constexpr uint32_t kPush = 48;              // last written push-buffer dword
constexpr uint32_t kFetchConstants = 1152;  // 0x4800, 32 x 6 dwords
constexpr uint32_t kVsConstants = 1920;     // 0x4000, 256 x vec4
constexpr uint32_t kPsConstants = 6016;     // 0x4400, 256 x vec4
constexpr uint32_t kBoolLoop = 10112;       // 0x4900, 40 dwords
constexpr uint32_t kClipPlanes = 10272;     // 0x2388, 6 x 4 dwords
constexpr uint32_t kScissorTL = 10436;      // window scissor x0 | y0 << 16 (15-bit signed)
constexpr uint32_t kScissorBR = 10440;      // x1 | y1 << 16
constexpr uint32_t kIndexBuffer = 12428;    // guest D3D index buffer object
constexpr uint32_t kRenderTargets = 12432;  // 4 x guest D3D surface object
constexpr uint32_t kDepthStencil = 12448;   // guest D3D surface object
constexpr uint32_t kPixelShader = 12684;    // guest D3D pixel shader object
constexpr uint32_t kVertexShader = 12688;   // guest D3D vertex shader object
constexpr uint32_t kVertexDecl = 11812;     // guest D3D vertex declaration object
}  // namespace dev

// Captured by the hook dispatch at the entry of a flushing call, before the guest code runs.
struct CallEntry {
  uint64_t dirty[6];             // device+0..+40, host order
  uint8_t vertex_stride_dwords;  // device+11824 (BeginVertices rebinds the VS when it changes)
};
CallEntry ReadDirtyMasks(const uint8_t* base, uint32_t device);

// The D3D functions that flush the shadow (directly or through the internal draw builders):
// the draws, DrawIndx2Inline/Split, Clear/ClearF, Resolve, Swap and fn_826A10A8. The dispatch
// calls ReadDirtyMasks before and GuestState().AfterCall after each of them, at every
// nesting depth, whenever --pgr4_native or the trace tap is on.
bool IsStateFlushingFn(uint32_t addr);

// Registers the D3D writes into the stream outside any draw state (Resolve's
// RB_COPY_DEST_BASE, the queries' RB_SAMPLE_COUNT_ADDR); CompareWithTap skips them.
bool IsTapOnlyRegister(uint32_t reg);

// A shader as the GPU loads it (IM_LOAD): physical ucode address and size in dwords.
//
// When the bound vertex declaration does not match a vertex shader's built-in binding, the
// D3D (sub_826A9858 -> sub_826A9508) copies the shader variant into the push buffer, patches
// its vertex fetches for the declaration there and loads it with IM_LOAD_IMMEDIATE. Then
// `immediate` points at a copy of the patched ucode (big-endian dwords, valid until the next
// immediate VS load reaches a draw), `ucode` is its push-buffer address and `immediate_seq`
// counts these loads (a cache key: push-buffer addresses get reused).
struct ShaderRef {
  uint32_t object = 0;  // guest D3D shader object (0 = none bound)
  uint32_t ucode = 0;   // physical address of the ucode, big-endian dwords
  uint32_t dwords = 0;
  const uint32_t* immediate = nullptr;
  uint32_t immediate_seq = 0;
};

struct DrawState {
  // The register file as the GPU sees it at this draw, host byte order, kGuestRegisterCount
  // entries (0x4000 VS constants, 0x4400 PS constants, 0x4800 fetch constants, 0x4900
  // bool/loop, 0x2000.. render/raster state). Valid until the next hooked D3D call.
  const uint32_t* regs = nullptr;
  uint32_t reg(uint32_t r) const { return regs[r]; }
  // Fetch constant slot 0..31 (6 dwords; texture fetch) or vertex fetch 0..95 (2 dwords).
  const uint32_t* fetch(uint32_t slot) const { return regs + 0x4800 + slot * 6; }
  const uint32_t* vfetch(uint32_t index) const { return regs + 0x4800 + index * 2; }

  HookFn fn{};
  uint32_t device = 0;
  uint32_t primitive = 0;    // xenos::PrimitiveType (VGT_DRAW_INITIATOR bits 0-5)
  uint32_t count = 0;        // indices (indexed) or vertices
  bool indexed = false;
  // Index buffer (indexed draws): physical address of the first index of this draw.
  uint32_t index_buffer = 0;     // guest D3D index buffer object
  uint32_t index_address = 0;    // physical
  bool index_32bit = false;
  uint32_t index_endian = 0;     // xenos::Endian of the indices
  uint32_t base_vertex = 0;      // = regs[0x2102] VGT_INDX_OFFSET
  // BeginVertices / DrawVerticesUP: inline vertices in the push buffer (vfetch 95).
  uint32_t inline_vertices = 0;  // physical, 0 if none
  uint32_t inline_bytes = 0;

  ShaderRef vs, ps;
  uint32_t vertex_decl = 0;
  uint32_t color_surface[4] = {};  // guest D3D surface objects bound as render targets
  uint32_t depth_surface = 0;
};

// One register value, for the render thread's copy of the register file (renderer.h).
struct RegWrite {
  uint32_t reg, value;
};

class GuestStateTracker {
 public:
  // After a flushing call returned. `r` = r3..r10 at entry (r[0] = device), `ret` = r3 after.
  void AfterCall(const uint8_t* base, uint32_t fn_addr, const CallEntry& entry, const uint32_t r[8],
                 uint64_t ret);
  // After sub_826A9508 (immediate VS load) returned; `r` = r3..r10 at its entry (r3 device,
  // r5 vertex shader object, r10 variant). The copy is taken over by the draw that made it.
  void AfterImmediateVs(const uint8_t* base, const uint32_t r[8]);
  // State of the draw `args` describes; call after AfterCall for that draw (OnDraw).
  bool ReadDrawState(const uint8_t* base, uint32_t device, const DrawArgs& args, DrawState* out);
  const uint32_t* regs() const { return regs_; }
  uint32_t device() const { return device_; }
  // Shaders bound at the last draw (the IM_LOAD address the D3D uses for them).
  const ShaderRef& vs() const { return vs_; }
  const ShaderRef& ps() const { return ps_; }
  // Appends the registers whose value changed since the last call (since start-up the first
  // time; the copy it feeds starts zeroed like regs_) and forgets them. The render thread
  // replays these into its own register file (renderer.cpp).
  void TakeChanges(std::vector<RegWrite>* out);

  // Oracle (M1 tap): compares the registers the tap has seen written (`tap_written[r] != 0`)
  // with this view at the current draw. Returns the number of differing registers and adds
  // one per register to `per_reg[r]` (may be null). A register whose last write in the
  // stream was predicated (`tap_written[r] == kTapWrittenPredicated`: a per-tile copy under
  // predicated tiling, e.g. the window scissor) has no single GPU value; differences there are
  // only counted in `*per_tile` (may be null). Called from the trace tap.
  static constexpr uint8_t kTapWrittenPredicated = 2;
  uint32_t CompareWithTap(const uint32_t* tap_regs, const uint8_t* tap_written, uint64_t* per_reg,
                          uint64_t* per_tile) const;

 private:
  void Attach(const uint8_t* base, uint32_t device);
  // Every write to regs_ goes through Put/Copy, which note the changed registers. raw_ keeps
  // each register as the guest stores it (big-endian), so Copy compares guest memory with it
  // four registers at a time without swapping.
  void Mark(uint32_t reg) {
    changed_[reg >> 6] |= 1ull << (reg & 63);
    changed_words_[reg >> 12] |= 1ull << ((reg >> 6) & 63);
  }
  void Put(uint32_t reg, uint32_t value) {
    if (regs_[reg] == value) return;
    regs_[reg] = value;
    raw_[reg] = __builtin_bswap32(value);
    Mark(reg);
  }
  void Copy(uint32_t reg, const uint8_t* base, uint32_t src, uint32_t count);
  // The block registers whose bit is set in `dirty` (masks +0..+40, host order).
  void CopyDirty(const uint8_t* base, uint32_t device, const uint64_t dirty[6]);
  // --pgr4_native_track_verify: logs block registers whose shadow differs from regs_.
  void Verify(const uint8_t* base, uint32_t device, uint32_t fn_addr);
  void LoadLiterals(const uint8_t* base, uint32_t table, uint32_t data);

  uint32_t device_ = 0;
  ShaderRef vs_, ps_;
  uint32_t inline_vertices_ = 0, inline_bytes_ = 0;
  // Vertex fetch 31 holds BeginVertices' inline buffer, not the shadow's value.
  bool inline_fetch_ = false;
  // Immediate VS loads: the one made during the current flushing call (pending) and the one
  // the GPU holds (vs_ points into it while vs_.immediate is set).
  std::vector<uint32_t> vs_imm_pending_, vs_imm_;
  ShaderRef vs_imm_pending_ref_;
  bool vs_imm_pending_valid_ = false;
  uint32_t vs_imm_seq_ = 0;
  uint32_t regs_[kGuestRegisterCount] = {};
  uint32_t raw_[kGuestRegisterCount] = {};
  static constexpr uint32_t kChangedWords = kGuestRegisterCount / 64;
  static constexpr uint32_t kChangedWordGroups = (kChangedWords + 63) / 64;
  uint64_t changed_[kChangedWords] = {};             // bit per register
  uint64_t changed_words_[kChangedWordGroups] = {};  // bit per non-zero changed_ word
};

GuestStateTracker& GuestState();

// Contract entry point: GuestState().ReadDrawState(...).
bool ReadDrawState(const uint8_t* guest_base, uint32_t device_addr, const DrawArgs& args,
                   DrawState* out);

}  // namespace pgr4::native
