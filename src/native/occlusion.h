// pgr4-native PGR4 native renderer: the game's occlusion queries, counted on the host.
//
// The guest D3D's QueryIssue (sub_826D2F00) brackets draws with EVENT_WRITE_ZPD reports into one
// block per EDRAM tile: the block addresses (physical) at query+28, their count at query+148.
// Each block is two xe_gpu_depth_sample_counts, the END report at +0 and the BEGIN report at
// +32, little-endian. END stores 0xFFFFFEED into the last block's ZPass lanes (directly, or with
// ring MEM_WRITEs and WAIT_REG_MEMs while the previous result is unread). GetData (sub_826D3188)
// returns S_FALSE until the GPU passed the fence stored at query+24 and those lanes no longer hold
// the sentinel; then it sums END - BEGIN of each counter over the blocks. The light flares
// (sub_826A12A8) fade by 1 - ZPass / Total, and skip a result whose Total is 1.
//
// Each draw recorded while a guest query is open gets a pair of host occlusion queries: the draw
// (ZPass), then the same draw with depth and stencil tests off and nothing written (coverage, the
// samples that reached the tests: Total). Canary's hybrid ZPD counts the same pre-test coverage
// in a shader (xenos_zpd_report.h, FromNativeQueryAndTotal). A guest query sums the pairs
// recorded between its BEGIN and END, so guest queries may overlap, and no host query spans a
// command list. The pairs are resolved at the Swap and read once the frame's fence completed.
// A pair whose coverage draw could not run (its pipeline, tests off, still compiling) is left
// out: with Total := ZPass its flare would snap to full brightness for those frames, and a query
// whose pairs are all left out keeps its previous result until they are counted.
//
// The game thread writes a result into the guest's blocks at the entry of GetData, once the
// guest fence passed: the command processor has then run the END's sentinel writes and waits, so
// nothing overwrites the result and no WAIT_REG_MEM waits on it. The SDK's command processor
// leaves the reports alone meanwhile (occlusion_query_external, patches/0049). The result is the
// query's latest one counted, which trails its END by the frames in flight: the game issues its
// queries again every frame, and a result held back for the END it belongs to would never be
// read. Canary's fast ZPD mode reports the same way (command_processor.cc, QueueZPDReport).
//
// Threading: Begin, End, Acquire, the Pass/Coverage brackets, Resolve and Retire belong to the
// thread that records (the render thread by default); WriteResult to the guest thread. Results pass
// between them under a mutex, a few dozen per frame.
#pragma once

#include "d3d12_device.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pgr4::native {

class OcclusionQueries {
 public:
  ~OcclusionQueries() {
    if (trace_) std::fclose(trace_);
  }
  static constexpr uint32_t kNoPair = UINT32_MAX;
  // Host query pairs in flight at most (about three frames of draws inside queries).
  static constexpr uint32_t kPairs = 4096;

  bool Initialize(ID3D12Device* device);

  // Recording thread, in the order of the guest's calls.
  void Begin(uint32_t query);
  void End(uint32_t query, uint64_t frame, uint64_t frame_fence);
  // Whether a guest query is open: the next draw needs a pair.
  bool counting() const { return !open_.empty(); }
  // The pair for the next draw, or kNoPair when the ring is full (the draw is not counted).
  uint32_t Acquire();
  void BeginPass(ID3D12GraphicsCommandList* list, uint32_t pair) const;
  void EndPass(ID3D12GraphicsCommandList* list, uint32_t pair) const;
  void BeginCoverage(ID3D12GraphicsCommandList* list, uint32_t pair) const;
  enum class Coverage : uint8_t {
    kCounted,  // the coverage draw ran
    kPasses,   // no depth target to test against: the coverage is the draw's ZPass
    kUnknown,  // the coverage pipeline was not ready: the pair is left out of the query's sums
  };
  // `guest_per_host`: guest samples per host sample of the draw's targets (fewer samples, or a
  // smaller render size).
  void EndCoverage(ID3D12GraphicsCommandList* list, uint32_t pair, Coverage coverage,
                   float guest_per_host);
  // Pairs recorded kUnknown so far (the frames CSV's occ_unknown).
  uint64_t unknown_pairs() const { return unknown_pairs_; }
  // At the Swap, into the frame's last list, which signals `frame_fence`.
  void Resolve(ID3D12GraphicsCommandList* list, uint64_t frame_fence);
  // After the Swap: the results of the guest queries whose frames completed, for WriteResult.
  void Retire(uint64_t completed_fence);

  // Guest thread, at GetData's entry: writes the query's latest result into its blocks, once the
  // guest fence passed and the blocks still wait for it.
  void WriteResult(uint8_t* base, uint32_t query, uint64_t frame);

 private:
  struct Open {
    uint32_t query;
    uint64_t first;  // pair
  };
  struct Ended {
    uint32_t query;
    uint64_t first, end;  // pairs [first, end)
    uint64_t fence;       // of the frame END was recorded in
    uint64_t ticks;       // when END was recorded
    bool known;           // BEGIN was recorded (not before a renderer switch)
  };
  // One guest query's recent underlying results: the delivered ZPass and Total are the sums of
  // the frames recorded within --pgr4_native_occlusion_window_ms of its newest one. The game
  // fades a flare by 1 - ZPass / Total, so the sum averages the frames' visibilities: a flare
  // that alternates per frame (its sprite tested against the previous frame's depth of a moving
  // scene) dims briefly instead of blinking, and an occlusion fades over the window rather than
  // in the one frame a light of a few samples takes to go behind something. A time rather than
  // a frame count, so that the fade lasts as long at every frame rate.
  static constexpr uint32_t kWindowMax = 32;  // frames: 300 ms at 90 fps, 100 ms at 320
  struct Frame {
    uint64_t z_pass, total, ticks;
  };
  struct Result {
    Frame win[kWindowMax] = {};
    uint32_t win_count = 0;  // pushed so far, up to kWindowMax
    uint32_t win_next = 0;   // the next slot to write (the oldest when full)
    bool known = false;
  };

  ComPtr<ID3D12QueryHeap> heap_;
  ComPtr<ID3D12Resource> readback_;
  const uint64_t* counts_ = nullptr;  // readback_, mapped: ZPass then coverage of each pair
  std::vector<uint8_t> coverage_;     // Coverage of each pair
  std::vector<float> guest_per_host_;
  std::vector<Open> open_;
  std::deque<Ended> ended_;
  std::deque<std::pair<uint64_t, uint64_t>> resolves_;  // (pairs resolved up to, frame fence)
  uint64_t next_ = 0, resolved_ = 0, retired_ = 0;      // pairs, counted from the start
  uint64_t unknown_pairs_ = 0;
  uint64_t window_ns_ = 0;      // --pgr4_native_occlusion_window_ms
  std::FILE* trace_ = nullptr;  // --pgr4_native_occlusion_trace

  std::mutex mutex_;
  std::unordered_map<uint32_t, Result> results_;  // the latest by query (mutex_)
};

}  // namespace pgr4::native
