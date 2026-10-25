// pgr4-native PGR4 native renderer: Xenos register state per draw, read from the guest D3D
// device instead of the push buffer.
//
// The XDK D3D device keeps a shadow of the GPU registers (offsets below, from the flush calls
// in DrawIndexedVertices sub_826D0340) and six 64-bit dirty masks at device+0..+40. The draw
// functions (and Clear/Resolve for the register blocks) flush the dirty parts of the shadow
// into the push buffer and clear the masks. What the GPU sees therefore differs from the
// shadow in three ways (seen in --pgr4_d3d_tap traces):
//   1. float constants written to the shadow without their dirty bit are never sent; the
//      bound shaders' literal constants are loaded straight from the shader objects
//      (LOAD_ALU_CONSTANT in sub_826DEBD8) when a shader is rebound, never into the shadow;
//   2. the draw functions write some registers straight into the stream: VGT_INDX_OFFSET
//      (base vertex / start vertex), vertex fetch slots 93-95 (BeginVertices' inline vertex
//      buffer); Resolve writes RB_COPY_* and queries write RB_SAMPLE_COUNT_ADDR (not draw
//      state, not tracked here);
//   3. the user clip planes PA_CL_UCP_0..5 (0x2388-0x239F) are not in the 0x2380 block (that
//      block is 8 registers, 0x2380-0x2387); they are flushed from device+10272, 16 bytes per
//      plane, by sub_826DDD50 under dirty bits 49..54 of the mask at +32.
//
// GuestState keeps the GPU's view of the register file: the hook dispatch captures the dirty
// masks at the entry of every flushing D3D call (ReadDirtyMasks) and reports the call's exit
// (AfterCall); the tracker then copies what the call flushed. ReadDrawState is cheap after
// that (no copy of the register file; DrawState points at the tracker's array).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "hook_args.h"

namespace pgr4::native {

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
constexpr uint32_t kVertexStride = 11824;   // u8: BeginVertices' vertex stride in dwords
constexpr uint32_t kIndexBuffer = 12428;    // guest D3D index buffer object
constexpr uint32_t kPixelShader = 12684;    // guest D3D pixel shader object
constexpr uint32_t kVertexShader = 12688;   // guest D3D vertex shader object
}  // namespace dev

// Captured by the hook dispatch at the entry of a flushing call, before the guest code runs.
struct CallEntry {
  uint64_t dirty[6];             // device+0..+40, host order
  uint8_t vertex_stride_dwords;  // dev::kVertexStride (BeginVertices rebinds the VS on a change)
};
CallEntry ReadDirtyMasks(const uint8_t* base, uint32_t device);

// At the entry of DrawIndexedVertices: starts fetching the bound index buffer object, which
// ReadDrawState reads after the call (and the replaced draw, guest_d3d.h, does not read at all).
void PrefetchIndexBuffer(const uint8_t* base, uint32_t device);

// Mask +16 bits 17..20 make a draw call the shader load sub_826DEDC8 (bit 19 = vertex shader,
// 20 = pixel shader). Besides the stream, it writes registers of the 0x2000, 0x2180 and 0x2200
// blocks into the shadow (0x2001, 0x2180-0x2182 and, through sub_826DD6F8, 0x2203, 0x2208)
// and the draw flushes them in the same call, so their dirty bits are not set at its entry.
constexpr uint64_t kShaderLoadBits = 0x1E0000ull;

// Sets the dirty bits of every state block the draws flush (float constants, register blocks,
// fetch and bool/loop constants, clip planes) and the shader rebind bits, so the next draw
// writes the whole shadow and both shaders into the stream.
void MarkAllDirty(uint8_t* base, uint32_t device);

// The D3D functions that flush the shadow (directly or through the internal draw builders):
// every HookFn (hook_args.h). The dispatch calls ReadDirtyMasks before and
// GuestState().AfterCall after each of them, at every nesting depth, whenever --pgr4_native or
// the trace tap is on.
constexpr bool IsStateFlushingFn(uint32_t addr) {
  switch (HookFn(addr)) {
    case HookFn::kBeginVertices:
    case HookFn::kDrawVerticesUP:
    case HookFn::kDrawVertices:
    case HookFn::kDrawIndexedVertices:
    case HookFn::kDrawIndx2Inline:
    case HookFn::kDrawIndx2Split:
    case HookFn::kClearF:
    case HookFn::kClear:
    case HookFn::kResolve:
    case HookFn::kSwap:
    case HookFn::kRunCommandBuffer:
      return true;
  }
  return false;
}
// Registers the D3D writes into the stream outside any draw state (Resolve's
// RB_COPY_DEST_BASE, the queries' RB_SAMPLE_COUNT_ADDR); CompareWithTap skips them.
bool IsTapOnlyRegister(uint32_t reg);

// A shader as the GPU loads it (IM_LOAD): physical ucode address and size in dwords.
//
// When the bound vertex declaration does not match a vertex shader's built-in binding, the
// D3D (sub_826DEDC8 -> sub_826DEA78) copies the shader variant into the push buffer, patches
// its vertex fetches for the declaration there and loads it with IM_LOAD_IMMEDIATE. Then
// `immediate` points at a copy of the patched ucode (big-endian dwords, valid until the next
// immediate VS load reaches a draw), `ucode` is its push-buffer address and `immediate_seq`
// counts these loads (a cache key: push-buffer addresses get reused).
//
// A built-in variant is also patched where it is: once the GPU has finished the frames that
// loaded it, binding it with another declaration (sub_826DEC88) rewrites its vertex fetches in
// place (sub_826DE190) and records the declaration in the variant's binding record (vs + 416 *
// variant: declaration id at +40, stream layout at +48 and +56). `binding` hashes that record;
// the ucode at `ucode` changes with it while its address, size and first and last dwords stay.
struct ShaderRef {
  uint32_t object = 0;  // guest D3D shader object (0 = none bound)
  uint32_t ucode = 0;   // physical address of the ucode, big-endian dwords
  uint32_t dwords = 0;
  const uint32_t* immediate = nullptr;
  uint32_t immediate_seq = 0;
  uint64_t binding = 0;
};

struct DrawState {
  // The register file as the GPU sees it at this draw, host byte order, kGuestRegisterCount
  // entries (0x4000 VS constants, 0x4400 PS constants, 0x4800 fetch constants, 0x4900
  // bool/loop, 0x2000.. render/raster state). Valid until the next hooked D3D call.
  const uint32_t* regs = nullptr;
  // Fetch constant slot 0..31 (6 dwords; texture fetch) or vertex fetch 0..95 (2 dwords).
  const uint32_t* fetch(uint32_t slot) const { return regs + 0x4800 + slot * 6; }

  HookFn fn{};
  uint32_t device = 0;
  uint32_t primitive = 0;  // xenos::PrimitiveType (VGT_DRAW_INITIATOR bits 0-5)
  uint32_t count = 0;      // indices (indexed) or vertices
  bool indexed = false;
  // Index buffer (indexed draws): physical address of the first index of this draw.
  uint32_t index_address = 0;
  bool index_32bit = false;
  uint32_t index_endian = 0;  // xenos::Endian of the indices
  uint32_t base_vertex = 0;   // = regs[0x2102] VGT_INDX_OFFSET
  // BeginVertices / DrawVerticesUP: inline vertices in the push buffer (vfetch 95).
  uint32_t inline_vertices = 0;  // physical, 0 if none

  ShaderRef vs, ps;
};

// One register value, for the render thread's copy of the register file (renderer.h).
struct RegWrite {
  uint32_t reg, value;
};

// Register writes in order, appended through a cursor: Reserve(n) makes room for n more and
// returns where they go, Commit(end) keeps the ones written before `end`. The tracker's copy
// loop then keeps the cursor in a register (a vector's end would be reloaded after every store,
// as clang-cl, like MSVC, does not assume strict aliasing).
class RegLog {
 public:
  size_t size() const { return size_; }
  const RegWrite& operator[](size_t i) const { return data_[i]; }
  void clear() { size_ = 0; }
  RegWrite* Reserve(size_t n) {
    if (capacity_ - size_ < n) [[unlikely]]
      Grow(size_ + n);
    return data_.get() + size_;
  }
  void Commit(const RegWrite* end) { size_ = size_t(end - data_.get()); }
  void Append(RegWrite w) {
    *Reserve(1) = w;
    ++size_;
  }

 private:
  void Grow(size_t capacity);
  std::unique_ptr<RegWrite[]> data_;
  size_t size_ = 0, capacity_ = 0;
};

class GuestStateTracker {
 public:
  // After a flushing call returned. `r` = r3..r10 at entry (r[0] = device), `ret` = r3 after.
  void AfterCall(const uint8_t* base, uint32_t fn_addr, const CallEntry& entry, const uint32_t r[8],
                 uint64_t ret);
  // After sub_826DEA78 (immediate VS load) returned; `r` = r3..r10 at its entry (r3 device,
  // r5 vertex shader object, r10 variant). The copy is taken over by the draw that made it.
  void AfterImmediateVs(const uint8_t* base, const uint32_t r[8]);
  // State of the draw `args` describes; call after AfterCall for that draw (OnDraw).
  bool ReadDrawState(const uint8_t* base, const HookArgs& args, DrawState* out);
  const uint32_t* regs() const { return regs_; }
  uint32_t device() const { return device_; }
  // Shaders bound at the last draw (the IM_LOAD address the D3D uses for them).
  const ShaderRef& vs() const { return vs_; }
  const ShaderRef& ps() const { return ps_; }
  // From now on appends every register write that changes a value to `*sink`, in order (null:
  // none). The first sink also gets every non-zero register, as the copy it feeds starts zeroed
  // like regs_. The render thread replays these into its own register file (renderer.cpp).
  void RecordInto(RegLog* sink);
  // Forgets the device after calls the tracker did not see: the next flushing call attaches
  // again and copies the whole shadow (recording what changed).
  void Resync() {
    device_ = 0;
    vs_imm_pending_valid_ = false;
  }

  // For the trace tap (--pgr4_d3d_tap): compares the registers the tap has seen written in the
  // push buffer (`tap_written[r] != 0`) with this view at the current draw. Returns the number
  // of differing registers and adds one per register to `per_reg[r]` (may be null). A register
  // whose last write in the stream was predicated (`tap_written[r] == kTapWrittenPredicated`: a
  // per-tile copy under predicated tiling, e.g. the window scissor) has no single GPU value;
  // differences there are only counted in `*per_tile` (may be null).
  static constexpr uint8_t kTapWrittenPredicated = 2;
  uint32_t CompareWithTap(const uint32_t* tap_regs, const uint8_t* tap_written, uint64_t* per_reg,
                          uint64_t* per_tile) const;

 private:
  void Attach(const uint8_t* base, uint32_t device);
  // Every write to regs_ goes through Put/Copy, which record the changed registers. raw_ keeps
  // each register as the guest stores it (big-endian), so Copy compares guest memory with it
  // four registers at a time without swapping.
  void Record(uint32_t reg, uint32_t value) {
    if (sink_) sink_->Append({reg, value});
  }
  void Put(uint32_t reg, uint32_t value) {
    if (regs_[reg] == value) return;
    regs_[reg] = value;
    raw_[reg] = __builtin_bswap32(value);
    Record(reg, value);
  }
  // `count` registers from guest memory at `src` (LoadLiterals).
  void Copy(uint32_t reg, const uint8_t* base, uint32_t src, uint32_t count);
  // The float constants and block registers whose bit is set in `flushed` (masks +0..+40, host
  // order).
  void CopyFlushed(const uint8_t* base, uint32_t device, const uint64_t flushed[6]);
  // --pgr4_native_track_verify: logs block registers whose shadow differs from regs_.
  void Verify(const uint8_t* base, uint32_t device, uint32_t fn_addr);
  void LoadLiterals(const uint8_t* base, uint32_t table, uint32_t data);

  uint32_t device_ = 0;
  ShaderRef vs_, ps_;
  uint32_t inline_vertices_ = 0;
  // Vertex fetch 31 holds BeginVertices' inline buffer, not the shadow's value.
  bool inline_fetch_ = false;
  // Immediate VS loads: the one made during the current flushing call (pending) and the one
  // the GPU holds (vs_ points into it while vs_.immediate is set).
  std::vector<uint32_t> vs_imm_pending_, vs_imm_;
  ShaderRef vs_imm_pending_ref_;
  bool vs_imm_pending_valid_ = false;
  uint32_t vs_imm_seq_ = 0;
  bool verify_ = false;  // --pgr4_native_track_verify, read at Attach
  uint32_t regs_[kGuestRegisterCount] = {};
  uint32_t raw_[kGuestRegisterCount] = {};
  RegLog* sink_ = nullptr;  // RecordInto
  RegLog discard_;          // Copy's writes while there is no sink
  bool synced_ = false;     // a sink got the whole register file
};

GuestStateTracker& GuestState();

}  // namespace pgr4::native
