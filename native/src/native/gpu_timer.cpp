// pgr4-native PGR4 native renderer: GPU time per pass of the native frame.
#include "gpu_timer.h"

#include "render_targets.h"
#include "resolve.h"

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace pgr4::native {

namespace {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
using enum rex::graphics::Register;  // XE_GPU_REG_*

constexpr uint32_t kMaxQueries = GpuTimer::kMaxSpans + 1;

// Key layout: kind in bits 0-2, the occurrence in the frame in bits 3-7 (set by Switch), then
// the kind's fields from bit 8 in the order the Key* functions write them.
// kShaders: draws keyed by their shader pair (per_shader_); counted as draws in the totals.
enum Kind : uint32_t { kStart, kDraw, kClear, kResolve, kPresent, kShaders, kSubmit, kKindCount };
constexpr const char* kKindNames[kKindCount] = {"start",   "draws",   "clears", "resolves",
                                                "present", "shaders", "submits"};
constexpr uint32_t kKindMask = 7;
constexpr uint32_t kOccurrenceShift = 3, kOccurrenceMax = 31;
constexpr uint32_t kFieldShift = 8;

class KeyWriter {
 public:
  explicit KeyWriter(Kind kind) : key_(kind) {}
  KeyWriter& Put(uint32_t value, uint32_t bits) {
    key_ |= uint64_t(value & ((1u << bits) - 1)) << shift_;
    shift_ += bits;
    return *this;
  }
  uint64_t key() const { return key_; }

 private:
  uint64_t key_;
  uint32_t shift_ = kFieldShift;
};

class KeyReader {
 public:
  explicit KeyReader(uint64_t key) : key_(key) {}
  uint32_t Get(uint32_t bits) {
    const uint32_t v = uint32_t(key_ >> shift_) & ((1u << bits) - 1);
    shift_ += bits;
    return v;
  }

 private:
  uint64_t key_;
  uint32_t shift_ = kFieldShift;
};

Kind KindOf(uint64_t key) { return Kind(key & kKindMask); }

const char* ColorFormatName(uint32_t f) {
  static constexpr const char* kNames[16] = {"k_8_8_8_8",
                                             "k_8_8_8_8_GAMMA",
                                             "k_2_10_10_10",
                                             "k_2_10_10_10_FLOAT",
                                             "k_16_16",
                                             "k_16_16_16_16",
                                             "k_16_16_FLOAT",
                                             "k_16_16_16_16_FLOAT",
                                             "fmt8",
                                             "fmt9",
                                             "k_2_10_10_10_AS_10_10_10_10",
                                             "fmt11",
                                             "k_2_10_10_10_FLOAT_AS_16_16_16_16",
                                             "fmt13",
                                             "k_32_FLOAT",
                                             "k_32_32_FLOAT"};
  return kNames[f & 15];
}
const char* DepthFormatName(uint32_t f) { return f ? "D24FS8" : "D24S8"; }

uint32_t Samples(uint32_t msaa) { return 1u << msaa; }

}  // namespace

std::string GpuTimer::Describe(uint64_t key) const {
  KeyReader r(key);
  char s[192];
  switch (KindOf(key)) {
    case kStart:
      return "list start (before the first pass)";
    case kDraw: {
      const uint32_t pitch = r.Get(14), msaa = r.Get(2), writes = r.Get(4), c0_format = r.Get(4),
                     c0_base = r.Get(12), depth_used = r.Get(1), depth_format = r.Get(1),
                     depth_base = r.Get(12);
      int n = std::snprintf(s, sizeof(s), "draws   pitch %u %ux", pitch, Samples(msaa));
      if (writes)
        n += std::snprintf(s + n, sizeof(s) - n, ", color rt0 %s@%u (writes %X)",
                           ColorFormatName(c0_format), c0_base, writes);
      if (depth_used)
        std::snprintf(s + n, sizeof(s) - n, ", depth %s@%u", DepthFormatName(depth_format),
                      depth_base);
      return s;
    }
    case kClear: {
      const uint32_t flags = r.Get(6), pitch = r.Get(14), msaa = r.Get(2), c0_format = r.Get(4),
                     c0_base = r.Get(12), depth_base = r.Get(12);
      std::snprintf(s, sizeof(s), "clear   pitch %u %ux, flags %02X, rt0 %s@%u, depth @%u", pitch,
                    Samples(msaa), flags, ColorFormatName(c0_format), c0_base, depth_base);
      return s;
    }
    case kResolve: {
      const uint32_t source = r.Get(3), msaa = r.Get(2), source_format = r.Get(4),
                     width = r.Get(13), height = r.Get(13), dest_format = r.Get(6),
                     clears = r.Get(2), phase = r.Get(2);
      static constexpr const char* kPhaseNames[4] = {"source", "copy", "release", "clear"};
      const auto* info = rex::graphics::FormatInfo::Get(dest_format);
      std::snprintf(s, sizeof(s), "resolve %s %ux%u %ux -> %s%s%s [%s]",
                    source == 4 ? DepthFormatName(source_format) : ColorFormatName(source_format),
                    width, height, Samples(msaa), info ? info->name : "?",
                    clears & 1 ? ", clears color" : "", clears & 2 ? ", clears depth" : "",
                    kPhaseNames[phase]);
      return s;
    }
    case kPresent:
      return "present blit";
    case kSubmit:
      return "submit  (the GPU waiting for the frame's next command list)";
    case kShaders: {
      const uint32_t pitch = r.Get(14), msaa = r.Get(2), id = r.Get(24);
      const auto [vs, ps] = id < pairs_.size() ? pairs_[id] : std::pair<uint64_t, uint64_t>{};
      std::snprintf(s, sizeof(s), "shaders VS %016llX PS %016llX, pitch %u %ux",
                    (unsigned long long)vs, (unsigned long long)ps, pitch, Samples(msaa));
      return s;
    }
    default:
      return "?";
  }
}

bool GpuTimer::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t slots) {
  if (Failed(queue->GetTimestampFrequency(&frequency_), "GetTimestampFrequency") || !frequency_)
    return false;
  D3D12_QUERY_HEAP_DESC qd = {};
  qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  qd.Count = slots * kMaxQueries;
  if (Failed(device->CreateQueryHeap(&qd, IID_PPV_ARGS(heap_.GetAddressOf())),
             "CreateQueryHeap(timestamp)"))
    return false;
  if (!CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, uint64_t(qd.Count) * sizeof(uint64_t),
                    D3D12_RESOURCE_STATE_COPY_DEST, "timestamp readback", &readback_))
    return false;
  frames_.resize(slots);
  for (Frame& f : frames_) f.spans.reserve(kMaxSpans);
  REXLOG_INFO("PGR4 native: GPU timing on ({} timestamp ticks/s, up to {} spans per frame)",
              frequency_, kMaxSpans);
  return true;
}

void GpuTimer::Stamp(ID3D12GraphicsCommandList* list) {
  Frame& f = frames_[slot_];
  list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * kMaxQueries + f.queries++);
}

void GpuTimer::BeginFrame(ID3D12GraphicsCommandList* list, uint32_t slot) {
  Collect(slot);
  slot_ = slot;
  Frame& f = frames_[slot];
  f.spans.clear();
  f.number = next_number_++;
  f.queries = 0;
  occurrences_.clear();
  current_ = kStart;
  Stamp(list);
  f.spans.push_back({kStart, 0});
}

void GpuTimer::Switch(ID3D12GraphicsCommandList* list, uint64_t key) {
  Frame& f = frames_[slot_];
  current_ = key;
  if (f.spans.size() >= kMaxSpans) {
    if (!overflow_logged_) {
      overflow_logged_ = true;
      REXLOG_WARN(
          "PGR4 native: GPU timing: more than {} passes in a frame; the rest are charged "
          "to the last one",
          kMaxSpans);
    }
    return;
  }
  Stamp(list);
  if (KindOf(key) == kShaders) {
    f.spans.push_back({key, 0});
    return;
  }
  uint32_t& n = occurrences_[key];
  f.spans.push_back({key | uint64_t(std::min(n, kOccurrenceMax)) << kOccurrenceShift, 0});
  ++n;
}

void GpuTimer::EndFrame(ID3D12GraphicsCommandList* list) {
  Frame& f = frames_[slot_];
  Stamp(list);
  list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * kMaxQueries, f.queries,
                         readback_.Get(), uint64_t(slot_) * kMaxQueries * sizeof(uint64_t));
  f.pending = true;
}

void GpuTimer::Collect(uint32_t slot) {
  Frame& f = frames_[slot];
  if (!f.pending) return;
  f.pending = false;
  const size_t first = size_t(slot) * kMaxQueries;
  const D3D12_RANGE range = {first * sizeof(uint64_t), (first + f.queries) * sizeof(uint64_t)};
  void* data = nullptr;
  if (FAILED(readback_->Map(0, &range, &data))) return;
  const uint64_t* ts = static_cast<const uint64_t*>(data) + first;
  const uint64_t begin = ts[0], end = ts[f.queries - 1];
  last_frame_ticks_ = end > begin ? end - begin : 0;
  const bool spike = IsSpike(last_frame_ticks_);
  if (spike) spike_spans_.clear();
  // One timestamp more than spans: span i runs from ts[i] to ts[i + 1].
  const size_t spans = std::min<size_t>(f.spans.size(), f.queries - 1);
  for (size_t i = 0; i < spans; ++i) {
    const uint64_t ticks = ts[i + 1] > ts[i] ? ts[i + 1] - ts[i] : 0;
    Group& g = groups_[f.spans[i].key];
    g.ticks += ticks;
    ++g.spans;
    g.commands += f.spans[i].commands;
    g.position += i;
    if (spike) spike_spans_.push_back({ticks, uint32_t(i)});
  }
  const D3D12_RANGE none = {0, 0};
  readback_->Unmap(0, &none);
  if (spike) LogSpike(f, last_frame_ticks_);
  if (end <= begin) return;
  ++frames_collected_;
  frame_ticks_ += end - begin;
  frame_ticks_max_ = std::max(frame_ticks_max_, end - begin);
  if (last_end_ && begin > last_end_) idle_ticks_ += begin - last_end_;
  last_end_ = end;
  spans_collected_ += spans;
}

bool GpuTimer::IsSpike(uint64_t ticks) {
  if (!ticks) return false;
  if (!typical_ticks_) {
    typical_ticks_ = ticks;
    return false;
  }
  // 1.75x the running mean and at least 5 ms more: a frame the player sees as a hitch.
  const bool spike = ticks > typical_ticks_ * 7 / 4 && ticks - typical_ticks_ > frequency_ / 200;
  const uint64_t clamped = std::min(ticks, typical_ticks_ * 2);
  typical_ticks_ = typical_ticks_ - typical_ticks_ / 32 + clamped / 32;
  return spike;
}

void GpuTimer::LogSpike(const Frame& f, uint64_t ticks) {
  constexpr uint32_t kSpikesPerWindow = 4, kSpansShown = 4;
  if (spikes_logged_ >= kSpikesPerWindow) return;
  ++spikes_logged_;
  const double ms_per_tick = 1000.0 / double(frequency_);
  const size_t shown = std::min<size_t>(kSpansShown, spike_spans_.size());
  std::partial_sort(spike_spans_.begin(), spike_spans_.begin() + shown, spike_spans_.end(),
                    [](const SpikeSpan& a, const SpikeSpan& b) { return a.ticks > b.ticks; });
  std::string spans;
  char item[256];
  for (size_t i = 0; i < shown; ++i) {
    const SpikeSpan& s = spike_spans_[i];
    const uint64_t key = f.spans[s.index].key;
    const Group& g = groups_[key];
    std::snprintf(item, sizeof(item), "%s%.2f ms (mean %.2f) span %u: %s", i ? " | " : "",
                  double(s.ticks) * ms_per_tick,
                  g.spans ? double(g.ticks) * ms_per_tick / double(g.spans) : 0.0, s.index,
                  Describe(key & ~(uint64_t(kOccurrenceMax) << kOccurrenceShift)).c_str());
    spans += item;
  }
  REXLOG_INFO("PGR4 native GPU spike: frame {} {:.2f} ms (typical {:.2f}), {} spans: {}", f.number,
              double(ticks) * ms_per_tick, double(typical_ticks_) * ms_per_tick,
              spike_spans_.size(), spans);
}

uint64_t GpuTimer::KeyDraw(const uint32_t* regs) {
  const reg::RB_SURFACE_INFO surface{.value = regs[XE_GPU_REG_RB_SURFACE_INFO]};
  const reg::RB_COLOR_INFO color0{.value = regs[XE_GPU_REG_RB_COLOR_INFO]};
  const reg::RB_DEPTH_INFO depth{.value = regs[XE_GPU_REG_RB_DEPTH_INFO]};
  const reg::RB_DEPTHCONTROL depth_control{.value = regs[XE_GPU_REG_RB_DEPTHCONTROL]};
  const uint32_t mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  uint32_t writes = 0;
  for (uint32_t i = 0; i < 4; ++i)
    if ((mask >> (4 * i)) & 15) writes |= 1u << i;
  return KeyWriter(kDraw)
      .Put(surface.surface_pitch, 14)
      .Put(uint32_t(surface.msaa_samples), 2)
      .Put(writes, 4)
      .Put(uint32_t(color0.color_format), 4)
      .Put(color0.value & 0xFFF, 12)
      .Put(depth_control.z_enable || depth_control.stencil_enable, 1)
      .Put(uint32_t(depth.depth_format), 1)
      .Put(depth.value & 0xFFF, 12)
      .key();
}

uint64_t GpuTimer::KeyClear(const ClearCall& c) {
  const reg::RB_SURFACE_INFO surface{.value = c.regs.surface_info};
  const reg::RB_COLOR_INFO color0{.value = c.regs.color_info[0]};
  return KeyWriter(kClear)
      .Put(c.p.flags, 6)
      .Put(surface.surface_pitch, 14)
      .Put(uint32_t(surface.msaa_samples), 2)
      .Put(uint32_t(color0.color_format), 4)
      .Put(color0.value & 0xFFF, 12)
      .Put(c.regs.depth_info & 0xFFF, 12)
      .key();
}

uint64_t GpuTimer::KeyResolve(const ResolveParams& p, ResolvePhase phase) {
  const reg::RB_SURFACE_INFO surface{.value = p.regs.surface_info};
  const uint32_t source_format =
      p.source == 4
          ? uint32_t(reg::RB_DEPTH_INFO{.value = p.regs.depth_info}.depth_format)
          : uint32_t(reg::RB_COLOR_INFO{.value = p.regs.color_info[p.source & 3]}.color_format);
  return KeyWriter(kResolve)
      .Put(p.source, 3)
      .Put(uint32_t(surface.msaa_samples), 2)
      .Put(source_format, 4)
      .Put(uint32_t(std::clamp(p.x1 - p.x0, 0, 8191)), 13)
      .Put(uint32_t(std::clamp(p.y1 - p.y0, 0, 8191)), 13)
      .Put(p.dest_format, 6)
      .Put((p.clear_flags >> 8) & 3, 2)
      .Put(phase, 2)
      .key();
}

uint64_t GpuTimer::KeyPresent() { return KeyWriter(kPresent).key(); }

uint64_t GpuTimer::KeySubmit() { return KeyWriter(kSubmit).key(); }

uint64_t GpuTimer::KeyDrawShaders(const uint32_t* regs, uint64_t vs_hash, uint64_t ps_hash) {
  const reg::RB_SURFACE_INFO surface{.value = regs[XE_GPU_REG_RB_SURFACE_INFO]};
  const auto [it, added] =
      pair_ids_.try_emplace(vs_hash ^ (ps_hash * 0x9E3779B97F4A7C15ull), uint32_t(pairs_.size()));
  if (added) pairs_.emplace_back(vs_hash, ps_hash);
  return KeyWriter(kShaders)
      .Put(surface.surface_pitch, 14)
      .Put(uint32_t(surface.msaa_samples), 2)
      .Put(it->second, 24)
      .key();
}

void GpuTimer::Log() {
  if (!frames_collected_) return;
  const double n = double(frames_collected_);
  const double ms_per_tick = 1000.0 / double(frequency_);
  const auto ms = [&](uint64_t ticks) { return double(ticks) * ms_per_tick / n; };
  double kind_ms[kKindCount] = {};
  std::vector<std::pair<uint64_t, const Group*>> top;
  top.reserve(groups_.size());
  for (const auto& [key, g] : groups_) {
    kind_ms[KindOf(key) == kShaders ? kDraw : KindOf(key)] += ms(g.ticks);
    top.emplace_back(key, &g);
  }
  REXLOG_INFO(
      "PGR4 native GPU: frames {} | GPU {:.2f} ms/frame (max {:.2f}), idle between "
      "frames {:.2f} ms, spans/frame {:.0f} | {} {:.2f}, {} {:.2f}, {} {:.2f}, {} {:.2f}, "
      "{} {:.2f}, {} {:.2f} ms",
      frames_collected_, ms(frame_ticks_), double(frame_ticks_max_) * ms_per_tick, ms(idle_ticks_),
      double(spans_collected_) / n, kKindNames[kDraw], kind_ms[kDraw], kKindNames[kClear],
      kind_ms[kClear], kKindNames[kResolve], kind_ms[kResolve], kKindNames[kPresent],
      kind_ms[kPresent], kKindNames[kSubmit], kind_ms[kSubmit], kKindNames[kStart],
      kind_ms[kStart]);
  // The costliest groups, in frame order.
  constexpr size_t kTop = 24;
  const size_t shown = std::min(kTop, top.size());
  std::partial_sort(top.begin(), top.begin() + shown, top.end(),
                    [](const auto& a, const auto& b) { return a.second->ticks > b.second->ticks; });
  top.resize(shown);
  std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
    return double(a.second->position) / double(a.second->spans) <
           double(b.second->position) / double(b.second->spans);
  });
  const double frame_ms = ms(frame_ticks_);
  for (const auto& [key, g] : top) {
    const uint32_t occurrence = uint32_t(key >> kOccurrenceShift) & kOccurrenceMax;
    REXLOG_INFO(
        "PGR4 native GPU: {:6.3f} ms {:5.1f} %  span {:4.0f}  #{:<2} x{:.2f}  {:5.0f} cmds  {}",
        ms(g->ticks), frame_ms > 0 ? 100.0 * ms(g->ticks) / frame_ms : 0.0,
        double(g->position) / double(g->spans), occurrence, double(g->spans) / n,
        double(g->commands) / n, Describe(key & ~(uint64_t(kOccurrenceMax) << kOccurrenceShift)));
    if (KindOf(key) != kShaders) continue;
    const uint32_t id = uint32_t(key >> (kFieldShift + 16)) & 0xFFFFFF;
    if (id >= pairs_.size()) continue;
    for (uint64_t hash : {pairs_[id].first, pairs_[id].second})
      if (hash && shaders_reported_.insert(hash).second) new_top_shaders_.push_back(hash);
  }
  groups_.clear();
  frames_collected_ = frame_ticks_ = frame_ticks_max_ = idle_ticks_ = spans_collected_ = 0;
  spikes_logged_ = 0;
}

}  // namespace pgr4::native
