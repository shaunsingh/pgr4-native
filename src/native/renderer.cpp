// pgr4-native PGR4 native renderer: NativeRenderer host.
#include "renderer.h"

#include "async_file.h"
#include "clock.h"
#include "d3d12_device.h"
#include "d3d_state.h"
#include "draw.h"
#include "frame_pacer.h"
#include "gpu_timer.h"
#include "guest_memory.h"
#include "occlusion.h"
#include "present.h"
#include "profile.h"
#include "render_targets.h"
#include "resolve.h"
#include "textures.h"
#include "upscaler.h"

#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

// patches/0025 (xam_input.cpp): what user 0's XamInputGetState calls returned
// since the last call, merged (bit layout there).
extern "C" uint64_t rex_xam_take_frame_pad();

REXCVAR_DEFINE_BOOL(pgr4_native, false, "PGR4 native",
                    "Run the native D3D12 renderer beside the Xenos plugin (debug window)");
REXCVAR_DEFINE_BOOL(pgr4_native_primary, false, "PGR4 native",
                    "The native renderer is the game's output: it presents into the game window "
                    "and the Xenos plugin skips all draws, resolves and swaps (gpu_skip_draws, "
                    "patches/0031); implies --pgr4_native");
REXCVAR_DEFINE_INT32(pgr4_native_msaa, 2, "PGR4 native",
                     "Samples of the 3D scene's MSAA without the upscaler: 1, 2 (the game's), 4 "
                     "or 8 (fewer if the GPU lacks them)");
REXCVAR_DEFINE_STRING(pgr4_native_csv, "", "PGR4 native",
                      "Per-frame CSV of the native path: counts and CPU time per hook class");
REXCVAR_DEFINE_INT32(pgr4_native_width, 1280, "PGR4 native", "Debug window width");
REXCVAR_DEFINE_INT32(pgr4_native_height, 720, "PGR4 native", "Debug window height");
REXCVAR_DEFINE_BOOL(pgr4_native_profile, false, "PGR4 native",
                    "Per-stage CPU time of the draw path (RDTSC), appended to the native CSV as "
                    "st_<stage>_us columns and to the stats log line");
REXCVAR_DEFINE_BOOL(pgr4_native_gpu_timing, false, "PGR4 native",
                    "GPU timestamps around every pass of the native frame (draws per render "
                    "target set, clears, resolves, present); the costliest passes are logged "
                    "every --pgr4_d3d_stats_frames frames");
REXCVAR_DEFINE_BOOL(pgr4_native_gpu_timing_shaders, false, "PGR4 native",
                    "With --pgr4_native_gpu_timing: time draws per vertex/pixel shader pair "
                    "instead of per render target set, and log the ucode of the costliest "
                    "shaders once (render thread mode)");
REXCVAR_DEFINE_BOOL(pgr4_native_texture_capture_order, true, "PGR4 native",
                    "Bind textures as they were when the game queued the draw: a texture the game "
                    "rewrites before the render thread reaches the draw keeps its older data for "
                    "it (TextureCache::MarkCapture)");
REXCVAR_DEFINE_BOOL(pgr4_native_render_thread, true, "PGR4 native",
                    "Record the native frames on their own thread (\"PGR4 native render\"): the "
                    "game thread only copies what each D3D call reads from guest memory and "
                    "queues it, at most one frame ahead of the render thread");
REXCVAR_DEFINE_INT32(pgr4_native_submit_draws, 256, "PGR4 native",
                     "Submit the frame's commands every this many draws, so the GPU starts on them "
                     "while the rest of the frame is recorded (0: one submission per frame, at the "
                     "Swap)");
REXCVAR_DEFINE_BOOL(pgr4_native_occlusion, true, "PGR4 native",
                    "With --pgr4_native_primary: count the samples of the game's occlusion "
                    "queries (the light flares) on the host, so lights behind walls and cars stay "
                    "hidden; off, every query reports its whole draw visible")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pgr4_native_renderer_switch, true, "PGR4 native",
                    "With --pgr4_native_primary, the key bind_renderer_switch (F9) switches "
                    "between the native renderer and the Xenos plugin at the next Swap");
REXCVAR_DEFINE_INT32(pgr4_native_frame_latency, 3, "PGR4 native",
                     "With --pgr4_native_primary: pace the game's frames on the display, with at "
                     "most this many frames begun but not yet shown (1-3, frame_pacer.h; the game "
                     "thread, render thread and GPU each hold one at 90 fps); 0 leaves "
                     "the pacing to the game's vblank wait (with --pgr4_native_csv the shown "
                     "presents are still logged)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pgr4_native_low_latency, true, "PGR4 native",
                    "With the frames paced on the display: begin each frame as late as the "
                    "time the frames before it waited for the display allows (frame_pacer.h), "
                    "so a frame limit lowers the latency instead of raising it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(pgr4_native_latency_margin_us, 4000, "PGR4 native",
                     "With --pgr4_native_low_latency: how long before the display shows a frame "
                     "the GPU should have finished it, in microseconds")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(
    pgr4_native_mark_audit, 0, "PGR4 native",
    "Debug, render thread mode: a mark (F12 or <capture dir>/capture.request) also "
    "logs the draws of the next this many frames to <capture dir>/draws-f<frame>.txt "
    "and audits them: each shader against the guest's ucode, the render thread's "
    "registers against the game thread's, and the shared memory each draw reads "
    "against guest memory as it was when the draw was queued (0: off)");
REXCVAR_DECLARE(int32_t, pgr4_d3d_stats_frames);
REXCVAR_DECLARE(bool, pgr4_native_luma_probe);

namespace pgr4::native {

struct NativeRenderer::Csv {
  std::unique_ptr<AsyncFile> file;
  // Window for the log summary.
  uint64_t frames = 0, draws = 0, draw_ns = 0, swap_ns = 0, frame_ns = 0, hook_ns = 0,
           hook_wait_ns = 0;
  // Profile: the window's stage ticks.
  uint64_t stage_ticks[profile::kStageCount] = {}, track_calls = 0;
  TextureCache::Stats tex_prev;                  // texture counters at the previous CSV row
  uint64_t evicted_prev = 0, evict_ns_prev = 0;  // the same for every row (tex_prev: profile)
  bool luma = false;                             // --pgr4_native_luma_probe columns
  std::string line;                              // the row being written
};

namespace {
// Set once the device or swapchain failed; the native path stops.
std::atomic<bool> g_failed{false};

// The game window (the SDK's display window), and ReXGlue's presenter, which presents into it
// unless the renderer does (OwnGameWindow).
rex::ui::Window* GameWindow() {
  rex::Runtime* runtime = rex::Runtime::instance();
  return runtime ? runtime->display_window() : nullptr;
}
rex::ui::Presenter* SdkPresenter() {
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::system::IGraphicsSystem* graphics = runtime ? runtime->graphics_system() : nullptr;
  return graphics ? graphics->presenter() : nullptr;
}

// `path`, or the first of <stem>.1<ext>, <stem>.2<ext>... that does not exist: a title launched
// from this one gets this process's arguments (games/common/title_launch.cpp), and PGR4 started
// again after Geometry Wars must not overwrite the first run's files.
std::string UnusedPath(const std::string& path) {
  std::filesystem::path p(path);
  std::error_code ec;
  for (int i = 1; std::filesystem::exists(p, ec); ++i) {
    p = path;
    p.replace_filename(p.stem().string() + "." + std::to_string(i) + p.extension().string());
  }
  return p.string();
}

bool FlagOn(const char* name) {
  const std::string v = rex::cvar::GetFlagByName(name);
  return v == "true" || v == "1";
}

// One column of the frame CSV: its name in the header and its value in this frame's row, so
// that the header and the rows come from one list.
struct CsvCell {
  enum Format : uint8_t { kUnsigned, kSigned, kHex4, kFixed1, kFixed3, kFixed4 };
  const char* name;
  Format format;
  uint64_t u;  // kUnsigned, kSigned (two's complement), kHex4
  double d;    // kFixed*: digits after the point
};
CsvCell Uint(const char* name, uint64_t v) { return {name, CsvCell::kUnsigned, v, 0.0}; }
CsvCell Int(const char* name, int64_t v) { return {name, CsvCell::kSigned, uint64_t(v), 0.0}; }
CsvCell Hex4(const char* name, uint64_t v) { return {name, CsvCell::kHex4, v, 0.0}; }
CsvCell Fix1(const char* name, double v) { return {name, CsvCell::kFixed1, 0, v}; }
CsvCell Fix3(const char* name, double v) { return {name, CsvCell::kFixed3, 0, v}; }
CsvCell Fix4(const char* name, double v) { return {name, CsvCell::kFixed4, 0, v}; }

// The cell's name (header) or value, after a comma unless it is the line's first.
void AppendCsvCell(const CsvCell& c, bool header, bool comma, std::string* line) {
  if (comma) line->push_back(',');
  if (header) {
    line->append(c.name);
    return;
  }
  char text[64];
  int n = 0;
  switch (c.format) {
    case CsvCell::kUnsigned:
      n = std::snprintf(text, sizeof(text), "%llu", (unsigned long long)c.u);
      break;
    case CsvCell::kSigned:
      n = std::snprintf(text, sizeof(text), "%lld", (long long)int64_t(c.u));
      break;
    case CsvCell::kHex4:
      n = std::snprintf(text, sizeof(text), "%04llX", (unsigned long long)c.u);
      break;
    case CsvCell::kFixed1:
      n = std::snprintf(text, sizeof(text), "%.1f", c.d);
      break;
    case CsvCell::kFixed3:
      n = std::snprintf(text, sizeof(text), "%.3f", c.d);
      break;
    case CsvCell::kFixed4:
      n = std::snprintf(text, sizeof(text), "%.4f", c.d);
      break;
  }
  line->append(text, size_t(std::clamp(n, 0, int(sizeof(text)) - 1)));
}

void AppendCsvCells(std::span<const CsvCell> cells, bool header, bool leading_comma,
                    std::string* line) {
  for (size_t i = 0; i < cells.size(); ++i)
    AppendCsvCell(cells[i], header, i || leading_comma, line);
}

// The window's presenter belongs to the UI thread.
void ConnectSdkPresenter(bool connect) {
  rex::ui::Window* window = GameWindow();
  rex::ui::Presenter* presenter = SdkPresenter();
  if (window && presenter)
    window->app_context().CallInUIThreadSynchronous(
        [&] { window->SetPresenter(connect ? presenter : nullptr); });
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Render thread mode (renderer.h).

struct NativeRenderer::Queue {
  // Commands per batch before the guest thread hands it over (and at every Swap). Each
  // hand-over can wake the render thread (a syscall), so batches are large.
  static constexpr size_t kBatchCommands = 256;

  enum Kind : uint8_t { kDraw, kClear, kResolve, kSwap, kQuery };
  struct Cmd {
    Kind kind;
    uint32_t index;    // into the batch's vector of that kind
    uint32_t reg_end;  // Batch::regs up to here are applied before the command
  };
  struct DrawCmd {
    DrawState state;  // regs and immediate ucode pointers are filled in on the render thread
    DrawPrep prep;
    uint32_t copy_begin, copy_end;    // Batch::copies
    uint32_t audit_begin, audit_end;  // Batch::audits
  };
  struct SwapCmd {
    SwapCall call;
    // Guest-thread numbers of the frame, added to the render thread's before its CSV row.
    uint64_t hook_ns, hook_wait_ns, hook_max_ns, state_fail, track_calls, pad;
    uint64_t guest_begin_ns, guest_swap_ns, pace_wait_ns;
    DrawRenderer::PrepareCounters prep;
    uint64_t ticks[profile::kStageCount];
    uint32_t log_frames;  // a marked Swap with --pgr4_native_mark_audit: the frames to log
  };
  struct Batch {
    std::vector<Cmd> cmds;
    RegLog regs;
    std::vector<VertexCopy> copies;
    std::vector<uint32_t> segments;  // indices into copies where a copy segment starts
    std::vector<AuditRange> audits;
    std::vector<DrawCmd> draws;
    std::vector<ClearCall> clears;
    std::vector<ResolveParams> resolves;
    std::vector<SwapCmd> swaps;
    std::vector<QueryCall> queries;
    void Clear() {
      cmds.clear();
      regs.clear();
      copies.clear();
      segments.clear();
      audits.clear();
      draws.clear();
      clears.clear();
      resolves.clear();
      swaps.clear();
      queries.clear();
    }
  };

  std::mutex mutex;
  std::condition_variable work_cv, done_cv;
  std::deque<std::unique_ptr<Batch>> pending;  // guest -> render thread
  std::vector<std::unique_ptr<Batch>> spare;   // render -> guest thread
  uint64_t frames_done = 0;                    // Swaps the render thread finished (mutex)
  // Frames whose fence the GPU reached (render thread writes; DrawRenderer::Prepare's tags).
  std::atomic<uint64_t> gpu_frames_done{0};
  std::thread thread;

  // Guest thread. The tracker records the register changes straight into the current batch.
  std::unique_ptr<Batch> current = std::make_unique<Batch>();
  Queue() { GuestState().RecordInto(&current->regs); }
  uint64_t frame = 0;  // Swaps queued
  bool capture_armed = false;
  // Time in the On* calls this frame: the Swap's in ns, the others' in ticks, summed per call
  // and converted at the Swap.
  uint64_t hook_ns = 0, hook_wait_ns = 0, hook_max_ns = 0, state_fail = 0;
  uint64_t hook_ticks = 0, hook_max_ticks = 0;
  uint64_t begin_ns = 0, pace_wait_ns = 0;  // the frame being captured (FrameStats)
  void AddHook(uint64_t ticks) {
    hook_ticks += ticks;
    hook_max_ticks = std::max(hook_max_ticks, ticks);
  }

  // Render thread.
  uint32_t regs[kGuestRegisterCount] = {};
  std::deque<std::pair<uint64_t, uint64_t>> frame_fences;  // (frames submitted, their fence)

  void Publish() {
    std::unique_ptr<Batch> next;
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending.push_back(std::move(current));
      if (!spare.empty()) {
        next = std::move(spare.back());
        spare.pop_back();
      }
    }
    work_cv.notify_one();
    current = next ? std::move(next) : std::make_unique<Batch>();
    GuestState().RecordInto(&current->regs);
  }
  void Push(Kind kind, uint32_t index) {
    current->cmds.emplace_back(kind, index, uint32_t(current->regs.size()));
    if (kind != kSwap && current->cmds.size() >= kBatchCommands) Publish();
  }
  // Blocks the guest thread until the render thread finished `frames` Swaps; returns the ns.
  uint64_t WaitFrames(uint64_t frames) {
    std::unique_lock<std::mutex> lock(mutex);
    if (frames_done >= frames) return 0;
    const uint64_t t0 = NowNs();
    done_cv.wait(lock, [&] { return frames_done >= frames; });
    return NowNs() - t0;
  }
};

NativeRenderer* NativeRenderer::Get() {
  if (!(REXCVAR_GET(pgr4_native) || REXCVAR_GET(pgr4_native_primary)) || g_failed) return nullptr;
  // Never destroyed: the process exits with the game, and the guest thread may still be in a
  // hook during static destruction.
  static NativeRenderer* r = [] {
    auto* n = new NativeRenderer();
    if (!n->Initialize()) {
      REXLOG_ERROR("PGR4 native: initialization failed; native path off for this run");
      g_failed = true;
      delete n;
      return static_cast<NativeRenderer*>(nullptr);
    }
    live_.store(n, std::memory_order_release);
    return n;
  }();
  return g_failed ? nullptr : r;
}

NativeRenderer::~NativeRenderer() {
  if (switch_bound_) rex::ui::UnregisterBind("bind_renderer_switch");
  if (upscaler_bound_) {
    rex::ui::UnregisterBind("bind_upscaler");
    rex::ui::UnregisterBind("bind_upscaler_debug");
  }
}

bool NativeRenderer::Initialize() {
  device_ = std::make_unique<D3D12Device>();
  uint32_t w = uint32_t(std::max(64, REXCVAR_GET(pgr4_native_width)));
  uint32_t h = uint32_t(std::max(64, REXCVAR_GET(pgr4_native_height)));
  const bool primary = primary_ = REXCVAR_GET(pgr4_native_primary);
  const bool paced = primary && REXCVAR_GET(pgr4_native_frame_latency) > 0;
  // The pacer, or with the CSV only its log of the presents shown.
  const bool pacer = paced || (primary && !REXCVAR_GET(pgr4_native_csv).empty());
  HWND game_window = nullptr;
  if (primary) {
    // The first D3D call comes after the SDK opened its window; wait briefly in case not.
    for (int i = 0; i < 100 && !game_window; ++i) {
      if (rex::ui::Window* window = GameWindow())
        game_window = static_cast<HWND>(window->GetNativeWindowHandle());
      if (!game_window) Sleep(50);
    }
    if (!game_window) {
      REXLOG_ERROR("PGR4 native: --pgr4_native_primary: no game window found");
      return false;
    }
    // The pacer needs FIFO: vkd3d-proton releases the frame latency waitable on the display's
    // present feedback only for capped present modes, and on the blit for the others. Without
    // the pacer, the same switch as the SDK presenter's.
    device_->set_vsync(paced || FlagOn("d3d12_present_vsync"));
    device_->set_frame_latency_waitable(pacer);
  }
  if (!device_->Initialize(w, h, primary ? L"PGR4 native" : L"PGR4 native (debug)", game_window))
    return false;
  if (REXCVAR_GET(pgr4_native_gpu_timing)) {
    if (device_->EnableGpuTimer()) {
      gpu_timer_ = device_->gpu_timer();
      gpu_timer_->set_per_shader(REXCVAR_GET(pgr4_native_gpu_timing_shaders));
    } else
      REXLOG_ERROR("PGR4 native: GPU timing unavailable");
  }
  rt_ = std::make_unique<RenderTargets>();
  if (!rt_->Initialize(device_->device())) return false;
  if (const int32_t msaa = REXCVAR_GET(pgr4_native_msaa);
      msaa == 1 || msaa == 2 || msaa == 4 || msaa == 8) {
    scene_msaa_ = rt_->SupportedSamples(uint32_t(msaa));
    if (scene_msaa_ != uint32_t(msaa))
      REXLOG_WARN("PGR4 native: {}x MSAA unsupported; the scene takes {}x", msaa, scene_msaa_);
  } else {
    REXLOG_ERROR("PGR4 native: --pgr4_native_msaa={} is not 1, 2, 4 or 8", msaa);
  }
  resolver_ = std::make_unique<Resolver>();
  if (!resolver_->Initialize(device_->device(), rt_.get(), device_->command_queue())) return false;
  resolver_->set_gpu_timer(gpu_timer_);
  SwitchUpscaler(Upscaler::Requested());
  textures_ = std::make_unique<TextureCache>(device_->device(), rex::system::kernel_memory());
  textures_->SetResolvedLookup([this](uint32_t a) { return resolver_->Find(a); });
  textures_->SetCaptureOrder(REXCVAR_GET(pgr4_native_texture_capture_order));
  resolver_->SetChangedCallback([this](uint32_t a) { textures_->OnResolved(a); });
  draw_ = std::make_unique<DrawRenderer>(*this);
  if (!draw_->Initialize()) return false;
  present_ = std::make_unique<Presenter>(*this);
  if (!present_->Initialize()) return false;
  // Beside the Xenos plugin (debug window) the plugin's fake counts are the game's.
  if (primary && REXCVAR_GET(pgr4_native_occlusion)) {
    occlusion_ = std::make_unique<OcclusionQueries>();
    if (!occlusion_->Initialize(device_->device()) ||
        !rex::cvar::SetFlagByName("occlusion_query_external", "true")) {
      REXLOG_ERROR(
          "PGR4 native: occlusion queries unavailable (patches/0049 missing?); every query "
          "reports its draw visible");
      occlusion_.reset();
    }
  }
  // Only once native is up: from here the Xenos plugin does no draw, copy or swap work, and its
  // command processor only runs fences, write-backs and interrupts.
  if (primary && !rex::cvar::SetFlagByName("gpu_skip_draws", "true"))
    REXLOG_ERROR(
        "PGR4 native: could not set gpu_skip_draws (patches/0031 missing?); "
        "Xenos keeps drawing");
  // A handheld has no keyboard for the replay name prompt; the title's default name is what
  // confirming the dialog unchanged returns.
  if (primary && !rex::cvar::SetFlagByName("xam_keyboard_accept_default", "true"))
    REXLOG_ERROR(
        "PGR4 native: could not set xam_keyboard_accept_default "
        "(patches/0024 missing?)");
  if (primary && REXCVAR_GET(pgr4_native_renderer_switch)) {
    rex::ui::RegisterBind("bind_renderer_switch", "F9",
                          "PGR4 native: switch between the native renderer and the Xenos plugin",
                          [this] { switch_requested_.store(true, std::memory_order_relaxed); });
    switch_bound_ = true;
  }
  if (primary) {
    rex::ui::RegisterBind(
        "bind_upscaler", "F7",
        "PGR4 native: switch the 3D scene between MSAA and FidelityFX Super Resolution",
        [this] { upscaler_switch_requested_.store(true, std::memory_order_relaxed); });
    rex::ui::RegisterBind(
        "bind_upscaler_debug", "F8",
        "PGR4 native: show FidelityFX Super Resolution's debug view (its inputs and history)",
        [this] { upscaler_debug_requested_.store(true, std::memory_order_relaxed); });
    upscaler_bound_ = true;
  }
  csv_ = std::make_unique<Csv>();
  profile::enabled.store(REXCVAR_GET(pgr4_native_profile), std::memory_order_relaxed);
  const std::string path = REXCVAR_GET(pgr4_native_csv).empty()
                               ? std::string()
                               : UnusedPath(REXCVAR_GET(pgr4_native_csv));
  if (!path.empty()) {
    csv_->file = AsyncFile::Create(path);
    if (csv_->file) {
      csv_->luma = REXCVAR_GET(pgr4_native_luma_probe);
      std::string header;
      AppendCsvColumns(FrameStats{}, 0, true, &header);
      if (profile::On()) AppendCsvProfileColumns(true, &header);
      if (csv_->luma) AppendCsvLumaColumns(true, &header);
      header += '\n';
      csv_->file->Append(header);
    } else {
      REXLOG_ERROR("PGR4 native: cannot open {}", path);
    }
  }
  if (pacer) {
    std::unique_ptr<AsyncFile> log;
    if (!path.empty()) {
      const std::string log_path =
          path.substr(0, path.size() - (path.ends_with(".csv") ? 4 : 0)) + "-presents.csv";
      if (!(log = AsyncFile::Create(log_path)))
        REXLOG_ERROR("PGR4 native: cannot open {}", log_path);
    }
    pacer_ = std::make_unique<FramePacer>(FramePacer::Options{
        .frame_latency = uint32_t(std::max(0, REXCVAR_GET(pgr4_native_frame_latency))),
        .low_latency = REXCVAR_GET(pgr4_native_low_latency),
        .latency_margin_ns =
            uint64_t(std::max(0, REXCVAR_GET(pgr4_native_latency_margin_us))) * 1000,
        .log = std::move(log)});
    if (pacer_->frame_latency() > 0)
      REXLOG_INFO("PGR4 native: frames paced on the display, at most {} begun but not shown",
                  pacer_->frame_latency());
  }
  frame_start_ns_ = NowNs();
  if (REXCVAR_GET(pgr4_native_render_thread)) {
    queue_ = std::make_unique<Queue>();
    queue_->thread = std::thread([this] { RenderThreadMain(); });
    REXLOG_INFO("PGR4 native: recording on the render thread");
  }
  return true;
}

ID3D12Device* NativeRenderer::d3d12() const { return device_->device(); }
ID3D12CommandQueue* NativeRenderer::command_queue() const { return device_->command_queue(); }
ID3D12GraphicsCommandList* NativeRenderer::command_list() const { return device_->command_list(); }
uint64_t NativeRenderer::frame_fence() const { return device_->frame_fence(); }

void NativeRenderer::SwitchUpscaler(bool on) {
  if (on && !upscaler_) {
    upscaler_ = std::make_unique<Upscaler>();
    if (!upscaler_->Initialize(device_->device())) {
      REXLOG_ERROR("PGR4 native: upscaler unavailable; the scene keeps its MSAA");
      upscaler_.reset();
      return;
    }
  }
  upscaler_on_ = on;
  // The scene's targets are keyed by their sample count: the next draws make the other ones,
  // and the unused ones age out (RenderTargets::BeginFrame).
  if (on) {
    rt_->OverrideScene(Upscaler::kSceneSurfaceMask, Upscaler::kSceneSurface, 1, upscaler_->scale());
    upscaler_->Reset();
  } else if (scene_msaa_ != 2) {
    rt_->OverrideScene(Upscaler::kSceneSurfaceMask, Upscaler::kSceneSurface, scene_msaa_, {});
  } else {
    rt_->OverrideScene(0, 0, 1, {});
  }
  resolver_->set_upscaler(upscaler());
  if (on)
    REXLOG_INFO("PGR4 native: the scene upscaled by FidelityFX");
  else
    REXLOG_INFO("PGR4 native: the scene with {}x MSAA", scene_msaa_);
}

void NativeRenderer::BeginFrameModules() {
  if (upscaler_switch_requested_.exchange(false, std::memory_order_relaxed))
    SwitchUpscaler(!upscaler_on_);
  if (upscaler_debug_requested_.exchange(false, std::memory_order_relaxed) && upscaler()) {
    // The context made again: its old resources may be in the lists still running.
    if (!upscaler_->debug_context()) device_->WaitIdle();
    if (!upscaler_->SetDebugView(!upscaler_->debug_view())) {
      REXLOG_ERROR("PGR4 native: upscaler unavailable; the scene keeps its MSAA");
      upscaler_.reset();
      SwitchUpscaler(false);
    }
  }
  const uint64_t fence = device_->frame_fence(), done = device_->completed_fence();
  rt_->BeginFrame(frame_index_, fence, done);
  resolver_->set_linear_output(device_->hdr());
  resolver_->BeginFrame(frame_index_, fence, done);
  if (Upscaler* u = upscaler()) u->BeginFrame(frame_index_);
  frame_begun_ = true;
}

void NativeRenderer::RenderThreadMain() {
  SetThreadDescription(GetCurrentThread(), L"PGR4 native render");
  Queue& q = *queue_;
  for (;;) {
    std::unique_ptr<Queue::Batch> b;
    {
      std::unique_lock<std::mutex> lock(q.mutex);
      q.work_cv.wait(lock, [&] { return !q.pending.empty(); });
      b = std::move(q.pending.front());
      q.pending.pop_front();
    }
    size_t reg = 0;
    uint32_t copies_issued = 0;
    size_t segment = 0;
    for (const Queue::Cmd& c : b->cmds) {
      stats_.reg_writes += c.reg_end - reg;
      for (; reg < c.reg_end; ++reg) q.regs[b->regs[reg].reg] = b->regs[reg].value;
      if (g_failed && c.kind != Queue::kSwap) continue;
      switch (c.kind) {
        case Queue::kDraw: {
          Queue::DrawCmd& d = b->draws[c.index];
          d.state.regs = q.regs;
          // A draw whose copies are not recorded yet is the first of its copy segment to
          // need any: it records the whole segment's (DrawRenderer::Prepare).
          uint32_t to = copies_issued;
          if (d.copy_end > copies_issued) {
            while (segment < b->segments.size() && b->segments[segment] <= copies_issued) ++segment;
            to = segment < b->segments.size() ? b->segments[segment] : uint32_t(b->copies.size());
            to = std::max(to, d.copy_end);
          }
          d.prep.copies = b->copies.data() + copies_issued;
          d.prep.copy_count = to - copies_issued;
          d.prep.audit = b->audits.data() + d.audit_begin;
          d.prep.audit_count = d.audit_end - d.audit_begin;
          copies_issued = to;
          ExecuteDraw(d.state, &d.prep);
          break;
        }
        case Queue::kClear:
          ExecuteClear(b->clears[c.index]);
          break;
        case Queue::kResolve:
          ExecuteResolve(b->resolves[c.index]);
          break;
        case Queue::kQuery:
          ExecuteQuery(b->queries[c.index]);
          break;
        case Queue::kSwap: {
          const Queue::SwapCmd& s = b->swaps[c.index];
          if (!g_failed) {
            stats_.hook_ns = s.hook_ns;
            stats_.hook_wait_ns = s.hook_wait_ns;
            stats_.hook_max_ns = s.hook_max_ns;
            stats_.pad = s.pad;
            stats_.guest_begin_ns = s.guest_begin_ns;
            stats_.guest_swap_ns = s.guest_swap_ns;
            stats_.pace_wait_ns = s.pace_wait_ns;
            stats_.state_fail += s.state_fail;
            draw_->AddPrepareCounters(s.prep);
            if (s.log_frames) draw_->LogFrames(s.log_frames);
            if (profile::On()) {
              for (uint32_t i = 0; i < profile::kStageCount; ++i) profile::g.ticks[i] += s.ticks[i];
              profile::g.track_calls += s.track_calls;
            }
            ExecuteSwap(s.call);
          }
          {
            std::lock_guard<std::mutex> lock(q.mutex);
            ++q.frames_done;
          }
          q.done_cv.notify_all();
          break;
        }
      }
    }
    b->Clear();
    std::lock_guard<std::mutex> lock(q.mutex);
    q.spare.push_back(std::move(b));
  }
}

void NativeRenderer::QueueDraw(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = Ticks();
  profile::Begin();
  Queue& q = *queue_;
  Queue::Batch& b = *q.current;
  Queue::DrawCmd& d = b.draws.emplace_back();
  // The render thread wrote the next slot last (batches come back through `spare`): its cache
  // lines are fetched for writing while this draw is captured.
  for (size_t offset = 0; offset < sizeof(Queue::DrawCmd); offset += 64)
    __builtin_prefetch(reinterpret_cast<const char*>(&d + 1) + offset, 1);
  const bool ok = GuestState().ReadDrawState(base, args, &d.state);
  profile::Mark(profile::kReadState);
  if (!ok) {
    b.draws.pop_back();
    ++q.state_fail;
  } else {
    // The render thread records a batch's copies while it runs the batch: each batch starts
    // a copy segment.
    if (b.draws.size() == 1) draw_->BeginCopySegment();
    d.copy_begin = uint32_t(b.copies.size());
    d.audit_begin = uint32_t(b.audits.size());
    // Upload memory of frame F is tagged F + 1: free once F's fence completed.
    draw_->Prepare(d.state, q.frame + 1, &d.prep, &b.copies, &b.audits);
    d.copy_end = uint32_t(b.copies.size());
    d.audit_end = uint32_t(b.audits.size());
    if (d.prep.copy_segment) b.segments.push_back(d.copy_begin);
    d.state.regs = nullptr;
    d.state.vs.immediate = d.state.ps.immediate = nullptr;  // Prepare loaded the shaders
    q.Push(Queue::kDraw, uint32_t(b.draws.size() - 1));
    profile::Mark(profile::kCapRecord);
  }
  q.AddHook(Ticks() - t0);
}

void NativeRenderer::QueueClear(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = Ticks();
  profile::Scope scope(profile::kCapRecord);
  Queue& q = *queue_;
  Queue::Batch& b = *q.current;
  ClearCall& c = b.clears.emplace_back();
  if (RenderTargets::DecodeClear(base, args, &c))
    q.Push(Queue::kClear, uint32_t(b.clears.size() - 1));
  else
    b.clears.pop_back();
  q.AddHook(Ticks() - t0);
}

void NativeRenderer::QueueResolve(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = Ticks();
  profile::Scope scope(profile::kCapRecord);
  Queue& q = *queue_;
  Queue::Batch& b = *q.current;
  ResolveParams& p = b.resolves.emplace_back();
  if (DecodeResolve(base, args, &p))
    q.Push(Queue::kResolve, uint32_t(b.resolves.size() - 1));
  else
    b.resolves.pop_back();  // logged once per reason by DecodeResolve
  q.AddHook(Ticks() - t0);
}

void NativeRenderer::QueueSwap(uint8_t* base, const HookArgs& args) {
  // The render thread failed a frame (Fail): what was queued since is dropped.
  if (g_failed) [[unlikely]]
    return HandOver(base, args.device);
  const uint64_t t0 = NowNs();
  Queue& q = *queue_;
  Queue::Batch& b = *q.current;
  Queue::SwapCmd& s = b.swaps.emplace_back();
  s.guest_begin_ns = q.begin_ns;
  s.guest_swap_ns = t0;
  s.pace_wait_ns = q.pace_wait_ns;
  Presenter::ReadSwap(base, args, &s.call);
  // Captures with the Xenos copy: from the Swap that arms one (HoldFullReadback) to the one
  // captured, the next shown (DrawRenderer::FrameIncomplete holds frames back), every Swap runs
  // before the guest continues, as without the render thread.
  bool sync = q.capture_armed, requested = false;
  if (sync) {
    s.call.capture_request = 0;
  } else {
    requested = present_->CaptureRequested(q.frame);
    s.call.capture_request = requested ? 1 : 0;
    sync = requested && present_->capture_holds_guest();
  }
  s.prep = draw_->EndFramePrepare(q.gpu_frames_done.load(std::memory_order_acquire));
  s.log_frames = requested ? uint32_t(std::max(0, REXCVAR_GET(pgr4_native_mark_audit))) : 0;
  if (s.log_frames) draw_->StartAudit(s.log_frames);
  // At most one frame ahead: frame F is queued once the render thread finished F - 1.
  const uint64_t wait = q.WaitFrames(q.frame);
  q.hook_wait_ns += wait;
  s.state_fail = q.state_fail;
  s.hook_wait_ns = q.hook_wait_ns;
  const uint64_t t1 = NowNs();
  s.hook_ns = q.hook_ns + TicksToNs(q.hook_ticks) + (t1 - t0);
  s.hook_max_ns = std::max({q.hook_max_ns, TicksToNs(q.hook_max_ticks), t1 - t0 - wait});
  s.track_calls = profile::g.track_calls;
  s.pad = rex_xam_take_frame_pad();
  const bool hand_over = s.call.hand_over = SwitchRequested();
  std::memcpy(s.ticks, profile::g.ticks, sizeof(s.ticks));
  for (uint64_t& t : profile::g.ticks) t = 0;
  profile::g.track_calls = 0;
  q.state_fail = q.hook_ns = q.hook_wait_ns = q.hook_ticks = q.hook_max_ticks = 0;
  q.Push(Queue::kSwap, uint32_t(b.swaps.size() - 1));
  q.Publish();
  ++q.frame;
  // Handing over and a capture's wait count towards the next frame.
  const uint64_t capture_wait = sync ? q.WaitFrames(q.frame) : 0;
  // The render thread has run this Swap: the capture is taken, or waits for a shown frame.
  if (sync) q.capture_armed = present_->capture_holding_guest();
  const uint64_t pace_wait = pacer_ ? pacer_->WaitToBegin() : 0;
  const uint64_t t2 = NowNs();
  // The pacer's wait is the gap between two frames, not time in the hook.
  const uint64_t tail = t2 - t1 - pace_wait;
  q.hook_wait_ns = capture_wait;
  q.hook_ns = tail;
  q.hook_max_ns = tail - capture_wait;
  q.begin_ns = t2;
  q.pace_wait_ns = pace_wait;
  if (hand_over) HandOver(base, args.device);
}

// ---------------------------------------------------------------------------------------------
// Hook entry points.

void NativeRenderer::OnDraw(const uint8_t* base, const HookArgs& args) {
  if (queue_) return QueueDraw(base, args);
  if (g_failed) [[unlikely]]
    return;
  profile::Begin();
  DrawState state;
  const bool ok = GuestState().ReadDrawState(base, args, &state);
  profile::Mark(profile::kReadState);
  if (ok)
    ExecuteDraw(state, nullptr);
  else
    ++stats_.state_fail;
}

void NativeRenderer::OnClear(const uint8_t* base, const HookArgs& args) {
  if (queue_) return QueueClear(base, args);
  if (g_failed) [[unlikely]]
    return;
  if (ClearCall c; RenderTargets::DecodeClear(base, args, &c)) ExecuteClear(c);
}

void NativeRenderer::OnResolve(const uint8_t* base, const HookArgs& args) {
  if (queue_) return QueueResolve(base, args);
  if (g_failed) [[unlikely]]
    return;
  if (ResolveParams p; DecodeResolve(base, args, &p)) ExecuteResolve(p);
}

void NativeRenderer::OnSwap(uint8_t* base, const HookArgs& args) {
  if (queue_) return QueueSwap(base, args);
  if (g_failed) [[unlikely]]
    return HandOver(base, args.device);
  stats_.guest_swap_ns = NowNs();
  SwapCall swap;
  Presenter::ReadSwap(base, args, &swap);
  stats_.pad = rex_xam_take_frame_pad();
  swap.hand_over = SwitchRequested();
  ExecuteSwap(swap);
  // stats_ is the next frame's from here.
  stats_.pace_wait_ns = pacer_ ? pacer_->WaitToBegin() : 0;
  stats_.guest_begin_ns = NowNs();
  if (swap.hand_over || g_failed) HandOver(base, args.device);
}

void NativeRenderer::OnQueryIssue(const uint8_t* base, const HookArgs& args) {
  // D3DQUERYTYPE_OCCLUSION at query+4, and D3DISSUE_END / D3DISSUE_BEGIN; QueryIssue takes BEGIN
  // when both are set.
  constexpr uint32_t kTypeOcclusion = 9, kIssueEnd = 1, kIssueBegin = 2;
  if (!occlusion_ || g_failed) return;
  const uint32_t query = args.r[0], flags = args.r[1];
  if (!query || Load32(base, query + 4) != kTypeOcclusion || !(flags & (kIssueBegin | kIssueEnd)))
    return;
  const QueryCall c = {query, !(flags & kIssueBegin)};
  if (!queue_) return ExecuteQuery(c);
  Queue::Batch& b = *queue_->current;
  b.queries.push_back(c);
  queue_->Push(Queue::kQuery, uint32_t(b.queries.size() - 1));
}

void NativeRenderer::OnQueryGetData(uint8_t* base, const HookArgs& args) {
  if (occlusion_) occlusion_->WriteResult(base, args.r[0]);
}

void NativeRenderer::OnSwapSuspended() {
  if (SwitchRequested()) TakeBack();
}

// ---------------------------------------------------------------------------------------------
// Renderer switch (primary mode)

void NativeRenderer::HandOver(uint8_t* base, uint32_t device) {
  // Beside the Xenos plugin (debug window) it never stopped drawing; Fail cleared live_ already.
  if (!primary_) return;
  // The replaced draws (guest_d3d.h) cleared dirty bits without writing the state to the
  // stream, and the plugin skipped the shader loads: it has neither. The next draw writes all
  // of it.
  MarkAllDirty(base, device);
  rex::cvar::SetFlagByName("gpu_skip_draws", "false");
  if (occlusion_) rex::cvar::SetFlagByName("occlusion_query_external", "false");
  // The plugin presents on its own: the game waits for its vblanks again.
  paces_frames_.store(false, std::memory_order_relaxed);
  live_.store(nullptr, std::memory_order_relaxed);
  if (g_failed)
    REXLOG_INFO("PGR4 native: the Xenos plugin draws for the rest of the run");
  else
    REXLOG_INFO("PGR4 native: renderer switch: the Xenos plugin draws");
}

void NativeRenderer::TakeBack() {
  // The tracker saw none of the calls the plugin drew. The next native frame's ExecuteSwap
  // takes the game window back (OwnGameWindow), and the pacing with it.
  GuestState().Resync();
  rex::cvar::SetFlagByName("gpu_skip_draws", "true");
  if (occlusion_) rex::cvar::SetFlagByName("occlusion_query_external", "true");
  live_.store(this, std::memory_order_release);
  REXLOG_INFO("PGR4 native: renderer switch: native draws");
}

// ---------------------------------------------------------------------------------------------
// Recording (render thread, or the guest thread without it).

void NativeRenderer::ExecuteDraw(const DrawState& state, const DrawPrep* prep) {
  const uint64_t t0 = Ticks();
  profile::Begin();
  BeginFrame();
  ++stats_.draws;
  if (gpu_timer_) {
    uint64_t key;
    if (gpu_timer_->per_shader() && prep && prep->vs) {
      const auto [vs, ps] = DrawRenderer::ShaderHashes(*prep);
      key = gpu_timer_->KeyDrawShaders(state.regs, vs, ps);
    } else {
      key = GpuTimer::KeyDraw(state.regs);
    }
    gpu_timer_->Pass(device_->command_list(), key);
  }
  draw_->OnDraw(state, prep);
  SubmitEarly();
  draw_ticks_ += Ticks() - t0;
}

void NativeRenderer::ExecuteClear(const ClearCall& c) {
  const uint64_t t0 = Ticks();
  BeginFrame();
  ++stats_.clears;
  if (gpu_timer_) gpu_timer_->Pass(device_->command_list(), GpuTimer::KeyClear(c));
  draw_->LogClear(c);
  rt_->Clear(device_->command_list(), c.regs, c.bound_colors, c.depth_bound, c.height, c.p);
  clear_ticks_ += Ticks() - t0;
}

void NativeRenderer::ExecuteResolve(const ResolveParams& p) {
  const uint64_t t0 = Ticks();
  BeginFrame();
  ++stats_.resolves;
  if (gpu_timer_) gpu_timer_->Pass(device_->command_list(), GpuTimer::KeyResolve(p));
  draw_->LogResolve(p);
  // Sets its own compute root signature, pipeline and descriptor heap on the list.
  resolver_->Resolve(device_->command_list(), p);
  draw_->InvalidateListState();
  resolve_ticks_ += Ticks() - t0;
}

void NativeRenderer::ExecuteQuery(const QueryCall& q) {
  if (q.end)
    occlusion_->End(q.query, device_->frame_fence());
  else
    occlusion_->Begin(q.query);
}

// With one submission per frame the GPU idles from the end of a frame until the render thread
// has recorded all of the next one. Every --pgr4_native_submit_draws draws, the frame so far is
// submitted and recording goes on in a fresh list, where nothing bound carries over. The texture
// copies recorded so far read upload memory the untile workers fill: while untiles are still
// running, the next draw or resolve tries again.
void NativeRenderer::SubmitEarly() {
  const int32_t every = REXCVAR_GET(pgr4_native_submit_draws);
  if (every <= 0 || stats_.draws - draws_submitted_ < uint64_t(every) || !textures_->UploadsDone())
    return;
  profile::Scope scope(profile::kSubmit);
  if (!CloseUploadLists() || !device_->Submit(upload_lists_)) return Fail();
  draw_->InvalidateListState(DrawRenderer::ListBreak::kSubmit);
  draws_submitted_ = stats_.draws;
  ++stats_.submits;
}

bool NativeRenderer::CloseUploadLists() {
  profile::Scope scope(profile::kUploadList);
  upload_lists_.clear();
  return textures_->CloseUploadLists(&upload_lists_);
}

void NativeRenderer::Fail() {
  g_failed = true;
  paces_frames_.store(false, std::memory_order_relaxed);
  if (pacer_) pacer_->Stop();
  REXLOG_ERROR("PGR4 native: frame {} failed; native path off for the rest of the run",
               frame_index_);
  if (!primary_) {
    live_.store(nullptr, std::memory_order_relaxed);
    return;
  }
  // The Xenos plugin skips its draws until the guest thread's next Swap hands the game back
  // (HandOver: it needs the guest device, between two of the guest's D3D calls); live_ stays
  // set so that the hooks reach that Swap.
  ReleaseGameWindow();
}

void NativeRenderer::ReleaseGameWindow() {
  D3D12Device& d = *device_;
  paces_frames_.store(false, std::memory_order_relaxed);
  if (d.has_swapchain()) {
    d.DetachSwapchain();
    if (pacer_) pacer_->Detach();
  }
  if (window_taken_) {
    ConnectSdkPresenter(true);
    window_taken_ = false;
    REXLOG_INFO("PGR4 native: the game window shows ReXGlue's presenter");
  }
}

bool NativeRenderer::OwnGameWindow(bool own) {
  D3D12Device& d = *device_;
  if (own == d.has_swapchain()) return true;
  if (!own) {
    ReleaseGameWindow();
    return true;
  }
  ConnectSdkPresenter(false);
  window_taken_ = true;
  if (!d.AttachSwapchain()) return false;
  if (pacer_) {
    HANDLE waitable = d.TakeFrameLatencyWaitable(FramePacer::kMaxFrameLatency);
    if (!waitable) return false;
    pacer_->Attach(waitable, d.fence());
    paces_frames_.store(pacer_->pacing(), std::memory_order_relaxed);
  }
  return true;
}

void NativeRenderer::ExecuteSwap(const SwapCall& swap) {
  const uint64_t t0 = NowNs();
  BeginFrame();
  D3D12Device& d = *device_;
  if (gpu_timer_) gpu_timer_->Pass(d.command_list(), GpuTimer::KeyPresent());
  // A scene's first frame without the draws whose pipelines were compiling: the previous frame
  // stays on screen while the next one waits for them.
  const bool incomplete = draw_->FrameIncomplete();
  if (incomplete) ++stats_.frames_held;
  if (primary_ && !OwnGameWindow(!swap.hand_over && !rex::kernel::xam::xeXamIsUIActive()))
    return Fail();
  const bool shown = !incomplete && d.has_swapchain();
  // The resolved front buffer into the back buffer (PRESENT in and out).
  present_->OnSwap(swap, shown);
  // The frame's texture copies read upload memory the untile workers fill.
  const uint64_t u0 = NowNs();
  textures_->FinishUploads();
  stats_.swap_untile_ns = NowNs() - u0;
  draw_->BeforeSubmit();
  const uint64_t fence = d.frame_fence();
  if (occlusion_) occlusion_->Resolve(d.command_list(), fence);
  if (!CloseUploadLists() || !d.EndFrame(shown, upload_lists_)) return Fail();
  const D3D12Device::EndFrameTimes& et = d.end_frame_times();
  stats_.swap_submit_ns = et.submit_ns;
  stats_.swap_present_ns = et.present_ns;
  stats_.swap_fence_ns = et.fence_ns;
  stats_.present = et.presented ? et.presents : 0;
  if (pacer_ && et.presented)
    pacer_->Presented(fence);
  else if (pacer_)
    pacer_->RetireUnshown();
  ++frame_index_;
  frame_begun_ = false;
  d.SetTitleFrame(frame_index_);
  const uint64_t completed = d.completed_fence();
  if (queue_) {
    // Frees the guest thread's vertex upload memory of finished frames (Prepare's tags).
    Queue& q = *queue_;
    q.frame_fences.emplace_back(frame_index_, fence);
    while (!q.frame_fences.empty() && q.frame_fences.front().second <= completed) {
      q.gpu_frames_done.store(q.frame_fences.front().first, std::memory_order_release);
      q.frame_fences.pop_front();
    }
  }
  textures_->Retire(completed);
  if (occlusion_) occlusion_->Retire(completed);
  draw_->EndFrame(completed);
  resolver_->AfterSwap();
  present_->AfterEndFrame();
  const uint64_t t1 = NowNs();
  stats_.swap_ns = t1 - t0;
  stats_.draw_ns = TicksToNs(draw_ticks_);
  stats_.clear_ns = TicksToNs(clear_ticks_);
  stats_.resolve_ns = TicksToNs(resolve_ticks_);
  draw_ticks_ = clear_ticks_ = resolve_ticks_ = 0;
  if (!queue_)
    stats_.hook_ns = stats_.draw_ns + stats_.clear_ns + stats_.resolve_ns + stats_.swap_ns;
  WriteCsvRow(stats_, t1 - frame_start_ns_);
  AddToStatsLog(stats_, t1 - frame_start_ns_);
  ResetFrameCounters();
  frame_start_ns_ = t1;
  stats_ = FrameStats{};
  draws_submitted_ = 0;
}

void NativeRenderer::AppendCsvColumns(const FrameStats& s, uint64_t frame_ns, bool header,
                                      std::string* line) {
  const DrawRenderer::Stats& ds = draw_->stats();
  const TextureCache::Stats& ts = textures_->stats();
  const CsvCell cells[] = {
      Uint("frame", frame_index_ - 1),
      Uint("draws", s.draws),
      Uint("clears", s.clears),
      Uint("resolves", s.resolves),
      Uint("state_fail", s.state_fail),
      Fix1("draw_us", s.draw_ns / 1000.0),
      Fix1("clear_us", s.clear_ns / 1000.0),
      Fix1("resolve_us", s.resolve_ns / 1000.0),
      Fix1("swap_us", s.swap_ns / 1000.0),
      Fix4("us_per_draw", s.draws ? double(s.draw_ns) / 1000.0 / double(s.draws) : 0.0),
      Fix3("frame_ms", frame_ns / 1e6),
      Uint("host_draws", ds.draws),
      Uint("skipped", ds.skipped),
      Uint("unsupported", ds.unsupported),
      Uint("failed", ds.failed),
      Uint("pso_created", ds.pso_created),
      Fix1("upload_kb", ds.upload_bytes / 1024.0),
      Uint("textures", ds.textures),
      Uint("textures_missing", ds.textures_missing),
      Uint("front_buffer", present_->front_buffer_found() ? 1 : 0),
      Fix1("hook_us", s.hook_ns / 1000.0),
      Fix1("hook_wait_us", s.hook_wait_ns / 1000.0),
      Uint("reg_writes", s.reg_writes),
      Fix1("swap_untile_us", s.swap_untile_ns / 1000.0),
      Fix1("swap_submit_us", s.swap_submit_ns / 1000.0),
      Fix1("swap_present_us", s.swap_present_ns / 1000.0),
      Fix1("swap_fence_us", s.swap_fence_ns / 1000.0),
      Fix1("hook_max_us", s.hook_max_ns / 1000.0),
      Uint("watch_faults", ds.watch_faults),
      Fix3("gpu_prev_ms", gpu_timer_ ? gpu_timer_->last_frame_ms() : 0.0),
      Uint("copy_batches", ds.copy_batches),
      Uint("watch_arms", ds.watch_arms),
      Uint("watch_arm_pages", ds.watch_arm_pages),
      Uint("watch_arm_changed", ds.watch_arm_changed),
      Fix1("watch_arm_us", ds.watch_arm_ns / 1000.0),
      Fix3("t_ms", NowNs() / 1e6),
      Uint("hash_pages", ds.hash_pages),
      Uint("hash_copies", ds.hash_copies),
      Uint("pso_pending", ds.pso_pending),
      Uint("pso_wait_us", ds.pso_wait_us),
      Uint("held", s.frames_held),
      Uint("tex_stale", ds.textures_stale),
      Uint("coh_stale", ds.stale_pages),
      Uint("coh_late", ds.late_pages),
      Uint("coh_gpu", ds.gpu_mismatch_pages),
      Uint("coh_regs", ds.reg_mismatches),
      Fix1("tex_mb", textures_->resident_bytes() / 1048576.0),
      Uint("tex_count", textures_->texture_count()),
      Uint("tex_evicted", ts.evicted - csv_->evicted_prev),
      Fix1("tex_evict_us", (ts.evict_ns - csv_->evict_ns_prev) / 1000.0),
      Uint("views", ds.views),
      Uint("pso_generic", ds.pso_generic),
      // rex_xam_take_frame_pad's layout.
      Hex4("pad_buttons", s.pad & 0xFFFF),
      Uint("pad_lt", uint8_t(s.pad >> 16)),
      Uint("pad_rt", uint8_t(s.pad >> 24)),
      Int("pad_lx", int8_t(s.pad >> 32)),
      Int("pad_ly", int8_t(s.pad >> 40)),
      Int("pad_rx", int8_t(s.pad >> 48)),
      Int("pad_ry", int8_t(s.pad >> 56)),
      Uint("submits", s.submits),
      Fix3("guest_begin_ms", s.guest_begin_ns / 1e6),
      Fix3("guest_swap_ms", s.guest_swap_ns / 1e6),
      Fix1("pace_wait_us", s.pace_wait_ns / 1000.0),
      Uint("present", s.present),
  };
  AppendCsvCells(cells, header, false, line);
}

void NativeRenderer::AppendCsvProfileColumns(bool header, std::string* line) {
  for (uint32_t i = 0; i < profile::kStageCount; ++i) {
    if (header)
      *line += std::string(",st_") + profile::kStageNames[i] + "_us";
    else
      AppendCsvCell(Fix1("", double(TicksToNs(profile::g.ticks[i])) / 1000.0), false, true, line);
  }
  const DrawRenderer::Stats& ds = draw_->stats();
  const TextureCache::Stats& t = textures_->stats();
  const TextureCache::Stats& p = csv_->tex_prev;
  const CsvCell cells[] = {
      Uint("track_calls", profile::g.track_calls),
      Uint("tex_memo_miss", t.memo_misses - p.memo_misses),
      Uint("tex_created", t.created - p.created),
      Fix1("tex_create_us", (t.create_ns - p.create_ns) / 1000.0),
      Uint("tex_uploads", t.uploads - p.uploads),
      Uint("tex_deferred", t.deferred_uploads - p.deferred_uploads),
      Fix1("tex_upload_kb", (t.upload_bytes - p.upload_bytes) / 1024.0),
      Fix1("tex_upload_us", (t.upload_ns - p.upload_ns) / 1000.0),
      Fix1("tex_untile_us", (t.untile_ns - p.untile_ns) / 1000.0),
      Fix1("tex_record_us", (t.upload_record_ns - p.upload_record_ns) / 1000.0),
      Uint("tex_resolved_copies", t.resolved_copies - p.resolved_copies),
      Fix1("tex_copy_resolved_us", (t.copy_resolved_ns - p.copy_resolved_ns) / 1000.0),
      Uint("tex_resolved_aliases", t.resolved_aliases - p.resolved_aliases),
      Uint("srv_lookups", ds.srv_lookups),
      Uint("srv_created", ds.srv_created),
      Uint("samplers_created", ds.samplers_created),
      Uint("bind_hits", ds.bind_memo_hits),
      Uint("bind_misses", ds.bind_memo_misses),
  };
  AppendCsvCells(cells, header, true, line);
  for (uint32_t i = 0; i < profile::kCounterCount; ++i)
    AppendCsvCell(Uint(profile::kCounterNames[i], profile::g.counters[i]), header, true, line);
}

void NativeRenderer::AppendCsvLumaColumns(bool header, std::string* line) {
  const LumaProbe& l = present_->luma_probe();
  AppendCsvCell(Uint("luma_frame", l.frame), header, true, line);
  static constexpr const char* kRowNames[LumaProbe::kRows] = {"top", "mid", "low"};
  for (uint32_t row = 0; row < LumaProbe::kRows; ++row) {
    for (uint32_t s = 0; s < LumaProbe::kSegments; ++s) {
      if (header)
        *line += ",luma_" + std::string(kRowNames[row]) + "_" + std::to_string(s);
      else
        AppendCsvCell(Fix1("", l.luma[row][s]), false, true, line);
    }
  }
}

void NativeRenderer::WriteCsvRow(const FrameStats& s, uint64_t frame_ns) {
  Csv& c = *csv_;
  if (!c.file) return;
  c.line.clear();
  AppendCsvColumns(s, frame_ns, false, &c.line);
  c.evicted_prev = textures_->stats().evicted;
  c.evict_ns_prev = textures_->stats().evict_ns;
  if (profile::On()) {
    AppendCsvProfileColumns(false, &c.line);
    c.tex_prev = textures_->stats();
  }
  if (c.luma) AppendCsvLumaColumns(false, &c.line);
  c.line += '\n';
  c.file->Append(c.line);
}

void NativeRenderer::AddToStatsLog(const FrameStats& s, uint64_t frame_ns) {
  Csv& w = *csv_;
  ++w.frames;
  w.draws += s.draws;
  w.draw_ns += s.draw_ns;
  w.swap_ns += s.swap_ns;
  w.frame_ns += frame_ns;
  w.hook_ns += s.hook_ns;
  w.hook_wait_ns += s.hook_wait_ns;
  if (profile::On()) {
    for (uint32_t i = 0; i < profile::kStageCount; ++i) w.stage_ticks[i] += profile::g.ticks[i];
    w.track_calls += profile::g.track_calls;
  }
  const int32_t every = REXCVAR_GET(pgr4_d3d_stats_frames);
  if (every <= 0 || w.frames < uint64_t(every)) return;
  const double n = double(w.frames);
  REXLOG_INFO(
      "PGR4 native: frames {} | per frame: draws {:.1f}, draw CPU {:.1f} us "
      "({:.4f} us/draw), swap {:.1f} us, frame {:.2f} ms, game thread {:.1f} us "
      "(waiting {:.1f} us)",
      frame_index_, w.draws / n, w.draw_ns / 1000.0 / n,
      w.draws ? w.draw_ns / 1000.0 / double(w.draws) : 0.0, w.swap_ns / 1000.0 / n,
      w.frame_ns / 1e6 / n, w.hook_ns / 1000.0 / n, w.hook_wait_ns / 1000.0 / n);
  if (profile::On() && w.draws) {
    // ns per draw for each stage (the hook-side ones are per draw too, for comparison).
    std::string line;
    char item[64];
    for (uint32_t i = 0; i < profile::kStageCount; ++i) {
      std::snprintf(item, sizeof(item), " %s %.0f", profile::kStageNames[i],
                    double(TicksToNs(w.stage_ticks[i])) / double(w.draws));
      line += item;
    }
    REXLOG_INFO("PGR4 native profile: ns/draw{} | track calls/frame {:.0f}", line,
                double(w.track_calls) / n);
    for (uint64_t& t : w.stage_ticks) t = 0;
    w.track_calls = 0;
  }
  if (gpu_timer_) {
    gpu_timer_->Log();
    for (uint64_t hash : gpu_timer_->TakeNewTopShaders())
      REXLOG_INFO("PGR4 native GPU: shader {:016X} ucode:\n{}", hash,
                  draw_->ShaderDisassembly(hash));
  }
  w.frames = w.draws = w.draw_ns = w.swap_ns = w.frame_ns = w.hook_ns = w.hook_wait_ns = 0;
}

void NativeRenderer::ResetFrameCounters() {
  draw_->ResetStats();
  if (profile::On()) {
    for (uint64_t& t : profile::g.ticks) t = 0;
    profile::g.track_calls = 0;
    for (uint64_t& c : profile::g.counters) c = 0;
  }
}
}  // namespace pgr4::native
