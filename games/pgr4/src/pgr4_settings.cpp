// The port's settings in the game's Options menu (games/pgr4/ui/: Screens/Pgr4/Settings.fnl,
// patches/Screens/Options/MainMenu.fnl): each a spinner of choices over the cvars that give it.
// A setting the renderer can change while it runs is applied at once (the cvars, then the
// renderer's request); one it takes at its start is kept until the next one. Both are saved into
// the configuration file, pgr4_recompiled.toml (LoadSettings), the one a player edits.
#include "pgr4_settings.h"

#include "native/frame_height.h"
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
#include <optional>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(pgr4_game_debug_menus, false, "PGR4",
                    "The game's own debug menus: GothamSystem.IsFinalBuild answers false, and "
                    "the menus that test it show their debug entries");

REXCVAR_DECLARE(bool, pgr4_native_classic);

// pgr4_hooks.cpp: the scene's lights after pgr4_headlights changed.
void Pgr4HeadlightsChanged();

namespace pgr4::settings {
namespace {

using pgr4::native::FrameHeight;
using pgr4::native::kGameFrameHeight;
using pgr4::native::NativeRenderer;

struct Choice {
  std::string label;
  std::string value;
};

// One cvar's value for a choice; `text` values are TOML strings in the configuration file.
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
  bool restart;                                                 // taken at start-up only
  std::string (*read)();                                        // the value running now
  Assignments (*assign)(const std::string& v, uint32_t scene);  // its cvars, for a scene height
  void (*applied)() = nullptr;  // after a live option's cvars were set
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

// Start-up options chosen since the start, by id: their cvars keep the running values.
std::map<std::string_view, std::string> g_pending;

// The scene's height from the next start: the aspect ratio chosen since this one, else this
// run's. FSR's sizes are a fraction of the scene's: the cvars hold this run's, and the saved ones
// the next start's, so those options are saved again with a new aspect ratio.
uint32_t SceneHeight() {
  const auto it = g_pending.find("aspect");
  return it != g_pending.end() ? uint32_t(std::atoi(it->second.c_str())) : FrameHeight();
}
constexpr std::string_view kSceneSized[] = {"fsr_resolution", "minimum_resolution"};
// The extent of `percent` of `extent`.
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
  // Display. The Xenos plugin draws only the game's 16:9 frame (native/frame_height.h): at 16:10
  // the renderer changes with the next start, which takes the aspect ratio with it.
  o.push_back({"display",
               "renderer",
               "Renderer",
               {{"Native", "false"}, {"Classic", "true"}},
               FrameHeight() != kGameFrameHeight,
               [] { return Flag("pgr4_native_classic"); },
               [](const std::string& v, uint32_t) { return Bool("pgr4_native_classic", v); },
               [] {
                 if (NativeRenderer* r = NativeRenderer::Get())
                   r->RequestRenderer(!REXCVAR_GET(pgr4_native_classic));
               }});
  o.push_back(
      {"display",
       "aspect",
       "Aspect ratio",
       {{"16:10", "800"}, {"16:9", "720"}},
       true,
       [] { return Flag("pgr4_render_height"); },
       [](const std::string& v, uint32_t) { return Assignments{{"pgr4_render_height", v}}; }});
  o.push_back({"display", "hdr", "HDR", on_off, true, [] { return Flag("pgr4_native_hdr"); },
               [](const std::string& v, uint32_t) { return Bool("pgr4_native_hdr", v); }, nullptr,
               native});
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
               [](const std::string& v, uint32_t) {
                 return Assignments{{"pgr4_native_hdr_paper_white", v}};
               },
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
               [](const std::string& v, uint32_t) {
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
                                100.0 * FlagInt("pgr4_native_upscaler_height") / FrameHeight());
               },
               [](const std::string& v, uint32_t scene) {
                 const int p = std::atoi(v.c_str());
                 return Assignments{{"pgr4_native_upscaler_width", Percent(1280, p)},
                                    {"pgr4_native_upscaler_height", Percent(scene, p)}};
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
               [](const std::string& v, uint32_t) {
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
                        100.0 * FlagInt("pgr4_native_upscaler_min_height") / FrameHeight());
       },
       [](const std::string& v, uint32_t scene) {
         return Assignments{
             {"pgr4_native_upscaler_min_height", Percent(scene, std::atoi(v.c_str()))}};
       },
       RequestAntialiasing,
       {"dynamic_resolution", {"true"}}});
  // FidelityFX's frames between the game's, from FSR's depth and motion (frame_generation.h).
  o.push_back(
      {"display", "frame_generation", "Frame generation", on_off, true,
       [] { return Flag("pgr4_native_frame_generation"); },
       [](const std::string& v, uint32_t) { return Bool("pgr4_native_frame_generation", v); },
       nullptr, fsr});
  o.push_back(
      {"display",
       "anisotropy",
       "Texture filtering",
       {{"Game's", "1"}, {"2x", "2"}, {"4x", "4"}, {"8x", "8"}, {"16x", "16"}},
       true,
       [] { return Nearest(Find("anisotropy"), FlagInt("pgr4_native_anisotropy")); },
       [](const std::string& v, uint32_t) { return Assignments{{"pgr4_native_anisotropy", v}}; },
       nullptr,
       native});
  // Each frame shown for 1 to 3 refreshes (D3D12Device::set_present_interval).
  std::vector<Choice> frame_rates;
  for (int n = 1; n <= 3; ++n)
    frame_rates.push_back(
        {std::to_string(std::lround(double(RefreshRate()) / n)) + " fps", std::to_string(n)});
  o.push_back({"display", "frame_rate", "Frame rate", std::move(frame_rates), false,
               [] { return Nearest(Find("frame_rate"), FlagInt("pgr4_native_present_interval")); },
               [](const std::string& v, uint32_t) {
                 return Assignments{{"pgr4_native_present_interval", v}};
               },
               [] {
                 if (NativeRenderer* r = NativeRenderer::Get()) r->RequestFrameRate();
               },
               native});
  o.push_back(
      {"display",
       "frame_latency",
       "Frame latency",
       {{"1", "1"}, {"2", "2"}, {"3", "3"}},
       true,
       [] { return Nearest(Find("frame_latency"), FlagInt("pgr4_native_frame_latency")); },
       [](const std::string& v, uint32_t) { return Assignments{{"pgr4_native_frame_latency", v}}; },
       nullptr,
       native});
  // Driving: the player's car's automatic (development.md's "Gearbox") and every vehicle's lights.
  o.push_back({"driving",
               "automatic",
               "Automatic downshifts",
               {{"Game's", "false"}, {"Earlier", "true"}},
               false,
               [] { return Flag("pgr4_automatic_keep_revs"); },
               [](const std::string& v, uint32_t) { return Bool("pgr4_automatic_keep_revs", v); }});
  o.push_back(
      {"driving",
       "headlights",
       "Vehicle lights",
       {{"Game's", "race"}, {"On", "on"}, {"Off", "off"}},
       false,
       [] { return Flag("pgr4_headlights"); },
       [](const std::string& v, uint32_t) { return Assignments{{"pgr4_headlights", v, true}}; },
       Pgr4HeadlightsChanged});
  // Developer.
  o.push_back({"developer", "debug_keys", "Debug keys", on_off, false,
               [] { return Flag("pgr4_native_debug_keys"); },
               [](const std::string& v, uint32_t) { return Bool("pgr4_native_debug_keys", v); }});
  o.push_back({"developer", "game_debug_menus", "Game debug menus", on_off, false,
               [] { return Flag("pgr4_game_debug_menus"); },
               [](const std::string& v, uint32_t) { return Bool("pgr4_game_debug_menus", v); }});
  // FidelityFX's debug views and FSR 4's watermark, from the next frame (fidelityfx.h).
  o.push_back({"developer", "fsr_debug", "FSR debug view", on_off, false,
               [] { return Flag("pgr4_native_fsr_debug"); },
               [](const std::string& v, uint32_t) { return Bool("pgr4_native_fsr_debug", v); }});
  o.push_back({"developer", "occlusion", "Occlusion queries", on_off, true,
               [] { return Flag("pgr4_native_occlusion"); },
               [](const std::string& v, uint32_t) { return Bool("pgr4_native_occlusion", v); }});
  o.push_back(
      {"developer", "async_pipelines", "Async pipelines", on_off, true,
       [] { return Flag("pgr4_native_async_pipelines"); },
       [](const std::string& v, uint32_t) { return Bool("pgr4_native_async_pipelines", v); }});
  o.push_back(
      {"developer", "pipeline_cache", "Pipeline cache", on_off, true,
       [] { return Flag("pgr4_native_pipeline_cache"); },
       [](const std::string& v, uint32_t) { return Bool("pgr4_native_pipeline_cache", v); }});
  o.push_back({"developer", "gpu_timing", "GPU timing log", on_off, true,
               [] { return Flag("pgr4_native_gpu_timing"); },
               [](const std::string& v, uint32_t) { return Bool("pgr4_native_gpu_timing", v); }});
  o.push_back({"developer",
               "log_level",
               "Log level",
               {{"Errors", "error"}, {"Warnings", "warn"}, {"Info", "info"}, {"Debug", "debug"}},
               false,
               [] { return Flag("log_level"); },
               [](const std::string& v, uint32_t) { return Assignments{{"log_level", v, true}}; }});
  return o;
}

const std::vector<Option>& Options() {
  static const std::vector<Option> options = MakeOptions();
  return options;
}

// The guest UI thread runs the menus; the lock is for the configuration file.
std::mutex g_mutex;
std::filesystem::path g_file;

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
  for (const Assignment& a : o.assign(v, FrameHeight()))
    if (!rex::cvar::SetFlagByName(a.cvar, a.value))
      REXLOG_ERROR("PGR4 settings: cannot set {} to {}", a.cvar, a.value);
  if (o.applied) o.applied();
}

std::string_view Trim(std::string_view s) {
  const size_t first = s.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

// The `# comment` ending a `name = value` line from `from` on, outside a string, with the blanks
// before it; empty without one. A basic string ("...") escapes a quote, a literal one ('...')
// cannot.
std::string_view Comment(std::string_view line, size_t from) {
  char quote = 0;
  for (size_t i = from; i < line.size(); ++i) {
    const char c = line[i];
    if (quote) {
      if (c == quote && !(quote == '"' && line[i - 1] == '\\')) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '#') {
      const size_t text = line.substr(0, i).find_last_not_of(" \t");
      return line.substr(text == std::string_view::npos ? 0 : text + 1);
    }
  }
  return {};
}

// A `name = value` line of a TOML file: the name and the value without the comment after it.
struct Entry {
  std::string_view name, value;
};
std::optional<Entry> Parse(std::string_view line) {
  line = Trim(line);
  const size_t eq = line.find('=');
  if (line.starts_with('#') || line.starts_with('[') || eq == std::string_view::npos) return {};
  const Entry e{Trim(line.substr(0, eq)),
                Trim(line.substr(eq + 1, line.size() - eq - 1 - Comment(line, eq + 1).size()))};
  if (e.name.empty() || e.value.empty()) return {};
  return e;
}

// `cvar = value` lines for the configuration file, the values as TOML writes them.
using Lines = std::vector<std::pair<std::string, std::string>>;

// The lines of the options `set` as they are chosen now, sized for the scene of the next start.
Lines Chosen(const std::vector<const Option*>& set) {
  Lines lines;
  for (const Option* o : set)
    for (const Assignment& a : o->assign(Value(*o), SceneHeight()))
      lines.emplace_back(a.cvar, a.text ? '"' + a.value + '"' : a.value);
  return lines;
}

// `set` written into the configuration file in place: the line of a cvar takes its value,
// keeping its comment, and a cvar the file has no line for gets one under the menu's comment
// unless the value is its default. The file's other lines, and the values it has for other cvars,
// stay.
bool Write(const Lines& set) {
  std::lock_guard lock(g_mutex);
  if (g_file.empty()) return false;
  std::vector<std::string> lines;
  bool crlf = false;
  {
    std::ifstream f(g_file, std::ios::binary);
    for (std::string line; std::getline(f, line);) {
      if (line.ends_with('\r')) {
        line.pop_back();
        crlf = true;
      }
      lines.push_back(std::move(line));
    }
  }
  // A cvar's line, and where new ones go: before the first table, whose keys are not cvars.
  std::map<std::string, size_t, std::less<>> at;
  size_t end = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (Trim(lines[i]).starts_with('[')) end = std::min(end, i);
    if (const std::optional<Entry> e = Parse(lines[i]); e && i < end) at.emplace(e->name, i);
  }
  constexpr std::string_view kAdded = "# Set in the game's Options menu.";
  bool header = std::ranges::contains(lines, kAdded);
  for (const auto& [cvar, value] : set) {
    const std::string line = cvar + " = " + value;
    if (const auto it = at.find(cvar); it != at.end()) {
      std::string& old = lines[it->second];
      old = line + std::string(Comment(old, old.find('=') + 1));
      continue;
    }
    const rex::cvar::FlagEntry* flag = rex::cvar::GetFlagInfo(cvar);
    if (flag && ('"' + flag->default_value + '"' == value || flag->default_value == value))
      continue;
    if (!header) {
      header = true;
      lines.insert(lines.begin() + std::ptrdiff_t(end), {"", std::string(kAdded)});
      end += 2;
    }
    lines.insert(lines.begin() + std::ptrdiff_t(end), line);
    at.emplace(cvar, end++);
  }
  std::string toml;
  for (const std::string& line : lines) toml += line + (crlf ? "\r\n" : "\n");
  const std::filesystem::path temp = std::filesystem::path(g_file) += ".new";
  if (std::ofstream f(temp, std::ios::binary | std::ios::trunc); !(f << toml)) {
    REXLOG_ERROR("PGR4 settings: cannot write {}", temp.string());
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(temp, g_file, ec);
  if (ec) {
    REXLOG_ERROR("PGR4 settings: cannot write {}: {}", g_file.string(), ec.message());
    return false;
  }
  return true;
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

// Lua: Pgr4.Apply(page, {[id] = choice from 0...}) -> whether a start-up option of the page
// differs from what runs (a restart applies it).
void LuaApply(PPCContext& ctx, uint8_t* base) {
  pgr4::lua::Lua lua(ctx, base, ctx.r3.u32);
  const std::string page = lua.ToString(1);
  if (lua.TypeOf(2) == pgr4::lua::kTable) {
    // Only the options changed are set and saved: an option's shown choice is the one nearest
    // its value, which a configuration file or the command line may have set between choices.
    std::vector<const Option*> changed;
    for (const Option& o : Options()) {
      if (o.page != page) continue;
      lua.GetField(2, o.id);
      if (lua.TypeOf(-1) == pgr4::lua::kNumber) {
        const double n = lua.ToNumber(-1);
        if (n >= 0 && n < double(o.choices.size()) && o.choices[size_t(n)].value != Value(o)) {
          Set(o, o.choices[size_t(n)].value);
          changed.push_back(&o);
        }
      }
      lua.Pop(1);
    }
    if (std::ranges::contains(changed, "aspect", &Option::id))
      for (const std::string_view id : kSceneSized)
        if (!std::ranges::contains(changed, id, &Option::id)) changed.push_back(&Find(id));
    if (!changed.empty()) Write(Chosen(changed));
  }
  lua.PushBoolean(std::ranges::any_of(
      Options(), [&](const Option& o) { return o.page == page && g_pending.contains(o.id); }));
  pgr4::lua::Results(ctx, 1);
}

constexpr pgr4::lua::Function kFunctions[] = {
    {"Page", LuaPage},
    {"Apply", LuaApply},
};

}  // namespace

void LoadSettings(const std::filesystem::path& config, const std::filesystem::path& legacy) {
  {
    std::lock_guard lock(g_mutex);
    g_file = config;
  }
  std::error_code ec;
  if (!std::filesystem::exists(legacy, ec)) return;
  rex::cvar::LoadConfig(legacy);
  Lines lines;
  {
    std::ifstream f(legacy);  // closed before the file goes: Windows keeps an open file
    for (std::string line; std::getline(f, line);)
      if (const std::optional<Entry> e = Parse(line)) lines.emplace_back(e->name, e->value);
  }
  if (Write(lines)) {
    std::filesystem::remove(legacy, ec);
    REXLOG_INFO("PGR4 settings: {} taken into {}", legacy.string(), config.string());
  }
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
