// pgr4-native PGR4 native renderer (M2 owner #6): one guest draw -> one host D3D12 draw.
//
// Per draw (DrawRenderer::OnDraw, after ReadDrawState):
//   - shaders: ShaderCache::Load of the bound VS/PS ucode, SelectModifications (the Xenos
//     plugin's variant selection), translations with the DXBC translator (host RT path);
//   - pipeline: blend, depth/stencil, rasterizer, primitive topology, RT/DSV formats from the
//     register file, the geometry shader for point/rectangle/quad lists; PSOs cached by a hash
//     of that description (ports of d3d12::PipelineCache::GetCurrentStateDescription and
//     CreateD3D12Pipeline, BSD);
//   - vertices: the translator reads them in the vertex shader from "shared memory", a GPU
//     buffer mirroring guest physical memory (t0). Ranges named by the vertex fetch constants
//     the VS uses, and the index buffer, are uploaded at their guest offsets; uploaded pages
//     stay valid until a CPU write hits them (physical memory access watches, like the
//     plugin's shared memory). The inline vertices of BeginVertices/DrawVerticesUP live in the
//     push buffer and are uploaded on every draw, never watched;
//   - indices: DMA index buffers are bound straight from shared memory (big-endian, the VS
//     swaps them by SystemConstants::vertex_index_endian); auto-indexed draws are non-indexed;
//   - constants: the translator's b0 system constants (port of
//     D3D12CommandProcessor::UpdateSystemConstantValues, host RT path), b1 the VS/PS float
//     constants packed by the shaders' constant_register_map, b2 bool/loop, b3 fetch, b4
//     bindless descriptor indices; uploaded to per-frame ring buffers, reused while unchanged;
//   - textures from TextureCache (TextureMod), samplers from the sampler bindings (port of
//     D3D12TextureCache::GetSamplerParameters/WriteSampler), bindless heaps;
//   - render targets from RenderTargets::Bind (RTResolve); viewport, scissor, blend factor
//     and stencil reference (port of draw_util::GetHostViewportInfo / GetScissor).
//
// Not handled (counted in Stats and skipped): memexport, tessellation, host-converted index
// buffers (triangle fans, line loops, primitive reset with an index other than 0xFFFF).
//
// Threading: the renderer's On* calls only (one guest thread). With the render thread
// (renderer.h, --pgr4_native_render_thread) the work is split: Prepare and EndFramePrepare run
// on the guest thread at the draw's call (shader lookup, vertex/index copies out of guest
// memory, which the game may rewrite right after the draw); everything else runs on the
// render thread, later, from the DrawPrep the hook side made.
#pragma once

#include "d3d12_device.h"
#include "d3d_state.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rex::graphics {
class DxbcShader;
}

namespace pgr4::native {

class NativeRenderer;
struct ClearCall;
struct CoherencyJob;

// Guest memory copied into upload memory, to be copied into shared memory at `dst`.
struct VertexCopy {
  ID3D12Resource* src;
  uint64_t src_offset;
  uint32_t dst, size;
};

// The guest-thread half of one draw (DrawRenderer::Prepare).
struct DrawPrep {
  rex::graphics::DxbcShader* vs = nullptr;  // nullptr: no vertex shader (the draw is dropped)
  rex::graphics::DxbcShader* ps = nullptr;
  bool upload_ok = true;                    // every vertex/index range was copied
  const VertexCopy* copies = nullptr;       // set by the render side before OnDraw
  uint32_t copy_count = 0;
  bool copy_segment = false;                // Prepare: the draw's copies start a copy segment
  // Triangle fans: the converted host indices in upload memory.
  D3D12_GPU_VIRTUAL_ADDRESS fan_indices = 0;
  uint32_t fan_count = 0;
  bool fan_wide = false;
  uint64_t captured = ~0ull;  // TextureCache::MarkCapture stamp (kCapturedNow: none)
  // --pgr4_native_coherency_check: XXH3 of the register file's blocks as Prepare saw them
  // (render state, float constants VS and PS, fetch and bool/loop constants), 0: not taken.
  uint64_t regs_hash[5] = {};
};

class DrawRenderer {
 public:
  struct Stats {
    uint64_t draws = 0;           // issued host draws
    uint64_t skipped = 0;         // dropped like the plugin does (EDRAM copy, nothing rasterizes)
    uint64_t unsupported = 0;     // see the header comment; logged once per reason
    uint64_t failed = 0;          // shader/PSO/RT/upload failures
    uint64_t pso_created = 0;
    uint64_t pso_pending = 0;     // draws skipped while their pipeline compiles (async)
    uint64_t pso_wait_us = 0;     // draws waiting for their pipelines in a burst of new ones
    uint64_t upload_bytes = 0;    // guest memory copied into shared memory
    uint64_t uploads = 0;         // copy regions recorded
    uint64_t textures = 0, textures_missing = 0;  // bindings; missing = null SRV bound
    uint64_t textures_stale = 0;  // bindings for draws queued before a newer guest write
    uint64_t srv_lookups = 0, srv_created = 0, samplers_created = 0;
    uint64_t views = 0;           // texture views in the bindless heap (at the frame's end)
    uint64_t bind_memo_hits = 0, bind_memo_misses = 0;  // whole-shader texture binding memo
    uint64_t watch_faults = 0;    // guest writes caught by a page watch (any watcher), per frame
    uint64_t copy_batches = 0;    // groups of shared-memory copies, each between two barriers
    uint64_t watch_arms = 0, watch_arm_pages = 0;  // vertex/index watches armed (calls, pages)
    uint64_t hash_pages = 0, hash_copies = 0;  // hot pages hashed, and copied as they changed
    // --pgr4_native_coherency_check: pages whose guest memory differed from shared memory's
    // copy although the page watch said it was current, and pages rewritten after a draw of
    // the frame read them.
    uint64_t stale_pages = 0, late_pages = 0;
    // ... and pages whose shared memory copy, read back at the end of a frame, differs from
    // the bytes the frame's copies carried.
    uint64_t gpu_mismatch_pages = 0;
    uint64_t reg_mismatches = 0;  // draws whose register file differs from Prepare's
  };
  // Prepare's numbers for one frame (EndFramePrepare), added to stats() by AddPrepareCounters.
  struct PrepareCounters {
    uint64_t upload_bytes = 0, uploads = 0, stale_pages = 0, late_pages = 0;
    std::shared_ptr<CoherencyJob> job;  // the frame's copied ranges to read back, or null
  };

  explicit DrawRenderer(NativeRenderer& renderer);
  ~DrawRenderer();
  DrawRenderer(const DrawRenderer&) = delete;
  DrawRenderer& operator=(const DrawRenderer&) = delete;

  // Root signature, descriptor heaps, shared memory buffer, the memory watch. Logs and
  // returns false on failure.
  bool Initialize();

  // After a successful ReadDrawState for the draw. Records into renderer.command_list().
  // `prep` (render thread): what Prepare made for this draw on the guest thread.
  void OnDraw(const uint8_t* base, const DrawState& state, const DrawPrep* prep = nullptr);

  // Guest thread, render thread mode: shader lookup and vertex/index copies for `state` (whose
  // regs are the tracker's, current). Copies go to the end of `copies`; upload memory is
  // tagged with `upload_tag` (EndFramePrepare's units).
  //
  // Copy segments: the render thread records all copies of a segment ahead of the segment's
  // first draw that needs them, so a segment costs one pair of shared-memory barriers instead
  // of one pair per uploading draw. A draw starts a new segment (prep->copy_segment) when one
  // of its copies would overwrite a page an earlier draw of the segment reads: that draw must
  // still see the old bytes. BeginCopySegment starts one unconditionally (a new batch).
  void Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
               std::vector<VertexCopy>* copies);
  void BeginCopySegment();
  // Guest thread, once per Swap: recycles vertex upload memory with tags <= `done`. Returns
  // Prepare's counters since the previous call.
  PrepareCounters EndFramePrepare(uint64_t done);
  // Render thread, at the frame's Swap.
  void AddPrepareCounters(const PrepareCounters& c);
  // Render thread, the last thing recorded into a frame's command list.
  void BeforeSubmit();

  // Something else set graphics/compute state or descriptor heaps on the command list (the
  // Resolver does): set everything again at the next draw. A new frame's list is detected
  // without this call.
  void InvalidateListState();
  // Writes the clear into the draw log while one is open (draw-debug.txt bit 16).
  void LogClear(const ClearCall& c);

  // At Swap, before EndFrame: the frame skipped draws in a burst of new pipelines (a scene's
  // first frame, before its draws began waiting for them), so it is not worth showing.
  bool FrameIncomplete() const;
  // Per frame, with the fence values of NativeRenderer (once per Swap, after EndFrame).
  void EndFrame(uint64_t completed_fence);

  const Stats& stats() const { return stats_; }
  void ResetStats() { stats_ = Stats{}; }

  // Ucode hashes of a prepared draw's (vertex, pixel) shaders, 0 where there is none.
  static std::pair<uint64_t, uint64_t> ShaderHashes(const DrawPrep& prep);
  // The ucode disassembly of a loaded shader; empty when no loaded shader has the hash.
  std::string ShaderDisassembly(uint64_t hash) const;

 private:
  struct Impl;
  NativeRenderer& renderer_;
  std::unique_ptr<Impl> impl_;
  Stats stats_;
};

}  // namespace pgr4::native
