// Force-included (clang-cl /FI) into the ReXGlue SDK shader translator sources that
// shader_xlate.cmake compiles into the app. Those sources define a few cvars
// (dxbc_switch, dxbc_source_map, draw_resolution_scaled_texture_offsets) that the
// rexgpu-xenos plugin DLL defines too; registering them twice makes the cvar registry log
// an error and ignore the plugin's copy. This keeps the app's copies as plain storage with
// the SDK defaults, unregistered, so the plugin keeps its command-line flags.
#pragma once

#include <rex/cvar.h>

#undef REXCVAR_DEFINE_BOOL
#define REXCVAR_DEFINE_BOOL(name, default_val, category, desc) \
  bool& FLAGS_##name##_storage_() {                            \
    static bool storage = (default_val);                       \
    return storage;                                            \
  }                                                            \
  static_assert(true)
