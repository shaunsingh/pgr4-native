// pgr4-native PGR4 native renderer: the presenter's debug tools, the PNG writer of captures,
// the glitch catcher and the luma probe.
#include "present_debug.h"

#include "clock.h"
#include "d3d12_device.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_STRING(pgr4_native_glitch_catch, "", "PGR4 native",
                      "Keep the frames shown, at half size and after the gamma ramp, in memory "
                      "and write <this prefix>glitch-<frame>.rec when 1 to 4 frames jump away "
                      "from the image and back, and <prefix>mark-<frame>.rec with the last 5 s "
                      "at a mark (F12, with --pgr4_native_capture_dir); native/tools/flicker.py "
                      "and marks.py show them");

namespace pgr4::native::present_debug {

namespace {

// The row pitch of `width` 32bpp texels in a placed footprint.
uint32_t RowPitch32(uint32_t width) {
  constexpr uint32_t kAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  return (width * 4 + kAlign - 1) & ~(kAlign - 1);
}

// ---- PNG ----

constexpr std::array<uint32_t, 256> kCrcTable = [] {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    table[i] = c;
  }
  return table;
}();

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; ++i) crc = kCrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

void PutBe32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24));
  v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}

void Chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
  PutBe32(out, uint32_t(data.size()));
  size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  PutBe32(out, Crc32(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
}

// ---- Glitch catcher ----

// A .rec file holds, per frame, a RecordHeader and height rows of width RGB texels.
struct RecordHeader {
  char magic[4] = {'P', 'G', 'R', 'F'};
  uint32_t width = 0, height = 0, reserved = 0;
  uint64_t frame = 0, time_us = 0;
};
static_assert(sizeof(RecordHeader) == 32);

// On the glitch catcher's thread: the frames it is handed, in a ring, and the whole-image jumps
// of native/tools/flicker.py --jumps found as they arrive (a frame whose mean block difference
// from the one before is at least 6x the median of the last kSteps and 15, followed within
// kMaxLen frames by one that is back near the frame before).
struct Catcher {
  // kRing: 5 s at 90 Hz, since a mark comes a few seconds after what it marks.
  static constexpr uint32_t kRing = 450, kBlock = 10, kMaxLen = 4, kPad = 8, kSteps = 512;
  static constexpr uint32_t kMaxDumps = 400, kMaxMarks = 30;  // ~14 MB and ~310 MB each
  struct Frame {
    RecordHeader h;
    std::vector<uint8_t> rgb;
    std::vector<float> means;  // per kBlock x kBlock block and channel
    float step = 0, floor = 0;
  };
  std::string prefix;
  std::vector<Frame> ring = std::vector<Frame>(kRing);
  uint64_t count = 0;     // frames added; frame n is ring[n % kRing]
  uint64_t consumed = 0;  // frames before this are part of a reported jump
  uint64_t dump_from = 0, dump_to = 0, dump_label = 0;
  bool dump_pending = false;
  std::deque<float> steps;
  std::vector<float> scratch;
  std::vector<uint32_t> sums;
  uint32_t dumps = 0, marks = 0;

  Frame& At(uint64_t n) { return ring[n % kRing]; }
  // Frames a..b (a <= b, both in the ring) are consecutive frames shown.
  bool Contiguous(uint64_t a, uint64_t b) { return At(b).h.frame - At(a).h.frame == b - a; }
  static float Diff(const Frame& a, const Frame& b);
  void Add(const RecordHeader& h, const uint8_t* rgba);
  void Mark(uint64_t frame);
  void Dump(uint64_t from, uint64_t to, const char* kind, uint64_t label);
};

float Catcher::Diff(const Frame& a, const Frame& b) {
  const size_t n = a.means.size();
  if (!n || b.means.size() != n) return 0.0f;
  float sum = 0.0f;
  for (size_t i = 0; i < n; ++i) sum += std::fabs(a.means[i] - b.means[i]);
  return sum / float(n);
}

void Catcher::Add(const RecordHeader& h, const uint8_t* rgba) {
  const uint64_t n = count++;
  Frame& f = At(n);
  f.h = h;
  const size_t px = size_t(h.width) * h.height;
  f.rgb.resize(px * 3);
  for (size_t i = 0; i < px; ++i) std::memcpy(&f.rgb[i * 3], rgba + i * 4, 3);
  const uint32_t bx = h.width / kBlock, by = h.height / kBlock;
  sums.assign(size_t(bx) * by * 3, 0);
  for (uint32_t y = 0; y < by * kBlock; ++y) {
    const uint8_t* row = f.rgb.data() + size_t(y) * h.width * 3;
    uint32_t* out = sums.data() + size_t(y / kBlock) * bx * 3;
    for (uint32_t x = 0; x < bx * kBlock; ++x)
      for (uint32_t c = 0; c < 3; ++c) out[(x / kBlock) * 3 + c] += row[x * 3 + c];
  }
  f.means.resize(sums.size());
  for (size_t i = 0; i < sums.size(); ++i) f.means[i] = float(sums[i]) / float(kBlock * kBlock);

  f.step = n && Contiguous(n - 1, n) ? Diff(f, At(n - 1)) : 0.0f;
  float median = 0.0f;
  if (!steps.empty()) {
    scratch.assign(steps.begin(), steps.end());
    auto mid = scratch.begin() + scratch.size() / 2;
    std::nth_element(scratch.begin(), mid, scratch.end());
    median = *mid;
  }
  f.floor = std::max(15.0f, 6.0f * median);
  if (f.step > 0.0f) {
    steps.push_back(f.step);
    if (steps.size() > kSteps) steps.pop_front();
  }
  // A jump of frames i..n-1, back at n: the earliest start first, each frame in one jump.
  for (uint64_t i = n >= kMaxLen ? n - kMaxLen : 1; i < n; ++i) {
    if (i < 1 || i < consumed || !Contiguous(i - 1, n)) continue;
    const Frame& s = At(i);
    if (s.step < s.floor || s.floor <= 0.0f) continue;
    const float back = Diff(f, At(i - 1));
    if (back * 2.0f >= s.step) continue;
    consumed = n + 1;
    REXLOG_INFO("PGR4 native glitch catch: frames {}-{} ({}) jump {:.1f} ({:.1f}x), back to {:.1f}",
                At(i).h.frame, At(n - 1).h.frame, n - i, s.step, s.step / s.floor, back);
    const uint64_t from = i > kPad ? i - kPad : 0;
    if (!dump_pending) {
      dump_pending = true;
      dump_from = std::max(from, count > kRing ? count - kRing : 0);
      dump_label = s.h.frame;
    }
    dump_to = std::min(n + kPad, dump_from + kRing - 1);
    break;
  }
  if (dump_pending && n >= dump_to) {
    dump_pending = false;
    if (dumps < kMaxDumps) {
      ++dumps;
      Dump(dump_from, n, "glitch", dump_label);
    }
  }
}

void Catcher::Mark(uint64_t frame) {
  if (!count || marks >= kMaxMarks) return;
  ++marks;
  const uint64_t last = count - 1;
  Dump(count > kRing ? count - kRing : 0, last, "mark", frame);
}

void Catcher::Dump(uint64_t from, uint64_t to, const char* kind, uint64_t label) {
  const std::string path = prefix + kind + "-" + std::to_string(label) + ".rec";
  std::FILE* out = std::fopen(path.c_str(), "wb");
  if (!out) {
    REXLOG_ERROR("PGR4 native glitch catch: cannot open {}", path);
    return;
  }
  bool ok = true;
  for (uint64_t n = from; n <= to && ok; ++n) {
    const Frame& f = At(n);
    ok = std::fwrite(&f.h, sizeof(f.h), 1, out) == 1 &&
         std::fwrite(f.rgb.data(), 1, f.rgb.size(), out) == f.rgb.size();
  }
  std::fclose(out);
  REXLOG_INFO("PGR4 native glitch catch: frames {}-{} {} {}", At(from).h.frame, At(to).h.frame,
              ok ? "written to" : "failed writing", path);
}

}  // namespace

void ToRgb(const uint8_t* src, uint32_t pitch, uint32_t width, uint32_t height, uint32_t mapping,
           bool rgb10a2, std::vector<uint8_t>* rgb) {
  rgb->resize(size_t(width) * height * 3);
  uint32_t m[3];
  for (uint32_t c = 0; c < 3; ++c) m[c] = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(c, mapping);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row = src + size_t(y) * pitch;
    uint8_t* out = rgb->data() + size_t(y) * width * 3;
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t t;
      std::memcpy(&t, row + x * 4, 4);
      uint8_t ch[4];
      if (rgb10a2) {
        for (uint32_t c = 0; c < 3; ++c)
          ch[c] = uint8_t((((t >> (10 * c)) & 1023) * 255 + 511) / 1023);
        ch[3] = uint8_t((t >> 30) * 85);
      } else {
        std::memcpy(ch, &t, 4);
      }
      for (uint32_t c = 0; c < 3; ++c) out[x * 3 + c] = m[c] < 4 ? ch[m[c]] : (m[c] == 5 ? 255 : 0);
    }
  }
}

bool WritePng(const wchar_t* path, const std::vector<uint8_t>& rgb, uint32_t width,
              uint32_t height) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (width * 3 + 1));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + size_t(y) * width * 3,
               rgb.begin() + size_t(y + 1) * width * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t off = 0; off < raw.size() || off == 0;) {
    size_t n = std::min<size_t>(65535, raw.size() - off);
    z.push_back(off + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    off += n;
    if (!n) break;
  }
  PutBe32(z, (b << 16) | a);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PutBe32(ihdr, width);
  PutBe32(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  Chunk(out, "IHDR", ihdr);
  Chunk(out, "IDAT", z);
  Chunk(out, "IEND", {});
  std::FILE* f = _wfopen(path, L"wb");
  if (!f) return false;
  bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

struct GlitchCatch::State {
  bool failed = false;
  Catcher catcher;
  std::atomic<uint64_t> mark_request{0};  // frame + 1: dump the catcher's ring
  ComPtr<ID3D12Resource> target, readback;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  const uint8_t* mapped = nullptr;
  uint32_t width = 0, height = 0, pitch = 0;
  uint64_t slot_bytes = 0;
  struct Slot {
    uint64_t frame = 0, time_us = 0;
    bool recorded = false;
  } slots[D3D12Device::kFramesInFlight];
  std::thread writer;
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::vector<uint8_t>> queue;   // a RecordHeader, then RGBA rows (width * 4 bytes)
  std::vector<std::vector<uint8_t>> spare;  // consumed buffers, reused
  bool stopping = false;
  uint64_t written = 0, dropped = 0;
  static constexpr size_t kMaxQueued = 96;

  ~State() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    cv.notify_all();
    if (writer.joinable()) writer.join();
  }
  bool Start(ID3D12Device* device, uint32_t w, uint32_t h);
  void Push(const Slot& s, uint32_t slot);
  void Write();
};

bool GlitchCatch::State::Start(ID3D12Device* device, uint32_t w, uint32_t h) {
  width = w;
  height = h;
  pitch = RowPitch32(w);
  slot_bytes = (uint64_t(pitch) * h + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
               ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC td = {};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = w;
  td.Height = h;
  td.DepthOrArraySize = td.MipLevels = 1;
  td.Format = D3D12Device::kBackBufferFormat;
  td.SampleDesc.Count = 1;
  td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  void* p = nullptr;
  if (Failed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                             IID_PPV_ARGS(&target)),
             "glitch catch target") ||
      !CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, slot_bytes * D3D12Device::kFramesInFlight,
                    D3D12_RESOURCE_STATE_COPY_DEST, "glitch catch readback", &readback) ||
      Failed(readback->Map(0, nullptr, &p), "glitch catch readback map") ||
      Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)), "glitch catch heap"))
    return false;
  mapped = static_cast<const uint8_t*>(p);
  device->CreateRenderTargetView(target.Get(), nullptr,
                                 rtv_heap->GetCPUDescriptorHandleForHeapStart());
  writer = std::thread([this] { Write(); });
  SetThreadDescription(writer.native_handle(), L"PGR4 native glitch catch");
  REXLOG_INFO(
      "PGR4 native glitch catch: the last {} frames at {}x{} kept, jumps written to "
      "{}glitch-<frame>.rec",
      Catcher::kRing, w, h, catcher.prefix);
  return true;
}

void GlitchCatch::State::Push(const Slot& s, uint32_t slot) {
  std::vector<uint8_t> buf;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (queue.size() >= kMaxQueued) {
      ++dropped;
      return;
    }
    if (!spare.empty()) {
      buf = std::move(spare.back());
      spare.pop_back();
    }
  }
  const size_t row = size_t(width) * 4;
  buf.resize(sizeof(RecordHeader) + row * height);
  RecordHeader h;
  h.width = width;
  h.height = height;
  h.frame = s.frame;
  h.time_us = s.time_us;
  std::memcpy(buf.data(), &h, sizeof(h));
  const uint8_t* src = mapped + slot_bytes * slot;
  for (uint32_t y = 0; y < height; ++y)
    std::memcpy(buf.data() + sizeof(h) + row * y, src + size_t(pitch) * y, row);
  {
    std::lock_guard<std::mutex> lock(mutex);
    queue.push_back(std::move(buf));
  }
  cv.notify_one();
}

void GlitchCatch::State::Write() {
  std::unique_lock<std::mutex> lock(mutex);
  for (;;) {
    cv.wait(lock, [this] { return stopping || !queue.empty(); });
    if (queue.empty()) break;
    std::vector<uint8_t> buf = std::move(queue.front());
    queue.pop_front();
    lock.unlock();
    RecordHeader h;
    std::memcpy(&h, buf.data(), sizeof(h));
    catcher.Add(h, buf.data() + sizeof(h));  // RGBA8 -> RGB8 into its ring
    if (const uint64_t m = mark_request.exchange(0, std::memory_order_relaxed)) catcher.Mark(m - 1);
    lock.lock();
    spare.push_back(std::move(buf));
    ++written;
  }
  REXLOG_INFO("PGR4 native glitch catch: {} frames seen, {} dropped (catcher behind)", written,
              dropped);
}

GlitchCatch::GlitchCatch() = default;
GlitchCatch::~GlitchCatch() = default;

void GlitchCatch::Initialize() {
  std::string prefix = REXCVAR_GET(pgr4_native_glitch_catch);
  if (prefix.empty()) return;
  state_ = std::make_unique<State>();
  state_->catcher.prefix = std::move(prefix);
}

void GlitchCatch::Collect(uint32_t slot) {
  State& g = *state_;
  if (!g.slots[slot].recorded) return;
  g.slots[slot].recorded = false;
  g.Push(g.slots[slot], slot);
}

void GlitchCatch::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, uint32_t slot,
                         float iw, float ih, uint32_t ramp_mode, uint64_t frame) {
  State& g = *state_;
  if (g.failed) return;
  const uint32_t w = uint32_t(iw) / 2, h = uint32_t(ih) / 2;
  if (!g.target && !g.Start(device, w, h)) {
    g.failed = true;
    return;
  }
  if (w != g.width || h != g.height) return;  // an image of another size: not recorded
  const struct {
    float image_size[2];
    uint32_t ramp_mode, point_sampled;
  } constants = {{iw, ih}, ramp_mode, 0};
  list->SetGraphicsRoot32BitConstants(1, 4, &constants, 0);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT vp = {0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
  const D3D12_RECT sc = {0, 0, LONG(w), LONG(h)};
  list->RSSetViewports(1, &vp);
  list->RSSetScissorRects(1, &sc);
  list->DrawInstanced(3, 1, 0, 0);
  D3D12_RESOURCE_BARRIER b = TransitionBarrier(g.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                               D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
  dst.pResource = g.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Offset = g.slot_bytes * slot;
  dst.PlacedFootprint.Footprint = {D3D12Device::kBackBufferFormat, w, h, 1, g.pitch};
  src.pResource = g.target.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
  list->ResourceBarrier(1, &b);
  g.slots[slot] = {frame, NowNs() / 1000, true};
}

void GlitchCatch::Mark(uint64_t frame) {
  state_->mark_request.store(frame + 1, std::memory_order_relaxed);
}

// ---- Luma probe ----

struct LumaProbeReadback::State {
  // A row of the widest texture each, so the buffer never grows while a slot is in flight.
  static constexpr uint32_t kMaxWidth = 8192, kRowBytes = kMaxWidth * 4;
  static constexpr uint32_t kRows = LumaProbe::kRows;
  struct Slot {
    uint64_t frame = 0;
    uint32_t width = 0, mapping = 0;
    bool rgb10a2 = false, recorded = false;
  } slots[D3D12Device::kFramesInFlight];
  ComPtr<ID3D12Resource> buffer;
  const uint8_t* mapped = nullptr;
  std::vector<uint8_t> rgb;
};

LumaProbeReadback::LumaProbeReadback() = default;
LumaProbeReadback::~LumaProbeReadback() = default;

void LumaProbeReadback::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, uint32_t slot,
                               ID3D12Resource* texture, uint32_t width, uint32_t height,
                               uint32_t mapping, bool rgb10a2, uint64_t frame) {
  using S = State;
  if (!state_) state_ = std::make_unique<State>();
  State& p = *state_;
  if (!p.buffer) {
    void* mapped = nullptr;
    if (!CreateBuffer(device, D3D12_HEAP_TYPE_READBACK,
                      uint64_t(S::kRowBytes) * S::kRows * D3D12Device::kFramesInFlight,
                      D3D12_RESOURCE_STATE_COPY_DEST, "luma probe readback", &p.buffer) ||
        FAILED(p.buffer->Map(0, nullptr, &mapped))) {
      p.buffer.Reset();
      return;
    }
    p.mapped = static_cast<const uint8_t*>(mapped);
  }
  S::Slot& ps = p.slots[slot];
  if (ps.recorded) {
    result_.frame = ps.frame;
    for (uint32_t i = 0; i < S::kRows; ++i) {
      ToRgb(p.mapped + size_t(slot * S::kRows + i) * S::kRowBytes, S::kRowBytes, ps.width, 1,
            ps.mapping, ps.rgb10a2, &p.rgb);
      for (uint32_t s = 0; s < LumaProbe::kSegments; ++s) {
        const size_t x0 = size_t(ps.width) * s / LumaProbe::kSegments * 3;
        const size_t x1 = size_t(ps.width) * (s + 1) / LumaProbe::kSegments * 3;
        uint64_t sum = 0;
        for (size_t x = x0; x < x1; ++x) sum += p.rgb[x];
        result_.luma[i][s] = x1 > x0 ? float(double(sum) / double(x1 - x0)) : 0.0f;
      }
    }
  }
  const uint32_t w = std::min(width, S::kMaxWidth);
  const D3D12_SUBRESOURCE_FOOTPRINT footprint = {texture->GetDesc().Format, w, 1, 1, RowPitch32(w)};
  for (uint32_t i = 0; i < S::kRows; ++i) {
    static constexpr uint32_t kRowPercent[S::kRows] = {10, 40, 80};
    const uint32_t y = height * kRowPercent[i] / 100;
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = p.buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = uint64_t(slot * S::kRows + i) * S::kRowBytes;
    dst.PlacedFootprint.Footprint = footprint;
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box = {0, y, 0, w, y + 1, 1};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  }
  ps = {frame, w, mapping, rgb10a2, true};
}

}  // namespace pgr4::native::present_debug
