// pgr4-native PGR4 native renderer: guest Resolve -> host texture. See resolve.h.
//
// Format packing follows ReXGlue/Xenia's resolve shaders and xenos.h conversions (BSD-3,
// Copyright Xenia/ReXGlue developers): 7e3 and 20e4 float encodings, unorm rounding, the
// red/blue swap of RB_COPY_DEST_INFO and the sample selections of RB_COPY_CONTROL.
#include "resolve.h"

#include "clock.h"
#include "gpu_timer.h"
#include "guest_memory.h"
#include "renderer.h"
#include "texture_layout.h"
#include "win_handle.h"

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

REXCVAR_DEFINE_STRING(
    pgr4_native_resolve_dump, "", "PGR4 native",
    "Directory for resolve oracle dumps; create <dir>\\dump.request to dump the next frame");
REXCVAR_DEFINE_BOOL(pgr4_native_resolve_selftest, false, "PGR4 native",
                    "Check the resolve shader's format packing against known encodings at start");
REXCVAR_DEFINE_BOOL(pgr4_native_resolve_specialize, true, "PGR4 native",
                    "Compile a copy shader per kind of resolve (formats, samples, swap, bias) on a "
                    "worker thread; resolves use the generic shader until it is ready");
REXCVAR_DECLARE(int32_t, pgr4_native_capture_hold_ms);

namespace pgr4::native {
namespace {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;

constexpr uint32_t kViewsPerResolve = 4;  // t0 color/depth, t1 stencil, u0 texel bits, u1 floats
constexpr uint32_t kHeapPerFrame = 128 * kViewsPerResolve;  // descriptors per frame slot
constexpr uint32_t kHeapSlots = 4;                          // > frames in flight
constexpr uint32_t kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
constexpr D3D12_RESOURCE_STATES kImageReadState = D3D12_RESOURCE_STATE_COPY_SOURCE |
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

#define PGR4_LOG_ONCE(counter, ...)                \
  do {                                             \
    if ((counter)++ < 4) REXLOG_WARN(__VA_ARGS__); \
  } while (0)

// How the shader writes a texel through the image's UAV (kShader's Store).
enum DestLayout : uint32_t {
  kR8,
  kR8G8,
  kR8G8B8A8,
  kR10G10B10A2,
  kR16,
  kR16G16,
  kR16G16B16A16,
  kR32,          // one to four 32-bit components
  kB5G6R5Unorm,  // no UINT view exists: unorm floats
};

// The host image of a copy destination format: the texture cache's host layout
// (GetHostFormat) in its typeless family, so the shader writes the Xenos texel bits through a
// UINT view and the cache binds or copies the image as its own format.
struct DestStorage {
  int32_t bpp_log2;
  DXGI_FORMAT resource;
  DXGI_FORMAT uav;
  DestLayout layout;
};

// nullptr if the resolve can't write the format.
const DestStorage* GetDestStorage(uint32_t f) {
  static constexpr DestStorage k8{0, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UINT, kR8};
  static constexpr DestStorage k8_8{1, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UINT, kR8G8};
  // k_5_6_5 and k_6_5_5 share the B5G6R5 host layout (the texture cache's load conversions).
  static constexpr DestStorage k565{1, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM,
                                    kB5G6R5Unorm};
  static constexpr DestStorage k16f{1, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UINT, kR16};
  static constexpr DestStorage k8888{2, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT,
                                     kR8G8B8A8};
  static constexpr DestStorage k2101010{2, DXGI_FORMAT_R10G10B10A2_TYPELESS,
                                        DXGI_FORMAT_R10G10B10A2_UINT, kR10G10B10A2};
  static constexpr DestStorage k16_16f{2, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UINT,
                                       kR16G16};
  static constexpr DestStorage k32f{2, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT, kR32};
  static constexpr DestStorage k16x4f{3, DXGI_FORMAT_R16G16B16A16_TYPELESS,
                                      DXGI_FORMAT_R16G16B16A16_UINT, kR16G16B16A16};
  static constexpr DestStorage k32_32f{3, DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_UINT,
                                       kR32};
  static constexpr DestStorage k32x4f{4, DXGI_FORMAT_R32G32B32A32_TYPELESS,
                                      DXGI_FORMAT_R32G32B32A32_UINT, kR32};
  switch (xenos::TextureFormat(f)) {
    case xenos::TextureFormat::k_8:
      return &k8;
    case xenos::TextureFormat::k_8_8:
      return &k8_8;
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_6_5_5:
      return &k565;
    case xenos::TextureFormat::k_16_FLOAT:
      return &k16f;
    case xenos::TextureFormat::k_8_8_8_8:
      return &k8888;
    case xenos::TextureFormat::k_2_10_10_10:
      return &k2101010;
    case xenos::TextureFormat::k_16_16_FLOAT:
      return &k16_16f;
    case xenos::TextureFormat::k_32_FLOAT:
      return &k32f;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return &k16x4f;
    case xenos::TextureFormat::k_32_32_FLOAT:
      return &k32_32f;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return &k32x4f;
    default:
      return nullptr;
  }
}

// log2 of the bytes per texel of a copy destination format; -1 if the resolve can't write it.
int DestBppLog2(uint32_t f) {
  const DestStorage* s = GetDestStorage(f);
  return s ? s->bpp_log2 : -1;
}

// Bytes per pixel of a color render target format (raw copies keep the EDRAM bits).
uint32_t ColorRtBppLog2(uint32_t f) { return (f == 5 || f == 7 || f == 15) ? 3 : 2; }

// The guest D3D's mapping from the destination texture's format to RB_COPY_DEST_INFO's
// (sub_826CBEE0: the cmplwi chain on the fetch format).
uint32_t D3DCopyDestFormat(uint32_t f) {
  switch (f) {
    case 54:
      return 7;  // k_2_10_10_10_AS_16_16_16_16 -> k_2_10_10_10
    case 55:
      return 16;  // k_10_11_11_AS_16_16_16_16 -> k_10_11_11
    case 56:
      return 17;  // k_11_11_10_AS_16_16_16_16 -> k_11_11_10
    case 27:
      return 30;  // k_16_EXPAND -> k_16_FLOAT
    case 28:
      return 31;  // k_16_16_EXPAND -> k_16_16_FLOAT
    case 29:
      return 32;  // k_16_16_16_16_EXPAND -> k_16_16_16_16_FLOAT
    case 22:      // k_24_8
    case 23:
      return 6;  // k_24_8_FLOAT -> k_8_8_8_8
    default:
      return f;
  }
}

bool IsFloatFormat(uint32_t f) {
  switch (xenos::TextureFormat(f)) {
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

const char kShader[] = R"(
cbuffer Params : register(b0) {
  int2 src_origin;      // source rectangle origin in the render target
  uint2 size;           // rectangle size
  uint2 dest_origin;    // destination point in the image
  uint cb_dest_format;  // xenos::TextureFormat
  uint cb_src_format;   // xenos::ColorRenderTargetFormat, or the depth format
  uint cb_flags;        // 0: convert, 1: swap, 4-6: sample select, 8-10: samples, 12-14: number
  int cb_exp_bias;
  uint cb_dest_layout;  // DestLayout of dst
  uint pad;
};
// A specialized variant has the parameters of one kind of resolve compiled in, so the compiler
// drops the paths and conversions it does not take; the generic shader reads them above.
#if SPECIALIZED
#define dest_format ((uint)SPEC_DEST_FORMAT)
#define src_format ((uint)SPEC_SRC_FORMAT)
#define flags ((uint)SPEC_FLAGS)
#define exp_bias ((int)SPEC_EXP_BIAS)
#define dest_layout ((uint)SPEC_DEST_LAYOUT)
#else
#define dest_format cb_dest_format
#define src_format cb_src_format
#define flags cb_flags
#define exp_bias cb_exp_bias
#define dest_layout cb_dest_layout
#endif
#if DEPTH
#if MSAA
Texture2DMS<float> src_depth : register(t0);
Texture2DMS<uint2> src_stencil : register(t1);
#else
Texture2D<float> src_depth : register(t0);
Texture2D<uint2> src_stencil : register(t1);
#endif
#else
#if MSAA
Texture2DMS<float4> src : register(t0);
#else
Texture2D<float4> src : register(t0);
#endif
#endif
// The destination image through its UINT view (Xenos texel bits), or (B5G6R5, and the R32_FLOAT
// image of depth resolves) as floats.
RWTexture2D<uint4> dst : register(u0);
RWTexture2D<float4> dst_float : register(u1);

uint SampleSelect() { return (flags >> 4) & 7; }
uint SampleCount() { return (flags >> 8) & 7; }
// Host sample of a guest sample s (0..3). SanitizeCopySampleSelect: 2x k2/k3 -> k0/k1, 1x -> 0.
// GetD3D10SampleIndexForGuest2xMSAA (native 2x): Xenos 2x sample 0 is the top one, which is
// sample 1 in Direct3D 10.1+; 4x samples map 1:1.
uint HostSample(uint s) {
  uint n = SampleCount();
  return n == 4u ? s : (n == 2u ? 1u - (s & 1u) : 0u);
}

uint U(float v, float scale) { return (uint)(saturate(v) * scale + 0.5); }
uint S8(float v) { return ((uint)(int)round(clamp(v, -1.0, 1.0) * 127.0)) & 0xFF; }

// Xenia xenos::Float32To7e3 (pre-clamped to [0, 31.875]).
uint F7e3(float f) {
  uint u = asuint(clamp(f, 0.0, 31.875));
  if (u < 0x3E800000u) {
    uint shift = min(125u - (u >> 23), 24u);
    u = ((u & 0x7FFFFFu) | 0x800000u) >> shift;
  } else {
    u += 0xC2000000u;
  }
  return ((u + 0x7FFFu + ((u >> 16) & 1u)) >> 16) & 0x3FFu;
}
// Xenia xenos::Float32To20e4 truncating, as ReXGlue's EDRAM store of host D32 depth
// (PreClampedDepthTo20e4 with depth_float24_round off, the plugin's default).
uint F20e4(float f) {
  uint u = asuint(clamp(f, 0.0, 1.99999988));
  if (u < 0x38800000u) {
    uint shift = 113u - (u >> 23);
    u = shift < 24u ? (((u & 0x7FFFFFu) | 0x800000u) >> shift) : 0u;
  } else {
    u += 0xC8000000u;
  }
  return (u >> 3) & 0xFFFFFFu;
}
float F20e4ToF32(uint v) {
  uint m = v & 0xFFFFFu, e = v >> 20;
  if (e == 0u) {
    if (m == 0u) return 0.0;
    uint lz = firstbithigh(m);  // 0..19
    return asfloat(((lz + 93u) << 23) | (((m << (20u - lz)) & 0xFFFFFu) << 3));
  }
  return asfloat(((e + 112u) << 23) | (m << 3));
}
// Float7e3To32(Float32To7e3(c)) without the encoding: from 2^-2 up, c rounded to nearest even
// at 7 mantissa bits; below, in 7e3's denormal steps of 2^-9, from c * 2^25 truncated as
// Float32To7e3's shift does (exact for the R16G16B16A16_FLOAT host targets either way).
float3 Quantize7e3(float3 c) {
  c = clamp(c, 0.0, 31.875);
  uint3 u = asuint(c);
  float3 normal = asfloat((u + 0x7FFFu + ((u >> 16) & 1u)) & 0xFFFF0000u);
  uint3 t = (uint3)(c * 33554432.0);
  float3 denormal = (float3)((t + 0x7FFFu + ((t >> 16) & 1u)) >> 16) * (1.0 / 512.0);
  return c < 0.25 ? denormal : normal;
}

#if !DEPTH
// One sample as the Xenos EDRAM holds it. The host target of a 7e3 surface is
// R16G16B16A16_FLOAT; ReXGlue packs every sample into 7e3 (alpha 2-bit unorm) when it stores
// the target to EDRAM before a resolve, so values below 7e3's smallest step (2^-9) become 0.
float4 EdramSample(float4 c) {
  if (src_format == 3u || src_format == 12u)
    return float4(Quantize7e3(c.rgb), (float)U(c.a, 3.0) * (1.0 / 3.0));
  return c;
}

float4 LoadColor(int2 p) {
#if MSAA
  uint n = SampleCount(), s = SampleSelect();
  if (s <= 3u) return EdramSample(src.Load(p, HostSample(s)));
  if (n == 4u) {
    if (s == 6u)
      return (EdramSample(src.Load(p, 0)) + EdramSample(src.Load(p, 1)) + EdramSample(src.Load(p, 2)) +
              EdramSample(src.Load(p, 3))) * 0.25;
    if (s == 5u) return (EdramSample(src.Load(p, 2)) + EdramSample(src.Load(p, 3))) * 0.5;
  }
  return (EdramSample(src.Load(p, 0)) + EdramSample(src.Load(p, 1))) * 0.5;
#else
  return EdramSample(src.Load(int3(p, 0)));
#endif
}

// The EDRAM bits of a color render target format (raw copies).
uint4 PackRt(float4 c) {
  switch (src_format) {
    case 0u: case 1u:
      return uint4(U(c.r, 255.0) | (U(c.g, 255.0) << 8) | (U(c.b, 255.0) << 16) | (U(c.a, 255.0) << 24), 0, 0, 0);
    case 2u: case 10u:
      return uint4(U(c.r, 1023.0) | (U(c.g, 1023.0) << 10) | (U(c.b, 1023.0) << 20) | (U(c.a, 3.0) << 30), 0, 0, 0);
    case 3u: case 12u:
      return uint4(F7e3(c.r) | (F7e3(c.g) << 10) | (F7e3(c.b) << 20) | (U(c.a, 3.0) << 30), 0, 0, 0);
    case 6u: return uint4(f32tof16(c.r) | (f32tof16(c.g) << 16), 0, 0, 0);
    case 7u: return uint4(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b) | (f32tof16(c.a) << 16), 0, 0);
    case 14u: return uint4(asuint(c.r), 0, 0, 0);
    case 15u: return uint4(asuint(c.r), asuint(c.g), 0, 0);
    default: return uint4(0, 0, 0, 0);
  }
}

// Texels of the destination format (convert copies).
uint4 PackDest(float4 c) {
  c *= exp2((float)exp_bias);
  if (flags & 2u) c = c.bgra;
  uint number = (flags >> 12) & 7u;
  switch (dest_format) {
    case 2u: return uint4(U(c.r, 255.0), 0, 0, 0);                                  // k_8
    case 10u: return uint4(U(c.r, 255.0) | (U(c.g, 255.0) << 8), 0, 0, 0);          // k_8_8
    // 16-bit formats in the texture cache's host layout (its load conversion applied):
    // k_5_6_5 as B5G6R5 (guest R in bits 0-4 moves to 11-15), k_6_5_5 as B5G6R5 holding
    // R | B << 5 | G << 11 (sampled with swizzle RBGG).
    case 4u: return uint4(U(c.b, 31.0) | (U(c.g, 63.0) << 5) | (U(c.r, 31.0) << 11), 0, 0, 0);
    case 5u: return uint4(U(c.r, 31.0) | (U(c.b, 63.0) << 5) | (U(c.g, 31.0) << 11), 0, 0, 0);
    case 6u:                                                                         // k_8_8_8_8
      if (number == 1u) return uint4(S8(c.r) | (S8(c.g) << 8) | (S8(c.b) << 16) | (S8(c.a) << 24), 0, 0, 0);
      return uint4(U(c.r, 255.0) | (U(c.g, 255.0) << 8) | (U(c.b, 255.0) << 16) | (U(c.a, 255.0) << 24), 0, 0, 0);
    case 7u:                                                                         // k_2_10_10_10
      return uint4(U(c.r, 1023.0) | (U(c.g, 1023.0) << 10) | (U(c.b, 1023.0) << 20) | (U(c.a, 3.0) << 30), 0, 0, 0);
    case 30u: return uint4(f32tof16(c.r), 0, 0, 0);
    case 31u: return uint4(f32tof16(c.r) | (f32tof16(c.g) << 16), 0, 0, 0);
    case 32u: return uint4(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b) | (f32tof16(c.a) << 16), 0, 0);
    case 36u: return uint4(asuint(c.r), 0, 0, 0);
    case 37u: return uint4(asuint(c.r), asuint(c.g), 0, 0);
    case 38u: return asuint(c);
    default: return uint4(0, 0, 0, 0);
  }
}
#endif

uint4 PackPixel(int2 p, out float r32f) {
  r32f = 0.0;
#if DEPTH
#if MSAA
  // Depth is never averaged (ReXGlue SanitizeCopySampleSelect): 4x k23 -> sample 2, else 0.
  uint sel = SampleSelect();
  uint s = HostSample(sel <= 3u ? sel : (sel == 5u && SampleCount() == 4u ? 2u : 0u));
  float d = src_depth.Load(p, s);
  uint stencil = src_stencil.Load(p, s).g;
#else
  float d = src_depth.Load(int3(p, 0));
  uint stencil = src_stencil.Load(int3(p, 0)).g;
#endif
  // D24S8: round to the nearest even like ReXGlue's EDRAM store (OpRoundNE of d * 0xFFFFFF).
  uint d24 = src_format == 1u ? F20e4(d) : (uint)round(saturate(d) * 16777215.0);
  r32f = src_format == 1u ? F20e4ToF32(d24) : (float)d24 / 16777215.0;
  return uint4((d24 << 8) | (stencil & 0xFFu), 0, 0, 0);
#else
  float4 c = LoadColor(p);
  return (flags & 1u) ? PackDest(c) : PackRt(c);
#endif
}

// The components of the UINT view holding packed texel bits v.
uint4 Unpack(uint4 v) {
  switch (dest_layout) {
    case 0u: return uint4(v.x & 0xFFu, 0, 0, 0);
    case 1u: return uint4(v.x & 0xFFu, (v.x >> 8) & 0xFFu, 0, 0);
    case 2u: return uint4(v.x & 0xFFu, (v.x >> 8) & 0xFFu, (v.x >> 16) & 0xFFu, v.x >> 24);
    case 3u: return uint4(v.x & 0x3FFu, (v.x >> 10) & 0x3FFu, (v.x >> 20) & 0x3FFu, v.x >> 30);
    case 4u: return uint4(v.x & 0xFFFFu, 0, 0, 0);
    case 5u: return uint4(v.x & 0xFFFFu, v.x >> 16, 0, 0);
    case 6u: return uint4(v.x & 0xFFFFu, v.x >> 16, v.y & 0xFFFFu, v.y >> 16);
    default: return v;
  }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= size.x || id.y >= size.y) return;
  float r32f;
  uint4 v = PackPixel(src_origin + int2(id.xy), r32f);
  uint2 p = dest_origin + id.xy;
#if DEPTH
  dst[p] = Unpack(v);
  dst_float[p] = r32f.xxxx;
#else
  if (dest_layout == 8u)  // B5G6R5: R in bits 11-15, exact unorm steps
    dst_float[p] = float4(float3((v.x >> 11) & 31u, (v.x >> 5) & 63u, v.x & 31u) / float3(31.0, 63.0, 31.0), 1.0);
  else
    dst[p] = Unpack(v);
#endif
}
)";

struct ShaderConstants {
  int32_t src_origin[2];
  uint32_t size[2];
  uint32_t dest_origin[2];
  uint32_t dest_format, src_format, flags;
  int32_t exp_bias;
  uint32_t dest_layout, pad;
};
static_assert(sizeof(ShaderConstants) == 48);

// What a copy pipeline is compiled for: the generic ones read the resolve's parameters from the
// root constants, a specialized one has them compiled in (kShader's SPEC_* macros).
struct PipelineSpec {
  bool depth = false, msaa = false, specialized = false;
  uint32_t src_format = 0, dest_format = 0, flags = 0, dest_layout = 0;
  int32_t exp_bias = 0;

  static PipelineSpec Of(bool depth, bool msaa, const ShaderConstants& c) {
    return {depth, msaa, true, c.src_format, c.dest_format, c.flags, c.dest_layout, c.exp_bias};
  }
  // Unique per variant: the formats fit 4 and 6 bits, the layout 4, flags 15, the bias is
  // 6-bit signed.
  uint64_t Key() const {
    return uint64_t(depth) | uint64_t(msaa) << 1 | uint64_t(src_format & 15) << 2 |
           uint64_t(dest_format & 63) << 6 | uint64_t(dest_layout & 15) << 12 |
           uint64_t(flags & 0xFFFF) << 16 | uint64_t(uint32_t(exp_bias) & 63) << 32;
  }
};

// RB_COPY_DEST_BASE / RB_COPY_DEST_PITCH (0x2319, 0x231A) in the shadow block 0x2300 at
// device+10680, as the guest Resolve that just ran left them, against the decoded copy. The
// Resolve zeroes RB_COPY_CONTROL and RB_COPY_DEST_INFO there afterwards, so those are checked
// offline against the trace (the dump's copy_control/dest_info vs the trace's RESOLVE lines,
// compare_resolves.py --trace).
void CheckAgainstShadow(const uint8_t* base, uint32_t device, const ResolveParams& p) {
  const uint32_t b = device + 10680 + 0x19 * 4;
  const uint32_t dest = Load32(base, b), pitch = Load32(base, b + 4);
  // The D3D moves the base by the tiled offset of the rectangle origin minus the dest point;
  // only the unshifted case is compared.
  const bool origin = p.x0 == p.dest_x && p.y0 == p.dest_y;
  // RB_COPY_DEST_PITCH's height is the level height minus the dest point's y (r18 in the D3D).
  if ((!origin || dest == p.dest_base) &&
      ((pitch >> 16) & 0x3FFF) == p.dest_height - uint32_t(p.dest_y))
    return;
  static uint32_t logged = 0;
  if (logged++ < 16)
    REXLOG_WARN(
        "PGR4 native resolve: decode vs D3D shadow: dest {:08X}/{:08X} pitch {:08X} (decoded "
        "{}x{}, rect {},{}-{},{} point {},{})",
        dest, p.dest_base, pitch, p.dest_width, p.dest_height, p.x0, p.y0, p.x1, p.y1, p.dest_x,
        p.dest_y);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Decoding

bool DecodeResolve(const uint8_t* base, const HookArgs& args, ResolveParams* out) {
  static uint32_t log_tex = 0, log_cmd = 0, log_packed = 0, log_fmt = 0, log_level = 0;
  ResolveParams p;
  const uint32_t device = args.device;
  const uint32_t flags = args.r[1];
  const uint32_t rect_ptr = args.r[2], tex = args.r[3], point_ptr = args.r[4];
  const uint32_t level = args.r[5], slice = args.r[6], color_ptr = args.r[7];
  if (level >= xenos::kTextureMaxMips) {
    PGR4_LOG_ONCE(log_level, "PGR4 native resolve: destination level {} out of range", level);
    return false;
  }
  p.regs = SurfaceRegsFromDevice(base, device);
  p.source = flags & 7;
  if (p.source > 4) {
    PGR4_LOG_ONCE(log_cmd, "PGR4 native resolve: source select {} unsupported", p.source);
    return false;
  }
  // Sample select: Flags bits 4-6 hold it plus one; 0 means "by the source's MSAA".
  uint32_t field = (flags >> 4) & 7;
  if (!field) {
    const uint32_t msaa = (p.regs.surface_info >> 16) & 3;
    field = msaa == 0 ? 1 : msaa == 1 ? 5 : 7;
  }
  p.sample_select = field - 1;
  if (flags & 0x2000) {
    PGR4_LOG_ONCE(log_cmd, "PGR4 native resolve: null copy (Flags {:08X}) unsupported", flags);
    return false;
  }
  p.copy_command = p.source == 4 ? 0 : 1;
  p.exp_bias = int32_t(flags << 0) >> 26;  // bits 26-31, signed

  // The destination texture's fetch constant: D3DBaseTexture + 28, 6 big-endian dwords.
  if (!tex) {
    PGR4_LOG_ONCE(log_tex, "PGR4 native resolve: no destination texture");
    return false;
  }
  uint32_t fetch[6];
  for (int i = 0; i < 6; ++i) fetch[i] = Load32(base, tex + 28 + 4 * i);
  TextureKey key;
  if (!TextureKeyFromFetch(fetch, &key)) {
    PGR4_LOG_ONCE(log_tex, "PGR4 native resolve: invalid destination fetch constant {:08X} {:08X}",
                  fetch[0], fetch[1]);
    return false;
  }
  if (key.dimension == xenos::DataDimension::k3D) {
    PGR4_LOG_ONCE(log_tex, "PGR4 native resolve: 3D destination unsupported");
    return false;
  }
  const uint32_t fetch_format = fetch[1] & 0x3F;
  p.dest_format = D3DCopyDestFormat(fetch_format);
  p.dest_endian = (fetch[1] >> 6) & 3;
  p.dest_swap = ((fetch[3] >> 1) & 7) != 0;  // swizzle X is not R
  {
    const uint32_t sign_x = (fetch[0] >> 2) & 3;
    const bool integer = fetch[3] & 1;
    p.dest_number = IsFloatFormat(p.dest_format) ? 7
                    : integer                    ? (sign_x == 1 ? 3 : 2)
                                                 : (sign_x == 1 ? 1 : 0);
  }
  if (DestBppLog2(p.dest_format) < 0) {
    PGR4_LOG_ONCE(
        log_fmt,
        "PGR4 native resolve: destination format {} (fetch format {}) unsupported, source {}, "
        "surface {:08X} {:08X} {:08X}",
        p.dest_format, fetch_format, p.source, p.regs.surface_info, p.regs.color_info[0],
        p.regs.depth_info);
    return false;
  }
  p.dest_width = std::max(1u, key.width >> level);
  p.dest_height = std::max(1u, key.height >> level);
  // Destination address of the level and slice. The texture object holds virtual addresses;
  // GpuAddress is also the physical address the GPU-side fetch constant (and so the texture
  // cache) sees.
  const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      key.dimension, key.pitch, key.width, key.height, key.depth_or_array_size, key.tiled,
      rex::graphics::GetBaseFormat(xenos::TextureFormat(fetch_format)), key.packed_mips, true,
      std::max(level, key.mip_max_level));
  const texture_util::TextureGuestLayout::Level* lv;
  if (level == 0) {
    p.dest_base = GpuAddress(fetch[1] & 0xFFFFF000u);
    lv = &layout.base;
  } else {
    if (layout.packed_level != UINT32_MAX && level >= layout.packed_level) {
      PGR4_LOG_ONCE(log_packed,
                    "PGR4 native resolve: packed mip tail destination (level {}) unsupported",
                    level);
      return false;
    }
    p.dest_base = GpuAddress(fetch[5] & 0xFFFFF000u) + layout.mip_offsets_bytes[level];
    lv = &layout.mips[level];
  }
  p.dest_base += lv->array_slice_stride_bytes * slice;
  p.dest_tiled = key.tiled;
  p.dest_pitch_blocks = lv->row_pitch_bytes >> DestBppLog2(p.dest_format);
  p.dest_extent_bytes = lv->array_slice_data_extent_bytes;

  // Rectangle: pSourceRect or the destination level's size; right/bottom rounded up to 8.
  if (rect_ptr) {
    p.x0 = int32_t(Load32(base, rect_ptr));
    p.y0 = int32_t(Load32(base, rect_ptr + 4));
    p.x1 = int32_t(Load32(base, rect_ptr + 8));
    p.y1 = int32_t(Load32(base, rect_ptr + 12));
  } else {
    p.x1 = int32_t(p.dest_width);
    p.y1 = int32_t(p.dest_height);
  }
  p.x1 = (p.x1 + 7) & ~7;
  p.y1 = (p.y1 + 7) & ~7;
  if (point_ptr) {
    p.dest_x = int32_t(Load32(base, point_ptr));
    p.dest_y = int32_t(Load32(base, point_ptr + 4));
  }
  // Clears: color from pClearColor (default: the D3D's constant at 0x82258A0C), depth from
  // ClearZ (f1), stencil from the stack argument (caller r1 + 92, read in sub_826CBEE0).
  p.clear_flags = flags & 0x300;
  if (p.clear_flags) {
    if (color_ptr) {
      for (int i = 0; i < 4; ++i) p.clear_color[i] = LoadFloat(base, color_ptr + 4 * i);
    } else {
      const float c = LoadFloat(base, 0x82258A0Cu);
      for (float& v : p.clear_color) v = c;
    }
    p.clear_depth = float(args.f[0]);
    p.clear_stencil = uint8_t(Load32(base, args.sp + 92));
    if (args.tiling == TilingCall::kEnd) {
      const uint32_t tiles = std::min(Load32(base, device + kDevTileCount), kMaxTiles);
      for (uint32_t i = 0; i < tiles; ++i) {
        const uint32_t r = device + kDevTileRects + 16 * i;
        if (p.x0 >= int32_t(Load32(base, r)) && p.y0 >= int32_t(Load32(base, r + 4)) &&
            p.x0 < int32_t(Load32(base, r + 8)) && p.y0 < int32_t(Load32(base, r + 12))) {
          p.edram_x = int32_t(Load32(base, device + kDevTileOrigins + 8 * i));
          p.edram_y = int32_t(Load32(base, device + kDevTileOrigins + 8 * i + 4));
          break;
        }
      }
    }
  }
  CheckAgainstShadow(base, device, p);
  *out = p;
  return true;
}

// ---------------------------------------------------------------------------------------------
// Resolver

struct Resolver::Image {
  ComPtr<ID3D12Resource> texture, depth_r32f;
  const DestStorage* storage = nullptr;
  uint32_t width = 0, height = 0;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;  // of both resources
  ResolvedImage view;
};

namespace {
uint32_t g_full_readback_holds = 0;
std::string g_readback_before;
}  // namespace

void HoldFullReadback(bool hold) {
  // The plugin reads the cvar per resolve on its GPU thread; "fast", "some" and "full" are all
  // short strings, so the assignment never reallocates under it.
  if (hold) {
    if (g_full_readback_holds++ == 0) {
      g_readback_before = rex::cvar::GetFlagByName("readback_resolve");
      if (!rex::cvar::SetFlagByName("readback_resolve", "full"))
        REXLOG_WARN(
            "PGR4 native: could not set readback_resolve=full; guest copies may be a frame old");
    }
  } else if (g_full_readback_holds && --g_full_readback_holds == 0) {
    rex::cvar::SetFlagByName("readback_resolve",
                             g_readback_before.empty() ? "none" : g_readback_before);
  }
}

struct Resolver::Dump {
  std::filesystem::path dir;
  bool recording = false;
  uint64_t frame = 0, fence = 0;  // the dumped frame and the fence it signals
  struct Entry {
    ResolveParams p;
    uint32_t width, height, row_pitch, bpp;
    ComPtr<ID3D12Resource> readback;
    std::vector<uint8_t> guest;  // guest memory at dest_base after the frame (AfterSwap)
  };
  std::vector<Entry> entries;
  bool guest_read = false;
};

// The copy pipelines. Variants are looked up and queued on the recording thread and compiled on
// the worker; Variant::done publishes the pipeline.
struct Resolver::Pipelines {
  enum class Specialize { kOff, kAsync, kSync };
  struct Variant {
    ComPtr<ID3D12PipelineState> pso;  // null if compilation failed
    std::atomic<bool> done{false};
  };

  ID3D12Device* device = nullptr;
  pD3DCompile compile = nullptr;
  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> generic[4];  // [depth][msaa]
  Specialize specialize = Specialize::kAsync;
  std::unordered_map<uint64_t, std::unique_ptr<Variant>> variants;

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::pair<PipelineSpec, Variant*>> jobs;
  bool stopping = false;
  std::thread worker;

  ~Pipelines() {
    if (!worker.joinable()) return;
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    cv.notify_all();
    worker.join();
  }

  ComPtr<ID3D12PipelineState> Compile(const PipelineSpec& s) const {
    std::vector<D3D_SHADER_MACRO> defines = {{"DEPTH", s.depth ? "1" : "0"},
                                             {"MSAA", s.msaa ? "1" : "0"},
                                             {"SPECIALIZED", s.specialized ? "1" : "0"}};
    const std::string values[] = {std::to_string(s.src_format) + "u",
                                  std::to_string(s.dest_format) + "u",
                                  std::to_string(s.flags) + "u",
                                  std::to_string(s.dest_layout) + "u", std::to_string(s.exp_bias)};
    if (s.specialized) {
      const char* names[] = {"SPEC_SRC_FORMAT", "SPEC_DEST_FORMAT", "SPEC_FLAGS",
                             "SPEC_DEST_LAYOUT", "SPEC_EXP_BIAS"};
      for (size_t i = 0; i < std::size(names); ++i)
        defines.push_back({names[i], values[i].c_str()});
    }
    defines.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(compile(kShader, sizeof(kShader) - 1, "pgr4_resolve.hlsl", defines.data(), nullptr,
                       "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
      REXLOG_ERROR("PGR4 native resolve: shader {:09X}{}: {}", s.Key(),
                   s.specialized ? "" : " (generic)",
                   errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
      return nullptr;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = root_signature.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    if (Failed(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)),
               "resolve CreateComputePipelineState"))
      return nullptr;
    return pso;
  }

  // The specialized pipeline of the spec once compiled, else the generic one; queues the
  // variant the first time it is asked for.
  ID3D12PipelineState* Get(const PipelineSpec& s) {
    ID3D12PipelineState* fallback = generic[(s.depth ? 2 : 0) + (s.msaa ? 1 : 0)].Get();
    if (specialize == Specialize::kOff) return fallback;
    auto [it, added] = variants.try_emplace(s.Key());
    if (added) {
      it->second = std::make_unique<Variant>();
      if (specialize == Specialize::kSync) {
        it->second->pso = Compile(s);
        it->second->done.store(true, std::memory_order_release);
      } else {
        {
          std::lock_guard lock(mutex);
          jobs.emplace_back(s, it->second.get());
        }
        if (!worker.joinable()) {
          worker = std::thread([this] { Work(); });
          SetThreadDescription(worker.native_handle(), L"PGR4 native resolve shaders");
        }
        cv.notify_one();
      }
    }
    const Variant& v = *it->second;
    return v.done.load(std::memory_order_acquire) && v.pso ? v.pso.Get() : fallback;
  }

  void Work() {
    for (;;) {
      std::pair<PipelineSpec, Variant*> job;
      {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return stopping || !jobs.empty(); });
        if (stopping) return;
        job = jobs.front();
        jobs.pop_front();
      }
      const uint64_t t0 = NowNs();
      job.second->pso = Compile(job.first);
      job.second->done.store(true, std::memory_order_release);
      REXLOG_INFO("PGR4 native resolve: copy shader {:09X} {} in {:.1f} ms", job.first.Key(),
                  job.second->pso ? "specialized" : "failed", double(NowNs() - t0) / 1e6);
    }
  }
};

Resolver::Resolver() = default;
Resolver::~Resolver() = default;
bool Resolver::Initialize(ID3D12Device* device, RenderTargets* targets, ID3D12CommandQueue* queue) {
  device_ = device;
  targets_ = targets;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = kHeapPerFrame * kHeapSlots;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)),
             "resolve CreateDescriptorHeap"))
    return false;
  heap_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {DXGI_FORMAT_B5G6R5_UNORM};
  b5g6r5_store_ =
      SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
      (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
  if (!b5g6r5_store_)
    REXLOG_WARN("PGR4 native resolve: no typed UAV store for B5G6R5; k_5_6_5 resolves unsupported");
  if (!CreatePipelines()) return false;
  const std::string dir = REXCVAR_GET(pgr4_native_resolve_dump);
  if (!dir.empty()) {
    dump_ = std::make_unique<Dump>();
    dump_->dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(dump_->dir, ec);
    REXLOG_INFO("PGR4 native resolve: dumps to {} (create dump.request there)", dir);
  }
  if (REXCVAR_GET(pgr4_native_resolve_selftest) && queue)
    REXLOG_INFO("PGR4 native resolve selftest: {}", SelfTest(queue) ? "all passed" : "FAILED");
  pipelines_->specialize = REXCVAR_GET(pgr4_native_resolve_specialize)
                               ? Pipelines::Specialize::kAsync
                               : Pipelines::Specialize::kOff;
  return true;
}

bool Resolver::CreatePipelines() {
  // Root signature: b0 = 12 root constants, one table of t0-t1 (sources) and u0-u1 (images).
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 2;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 2;
  ranges[1].OffsetInDescriptorsFromTableStart = 2;
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = sizeof(ShaderConstants) / 4;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 2;
  params[1].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rd = {};
  rd.NumParameters = 2;
  rd.pParameters = params;
  ComPtr<ID3DBlob> blob, error;
  if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) {
    REXLOG_ERROR("PGR4 native resolve: root signature: {}",
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    return false;
  }
  pipelines_ = std::make_unique<Pipelines>();
  Pipelines& pl = *pipelines_;
  pl.device = device_;
  if (Failed(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&pl.root_signature)),
             "resolve CreateRootSignature"))
    return false;

  pl.compile = D3DCompiler();
  if (!pl.compile) return false;
  for (int depth = 0; depth < 2; ++depth) {
    for (int msaa = 0; msaa < 2; ++msaa) {
      PipelineSpec s;
      s.depth = depth;
      s.msaa = msaa;
      if (!(pl.generic[depth * 2 + msaa] = pl.Compile(s))) return false;
    }
  }
  return true;
}

Resolver::Image* Resolver::GetImage(const ResolveParams& p) {
  static uint32_t log_store = 0;
  const DestStorage& storage = *GetDestStorage(p.dest_format);
  if (storage.layout == kB5G6R5Unorm && !b5g6r5_store_) {
    PGR4_LOG_ONCE(log_store, "PGR4 native resolve: no typed UAV store for B5G6R5 (format {})",
                  p.dest_format);
    return nullptr;
  }
  const bool depth = p.source == 4;
  Destination& dest = destinations_[p.dest_base];
  for (const std::unique_ptr<Image>& img : dest.images)
    if (img->storage == &storage && img->width == p.dest_width && img->height == p.dest_height &&
        bool(img->depth_r32f) == depth)
      return img.get();
  auto img = std::make_unique<Image>();
  img->storage = &storage;
  img->width = p.dest_width;
  img->height = p.dest_height;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = p.dest_width;
  d.Height = p.dest_height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = storage.resource;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&img->texture)),
             "resolve destination texture"))
    return nullptr;
  if (depth) {
    d.Format = DXGI_FORMAT_R32_FLOAT;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&img->depth_r32f)),
               "resolve depth_r32f texture"))
      return nullptr;
  }
  img->view.guest_base = p.dest_base;
  img->view.format = xenos::TextureFormat(p.dest_format);
  img->view.texture = img->texture.Get();
  img->view.depth_r32f = img->depth_r32f.Get();
  // Creation is a stall on the guest thread; every one is logged so frame-time spikes can be
  // matched against it.
  REXLOG_INFO("PGR4 native resolve: image {} at {:08X}: format {} {}x{}{} ({} at this address)",
              ++image_count_, p.dest_base, p.dest_format, p.dest_width, p.dest_height,
              depth ? " + depth float" : "", dest.images.size() + 1);
  return dest.images.emplace_back(std::move(img)).get();
}

const ResolvedImage* Resolver::Find(uint32_t guest_phys_base) const {
  auto it = destinations_.find(guest_phys_base);
  if (it == destinations_.end() || !it->second.current) return nullptr;
  return &it->second.current->view;
}

void Resolver::BeginFrame(uint64_t frame, uint64_t frame_fence, uint64_t completed_fence) {
  frame_ = frame;
  frame_fence_ = frame_fence;
  heap_next_ = 0;
  if (dump_) DumpBeginFrame(completed_fence);
}

static uint32_t CopyControlOf(const ResolveParams& p) {
  return p.source | (p.sample_select << 4) | p.clear_flags | (p.copy_command << 20);
}
static uint32_t DestInfoOf(const ResolveParams& p) {
  return p.dest_endian | (p.dest_format << 7) | (p.dest_number << 13) |
         ((uint32_t(p.exp_bias) & 0x3F) << 16) | (uint32_t(p.dest_swap) << 24);
}

bool Resolver::Resolve(ID3D12GraphicsCommandList* list, const ResolveParams& p) {
  static uint32_t log_raw = 0, log_rt = 0, log_img = 0, log_heap = 0;
  const bool depth = p.source == 4;
  const DestStorage* storage = GetDestStorage(p.dest_format);
  if (!storage) return false;
  RenderTarget* rt = depth ? targets_->GetDepth(p.regs, uint32_t(std::max(p.y1, 0)))
                           : targets_->GetColor(p.regs, p.source, uint32_t(std::max(p.y1, 0)));
  if (!rt) {
    PGR4_LOG_ONCE(log_rt, "PGR4 native resolve: no host target for source {} (surface {:08X})",
                  p.source, p.regs.surface_info);
    return false;
  }
  if (p.copy_command == 0 && !depth &&
      ColorRtBppLog2(rt->guest_format) != uint32_t(storage->bpp_log2)) {
    PGR4_LOG_ONCE(log_raw, "PGR4 native resolve: raw copy from RT format {} into format {}",
                  rt->guest_format, p.dest_format);
    return false;
  }
  if (!depth) targets_->SyncEdram(list, rt);
  // Clip the rectangle to the source target and the destination level.
  int32_t x0 = std::max(p.x0, 0), y0 = std::max(p.y0, 0);
  int32_t x1 = std::min<int32_t>(p.x1, int32_t(rt->width)),
          y1 = std::min<int32_t>(p.y1, int32_t(rt->height));
  int32_t dx = p.dest_x + (x0 - p.x0), dy = p.dest_y + (y0 - p.y0);
  x1 = std::min<int32_t>(x1, x0 + int32_t(p.dest_width) - dx);
  y1 = std::min<int32_t>(y1, y0 + int32_t(p.dest_height) - dy);
  if (x1 <= x0 || y1 <= y0 || dx < 0 || dy < 0) return true;  // nothing to copy
  const uint32_t w = uint32_t(x1 - x0), h = uint32_t(y1 - y0);

  Image* img = GetImage(p);
  if (!img) {
    PGR4_LOG_ONCE(log_img, "PGR4 native resolve: no host image for format {} {}x{}", p.dest_format,
                  p.dest_width, p.dest_height);
    return false;
  }

  // Descriptors: t0 = color / depth plane, t1 = stencil plane, u0 = the image's UINT view,
  // u1 = its float view (B5G6R5) or the depth float image; null where unused.
  if (heap_next_ + kViewsPerResolve > kHeapPerFrame) {
    // The frame's earlier dispatches read their descriptors when the GPU runs them.
    PGR4_LOG_ONCE(log_heap, "PGR4 native resolve: more than {} resolves in a frame; skipped",
                  kHeapPerFrame / kViewsPerResolve);
    return false;
  }
  const uint32_t slot = uint32_t(frame_ % kHeapSlots) * kHeapPerFrame + heap_next_;
  heap_next_ += kViewsPerResolve;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(slot) * heap_stride_;
  auto view = [&](uint32_t i) {
    return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + size_t(i) * heap_stride_};
  };
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(slot) * heap_stride_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.ViewDimension =
      rt->samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
  if (rt->samples == 1) sd.Texture2D.MipLevels = 1;
  sd.Format = rt->srv_format;
  device_->CreateShaderResourceView(rt->resource.Get(), &sd, view(0));
  if (depth) {
    sd.Format = rt->stencil_srv_format;
    if (rt->samples == 1) sd.Texture2D.PlaneSlice = 1;
    device_->CreateShaderResourceView(rt->resource.Get(), &sd, view(1));
  } else {
    D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
    nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nd.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(nullptr, &nd, view(1));
  }
  const bool float_texels = storage->layout == kB5G6R5Unorm;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  ud.Format = float_texels ? DXGI_FORMAT_R32_UINT : storage->uav;
  device_->CreateUnorderedAccessView(float_texels ? nullptr : img->texture.Get(), nullptr, &ud,
                                     view(2));
  ud.Format = float_texels ? storage->uav : DXGI_FORMAT_R32_FLOAT;
  device_->CreateUnorderedAccessView(float_texels ? img->texture.Get() : img->depth_r32f.Get(),
                                     nullptr, &ud, view(3));

  // Source readable by the compute shader, the image(s) writable: one barrier batch.
  D3D12_RESOURCE_BARRIER bs[3];
  uint32_t nb = 0;
  constexpr D3D12_RESOURCE_STATES kSourceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (rt->state != kSourceState) {
    bs[nb++] = TransitionBarrier(rt->resource.Get(), rt->state, kSourceState);
    rt->state = kSourceState;
  }
  if (img->state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    bs[nb++] =
        TransitionBarrier(img->texture.Get(), img->state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (img->depth_r32f)
      bs[nb++] = TransitionBarrier(img->depth_r32f.Get(), img->state,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (nb) list->ResourceBarrier(nb, bs);
  ShaderConstants c = {};
  c.src_origin[0] = x0;
  c.src_origin[1] = y0;
  c.size[0] = w;
  c.size[1] = h;
  c.dest_origin[0] = uint32_t(dx);
  c.dest_origin[1] = uint32_t(dy);
  c.dest_format = p.dest_format;
  c.src_format = rt->guest_format;
  c.flags = (p.copy_command == 1 ? 1u : 0u) | (p.dest_swap ? 2u : 0u) | (p.sample_select << 4) |
            (rt->samples << 8) | (p.dest_number << 12);
  c.exp_bias = p.exp_bias;
  c.dest_layout = storage->layout;
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(pipelines_->root_signature.Get());
  list->SetPipelineState(pipelines_->Get(PipelineSpec::Of(depth, rt->samples > 1, c)));
  list->SetComputeRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
  list->SetComputeRootDescriptorTable(1, gpu);
  if (timer_) timer_->Pass(list, GpuTimer::KeyResolve(p, GpuTimer::kResolveCopy));
  list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

  if (timer_) timer_->Pass(list, GpuTimer::KeyResolve(p, GpuTimer::kResolveRelease));
  nb = 0;
  bs[nb++] =
      TransitionBarrier(img->texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kImageReadState);
  if (img->depth_r32f)
    bs[nb++] = TransitionBarrier(img->depth_r32f.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 kImageReadState);
  list->ResourceBarrier(nb, bs);
  img->state = kImageReadState;
  img->view.width = std::max(img->view.width, uint32_t(dx) + w);
  img->view.height = std::max(img->view.height, uint32_t(dy) + h);
  img->view.seq = ++seq_;
  destinations_[p.dest_base].current = img;
  if (changed_) changed_(p.dest_base);

  if (dump_ && dump_->recording) DumpRecord(list, p, img, uint32_t(dx) + w, uint32_t(dy) + h);

  // Clears of the source surfaces over the resolved rectangle (after the copy).
  if (p.clear_flags) {
    if (timer_) timer_->Pass(list, GpuTimer::KeyResolve(p, GpuTimer::kResolveClear));
    ClearParams cp;
    cp.flags = ((p.clear_flags & 0x100) ? (1u << (depth ? 0 : p.source)) : 0) |
               ((p.clear_flags & 0x200) ? 0x30u : 0);
    std::memcpy(cp.color, p.clear_color, sizeof(cp.color));
    cp.depth = p.clear_depth;
    cp.stencil = p.clear_stencil;
    cp.rects.push_back({LONG(x0), LONG(y0), LONG(x1), LONG(y1)});
    if (p.edram_x || p.edram_y)
      cp.edram_rects.push_back(
          {LONG(x0 - p.edram_x), LONG(y0 - p.edram_y), LONG(x1 - p.edram_x), LONG(y1 - p.edram_y)});
    targets_->Clear(list, p.regs, 0xF, true, uint32_t(std::max(p.y1, 0)), cp);
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Oracle dumps

void Resolver::DumpRecord(ID3D12GraphicsCommandList* list, const ResolveParams& p, Image* img,
                          uint32_t width, uint32_t height) {
  Dump& d = *dump_;
  Dump::Entry e;
  e.p = p;
  e.width = width;
  e.height = height;
  e.bpp = 1u << DestBppLog2(p.dest_format);
  e.row_pitch = (width * e.bpp + kPitchAlign - 1) & ~(kPitchAlign - 1);
  if (!CreateBuffer(device_, D3D12_HEAP_TYPE_READBACK, uint64_t(e.row_pitch) * height,
                    D3D12_RESOURCE_STATE_COPY_DEST, "resolve dump readback", &e.readback))
    return;
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = img->texture.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = e.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {img->storage->resource, width, height, 1, e.row_pitch};
  D3D12_BOX box = {0, 0, 0, width, height, 1};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  d.entries.push_back(std::move(e));
}

void Resolver::AfterSwap() {
  if (!dump_ || !dump_->recording) return;
  Dump& d = *dump_;
  d.recording = false;
  // The guest thread is in the dumped frame's Swap: let the Xenos plugin's GPU thread execute
  // the frame (its resolves read back synchronously, HoldFullReadback) before reading memory.
  Sleep(DWORD(std::max(0, REXCVAR_GET(pgr4_native_capture_hold_ms))));
  const uint8_t* physical = rex::system::kernel_memory()->physical_membase();
  for (Dump::Entry& e : d.entries) {
    const uint8_t* g = physical + e.p.dest_base;
    e.guest.assign(g, g + e.p.dest_extent_bytes);
  }
  d.guest_read = true;
  HoldFullReadback(false);
  REXLOG_INFO("PGR4 native resolve: read guest memory of {} resolves of frame {}", d.entries.size(),
              d.frame);
}

void Resolver::DumpBeginFrame(uint64_t completed_fence) {
  Dump& d = *dump_;
  const std::string prefix = "f" + std::to_string(d.frame);
  if (d.guest_read && completed_fence >= d.fence) {
    std::FILE* meta = std::fopen((d.dir / (prefix + "-resolves.jsonl")).string().c_str(), "wb");
    for (size_t i = 0; i < d.entries.size(); ++i) {
      Dump::Entry& e = d.entries[i];
      const ResolveParams& p = e.p;
      const std::string stem = prefix + "-r" + std::to_string(i);
      void* data = nullptr;
      D3D12_RANGE range = {0, size_t(e.row_pitch) * e.height};
      if (SUCCEEDED(e.readback->Map(0, &range, &data))) {
        std::FILE* f = std::fopen((d.dir / (stem + "-native.bin")).string().c_str(), "wb");
        if (f) {
          for (uint32_t y = 0; y < e.height; ++y)
            std::fwrite(static_cast<const uint8_t*>(data) + size_t(y) * e.row_pitch, 1,
                        size_t(e.width) * e.bpp, f);
          std::fclose(f);
        }
        D3D12_RANGE none = {0, 0};
        e.readback->Unmap(0, &none);
      }
      if (std::FILE* f = std::fopen((d.dir / (stem + "-guest.bin")).string().c_str(), "wb")) {
        std::fwrite(e.guest.data(), 1, e.guest.size(), f);
        std::fclose(f);
      }
      if (meta)
        std::fprintf(meta,
                     "{\"index\":%zu,\"source\":%u,\"surface_info\":%u,\"source_info\":%u,"
                     "\"sample_select\":%u,\"copy_command\":%u,\"rect\":[%d,%d,%d,%d],"
                     "\"dest_point\":[%d,%d],\"dest_base\":%u,\"dest_format\":%u,\"dest_width\":%u,"
                     "\"dest_height\":%u,\"endian\":%u,\"number\":%u,\"swap\":%d,\"exp_bias\":%d,"
                     "\"tiled\":%d,\"pitch_blocks\":%u,\"extent_bytes\":%u,\"bpp\":%u,"
                     "\"native_width\":%u,\"native_height\":%u,\"clear_flags\":%u,"
                     "\"copy_control\":%u,\"dest_info\":%u}\n",
                     i, p.source, p.regs.surface_info,
                     p.source == 4 ? p.regs.depth_info : p.regs.color_info[p.source & 3],
                     p.sample_select, p.copy_command, p.x0, p.y0, p.x1, p.y1, p.dest_x, p.dest_y,
                     p.dest_base, p.dest_format, p.dest_width, p.dest_height, p.dest_endian,
                     p.dest_number, p.dest_swap ? 1 : 0, p.exp_bias, p.dest_tiled ? 1 : 0,
                     p.dest_pitch_blocks, p.dest_extent_bytes, e.bpp, e.width, e.height,
                     p.clear_flags, CopyControlOf(p), DestInfoOf(p));
    }
    if (meta) std::fclose(meta);
    REXLOG_INFO("PGR4 native resolve: dumped {} resolves (native + guest) of frame {} to {}",
                d.entries.size(), d.frame, d.dir.string());
    d.entries.clear();
    d.guest_read = false;
  }
  if (!d.recording && !d.guest_read) {
    std::error_code ec;
    if (std::filesystem::remove(d.dir / "dump.request", ec)) {
      HoldFullReadback(true);  // before any of this frame's resolves reach the plugin
      d.recording = true;
      d.frame = frame_;
      d.fence = frame_fence_;  // signalled when the dumped frame is done on the GPU
      REXLOG_INFO("PGR4 native resolve: dumping the resolves of frame {}", frame_);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Self test

bool Resolver::SelfTest(ID3D12CommandQueue* queue) {
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  if (Failed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
             "selftest allocator") ||
      Failed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                        IID_PPV_ARGS(&list)),
             "selftest list") ||
      Failed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
             "selftest fence"))
    return false;
  list->Close();
  struct Case {
    const char* name;
    bool depth;
    uint32_t msaa;    // RB_SURFACE_INFO msaa_samples
    uint32_t format;  // color or depth render target format
    float color[4];
    float z;
    uint8_t stencil;
    uint32_t dest_format, copy_command, sample_select;
    bool swap;
    int32_t bias;
    uint32_t expect[2];  // first one or two dwords of every texel
    float expect_r32f;
  };
  // Expected values: the Xenos encodings, worked out by hand (unorm: v * max + 0.5 truncated;
  // 20e4 and 7e3 as xenos.h; halves exact). 7e3 sources go through the EDRAM's 7e3 first:
  // 1e-6 is below 7e3's smallest step and becomes 0, alpha 0.5 becomes 2/3 (2-bit unorm).
  const Case cases[] = {
      {"8888 1x -> k_8_8_8_8",
       false,
       0,
       0,
       {64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f},
       0,
       0,
       6,
       1,
       0,
       false,
       0,
       {0xFFBF8040u, 0},
       0},
      {"8888 1x -> k_8_8_8_8 swap",
       false,
       0,
       0,
       {64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f},
       0,
       0,
       6,
       1,
       0,
       true,
       0,
       {0xFF4080BFu, 0},
       0},
      {"8888 1x raw",
       false,
       0,
       0,
       {64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f},
       0,
       0,
       6,
       0,
       0,
       false,
       0,
       {0xFFBF8040u, 0},
       0},
      {"7e3 4x avg4 -> k_16_16_16_16_FLOAT",
       false,
       2,
       3,
       {1.5f, 0.25f, 1e-6f, 0.5f},
       0,
       0,
       32,
       1,
       6,
       false,
       0,
       {0x34003E00u, 0x39550000u},
       0},
      {"7e3 1x -> k_8_8_8_8 (saturate)",
       false,
       0,
       3,
       {1.5f, 0.25f, 0.0f, 0.5f},
       0,
       0,
       6,
       1,
       0,
       false,
       0,
       {0xAA0040FFu, 0},
       0},
      // Ties round to even: 2.5 denormal steps -> 2, 1 + 1/256 -> 1, 1 + 3/256 -> 1 + 2/128.
      {"7e3 1x ties -> k_16_16_16_16_FLOAT",
       false,
       0,
       3,
       {2.5f / 512, 1.00390625f, 1.01171875f, 1.0f},
       0,
       0,
       32,
       1,
       0,
       false,
       0,
       {0x3C001C00u, 0x3C003C10u},
       0},
      {"7e3_AS_16 2x avg2 -> k_2_10_10_10",
       false,
       1,
       12,
       {0.5f, 0.25f, 1.0f, 1.0f},
       0,
       0,
       7,
       1,
       4,
       false,
       0,
       {0xFFF40200u, 0},
       0},
      {"8888 1x -> k_5_6_5",
       false,
       0,
       0,
       {1.0f, 0.0f, 0.0f, 1.0f},
       0,
       0,
       4,
       1,
       0,
       false,
       0,
       {0xF800u, 0},
       0},
      {"8888 1x -> k_6_5_5",
       false,
       0,
       0,
       {0.0f, 1.0f, 0.0f, 1.0f},
       0,
       0,
       5,
       1,
       0,
       false,
       0,
       {0xF800u, 0},
       0},
      {"32F 1x -> k_32_FLOAT",
       false,
       0,
       14,
       {3.0f, 0, 0, 0},
       0,
       0,
       36,
       1,
       0,
       false,
       0,
       {0x40400000u, 0},
       0},
      {"16F 1x bias -1 -> k_16_16_16_16_FLOAT",
       false,
       0,
       7,
       {2.0f, 1.0f, 0.5f, 4.0f},
       0,
       0,
       32,
       1,
       0,
       false,
       -1,
       {0x38003C00u, 0x40003400u},
       0},
      {"D24FS8 2x raw -> k_8_8_8_8",
       true,
       1,
       1,
       {},
       0.5f,
       0x5A,
       6,
       0,
       0,
       false,
       0,
       {0xE000005Au, 0},
       0.5f},
      // 0.5 + 7 ulp: ReXGlue truncates host D32 to 20e4 (round to nearest even would give E00001).
      {"D24FS8 1x truncate -> k_8_8_8_8",
       true,
       0,
       1,
       {},
       std::bit_cast<float>(0x3F000007u),
       0x5A,
       6,
       0,
       0,
       false,
       0,
       {0xE000005Au, 0},
       0.5f},
      {"D24S8 1x raw -> k_8_8_8_8",
       true,
       0,
       0,
       {},
       0.25f,
       0x11,
       6,
       0,
       0,
       false,
       0,
       {0x40000011u, 0},
       float(0x400000) / 16777215.0f},
  };
  constexpr uint32_t kPitch = 64, kDestW = 64, kDestH = 32;
  const int32_t rx0 = 8, ry0 = 8, rx1 = 40, ry1 = 24, px = 16, py = 4;  // rect and dest point
  uint64_t fence_value = 0;
  bool all_ok = true;
  // Every case through the generic pipeline, then through its specialized one.
  for (const Pipelines::Specialize mode :
       {Pipelines::Specialize::kOff, Pipelines::Specialize::kSync}) {
    const char* const mode_name = mode == Pipelines::Specialize::kOff ? "generic" : "specialized";
    pipelines_->specialize = mode;
    uint32_t index = 0;
    for (const Case& c : cases) {
      ResolveParams p;
      p.regs.surface_info = kPitch | (c.msaa << 16);
      const uint32_t base = 1024 + index * 32;  // EDRAM bases the game does not use at start
      if (c.depth)
        p.regs.depth_info = base | (c.format << 16);
      else
        p.regs.color_info[0] = base | (c.format << 16);
      p.source = c.depth ? 4 : 0;
      p.sample_select = c.sample_select;
      p.copy_command = c.copy_command;
      p.x0 = rx0;
      p.y0 = ry0;
      p.x1 = rx1;
      p.y1 = ry1;
      p.dest_x = px;
      p.dest_y = py;
      p.dest_base = 0xFFF00000u + index * 0x10000u;  // not a guest address: never found by textures
      p.dest_format = c.dest_format;
      p.dest_width = kDestW;
      p.dest_height = kDestH;
      p.dest_number = IsFloatFormat(c.dest_format) ? 7 : 0;
      p.dest_swap = c.swap;
      p.exp_bias = c.bias;
      ++index;

      alloc->Reset();
      list->Reset(alloc.Get(), nullptr);
      ClearParams cp;
      cp.flags = c.depth ? 0x30 : 0x1;
      std::memcpy(cp.color, c.color, sizeof(cp.color));
      cp.depth = c.z;
      cp.stencil = c.stencil;
      targets_->Clear(list.Get(), p.regs, 1, c.depth, kDestH * 2, cp);
      bool ok = Resolve(list.Get(), p);
      Image* img = ok ? destinations_[p.dest_base].current : nullptr;
      const uint32_t bpp = 1u << DestBppLog2(c.dest_format);
      const uint32_t w = uint32_t(rx1 - rx0), h = uint32_t(ry1 - ry0);
      const uint32_t row_pitch = (kDestW * bpp + kPitchAlign - 1) & ~(kPitchAlign - 1);
      ComPtr<ID3D12Resource> readback[2];
      if (img) {
        for (int k = 0; k < (img->depth_r32f ? 2 : 1); ++k) {
          if (!CreateBuffer(device_, D3D12_HEAP_TYPE_READBACK, uint64_t(row_pitch) * kDestH,
                            D3D12_RESOURCE_STATE_COPY_DEST, "selftest readback", &readback[k]))
            return false;
          D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
          src.pResource = k ? img->depth_r32f.Get() : img->texture.Get();
          src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          dst.pResource = readback[k].Get();
          dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          dst.PlacedFootprint.Footprint = {k ? DXGI_FORMAT_R32_FLOAT : img->storage->resource,
                                           kDestW, kDestH, 1, row_pitch};
          list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
      }
      list->Close();
      ID3D12CommandList* lists[] = {list.Get()};
      queue->ExecuteCommandLists(1, lists);
      queue->Signal(fence.Get(), ++fence_value);
      const UniqueHandle done = CreateAutoResetEvent();
      if (!done || FAILED(fence->SetEventOnCompletion(fence_value, done.get())) ||
          WaitForSingleObject(done.get(), 5000) != WAIT_OBJECT_0) {
        REXLOG_ERROR("PGR4 native resolve selftest: {} {}: the GPU did not finish", mode_name,
                     c.name);
        return false;
      }
      uint32_t bad = 0, first_bad[2] = {}, first_x = 0, first_y = 0;
      float r32f_seen = 0;
      if (img) {
        void* mapped[2] = {};
        if (Failed(readback[0]->Map(0, nullptr, &mapped[0]), "selftest Map") ||
            (readback[1] && Failed(readback[1]->Map(0, nullptr, &mapped[1]), "selftest Map")))
          return false;
        const auto* data = static_cast<const uint8_t*>(mapped[0]);
        const auto* fdata = static_cast<const uint8_t*>(mapped[1]);
        for (uint32_t y = 0; y < h; ++y) {
          for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* t = data + size_t(py + y) * row_pitch + size_t(px + x) * bpp;
            uint32_t v[2] = {};
            std::memcpy(v, t, std::min(bpp, 8u));
            bool texel_ok = v[0] == c.expect[0] && (bpp < 8 || v[1] == c.expect[1]);
            if (fdata) {
              float f;
              std::memcpy(&f, fdata + size_t(py + y) * row_pitch + size_t(px + x) * 4, 4);
              r32f_seen = f;
              texel_ok = texel_ok && std::fabs(f - c.expect_r32f) <= 1e-6f;
            }
            if (!texel_ok && bad++ == 0) {
              first_bad[0] = v[0];
              first_bad[1] = v[1];
              first_x = x;
              first_y = y;
            }
          }
        }
        readback[0]->Unmap(0, nullptr);
        if (readback[1]) readback[1]->Unmap(0, nullptr);
        image_count_ -= destinations_[p.dest_base].images.size();
        destinations_.erase(p.dest_base);
        if (changed_) changed_(p.dest_base);
      }
      const bool pass = img && bad == 0;
      all_ok = all_ok && pass;
      if (pass)
        REXLOG_INFO("PGR4 native resolve selftest: ok   {} {} ({:08X} {:08X})", mode_name, c.name,
                    c.expect[0], c.expect[1]);
      else
        REXLOG_ERROR(
            "PGR4 native resolve selftest: FAIL {} {}: {} of {} texels wrong, first at ({}, {}) = "
            "{:08X} {:08X}, expected {:08X} {:08X}, r32f {} (expected {}){}",
            mode_name, c.name, bad, w * h, first_x, first_y, first_bad[0], first_bad[1],
            c.expect[0], c.expect[1], r32f_seen, c.expect_r32f, img ? "" : ", resolve failed");
    }
  }
  return all_ok;
}

}  // namespace pgr4::native
