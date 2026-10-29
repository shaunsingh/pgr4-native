// The port's settings in the game's Options menu (games/pgr4/ui/: Screens/Pgr4/Settings.fnl,
// patches/Screens/Options/MainMenu.fnl): each a spinner of choices over the cvars that give it.
// A setting the renderer can change while it runs is applied at once (the cvars, then the
// renderer's request); one it takes at its start is kept until the next one. Both are saved to
// the settings file (LoadSettings), loaded after the configuration file at start-up.
#include "pgr4_settings.h"

#include "native/renderer.h"
#include "pgr4_lua.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(pgr4_game_debug_menus, false, "PGR4",
                    "The game's own debug menus: GothamSystem.IsFinalBuild answers false, and "
                    "the menus that test it show their debug entries");

REXCVAR_DECLARE(int32_t, pgr4_render_height);
REXCVAR_DECLARE(bool, pgr4_native_classic);

namespace pgr4::settings {
namespace {

using pgr4::native::NativeRenderer;

struct Choice {
  std::string label;
  std::string value;
};

// One cvar's value for a choice; `text` values are TOML strings in the settings file.
struct Assignment {
  std::string_view cvar;
  std::string value;
  bool text = false;
};
using Assignments = std::vector<Assignment>;

// The page shows an option only while the option `id` before it is shown with one of `values`.
struct Condition {
  std::string_view id;
  std::vector<std::string> values;
};

struct Option {
  std::string_view page, id, label;
  std::vector<Choice> choices;
  bool restart;                                 // taken at start-up only
  std::string (*read)();                        // the value running now
  Assignments (*assign)(const std::string& v);  // the cvars that give a value
  void (*applied)() = nullptr;                  // after a live option's cvars were set
  Condition shown_with = {};
};

std::string Flag(std::string_view cvar) { return rex::cvar::GetFlagByName(cvar); }
int FlagInt(std::string_view cvar) { return std::atoi(Flag(cvar).c_str()); }

// The choice whose value is nearest the number `n`: settings with a range (a size, a sample
// count) show the choice nearest what a configuration file set.
std::string Nearest(const Option& o, double n) {
  const auto d = [n](const Choice& c) { return std::abs(std::atof(c.value.c_str()) - n); };
  return std::ranges::min(o.choices, {}, d).value;
}

const std::vector<Option>& Options();
const Option& Find(std::string_view id) { return *std::ranges::find(Options(), id, &Option::id); }

// The scene's height at start-up, and the extent of `percent` of it.
uint32_t SceneHeight() { return uint32_t(REXCVAR_GET(pgr4_render_height)); }
std::string Percent(uint32_t extent, int percent) {
  return std::to_string(uint32_t(std::lround(extent * percent / 100.0)));
}
// The display's refresh rate, which the frames are paced on (frame_pacer.h).
int RefreshRate() { return std::max(1, FlagInt("video_mode_refresh_rate")); }

Assignments Bool(std::string_view cvar, const std::string& v) { return {{cvar, v}}; }

void RequestAntialiasing() {
  if (NativeRenderer* r = NativeRenderer::Get()) r->RequestAntialiasing();
}

std::vector<Option> MakeOptions() {
  const std::vector<Choice> on_off = {{"Off", "false"}, {"On", "true"}};
  // Options the native renderer alone has, shown while it is the one chosen.
  const Condition native = {"renderer", {"false"}};
  const Condition fsr = {"antialiasing", {"fsr"}};
  std::vector<Option> o;
  // Display.
  o.push_back({"display",
               "renderer",
               "Renderer",
               {{"Native", "false"}, {"Classic", "true"}},
               false,
               [] { return Flag("pgr4_native_classic"); },
               [](const std::string& v) { return Bool("pgr4_native_classic", v); },
               [] {
                 if (NativeRenderer* r = NativeRenderer::Get())
                   r->RequestRenderer(!REXCVAR_GET(pgr4_native_classic));
               }});
  o.push_back({"display",
               "aspect",
               "Aspect ratio",
               {{"16:10", "800"}, {"16:9", "720"}},
               true,
               [] { return std::to_string(SceneHeight()); },
               [](const std::string& v) { return Assignments{{"pgr4_render_height", v}}; }});
  o.push_back({"display", "hdr", "HDR", on_off, true, [] { return Flag("pgr4_native_hdr"); },
               [](const std::string& v) { return Bool("pgr4_native_hdr", v); }, nullptr, native});
  o.push_back({"display",
               "paper_white",
               "HDR brightness",
               {{"203 nits", "203"},
                {"300 nits", "300"},
                {"400 nits", "400"},
                {"500 nits", "500"},
                {"600 nits", "600"}},
               false,
               [] { return Nearest(Find("paper_white"), FlagInt("pgr4_native_hdr_paper_white")); },
               [](const std::string& v) { return Assignments{{"pgr4_native_hdr_paper_white", v}}; },
               nullptr,
               {"hdr", {"true"}}});
  o.push_back({"display",
               "antialiasing",
               "Antialiasing",
               {{"Off", "1"}, {"MSAA 2x", "2"}, {"MSAA 4x", "4"}, {"MSAA 8x", "8"}, {"FSR", "fsr"}},
               false,
               [] {
                 return Flag("pgr4_native_upscaler") == "true"
                            ? std::string("fsr")
                            : Nearest(Find("antialiasing"), FlagInt("pgr4_native_msaa"));
               },
               [](const std::string& v) {
                 if (v == "fsr") return Assignments{{"pgr4_native_upscaler", "true"}};
                 return Assignments{{"pgr4_native_upscaler", "false"}, {"pgr4_native_msaa", v}};
               },
               RequestAntialiasing,
               native});
  // FSR's render size, a fraction of the scene's on both axes: its quality modes.
  o.push_back({"display",
               "fsr_resolution",
               "FSR resolution",
               {{"Native", "100"}, {"Quality", "67"}, {"Balanced", "59"}, {"Performance", "50"}},
               false,
               [] {
                 return Nearest(Find("fsr_resolution"),
                                100.0 * FlagInt("pgr4_native_upscaler_height") / SceneHeight());
               },
               [](const std::string& v) {
                 const int p = std::atoi(v.c_str());
                 return Assignments{{"pgr4_native_upscaler_width", Percent(1280, p)},
                                    {"pgr4_native_upscaler_height", Percent(SceneHeight(), p)}};
               },
               RequestAntialiasing,
               fsr});
  // FSR's dynamic resolution: the render size each frame, down to the minimum, so the GPU keeps
  // up with the display (upscaler.h).
  o.push_back({"display", "dynamic_resolution", "Dynamic resolution", on_off, false,
               [] {
                 return std::string(FlagInt("pgr4_native_upscaler_target_fps") > 0 ? "true"
                                                                                   : "false");
               },
               [](const std::string& v) {
                 return Assignments{{"pgr4_native_upscaler_target_fps",
                                     v == "true" ? std::to_string(RefreshRate()) : "0"}};
               },
               RequestAntialiasing, fsr});
  o.push_back(
      {"display",
       "minimum_resolution",
       "Minimum resolution",
       {{"None", "25"}, {"33%", "33"}, {"50%", "50"}, {"59%", "59"}, {"67%", "67"}, {"77%", "77"}},
       false,
       [] {
         return Nearest(Find("minimum_resolution"),
                        100.0 * FlagInt("pgr4_native_upscaler_min_height") / SceneHeight());
       },
       [](const std::string& v) {
         return Assignments{
             {"pgr4_native_upscaler_min_height", Percent(SceneHeight(), std::atoi(v.c_str()))}};
       },
       RequestAntialiasing,
       {"dynamic_resolution", {"true"}}});
  // FidelityFX's frames between the game's, from FSR's depth and motion (frame_generation.h).
  o.push_back({"display", "frame_generation", "Frame generation", on_off, true,
               [] { return Flag("pgr4_native_frame_generation"); },
               [](const std::string& v) { return Bool("pgr4_native_frame_generation", v); },
               nullptr, fsr});
  o.push_back({"display",
               "anisotropy",
               "Texture filtering",
               {{"Game's", "1"}, {"2x", "2"}, {"4x", "4"}, {"8x", "8"}, {"16x", "16"}},
               true,
               [] { return Nearest(Find("anisotropy"), FlagInt("pgr4_native_anisotropy")); },
               [](const std::string& v) { return Assignments{{"pgr4_native_anisotropy", v}}; },
               nullptr,
               native});
  // Each frame shown for 1 to 3 refreshes (D3D12Device::set_present_interval).
  std::vector<Choice> frame_rates;
  for (int n = 1; n <= 3; ++n)
    frame_rates.push_back(
        {std::to_string(std::lround(double(RefreshRate()) / n)) + " fps", std::to_string(n)});
  o.push_back(
      {"display", "frame_rate", "Frame rate", std::move(frame_rates), false,
       [] { return Nearest(Find("frame_rate"), FlagInt("pgr4_native_present_interval")); },
       [](const std::string& v) { return Assignments{{"pgr4_native_present_interval", v}}; },
       [] {
         if (NativeRenderer* r = NativeRenderer::Get()) r->RequestFrameRate();
       },
       native});
  o.push_back({"display",
               "frame_latency",
               "Frame latency",
               {{"1", "1"}, {"2", "2"}, {"3", "3"}},
               true,
               [] { return Nearest(Find("frame_latency"), FlagInt("pgr4_native_frame_latency")); },
               [](const std::string& v) { return Assignments{{"pgr4_native_frame_latency", v}}; },
               nullptr,
               native});
  // Developer.
  o.push_back({"developer", "debug_keys", "Debug keys", on_off, false,
               [] { return Flag("pgr4_native_debug_keys"); },
               [](const std::string& v) { return Bool("pgr4_native_debug_keys", v); }});
  o.push_back({"developer", "game_debug_menus", "Game debug menus", on_off, false,
               [] { return Flag("pgr4_game_debug_menus"); },
               [](const std::string& v) { return Bool("pgr4_game_debug_menus", v); }});
  // FidelityFX's debug views and FSR 4's watermark, from the next frame (fidelityfx.h).
  o.push_back({"developer", "fsr_debug", "FSR debug view", on_off, false,
               [] { return Flag("pgr4_native_fsr_debug"); },
               [](const std::string& v) { return Bool("pgr4_native_fsr_debug", v); }});
  o.push_back({"developer", "occlusion", "Occlusion queries", on_off, true,
               [] { return Flag("pgr4_native_occlusion"); },
               [](const std::string& v) { return Bool("pgr4_native_occlusion", v); }});
  o.push_back({"developer", "async_pipelines", "Async pipelines", on_off, true,
               [] { return Flag("pgr4_native_async_pipelines"); },
               [](const std::string& v) { return Bool("pgr4_native_async_pipelines", v); }});
  o.push_back({"developer", "pipeline_cache", "Pipeline cache", on_off, true,
               [] { return Flag("pgr4_native_pipeline_cache"); },
               [](const std::string& v) { return Bool("pgr4_native_pipeline_cache", v); }});
  o.push_back({"developer", "gpu_timing", "GPU timing log", on_off, true,
               [] { return Flag("pgr4_native_gpu_timing"); },
               [](const std::string& v) { return Bool("pgr4_native_gpu_timing", v); }});
  o.push_back({"developer",
               "log_level",
               "Log level",
               {{"Errors", "error"}, {"Warnings", "warn"}, {"Info", "info"}, {"Debug", "debug"}},
               false,
               [] { return Flag("log_level"); },
               [](const std::string& v) { return Assignments{{"log_level", v, true}}; }});
  return o;
}

const std::vector<Option>& Options() {
  static const std::vector<Option> options = MakeOptions();
  return options;
}

// The guest UI thread runs the menus; the lock is for the settings file's path.
std::mutex g_mutex;
std::filesystem::path g_file;
// Start-up options chosen since the start, by id: their cvars keep the running values.
std::map<std::string_view, std::string> g_pending;

std::string Value(const Option& o) {
  if (const auto it = g_pending.find(o.id); it != g_pending.end()) return it->second;
  return o.read();
}

void Set(const Option& o, const std::string& v) {
  if (o.restart) {
    if (v == o.read())
      g_pending.erase(o.id);
    else
      g_pending[o.id] = v;
    return;
  }
  for (const Assignment& a : o.assign(v))
    if (!rex::cvar::SetFlagByName(a.cvar, a.value))
      REXLOG_ERROR("PGR4 settings: cannot set {} to {}", a.cvar, a.value);
  if (o.applied) o.applied();
}

// The file's values by cvar, as Save wrote them: `cvar = value` lines.
std::map<std::string, std::string, std::less<>> Saved() {
  std::map<std::string, std::string, std::less<>> saved;
  std::ifstream f(g_file, std::ios::binary);
  for (std::string line; std::getline(f, line);)
    if (const size_t eq = line.find(" = "); !line.starts_with('#') && eq != std::string::npos)
      saved.emplace(line.substr(0, eq), line.substr(eq + 3));
  return saved;
}

// Every option's cvars as they are chosen now, over the file's. A flag on the command line holds
// for its session only: unless the menu changed it, the file keeps the value it had.
void Save() {
  std::lock_guard lock(g_mutex);
  if (g_file.empty()) return;
  std::string toml =
      "# PGR4's settings, written by its Options menu (Display, Developer) and read after\n"
      "# pgr4_recompiled.toml at start-up. A flag on the command line overrides them.\n";
  const auto saved = Saved();
  for (const Option& o : Options()) {
    const bool chosen = g_pending.contains(o.id);
    for (const Assignment& a : o.assign(Value(o))) {
      const std::string name(a.cvar);
      if (!chosen && rex::cvar::GetFlagSource(a.cvar) == rex::cvar::Source::kCommandLine) {
        if (const auto it = saved.find(name); it != saved.end())
          toml += name + " = " + it->second + '\n';
        continue;
      }
      toml += name + " = " + (a.text ? '"' + a.value + '"' : a.value) + '\n';
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(g_file.parent_path(), ec);
  const std::filesystem::path temp = std::filesystem::path(g_file) += ".new";
  if (std::ofstream f(temp, std::ios::binary | std::ios::trunc); !(f << toml)) {
    REXLOG_ERROR("PGR4 settings: cannot write {}", temp.string());
    return;
  }
  std::filesystem::rename(temp, g_file, ec);
  if (ec) REXLOG_ERROR("PGR4 settings: cannot write {}: {}", g_file.string(), ec.message());
}

// Lua: Pgr4.Page(page) -> {{id, label, choices = {label...}, value = shown choice from 0,
// shown_with = {id, choices = {[choice from 0] = true...}} or nil}...}, the labels string IDs
// (Lua::Text). A start-up option's choices other than the running one are marked " *". A row with
// shown_with is shown while the row `id` is, with one of those choices.
void LuaPage(PPCContext& ctx, uint8_t* base) {
  pgr4::lua::Lua lua(ctx, base, ctx.r3.u32);
  const std::string page = lua.ToString(1);
  lua.CreateTable(0, 0);
  int row = 0;
  for (const Option& o : Options()) {
    if (o.page != page) continue;
    lua.CreateTable(0, 5);
    lua.PushString(o.id);
    lua.SetField(-2, "id");
    lua.PushNumber(lua.Text(o.label));
    lua.SetField(-2, "label");
    const std::string value = Value(o), running = o.read();
    lua.CreateTable(int(o.choices.size()), 0);
    int shown = 0;
    for (size_t i = 0; i < o.choices.size(); ++i) {
      const Choice& c = o.choices[i];
      if (c.value == value) shown = int(i);
      lua.PushNumber(lua.Text(o.restart && c.value != running ? c.label + " *" : c.label));
      lua.RawSetI(-2, int(i) + 1);
    }
    lua.SetField(-2, "choices");
    lua.PushNumber(shown);
    lua.SetField(-2, "value");
    if (const Condition& when = o.shown_with; !when.id.empty()) {
      const Option& by = Find(when.id);
      lua.CreateTable(0, 2);
      lua.PushString(by.id);
      lua.SetField(-2, "id");
      lua.CreateTable(0, int(when.values.size()));
      for (size_t i = 0; i < by.choices.size(); ++i) {
        if (std::ranges::find(when.values, by.choices[i].value) == when.values.end()) continue;
        lua.PushBoolean(true);
        lua.RawSetI(-2, int(i));
      }
      lua.SetField(-2, "choices");
      lua.SetField(-2, "shown_with");
    }
    lua.RawSetI(-2, ++row);
  }
  pgr4::lua::Results(ctx, 1);
}

// Lua: Pgr4.Apply(page, {[id] = choice from 0...}) -> whether a start-up option differs from
// what runs (a restart applies it).
void LuaApply(PPCContext& ctx, uint8_t* base) {
  pgr4::lua::Lua lua(ctx, base, ctx.r3.u32);
  const std::string page = lua.ToString(1);
  if (lua.TypeOf(2) == pgr4::lua::kTable) {
    for (const Option& o : Options()) {
      if (o.page != page) continue;
      lua.GetField(2, o.id);
      if (lua.TypeOf(-1) == pgr4::lua::kNumber) {
        const double n = lua.ToNumber(-1);
        if (n >= 0 && n < double(o.choices.size())) Set(o, o.choices[size_t(n)].value);
      }
      lua.Pop(1);
    }
    Save();
  }
  lua.PushBoolean(!g_pending.empty());
  pgr4::lua::Results(ctx, 1);
}

constexpr pgr4::lua::Function kFunctions[] = {
    {"Page", LuaPage},
    {"Apply", LuaApply},
};

}  // namespace

void LoadSettings(const std::filesystem::path& file) {
  {
    std::lock_guard lock(g_mutex);
    g_file = file;
  }
  std::error_code ec;
  if (std::filesystem::exists(file, ec)) rex::cvar::LoadConfig(file);
}

}  // namespace pgr4::settings

std::span<const pgr4::lua::Function> pgr4::lua::Pgr4LuaFunctions() {
  return pgr4::settings::kFunctions;
}

// GothamSystem.IsFinalBuild (sub_825CC570), a lua_CFunction: true in the shipped game.
REX_HOOK_RAW(sub_825CC570) {
  pgr4::lua::Lua lua(ctx, base, ctx.r3.u32);
  lua.PushBoolean(!REXCVAR_GET(pgr4_game_debug_menus));
  pgr4::lua::Results(ctx, 1);
}
