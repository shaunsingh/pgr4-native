// Game patches for Project Gotham Racing 4, Title Update #2: the mid-asm hooks the manifest
// (pgr4recompiled_manifest.toml) places in the generated code.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>

REXCVAR_DEFINE_BOOL(pgr4_60fps, false, "PGR4",
                    "Present every vblank instead of the game's interval (30 FPS) when the native "
                    "renderer does not pace the frames")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// The native renderer (src/pgr4_d3d_trace.cpp).
uint32_t Pgr4PresentationInterval(uint32_t guest_interval);
bool Pgr4FrameRateUnlocked();
float Pgr4MotionBlurStep(float step);

bool Pgr4Enable60Fps() { return REXCVAR_GET(pgr4_60fps); }

void Pgr4PresentationIntervalHook(PPCRegister& r10) { r10.u64 = Pgr4PresentationInterval(r10.u32); }

void Pgr4MotionBlurStepHook(PPCRegister& f0) { f0.f64 = double(Pgr4MotionBlurStep(float(f0.f64))); }

// Native 16:10 (development.md): the manifest hooks every instruction that loads the 720 of the
// game's hardcoded 1280x720 frame; each gets the new render height instead. The boundary hooks
// below cover the copies the loads cannot: structs the game fills itself and hands to the D3D
// (viewport, scissor and resolve rects) and the surface creators whose packed sizes the
// viewport clamp reads back (sub_826C6100).
REXCVAR_DECLARE(int32_t, pgr4_render_height);  // native/renderer.cpp: the FFX effects size from it

void Pgr4RenderHeightHook(PPCRegister& r) {
  if (r.u32 == 720) r.u64 = uint32_t(REXCVAR_GET(pgr4_render_height));
}

uint32_t GuestLoad32(uint32_t a) {
  return __builtin_bswap32(*rex::system::kernel_memory()->TranslateVirtual<const uint32_t*>(a));
}

void GuestStore32(uint32_t a, uint32_t v) {
  *rex::system::kernel_memory()->TranslateVirtual<uint32_t*>(a) = __builtin_bswap32(v);
}

// The D3D structs that carry a 1280x720 frame: Resolve's rect (r5 of sub_826CBEE0), the int
// SetViewport's (r4 of sub_826C6378) and SetScissorRect's (r4 of sub_826C59D8), both
// {x1, y1, x2, y2}. A 1280x720 one becomes the render height.
void Pgr4ResolveRectHook(PPCRegister& r5) {
  if (!r5.u32) return;
  if (GuestLoad32(r5.u32 + 8) == 1280 && GuestLoad32(r5.u32 + 12) == 720)
    GuestStore32(r5.u32 + 12, uint32_t(REXCVAR_GET(pgr4_render_height)));
}

void Pgr4ViewportHook(PPCRegister& r4) {
  if (!r4.u32) return;
  if (GuestLoad32(r4.u32 + 8) == 1280 && GuestLoad32(r4.u32 + 12) == 720)
    GuestStore32(r4.u32 + 12, uint32_t(REXCVAR_GET(pgr4_render_height)));
}

// The float viewport setter (sub_826C63F8, the one the scene's views use): r4 points at
// {X, Y, W, H, MinZ, MaxZ} floats.
void Pgr4ViewportFHook(PPCRegister& r4) {
  if (!r4.u32) return;
  constexpr uint32_t kW = 0x44A00000;  // 1280.0f
  constexpr uint32_t kH = 0x44340000;  // 720.0f
  if (GuestLoad32(r4.u32 + 8) == kW && GuestLoad32(r4.u32 + 12) == kH) {
    const float h = float(REXCVAR_GET(pgr4_render_height));
    GuestStore32(r4.u32 + 12, std::bit_cast<uint32_t>(h));
  }
}

// Vertex constant c45, the HUD's orthographic projection, in the device's shadow constants
// (device + 1920, c0 on) at the end of SetVertexShaderConstantF (sub_826C9908, r3 the device):
// the game maps its 640x480 HUD space into the central 960x720 of its 720-tall frame (4:3-safe,
// the pixel scale 1.5). With the frame pgr4_render_height tall, the y scale and offset still
// divide by 720 and every c45 draw (the dials, the HUD text, the menus) stretches by
// height/720. Rewrite the game's own value to the same mapping of the taller frame: the central
// 960x720 of it, centered, as the 960-wide area already is. A rewritten value no longer
// matches, so uploads of other registers leave it be.
void Pgr4HudOrthoHook(PPCRegister& r3) {
  const float h = float(REXCVAR_GET(pgr4_render_height));
  if (h == 720.0f) return;                      // the 16:10 patch is off
  const uint32_t c = r3.u32 + (120 + 45) * 16;  // c45 of the device's shadow
  if (std::bit_cast<float>(GuestLoad32(c)) != 3.0f / 1280.0f ||
      std::bit_cast<float>(GuestLoad32(c + 4)) != -3.0f / 720.0f ||
      std::bit_cast<float>(GuestLoad32(c + 8)) != -0.75f ||
      std::bit_cast<float>(GuestLoad32(c + 12)) != 1.0f)
    return;  // not the game's HUD ortho (another mode's): leave it
  GuestStore32(c + 4, std::bit_cast<uint32_t>(-3.0f / h));
  GuestStore32(c + 12, std::bit_cast<uint32_t>(720.0f / h));
}

REXCVAR_DEFINE_BOOL(pgr4_automatic_keep_revs, true, "PGR4",
                    "The automatic transmission of the player's car takes the lower gear as soon "
                    "as the engine would run under 90 % of the car's upshift point in it (the "
                    "game's own limit is 82 to 90 % by car), so it shifts down at a higher speed "
                    "and keeps the engine, and a turbo's boost, higher in its range");

// The automatic (sub_822DE250, development.md's "Gearbox") shifts down while the next lower gear,
// estimated from the wheel speeds, stays under car data + 440 (0.82 to 0.9 by car) times the
// upshift point at car data + 408 (rpm), in rad/s: f31 after 0x822DE400. Raising the factor to
// 0.9 keeps a tenth of the speed between the two shifts, so a downshift never re-upshifts at
// once. Only for the player's car (car + 20 is 0; the opponents are 1, and their automatic stays
// the one the game balanced them with).
void Pgr4AutomaticDownshiftHook(PPCRegister& r10, PPCRegister& f31) {
  constexpr double kKeepRevs = 0.9;
  constexpr double kRpmToRadPerS = 3.14159265358979323846 / 30.0;
  const uint32_t car = r10.u32;
  if (!REXCVAR_GET(pgr4_automatic_keep_revs) || GuestLoad32(car + 20) != 0) return;
  const double upshift = std::bit_cast<float>(GuestLoad32(GuestLoad32(car + 804) + 408));
  f31.f64 = std::max(f31.f64, double(float(kKeepRevs * upshift * kRpmToRadPerS)));
}

// SetScissorRect (fn_826C59D8): r4 points at {x1, y1, x2, y2}.
void Pgr4ScissorRectHook(PPCRegister& r4) {
  if (!r4.u32) return;
  if (GuestLoad32(r4.u32 + 8) == 1280 && GuestLoad32(r4.u32 + 12) == 720)
    GuestStore32(r4.u32 + 12, uint32_t(REXCVAR_GET(pgr4_render_height)));
}

namespace {

// The walking camera's call to its collision callback (the bctrl at 0x827DD51C).
constexpr uint32_t kGarageWalkReturn = 0x827DD520;
// The callback makes no calls between its entry and the step count but the register save helper,
// so the caller recorded at entry is still this frame's.
thread_local bool t_garage_walk_call = false;

constexpr uint32_t kCarPreviewCellLimit = 1024;
constexpr uint32_t kCarPreviewChainLimit = 4096;
thread_local uint32_t t_car_preview_cells = 0;
thread_local uint32_t t_car_preview_chain = 0;

}  // namespace

void Pgr4GarageCollisionCallerHook(PPCRegister& r12) {
  t_garage_walk_call = r12.u32 == kGarageWalkReturn;
}

// The step count is trunc(distance * 20), and a count of zero skips the move. The walking caller
// has already rejected effectively zero moves, so at unlocked frame rates the sub-5 cm moves of a
// short frame get one step instead of none; other collision users and 30 FPS keep the game's count.
void Pgr4GarageCollisionStepsHook(PPCRegister& r29) {
  if (r29.s32 == 0 && t_garage_walk_call && Pgr4FrameRateUnlocked()) r29.s64 = 1;
}

void Pgr4CarPreviewEnterHook() { t_car_preview_cells = 0; }

bool Pgr4CarPreviewCellHook(PPCRegister& r30, PPCRegister& r31) {
  if (++t_car_preview_cells <= kCarPreviewCellLimit) [[likely]]
    return false;
  static std::atomic_bool logged = false;
  if (!logged.exchange(true))
    REXLOG_WARN(
        "PGR4 car preview: visibility search passed {} cells (row {}, column {}); ending it",
        kCarPreviewCellLimit, r30.u32, r31.u32);
  return true;
}

void Pgr4CarPreviewChainStartHook() { t_car_preview_chain = 0; }

bool Pgr4CarPreviewChainHook(PPCRegister& r3, PPCRegister& r10) {
  if (++t_car_preview_chain <= kCarPreviewChainLimit) [[likely]]
    return false;
  static std::atomic_bool logged = false;
  if (!logged.exchange(true))
    REXLOG_WARN(
        "PGR4 car preview: visibility list passed {} nodes (node {:#x}, cell {:#x}); "
        "skipping the rest",
        kCarPreviewChainLimit, r3.u32, r10.u32);
  return true;
}
