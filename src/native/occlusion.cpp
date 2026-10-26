// pgr4-native PGR4 native renderer: the game's occlusion queries, counted on the host.
#include "occlusion.h"

#include "guest_memory.h"
#include "log.h"

#include <rex/logging.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace pgr4::native {

namespace {

// The guest D3D's query object (QueryIssue sub_826D2F00, GetData sub_826D3188).
constexpr uint32_t kQueryDevice = 0;   // the D3D device
constexpr uint32_t kQueryFence = 24;   // the device fence the last Issue was written before
constexpr uint32_t kQueryBlocks = 28;  // report block addresses, one per EDRAM tile
constexpr uint32_t kQueryBlockCount = 148;
constexpr uint32_t kMaxBlocks = 15;  // render_targets.h kMaxTiles
// The guest D3D device: the fence its next kick writes, and where the GPU writes the last one.
constexpr uint32_t kDevFenceNext = 10908, kDevFenceDone = 10896;

// xe_gpu_depth_sample_counts as the GPU writes it: little-endian, each counter in two lanes that
// the D3D adds.
struct SampleCounts {
  uint32_t total[2], z_fail[2], z_pass[2], stencil_fail[2];
};
static_assert(sizeof(SampleCounts) == 32);
constexpr uint32_t kEndReport = 0, kBeginReport = 32, kBlockSize = 64;
// END's 0xFFFFFEED, which the D3D stores big-endian, read as a little-endian lane.
constexpr uint32_t kPendingLane = 0xEDFEFFFFu;

// Canary's XenosZPDReport::WriteTo split: a count across the A and B lanes, low 32 bits.
void SetLanes(uint32_t lanes[2], uint64_t count) {
  lanes[0] = uint32_t(count) - (uint32_t(count) >> 1);
  lanes[1] = uint32_t(count) >> 1;
}

// A host count in guest samples, rounded to nearest and at least 1 when not 0 (Canary's
// XenosZPDReport::Normalized for its draw resolution scale).
uint64_t GuestCount(uint64_t count, float guest_per_host) {
  if (!count || guest_per_host == 1.0f) return count;
  return std::max<uint64_t>(1, uint64_t(double(count) * guest_per_host + 0.5));
}

}  // namespace

bool OcclusionQueries::Initialize(ID3D12Device* device) {
  D3D12_QUERY_HEAP_DESC qd = {};
  qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  qd.Count = 2 * kPairs;
  if (Failed(device->CreateQueryHeap(&qd, IID_PPV_ARGS(heap_.GetAddressOf())),
             "CreateQueryHeap(occlusion)"))
    return false;
  const uint64_t size = uint64_t(qd.Count) * sizeof(uint64_t);
  if (!CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST,
                    "occlusion readback", &readback_))
    return false;
  // Readback memory stays mapped; each pair is read only after the fence of its resolve.
  const D3D12_RANGE all = {0, size_t(size)};
  void* data = nullptr;
  if (Failed(readback_->Map(0, &all, &data), "Map(occlusion readback)")) return false;
  counts_ = static_cast<const uint64_t*>(data);
  coverage_drawn_.assign(kPairs, 0);
  guest_per_host_.assign(kPairs, 1.0f);
  REXLOG_INFO("PGR4 native: the game's occlusion queries are counted on the host");
  return true;
}

void OcclusionQueries::Begin(uint32_t query) {
  for (Open& o : open_) {
    if (o.query == query) {
      o.first = next_;
      return;
    }
  }
  open_.push_back({query, next_});
}

void OcclusionQueries::End(uint32_t query, uint64_t frame_fence) {
  Ended e = {query, next_, next_, frame_fence, false};
  for (auto it = open_.begin(); it != open_.end(); ++it) {
    if (it->query == query) {
      e.first = it->first;
      e.known = true;
      open_.erase(it);
      break;
    }
  }
  ended_.push_back(e);
}

uint32_t OcclusionQueries::Acquire() {
  // A pair's slot is free again once every guest query that sums it has its result.
  uint64_t oldest = retired_;
  for (const Open& o : open_) oldest = std::min(oldest, o.first);
  for (const Ended& e : ended_) oldest = std::min(oldest, e.first);
  if (next_ - oldest >= kPairs) {
    PGR4_WARN_ONCE("PGR4 native: occlusion query ring full; draws inside queries go uncounted");
    return kNoPair;
  }
  return uint32_t(next_++ % kPairs);
}

void OcclusionQueries::BeginPass(ID3D12GraphicsCommandList* list, uint32_t pair) const {
  list->BeginQuery(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, 2 * pair);
}

void OcclusionQueries::EndPass(ID3D12GraphicsCommandList* list, uint32_t pair) const {
  list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, 2 * pair);
}

void OcclusionQueries::BeginCoverage(ID3D12GraphicsCommandList* list, uint32_t pair) const {
  list->BeginQuery(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, 2 * pair + 1);
}

void OcclusionQueries::EndCoverage(ID3D12GraphicsCommandList* list, uint32_t pair, bool drawn,
                                   float guest_per_host) {
  // Ended even without a draw: every query of a resolved range must have been issued.
  list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, 2 * pair + 1);
  coverage_drawn_[pair] = drawn;
  guest_per_host_[pair] = guest_per_host;
}

void OcclusionQueries::Resolve(ID3D12GraphicsCommandList* list, uint64_t frame_fence) {
  if (resolved_ == next_) return;
  const uint64_t n = next_ - resolved_;
  const uint32_t first = uint32_t(resolved_ % kPairs);
  const uint32_t head = uint32_t(std::min<uint64_t>(n, kPairs - first));
  list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, 2 * first, 2 * head,
                         readback_.Get(), uint64_t(first) * 2 * sizeof(uint64_t));
  if (n > head)
    list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, uint32_t(2 * (n - head)),
                           readback_.Get(), 0);
  resolves_.emplace_back(next_, frame_fence);
  resolved_ = next_;
}

void OcclusionQueries::Retire(uint64_t completed_fence) {
  while (!resolves_.empty() && resolves_.front().second <= completed_fence) {
    retired_ = resolves_.front().first;
    resolves_.pop_front();
  }
  // A frame's Swap resolved every pair recorded up to its END: its fence covers them.
  if (ended_.empty() || ended_.front().fence > completed_fence) return;
  std::lock_guard<std::mutex> lock(mutex_);
  while (!ended_.empty() && ended_.front().fence <= completed_fence) {
    const Ended& e = ended_.front();
    Result r = {0, 0, e.known};
    for (uint64_t p = e.first; p < e.end; ++p) {
      const uint32_t i = uint32_t(p % kPairs);
      const uint64_t z_pass = counts_[2 * i];
      const uint64_t coverage = coverage_drawn_[i] ? std::max(counts_[2 * i + 1], z_pass) : z_pass;
      r.z_pass += GuestCount(z_pass, guest_per_host_[i]);
      r.coverage += GuestCount(coverage, guest_per_host_[i]);
    }
    results_[e.query] = r;
    ended_.pop_front();
  }
}

void OcclusionQueries::WriteResult(uint8_t* base, uint32_t query) {
  // GetData's own test: the reports are not read before the GPU passed the query's fence.
  if (const uint32_t fence = Load32(base, query + kQueryFence)) {
    const uint32_t device = Load32(base, query + kQueryDevice);
    const uint32_t next = Load32(base, device + kDevFenceNext);
    const uint32_t done = Load32(base, Load32(base, device + kDevFenceDone));
    if (next - fence < next - done) return;
  }
  const uint32_t blocks = Load32(base, query + kQueryBlockCount);
  if (!blocks || blocks > kMaxBlocks) return;
  auto block = [&](uint32_t i) {
    // GetData's view of the block: the GPU address in the 0xC0000000 view.
    return HostPtr(base, 0xC0000000u + GpuAddress(Load32(base, query + kQueryBlocks + 4 * i)));
  };
  uint8_t* const last = block(blocks - 1);
  uint32_t pending[2];
  std::memcpy(pending, last + kEndReport + offsetof(SampleCounts, z_pass), sizeof(pending));
  if (pending[0] != kPendingLane || pending[1] != kPendingLane) return;
  Result r;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = results_.find(query);
    if (it == results_.end()) return;  // none counted yet: GetData returns S_FALSE
    r = it->second;
  }
  // No BEGIN seen (recorded before a renderer switch): a Total of 1, which the flares skip.
  if (!r.known) r.z_pass = r.coverage = 1;
  for (uint32_t i = 0; i + 1 < blocks; ++i) std::memset(block(i), 0, kBlockSize);
  std::memset(last + kBeginReport, 0, sizeof(SampleCounts));
  // Rejected samples go in ZFail: the host cannot tell them from StencilFail (Canary's split).
  SampleCounts end = {};
  SetLanes(end.total, r.coverage);
  SetLanes(end.z_fail, r.coverage - r.z_pass);
  SetLanes(end.z_pass, r.z_pass);
  // The ZPass lanes, which GetData waits on, last.
  std::memcpy(last + kEndReport, &end, offsetof(SampleCounts, z_pass));
  std::memcpy(last + kEndReport + offsetof(SampleCounts, z_pass), &end.z_pass,
              sizeof(SampleCounts) - offsetof(SampleCounts, z_pass));
}

}  // namespace pgr4::native
