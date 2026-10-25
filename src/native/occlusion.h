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
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pgr4::native {

class OcclusionQueries {
 public:
  static constexpr uint32_t kNoPair = UINT32_MAX;
  // Host query pairs in flight at most (about three frames of draws inside queries).
  static constexpr uint32_t kPairs = 4096;

  bool Initialize(ID3D12Device* device);

  // Recording thread, in the order of the guest's calls.
  void Begin(uint32_t query);
  void End(uint32_t query, uint64_t frame_fence);
  // Whether a guest query is open: the next draw needs a pair.
  bool counting() const { return !open_.empty(); }
  // The pair for the next draw, or kNoPair when the ring is full (the draw is not counted).
  uint32_t Acquire();
  void BeginPass(ID3D12GraphicsCommandList* list, uint32_t pair) const;
  void EndPass(ID3D12GraphicsCommandList* list, uint32_t pair) const;
  void BeginCoverage(ID3D12GraphicsCommandList* list, uint32_t pair) const;
  // `drawn`: the coverage draw was issued; otherwise the pair's coverage is its ZPass (no depth
  // or stencil test to fail, or no pipeline for the coverage draw yet).
  void EndCoverage(ID3D12GraphicsCommandList* list, uint32_t pair, bool drawn);
  // At the Swap, into the frame's last list, which signals `frame_fence`.
  void Resolve(ID3D12GraphicsCommandList* list, uint64_t frame_fence);
  // After the Swap: the results of the guest queries whose frames completed, for WriteResult.
  void Retire(uint64_t completed_fence);

  // Guest thread, at GetData's entry: writes the query's latest result into its blocks, once the
  // guest fence passed and the blocks still wait for it.
  void WriteResult(uint8_t* base, uint32_t query);

 private:
  struct Open {
    uint32_t query;
    uint64_t first;  // pair
  };
  struct Ended {
    uint32_t query;
    uint64_t first, end;  // pairs [first, end)
    uint64_t fence;       // of the frame END was recorded in
    bool known;           // BEGIN was recorded (not before a renderer switch)
  };
  struct Result {
    uint64_t z_pass, coverage;
    bool known;
  };

  ComPtr<ID3D12QueryHeap> heap_;
  ComPtr<ID3D12Resource> readback_;
  const uint64_t* counts_ = nullptr;  // readback_, mapped: ZPass then coverage of each pair
  std::vector<uint8_t> coverage_drawn_;
  std::vector<Open> open_;
  std::deque<Ended> ended_;
  std::deque<std::pair<uint64_t, uint64_t>> resolves_;  // (pairs resolved up to, frame fence)
  uint64_t next_ = 0, resolved_ = 0, retired_ = 0;      // pairs, counted from the start

  std::mutex mutex_;
  std::unordered_map<uint32_t, Result> results_;  // the latest by query (mutex_)
};

}  // namespace pgr4::native
