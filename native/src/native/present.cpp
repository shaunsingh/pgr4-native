// pgr4-native PGR4 native renderer: Swap -> debug window, front buffer captures.
// See present.h.
#include "present.h"

#include "d3d12_device.h"
#include "guest_memory.h"
#include "resolve.h"
#include "texture_layout.h"

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_STRING(
    pgr4_native_capture_dir, "", "PGR4 native",
    "Folder for front buffer captures (native + Xenos PNGs, PSNR), marks and draw "
    "logs; when set, <dir>\\capture.request and the mark key (F12) take a "
    "capture and <dir>\\draw-debug.txt is read. Empty: nothing is polled");
REXCVAR_DEFINE_INT32(pgr4_native_capture_frame, -1, "PGR4 native",
                     "Capture the front buffer of this native frame index (-1 = off)");
REXCVAR_DEFINE_INT32(pgr4_native_capture_hold_ms, 500, "PGR4 native",
                     "How long a capture holds the guest thread in Swap so the Xenos plugin "
                     "finishes the frame before its front buffer is read from guest memory");
REXCVAR_DEFINE_BOOL(pgr4_native_capture_xenos, true, "PGR4 native",
                    "Captures also write the Xenos plugin's copy of the frame and the PSNR "
                    "between them, holding the guest thread (--pgr4_native_capture_hold_ms); "
                    "false: the native image alone, written off the guest thread");
REXCVAR_DEFINE_BOOL(pgr4_native_luma_probe, false, "PGR4 native",
                    "Read back three rows of every front buffer (at 10, 40 and 80 % of its "
                    "height) and log their mean brightness in the frame CSV, to find frames "
                    "drawn partly black");
REXCVAR_DEFINE_STRING(pgr4_native_glitch_catch, "", "PGR4 native",
                      "Keep the frames shown, at half size and after the gamma ramp, in memory "
                      "and write <this prefix>glitch-<frame>.rec when 1 to 4 frames jump away "
                      "from the image and back, and <prefix>mark-<frame>.rec with the last 5 s "
                      "at a mark (F12, with --pgr4_native_capture_dir); native/tools/flicker.py "
                      "and marks.py show them");

namespace pgr4::native {

namespace xenos = rex::graphics::xenos;

namespace {

// The display controller's gamma ramp, as the Xenos command processor holds it from the guest's
// DC_LUT register writes: 128 PWL entries per channel (DC_LUT_PWL_DATA: base 0:15, delta
// 16:31) for 2_10_10_10 front buffers, 256 table entries (DC_LUT_30_COLOR: blue 0:9, green
// 10:19, red 20:29) for 8_8_8_8 ones, as D3D's presentation parameters set them up. The ramp
// math follows Xenia's apply_gamma_pwl / apply_gamma_table (BSD-3-Clause, see
// THIRD_PARTY_NOTICES.md). The ramp applies to each texel before filtering, so a scaled image
// filters display values like the console's scaler does.
enum RampMode : uint32_t { kRampNone = 0, kRampPwl = 1, kRampTable = 2 };
constexpr uint32_t kPwlWords = 128 * 3, kTableWords = 256;
constexpr uint32_t kRampSlotBytes = kPwlWords * 4;  // a multiple of the 256-byte CBV alignment
static_assert(kRampSlotBytes % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0);
static_assert(sizeof(rex::graphics::reg::DC_LUT_PWL_DATA) == 4 &&
              sizeof(rex::graphics::reg::DC_LUT_30_COLOR) == 4);

// image_size: the image in texels (the texture can be larger than the image); point: the
// viewport is the image's size, every pixel is one texel.
const char kBlitHlsl[] = R"(
Texture2D<float4> src : register(t0);
cbuffer blit : register(b0) { float2 image_size; uint ramp_mode; uint point_sampled; };
cbuffer gamma : register(b1) { uint4 ramp[96]; };
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 texel : TEXCOORD0) {
  float2 uv = float2((id << 1) & 2, id & 2);
  pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  texel = uv * image_size;
}
uint RampWord(uint i) { return ramp[i >> 2][i & 3]; }
float3 Gamma(float3 c) {
  if (ramp_mode == 1) {
    uint3 x = uint3(c * 1023.0 + 0.5);
    uint3 w = uint3(RampWord((x.r >> 3) * 3), RampWord((x.g >> 3) * 3 + 1),
                    RampWord((x.b >> 3) * 3 + 2));
    return min((float3(w & 0xFFFF) + float3((x & 7) * (w >> 16)) * 0.125) * (1.0 / 65472.0), 1.0);
  }
  if (ramp_mode == 2) {
    uint3 x = uint3(c * 255.0 + 0.5);
    return float3((RampWord(x.r) >> 20) & 1023, (RampWord(x.g) >> 10) & 1023,
                  RampWord(x.b) & 1023) * (1.0 / 1023.0);
  }
  return c;
}
float3 Texel(int2 p) { return Gamma(src.Load(int3(p, 0)).rgb); }
float4 ps(float4 pos : SV_Position, float2 texel : TEXCOORD0) : SV_Target {
  int2 hi = int2(image_size) - 1;
  if (point_sampled) return float4(Texel(min(int2(texel), hi)), 1.0);
  float2 t = texel - 0.5;
  float2 f = t - floor(t);
  int2 a = clamp(int2(floor(t)), 0, hi), b = min(a + 1, hi);
  float3 top = lerp(Texel(a), Texel(int2(b.x, a.y)), f.x);
  float3 bottom = lerp(Texel(int2(a.x, b.y)), Texel(b), f.x);
  return float4(lerp(top, bottom, f.y), 1.0);
}
)";

const rex::graphics::CommandProcessor* GuestCommandProcessor() {
  rex::Runtime* runtime = rex::Runtime::instance();
  // The Xenos plugin's graphics system is the only IGraphicsSystem this title runs with.
  auto* graphics =
      runtime ? static_cast<rex::graphics::GraphicsSystem*>(runtime->graphics_system()) : nullptr;
  return graphics ? graphics->command_processor() : nullptr;
}

// Output of a PWL ramp channel for a 10-bit input (the shader's math), for the log.
double PwlOut(const uint32_t* words, uint32_t channel, uint32_t x) {
  const uint32_t w = words[(x >> 3) * 3 + channel];
  return std::min(1.0, ((w & 0xFFFFu) + (x & 7u) * (w >> 16) / 8.0) / 65472.0);
}

// ---- PNG (stored deflate blocks; no compression library needed) ----

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

// rgb: width * height * 3 bytes.
bool WritePng(const std::filesystem::path& path, const std::vector<uint8_t>& rgb, uint32_t width,
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
  std::FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;
  bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

// 32bpp host texels (row pitch; RGBA8 or RGB10A2 unorm) -> 8-bit RGB with a D3D12 component
// mapping (the fetch swizzle).
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

double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size() || a.empty()) return 0.0;
  double se = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    double d = double(a[i]) - double(b[i]);
    se += d * d;
  }
  double mse = se / double(a.size());
  return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace

struct Presenter::Impl {
  NativeRenderer& r;
  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> pso;
  ComPtr<ID3D12DescriptorHeap> heap;
  uint32_t heap_stride = 0, heap_next = 0;
  static constexpr uint32_t kHeapSize = 16;
  // One gamma ramp slot per frame in flight (D3D12Device::frame_slot), persistently mapped.
  ComPtr<ID3D12Resource> ramp_buffer;
  uint8_t* ramp_mapped = nullptr;
  const rex::graphics::CommandProcessor* command_processor = nullptr;
  uint32_t last_ramp[kPwlWords] = {};
  RampMode last_ramp_mode = kRampNone;
  uint32_t ramp_changes_logged = 0;

  struct Capture {
    bool armed = false;  // requested: capture the next shown frame
    bool xenos = true;   // with the Xenos copy (--pgr4_native_capture_xenos): full readback held
    bool pending = false;
    uint64_t frame = 0, fence = 0;
    uint32_t fetch[6] = {};
    uint32_t base = 0;
    uint32_t width = 0, height = 0;
    uint32_t mapping = 0;
    bool rgb10a2 = false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    ComPtr<ID3D12Resource> readback;
    std::thread writer;  // the PNG of a capture without the Xenos copy
  } capture;
  bool log_fmt = false, log_missing = false;

  // --pgr4_native_luma_probe: kProbeRows rows of each front buffer, a readback slot per frame in
  // flight, read when the slot comes round again (its frame has completed).
  static constexpr uint32_t kProbeRows = LumaProbe::kRows;
  struct ProbeSlot {
    uint64_t frame = 0;
    uint32_t width = 0, mapping = 0;
    bool rgb10a2 = false, recorded = false;
  } probe_slots[D3D12Device::kFramesInFlight];
  ComPtr<ID3D12Resource> probe_buffer;
  const uint8_t* probe_mapped = nullptr;
  LumaProbe probe_result;
  std::vector<uint8_t> probe_rgb;

  // --pgr4_native_glitch_catch: the blit drawn again at half the image's size (a 2x2 box filter)
  // into `target`, copied to a readback slot per frame in flight, and handed when the slot comes
  // round to the catcher's thread. A .rec file holds, per frame, a RecordHeader and height rows
  // of width RGB texels.
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
  struct GlitchCatch {
    bool on = false, failed = false;
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

    bool Start(ID3D12Device* device, uint32_t w, uint32_t h);
    void Push(const Slot& s, uint32_t slot);
    // Hands the slot's previous frame to the catcher's thread (its fence was waited for), every
    // Swap.
    void Collect(uint32_t slot) {
      if (!slots[slot].recorded) return;
      slots[slot].recorded = false;
      Push(slots[slot], slot);
    }
    void Write();
    void Stop();
  } glitch;

  explicit Impl(NativeRenderer& renderer) : r(renderer) {}
  ~Impl() {
    glitch.Stop();
    if (glitch.writer.joinable()) glitch.writer.join();
    if (capture.writer.joinable()) capture.writer.join();
  }
  // In the blit's state (root signature, pipeline, source and ramp bound): records this frame
  // for the glitch catcher (after GlitchCatch::Collect of the slot).
  void Record(ID3D12GraphicsCommandList* list, uint32_t slot, float iw, float ih,
              uint32_t ramp_mode);
  // Reads the slot's previous rows into probe_result, then records this frame's copies.
  void Probe(ID3D12GraphicsCommandList* list, uint32_t slot, const ResolvedImage& image,
             uint32_t mapping, bool rgb10a2);

  bool Initialize();
  // Copies the guest's current ramp of the mode into the frame's slot; false without one.
  bool WriteRamp(RampMode mode, uint32_t slot);
  bool CaptureRequested(uint64_t frame);
  // With the Xenos copy: waits for the frame, holds the guest thread while the plugin finishes
  // it, reads both images and writes the PNGs and their PSNR.
  void FinishCapture();
  // Without it, once the frame has completed: the native PNG, on capture.writer.
  void WriteCapture();

  // The mark key, F12 (a Steam Input binding can send it from a back grip). True on the press.
  bool MarkPressed();
  bool mark_down = false;
  // --pgr4_native_capture_dir; empty: no request file and no mark key are polled.
  std::filesystem::path capture_dir, capture_request;
  uint32_t request_poll = 0;
};

bool Presenter::Impl::Initialize() {
  ID3D12Device* device = r.d3d12();
  const pD3DCompile compile = D3DCompiler();
  if (!compile) return false;
  ComPtr<ID3DBlob> vs, ps, err;
  if (FAILED(compile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr, "vs", "vs_5_1", 0,
                     0, &vs, &err)) ||
      FAILED(compile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr, "ps", "ps_5_1", 0,
                     0, &ps, &err))) {
    REXLOG_ERROR("PGR4 native present: blit shader: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  }
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[3] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 4};  // b0: image_size, ramp_mode, point_sampled
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[2].Descriptor = {1, 0};  // b1: the gamma ramp
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rs = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob;
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
             "present root signature serialize") ||
      Failed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature)),
             "present root signature"))
    return false;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = root_signature.Get();
  d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = D3D12Device::kBackBufferFormat;
  d.SampleDesc.Count = 1;
  if (Failed(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), "present pipeline"))
    return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kHeapSize,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "present heap")) return false;
  heap_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  {
    void* p = nullptr;
    const D3D12_RANGE none = {0, 0};
    if (!CreateBuffer(
            device, D3D12_HEAP_TYPE_UPLOAD, uint64_t(kRampSlotBytes) * D3D12Device::kFramesInFlight,
            D3D12_RESOURCE_STATE_GENERIC_READ, "present gamma ramp buffer", &ramp_buffer) ||
        Failed(ramp_buffer->Map(0, &none, &p), "present gamma ramp map"))
      return false;
    ramp_mapped = static_cast<uint8_t*>(p);
  }
  capture_dir = std::filesystem::path(REXCVAR_GET(pgr4_native_capture_dir));
  if (!capture_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(capture_dir, ec);
    capture_request = capture_dir / "capture.request";
  }
  glitch.catcher.prefix = REXCVAR_GET(pgr4_native_glitch_catch);
  glitch.on = !glitch.catcher.prefix.empty();
  return true;
}

bool Presenter::Impl::GlitchCatch::Start(ID3D12Device* device, uint32_t w, uint32_t h) {
  width = w;
  height = h;
  pitch =
      (w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
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

void Presenter::Impl::GlitchCatch::Push(const Slot& s, uint32_t slot) {
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

void Presenter::Impl::GlitchCatch::Write() {
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

float Presenter::Impl::Catcher::Diff(const Frame& a, const Frame& b) {
  const size_t n = a.means.size();
  if (!n || b.means.size() != n) return 0.0f;
  float sum = 0.0f;
  for (size_t i = 0; i < n; ++i) sum += std::fabs(a.means[i] - b.means[i]);
  return sum / float(n);
}

void Presenter::Impl::Catcher::Add(const RecordHeader& h, const uint8_t* rgba) {
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

void Presenter::Impl::Catcher::Mark(uint64_t frame) {
  if (!count || marks >= kMaxMarks) return;
  ++marks;
  const uint64_t last = count - 1;
  Dump(count > kRing ? count - kRing : 0, last, "mark", frame);
}

void Presenter::Impl::Catcher::Dump(uint64_t from, uint64_t to, const char* kind, uint64_t label) {
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

void Presenter::Impl::GlitchCatch::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    stopping = true;
  }
  cv.notify_all();
}

void Presenter::Impl::Record(ID3D12GraphicsCommandList* list, uint32_t slot, float iw, float ih,
                             uint32_t ramp_mode) {
  GlitchCatch& g = glitch;
  if (g.failed) return;
  const uint32_t w = uint32_t(iw) / 2, h = uint32_t(ih) / 2;
  if (!g.target && !g.Start(r.d3d12(), w, h)) {
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
  g.slots[slot] = {r.frame_index(), NowNs() / 1000, true};
}

bool Presenter::Impl::WriteRamp(RampMode mode, uint32_t slot) {
  if (!command_processor && !(command_processor = GuestCommandProcessor())) return false;
  const uint32_t* words =
      mode == kRampPwl
          ? reinterpret_cast<const uint32_t*>(command_processor->gamma_ramp_pwl_rgb())
          : reinterpret_cast<const uint32_t*>(command_processor->gamma_ramp_256_entry_table());
  const uint32_t count = mode == kRampPwl ? kPwlWords : kTableWords;
  // The command processor's thread writes the ramp as it executes the guest's register writes;
  // a ramp read during an update shows for one frame, the next frame reads the finished one.
  uint32_t ramp[kPwlWords];
  for (uint32_t i = 0; i < count; ++i) ramp[i] = __atomic_load_n(words + i, __ATOMIC_RELAXED);
  std::memcpy(ramp_mapped + size_t(slot) * kRampSlotBytes, ramp, count * 4);
  if (mode != last_ramp_mode || std::memcmp(ramp, last_ramp, count * 4)) {
    last_ramp_mode = mode;
    std::memcpy(last_ramp, ramp, count * 4);
    if (ramp_changes_logged < 8) {
      ++ramp_changes_logged;
      if (mode == kRampPwl)
        REXLOG_INFO(
            "PGR4 native present: PWL gamma ramp at frame {}: 0.25/0.5/0.75 -> R {:.3f} "
            "{:.3f} {:.3f}, G {:.3f} {:.3f} {:.3f}, B {:.3f} {:.3f} {:.3f}",
            r.frame_index(), PwlOut(ramp, 0, 256), PwlOut(ramp, 0, 512), PwlOut(ramp, 0, 767),
            PwlOut(ramp, 1, 256), PwlOut(ramp, 1, 512), PwlOut(ramp, 1, 767), PwlOut(ramp, 2, 256),
            PwlOut(ramp, 2, 512), PwlOut(ramp, 2, 767));
      else
        REXLOG_INFO(
            "PGR4 native present: gamma table at frame {}: 64/128/192 -> R {} {} {} (of 1023)",
            r.frame_index(), (ramp[64] >> 20) & 1023, (ramp[128] >> 20) & 1023,
            (ramp[192] >> 20) & 1023);
    }
  }
  return true;
}

bool Presenter::Impl::CaptureRequested(uint64_t frame) {
  const int32_t at = REXCVAR_GET(pgr4_native_capture_frame);
  bool want = at >= 0 && frame == uint64_t(at);
  if (capture_dir.empty()) return want;
  if (MarkPressed()) {
    REXLOG_INFO("PGR4 native mark at frame {}", frame);
    want = true;
  }
  // Every 16th frame: a file query goes through Wine's path translation, and the scripts that
  // write the request do not mind a few frames' delay.
  if (++request_poll >= 16) {
    request_poll = 0;
    std::error_code ec;
    if (std::filesystem::remove(capture_request, ec)) want = true;
  }
  return want;
}

bool Presenter::Impl::MarkPressed() {
  // The key's state rather than GetAsyncKeyState's "pressed since the last call" bit, which
  // most F12 presses from a Steam Input binding did not set under Proton. A key and not a pad
  // chord: the game reads the pad too, and R3 (L3+R3 was the chord) is PGR4's look back.
  const bool down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
  const bool pressed = down && !mark_down;
  mark_down = down;
  return pressed;
}

namespace {

// A completed front buffer copy as RGB rows.
bool ReadbackRgb(ID3D12Resource* readback, uint32_t row_pitch, uint32_t width, uint32_t height,
                 uint32_t mapping, bool rgb10a2, std::vector<uint8_t>* rgb) {
  void* p = nullptr;
  const D3D12_RANGE range = {0, SIZE_T(row_pitch) * height};
  if (FAILED(readback->Map(0, &range, &p))) {
    REXLOG_ERROR("PGR4 native capture: readback map failed");
    return false;
  }
  ToRgb(static_cast<const uint8_t*>(p), row_pitch, width, height, mapping, rgb10a2, rgb);
  const D3D12_RANGE none = {0, 0};
  readback->Unmap(0, &none);
  return true;
}

}  // namespace

void Presenter::Impl::WriteCapture() {
  Capture& c = capture;
  c.pending = false;
  if (c.writer.joinable()) c.writer.join();
  c.writer = std::thread([readback = std::move(c.readback), pitch = c.footprint.Footprint.RowPitch,
                          w = c.width, h = c.height, mapping = c.mapping, rgb10a2 = c.rgb10a2,
                          frame = c.frame, dir = capture_dir] {
    std::vector<uint8_t> rgb;
    if (!ReadbackRgb(readback.Get(), pitch, w, h, mapping, rgb10a2, &rgb)) return;
    const std::string name = "f" + std::to_string(frame) + "-native.png";
    const bool ok = WritePng(dir / name, rgb, w, h);
    REXLOG_INFO("PGR4 native capture: frame {} {}x{} ({}\\{}{})", frame, w, h, dir.string(), name,
                ok ? "" : ", write failed");
  });
}

void Presenter::Impl::FinishCapture() {
  Capture& c = capture;
  c.pending = false;
  r.device().WaitFence(c.fence);
  // Let the Xenos plugin's GPU thread execute this frame (its Swap was forwarded already) and
  // copy the resolve back into guest memory.
  Sleep(DWORD(std::max(0, REXCVAR_GET(pgr4_native_capture_hold_ms))));
  HoldFullReadback(false);

  std::vector<uint8_t> native_rgb, xenos_rgb;
  const bool read = ReadbackRgb(c.readback.Get(), c.footprint.Footprint.RowPitch, c.width, c.height,
                                c.mapping, c.rgb10a2, &native_rgb);
  c.readback.Reset();
  if (!read) return;
  {
    TextureKey key;
    if (!TextureKeyFromFetch(c.fetch, &key)) return;
    std::vector<HostSubresource> subs;
    uint64_t bytes = GetHostFootprints(key, false, &subs);
    uint32_t base_bytes = 0, mips_bytes = 0;
    GetGuestExtents(key, &base_bytes, &mips_bytes);
    const uint8_t* membase = rex::system::kernel_memory()->physical_membase();
    GuestTextureMemory mem;
    mem.base = membase + (key.base_page << 12);
    mem.base_size = base_bytes;
    mem.mips = key.mip_page ? membase + (key.mip_page << 12) : nullptr;
    mem.mips_size = mips_bytes;
    std::vector<uint8_t> host(bytes), scratch;
    if (!bytes || subs.empty() || !UntileTexture(key, mem, subs, host.data(), &scratch)) {
      REXLOG_ERROR("PGR4 native capture: untiling the Xenos front buffer failed");
      return;
    }
    const HostSubresource& s0 = subs[0];
    uint32_t w = std::min(c.width, s0.width), h = std::min(c.height, s0.height);
    ToRgb(host.data() + s0.offset, s0.row_pitch, w, h, c.mapping, c.rgb10a2, &xenos_rgb);
    if (w != c.width || h != c.height) {
      std::vector<uint8_t> crop(size_t(w) * h * 3);
      for (uint32_t y = 0; y < h; ++y)
        std::memcpy(crop.data() + size_t(y) * w * 3, native_rgb.data() + size_t(y) * c.width * 3,
                    size_t(w) * 3);
      native_rgb.swap(crop);
      c.width = w;
      c.height = h;
    }
  }
  const std::filesystem::path& dir = capture_dir;
  const std::string stem = "f" + std::to_string(c.frame);
  bool ok = WritePng(dir / (stem + "-native.png"), native_rgb, c.width, c.height) &&
            WritePng(dir / (stem + "-xenos.png"), xenos_rgb, c.width, c.height);
  const double psnr = Psnr(native_rgb, xenos_rgb);
  REXLOG_INFO(
      "PGR4 native capture: frame {} {}x{} front buffer {:08X}: PSNR {:.2f} dB ({}\\{}-*.png{})",
      c.frame, c.width, c.height, c.base, psnr, dir.string(), stem, ok ? "" : ", write failed");
  if (std::FILE* f = _wfopen((dir / "captures.csv").c_str(), L"a")) {
    std::fprintf(f, "%llu,%u,%u,%08X,%.3f\n", (unsigned long long)c.frame, c.width, c.height,
                 c.base, psnr);
    std::fclose(f);
  }
}

Presenter::Presenter(NativeRenderer& renderer)
    : renderer_(renderer), impl_(std::make_unique<Impl>(renderer)) {}
Presenter::~Presenter() = default;

bool Presenter::Initialize() { return impl_->Initialize(); }

bool Presenter::CaptureRequested(uint64_t frame) { return impl_->CaptureRequested(frame); }

void Presenter::ReadSwap(const uint8_t* base, const HookArgs& args, SwapCall* out) {
  // The front buffer texture's fetch constant (D3DBaseTexture + 28).
  out->texture = args.r[1];
  if (out->texture)
    for (int i = 0; i < 6; ++i) out->fetch[i] = Load32(base, out->texture + 28 + 4 * i);
}

void Presenter::OnSwap(const SwapCall& swap, bool shown) {
  Impl& m = *impl_;
  D3D12Device& d = renderer_.device();
  ID3D12GraphicsCommandList* list = d.command_list();
  ID3D12Device* device = renderer_.d3d12();

  const uint32_t tex = swap.texture;
  uint32_t fetch[6] = {};
  const ResolvedImage* image = nullptr;
  if (tex) {
    std::memcpy(fetch, swap.fetch, sizeof(fetch));
    // The object holds the D3D's virtual address; the GPU (and the Resolver) use the physical one.
    const uint32_t phys = GpuAddress(fetch[1] & 0xFFFFF000u);
    fetch[1] = (fetch[1] & 0xFFFu) | phys;
    image = renderer_.resolver().Find(phys);
  }
  last_found_ = image && image->texture;
  // The host image is in the copy destination format (k_2_10_10_10 for PGR4's
  // k_2_10_10_10_AS_16_16_16_16 front buffer).
  const xenos::TextureFormat format = last_found_ ? image->format : xenos::TextureFormat(0);
  const HostFormat* host = last_found_ ? GetHostFormat(format) : nullptr;
  if (last_found_ && (!host || (format != xenos::TextureFormat::k_8_8_8_8 &&
                                format != xenos::TextureFormat::k_2_10_10_10))) {
    if (!m.log_fmt) {
      m.log_fmt = true;
      REXLOG_WARN("PGR4 native present: front buffer format {} not handled", uint32_t(format));
    }
    last_found_ = false;
  }
  if (!image && !m.log_missing) {
    m.log_missing = true;
    REXLOG_WARN(
        "PGR4 native present: no resolved image for the front buffer {:08X} (texture {:08X})",
        fetch[1] & 0xFFFFF000u, tex);
  }

  uint32_t mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (last_found_)
    mapping = GuestToHostComponentMapping((fetch[3] >> 1) & 0xFFF, host->host_swizzle);
  if (m.glitch.on) m.glitch.Collect(d.frame_slot());
  // A held frame (DrawRenderer::FrameIncomplete) never reaches the screen: it is neither blitted
  // nor recorded, so the glitch catcher and the luma probe see what was shown.
  if (shown) {
    D3D12_RESOURCE_BARRIER b = TransitionBarrier(d.back_buffer(), D3D12_RESOURCE_STATE_PRESENT,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.back_buffer_rtv();
    // Black bars around the aspect-fit image (and a black frame when there is no image).
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    if (last_found_) {
      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = host->unorm;
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Shader4ComponentMapping = mapping;
      sd.Texture2D.MipLevels = 1;
      const uint32_t slot = m.heap_next++ % Impl::kHeapSize;
      D3D12_CPU_DESCRIPTOR_HANDLE cpu = m.heap->GetCPUDescriptorHandleForHeapStart();
      cpu.ptr += size_t(slot) * m.heap_stride;
      D3D12_GPU_DESCRIPTOR_HANDLE gpu = m.heap->GetGPUDescriptorHandleForHeapStart();
      gpu.ptr += uint64_t(slot) * m.heap_stride;
      device->CreateShaderResourceView(image->texture, &sd, cpu);
      ID3D12DescriptorHeap* heaps[] = {m.heap.Get()};
      list->SetDescriptorHeaps(1, heaps);
      list->SetGraphicsRootSignature(m.root_signature.Get());
      list->SetPipelineState(m.pso.Get());
      list->SetGraphicsRootDescriptorTable(0, gpu);
      // The image (1280x720 in PGR4's mode) scaled to fit the window, centred, aspect kept.
      const D3D12_RESOURCE_DESC desc = image->texture->GetDesc();
      const float iw =
          float(std::min<uint64_t>(image->width ? image->width : desc.Width, desc.Width));
      const float ih =
          float(std::min<uint32_t>(image->height ? image->height : desc.Height, desc.Height));
      const float ww = float(d.width()), wh = float(d.height());
      const float s = std::min(ww / iw, wh / ih);
      const float vw = std::round(iw * s), vh = std::round(ih * s);
      const RampMode wanted = format == xenos::TextureFormat::k_2_10_10_10 ? kRampPwl : kRampTable;
      const uint32_t ramp_slot = d.frame_slot();
      struct {
        float image_size[2];
        uint32_t ramp_mode, point_sampled;
      } constants = {
          {iw, ih}, m.WriteRamp(wanted, ramp_slot) ? wanted : kRampNone, vw == iw && vh == ih};
      list->SetGraphicsRoot32BitConstants(1, 4, &constants, 0);
      list->SetGraphicsRootConstantBufferView(
          2, m.ramp_buffer->GetGPUVirtualAddress() + uint64_t(ramp_slot) * kRampSlotBytes);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      D3D12_VIEWPORT vp = {
          std::floor((ww - vw) * 0.5f), std::floor((wh - vh) * 0.5f), vw, vh, 0.0f, 1.0f};
      D3D12_RECT sc = {0, 0, LONG(d.width()), LONG(d.height())};
      list->RSSetViewports(1, &vp);
      list->RSSetScissorRects(1, &sc);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->DrawInstanced(3, 1, 0, 0);
      if (m.glitch.on) m.Record(list, ramp_slot, iw, ih, constants.ramp_mode);
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    if (last_found_ && REXCVAR_GET(pgr4_native_luma_probe))
      m.Probe(list, d.frame_slot(), *image, mapping, format == xenos::TextureFormat::k_2_10_10_10);
  }

  // A request arms the capture of the next shown frame. With the Xenos copy, that frame's
  // resolves must reach guest memory synchronously (HoldFullReadback) from its start, or the
  // copy is the previous frame's; the native image alone is this frame's.
  Impl::Capture& c = m.capture;
  if (!c.armed && (swap.capture_request < 0 ? m.CaptureRequested(renderer_.frame_index())
                                            : swap.capture_request > 0)) {
    if (m.glitch.on)
      m.glitch.mark_request.store(renderer_.frame_index() + 1, std::memory_order_relaxed);
    c.armed = true;
    c.xenos = REXCVAR_GET(pgr4_native_capture_xenos);
    if (c.xenos) {
      HoldFullReadback(true);
      return;
    }
  }
  if (c.armed && shown && !c.pending) {
    c.armed = false;
    if (!last_found_) {
      if (c.xenos) HoldFullReadback(false);
      REXLOG_WARN("PGR4 native capture: frame {}: no native front buffer to capture",
                  renderer_.frame_index());
      return;
    }
    const D3D12_RESOURCE_DESC desc = image->texture->GetDesc();
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &c.footprint, nullptr, nullptr, &total);
    if (!CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST,
                      "capture readback", &c.readback)) {
      if (c.xenos) HoldFullReadback(false);
      return;
    }
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = c.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = c.footprint;
    src.pResource = image->texture;  // COPY_SOURCE | shader resource (the Resolver's read state)
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    c.pending = true;
    c.frame = renderer_.frame_index();
    c.fence = renderer_.frame_fence();
    std::memcpy(c.fetch, fetch, sizeof(fetch));
    c.base = fetch[1] & 0xFFFFF000u;
    c.width = std::min<uint32_t>(image->width, uint32_t(desc.Width));
    c.height = std::min<uint32_t>(image->height, desc.Height);
    c.mapping = mapping;
    c.rgb10a2 = format == xenos::TextureFormat::k_2_10_10_10;
  }
}

void Presenter::Impl::Probe(ID3D12GraphicsCommandList* list, uint32_t slot,
                            const ResolvedImage& image, uint32_t mapping, bool rgb10a2) {
  // A row of the widest texture each, so the buffer never grows while a slot is in flight.
  constexpr uint32_t kRowBytes = 8192 * 4;
  if (!probe_buffer) {
    void* mapped = nullptr;
    if (!CreateBuffer(r.d3d12(), D3D12_HEAP_TYPE_READBACK,
                      uint64_t(kRowBytes) * kProbeRows * D3D12Device::kFramesInFlight,
                      D3D12_RESOURCE_STATE_COPY_DEST, "luma probe readback", &probe_buffer) ||
        FAILED(probe_buffer->Map(0, nullptr, &mapped))) {
      probe_buffer.Reset();
      return;
    }
    probe_mapped = static_cast<const uint8_t*>(mapped);
  }
  ProbeSlot& ps = probe_slots[slot];
  if (ps.recorded) {
    probe_result.frame = ps.frame;
    for (uint32_t i = 0; i < kProbeRows; ++i) {
      ToRgb(probe_mapped + size_t(slot * kProbeRows + i) * kRowBytes, kRowBytes, ps.width, 1,
            ps.mapping, ps.rgb10a2, &probe_rgb);
      for (uint32_t s = 0; s < LumaProbe::kSegments; ++s) {
        const size_t x0 = size_t(ps.width) * s / LumaProbe::kSegments * 3;
        const size_t x1 = size_t(ps.width) * (s + 1) / LumaProbe::kSegments * 3;
        uint64_t sum = 0;
        for (size_t x = x0; x < x1; ++x) sum += probe_rgb[x];
        probe_result.luma[i][s] = x1 > x0 ? float(double(sum) / double(x1 - x0)) : 0.0f;
      }
    }
  }
  const D3D12_RESOURCE_DESC desc = image.texture->GetDesc();
  const uint32_t w = std::min<uint32_t>(
      {image.width ? image.width : uint32_t(desc.Width), uint32_t(desc.Width), 8192u});
  const uint32_t h = std::min<uint32_t>(image.height ? image.height : desc.Height, desc.Height);
  const uint32_t pitch =
      (w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  for (uint32_t i = 0; i < kProbeRows; ++i) {
    static constexpr uint32_t kRowPercent[kProbeRows] = {10, 40, 80};
    const uint32_t y = h * kRowPercent[i] / 100;
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = probe_buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = uint64_t(slot * kProbeRows + i) * kRowBytes;
    dst.PlacedFootprint.Footprint = {desc.Format, w, 1, 1, pitch};
    src.pResource = image.texture;  // COPY_SOURCE | shader resource (the Resolver's read state)
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box = {0, y, 0, w, y + 1, 1};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  }
  ps = {r.frame_index(), w, mapping, rgb10a2, true};
}

const LumaProbe& Presenter::luma_probe() const { return impl_->probe_result; }

bool Presenter::capture_holds_guest() const { return REXCVAR_GET(pgr4_native_capture_xenos); }

void Presenter::AfterEndFrame() {
  Impl& m = *impl_;
  if (!m.capture.pending) return;
  if (m.capture.xenos)
    m.FinishCapture();
  else if (renderer_.device().completed_fence() >= m.capture.fence)
    m.WriteCapture();
}

}  // namespace pgr4::native
