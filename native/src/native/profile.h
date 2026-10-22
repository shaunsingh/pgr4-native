// pgr4-native PGR4 native renderer: per-stage CPU time of the draw path (--pgr4_native_profile).
//
// Stage boundaries are timestamped with RDTSC (a few ns each, unlike QueryPerformanceCounter);
// NativeRenderer calibrates ticks to ns once per frame against NowNs and appends one µs column
// per stage to the native CSV. Off by default: then each Mark is one predictable branch.
//
// Threading: one State per thread. With the render thread (renderer.h) the guest thread's
// stages (read_state, cap_*, track, imm_vs) are handed to the render thread at each Swap and
// added to its State before the CSV row is written.
#pragma once

#include <intrin.h>

#include <cstdint>

namespace pgr4::native::profile {

enum Stage : uint32_t {
  // NativeRenderer::OnDraw / DrawRenderer::Impl::Draw, in order.
  kReadState,   // BeginFrame + ReadDrawState
  kShaders,     // shader memo/Load + SelectModifications + checks
  kSetup,       // primitive, index buffer, scissor/viewport, rasterization
  kRtBind,      // RenderTargets::Bind
  kTranslate,   // ShaderCache::Translate (VS, PS)
  kPipeline,    // PipelineKey + GetPipeline
  kTextures,    // TextureCache::Get, SRV/sampler descriptors, b4 tables
  kSysConst,    // b0 system constants
  kPack,        // b1 float constant packing
  kVertices,    // RequestRange (vertex fetch, indices) + FlushCopies
  kCbv,         // UploadCbv b0..b4
  kRecord,      // fixed-function state, render targets, IA, draw call
  // Hook side (pgr4_d3d_trace.cpp), outside OnDraw.
  kTrack,       // ReadDirtyMasks + GuestStateTracker::AfterCall, every flushing call
  kImmediateVs, // GuestStateTracker::AfterImmediateVs
  // Guest thread, render thread mode (renderer.h): the hook-side half of the On* calls.
  kCapShaders,  // DrawRenderer::Prepare: shader memo/Load
  kCapVertices, // DrawRenderer::Prepare: vertex/index copies, fan indices
  kCapRecord,   // register deltas, decoding Clear/Resolve/Swap, queueing
  // Nested inside kTextures (Scope timers, not part of the Mark chain; not summed with it).
  kTexMiss,     // bind_textures with no BindMemo hit (includes kTexGet and kTexSrv)
  kTexGet,      // TextureCache::Get calls on that path
  kTexSrv,      // SRV index lookups on that path
  // Nested inside kCapVertices: Memory::EnablePhysicalMemoryAccessCallbacks (the SDK's global
  // lock, then page protection on each of the three physical views).
  kCapWatch,
  // Hook side, overlapping kTrack (not summed with the others): time inside the original guest
  // D3D functions at the outermost hooked call, i.e. the D3D library's own work (packet
  // building, state flushes) that a recompilation which replaces the D3D API would not run.
  kGuestD3d,
  kStageCount
};

inline constexpr const char* kStageNames[kStageCount] = {
    "read_state", "shaders",  "setup",    "rt_bind",  "translate", "pipeline",
    "textures",   "sysconst", "pack",     "vertices", "cbv",       "record",
    "track",      "imm_vs",   "cap_shaders", "cap_vertices", "cap_record", "tex_miss",
    "tex_get",    "tex_srv",  "cap_watch", "guest_d3d"};

// Per-frame event counts (CSV columns, reset every frame). Counted whether or not the profiler
// is on (one increment each).
enum Counter : uint32_t {
  kBindHit,         // BindMemo hits (whole shader)
  kBindMru,         // lookups answered by the shader's last entry, without hashing
  kBindMissEmpty,   // memo slot empty or another shader's (first sight, evicted, not memoizable)
  kBindMissFetch,   // memo slot of this shader, but for other fetch constants
  kBindMissTexture, // same fetch constants, a texture changed (guest write or resolve)
  kBindNoMemo,      // miss that could not be memoized (missing texture, too many slots)
  kGetMissFetch,    // TextureCache::Get memo slot held other fetch dwords (or none)
  kGetMissChanged,  // same fetch dwords, texture changed
  kSrvMemoHit,      // SRV index from a memo (no XXH3 + map)
  kUploadSkipped,   // dirty texture not untiled: resolved images overwrite all of it
  kCounterCount
};

inline constexpr const char* kCounterNames[kCounterCount] = {
    "bind_hit",       "bind_mru",         "bind_miss_empty", "bind_miss_fetch", "bind_miss_tex",
    "bind_nomemo",    "get_miss_fetch",   "get_miss_changed", "srv_memo_hit",   "tex_upload_skip"};

struct State {
  bool on = false;
  uint64_t last = 0;
  uint64_t ticks[kStageCount] = {};
  uint64_t track_calls = 0;
  uint64_t counters[kCounterCount] = {};
};
inline thread_local State g;

inline void Count(Counter c) { ++g.counters[c]; }

inline uint64_t Now() { return __rdtsc(); }

// Starts a run of stages at `now`.
inline void Begin() {
  if (g.on) g.last = Now();
}
// Charges the time since the previous Begin/Mark to `stage`.
inline void Mark(Stage stage) {
  if (!g.on) return;
  const uint64_t t = Now();
  g.ticks[stage] += t - g.last;
  g.last = t;
}

// Times one call outside the Begin/Mark chain.
class Scope {
 public:
  explicit Scope(Stage s) : stage_(s), start_(g.on ? Now() : 0) {}
  ~Scope() {
    if (start_) g.ticks[stage_] += Now() - start_;
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Stage stage_;
  uint64_t start_;
};

}  // namespace pgr4::native::profile
