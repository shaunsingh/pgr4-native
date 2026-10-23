// pgr4-native PGR4 native renderer: guest D3D functions replaced in primary mode
// (--pgr4_native_replace_draws).
//
// With --pgr4_native_primary nothing draws the guest's PM4 stream (the Xenos plugin skips all
// draws), so the guest D3D's packet building for draws is wasted work. These replacements keep
// the guest device in the state the original leaves it in for everything that is read later
// (the register shadow, the dirty masks, the shader load that produces the patched vertex
// shader copy for the native draw) and skip the packets. Ring kicks, fences, Swap, Clear and
// Resolve stay on the original path.
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>

#include <cstdint>

REXCVAR_DECLARE(bool, pgr4_native_replace_draws);

namespace pgr4::native::guest_d3d {

// sub_826D0340 (DrawIndexedVertices): runs the replacement and returns true, or returns false
// when the original must run (flag off, not primary, no native renderer, predicated tiling).
bool DrawIndexed(PPCContext& ctx, uint8_t* base);

}  // namespace pgr4::native::guest_d3d
