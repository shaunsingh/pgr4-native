// pgr4-native PGR4 native renderer: guest D3D functions replaced in primary mode (guest_d3d.h).
#include "guest_d3d.h"

#include "d3d_state.h"
#include "guest_memory.h"
#include "renderer.h"

#include <cstring>

REXCVAR_DEFINE_BOOL(pgr4_native_replace_draws, true, "PGR4 native",
                    "With --pgr4_native_primary, replace the guest D3D's DrawIndexedVertices "
                    "(sub_826D0340): keep the device state, skip its PM4 packets");
REXCVAR_DECLARE(bool, pgr4_native_primary);

REX_EXTERN(sub_826DEDC8);

namespace pgr4::native::guest_d3d {

// The original (sub_826D0340(device, primitive, base vertex, start index, index count)):
//   1. flushes the dirty float constants (masks +0, +8, sub_826DE030), clears both masks;
//   2. mask +16 & kShaderLoadBits (d3d_state.h): sub_826DEDC8 (shader load; the immediate,
//      patched VS copy comes from its call to sub_826DEA78), which returns the new mask +16;
//      that mask & mask +40: sub_826DD8F8 (stream-only registers derived from it);
//   3. flushes the register blocks of masks +16, +24, +32 (sub_826DDC90, the fetch constants
//      sub_826DDED0, clip planes sub_826DDD50), clears the three masks;
//   4. writes VGT_INDX_OFFSET and DRAW_INDX (in pieces of at most 65535 indices), MakeSpace
//      (sub_826CE8C0) when the push buffer is full.
// Steps 1, 3 and 4 and sub_826DD8F8 write only the push buffer and its cursor (device +48); the
// shadow is unchanged and the state tracker (d3d_state.cpp) reads the shadow and which dirty
// bits the call cleared. So the replacement runs step 2's shader load and clears the masks.
// With predicated tiling on (device byte +10940, bit 0) the original writes the draw packet in
// its predicated form and records its address in the tile list at +13232 for the per-tile
// replay; without a packet there is nothing to record, so the replacement covers both forms.
bool DrawIndexed(PPCContext& ctx, uint8_t* base) {
  // Start-up flags: the renderer switch changes NativeRenderer::Live, not these.
  static const bool kReplace =
      REXCVAR_GET(pgr4_native_replace_draws) && REXCVAR_GET(pgr4_native_primary);
  const uint32_t device = ctx.r3.u32;
  if (!kReplace || !device || !NativeRenderer::Live()) return false;
  uint8_t* masks = HostPtr(base, device + dev::kDirtyMasks);
  // Same order as the original: the constant masks are cleared before the shader load (which
  // may dirty constants again), the register masks after it.
  std::memset(masks, 0, 16);
  const uint64_t shader = Load64(base, device + dev::kDirtyMasks + 16);
  if (shader & kShaderLoadBits) {
    // Volatile registers are the callee's, as in the original; r1 gets a frame below ours.
    const uint32_t sp = ctx.r1.u32;
    ctx.r1.u64 = sp - 256;
    ctx.r3.u64 = device;
    ctx.r4.u64 = shader;
    sub_826DEDC8(ctx, base);
    ctx.r1.u64 = sp;
  }
  std::memset(masks + 16, 0, 24);  // masks +16, +24, +32
  return true;
}

}  // namespace pgr4::native::guest_d3d
