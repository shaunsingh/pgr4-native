// pgr4-native PGR4 native renderer: guest D3D API hooks and frame trace.
//
// Every function in d3d_surface.inc (the XDK D3D entry points PGR4's game code calls, plus the
// internal draw builders and MakeSpace) is overridden with a REX_HOOK_RAW that forwards to the
// original __imp__sub_X and counts the call (the pass-through ones only while the tap or the
// profiler is on). For debugging and oracle comparisons, forwarding can be switched off per class
// at run time (`skip draw|clear|resolve|none`). The native renderer as the presenter
// (--pgr4_native_primary) does not use it: it keeps forwarding and stops the Xenos plugin's host
// draws with the SDK's --gpu_skip_draws (patches/0031).
//
// With --pgr4_d3d_tap the hooks also follow the D3D push buffer: the D3D device keeps its write
// pointer at device+48 (last written dword) and its limit at device+56, and every packet the
// library emits is written there before the GPU sees it. At the entry and exit of each device
// call the new dwords are decoded (PM4 types 0-3), so register, constant, shader and draw state
// is known per API call, without running or parsing the GPU side. Each draw packet is counted
// and, on traced frames, written with its shaders, fetch constants, render-target registers and
// a cross-check of the decoded registers against the device's own register shadow.
//
// Control at run time: --pgr4_d3d_control=<file>. The file is read (then deleted) once per
// frame, one command per line:
//   trace <tag> [frames]   write the next N complete frames to --pgr4_d3d_trace_dir (ignored
//                          while the tap is off)
//   skip <draw,clear,resolve|none>
//   tap on|off             stop or resume decoding (hooks keep counting and forwarding); off
//                          also drops the frames a trace has still to write
// --pgr4_d3d_tap, --pgr4_d3d_tap_check and --pgr4_d3d_skip set the state at the first device the
// game creates; the one it creates at the title screen keeps what the commands changed since.
//
// With --pgr4_native the draw, clear, resolve and Swap hooks also call the native D3D12
// renderer (src/native/renderer.h) after the guest function returned.
//
// Built only into the PGR4 game project (games/pgr4, scripts/build.sh).

#include "native/clock.h"
#include "native/d3d_state.h"
#include "native/guest_d3d.h"
#include "native/guest_memory.h"
#include "native/log.h"
#include "native/profile.h"
#include "native/renderer.h"

#include <rex/cvar.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/xenos.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_BOOL(pgr4_d3d_tap, false, "PGR4 native",
                    "Decode the guest D3D push buffer at each hooked D3D call (trace/oracle)");
REXCVAR_DEFINE_STRING(pgr4_d3d_control, "", "PGR4 native",
                      "Command file polled once per frame (trace/skip/tap commands)");
REXCVAR_DEFINE_STRING(pgr4_d3d_trace_dir, "", "PGR4 native", "Directory for frame traces");
REXCVAR_DEFINE_STRING(pgr4_d3d_skip, "", "PGR4 native",
                      "Classes not forwarded to the guest D3D at start: draw,clear,resolve");
REXCVAR_DEFINE_STRING(pgr4_d3d_frame_csv, "", "PGR4 native",
                      "Per-frame counter CSV (frame, calls, draw calls, tap draws, ...)");
REXCVAR_DECLARE(bool, pgr4_native_profile);
REXCVAR_DEFINE_INT32(pgr4_d3d_stats_frames, 150, "PGR4 native",
                     "Frames per summary line in the log (0 = off)");
REXCVAR_DEFINE_BOOL(pgr4_d3d_tap_check, false, "PGR4 native",
                    "With --pgr4_d3d_tap: compare the tracked state with the stream at every draw "
                    "of every frame and log the frames that differ (slow)");

namespace {

namespace ucode = rex::graphics::ucode;
namespace xenos = rex::graphics::xenos;

enum Cat : uint8_t {
  kDraw,
  kResolve,
  kSwap,
  kPush,
  kSync,
  kConst,
  kShader,
  kTexture,
  kStream,
  kSurface,
  kState,
  kGet,
  kClear,
  kTiling,
  kQuery,
  kDevice,
  kResource,
  kOther,
};

enum FnId : uint32_t {
#define D3D_FN(a, n, c) kFn_##a,
#include "d3d_surface.inc"
#undef D3D_FN
  kFnCount
};

struct FnInfo {
  uint32_t addr;
  const char* name;
  Cat cat;
};

constexpr FnInfo kFns[kFnCount] = {
#define D3D_FN(a, n, c) {0x##a##u, n, c},
#include "d3d_surface.inc"
#undef D3D_FN
};

// Entry points the dispatch handles by name (d3d_surface.inc names every hooked one).
constexpr FnId kFnSwap = kFn_826CB148, kFnResolve = kFn_826CBEE0, kFnCreateDevice = kFn_826CD270,
               kFnRestoreSegment13392 = kFn_826CD380, kFnRestoreSegment = kFn_826CD868,
               kFnStartSegment = kFn_826CE4E0, kFnSwitchSegment = kFn_826CE738,
               kFnMakeSpace = kFn_826CE8C0, kFnBeginVertices = kFn_826CFA60,
               kFnRestorePush = kFn_826CFF00, kFnDrawVerticesUP = kFn_826CFF10,
               kFnDrawVertices = kFn_826CFF58, kFnDrawIndexed = kFn_826D0340,
               kFnClear = kFn_826D1A08, kFnBeginTiling = kFn_826D1C38, kFnEndTiling = kFn_826D20D0;

// The five draw builders, which every game draw goes through.
constexpr FnId kDrawCounterFns[] = {kFnBeginVertices, kFnDrawVertices, kFnDrawIndexed, kFn_826D0798,
                                    kFn_826DC888};
constexpr bool IsDrawCounterFn(FnId id) {
  for (FnId d : kDrawCounterFns)
    if (d == id) return true;
  return false;
}
// The calls rex_drawcount_api_counters reports: the draw builders and Resolve.
constexpr bool IsCountedFn(FnId id) { return IsDrawCounterFn(id) || id == kFnResolve; }

// Skippable entry points: void, and nothing in the caller depends on their result.
enum Skip : uint32_t { kSkipDraw = 1, kSkipClear = 2, kSkipResolve = 4 };
constexpr uint32_t SkipClassOf(FnId id) {
  switch (id) {
    case kFnDrawIndexed:
    case kFnDrawVertices:
    case kFnDrawVerticesUP:
      return kSkipDraw;
    case kFnClear:
      return kSkipClear;
    case kFnResolve:
      return kSkipResolve;
    default:
      return 0;
  }
}

// The game's time step: the time between two frames' begins, which the Swap hook returns at
// (after the frame pacer's wait in the native renderer, the plugin's vblank wait otherwise); the
// game reads its timer just after. It draws a frame while it runs the next, so the frame it draws
// moved by the step before the last: that one matches the game's within 0.11 ms in the median and
// 1.4 ms in 99 % of the frames at 90 fps (td-125). NoteFrameBegin runs on the thread that calls
// Swap; the motion blur pass reads g_drawn_step_ns.
uint64_t g_frame_begin_ns = 0;
int64_t g_frame_step_ns = 0;
std::atomic<int64_t> g_drawn_step_ns{0};

void NoteFrameBegin() {
  const uint64_t now = pgr4::native::NowNs();
  g_drawn_step_ns.store(g_frame_step_ns, std::memory_order_relaxed);
  if (g_frame_begin_ns) g_frame_step_ns = int64_t(now - g_frame_begin_ns);
  g_frame_begin_ns = now;
}

constexpr uint32_t kRegCount = pgr4::native::kGuestRegisterCount;
namespace dev = pgr4::native::dev;
// First register of each constant kind in SET_CONSTANT and LOAD_ALU_CONSTANT: ALU (float),
// fetch, bool, loop, register.
constexpr uint32_t kConstantBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};

using pgr4::native::Load32;  // the D3D's push buffer can live in the 0xE0000000 view

uint64_t Fnv(const uint32_t* d, size_t n, uint64_t h = 1469598103934665603ull) {
  for (size_t i = 0; i < n; ++i) {
    h ^= d[i];
    h *= 1099511628211ull;
  }
  return h;
}

const char* RegName(uint32_t r) {
  static const std::unordered_map<uint32_t, const char*> names = [] {
    std::unordered_map<uint32_t, const char*> m;
#define XE_GPU_REGISTER(index, type, name) m.emplace(index, #name);
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
    return m;
  }();
  auto it = names.find(r);
  return it == names.end() ? nullptr : it->second;
}

const char* OpName(uint32_t op) {
  switch (op) {
    case 0x10:
      return "NOP";
    case 0x21:
      return "REG_RMW";
    case 0x22:
      return "DRAW_INDX";
    case 0x23:
      return "VIZ_QUERY";
    case 0x26:
      return "WAIT_FOR_IDLE";
    case 0x27:
      return "IM_LOAD";
    case 0x2B:
      return "IM_LOAD_IMMEDIATE";
    case 0x2D:
      return "SET_CONSTANT";
    case 0x2F:
      return "LOAD_ALU_CONSTANT";
    case 0x34:
      return "DRAW_INDX_BIN";
    case 0x35:
      return "DRAW_INDX_2_BIN";
    case 0x36:
      return "DRAW_INDX_2";
    case 0x37:
      return "INDIRECT_BUFFER_PFD";
    case 0x3B:
      return "INVALIDATE_STATE";
    case 0x3C:
      return "WAIT_REG_MEM";
    case 0x3D:
      return "MEM_WRITE";
    case 0x3E:
      return "REG_TO_MEM";
    case 0x3F:
      return "INDIRECT_BUFFER";
    case 0x44:
      return "COND_EXEC";
    case 0x45:
      return "COND_WRITE";
    case 0x46:
      return "EVENT_WRITE";
    case 0x48:
      return "ME_INIT";
    case 0x4A:
      return "SET_SHADER_BASES";
    case 0x4B:
      return "SET_BIN_BASE_OFFSET";
    case 0x50:
      return "SET_BIN_MASK";
    case 0x51:
      return "SET_BIN_SELECT";
    case 0x52:
      return "WAIT_REG_EQ";
    case 0x53:
      return "WAIT_REG_GTE";
    case 0x54:
      return "INTERRUPT";
    case 0x55:
      return "SET_CONSTANT2";
    case 0x56:
      return "SET_SHADER_CONSTANTS";
    case 0x58:
      return "EVENT_WRITE_SHD";
    case 0x59:
      return "EVENT_WRITE_CFL";
    case 0x5A:
      return "EVENT_WRITE_EXT";
    case 0x5B:
      return "EVENT_WRITE_ZPD";
    case 0x5C:
      return "WAIT_UNTIL_READ";
    case 0x5D:
      return "WAIT_IB_PFD_COMPLETE";
    case 0x5E:
      return "CONTEXT_UPDATE";
    case 0x60:
      return "SET_BIN_MASK_LO";
    case 0x61:
      return "SET_BIN_MASK_HI";
    case 0x62:
      return "SET_BIN_SELECT_LO";
    case 0x63:
      return "SET_BIN_SELECT_HI";
    case 0x64:
      return "XE_SWAP";
    default:
      return nullptr;
  }
}

// Fetch constants a shader reads (texture fetch slots, vertex fetch constants).
struct ShaderInfo {
  uint32_t type = 0, addr = 0, dwords = 0;
  uint64_t hash = 0;
  uint32_t tfetch_mask = 0;  // bit i: tf[i]
  uint64_t vfetch_lo = 0;    // bit i: vf[i], i < 64
  uint32_t vfetch_hi = 0;    // bit i: vf[64 + i]
  uint32_t uses = 0;
};

void ScanUcode(const uint32_t* host_le, uint32_t dwords, ShaderInfo& info) {
  uint32_t first_exec = dwords / 3;
  for (uint32_t pair = 0; pair * 3 + 3 <= dwords && pair < first_exec; ++pair) {
    ucode::ControlFlowInstruction cf[2];
    ucode::UnpackControlFlowInstructions(host_le + pair * 3, cf);
    bool end = false;
    for (auto& c : cf) {
      auto op = c.opcode();
      if (!ucode::IsControlFlowOpcodeExec(op)) continue;
      uint32_t addr = c.exec.address(), count = c.exec.count(), seq = c.exec.sequence();
      if (count) first_exec = std::min(first_exec, addr);
      for (uint32_t k = 0; k < count; ++k) {
        if (!((seq >> (k * 2)) & 1)) continue;  // ALU
        uint32_t at = (addr + k) * 3;
        if (at + 3 > dwords) break;
        ucode::FetchInstruction fi;
        std::memcpy(&fi, host_le + at, sizeof(fi));
        if (fi.opcode() == ucode::FetchOpcode::kVertexFetch) {
          uint32_t vf = fi.vertex_fetch().fetch_constant_index();
          if (vf < 64)
            info.vfetch_lo |= 1ull << vf;
          else if (vf < 96)
            info.vfetch_hi |= 1u << (vf - 64);
        } else {
          info.tfetch_mask |= 1u << (fi.texture_fetch().fetch_constant_index() & 31);
        }
      }
      end |= ucode::DoesControlFlowOpcodeEndShader(op);
    }
    if (end) break;
  }
}

struct FrameCounters {
  uint64_t calls = 0, draw_calls = 0;  // hooked calls; DrawCounter's five builders
  uint64_t tap_draws = 0, tap_aux = 0, tap_resolves = 0;  // predicated / other draw packets
  uint64_t dwords = 0, segments = 0, resyncs = 0, torn = 0, unknown_ops = 0, ibs = 0, zero_pad = 0;
  uint64_t checked = 0, differ = 0;  // --pgr4_d3d_tap_check: draws compared, draws that differ
};

// Belongs to the thread that drives the guest D3D device: the game uses the device from one
// thread at a time (renderer.h), and only the hooked calls on that device and CreateDevice, which
// runs before the calls on the device it creates, change this state. Hooked calls on other
// threads (r3 not the device) read `device` and `enabled` only.
struct Tap {
  uint32_t device = 0;
  uint32_t cursor = 0;
  bool enabled = false;
  bool configured = false;  // the cvars were read (at the first CreateDevice)

  // Decoder.
  uint32_t regs[kRegCount] = {};
  uint8_t written[kRegCount] = {};
  std::vector<uint32_t> pkt;
  uint32_t header = 0, need = 0;
  bool in_packet = false;
  uint32_t vs_addr = 0, vs_dwords = 0, ps_addr = 0, ps_dwords = 0;
  uint64_t vs_imm_hash = 0, ps_imm_hash = 0;  // IM_LOAD_IMMEDIATE

  FrameCounters frame;
  uint64_t frame_index = 0;

  // Tracing.
  bool tracing = false;     // the current frame is being written
  uint32_t trace_left = 0;  // frames still to trace after this one
  std::string trace_tag;
  std::string out;
  uint64_t trace_frame = 0, call_seq = 0, draw_seq = 0;
  // Oracle for native/d3d_state: its GPU view vs the decoded stream at every top-level draw.
  uint64_t state_draws = 0, state_bad_draws = 0, state_shader_bad = 0, state_unchecked = 0;
  uint64_t state_details = 0, state_per_tile = 0;
  bool state_pending = false;  // BeginVertices: compared when RestorePush publishes its packets
  bool check = false;          // --pgr4_d3d_tap_check
  std::string check_first;     // the frame's first differing draw
  std::vector<uint64_t> state_regs = std::vector<uint64_t>(kRegCount);
  std::map<uint32_t, uint64_t> op_counts;
  std::map<uint64_t, ShaderInfo> shaders;                      // key: type<<63 | addr<<20 ^ dwords
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> textures;  // (dword1, dword2) -> draws
  uint64_t fn_calls[kFnCount] = {};
};

Tap g_tap;
std::atomic<uint64_t> g_calls[kFnCount];  // IsCountedFn's, for rex_drawcount_api_counters
std::atomic<uint32_t> g_skip{0};
// Pass-through hooks (IsPassThrough) take OnHook's path only while the tap decodes the stream or
// the profiler times the guest D3D; otherwise they jump straight to the original.
std::atomic<bool> g_hook_all{false};
std::atomic<uint64_t> g_tap_counts[3];  // draws, aux, resolves (cumulative, for stream stats)

struct CallFrame {
  FnId id;
  uint64_t seq;
};
thread_local CallFrame t_stack[64];
thread_local uint32_t t_depth = 0;

uint32_t ParseSkip(const std::string& s) {
  uint32_t m = 0;
  if (s.find("draw") != std::string::npos) m |= kSkipDraw;
  if (s.find("clear") != std::string::npos) m |= kSkipClear;
  if (s.find("resolve") != std::string::npos) m |= kSkipResolve;
  return m;
}

void AppendV(std::string& s, const char* f, va_list args) {
  char buf[4096];
  int n = std::vsnprintf(buf, sizeof(buf), f, args);
  if (n > 0) s.append(buf, std::min<size_t>(size_t(n), sizeof(buf) - 1));
}

__attribute__((format(printf, 2, 3))) void Append(std::string& s, const char* f, ...) {
  va_list args;
  va_start(args, f);
  AppendV(s, f, args);
  va_end(args);
}

FnId CurrentFn() { return t_depth ? t_stack[std::min<uint32_t>(t_depth, 64) - 1].id : kFnCount; }
uint64_t CurrentSeq() { return t_depth ? t_stack[std::min<uint32_t>(t_depth, 64) - 1].seq : 0; }

// `how`: 1, or GuestStateTracker::kTapWrittenPredicated for a predicated (per-tile) packet.
void WriteReg(Tap& t, uint32_t reg, uint32_t v, uint8_t how = 1) {
  if (reg >= kRegCount) return;
  t.regs[reg] = v;
  t.written[reg] = how;
}

const ShaderInfo* Shader(Tap& t, uint32_t type, uint32_t addr, uint32_t dwords) {
  if (!addr || !dwords) return nullptr;
  uint64_t key = (uint64_t(type) << 63) | ((uint64_t(addr) << 20) ^ dwords);
  auto it = t.shaders.find(key);
  if (it != t.shaders.end()) return &it->second;
  ShaderInfo info;
  info.type = type;
  info.addr = addr;
  info.dwords = dwords;
  auto* mem = rex::system::kernel_memory();
  const uint8_t* p = mem->TranslatePhysical<const uint8_t*>(addr);
  std::vector<uint32_t> le(dwords);
  for (uint32_t i = 0; i < dwords; ++i) {
    uint32_t v;
    std::memcpy(&v, p + i * 4, 4);
    le[i] = __builtin_bswap32(v);
  }
  info.hash = Fnv(le.data(), le.size());
  ScanUcode(le.data(), dwords, info);
  return &t.shaders.emplace(key, info).first->second;
}

// The GPU view d3d_state keeps (dirty masks + bound objects + draw arguments) against what the
// tap decoded: every register the tap has seen written, and the shaders it saw IM_LOADed.
void CompareState(Tap& t, FnId id) {
  const auto& gs = pgr4::native::GuestState();
  if (gs.device() != t.device) {
    ++t.state_unchecked;
    if (t.tracing && t.state_details < 64) {
      ++t.state_details;
      Append(t.out, "# state_unchecked d=%llu fn=%s tracker_device=%08X tap_device=%08X\n",
             (unsigned long long)t.draw_seq, kFns[id].name, gs.device(), t.device);
    }
    return;
  }
  ++t.state_draws;
  ++t.frame.checked;
  uint32_t bad = gs.CompareWithTap(t.regs, t.written, t.state_regs.data(), &t.state_per_tile);
  const auto& gvs = gs.vs();
  bool vs_bad = t.vs_addr && (gvs.ucode != t.vs_addr || gvs.dwords != t.vs_dwords || gvs.immediate);
  if (!t.vs_addr && t.vs_dwords) {  // IM_LOAD_IMMEDIATE: the tracker's copy must hash the same
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; gvs.immediate && i < gvs.dwords; ++i) {
      uint32_t d = __builtin_bswap32(gvs.immediate[i]);
      h = Fnv(&d, 1, h);
    }
    vs_bad = !gvs.immediate || gvs.dwords != t.vs_dwords || h != t.vs_imm_hash;
  }
  const bool ps_bad = t.ps_addr && (gs.ps().ucode != t.ps_addr || gs.ps().dwords != t.ps_dwords);
  if (vs_bad || ps_bad) ++t.state_shader_bad;
  if (!bad && !vs_bad && !ps_bad) return;
  ++t.state_bad_draws;
  ++t.frame.differ;
  // Traced frames list 64 differing draws; --pgr4_d3d_tap_check keeps each frame's first.
  const bool detail = t.tracing && t.state_details < 64;
  if (!detail && (t.tracing || !t.check_first.empty())) return;
  if (detail) ++t.state_details;
  std::string s;
  Append(s, "d=%llu fn=%s", (unsigned long long)t.draw_seq, kFns[id].name);
  const uint32_t* v = gs.regs();
  uint32_t shown = 0;
  for (uint32_t r = 0x2000; r < kRegCount && shown < 16; ++r) {
    if (t.written[r] != 1 || t.regs[r] == v[r] || pgr4::native::IsTapOnlyRegister(r)) continue;
    Append(s, " %04X tap=%08X state=%08X", r, t.regs[r], v[r]);
    ++shown;
  }
  if (vs_bad)
    Append(s, " vs tap=%08X:%u%s state=%08X:%u%s", t.vs_addr, t.vs_dwords, t.vs_addr ? "" : ":imm",
           gvs.ucode, gvs.dwords, gvs.immediate ? ":imm" : "");
  if (ps_bad)
    Append(s, " ps tap=%08X:%u state=%08X:%u", t.ps_addr, t.ps_dwords, gs.ps().ucode,
           gs.ps().dwords);
  if (detail)
    Append(t.out, "# state_diff %s\n", s.c_str());
  else
    t.check_first = std::move(s);
}

void OnDraw(Tap& t, uint32_t op, bool predicated) {
  // DRAW_INDX: viz, initiator, [index base, index size]; DRAW_INDX_2: initiator, indices.
  uint32_t initiator =
      op == 0x22 ? (t.pkt.size() > 1 ? t.pkt[1] : 0) : (t.pkt.empty() ? 0 : t.pkt[0]);
  bool resolve = (t.regs[0x2208] & 7) == uint32_t(xenos::EdramMode::kCopy);
  if (resolve) {
    ++t.frame.tap_resolves;
    g_tap_counts[2].fetch_add(1, std::memory_order_relaxed);
  }
  if (predicated) {
    ++t.frame.tap_draws;
    g_tap_counts[0].fetch_add(1, std::memory_order_relaxed);
  } else {
    ++t.frame.tap_aux;
    g_tap_counts[1].fetch_add(1, std::memory_order_relaxed);
  }
  if (!t.tracing) return;

  uint32_t prim = initiator & 0x3F, src = (initiator >> 6) & 3, count = initiator >> 16;
  FnId fn = CurrentFn();
  Append(t.out, "D %llu c=%llu fn=%s op=%s pred=%d %s prim=%u src=%s idx32=%u count=%u",
         (unsigned long long)t.draw_seq++, (unsigned long long)CurrentSeq(),
         fn < kFnCount ? kFns[fn].name : "-", OpName(op), predicated ? 1 : 0,
         resolve ? "RESOLVE" : "draw", prim,
         src == 0   ? "dma"
         : src == 2 ? "auto"
                    : "imm",
         (initiator >> 11) & 1, count);
  if (op == 0x22 && src == 0 && t.pkt.size() >= 4)
    Append(t.out, " ib=%08X:%u", t.pkt[2], t.pkt[3] & 0xFFFFFF);
  if (resolve) {
    Append(t.out, " copy_ctl=%08X dest=%08X pitch=%08X info=%08X surf=%08X c0=%08X d=%08X\n",
           t.regs[0x2318], t.regs[0x2319], t.regs[0x231A], t.regs[0x231B], t.regs[0x2000],
           t.regs[0x2001], t.regs[0x2002]);
    return;
  }
  const ShaderInfo* vs = Shader(t, 0, t.vs_addr, t.vs_dwords);
  const ShaderInfo* ps = Shader(t, 1, t.ps_addr, t.ps_dwords);
  Append(t.out, " vs=%08X:%u:%016llX ps=%08X:%u:%016llX", t.vs_addr, t.vs_dwords,
         (unsigned long long)(vs ? vs->hash : t.vs_imm_hash), t.ps_addr, t.ps_dwords,
         (unsigned long long)(ps ? ps->hash : t.ps_imm_hash));
  Append(t.out,
         " mode=%08X surf=%08X c0=%08X c1=%08X c2=%08X c3=%08X d=%08X mask=%08X depth=%08X"
         " blend0=%08X sc=%08X,%08X vgt_offs=%u",
         t.regs[0x2208], t.regs[0x2000], t.regs[0x2001], t.regs[0x2003], t.regs[0x2004],
         t.regs[0x2005], t.regs[0x2002], t.regs[0x2104], t.regs[0x2200], t.regs[0x2201],
         t.regs[0x2081], t.regs[0x2082], t.regs[0x2102]);
  uint32_t tmask = (vs ? vs->tfetch_mask : 0) | (ps ? ps->tfetch_mask : 0);
  for (uint32_t s = 0; s < 32; ++s) {
    if (!(tmask >> s & 1)) continue;
    const uint32_t* f = &t.regs[0x4800 + s * 6];
    uint32_t w = (f[2] & 0x1FFF) + 1, h = ((f[2] >> 13) & 0x1FFF) + 1;
    Append(t.out, " t%u=%08X/fmt%u/%ux%u/%s/dim%u", s, f[1] & 0xFFFFF000u, f[1] & 0x3F, w, h,
           (f[0] >> 31) ? "tiled" : "linear", (f[5] >> 9) & 3);
    ++t.textures[{f[1], f[2]}];
  }
  if (vs) {
    for (uint32_t v = 0; v < 96; ++v) {
      bool used = v < 64 ? (vs->vfetch_lo >> v & 1) : (vs->vfetch_hi >> (v - 64) & 1);
      if (!used) continue;
      const uint32_t* f = &t.regs[0x4800 + v * 2];
      Append(t.out, " v%u=%08X:%u", v, f[0] & 0xFFFFFFFCu, (f[1] >> 2) & 0xFFFFFF);
    }
  }
  Append(t.out, "\n");
}

void HandlePacket(Tap& t) {
  uint32_t type = t.header >> 30;
  if (type == 0) {
    uint32_t reg = t.header & 0x7FFF;
    bool one = (t.header >> 15) & 1;
    for (size_t i = 0; i < t.pkt.size(); ++i) WriteReg(t, one ? reg : reg + uint32_t(i), t.pkt[i]);
    if (t.tracing) {
      Append(t.out, "W %04X", reg);
      for (size_t i = 0; i < t.pkt.size() && i < 64; ++i) Append(t.out, " %08X", t.pkt[i]);
      Append(t.out, "%s\n", t.pkt.size() > 64 ? " ..." : "");
    }
    return;
  }
  if (type == 1) {
    WriteReg(t, t.header & 0x7FF, t.pkt[0]);
    WriteReg(t, (t.header >> 11) & 0x7FF, t.pkt[1]);
    return;
  }
  uint32_t op = (t.header >> 8) & 0x7F;
  bool pred = t.header & 1;
  if (t.tracing) ++t.op_counts[op];
  if (!OpName(op)) ++t.frame.unknown_ops;
  auto range = [&](uint32_t reg, size_t first) {
    const uint8_t how = pred ? pgr4::native::GuestStateTracker::kTapWrittenPredicated : 1;
    for (size_t i = first; i < t.pkt.size(); ++i)
      WriteReg(t, reg + uint32_t(i - first), t.pkt[i], how);
    if (t.tracing) {
      size_t n = t.pkt.size() - first;
      if (reg >= 0x4000 && reg < 0x4800)
        Append(t.out, "K %04X n=%zu h=%016llX\n", reg, n,
               (unsigned long long)Fnv(t.pkt.data() + first, n));
      else {
        Append(t.out, "W %04X", reg);
        for (size_t i = first; i < t.pkt.size() && i < first + 64; ++i)
          Append(t.out, " %08X", t.pkt[i]);
        Append(t.out, "\n");
      }
    }
  };
  switch (op) {
    case 0x2D: {  // SET_CONSTANT
      if (t.pkt.empty()) break;
      uint32_t index = t.pkt[0] & 0x7FF, kind = (t.pkt[0] >> 16) & 0xFF;
      if (kind < std::size(kConstantBase)) range(kConstantBase[kind] + index, 1);
      break;
    }
    case 0x55:  // SET_CONSTANT2
    case 0x56:  // SET_SHADER_CONSTANTS
      if (!t.pkt.empty()) range(t.pkt[0] & 0xFFFF, 1);
      break;
    case 0x2F: {  // LOAD_ALU_CONSTANT
      if (t.pkt.size() < 3) break;
      uint32_t addr = t.pkt[0] & 0x3FFFFFFF, index = t.pkt[1] & 0x7FF,
               kind = (t.pkt[1] >> 16) & 0xFF;
      uint32_t n = t.pkt[2] & 0xFFF;
      if (kind >= std::size(kConstantBase)) break;
      const uint8_t* p = rex::system::kernel_memory()->TranslatePhysical<const uint8_t*>(addr);
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, p + i * 4, 4);
        WriteReg(t, kConstantBase[kind] + index + i, __builtin_bswap32(v));
      }
      if (t.tracing) Append(t.out, "L %04X n=%u from=%08X\n", kConstantBase[kind] + index, n, addr);
      break;
    }
    case 0x27:  // IM_LOAD
      if (t.pkt.size() < 2) break;
      if ((t.pkt[0] & 3) == 0) {
        t.vs_addr = t.pkt[0] & ~3u;
        t.vs_dwords = t.pkt[1] & 0xFFFF;
      } else {
        t.ps_addr = t.pkt[0] & ~3u;
        t.ps_dwords = t.pkt[1] & 0xFFFF;
      }
      if (t.tracing)
        Append(t.out, "S %s %08X %u\n", (t.pkt[0] & 3) ? "ps" : "vs", t.pkt[0] & ~3u,
               t.pkt[1] & 0xFFFF);
      break;
    case 0x2B: {  // IM_LOAD_IMMEDIATE
      if (t.pkt.size() < 2) break;
      uint64_t h = Fnv(t.pkt.data() + 2, t.pkt.size() - 2);
      if ((t.pkt[0] & 3) == 0) {
        t.vs_addr = 0;
        t.vs_dwords = t.pkt[1] & 0xFFFF;
        t.vs_imm_hash = h;
      } else {
        t.ps_addr = 0;
        t.ps_dwords = t.pkt[1] & 0xFFFF;
        t.ps_imm_hash = h;
      }
      if (t.tracing)
        Append(t.out, "S %s imm %u %016llX\n", (t.pkt[0] & 3) ? "ps" : "vs", t.pkt[1] & 0xFFFF,
               (unsigned long long)h);
      break;
    }
    case 0x22:
    case 0x36:
    case 0x34:
    case 0x35:
      OnDraw(t, op, pred);
      break;
    case 0x3F:
    case 0x37:
      ++t.frame.ibs;
      if (t.tracing && t.pkt.size() >= 2)
        Append(t.out, "I %08X %u\n", t.pkt[0], t.pkt[1] & 0xFFFFF);
      break;
    case 0x10:  // NOP (also carries inline vertex data for the UP draws)
      break;
    default:
      if (t.tracing) {
        Append(t.out, "P %s%s n=%zu", OpName(op) ? OpName(op) : "op", pred ? " pred" : "",
               t.pkt.size());
        if (!OpName(op)) Append(t.out, " opcode=%02X", op);
        for (size_t i = 0; i < t.pkt.size() && i < 8; ++i) Append(t.out, " %08X", t.pkt[i]);
        Append(t.out, "\n");
      }
      break;
  }
}

void Feed(Tap& t, uint32_t dw) {
  if (!t.in_packet) {
    // MakeSpace leaves dwords it patches later as zeros; the D3D never writes register 0, so
    // a zero header is padding, not a one-register type-0 packet.
    if (dw == 0) {
      ++t.frame.zero_pad;
      return;
    }
    t.header = dw;
    t.pkt.clear();
    switch (dw >> 30) {
      case 0:
      case 3:
        t.need = ((dw >> 16) & 0x3FFF) + 1;
        break;
      case 1:
        t.need = 2;
        break;
      default:
        return;  // type 2: NOP
    }
    t.in_packet = true;
    return;
  }
  t.pkt.push_back(dw);
  if (--t.need == 0) {
    t.in_packet = false;
    HandlePacket(t);
  }
}

// Decode everything the D3D wrote since the last sync.
// `jump`: the call that just returned moved the push pointer to a new segment by assignment,
// so the dwords up to the new pointer (reserved patch space, the unwritten segment start) were
// not written in this lap and are skipped.
void Sync(Tap& t, uint8_t* base, bool segment_switch_expected, bool jump = false) {
  uint32_t cur = Load32(base, t.device + dev::kPush);
  if (cur == t.cursor) return;
  if (cur > t.cursor && cur - t.cursor <= (16u << 20) && !jump) {
    // Each window starts at a packet boundary: the D3D completes its packets before it calls
    // another function or returns. A packet still open here was cut short (or the decoder
    // misread a length); drop it rather than let a bad length swallow the following stream.
    if (t.in_packet) {
      ++t.frame.torn;
      if (t.tracing)
        Append(t.out, "# torn packet header=%08X still needs %u dwords at window %08X\n", t.header,
               t.need, t.cursor + 4);
      t.in_packet = false;
    }
    if (t.tracing) {  // raw dwords, for checking the decoder against the stream
      uint32_t n = (cur - t.cursor) / 4;
      Append(t.out, "X %08X n=%u", t.cursor + 4, n);
      for (uint32_t i = 0; i < n && i < 24; ++i)
        Append(t.out, " %08X", Load32(base, t.cursor + 4 + i * 4));
      Append(t.out, "%s\n", n > 24 ? " ..." : "");
    }
    for (uint32_t a = t.cursor + 4; a <= cur; a += 4) Feed(t, Load32(base, a));
    t.frame.dwords += (cur - t.cursor) / 4;
  } else {
    // The pointer moved to another segment (MakeSpace, RestorePush). Packets end at segment
    // boundaries, so a partial packet here means the decoder lost data.
    if (segment_switch_expected)
      ++t.frame.segments;
    else
      ++t.frame.resyncs;
    if (t.in_packet) ++t.frame.resyncs;
    t.in_packet = false;
    if (t.tracing)
      Append(t.out, "# segment %08X -> %08X%s\n", t.cursor, cur,
             segment_switch_expected ? "" : " (unexpected)");
  }
  t.cursor = cur;
}

void FlushTrace(Tap& t) {
  Append(t.out,
         "# summary frame=%llu calls=%llu draw_calls=%llu tap_draws=%llu tap_aux=%llu"
         " tap_resolves=%llu dwords=%llu segments=%llu resyncs=%llu torn=%llu unknown_ops=%llu"
         " ibs=%llu zero_pad=%llu state_checked_draws=%llu state_mismatch_draws=%llu"
         " state_shader_mismatch=%llu state_unchecked=%llu state_per_tile=%llu\n",
         (unsigned long long)t.trace_frame, (unsigned long long)t.frame.calls,
         (unsigned long long)t.frame.draw_calls, (unsigned long long)t.frame.tap_draws,
         (unsigned long long)t.frame.tap_aux, (unsigned long long)t.frame.tap_resolves,
         (unsigned long long)t.frame.dwords, (unsigned long long)t.frame.segments,
         (unsigned long long)t.frame.resyncs, (unsigned long long)t.frame.torn,
         (unsigned long long)t.frame.unknown_ops, (unsigned long long)t.frame.ibs,
         (unsigned long long)t.frame.zero_pad, (unsigned long long)t.state_draws,
         (unsigned long long)t.state_bad_draws, (unsigned long long)t.state_shader_bad,
         (unsigned long long)t.state_unchecked, (unsigned long long)t.state_per_tile);
  for (uint32_t r = 0; r < kRegCount; ++r)
    if (t.state_regs[r])
      Append(t.out, "# state_mismatch %04X %s draws=%llu\n", r, RegName(r) ? RegName(r) : "?",
             (unsigned long long)t.state_regs[r]);
  for (uint32_t i = 0; i < kFnCount; ++i)
    if (t.fn_calls[i])
      Append(t.out, "# calls %08X %s %llu\n", kFns[i].addr, kFns[i].name,
             (unsigned long long)t.fn_calls[i]);
  for (auto& [op, n] : t.op_counts)
    Append(t.out, "# packets %s %llu\n", OpName(op) ? OpName(op) : "?", (unsigned long long)n);
  for (auto& [k, s] : t.shaders)
    Append(t.out, "# shader %s %08X dwords=%u hash=%016llX tfetch=%08X vfetch=%08X%016llX\n",
           s.type ? "ps" : "vs", s.addr, s.dwords, (unsigned long long)s.hash, s.tfetch_mask,
           s.vfetch_hi, (unsigned long long)s.vfetch_lo);
  for (auto& [k, n] : t.textures)
    Append(t.out, "# texture %08X fmt%u %ux%u draws=%llu\n", k.first & 0xFFFFF000u, k.first & 0x3F,
           (k.second & 0x1FFF) + 1, ((k.second >> 13) & 0x1FFF) + 1, (unsigned long long)n);
  std::filesystem::path dir = REXCVAR_GET(pgr4_d3d_trace_dir);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  char name[256];
  std::snprintf(name, sizeof(name), "pgr4-d3d-%s-f%llu.txt", t.trace_tag.c_str(),
                (unsigned long long)t.trace_frame);
  std::ofstream(dir / name, std::ios::binary).write(t.out.data(), std::streamsize(t.out.size()));
  REXLOG_INFO("PGR4 D3D trace: wrote {} ({} bytes, {} draws, {} resolves)", (dir / name).string(),
              t.out.size(), t.frame.tap_draws, t.frame.tap_resolves);
  t.out.clear();
  t.out.shrink_to_fit();
}

void BeginTrace(Tap& t) {
  t.trace_frame = t.frame_index;
  t.call_seq = t.draw_seq = 0;
  t.state_draws = t.state_bad_draws = t.state_shader_bad = t.state_unchecked = t.state_details = 0;
  t.state_per_tile = 0;
  t.state_pending = false;
  std::fill(t.state_regs.begin(), t.state_regs.end(), 0);
  t.op_counts.clear();
  t.shaders.clear();
  t.textures.clear();
  std::memset(t.fn_calls, 0, sizeof(t.fn_calls));
  t.out.reserve(32u << 20);
  Append(t.out, "# pgr4-d3d-trace v1 tag=%s frame=%llu device=%08X push=%08X\n",
         t.trace_tag.c_str(), (unsigned long long)t.frame_index, t.device, t.cursor);
  Append(t.out, "# state at frame start (registers the tap has seen written)\n");
  for (uint32_t r = 0; r < kRegCount; ++r) {
    if (!t.written[r]) continue;
    if (r >= 0x4000 && r < 0x4800) continue;  // float constants: hashed per block below
    const char* n = RegName(r);
    Append(t.out, "R %04X %08X %s\n", r, t.regs[r], n ? n : "");
  }
  for (uint32_t b = 0x4000; b < 0x4800; b += 256)
    Append(t.out, "K %04X n=256 h=%016llX\n", b, (unsigned long long)Fnv(&t.regs[b], 256));
  Append(t.out, "S vs %08X %u\nS ps %08X %u\n", t.vs_addr, t.vs_dwords, t.ps_addr, t.ps_dwords);
}

void PollControl(Tap& t, uint8_t* base) {
  const std::string& path = REXCVAR_GET(pgr4_d3d_control);
  if (path.empty()) return;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return;
  std::ifstream in(path);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(in, line)) lines.push_back(line);
  in.close();
  std::filesystem::remove(path, ec);
  for (auto& l : lines) {
    std::istringstream ss(l);
    std::string cmd, arg;
    ss >> cmd >> arg;
    if (cmd == "trace") {
      if (!t.enabled) {
        PGR4_WARN_ONCE("PGR4 D3D control: '{}' ignored: the tap is off", l);
        continue;
      }
      uint32_t frames = 1;
      ss >> frames;
      t.trace_tag = arg.empty() ? "frame" : arg;
      t.trace_left = std::max<uint32_t>(frames, 1);
    } else if (cmd == "skip") {
      g_skip.store(arg == "none" ? 0 : ParseSkip(arg));
    } else if (cmd == "tap") {
      bool on = arg != "off";
      if (on && !t.enabled && t.device) {  // resume at the current push position
        t.cursor = Load32(base, t.device + dev::kPush);
        t.in_packet = false;
      }
      t.enabled = on;
      if (!on) t.trace_left = 0;
      g_hook_all.store(on || REXCVAR_GET(pgr4_native_profile), std::memory_order_relaxed);
    }
    REXLOG_INFO("PGR4 D3D control: {} (skip mask {})", l, g_skip.load());
  }
}

struct Window {
  uint64_t frames = 0, calls = 0, draw_calls = 0, tap_draws = 0, tap_aux = 0, tap_resolves = 0;
  uint64_t dwords = 0, segments = 0, resyncs = 0, torn = 0, unknown_ops = 0, min_draws = ~0ull,
           max_draws = 0;
} g_window;

std::FILE* FrameCsv() {
  static std::FILE* f = [] {
    std::string p = REXCVAR_GET(pgr4_d3d_frame_csv);
    std::FILE* f = p.empty() ? nullptr : std::fopen(p.c_str(), "w");
    if (f)
      std::fprintf(f,
                   "frame,calls,draw_calls,tap_draws,tap_aux,tap_resolves,dwords,segments,resyncs,"
                   "torn,unknown_ops\n");
    return f;
  }();
  return f;
}

void EndFrame(Tap& t, uint8_t* base) {
  auto& c = t.frame;
  if (std::FILE* f = FrameCsv()) {
    std::fprintf(f, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                 (unsigned long long)t.frame_index, (unsigned long long)c.calls,
                 (unsigned long long)c.draw_calls, (unsigned long long)c.tap_draws,
                 (unsigned long long)c.tap_aux, (unsigned long long)c.tap_resolves,
                 (unsigned long long)c.dwords, (unsigned long long)c.segments,
                 (unsigned long long)c.resyncs, (unsigned long long)c.torn,
                 (unsigned long long)c.unknown_ops);
    if ((t.frame_index & 31) == 0) std::fflush(f);
  }
  auto& w = g_window;
  ++w.frames;
  w.calls += c.calls;
  w.draw_calls += c.draw_calls;
  w.tap_draws += c.tap_draws;
  w.tap_aux += c.tap_aux;
  w.tap_resolves += c.tap_resolves;
  w.dwords += c.dwords;
  w.segments += c.segments;
  w.resyncs += c.resyncs;
  w.torn += c.torn;
  w.unknown_ops += c.unknown_ops;
  w.min_draws = std::min(w.min_draws, c.tap_draws);
  w.max_draws = std::max(w.max_draws, c.tap_draws);
  int32_t every = REXCVAR_GET(pgr4_d3d_stats_frames);
  if (every > 0 && w.frames >= uint64_t(every)) {
    double n = double(w.frames);
    REXLOG_INFO(
        "PGR4 D3D: frames {} | per frame: calls {:.0f}, draw calls {:.1f}, tap draws {:.1f} "
        "(min {} max {}), tap aux {:.1f}, tap resolves {:.1f}, dwords {:.0f}, segments {:.1f}, "
        "resyncs {}, torn {}, unknown ops {} | tap {} skip {}",
        t.frame_index, w.calls / n, w.draw_calls / n, w.tap_draws / n, w.min_draws, w.max_draws,
        w.tap_aux / n, w.tap_resolves / n, w.dwords / n, w.segments / n, w.resyncs, w.torn,
        w.unknown_ops, t.enabled ? "on" : "off", g_skip.load());
    w = Window{};
  }
  if (t.check && c.differ) {
    const pgr4::native::NativeRenderer* r = pgr4::native::NativeRenderer::Get();
    PGR4_LOG_FIRST(
        2000, INFO,
        "PGR4 D3D check: frame {} (native {}): {} of {} draws differ from the stream; first {}",
        t.frame_index, r ? r->frame_index() : 0, c.differ, c.checked, t.check_first);
  }
  t.check_first.clear();
  if (t.tracing) {
    FlushTrace(t);
    t.tracing = false;
  }
  t.frame = FrameCounters{};
  ++t.frame_index;
  PollControl(t, base);
  if (t.trace_left) {  // a traced frame runs from one Swap's return to the next
    --t.trace_left;
    t.tracing = true;
    BeginTrace(t);
  }
}

// The game creates the device more than once (once more at the title screen); the device a Swap
// or draw call runs on is the current one.
[[gnu::noinline]] void FollowDevice(Tap& t, uint8_t* base, FnId id, uint32_t device) {
  PGR4_LOG_FIRST(8, INFO, "PGR4 D3D: {} runs on device {:08X} (was {:08X}); following it",
                 kFns[id].name, device, t.device);
  t.device = device;
  t.cursor = Load32(base, t.device + dev::kPush);
  t.in_packet = false;
}

// The tap's side of one hooked call: counts it, follows the device it runs on and, with the
// tap on, decodes the push buffer around it. `run` runs the guest function (OnHook's wrapper).
template <FnId kId, typename Run>
inline void OnCall(PPCContext& ctx, uint8_t* base, Run run) {
  // One guest thread drives the device: a plain increment keeps the counts exact, without the
  // locked add.
  if constexpr (IsCountedFn(kId))
    g_calls[kId].store(g_calls[kId].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  Tap& t = g_tap;
  const uint32_t r3 = ctx.r3.u32;
  constexpr uint32_t kSkip = SkipClassOf(kId);
  const bool forward = kSkip == 0 || !(kSkip & g_skip.load(std::memory_order_relaxed));
  auto call = [&] {
    if (forward)
      run(ctx, base);
    else
      ctx.r3.u64 = 0;
  };
  if constexpr (kId == kFnSwap || kId == kFnDrawIndexed || kId == kFnDrawVertices) {
    if (r3 && r3 != t.device && t.device) [[unlikely]]
      FollowDevice(t, base, kId, r3);
  }
  if (!(t.device && r3 == t.device)) {
    if constexpr (kId == kFnCreateDevice) {  // CreateDevice(..., ppDevice in r8)
      const uint32_t out = ctx.r8.u32;
      run(ctx, base);
      t.device = Load32(base, out);
      t.cursor = t.device ? Load32(base, t.device + dev::kPush) : 0;
      t.in_packet = false;
      // Only once: a later device would undo the control commands sent since.
      if (!t.configured) {
        t.configured = true;
        t.enabled = REXCVAR_GET(pgr4_d3d_tap);
        t.check = t.enabled && REXCVAR_GET(pgr4_d3d_tap_check);
        g_hook_all.store(t.enabled || REXCVAR_GET(pgr4_native_profile), std::memory_order_relaxed);
        g_skip.store(ParseSkip(REXCVAR_GET(pgr4_d3d_skip)));
      }
      REXLOG_INFO("PGR4 D3D: device {:08X}, push {:08X}, tap {}{}, {} hooked functions", t.device,
                  t.cursor, t.enabled ? "on" : "off", t.check ? " (state check)" : "",
                  uint32_t(kFnCount));
      return;
    }
    call();
    return;
  }
  if (!t.enabled) {  // the hooks only forward; Swap still polls the control file
    call();
    if constexpr (kId == kFnSwap) {
      ++t.frame_index;
      PollControl(t, base);
    }
    return;
  }

  Sync(t, base, false);
  const uint64_t seq = ++t.call_seq;
  ++t.frame.calls;
  if constexpr (IsDrawCounterFn(kId)) ++t.frame.draw_calls;
  if (t.tracing) {
    ++t.fn_calls[kId];
    Append(t.out, "C %llu %u %s r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X%s\n",
           (unsigned long long)seq, t_depth, kFns[kId].name, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
           ctx.r7.u32, ctx.r8.u32, forward ? "" : " SKIPPED");
  }
  if (t_depth < 64) t_stack[t_depth] = {kId, seq};
  ++t_depth;
  call();
  // These move the push pointer by assignment: MakeSpace and RestorePush stay in the written
  // stream; the segment functions leave a hole (reserved dwords, an unwritten new segment).
  constexpr bool kJump = kId == kFnSwitchSegment || kId == kFnStartSegment ||
                         kId == kFnRestoreSegment || kId == kFnRestoreSegment13392;
  Sync(t, base, kJump || kId == kFnMakeSpace || kId == kFnRestorePush || kId == kFnDrawVerticesUP,
       kJump);
  --t_depth;
  if (t.tracing) Append(t.out, "R %llu r3=%08X\n", (unsigned long long)seq, ctx.r3.u32);
  if (t_depth == 0 && (t.tracing || t.check)) {
    if (t.state_pending) {  // BeginVertices' packets are in the stream once RestorePush ran
      t.state_pending = false;
      if constexpr (kId == kFnRestorePush)
        CompareState(t, kFnBeginVertices);
      else
        ++t.state_unchecked;
    }
    if constexpr (kFns[kId].cat == kDraw) {
      if constexpr (kId == kFnBeginVertices)
        t.state_pending = true;
      else
        CompareState(t, kId);
    }
  }
  if constexpr (kId == kFnSwap) EndFrame(t, base);
}

// Native renderer dispatch (--pgr4_native). Only the outermost draw/clear/resolve/Swap call
// reaches the renderer: the D3D's own draws inside them (Clear -> ClearF -> DrawIndx2Inline,
// Resolve's copy quads, Swap -> DrawIndx2Split, BeginVertices inside DrawVerticesUP) are not
// game draws. Other hooked functions do not count as outer calls (BeginTiling fn_826D1C38
// clears through a nested ClearF, which must reach OnClear). A top-level BeginVertices only
// reserves space: the game writes the vertices after it returns, so its OnDraw is held until
// the entry of RestorePush (EndVertices) or of the next hooked call that is not a pass-through.
enum NativeClass : uint8_t { kNatNone, kNatDraw, kNatClear, kNatResolve, kNatSwap };

constexpr NativeClass NativeClassOf(Cat c) {
  switch (c) {
    case kDraw:
      return kNatDraw;
    case kClear:
      return kNatClear;
    case kResolve:
      return kNatResolve;
    case kSwap:
      return kNatSwap;
    default:
      return kNatNone;
  }
}

thread_local uint32_t t_native_depth = 0;  // active draw/clear/resolve/Swap calls
thread_local bool t_pending_vertices = false;
thread_local pgr4::native::HookArgs t_pending_args;
thread_local uint32_t t_original_depth = 0;  // hooked originals running (profiler on)
thread_local pgr4::native::TilingCall t_tiling_call = pgr4::native::TilingCall::kNone;

// Hooked functions that neither the renderer nor the state tracker needs to see: no draw,
// clear, resolve or Swap, no state flush (d3d_state.h IsStateFlushingFn), not the tiling
// bracket and not CreateDevice. Without the tap they run straight through, and without the
// profiler too they skip OnHook (HasFastPath).
constexpr bool IsPassThrough(FnId id) {
  return NativeClassOf(kFns[id].cat) == kNatNone &&
         !pgr4::native::IsStateFlushingFn(kFns[id].addr) && id != kFnBeginTiling &&
         id != kFnEndTiling && id != kFnCreateDevice;
}

// The pass-through hooks with a fast path. RestorePush (EndVertices) keeps OnHook's: the draw a
// top-level BeginVertices holds is queued at its entry, once the game has written the vertices.
constexpr bool HasFastPath(FnId id) { return IsPassThrough(id) && id != kFnRestorePush; }

using GuestFn = void (*)(PPCContext&, uint8_t*);

// profile::kGuestD3d by function (--pgr4_native_profile), on the thread that calls Swap.
struct GuestD3dTimes {
  uint64_t ticks[kFnCount];
  uint64_t calls[kFnCount];
  uint32_t frames;
};
thread_local GuestD3dTimes t_guest_d3d_times;

// Every --pgr4_d3d_stats_frames Swaps: the functions with the most guest D3D time.
[[gnu::noinline]] void LogGuestD3dTimes() {
  GuestD3dTimes& t = t_guest_d3d_times;
  const int32_t every = REXCVAR_GET(pgr4_d3d_stats_frames);
  if (every <= 0 || ++t.frames < uint32_t(every)) return;
  uint32_t order[kFnCount];
  std::iota(order, order + kFnCount, 0u);
  const uint32_t shown = std::min<uint32_t>(12, kFnCount);
  std::partial_sort(order, order + shown, order + kFnCount,
                    [&](uint32_t a, uint32_t b) { return t.ticks[a] > t.ticks[b]; });
  const double n = t.frames;
  std::string line;
  char item[96];
  for (uint32_t k = 0; k < shown && t.ticks[order[k]]; ++k) {
    const uint32_t i = order[k];
    std::snprintf(item, sizeof(item), " %s %.0f us %.0f calls,", kFns[i].name,
                  double(pgr4::native::TicksToNs(t.ticks[i])) / 1e3 / n, double(t.calls[i]) / n);
    line += item;
  }
  if (!line.empty()) line.pop_back();
  REXLOG_INFO("PGR4 native guest D3D: per frame by outermost call:{}", line);
  t = GuestD3dTimes{};
}

// Runs a hooked guest D3D function. The outermost one is timed as profile::kGuestD3d, with the
// hooked calls it makes (Clear's ClearF and DrawIndx2Inline, for one) and their tracking.
template <FnId kId, GuestFn kFn>
inline void RunOriginal(PPCContext& c, uint8_t* b) {
  namespace nat = pgr4::native;
  if (!nat::profile::On()) [[likely]] {
    kFn(c, b);
    return;
  }
  if (t_original_depth++ == 0) {
    const uint64_t start = nat::Ticks();
    kFn(c, b);
    const uint64_t ticks = nat::Ticks() - start;
    nat::profile::g.ticks[nat::profile::kGuestD3d] += ticks;
    t_guest_d3d_times.ticks[kId] += ticks;
    ++t_guest_d3d_times.calls[kId];
  } else {
    kFn(c, b);
  }
  --t_original_depth;
}

// The guest function, or its replacement (--pgr4_native_replace_draws, native/guest_d3d.h).
template <FnId kId, GuestFn kOriginal>
void Body(PPCContext& c, uint8_t* b) {
  if constexpr (kId == kFnDrawIndexed)
    if (pgr4::native::guest_d3d::DrawIndexed(c, b)) return;
  kOriginal(c, b);
}

// Queues the draw a top-level BeginVertices holds: the game has written its vertices by now.
inline void QueuePendingVertices(pgr4::native::NativeRenderer* r, const uint8_t* base) {
  if (t_pending_vertices && r) {
    t_pending_vertices = false;
    r->OnDraw(base, t_pending_args);
  }
}

// r3..r10 at a hooked call's entry (HookArgs::r, the state tracker's `r`).
inline std::array<uint32_t, 8> EntryRegs(const PPCContext& ctx) {
  return {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
          ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
}

// A hooked call's arguments at its entry, before the guest function clobbers them.
inline pgr4::native::HookArgs EntryArgs(uint32_t fn, const PPCContext& ctx) {
  pgr4::native::HookArgs a;
  a.fn = pgr4::native::HookFn(fn);
  a.device = ctx.r3.u32;
  const std::array<uint32_t, 8> r = EntryRegs(ctx);
  std::copy(r.begin(), r.end(), a.r);
  a.sp = ctx.r1.u32;
  a.f[0] = ctx.f1.f64;
  a.f[1] = ctx.f2.f64;
  a.f[2] = ctx.f3.f64;
  a.f[3] = ctx.f4.f64;
  a.ret = 0;
  a.tiling = t_tiling_call;
  return a;
}

// Whether the state tracker (native/d3d_state) follows the guest D3D: for the native renderer
// `r` (NativeRenderer::Live) and for the tap's compare.
inline bool TrackState(const pgr4::native::NativeRenderer* r) {
  return r || g_tap.enabled || REXCVAR_GET(pgr4_d3d_tap);
}

// The state tracker (native/d3d_state) needs the dirty masks at the entry of every flushing
// call, at every depth, and the call's exit before anything reads the state (the tap's per-draw
// compare inside OnCall, OnDraw here). So the original is wrapped; OnCall's skip path does not
// run it and nothing is tracked then. Everything that depends on the function is resolved at
// compile time: each hook holds only its own path.
template <FnId kId, GuestFn kOriginal>
inline void OnHook(PPCContext& ctx, uint8_t* base) {
  namespace nat = pgr4::native;
  constexpr uint32_t kAddr = kFns[kId].addr;
  constexpr NativeClass kClass = NativeClassOf(kFns[kId].cat);
  if constexpr (IsPassThrough(kId)) {
    if (!g_tap.enabled) {
      if constexpr (!HasFastPath(kId))
        if (t_pending_vertices) QueuePendingVertices(nat::NativeRenderer::Live(), base);
      RunOriginal<kId, kOriginal>(ctx, base);
      return;
    }
  }
  nat::NativeRenderer* const r = nat::NativeRenderer::Live();
  const bool track = TrackState(r);
  auto run = [&](PPCContext& c, uint8_t* b) {
    if constexpr (nat::IsStateFlushingFn(kAddr)) {
      if (track && c.r3.u32) {
        const std::array<uint32_t, 8> rr = EntryRegs(c);
        nat::CallEntry e;
        {
          nat::profile::Scope scope(nat::profile::kTrack);
          e = nat::ReadDirtyMasks(b, rr[0]);
          if constexpr (kId == kFnDrawIndexed) nat::PrefetchIndexBuffer(b, rr[0]);
        }
        RunOriginal<kId, Body<kId, kOriginal>>(c, b);
        nat::profile::Scope scope(nat::profile::kTrack);
        ++nat::profile::g.track_calls;
        nat::GuestState().AfterCall(b, kAddr, e, rr.data(), c.r3.u64);
        return;
      }
    }
    RunOriginal<kId, Body<kId, kOriginal>>(c, b);
  };
  if (!r) {
    OnCall<kId>(ctx, base, run);
    if constexpr (kId == kFnSwap) {
      if (nat::NativeRenderer* suspended = nat::NativeRenderer::Get()) suspended->OnSwapSuspended();
      NoteFrameBegin();
    }
    return;
  }
  if constexpr (!HasFastPath(kId)) QueuePendingVertices(r, base);
  if constexpr (kClass == kNatNone) {
    if constexpr (kId == kFnBeginTiling || kId == kFnEndTiling) {
      const nat::TilingCall outer = t_tiling_call;
      t_tiling_call = kId == kFnBeginTiling ? nat::TilingCall::kBegin : nat::TilingCall::kEnd;
      OnCall<kId>(ctx, base, run);
      t_tiling_call = outer;
    } else {
      OnCall<kId>(ctx, base, run);
    }
  } else {
    // Only the outermost call reaches the renderer, with its arguments at entry.
    const bool outer = t_native_depth++ == 0;
    nat::HookArgs a;
    if (outer) a = EntryArgs(kAddr, ctx);
    OnCall<kId>(ctx, base, run);
    --t_native_depth;
    if (!outer) return;
    a.ret = ctx.r3.u64;
    if constexpr (kClass == kNatDraw) {
      if constexpr (kId == kFnBeginVertices) {
        t_pending_args = a;
        t_pending_vertices = true;
      } else {
        r->OnDraw(base, a);
      }
    } else if constexpr (kClass == kNatClear) {
      r->OnClear(base, a);
    } else if constexpr (kClass == kNatResolve) {
      r->OnResolve(base, a);
    } else {
      static_assert(kClass == kNatSwap);
      r->OnSwap(base, a);
      NoteFrameBegin();
      if (nat::profile::On()) LogGuestD3dTimes();
    }
  }
}
}  // namespace

// Cumulative counters for the SDK's --gpu_stream_stats (instrument patch): names
// starting with "draw" are summed into its api_total, as in DrawCounter's hooks.
extern "C" __declspec(dllexport) uint32_t rex_drawcount_api_counters(const uint64_t** values,
                                                                     const char* const** names) {
  // kDrawCounterFns, then Resolve, then g_tap_counts.
  static constexpr const char* kNames[] = {"draw_826CFA60", "draw_826CFF58", "draw_826D0340",
                                           "draw_826D0798", "draw_826DC888", "aux_826CBEE0",
                                           "tap_draws",     "tap_aux",       "tap_resolves"};
  constexpr size_t kDraws = std::size(kDrawCounterFns), kCount = std::size(kNames);
  static_assert(kCount == kDraws + 1 + std::size(g_tap_counts));
  static uint64_t snapshot[kCount];
  for (size_t i = 0; i < kDraws; ++i)
    snapshot[i] = g_calls[kDrawCounterFns[i]].load(std::memory_order_relaxed);
  snapshot[kDraws] = g_calls[kFnResolve].load(std::memory_order_relaxed);
  for (size_t i = 0; i < std::size(g_tap_counts); ++i)
    snapshot[kDraws + 1 + i] = g_tap_counts[i].load(std::memory_order_relaxed);
  *values = snapshot;
  *names = kNames;
  return uint32_t(kCount);
}

bool Pgr4Enable60Fps();  // games/pgr4/src/pgr4_hooks.cpp

// The presentation interval KickOffEx (sub_826CACA0) hands to the GPU, read from the device
// (Pgr4PresentationIntervalHook). While the native renderer paces the frames on the display the
// guest presents at once: waiting for a vblank as well would pace the game on ReXGlue's vblank
// timer a second time (frame_pacer.h). Otherwise the game's own interval, or ONE with the 60 FPS
// option.
uint32_t Pgr4PresentationInterval(uint32_t guest) {
  constexpr uint32_t kOne = 1, kImmediate = 0x80000000;  // D3DPRESENT_INTERVAL_*
  if (pgr4::native::NativeRenderer::PacesFrames()) return kImmediate;
  return Pgr4Enable60Fps() ? kOne : guest;
}

// Whether the game runs faster than the console's 30 FPS.
bool Pgr4FrameRateUnlocked() {
  return pgr4::native::NativeRenderer::PacesFrames() || Pgr4Enable60Fps();
}

// The motion blur pass (sub_82859FC0) smears each pixel along its reprojection to the previous
// frame (c154-c157), which spans the scene's motion over one time step. Its per-sample step
// (c159), the tweak's strength over the sample count, was tuned on the console's 30 FPS, so at
// 90 fps the blur was a third as long. Scaled by the steps per 1/30 s, it is the blur of a 30 FPS
// frame at any frame rate: in the same race at 30 and 90 fps the reprojection moved 2.95 times as
// far per frame at 30, and its product with the step matched within 2 % (td-127, td-128).
// Pgr4MotionBlurStepHook calls this.
float Pgr4MotionBlurStep(float step) {
  constexpr double kConsoleFrameNs = 1e9 / 30;
  constexpr int64_t kMinStepNs = 1'000'000, kMaxStepNs = 100'000'000;
  // Not measured yet (the first frames): the console's step. Clamped: steps under 1 ms are frames
  // queued behind a stall, which begin at once; steps over 100 ms are loads.
  const int64_t drawn_ns = g_drawn_step_ns.load(std::memory_order_relaxed);
  const int64_t frame_ns =
      drawn_ns ? std::clamp(drawn_ns, kMinStepNs, kMaxStepNs) : int64_t(kConsoleFrameNs);
  return float(step * (kConsoleFrameNs / double(frame_ns)));
}

#define D3D_FN(a, n, c)                                           \
  REX_EXTERN(__imp__sub_##a);                                     \
  REX_HOOK_RAW(sub_##a) {                                         \
    if constexpr (HasFastPath(kFn_##a)) {                         \
      if (!g_hook_all.load(std::memory_order_relaxed)) [[likely]] \
        return __imp__sub_##a(ctx, base);                         \
    }                                                             \
    OnHook<kFn_##a, __imp__sub_##a>(ctx, base);                   \
  }
#include "d3d_surface.inc"
#undef D3D_FN

// sub_826DEA78: the D3D's immediate vertex shader load (the variant copied into the push buffer
// and patched for the bound vertex declaration), called from the draw flush sub_826DEDC8. The
// state tracker keeps the patched ucode for the native draws (d3d_state.h, ShaderRef).
REX_EXTERN(__imp__sub_826DEA78);
REX_HOOK_RAW(sub_826DEA78) {
  const std::array<uint32_t, 8> r = EntryRegs(ctx);
  __imp__sub_826DEA78(ctx, base);
  if (TrackState(pgr4::native::NativeRenderer::Live())) {
    pgr4::native::profile::Scope scope(pgr4::native::profile::kImmediateVs);
    pgr4::native::GuestState().AfterImmediateVs(base, r.data());
  }
}
