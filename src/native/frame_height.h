// pgr4-native PGR4 native renderer: the height of the game's frame in this run.
//
// The game renders a frame 1280 wide and FrameHeight() rows tall: --pgr4_render_height (800 for
// the Steam Deck's 16:10 panel, through pgr4_hooks.cpp's hooks) when the native renderer draws it
// from the start, and the game's own kGameFrameHeight (16:9) when the Xenos plugin does
// (--pgr4_native_classic, or without --pgr4_native_primary). The plugin is the original renderer,
// kept as it is, and draws the taller frame wrong: the scene in its top 300 rows, black and
// garbage below (td-462; at 720, td-463, it is whole). The game sizes its surfaces once, so the
// height is the one at the first call, before the game creates them, for the rest of the run;
// the renderer switch hands the game to the plugin only at kGameFrameHeight.
#pragma once

#include <cstdint>

namespace pgr4::native {

inline constexpr uint32_t kGameFrameHeight = 720;
uint32_t FrameHeight();

}  // namespace pgr4::native
