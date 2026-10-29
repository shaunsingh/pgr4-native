// pgr4-native PGR4 native renderer: the game's occlusion queries, counted on the host.
#include "occlusion.h"

#include "clock.h"
#include "guest_memory.h"
#include "log.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

REXCVAR_DEFINE_STRING(pgr4_native_occlusion_trace, "", "PGR4 native",
                      "Log every occlusion query result the game reads to this file: frame, query "
                      "and its ZPass and Total, or why none was written");
REXCVAR_DEFINE_INT32(pgr4_native_occlusion_window_ms, 100, "PGR4 native",
                     "Milliseconds of an occlusion query's newest results whose ZPass and Total "
                     "sums it delivers (0-300, 0 = the latest frame only). A light flare's test "
                     "covers a few pixels, so one frame's result is nearly all or nothing; the "
                     "game fades the flare by 1 - ZPass / Total, and the sum makes that a fade "
                     "over the window instead of a blink");

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
  coverage_.assign(kPairs, 0);
  guest_per_host_.assign(kPairs, 1.0f);
  if (const std::string& trace = REXCVAR_GET(pgr4_native_occlusion_trace); !trace.empty()) {
    trace_ = std::fopen(trace.c_str(), "wb");
    if (!trace_) REXLOG_ERROR("PGR4 native: cannot write the occlusion trace ({})", trace);
  }
  window_ns_ = uint64_t(std::clamp(REXCVAR_GET(pgr4_native_occlusion_window_ms), 0, 300)) * 1000000;
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

void OcclusionQueries::End(uint32_t query, uint64_t frame, uint64_t frame_fence) {
  Ended e = {query, next_, next_, frame_fence, Ticks(), false};
  for (auto it = open_.begin(); it != open_.end(); ++it) {
    if (it->query == query) {
      e.first = it->first;
      e.known = true;
      open_.erase(it);
      break;
    }
  }
  // The query's pair range: joins the draw logs' occ lines to this query.
  if (trace_)
    std::fprintf(trace_, "%llu %08X end %llu %llu\n", (unsigned long long)frame, query,
                 (unsigned long long)e.first, (unsigned long long)e.end);
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

void OcclusionQueries::EndCoverage(ID3D12GraphicsCommandList* list, uint32_t pair,
                                   Coverage coverage, float guest_per_host) {
  // Ended even without a draw: every query of a resolved range must have been issued.
  list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, 2 * pair + 1);
  coverage_[pair] = uint8_t(coverage);
  guest_per_host_[pair] = guest_per_host;
  unknown_pairs_ += coverage == Coverage::kUnknown;
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
    uint64_t z_pass = 0, coverage = 0;
    bool counted = false;
    for (uint64_t p = e.first; p < e.end; ++p) {
      const uint32_t i = uint32_t(p % kPairs);
      if (coverage_[i] == uint8_t(Coverage::kUnknown)) continue;
      const uint64_t pair_z_pass = counts_[2 * i];
      const uint64_t pair_coverage = coverage_[i] == uint8_t(Coverage::kCounted)
                                         ? std::max(counts_[2 * i + 1], pair_z_pass)
                                         : pair_z_pass;
      z_pass += GuestCount(pair_z_pass, guest_per_host_[i]);
      coverage += GuestCount(pair_coverage, guest_per_host_[i]);
      counted = true;
    }
    // The underlying per-frame result, before the windowed delivery (the trace's "result"
    // lines are what the game reads; these are what the GPU measured).
    if (trace_ && e.known)
      std::fprintf(trace_, "%llu %08X retire %llu %llu\n", (unsigned long long)e.fence, e.query,
                   (unsigned long long)z_pass, (unsigned long long)coverage);
    if (!e.known) {
      results_[e.query] = Result{};
    } else if (counted || e.first == e.end) {
      // Every pair's coverage unknown: keep the query's previous window rather than push a
      // Total of its ZPass, which would show its flares at full brightness for these frames.
      Result& d = results_[e.query];
      d.win[d.win_next] = {z_pass, coverage, e.ticks};
      d.win_next = (d.win_next + 1) % kWindowMax;
      if (d.win_count < kWindowMax) ++d.win_count;
      d.known = true;
    }
    ended_.pop_front();
  }
}

void OcclusionQueries::WriteResult(uint8_t* base, uint32_t query, uint64_t frame) {
  auto trace = [&](const char* what, uint64_t z_pass, uint64_t coverage) {
    if (trace_)
      std::fprintf(trace_, "%llu %08X %s %llu %llu\n", (unsigned long long)frame, query, what,
                   (unsigned long long)z_pass, (unsigned long long)coverage);
  };
  // GetData's own test: the reports are not read before the GPU passed the query's fence.
  if (const uint32_t fence = Load32(base, query + kQueryFence)) {
    const uint32_t device = Load32(base, query + kQueryDevice);
    const uint32_t next = Load32(base, device + kDevFenceNext);
    const uint32_t done = Load32(base, Load32(base, device + kDevFenceDone));
    if (next - fence < next - done) return trace("fence", 0, 0);
  }
  const uint32_t blocks = Load32(base, query + kQueryBlockCount);
  if (!blocks || blocks > kMaxBlocks) return trace("blocks", 0, 0);
  auto block = [&](uint32_t i) {
    // GetData's view of the block: the GPU address in the 0xC0000000 view.
    return HostPtr(base, 0xC0000000u + GpuAddress(Load32(base, query + kQueryBlocks + 4 * i)));
  };
  uint8_t* const last = block(blocks - 1);
  uint32_t pending[2];
  std::memcpy(pending, last + kEndReport + offsetof(SampleCounts, z_pass), sizeof(pending));
  if (pending[0] != kPendingLane || pending[1] != kPendingLane) return trace("read", 0, 0);
  uint64_t z_pass = 0, coverage = 0;
  bool known;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = results_.find(query);
    if (it == results_.end()) return trace("none", 0, 0);  // none counted yet: S_FALSE
    const Result& d = it->second;
    known = d.known;
    // The sums of the window's frames: the average visibility the game fades by.
    const uint32_t newest = (d.win_next + kWindowMax - 1) % kWindowMax;
    for (uint32_t i = 0; i < d.win_count; ++i) {
      const Frame& f = d.win[(newest + kWindowMax - i) % kWindowMax];
      if (i && TicksToNs(d.win[newest].ticks - f.ticks) >= window_ns_) break;
      z_pass += f.z_pass;
      coverage += f.total;
    }
  }
  // No BEGIN seen (recorded before a renderer switch): a Total of 1, which the flares skip.
  if (!known) z_pass = coverage = 1;
  trace("result", z_pass, coverage);
  for (uint32_t i = 0; i + 1 < blocks; ++i) std::memset(block(i), 0, kBlockSize);
  std::memset(last + kBeginReport, 0, sizeof(SampleCounts));
  // Rejected samples go in ZFail: the host cannot tell them from StencilFail (Canary's split).
  SampleCounts end = {};
  SetLanes(end.total, coverage);
  SetLanes(end.z_fail, coverage - z_pass);
  SetLanes(end.z_pass, z_pass);
  // The ZPass lanes, which GetData waits on, last.
  std::memcpy(last + kEndReport, &end, offsetof(SampleCounts, z_pass));
  std::memcpy(last + kEndReport + offsetof(SampleCounts, z_pass), &end.z_pass,
              sizeof(SampleCounts) - offsetof(SampleCounts, z_pass));
}

}  // namespace pgr4::native
