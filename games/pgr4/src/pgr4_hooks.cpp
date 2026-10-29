// Game patches for Project Gotham Racing 4, Title Update #2: the mid-asm hooks the manifest
// (pgr4recompiled_manifest.toml) places in the generated code.

#include "native/frame_height.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/keybinds.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <string>

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
// viewport clamp reads back (sub_826C6100). The height is the run's (native/frame_height.h).
using pgr4::native::FrameHeight;

void Pgr4RenderHeightHook(PPCRegister& r) {
  if (r.u32 == 720) r.u64 = FrameHeight();
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
    GuestStore32(r5.u32 + 12, FrameHeight());
}

void Pgr4ViewportHook(PPCRegister& r4) {
  if (!r4.u32) return;
  if (GuestLoad32(r4.u32 + 8) == 1280 && GuestLoad32(r4.u32 + 12) == 720)
    GuestStore32(r4.u32 + 12, FrameHeight());
}

// The float viewport setter (sub_826C63F8, the one the scene's views use): r4 points at
// {X, Y, W, H, MinZ, MaxZ} floats.
void Pgr4ViewportFHook(PPCRegister& r4) {
  if (!r4.u32) return;
  constexpr uint32_t kW = 0x44A00000;  // 1280.0f
  constexpr uint32_t kH = 0x44340000;  // 720.0f
  if (GuestLoad32(r4.u32 + 8) == kW && GuestLoad32(r4.u32 + 12) == kH) {
    GuestStore32(r4.u32 + 12, std::bit_cast<uint32_t>(float(FrameHeight())));
  }
}

// The HUD's widgets that place themselves (the rev counter among them): sub_8245C2A8 and
// sub_8245C310 set one's x, y, width and height (r3 + 68 on, or + 84 on) from fractions of its
// viewport (r4; f1 to f4), whose {sizeX, sizeY, topLeftX, topLeftY, heightRatio} the race
// loading's SendViewportDetailVariables keeps at kHudViewports. The viewports are the game's,
// within the UI's rows 0 to 480 (ui/patches/Screens/GUIBank.fnl); a taller frame adds
// (480 * FrameHeight() / 720 - 480) / 2 rows of the UI's space above them and as many below.
// As HudHelper.fnl moves the HUD's items, a widget above its viewport's middle moves up by the
// rows added above the viewport, one below it down by those added below.
namespace {
constexpr uint32_t kHudViewports = 0x82AAD3EC;

float GuestLoadF(uint32_t a) { return std::bit_cast<float>(GuestLoad32(a)); }

void AnchorHudWidget(uint32_t y, uint32_t viewport, double fraction) {
  const float added = (480.0f * FrameHeight() / pgr4::native::kGameFrameHeight - 480.0f) / 2;
  const uint32_t v = kHudViewports + 20 * viewport;
  const float top = GuestLoadF(v + 12), bottom = top + GuestLoadF(v + 4);
  const float shift = fraction < 0.5 && top <= 0        ? -added
                      : fraction > 0.5 && bottom >= 480 ? added
                                                        : 0.0f;
  if (shift != 0.0f) GuestStore32(y, std::bit_cast<uint32_t>(GuestLoadF(y) + shift));
}
}  // namespace

void Pgr4HudWidgetHook(PPCRegister& r3, PPCRegister& r4, PPCRegister& f2) {
  AnchorHudWidget(r3.u32 + 72, r4.u32, f2.f64);
}

void Pgr4HudWidgetBHook(PPCRegister& r3, PPCRegister& r4, PPCRegister& f2) {
  AnchorHudWidget(r3.u32 + 88, r4.u32, f2.f64);
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
    GuestStore32(r4.u32 + 12, FrameHeight());
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

REXCVAR_DEFINE_STRING(pgr4_headlights, "race", "PGR4",
                      "The lights of every car and bike: race (as the race's lighting has them, "
                      "the cars' at night and the bikes' by day too), on or off. bind_headlights "
                      "(F5) switches them from what they show, until this setting changes");

// The scene's lighting (0x82A1D220) holds the race lighting's UseFogLights, UseCarNightLights and
// UseBikeNightLights at +52, +53 and +54 (sub_823CF460 copies them in, sub_823CDA80 blends two
// lightings). Each vehicle takes its lights from +53 or +54 (sub_82415138), and the lit and unlit
// lamp parts are chosen by +53 (sub_82421E68, sub_82448990).
namespace {
constexpr uint32_t kSceneLighting = 0x82A1D220;
// The lights forced: -1 none (the race's), 0 off, 1 on. pgr4_headlights sets it at start-up and
// from the Options menu, bind_headlights from then on. The hook reads this, not the cvar's
// string, which the menu's thread may be writing.
std::atomic<int> g_headlights{-1};

uint8_t* GuestByte(uint32_t a) {
  return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a);
}

int HeadlightsSetting() {
  const std::string& v = REXCVAR_GET(pgr4_headlights);
  return v == "on" ? 1 : v == "off" ? 0 : -1;
}

void ForceHeadlights(uint32_t lighting, int on) {
  *GuestByte(lighting + 53) = uint8_t(on);
  *GuestByte(lighting + 54) = uint8_t(on);
}
}  // namespace

// After sub_823CF460's stb r11,54(r31): r31 is the lighting just copied in.
void Pgr4SceneLightsHook(PPCRegister& r31) {
  if (const int on = g_headlights.load(std::memory_order_relaxed); on >= 0)
    ForceHeadlights(r31.u32, on);
}

// "race" leaves the scene's lights as they are until the next race lighting is copied in.
void Pgr4HeadlightsChanged() {
  const int on = HeadlightsSetting();
  g_headlights.store(on, std::memory_order_relaxed);
  if (on >= 0) ForceHeadlights(kSceneLighting, on);
}

void Pgr4Setup() {
  g_headlights.store(HeadlightsSetting(), std::memory_order_relaxed);
  rex::ui::RegisterBind("bind_headlights", "F5",
                        "PGR4: switch the lights of every car and bike on or off", [] {
                          const int on = *GuestByte(kSceneLighting + 53) ? 0 : 1;
                          g_headlights.store(on, std::memory_order_relaxed);
                          ForceHeadlights(kSceneLighting, on);
                          REXLOG_INFO("PGR4: headlights {}", on ? "on" : "off");
                        });
}
