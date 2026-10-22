// pgr4-native PGR4 native renderer (M2 owner #6): Swap -> debug window, front buffer captures.
// See present.h.
#include "present.h"

#include "d3d12_device.h"
#include "resolve.h"
#include "texture_layout.h"

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(pgr4_native_capture_dir, "C:\\recomp\\logs\\pgr4-native-captures",
                      "PGR4 native",
                      "Front buffer captures (native + Xenos PNGs, PSNR); <dir>\\capture.request "
                      "or F12 triggers one");
REXCVAR_DEFINE_INT32(pgr4_native_capture_frame, -1, "PGR4 native",
                     "Capture the front buffer of this native frame index (-1 = off)");
REXCVAR_DEFINE_INT32(pgr4_native_capture_hold_ms, 500, "PGR4 native",
                     "How long a capture holds the guest thread in Swap so the Xenos plugin "
                     "finishes the frame before its front buffer is read from guest memory");

namespace pgr4::native {

namespace xenos = rex::graphics::xenos;

namespace {

inline uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a + (a >= 0xE0000000u ? 0x1000u : 0u), 4);
  return __builtin_bswap32(v);
}

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
  auto* graphics = runtime ? static_cast<rex::graphics::GraphicsSystem*>(runtime->graphics_system())
                           : nullptr;
  return graphics ? graphics->command_processor() : nullptr;
}

// Output of a PWL ramp channel for a 10-bit input (the shader's math), for the log.
double PwlOut(const uint32_t* words, uint32_t channel, uint32_t x) {
  const uint32_t w = words[(x >> 3) * 3 + channel];
  return std::min(1.0, ((w & 0xFFFFu) + (x & 7u) * (w >> 16) / 8.0) / 65472.0);
}

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                        ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                        ID3DBlob**);

// ---- PNG (stored deflate blocks; no compression library needed) ----

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
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
        for (uint32_t c = 0; c < 3; ++c) ch[c] = uint8_t((((t >> (10 * c)) & 1023) * 255 + 511) / 1023);
        ch[3] = uint8_t((t >> 30) * 85);
      } else {
        std::memcpy(ch, &t, 4);
      }
      for (uint32_t c = 0; c < 3; ++c)
        out[x * 3 + c] = m[c] < 4 ? ch[m[c]] : (m[c] == 5 ? 255 : 0);
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
    bool armed = false;  // requested at the previous Swap: full readback held, capture this one
    bool pending = false;
    uint64_t frame = 0, fence = 0;
    uint32_t fetch[6] = {};
    uint32_t base = 0;
    uint32_t width = 0, height = 0;
    uint32_t mapping = 0;
    bool rgb10a2 = false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    ComPtr<ID3D12Resource> readback;
  } capture;
  bool log_fmt = false, log_missing = false;

  explicit Impl(NativeRenderer& renderer) : r(renderer) {}

  bool Initialize();
  // Copies the guest's current ramp of the mode into the frame's slot; false without one.
  bool WriteRamp(RampMode mode, uint32_t slot);
  bool CaptureRequested(uint64_t frame);
  void FinishCapture(const uint8_t* base);
};

bool Presenter::Impl::Initialize() {
  ID3D12Device* device = r.d3d12();
  HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = dll ? reinterpret_cast<PFN_D3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  if (!compile) {
    REXLOG_ERROR("PGR4 native present: d3dcompiler_47.dll / D3DCompile not found");
    return false;
  }
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
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = uint64_t(kRampSlotBytes) * D3D12Device::kFramesInFlight;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* p = nullptr;
    const D3D12_RANGE none = {0, 0};
    if (Failed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&ramp_buffer)),
               "present gamma ramp buffer") ||
        Failed(ramp_buffer->Map(0, &none, &p), "present gamma ramp map"))
      return false;
    ramp_mapped = static_cast<uint8_t*>(p);
  }
  std::error_code ec;
  std::filesystem::create_directories(REXCVAR_GET(pgr4_native_capture_dir), ec);
  return true;
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
        REXLOG_INFO("PGR4 native present: PWL gamma ramp at frame {}: 0.25/0.5/0.75 -> R {:.3f} "
                    "{:.3f} {:.3f}, G {:.3f} {:.3f} {:.3f}, B {:.3f} {:.3f} {:.3f}",
                    r.frame_index(), PwlOut(ramp, 0, 256), PwlOut(ramp, 0, 512),
                    PwlOut(ramp, 0, 767), PwlOut(ramp, 1, 256), PwlOut(ramp, 1, 512),
                    PwlOut(ramp, 1, 767), PwlOut(ramp, 2, 256), PwlOut(ramp, 2, 512),
                    PwlOut(ramp, 2, 767));
      else
        REXLOG_INFO("PGR4 native present: gamma table at frame {}: 64/128/192 -> R {} {} {} (of 1023)",
                    r.frame_index(), (ramp[64] >> 20) & 1023, (ramp[128] >> 20) & 1023,
                    (ramp[192] >> 20) & 1023);
    }
  }
  return true;
}

bool Presenter::Impl::CaptureRequested(uint64_t frame) {
  bool want = false;
  const int32_t at = REXCVAR_GET(pgr4_native_capture_frame);
  if (at >= 0 && frame == uint64_t(at)) want = true;
  if (GetAsyncKeyState(VK_F12) & 1) want = true;
  std::filesystem::path req = std::filesystem::path(REXCVAR_GET(pgr4_native_capture_dir)) /
                              "capture.request";
  std::error_code ec;
  if (std::filesystem::exists(req, ec)) {
    std::filesystem::remove(req, ec);
    want = true;
  }
  return want;
}

void Presenter::Impl::FinishCapture(const uint8_t* base) {
  (void)base;
  Capture& c = capture;
  c.pending = false;
  r.device().WaitFence(c.fence);
  // Let the Xenos plugin's GPU thread execute this frame (its Swap was forwarded already) and
  // copy the resolve back into guest memory.
  Sleep(DWORD(std::max(0, REXCVAR_GET(pgr4_native_capture_hold_ms))));
  HoldFullReadback(false);

  std::vector<uint8_t> native_rgb, xenos_rgb;
  {
    void* p = nullptr;
    D3D12_RANGE range = {0, SIZE_T(c.footprint.Footprint.RowPitch) * c.height};
    if (FAILED(c.readback->Map(0, &range, &p))) {
      REXLOG_ERROR("PGR4 native capture: readback map failed");
      return;
    }
    ToRgb(static_cast<const uint8_t*>(p), c.footprint.Footprint.RowPitch, c.width, c.height,
          c.mapping, c.rgb10a2, &native_rgb);
    D3D12_RANGE none = {0, 0};
    c.readback->Unmap(0, &none);
  }
  c.readback.Reset();
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
  const std::filesystem::path dir(REXCVAR_GET(pgr4_native_capture_dir));
  const std::string stem = "f" + std::to_string(c.frame);
  bool ok = WritePng(dir / (stem + "-native.png"), native_rgb, c.width, c.height) &&
            WritePng(dir / (stem + "-xenos.png"), xenos_rgb, c.width, c.height);
  const double psnr = Psnr(native_rgb, xenos_rgb);
  REXLOG_INFO("PGR4 native capture: frame {} {}x{} front buffer {:08X}: PSNR {:.2f} dB ({}\\{}-*.png{})",
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

void Presenter::OnSwap(const SwapCall& swap) {
  Impl& m = *impl_;
  D3D12Device& d = renderer_.device();
  ID3D12GraphicsCommandList* list = d.command_list();
  ID3D12Device* device = renderer_.d3d12();

  const uint32_t tex = swap.texture;
  uint32_t fetch[6] = {};
  const ResolvedImage* image = nullptr;
  if (tex) {
    std::memcpy(fetch, swap.fetch, sizeof(fetch));
    // The object holds the D3D's virtual address; the GPU (and the Resolver) use the physical
    // one: low 29 bits, plus 4 KiB for the 0xE0000000 view (as the D3D converts it).
    const uint32_t a = fetch[1] & 0xFFFFF000u;
    const uint32_t phys = (a & 0x1FFFFFFFu) + ((((a >> 20) & 0xFFFu) + 0x200u) & 0x1000u);
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
    REXLOG_WARN("PGR4 native present: no resolved image for the front buffer {:08X} (texture {:08X})",
                fetch[1] & 0xFFFFF000u, tex);
  }

  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = d.back_buffer();
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  list->ResourceBarrier(1, &b);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.back_buffer_rtv();
  // Black bars around the aspect-fit image (and a black frame when there is no image).
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  list->ClearRenderTargetView(rtv, black, 0, nullptr);
  uint32_t mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (last_found_) {
    mapping = GuestToHostComponentMapping((fetch[3] >> 1) & 0xFFF, host->host_swizzle);
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
    const float iw = float(std::min<uint64_t>(image->width ? image->width : desc.Width, desc.Width));
    const float ih = float(std::min<uint32_t>(image->height ? image->height : desc.Height, desc.Height));
    const float ww = float(d.width()), wh = float(d.height());
    const float s = std::min(ww / iw, wh / ih);
    const float vw = std::round(iw * s), vh = std::round(ih * s);
    const RampMode wanted = format == xenos::TextureFormat::k_2_10_10_10 ? kRampPwl : kRampTable;
    const uint32_t ramp_slot = d.frame_slot();
    struct {
      float image_size[2];
      uint32_t ramp_mode, point_sampled;
    } constants = {{iw, ih}, m.WriteRamp(wanted, ramp_slot) ? wanted : kRampNone,
                   vw == iw && vh == ih};
    list->SetGraphicsRoot32BitConstants(1, 4, &constants, 0);
    list->SetGraphicsRootConstantBufferView(
        2, m.ramp_buffer->GetGPUVirtualAddress() + uint64_t(ramp_slot) * kRampSlotBytes);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = {std::floor((ww - vw) * 0.5f), std::floor((wh - vh) * 0.5f), vw, vh, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, LONG(d.width()), LONG(d.height())};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
  }
  std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
  list->ResourceBarrier(1, &b);

  // A request arms the capture of the next frame: its resolves must reach guest memory
  // synchronously (HoldFullReadback), or the Xenos copy is the previous frame's.
  if (!m.capture.armed) {
    if (swap.capture_request < 0 ? m.CaptureRequested(renderer_.frame_index())
                                 : swap.capture_request > 0) {
      HoldFullReadback(true);
      m.capture.armed = true;
    }
  } else {
    m.capture.armed = false;
    if (!last_found_) {
      HoldFullReadback(false);
      REXLOG_WARN("PGR4 native capture: frame {}: no native front buffer to capture",
                  renderer_.frame_index());
      return;
    }
    Impl::Capture& c = m.capture;
    const D3D12_RESOURCE_DESC desc = image->texture->GetDesc();
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &c.footprint, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = total;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (Failed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&c.readback)),
               "capture readback")) {
      HoldFullReadback(false);
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

void Presenter::AfterEndFrame(const uint8_t* base) {
  if (impl_->capture.pending) impl_->FinishCapture(base);
}

}  // namespace pgr4::native
