// pgr4-native PGR4 native renderer (M2 owner #6): guest draws -> host D3D12 draws. See draw.h.
//
// Ports (BSD-3, Xenia / ReXGlue): d3d12::PipelineCache::GetCurrentStateDescription and
// CreateD3D12Pipeline (pipeline state), D3D12CommandProcessor::IssueDraw,
// UpdateFixedFunctionState, UpdateSystemConstantValues and UpdateBindings (host render target
// path, bindless), draw_util::GetHostViewportInfo / GetScissor / GetNormalizedDepthControl /
// GetNormalizedColorMask / IsRasterizationPotentiallyDone / GetPreferredFacePolygonOffset,
// D3D12TextureCache::GetSamplerParameters / WriteSampler, and the DMA index path of
// PrimitiveProcessor::Process. Copyright Ben Vanik and Xenia developers; adapted by Tom Clay
// for ReXGlue; adapted here for the native renderer.
#include "draw.h"

#include "geometry_shader.h"
#include "pipeline_store.h"
#include "profile.h"
#include "render_targets.h"
#include "renderer.h"
#include "shader_cache.h"
#include "shader_specialize.h"
#include "shader_translate.h"
#include "textures.h"
#include "upload_pool.h"

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <xxhash.h>
#include <emmintrin.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_BOOL(pgr4_native_async_pipelines, true, "PGR4 native",
                    "Compile new pipeline states on worker threads and skip the draws that need "
                    "them until they are ready, instead of stalling the game thread");
REXCVAR_DEFINE_INT32(pgr4_native_pipeline_threads, 0, "PGR4 native",
                     "Pipeline compile threads with --pgr4_native_async_pipelines (0: a quarter "
                     "of the hardware threads, 1 to 4)");
REXCVAR_DEFINE_INT32(pgr4_native_pipeline_burst, 8, "PGR4 native",
                     "With --pgr4_native_async_pipelines, a frame that needs this many new "
                     "pipelines (or follows one that did) waits for them, up to 0.5 s, instead of "
                     "drawing the scene without them (0: never wait)");
REXCVAR_DEFINE_BOOL(pgr4_native_pipeline_cache, true, "PGR4 native",
                    "Keep the shaders and pipelines the native renderer creates under "
                    "<cache_root>\\pgr4-native and compile them at start-up in later runs "
                    "(prewarm, plus a D3D12 pipeline library)");
REXCVAR_DEFINE_STRING(pgr4_native_watch_trace, "", "PGR4 native",
                      "Binary log of vertex write-watch events (arms and faults, one 64-bit "
                      "record each, see WatchTrace) for offline analysis of the watch policy");
REXCVAR_DEFINE_BOOL(pgr4_native_dynamic_depth_bias, true, "PGR4 native",
                    "Set the polygon offset with RSSetDepthBias instead of baking it into the "
                    "pipeline state, where the driver supports it (the game animates it, which "
                    "made hundreds of pipelines per shader pair)");
REXCVAR_DEFINE_BOOL(pgr4_native_bool_specialize, true, "PGR4 native",
                    "Translate shaders whose control flow tests bool constants for each draw's "
                    "values, without the branches (shader_specialize.h)");

namespace pgr4::native {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
using rex::graphics::RegisterFile;
using rex::graphics::Shader;
using rex::graphics::draw_util::Scissor;
using rex::graphics::draw_util::ViewportInfo;
namespace texture_util = rex::graphics::texture_util;

namespace {

#define DRAW_LOG_ONCE(flag, ...) \
  do {                           \
    if (!(flag)) {               \
      (flag) = true;             \
      REXLOG_WARN(__VA_ARGS__);  \
    }                            \
  } while (0)

constexpr uint32_t kSharedMemorySize = 512u << 20;  // guest physical address space
// Shared memory is a reserved buffer whose 4 MB chunks get memory when a range in them is first
// requested (the Xenos plugin's shared memory does the same): a committed 512 MB buffer took half
// of a Steam Deck's 1 GB of VRAM, of which a race touches a fraction.
constexpr uint32_t kSharedChunkShift = 22;
constexpr uint32_t kSharedChunkCount = kSharedMemorySize >> kSharedChunkShift;
constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPageCount = kSharedMemorySize >> kPageShift;
// The memory unwatches at most the 4 MB-aligned block around a write fault
// (PhysicalHeap::TriggerCallbacks), so the invalidation callback looks no further.
constexpr uint32_t kUnwatchWindow = 4u << 20;
constexpr uint32_t kViewHeapSize = 65536;
constexpr uint32_t kSamplerHeapSize = 2048;
// View heap: 0 shared memory SRV, 1 shared memory UAV, then the bindless table (descriptor
// indices in b4 count from here): 0 null 2D array, 1 null 3D, 2 null cube, then textures.
constexpr uint32_t kViewTableStart = 2;
constexpr uint32_t kNull2DArray = 0, kNull3D = 1, kNullCube = 2, kFirstTexture = 3;
constexpr D3D12_RESOURCE_STATES kSharedReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_INDEX_BUFFER;

using enum rex::graphics::Register;  // XE_GPU_REG_* (clang-cl, C++20)

// reg::RB_COLOR_INFO / RB_BLENDCONTROL::rt_register_indices live in the plugin DLL (not exported).
constexpr rex::graphics::Register kColorInfoRegs[4] = {XE_GPU_REG_RB_COLOR_INFO,
                                                       XE_GPU_REG_RB_COLOR1_INFO,
                                                       XE_GPU_REG_RB_COLOR2_INFO,
                                                       XE_GPU_REG_RB_COLOR3_INFO};
constexpr rex::graphics::Register kBlendControlRegs[4] = {
    XE_GPU_REG_RB_BLENDCONTROL0, XE_GPU_REG_RB_BLENDCONTROL1, XE_GPU_REG_RB_BLENDCONTROL2,
    XE_GPU_REG_RB_BLENDCONTROL3};

// ---- draw_util ports (host render targets, no resolution scale) ----

bool IsRasterizationPotentiallyDone(const RegisterFile& regs, bool primitive_polygonal) {
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) {
    return false;
  }
  if (regs.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode ==
          xenos::VertexShaderExportMode::kMultipass ||
      !regs.Get<reg::RB_SURFACE_INFO>().surface_pitch) {
    return false;
  }
  if (primitive_polygonal) {
    auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (sc.cull_front && sc.cull_back) return false;
  }
  return true;
}

reg::RB_DEPTHCONTROL GetNormalizedDepthControl(const RegisterFile& regs) {
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) {
    reg::RB_DEPTHCONTROL disabled;
    disabled.value = 0;
    return disabled;
  }
  reg::RB_DEPTHCONTROL depthcontrol = regs.Get<reg::RB_DEPTHCONTROL>();
  if (depthcontrol.z_enable && !depthcontrol.z_write_enable &&
      depthcontrol.zfunc == xenos::CompareFunction::kAlways) {
    depthcontrol.z_enable = 0;
  }
  return depthcontrol;
}

void GetPreferredFacePolygonOffset(const RegisterFile& regs, bool primitive_polygonal,
                                   float& scale_out, float& offset_out) {
  float scale = 0.0f, offset = 0.0f;
  auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal) {
    if (sc.poly_offset_front_enable && !sc.cull_front) {
      scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
      offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
    if (sc.poly_offset_back_enable && !sc.cull_back && !scale && !offset) {
      scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
      offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
    }
  } else if (sc.poly_offset_para_enable) {
    scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
    offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
  }
  scale_out = scale;
  offset_out = offset;
}

uint32_t GetNormalizedColorMask(const RegisterFile& regs, uint32_t ps_writes_color_targets) {
  if (regs.Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kColorDepth) return 0;
  uint32_t normalized = 0;
  uint32_t rb_color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(ps_writes_color_targets & (1u << i))) continue;
    uint32_t format_mask =
        (1u << xenos::GetColorRenderTargetFormatComponentCount(
             regs.Get<reg::RB_COLOR_INFO>(kColorInfoRegs[i]).color_format)) -
        1;
    uint32_t rt_mask = (rb_color_mask >> (4 * i)) & format_mask;
    if (!rt_mask) continue;
    rt_mask |= 0b1111 & ~format_mask;
    normalized |= rt_mask << (4 * i);
  }
  return normalized;
}

// GetHostViewportInfo with origin_bottom_left = true, x/y max = D3D12_VIEWPORT_BOUNDS_MAX, no
// reverse Z, no float24 conversion, resolution scale 1 and half_pixel_offset on (the plugin's
// settings), and full_float24_in_0_to_1 = false: the host depth buffers here are not shared
// with EDRAM ownership transfers, so depth stays in 0...1 (the Clear values RenderTargets uses,
// and the shader-written depth: the system flags never carry kSysFlag_DepthFloat24).
void HostViewportInfo(const RegisterFile& regs, bool pixel_shader_writes_depth,
                         ViewportInfo& out) {
  auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto vtx = regs.Get<reg::PA_SU_VTX_CNTL>();
  float scale_xy[] = {
      vte.vport_x_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f,
      vte.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f,
  };
  float scale_z = vte.vport_z_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
  float offset_base_xy[] = {
      vte.vport_x_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f,
      vte.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f,
  };
  float offset_z = vte.vport_z_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
  float offset_add_xy[2] = {};
  if (sc.vtx_window_offset_enable) {
    auto wo = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    offset_add_xy[0] += float(wo.window_x_offset);
    offset_add_xy[1] += float(wo.window_y_offset);
  }
  if (vtx.pix_center == xenos::PixelCenter::kD3DZero) {
    offset_add_xy[0] += 0.5f;
    offset_add_xy[1] += 0.5f;
  }
  const uint32_t xy_max[] = {D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX};
  float z_min, z_max, ndc_scale[3], ndc_offset[3];
  if (clip.clip_disable) {
    for (uint32_t i = 0; i < 2; ++i) {
      out.xy_offset[i] = 0;
      uint32_t extent = std::min(xenos::kTexture2DCubeMaxWidthHeight, xy_max[i]);
      out.xy_extent[i] = extent;
      float extent_f = float(extent);
      float px_to_ndc = 2.0f / extent_f;
      ndc_scale[i] = scale_xy[i] * px_to_ndc;
      ndc_offset[i] = (offset_base_xy[i] - extent_f * 0.5f + offset_add_xy[i]) * px_to_ndc;
    }
    z_min = 0.0f;
    z_max = 1.0f;
    ndc_scale[2] = scale_z;
    ndc_offset[2] = offset_z;
  } else {
    for (uint32_t i = 0; i < 2; ++i) {
      float offset_axis = offset_base_xy[i] + offset_add_xy[i];
      float scale_axis = scale_xy[i];
      float scale_abs = std::abs(scale_axis);
      float max_f = float(xy_max[i]);
      uint32_t a0 = uint32_t(std::clamp(offset_axis - scale_abs, 0.0f, max_f));
      uint32_t a1 = uint32_t(std::clamp(offset_axis + scale_abs, 0.0f, max_f));
      uint32_t extent = a1 - a0;
      out.xy_offset[i] = a0;
      out.xy_extent[i] = extent;
      if (extent) {
        float extent_f = float(extent);
        ndc_scale[i] = scale_axis * 2.0f / extent_f;
        ndc_offset[i] = (offset_axis - (float(a0) + extent_f * 0.5f)) * 2.0f / extent_f;
      } else {
        ndc_scale[i] = 1.0f;
        ndc_offset[i] = 0.0f;
      }
    }
    float host_offset_z, host_scale_z;
    if (clip.dx_clip_space_def) {
      host_offset_z = offset_z;
      host_scale_z = scale_z;
      ndc_scale[2] = 1.0f;
      ndc_offset[2] = 0.0f;
    } else {
      host_offset_z = offset_z - scale_z;
      host_scale_z = scale_z * 2.0f;
      ndc_scale[2] = 0.5f;
      ndc_offset[2] = 0.5f;
    }
    if (pixel_shader_writes_depth) {
      z_min = 0.0f;
      z_max = 1.0f;
    } else {
      z_min = std::clamp(host_offset_z, 0.0f, 1.0f);
      z_max = std::clamp(host_offset_z + host_scale_z, 0.0f, 1.0f);
      if (z_min > z_max) {
        std::swap(z_min, z_max);
        ndc_scale[2] = -ndc_scale[2];
        ndc_offset[2] = 1.0f - ndc_offset[2];
      }
    }
  }
  out.z_min = z_min;
  out.z_max = z_max;
  ndc_scale[1] = -ndc_scale[1];  // origin_bottom_left
  ndc_offset[1] = -ndc_offset[1];
  for (uint32_t i = 0; i < 3; ++i) {
    out.ndc_scale[i] = ndc_scale[i];
    out.ndc_offset[i] = ndc_offset[i];
  }
}

void HostScissor(const RegisterFile& regs, Scissor& out) {
  auto tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  auto br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t tl_x = int32_t(tl.tl_x), tl_y = int32_t(tl.tl_y);
  int32_t br_x = int32_t(br.br_x), br_y = int32_t(br.br_y);
  if (!tl.window_offset_disable) {
    auto wo = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    tl_x += wo.window_x_offset;
    tl_y += wo.window_y_offset;
    br_x += wo.window_x_offset;
    br_y += wo.window_y_offset;
  }
  auto stl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
  auto sbr = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  tl_x = std::max(tl_x, int32_t(stl.tl_x));
  tl_y = std::max(tl_y, int32_t(stl.tl_y));
  br_x = std::min(br_x, int32_t(sbr.br_x));
  br_y = std::min(br_y, int32_t(sbr.br_y));
  uint32_t pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
  tl_x = std::min(tl_x, int32_t(pitch));
  br_x = std::min(br_x, int32_t(pitch));
  tl_x = std::max(tl_x, 0);
  tl_y = std::max(tl_y, 0);
  br_x = std::max(br_x, tl_x);
  br_y = std::max(br_y, tl_y);
  out.offset[0] = uint32_t(tl_x);
  out.offset[1] = uint32_t(tl_y);
  out.extent[0] = uint32_t(br_x - tl_x);
  out.extent[1] = uint32_t(br_y - tl_y);
}

// ---- pipeline description ----

struct PipelineKey {
  const void* vs;  // translation objects (stable for the cache's lifetime)
  const void* ps;
  uint32_t gs_key;
  uint8_t topology_type;  // D3D12_PRIMITIVE_TOPOLOGY_TYPE
  uint8_t strip_cut;      // 0 none, 1 0xFFFF, 2 0xFFFFFFFF
  uint8_t cull;           // D3D12_CULL_MODE
  uint8_t wireframe;
  uint8_t front_ccw;
  uint8_t depth_clip;
  uint8_t samples;
  uint8_t disable_raster;
  int32_t depth_bias;
  float slope_scaled_depth_bias;
  uint8_t depth_enable, depth_write, depth_func, stencil_enable;
  uint8_t stencil_read_mask, stencil_write_mask;
  uint8_t front[4], back[4];  // fail, depth fail, pass, func (D3D12 values)
  uint8_t num_rts, pad[3];
  DXGI_FORMAT dsv;
  DXGI_FORMAT rtv[4];
  struct Rt {
    uint8_t blend, src, dst, op, src_a, dst_a, op_a, mask;
  } rt[4];
};
// Bump when PipelineKey's layout or the meaning of a field changes: keys are stored on disk
// (PipelineStore) and an old file is then discarded.
// 2: depth bias zero with dynamic depth bias; 3: pixel shaders specialized for unsigned textures
constexpr uint32_t kPipelineKeyVersion = 3;

enum class BlendFactor : uint8_t {
  kZero, kOne, kSrcColor, kInvSrcColor, kSrcAlpha, kInvSrcAlpha, kDestColor, kInvDestColor,
  kDestAlpha, kInvDestAlpha, kBlendFactor, kInvBlendFactor, kSrcAlphaSat,
};
constexpr BlendFactor kBlendFactorMap[32] = {
    BlendFactor::kZero,        BlendFactor::kOne,          BlendFactor::kZero,
    BlendFactor::kZero,        BlendFactor::kSrcColor,     BlendFactor::kInvSrcColor,
    BlendFactor::kSrcAlpha,    BlendFactor::kInvSrcAlpha,  BlendFactor::kDestColor,
    BlendFactor::kInvDestColor, BlendFactor::kDestAlpha,   BlendFactor::kInvDestAlpha,
    BlendFactor::kBlendFactor, BlendFactor::kInvBlendFactor, BlendFactor::kBlendFactor,
    BlendFactor::kInvBlendFactor, BlendFactor::kSrcAlphaSat,
};
constexpr BlendFactor kBlendFactorAlphaMap[32] = {
    BlendFactor::kZero,       BlendFactor::kOne,          BlendFactor::kZero,
    BlendFactor::kZero,       BlendFactor::kSrcAlpha,     BlendFactor::kInvSrcAlpha,
    BlendFactor::kSrcAlpha,   BlendFactor::kInvSrcAlpha,  BlendFactor::kDestAlpha,
    BlendFactor::kInvDestAlpha, BlendFactor::kDestAlpha,  BlendFactor::kInvDestAlpha,
    BlendFactor::kBlendFactor, BlendFactor::kInvBlendFactor, BlendFactor::kBlendFactor,
    BlendFactor::kInvBlendFactor, BlendFactor::kSrcAlphaSat,
};
constexpr D3D12_BLEND kD3DBlend[] = {
    D3D12_BLEND_ZERO,         D3D12_BLEND_ONE,           D3D12_BLEND_SRC_COLOR,
    D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_SRC_ALPHA,    D3D12_BLEND_INV_SRC_ALPHA,
    D3D12_BLEND_DEST_COLOR,   D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_DEST_ALPHA,
    D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR,
    D3D12_BLEND_SRC_ALPHA_SAT,
};
constexpr D3D12_BLEND_OP kD3DBlendOp[8] = {
    D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT, D3D12_BLEND_OP_MIN, D3D12_BLEND_OP_MAX,
    D3D12_BLEND_OP_REV_SUBTRACT, D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_ADD};

// ---- samplers ----

struct SamplerKey {
  uint8_t clamp_x, clamp_y, clamp_z, border;
  uint8_t aniso, mag_linear, min_linear, mip_linear;
  uint8_t mip_base_map, mip_min_level, pad[2];
  bool operator==(const SamplerKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
static_assert(sizeof(SamplerKey) == 12);
uint64_t SamplerKeyValue(const SamplerKey& k) {
  uint64_t v = 0;
  std::memcpy(&v, &k, 8);
  uint32_t hi = 0;
  std::memcpy(&hi, reinterpret_cast<const uint8_t*>(&k) + 8, 4);
  return v ^ (uint64_t(hi) * 0x9E3779B97F4A7C15ull);
}

xenos::ClampMode NormalizeClampMode(xenos::ClampMode m) {
  if (m == xenos::ClampMode::kClampToHalfway) return xenos::ClampMode::kClampToEdge;
  if (m == xenos::ClampMode::kMirrorClampToHalfway || m == xenos::ClampMode::kMirrorClampToBorder)
    return xenos::ClampMode::kMirrorClampToEdge;
  return m;
}

SamplerKey GetSamplerKey(const RegisterFile& regs, const DxbcShader::SamplerBinding& binding) {
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
  SamplerKey k{};
  xenos::ClampMode cx, cy, cz;
  texture_util::GetClampModesForDimension(fetch, cx, cy, cz);
  cx = NormalizeClampMode(cx);
  cy = NormalizeClampMode(cy);
  cz = NormalizeClampMode(cz);
  k.clamp_x = uint8_t(cx);
  k.clamp_y = uint8_t(cy);
  k.clamp_z = uint8_t(cz);
  k.border = (xenos::ClampModeUsesBorder(cx) || xenos::ClampModeUsesBorder(cy) ||
              xenos::ClampModeUsesBorder(cz))
                 ? uint8_t(fetch.border_color)
                 : uint8_t(xenos::BorderColor::k_ABGR_Black);
  uint32_t mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr,
                                                 nullptr, &mip_min_level, &mip_max_level);
  k.mip_min_level = uint8_t(mip_min_level);
  auto mag = binding.mag_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mag_filter
                                                                        : binding.mag_filter;
  auto min = binding.min_filter == xenos::TextureFilter::kUseFetchConst ? fetch.min_filter
                                                                        : binding.min_filter;
  auto mip = binding.mip_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mip_filter
                                                                        : binding.mip_filter;
  auto aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst ? fetch.aniso_filter
                                                                          : binding.aniso_filter;
  aniso = std::min(aniso, xenos::AnisoFilter::kMax_16_1);
  k.aniso = uint8_t(aniso);
  if (aniso != xenos::AnisoFilter::kDisabled) {
    k.mag_linear = k.min_linear = k.mip_linear = 1;
  } else {
    k.mag_linear = mag == xenos::TextureFilter::kLinear;
    k.min_linear = min == xenos::TextureFilter::kLinear;
    k.mip_linear = mip == xenos::TextureFilter::kLinear;
  }
  k.mip_base_map = mip == xenos::TextureFilter::kBaseMap;
  return k;
}

D3D12_SAMPLER_DESC SamplerDesc(const SamplerKey& k) {
  D3D12_SAMPLER_DESC d = {};
  if (xenos::AnisoFilter(k.aniso) != xenos::AnisoFilter::kDisabled) {
    d.Filter = D3D12_FILTER_ANISOTROPIC;
    d.MaxAnisotropy = 1u << (uint32_t(k.aniso) - 1);
  } else {
    d.Filter = D3D12_ENCODE_BASIC_FILTER(
        k.min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        k.mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        k.mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    d.MaxAnisotropy = 1;
  }
  static const D3D12_TEXTURE_ADDRESS_MODE kAddress[] = {
      D3D12_TEXTURE_ADDRESS_MODE_WRAP,        D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,       D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,       D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      D3D12_TEXTURE_ADDRESS_MODE_BORDER,      D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
  };
  d.AddressU = kAddress[k.clamp_x & 7];
  d.AddressV = kAddress[k.clamp_y & 7];
  d.AddressW = kAddress[k.clamp_z & 7];
  d.MipLODBias = 0.0f;
  d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  switch (xenos::BorderColor(k.border)) {
    case xenos::BorderColor::k_ABGR_White:
      d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = 1.0f;
      break;
    case xenos::BorderColor::k_ACBYCR_Black:
      d.BorderColor[0] = 0.5f;
      d.BorderColor[2] = 0.5f;
      break;
    case xenos::BorderColor::k_ACBCRY_Black:
      d.BorderColor[1] = 0.5f;
      d.BorderColor[2] = 0.5f;
      break;
    default:
      break;
  }
  d.MinLOD = float(k.mip_min_level);
  if (k.mip_base_map) {
    d.MaxLOD = d.MinLOD;
    if (xenos::AnisoFilter(k.aniso) == xenos::AnisoFilter::kDisabled) d.MaxLOD += 0.25f;
  } else {
    d.MaxLOD = FLT_MAX;
  }
  return d;
}

// Whether `size` bytes at `a` and `b` are equal: 16 bytes at a time, four per round, no
// ordering (cheaper than memcmp).
inline bool Equal(const void* a, const void* b, uint32_t size) {
  auto* x = static_cast<const uint8_t*>(a);
  auto* y = static_cast<const uint8_t*>(b);
  auto load = [](const uint8_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); };
  auto zero = [](__m128i v) {
    return _mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_setzero_si128())) == 0xFFFF;
  };
  uint32_t i = 0;
  for (; i + 64 <= size; i += 64) {
    const __m128i d = _mm_or_si128(
        _mm_or_si128(_mm_xor_si128(load(x + i), load(y + i)),
                     _mm_xor_si128(load(x + i + 16), load(y + i + 16))),
        _mm_or_si128(_mm_xor_si128(load(x + i + 32), load(y + i + 32)),
                     _mm_xor_si128(load(x + i + 48), load(y + i + 48))));
    if (!zero(d)) return false;
  }
  for (; i + 16 <= size; i += 16)
    if (!zero(_mm_xor_si128(load(x + i), load(y + i)))) return false;
  return i == size || std::memcmp(x + i, y + i, size - i) == 0;
}

// A constant buffer that is re-uploaded only when its bytes change within a frame. The
// upload memory is CPU-cached (UploadPool cpu_cached), so new data is compared with the last
// upload itself (`cpu`) and copied once. `last` is the staging area of the packed float
// constants (the largest CBV is 256 float4 = 4 KB). gpu == 0: nothing current uploaded.
struct CachedCbv {
  static constexpr uint32_t kMaxSize = 256 * 16;
  alignas(16) uint8_t last[kMaxSize];
  uint32_t size = 0;
  const uint8_t* cpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
  D3D12_GPU_VIRTUAL_ADDRESS bound = 0;
  void Reset() {
    size = 0;
    cpu = nullptr;
    gpu = 0;
    bound = 0;
  }
};

// Float constants packed by a shader's constant_register_map (UpdateBindings) straight into
// `cb.last`, compared with what was there on the way (one register = 16 bytes = one SSE
// load/store, no calls); 16 zero bytes for no shader or no constants (a CBV needs some data).
void PackFloats(CachedCbv& cb, const DxbcShader* shader, const uint32_t* regs, uint32_t reg_base) {
  const uint64_t* bitmap = shader ? shader->constant_register_map().float_bitmap : nullptr;
  uint32_t n = 0;
  if (bitmap)
    for (uint32_t i = 0; i < 4; ++i) n += uint32_t(std::popcount(bitmap[i]));
  if (!n) {
    static constexpr uint8_t kZero[16] = {};
    if (cb.size != 16 || !Equal(cb.last, kZero, 16)) {
      std::memset(cb.last, 0, 16);
      cb.size = 16;
      cb.gpu = 0;
    }
    return;
  }
  const uint32_t size = n * 16;
  const bool resized = cb.size != size;
  cb.size = size;
  __m128i* out = reinterpret_cast<__m128i*>(cb.last);
  __m128i diff = _mm_setzero_si128();
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t bits = bitmap[i];
    const uint32_t* base = regs + reg_base + (i << 8);
    while (bits) {
      const uint32_t b = uint32_t(std::countr_zero(bits));
      bits &= bits - 1;
      const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(base + (b << 2)));
      diff = _mm_or_si128(diff, _mm_xor_si128(v, _mm_load_si128(out)));
      _mm_store_si128(out++, v);
    }
  }
  if (resized || _mm_movemask_epi8(_mm_cmpeq_epi8(diff, _mm_setzero_si128())) != 0xFFFF)
    cb.gpu = 0;
}

}  // namespace

struct DrawRenderer::Impl {
  NativeRenderer& r;
  DrawRenderer::Stats& stats;
  ID3D12Device* device = nullptr;
  rex::memory::Memory* memory = nullptr;
  const uint8_t* membase = nullptr;
  ShaderCache shaders;
  BoolSpecializer vs_bools{shaders}, ps_bools{shaders};
  bool bool_specialize = REXCVAR_GET(pgr4_native_bool_specialize);
  ComPtr<ID3D12RootSignature> root_signature;

  // Shared memory: guest physical memory mirror and its page validity (cleared by CPU writes).
  ComPtr<ID3D12Resource> shared;
  D3D12_RESOURCE_STATES shared_state = D3D12_RESOURCE_STATE_COPY_DEST;
  D3D12_GPU_VIRTUAL_ADDRESS shared_gpu = 0;
  // Reserved shared buffer: which chunks have memory (set under shared_map_mutex, read without
  // it), and the tile heaps behind them. False: a committed buffer.
  bool shared_tiled = false;
  std::atomic<uint64_t> shared_mapped[kSharedChunkCount / 64] = {};
  std::mutex shared_map_mutex;
  std::vector<ComPtr<ID3D12Heap>> shared_heaps;
  uint32_t shared_mapped_chunks = 0;
  std::unique_ptr<std::atomic<uint64_t>[]> valid;
  void* invalidation_handle = nullptr;
  using PendingCopy = VertexCopy;
  // Without the render thread: the copies of the draw being recorded. With it, Prepare
  // (guest thread) writes into the batch's vector instead: this one is the render thread's.
  std::vector<PendingCopy> copies;
  // Fence (or Prepare's tag) RequestRange tags vertex upload memory with.
  uint64_t upload_fence = 0;
  // Render thread mode: vertex_upload belongs to the guest thread (Prepare/EndFramePrepare).
  bool threaded = false;
  uint64_t prep_upload_bytes = 0, prep_uploads = 0;
  // Copy segments (DrawRenderer::Prepare): what the current segment's draws read. Watched
  // ranges are copied in whole pages, so their reads are kept one bit per page, valid in the
  // 64-page words whose epoch is the segment's. Unwatched ranges (inline vertices, packed
  // back to back in the guest's command buffer and often several to a page) are copied to the
  // byte, so their reads are kept as byte intervals.
  struct Read {
    uint32_t begin, end;
    bool exact;
  };
  // This many exact intervals end the segment: the overlap test scans them all.
  static constexpr size_t kSegmentExactLimit = 64;
  uint32_t segment_epoch = 1;
  std::unique_ptr<uint32_t[]> segment_word_epoch;
  std::unique_ptr<uint64_t[]> segment_pages;
  std::vector<std::pair<uint32_t, uint32_t>> segment_exact;
  // The draw being prepared: its ranges, and whether one of its copies overwrites memory the
  // segment reads.
  std::vector<Read> draw_reads;
  bool preparing = false, segment_conflict = false;
  static uint64_t PageMask(uint32_t w, uint32_t first, uint32_t last) {
    uint64_t m = ~0ull;
    if (w == first >> 6) m &= ~0ull << (first & 63);
    if (w == last >> 6) m &= ~0ull >> (63 - (last & 63));
    return m;
  }
  void NewSegment() {
    ++segment_epoch;
    segment_exact.clear();
  }
  bool SegmentReads(uint32_t begin, uint32_t end) const {
    for (const auto& [b, e] : segment_exact)
      if (b < end && begin < e) return true;
    const uint32_t first = begin >> kPageShift, last = (end - 1) >> kPageShift;
    for (uint32_t w = first >> 6; w <= last >> 6; ++w)
      if (segment_word_epoch[w] == segment_epoch && (segment_pages[w] & PageMask(w, first, last)))
        return true;
    return false;
  }
  void SegmentAdd(const Read& read) {
    if (read.exact) {
      // Successive inline blocks are separated by the draw packets and constant writes around
      // them: the merged interval covers those too, which no draw copies.
      constexpr uint32_t kMergeGap = 16 << 10;
      if (!segment_exact.empty() && segment_exact.back().second + kMergeGap >= read.begin &&
          read.end + kMergeGap >= segment_exact.back().first) {
        auto& last = segment_exact.back();
        last = {std::min(last.first, read.begin), std::max(last.second, read.end)};
      } else {
        segment_exact.push_back({read.begin, read.end});
      }
      return;
    }
    const uint32_t first = read.begin >> kPageShift, last = (read.end - 1) >> kPageShift;
    for (uint32_t w = first >> 6; w <= last >> 6; ++w) {
      if (segment_word_epoch[w] != segment_epoch) {
        segment_word_epoch[w] = segment_epoch;
        segment_pages[w] = 0;
      }
      segment_pages[w] |= PageMask(w, first, last);
    }
  }

  UploadPool vertex_upload{16u << 20};
  // CPU-cached: CachedCbv compares new constants with its last upload in place.
  UploadPool constant_upload{4u << 20, /*cpu_cached=*/true};

  // Descriptors.
  ComPtr<ID3D12DescriptorHeap> view_heap, sampler_heap;
  uint32_t view_stride = 0, sampler_stride = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE view_cpu{}, sampler_cpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE view_gpu{}, sampler_gpu{};
  uint32_t views_allocated = kFirstTexture;
  uint32_t samplers_allocated = 0;
  std::unordered_map<uint64_t, uint32_t> srv_indices;      // XXH3(resource, srv desc)
  std::unordered_map<uint64_t, uint32_t> sampler_indices;  // SamplerKeyValue
  // Direct-mapped memo in front of srv_indices: (resource, component mapping, signed, fetch
  // dimension) -> descriptor index; a hit skips TextureDescriptor's desc build, XXH3 and map
  // lookup. Every other field of the SRV desc (format, view dimension, mips, array size) is fixed
  // per resource, and textures are never destroyed, so the key determines the index.
  struct SrvMemo {
    ID3D12Resource* resource = nullptr;
    uint32_t mapping = 0;
    uint8_t is_signed = 0, dimension = 0xFF;
    uint32_t index = 0;
  };
  static constexpr uint32_t kSrvMemoBits = 12;
  std::unique_ptr<SrvMemo[]> srv_memo{new SrvMemo[size_t(1) << kSrvMemoBits]};
  // Direct-mapped memo in front of GetSamplerKey + sampler_indices: (6 fetch dwords, binding
  // mag/min/mip/aniso) -> sampler index; the sampler key is a function of exactly those.
  struct SamplerMemo {
    uint32_t fetch[6] = {};
    uint8_t filters[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t index = 0;
  };
  static constexpr uint32_t kSamplerMemoBits = 10;
  std::unique_ptr<SamplerMemo[]> sampler_memo{new SamplerMemo[size_t(1) << kSamplerMemoBits]};
  // Fetch constants a shader reads (texture and sampler bindings), its descriptor table size and
  // its last BindMemo entry (checked before hashing), direct-mapped by shader.
  struct BindMemo;
  struct ShaderSlots {
    const DxbcShader* shader = nullptr;
    uint32_t mask = 0;   // fetch constants read
    uint32_t count = 0;  // bits in mask
    uint32_t n = 0;      // descriptor table entries (max bindless index + 1, at most 64)
    BindMemo* last = nullptr;
  };
  ShaderSlots shader_slots[512];
  // Whole-shader binding memo, 2-way set-associative by (shader, the fetch constants it reads):
  // the descriptor table the shader's textures and samplers resolved to and the texture signs it
  // set. Valid while every texture it bound is unchanged (TextureCache::Unchanged: no guest
  // write, no resolve into it); `epoch` is the TextureCache::change_epoch at which that was last
  // known, so while the epoch stays the same the per-texture checks are skipped. A hit skips
  // TextureCache::Get and the descriptor memos for the whole shader. Keying on the fetch
  // constants keeps one entry per texture set a shader is drawn with (one entry per shader missed
  // ~900 times per frame on the race). Shaders with a missing texture are not memoized.
  struct BindMemo {
    static constexpr uint32_t kMaxSlots = 16, kMaxBound = 32, kMaxIndices = 64;
    // Fields a hit reads, packed so a hit reads one contiguous run (entry[] only when the epoch
    // moved).
    const DxbcShader* shader = nullptr;
    uint64_t epoch = 0;
    uint64_t used = 0;  // bind_clock at the last fill or set lookup (the set's replacement order)
    uint32_t bound = 0;  // textures bound, in order: their cache entries and signs[] writes
    uint32_t textures = 0;
    // The shader's fetch constants in ShaderSlots::mask order (count x 6 dwords), then its
    // descriptor table (n), then one signs[] write per bound texture (slot | value << 8).
    uint32_t data[kMaxSlots * 6 + kMaxIndices + kMaxBound];
    const void* entry[kMaxBound];
  };
  static constexpr uint32_t kBindMemoSetBits = 11;
  std::unique_ptr<BindMemo[]> bind_memo{new BindMemo[size_t(2) << kBindMemoSetBits]};
  uint64_t bind_clock = 0;

  // Pipelines by PipelineKey hash. With --pgr4_native_async_pipelines new ones compile on
  // pso_threads and the draws that need them are skipped until they are ready
  // (stats.pso_pending), so a pipeline first needed mid-race does not stall the game thread.
  // A scene's first frame needs many at once (180 at race start), and skipping their draws
  // would build the scene up part by part over several frames: from pipeline_burst new
  // pipelines in a frame (or the one before), draws wait for theirs instead (a loading step,
  // at most kPipelineWaitBudget per frame). Entries are never removed.
  // With --pgr4_native_pipeline_cache every new pipeline is recorded in `store`; the next run
  // translates and creates the recorded ones at start-up (Prewarm, a slice per frame) and the
  // workers load them from `library` instead of compiling them again.
  enum : uint8_t { kCompiling, kReady, kFailed };
  struct Pipeline {
    ComPtr<ID3D12PipelineState> pso;
    std::atomic<uint8_t> state{kCompiling};
    bool prewarm = false;  // its job was queued in prewarm_jobs (draw thread only)
  };
  std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> pipelines;
  struct PipelineJob {
    Pipeline* target;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;  // bytecode and root signature outlive the job
  };
  std::vector<std::thread> pso_threads;
  std::mutex pso_mutex;
  std::condition_variable pso_cv;
  std::condition_variable pso_ready_cv;  // a job finished (notified under pso_mutex)
  std::deque<PipelineJob> pso_jobs;      // a draw is waiting for these
  std::deque<PipelineJob> prewarm_jobs;  // taken only when pso_jobs is empty
  bool pso_stopping = false;
  static constexpr std::chrono::milliseconds kPipelineWaitBudget{500};
  uint32_t pipeline_burst = 0;
  uint32_t frame_new_pipelines = 0, last_frame_new_pipelines = 0;  // created or promoted
  uint32_t frame_pending_draws = 0;  // skipped this frame, their pipeline compiling
  std::chrono::steady_clock::duration frame_pipeline_wait{};
  PipelineStore store;
  std::once_flag library_open;
  PipelineLibrary library;  // pipeline workers only
  // D3D12 OPTIONS16 dynamic depth bias: pipelines are created with
  // D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS and keyed with a zero bias, and draws set it
  // with RSSetDepthBias.
  bool dynamic_depth_bias = false;
  ComPtr<ID3D12GraphicsCommandList9> list9;
  ID3D12GraphicsCommandList* list9_of = nullptr;
  std::filesystem::path library_path;
  size_t prewarm_next = 0;  // into store.loaded_pipelines()
  uint32_t prewarm_failed = 0, prewarm_frames = 0;
  double prewarm_ms = 0;
  std::unordered_map<uint32_t, std::vector<uint32_t>> geometry_shaders;

  // Shader lookup memo, direct-mapped by ucode address and size (plus the immediate load
  // sequence): a hit skips ShaderCache::Load's XXH3 over the whole ucode. An entry also keeps
  // the first and last dwords of the ucode it was made from, so a different shader loaded at
  // the same address with the same size misses.
  struct ShaderMemo {
    static constexpr uint32_t kEntries = 64, kSample = 16;
    uint32_t ucode = 0, dwords = 0, immediate_seq = 0;
    uint32_t sample[kSample] = {};
    DxbcShader* shader = nullptr;
  };
  ShaderMemo vs_memo[ShaderMemo::kEntries], ps_memo[ShaderMemo::kEntries];
  ShaderMemo* vs_last = nullptr;  // the entry the previous draw used
  ShaderMemo* ps_last = nullptr;
  // SelectModifications is a function of the two shaders and these registers: the last call.
  struct ModsMemo {
    const DxbcShader* vs = nullptr;
    const DxbcShader* ps = nullptr;
    ShaderDrawRegs sdr;
    ShaderModifications mods;
    uint32_t ps_textures = 0;  // fetch constants the pixel shader reads (bit per constant)
  } mods_memo;
  const uint64_t signed_is_unsigned_formats = SignedViewIsUnsignedFormats();
  // Translation per shader, direct-mapped by shader: the last modification it was translated
  // with (valid translations only; they live as long as the shader cache).
  struct TranslationMemo {
    const DxbcShader* shader = nullptr;
    uint64_t modification = 0;
    DxbcShader::Translation* translation = nullptr;
  };
  TranslationMemo translation_memo[256];
  // Ready pipelines by PipelineKey hash, direct-mapped, in front of GetPipeline's map (ready
  // pipelines are never replaced or freed).
  struct PsoMemo {
    uint64_t hash = 0;
    ID3D12PipelineState* pso = nullptr;
  };
  PsoMemo pso_memo[1024];

  // Draw path counters, logged every 300 frames with --pgr4_native_profile.
  struct Diag {
    uint64_t frames = 0, draws = 0, shader_loads = 0, mods_hits = 0, translation_hits = 0,
             pso_hits = 0, ranges = 0, range_runs = 0, range_pages = 0, unwatched = 0,
             flushes = 0;
    uint64_t cbv_uploads[16] = {}, cbv_bytes[16] = {};  // by root parameter
    // Textures bound, by the fetch constant state the shaders read at run time: swizzled signs
    // all unsigned, gamma RGB + unsigned A, all gamma, all signed, other; then exponent bias
    // set, LOD bias set (any signs).
    uint64_t fetch_classes[7] = {};
  } diag;

  // List state.
  uint64_t list_frame = UINT64_MAX;
  bool list_dirty = true;
  ID3D12PipelineState* bound_pso = nullptr;
  D3D_PRIMITIVE_TOPOLOGY bound_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  D3D12_VIEWPORT bound_viewport{};
  D3D12_RECT bound_scissor{};
  float bound_blend[4] = {};
  uint32_t bound_stencil_ref = UINT32_MAX;
  float bound_depth_bias = NAN, bound_slope_bias = NAN;
  D3D12_CPU_DESCRIPTOR_HANDLE bound_rtv[4] = {};
  D3D12_CPU_DESCRIPTOR_HANDLE bound_dsv{};
  uint32_t bound_rt_count = UINT32_MAX;
  D3D12_INDEX_BUFFER_VIEW bound_ibv{};
  CachedCbv cb_system, cb_float_vs, cb_float_ps, cb_bool_loop, cb_fetch, cb_desc_vs, cb_desc_ps;

  // Logged once.
  // Oracle toggles from <capture dir>\draw-debug.txt
  // ("<mask> [max_draws] [skip_from] [skip_to] [shader hash]..."), read every 15 frames:
  // 1 depth/stencil off, 2 culling off, 4 blending off, 8 alpha test off, 16 log the next frame's
  // draws, 32 upload every draw, 64 polygon offset off, 128 issue only the draws the hashes
  // select, 256 snapshots (below). max_draws > 0 issues only the first N draws of a frame;
  // draws whose vertex or pixel shader ucode hash is listed are skipped (selected with 128).
  // Hashes stay stable across frames where draw indices do not.
  uint32_t debug_mask = 0, debug_max_draws = 0, frame_draws = 0, debug_poll = 0;
  uint32_t debug_skip_from = 0, debug_skip_to = 0;
  std::array<uint64_t, 8> debug_hashes{};
  uint32_t debug_hash_count = 0;
  bool draw_log_pending = false;
  std::FILE* draw_log = nullptr;
  // 256: snapshots instead of skips. After the first draw of the next frame that uses each
  // listed hash (or, with none listed, after draw skip_from), the draw's bound single-sampled
  // targets are read back to <capture dir>\snap-f<frame>-d<draw>-{depth,c<i>}.bin, each with a
  // .txt holding "width height row_bytes dxgi_format".
  struct Snap {
    std::string stem;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    uint32_t rows = 0;
    uint64_t row_bytes = 0;
  };
  std::vector<Snap> snaps;
  uint64_t snap_fence = 0;
  uint32_t snap_pending = 0;  // bit i: listed hash i (bit 31: draw skip_from) still to snapshot
  void SnapshotTargets(ID3D12GraphicsCommandList* list, const BoundTargets& targets, uint32_t draw_index);
  void Snapshot(ID3D12GraphicsCommandList* list, RenderTarget* rt, const std::string& stem);
  void WriteSnapshots(uint64_t completed_fence);
  bool log_memexport = false, log_tess = false, log_reset = false, log_prim = false,
       log_vfetch = false, log_pso = false, log_rt = false, log_upload = false,
       log_shader = false, log_heap = false, log_index = false;

  Impl(NativeRenderer& renderer, DrawRenderer::Stats& s) : r(renderer), stats(s) {}
  ~Impl() {
    if (!pso_threads.empty()) {
      {
        std::lock_guard<std::mutex> lock(pso_mutex);
        pso_stopping = true;
      }
      pso_cv.notify_all();
      for (std::thread& t : pso_threads) t.join();
      library.Save();
    }
    if (invalidation_handle && memory)
      memory->UnregisterPhysicalMemoryInvalidationCallback(invalidation_handle);
    if (trace.file) {
      trace.Flush();
      std::fclose(trace.file);
    }
  }

  bool Initialize();
  void Draw(const uint8_t* base, const DrawState& s, const DrawPrep* prep);
  DxbcShader* LoadShader(ShaderMemo* table, ShaderMemo*& last, const ShaderRef& ref,
                         xenos::ShaderType type);
  // Copies go to the end of `out`.
  bool RequestVertices(const DrawState& s, const DxbcShader& vs, std::vector<PendingCopy>& out);
  void Prepare(const DrawState& s, uint64_t tag, DrawPrep* prep, std::vector<VertexCopy>* out);
  // A triangle fan as a host triangle list (v1, v2, v0), (v2, v3, v0)... like the plugin's
  // primitive processor; guest indices are copied raw (the VS still swaps them).
  void WriteFanIndices(const DrawState& s, uint32_t count, bool wide, uint8_t* out) const;
  void ResetListState();
  bool RequestRange(uint32_t start, uint32_t length, bool watch, std::vector<PendingCopy>& out);
  // Gives the shared buffer memory under [start, start + length) if it is reserved. Any thread;
  // the tile mappings are queued ahead of the copies the caller records afterwards.
  bool MapShared(uint32_t start, uint32_t length);
  void FlushCopies(ID3D12GraphicsCommandList* list);
  void TransitionShared(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES to);
  // Uploads cb.size bytes of `data` unless cb.gpu is current, and binds it at `root_index`.
  bool CommitCbv(CachedCbv& cb, const void* data, uint32_t root_index,
                 ID3D12GraphicsCommandList* list);
  bool UploadCbv(CachedCbv& cb, const void* data, uint32_t size, uint32_t root_index,
                 ID3D12GraphicsCommandList* list) {
    if (cb.gpu && (cb.size != size || !Equal(cb.cpu, data, size))) cb.gpu = 0;
    cb.size = size;
    return CommitCbv(cb, data, root_index, list);
  }
  uint32_t TextureDescriptor(const TextureBinding& b, const DxbcShader::TextureBinding& tb);
  uint32_t SamplerDescriptor(const SamplerKey& k);
  // The pipeline for `key` (`hash`: its XXH3), added and recorded in the store if new.
  Pipeline* GetPipeline(uint64_t hash, const PipelineKey& key, const DxbcShader::Translation& vs,
                        const DxbcShader::Translation* ps);
  // Moves a pipeline's prewarm job, if no worker has taken it yet, to the draw queue: a draw
  // waits for it now.
  void PromotePipeline(Pipeline* p);
  // In a burst of new pipelines, waits until `p` is no longer compiling (within the frame's
  // budget). Returns whether it waited.
  bool WaitForPipeline(const Pipeline& p);
  // Adds the pipeline under `hash` and creates it (queued on pso_threads when they run; a
  // prewarm job waits behind every draw's job).
  Pipeline* CreatePipeline(uint64_t hash, const PipelineKey& key, const DxbcShader::Translation& vs,
                           const DxbcShader::Translation* ps, bool prewarm);
  // `saves`: this worker saves the library when the workers have been idle for a while.
  void PipelineWorker(bool saves);
  // Per frame: translates and queues stored pipelines from earlier runs for up to ~2 ms.
  void Prewarm();

  // A watched page may be unwatched once its valid bit is clear: the next request copies it
  // again and re-arms the watch. Unless the range must be exact, the pages around it that hold
  // no valid copy are offered for unwatching too (InvalidRunAround). Valid neighbours stay
  // watched: unwatching them too (one fault per 256 KB block instead of per 4 KB page) tripled
  // the bytes re-copied per frame.
  static std::pair<uint32_t, uint32_t> InvalidationThunk(void* context, uint32_t start,
                                                         uint32_t length, bool exact) {
    auto* self = static_cast<Impl*>(context);
    self->watch_faults.fetch_add(1, std::memory_order_relaxed);
    if (start >= kSharedMemorySize || !length) return {start, length};
    const uint32_t end = std::min(kSharedMemorySize, start + length);
    const uint32_t first = start >> kPageShift, last = (end - 1) >> kPageShift;
    const uint32_t frame = self->watch_frame.load(std::memory_order_relaxed);
    for (uint32_t p = first; p <= last; ++p) {
      const uint64_t bit = 1ull << (p & 63);
      std::atomic<uint64_t>& valid_word = self->valid[p >> 6];
      const bool was_valid = valid_word.fetch_and(~bit, std::memory_order_relaxed) & bit;
      if (self->trace.file)
        self->trace.Add(was_valid ? WatchTrace::kFaultValid : WatchTrace::kFaultOther, p, 1, 0);
      if (!was_valid) continue;
      std::atomic<uint8_t>& quick = self->quick_rewrites[p];
      uint8_t n = quick.load(std::memory_order_relaxed);
      n = frame - self->armed_frame[p].load(std::memory_order_relaxed) > kHotRewriteFrames
              ? 0
              : std::min<uint8_t>(n + 1, 2);
      quick.store(n, std::memory_order_relaxed);
      if (n == 2) self->hot[p >> 6].fetch_or(bit, std::memory_order_relaxed);
      // The page after one written since the previous frame: a buffer being filled in order.
      // Its valid pages ahead, to the end of the 256 KB block (the widest range the Xenos
      // plugin's callbacks let the memory unwatch), are invalidated with it, so the fill faults
      // about once per block instead of once per page. In the race this cut the faults on valid
      // pages 4x, and under 1 % of the pages invalidated ahead went unwritten.
      const uint32_t previous = p ? self->write_frame[p - 1].load(std::memory_order_relaxed) : 0;
      self->write_frame[p].store(frame + 1, std::memory_order_relaxed);
      const uint32_t shift = (p & 63) + 1;
      if (!previous || frame + 1 - previous > 1 || shift == 64) continue;
      const uint32_t ahead =
          uint32_t(std::countr_one(valid_word.load(std::memory_order_relaxed) >> shift));
      if (!ahead) continue;
      valid_word.fetch_and(~(((1ull << ahead) - 1) << shift), std::memory_order_relaxed);
      // Counted as written, so a fault on the next block's first page continues the sequence.
      for (uint32_t q = p + 1; q <= p + ahead; ++q)
        self->write_frame[q].store(frame + 1, std::memory_order_relaxed);
      if (self->trace.file) self->trace.Add(WatchTrace::kAhead, p + 1, ahead, 0);
    }
    return exact ? std::pair{start, length} : self->InvalidRunAround(first, last);
  }
  // Pages [first, last] widened to the nearest valid page on each side within the
  // kUnwatchWindow block, as a byte range. Pages without a valid copy lose nothing by being
  // unwatched (the next request copies and re-arms them). The memory unwatches only the
  // intersection of every callback's range, so with exact ranges a texture or buffer being
  // filled faulted once per 4 KB page although no consumer still watched its other pages (the
  // race's write faults came in runs of about 10 adjacent pages). A page made valid meanwhile is
  // re-armed after the unwatch: both hold the global lock.
  std::pair<uint32_t, uint32_t> InvalidRunAround(uint32_t first, uint32_t last) const {
    constexpr uint32_t kWindowPages = kUnwatchWindow >> kPageShift;
    const uint32_t window_first = first & ~(kWindowPages - 1);
    const uint32_t window_last = last | (kWindowPages - 1);
    uint32_t w = first >> 6;
    uint64_t bits = valid[w].load(std::memory_order_relaxed) & ((1ull << (first & 63)) - 1);
    while (!bits && w > window_first >> 6) bits = valid[--w].load(std::memory_order_relaxed);
    const uint32_t lo = bits ? (w << 6) + 64 - uint32_t(std::countl_zero(bits)) : window_first;
    w = last >> 6;
    bits = valid[w].load(std::memory_order_relaxed) & ~((2ull << (last & 63)) - 1);
    while (!bits && w < window_last >> 6) bits = valid[++w].load(std::memory_order_relaxed);
    const uint32_t end = bits ? (w << 6) + uint32_t(std::countr_zero(bits)) : window_last + 1;
    return {lo << kPageShift, (end - lo) << kPageShift};
  }
  // Dynamic vertex data. A valid page written again within kHotRewriteFrames of being armed,
  // twice in a row (once is more often a buffer being reused), becomes hot: data the game
  // rewrites every few frames, where each write fault and re-arm (a protection change, and a
  // TLB shootdown across the process's threads: tens of µs each under Wine) costs far more than
  // hashing the page. Hot pages are not watched: each request hashes them (about 0.1 µs) and
  // copies those whose content changed since the last copy. A hot page hashed kHotMaxHashes
  // times without a change (a large buffer that many draws read whole) is watched again, which
  // then costs less. Frames are counted by watch_frame on the thread that calls RequestRange.
  static constexpr uint32_t kHotRewriteFrames = 16;
  static constexpr uint16_t kHotMaxHashes = 64;
  std::unique_ptr<std::atomic<uint64_t>[]> hot;          // one bit per page
  std::unique_ptr<std::atomic<uint32_t>[]> armed_frame;  // watch_frame of the last arm
  std::unique_ptr<std::atomic<uint8_t>[]> quick_rewrites;  // quick rewrites in a row (to 2)
  std::unique_ptr<uint64_t[]> hot_hash;     // XXH3 | 1 of the content last copied, 0: none
  std::unique_ptr<uint16_t[]> hot_hashes;   // hashes since the content last changed
  std::unique_ptr<std::atomic<uint32_t>[]> write_frame;  // watch_frame + 1 of the last write, 0: none
  std::atomic<uint32_t> watch_frame{0};
  // Hot pages hashed and copied (content changed) since the last EndFrame.
  std::atomic<uint64_t> hash_pages{0}, hash_copies{0};
  // Write-watch faults (InvalidationThunk calls) since the last EndFrame; any guest thread.
  std::atomic<uint64_t> watch_faults{0};
  // RequestRange's EnablePhysicalMemoryAccessCallbacks calls and their pages since EndFrame.
  std::atomic<uint64_t> watch_arms{0}, watch_arm_pages{0};

  // --pgr4_native_watch_trace. Record: type (2 bits) << 62 | frame (22) << 40 | page (17) << 23
  // | pages (9, saturated) << 14 | request (14): arms of one RequestRange call share the request
  // number. Faults come from any guest thread, hence the mutex.
  struct WatchTrace {
    enum Type : uint64_t { kArm, kFaultValid, kFaultOther, kAhead };
    std::mutex mutex;
    std::vector<uint64_t> records;
    std::FILE* file = nullptr;
    uint32_t frame = 0;
    std::atomic<uint32_t> request{0};
    void Add(Type type, uint32_t page, uint32_t pages, uint32_t req) {
      std::lock_guard<std::mutex> lock(mutex);
      records.push_back(uint64_t(type) << 62 | uint64_t(frame & 0x3FFFFF) << 40 |
                        uint64_t(page & 0x1FFFF) << 23 | uint64_t(std::min(pages, 511u)) << 14 |
                        (req & 0x3FFF));
    }
    void Flush() {
      std::lock_guard<std::mutex> lock(mutex);
      if (file && !records.empty()) std::fwrite(records.data(), 8, records.size(), file);
      records.clear();
    }
  } trace;
};

bool DrawRenderer::Impl::Initialize() {
  device = r.d3d12();
  memory = rex::system::kernel_memory();
  if (!memory) {
    REXLOG_ERROR("PGR4 native draw: no guest memory");
    return false;
  }
  membase = memory->physical_membase();
  vertex_upload.SetDevice(device);
  threaded = REXCVAR_GET(pgr4_native_render_thread);
  constant_upload.SetDevice(device);
  // Race start keeps 3 vertex and 10 constant blocks in flight; creating one there costs up to 16 ms.
  vertex_upload.Reserve(4);
  constant_upload.Reserve(12);
  root_signature = CreateBindlessRootSignature(device);
  if (!root_signature) return false;

  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = kSharedMemorySize;
  rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  // Tier 2: reads of chunks without memory return zero, as in a committed buffer never written.
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  shared_tiled = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                       sizeof(options))) &&
                 options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2 &&
                 SUCCEEDED(device->CreateReservedResource(&rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                          nullptr, IID_PPV_ARGS(&shared)));
  if (!shared_tiled) {
    if (Failed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&shared)),
               "shared memory buffer (512 MB)"))
      return false;
  }
  shared_state = D3D12_RESOURCE_STATE_COPY_DEST;
  shared_gpu = shared->GetGPUVirtualAddress();
  valid.reset(new std::atomic<uint64_t>[kPageCount / 64]);
  for (uint32_t i = 0; i < kPageCount / 64; ++i) valid[i].store(0, std::memory_order_relaxed);
  hot.reset(new std::atomic<uint64_t>[kPageCount / 64]);
  for (uint32_t i = 0; i < kPageCount / 64; ++i) hot[i].store(0, std::memory_order_relaxed);
  armed_frame.reset(new std::atomic<uint32_t>[kPageCount]);
  for (uint32_t i = 0; i < kPageCount; ++i) armed_frame[i].store(0, std::memory_order_relaxed);
  write_frame.reset(new std::atomic<uint32_t>[kPageCount]);
  for (uint32_t i = 0; i < kPageCount; ++i) write_frame[i].store(0, std::memory_order_relaxed);
  quick_rewrites.reset(new std::atomic<uint8_t>[kPageCount]);
  for (uint32_t i = 0; i < kPageCount; ++i) quick_rewrites[i].store(0, std::memory_order_relaxed);
  hot_hash.reset(new uint64_t[kPageCount]());
  hot_hashes.reset(new uint16_t[kPageCount]());
  segment_word_epoch.reset(new uint32_t[kPageCount / 64]());
  segment_pages.reset(new uint64_t[kPageCount / 64]());
  if (const std::string& path = REXCVAR_GET(pgr4_native_watch_trace); !path.empty())
    trace.file = std::fopen(path.c_str(), "wb");
  invalidation_handle = memory->RegisterPhysicalMemoryInvalidationCallback(InvalidationThunk, this);

  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewHeapSize,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&view_heap)), "draw view heap"))
    return false;
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&sampler_heap)), "draw sampler heap"))
    return false;
  view_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  sampler_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  view_cpu = view_heap->GetCPUDescriptorHandleForHeapStart();
  view_gpu = view_heap->GetGPUDescriptorHandleForHeapStart();
  sampler_cpu = sampler_heap->GetCPUDescriptorHandleForHeapStart();
  sampler_gpu = sampler_heap->GetGPUDescriptorHandleForHeapStart();

  auto view_at = [&](uint32_t i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = view_cpu;
    h.ptr += size_t(i) * view_stride;
    return h;
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = DXGI_FORMAT_R32_TYPELESS;
  sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Buffer.NumElements = kSharedMemorySize / 4;
  sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  device->CreateShaderResourceView(shared.Get(), &sd, view_at(0));
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = DXGI_FORMAT_R32_TYPELESS;
  ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = kSharedMemorySize / 4;
  ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device->CreateUnorderedAccessView(shared.Get(), nullptr, &ud, view_at(1));
  // Null textures for unbound or mismatching bindings.
  D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
  nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  nd.Texture2DArray.MipLevels = 1;
  nd.Texture2DArray.ArraySize = 1;
  device->CreateShaderResourceView(nullptr, &nd, view_at(kViewTableStart + kNull2DArray));
  nd = {};
  nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  nd.Texture3D.MipLevels = 1;
  device->CreateShaderResourceView(nullptr, &nd, view_at(kViewTableStart + kNull3D));
  nd = {};
  nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  nd.TextureCube.MipLevels = 1;
  device->CreateShaderResourceView(nullptr, &nd, view_at(kViewTableStart + kNullCube));
  views_allocated = kViewTableStart + kFirstTexture;
  REXLOG_INFO("PGR4 native draw: up (shared memory 512 MB {}, bindless heaps {} views / {} samplers)",
              shared_tiled ? "reserved" : "committed", kViewHeapSize, kSamplerHeapSize);
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16 = {};
  dynamic_depth_bias = REXCVAR_GET(pgr4_native_dynamic_depth_bias) &&
                       SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16,
                                                             &options16, sizeof(options16))) &&
                       options16.DynamicDepthBiasSupported;
  REXLOG_INFO("PGR4 native draw: dynamic depth bias {}", dynamic_depth_bias ? "on" : "off");
  if (REXCVAR_GET(pgr4_native_pipeline_cache)) {
    // The runtime's cache root (userdata\cache unless --cache_root): co_run keeps it.
    rex::Runtime* runtime = rex::system::kernel_state() ? rex::system::kernel_state()->emulator()
                                                       : nullptr;
    std::filesystem::path dir = runtime ? runtime->cache_root() : std::filesystem::path();
    if (dir.empty()) {
      REXLOG_WARN("PGR4 native pipelines: no cache root, no pipeline cache");
    } else {
      dir /= "pgr4-native";
      store.Open(dir, sizeof(PipelineKey), kPipelineKeyVersion);
      library_path = dir / "pipelines.d3d12lib";
    }
  }
  if (REXCVAR_GET(pgr4_native_async_pipelines)) {
    pipeline_burst = uint32_t(std::max(REXCVAR_GET(pgr4_native_pipeline_burst), 0));
    int threads = REXCVAR_GET(pgr4_native_pipeline_threads);
    if (threads <= 0) threads = int(std::thread::hardware_concurrency() / 4);
    threads = std::clamp(threads, 1, 4);
    for (int i = 0; i < threads; ++i) {
      pso_threads.emplace_back([this, i] { PipelineWorker(i == 0); });
      wchar_t name[32];
      std::swprintf(name, 32, L"PGR4 native pipelines %d", i);
      SetThreadDescription(pso_threads.back().native_handle(), name);
    }
  }
  Prewarm();  // the first slice before the first frame, which needs some of them
  return true;
}

void DrawRenderer::Impl::PipelineWorker(bool saves) {
  std::call_once(library_open, [this] { library.Open(device, library_path); });
  std::unique_lock<std::mutex> lock(pso_mutex);
  auto has_work = [this] {
    return pso_stopping || !pso_jobs.empty() || !prewarm_jobs.empty();
  };
  for (;;) {
    if (saves && library.dirty()) {
      // Save once no new pipeline came for 3 s (the burst is over).
      if (!pso_cv.wait_for(lock, std::chrono::seconds(3), has_work)) {
        lock.unlock();
        library.Save();
        lock.lock();
        continue;
      }
    } else {
      pso_cv.wait(lock, has_work);
    }
    if (pso_stopping) return;  // pending pipelines never get used
    std::deque<PipelineJob>& queue = !pso_jobs.empty() ? pso_jobs : prewarm_jobs;
    const PipelineJob job = queue.front();
    queue.pop_front();
    lock.unlock();
    ComPtr<ID3D12PipelineState> pso;
    bool loaded;
    const HRESULT hr = library.Create(job.desc, &pso, &loaded);
    if (SUCCEEDED(hr)) {
      job.target->pso = std::move(pso);
      job.target->state.store(kReady, std::memory_order_release);
    } else {
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true))
        REXLOG_WARN("PGR4 native draw: CreateGraphicsPipelineState failed {:08X}", uint32_t(hr));
      job.target->state.store(kFailed, std::memory_order_release);
    }
    lock.lock();
    pso_ready_cv.notify_all();
  }
}

void DrawRenderer::Impl::ResetListState() {
  list_dirty = true;
  bound_pso = nullptr;
  bound_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  bound_viewport = {};
  bound_scissor = {};
  std::memset(bound_blend, 0xFF, sizeof(bound_blend));  // NaN: never equal
  bound_stencil_ref = UINT32_MAX;
  bound_depth_bias = bound_slope_bias = NAN;
  for (auto& h : bound_rtv) h.ptr = 0;
  bound_dsv.ptr = 0;
  bound_rt_count = UINT32_MAX;
  bound_ibv = {};
  for (CachedCbv* cb : {&cb_system, &cb_float_vs, &cb_float_ps, &cb_bool_loop, &cb_fetch,
                        &cb_desc_vs, &cb_desc_ps})
    cb->bound = 0;
}

void DrawRenderer::Impl::TransitionShared(ID3D12GraphicsCommandList* list,
                                          D3D12_RESOURCE_STATES to) {
  if (shared_state == to) return;
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = shared.Get();
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = shared_state;
  b.Transition.StateAfter = to;
  list->ResourceBarrier(1, &b);
  shared_state = to;
}

bool DrawRenderer::Impl::RequestRange(uint32_t start, uint32_t length, bool watch,
                                      std::vector<PendingCopy>& out) {
  if (!length) return true;
  if (start >= kSharedMemorySize || kSharedMemorySize - start < length) {
    DRAW_LOG_ONCE(log_upload, "PGR4 native draw: range {:08X}+{:X} outside physical memory",
                  start, length);
    return false;
  }
  const bool exact = !watch || (debug_mask & 32);  // 32: upload every draw
  if (preparing) draw_reads.push_back({start, start + length, exact});
  const uint64_t fence = upload_fence;
  auto copy = [&](uint32_t from, uint32_t size) {
    if (preparing && !segment_conflict) segment_conflict = SegmentReads(from, from + size);
    if (!MapShared(from, size)) return false;
    UploadPool::Alloc a;
    if (!vertex_upload.Allocate(size, 16, fence, &a)) return false;
    std::memcpy(a.cpu, membase + from, size);
    out.push_back({a.resource, a.offset, from, size});
    if (threaded) {
      prep_upload_bytes += size;
      ++prep_uploads;
    } else {
      stats.upload_bytes += size;
      ++stats.uploads;
    }
    return true;
  };
  if (exact) {
    ++diag.unwatched;
    return copy(start, length);
  }
  ++diag.ranges;
  const uint32_t first = start >> kPageShift, last = (start + length - 1) >> kPageShift;
  diag.range_pages += last - first + 1;
  const uint32_t trace_request =
      trace.file ? trace.request.fetch_add(1, std::memory_order_relaxed) : 0;
  const uint32_t frame = watch_frame.load(std::memory_order_relaxed);
  // Hot pages whose content changed, copied in runs of adjacent pages.
  uint32_t changed_first = 0, changed_end = 0;
  auto copy_changed = [&] {
    return changed_first == changed_end ||
           copy(changed_first << kPageShift, (changed_end - changed_first) << kPageShift);
  };
  uint64_t hashed = 0, changed = 0;
  uint32_t p = first;
  while (p <= last) {
    // Skip valid pages that are not hot a 64-page word at a time (whole vertex buffers are
    // requested per draw).
    const uint32_t w = p >> 6;
    const uint64_t hot_bits = hot[w].load(std::memory_order_relaxed);
    uint64_t work = (~valid[w].load(std::memory_order_relaxed) | hot_bits) & (~0ull << (p & 63));
    if (w == last >> 6) work &= ~0ull >> (63 - (last & 63));
    if (!work) {
      p = (w + 1) << 6;
      continue;
    }
    p = (w << 6) + uint32_t(std::countr_zero(work));
    const uint64_t bit = 1ull << (p & 63);
    if (hot_bits & bit) {
      ++hashed;
      const uint64_t hash = XXH3_64bits(membase + (size_t(p) << kPageShift), 1u << kPageShift) | 1;
      if (hash != hot_hash[p]) {
        hot_hash[p] = hash;
        hot_hashes[p] = 0;
        ++changed;
        if (changed_end != p) {
          if (!copy_changed()) return false;
          changed_first = p;
        }
        changed_end = p + 1;
        ++p;
        continue;
      }
      if (++hot_hashes[p] < kHotMaxHashes) {
        ++p;
        continue;
      }
      // Read far more often than written: watched again, starting a run of invalid pages.
      hot[w].fetch_and(~bit, std::memory_order_relaxed);
      hot_hash[p] = 0;
      quick_rewrites[p].store(0, std::memory_order_relaxed);
    }
    uint32_t run_end = p + 1;
    while (run_end <= last) {
      const uint64_t b = 1ull << (run_end & 63);
      if ((valid[run_end >> 6].load(std::memory_order_relaxed) |
           hot[run_end >> 6].load(std::memory_order_relaxed)) & b)
        break;
      ++run_end;
    }
    // Valid and watched before reading, so a write during the copy invalidates again.
    for (uint32_t q = p; q < run_end;) {
      const uint32_t word_end = std::min(run_end, (q | 63) + 1);
      const uint32_t n = word_end - q;
      const uint64_t mask = (n == 64 ? ~0ull : ((1ull << n) - 1)) << (q & 63);
      valid[q >> 6].fetch_or(mask, std::memory_order_relaxed);
      q = word_end;
    }
    for (uint32_t q = p; q < run_end; ++q) armed_frame[q].store(frame, std::memory_order_relaxed);
    const uint32_t from = p << kPageShift, size = (run_end - p) << kPageShift;
    {
      profile::Scope scope(profile::kCapWatch);
      memory->EnablePhysicalMemoryAccessCallbacks(from, size, true, false);
    }
    watch_arms.fetch_add(1, std::memory_order_relaxed);
    watch_arm_pages.fetch_add(run_end - p, std::memory_order_relaxed);
    if (trace.file) trace.Add(WatchTrace::kArm, p, run_end - p, trace_request);
    ++diag.range_runs;
    if (!copy(from, size)) return false;
    p = run_end;
  }
  if (hashed) {
    hash_pages.fetch_add(hashed, std::memory_order_relaxed);
    hash_copies.fetch_add(changed, std::memory_order_relaxed);
  }
  return copy_changed();
}

bool DrawRenderer::Impl::MapShared(uint32_t start, uint32_t length) {
  if (!shared_tiled) return true;
  const uint32_t first = start >> kSharedChunkShift;
  const uint32_t last = (start + length - 1) >> kSharedChunkShift;
  auto mapped = [&](uint32_t c) {
    return (shared_mapped[c >> 6].load(std::memory_order_acquire) >> (c & 63)) & 1;
  };
  bool all = true;
  for (uint32_t c = first; c <= last && all; ++c) all = mapped(c);
  if (all) return true;

  std::lock_guard<std::mutex> lock(shared_map_mutex);
  for (uint32_t c = first; c <= last; ++c) {
    if (mapped(c)) continue;
    uint32_t end = c + 1;
    while (end <= last && !mapped(end)) ++end;
    const uint32_t chunks = end - c;
    D3D12_HEAP_DESC hd = {};
    hd.SizeInBytes = uint64_t(chunks) << kSharedChunkShift;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> heap;
    if (Failed(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)), "shared memory tile heap"))
      return false;
    D3D12_TILED_RESOURCE_COORDINATE coord = {};
    coord.X = UINT(uint64_t(c) << kSharedChunkShift) / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    D3D12_TILE_REGION_SIZE size = {};
    size.NumTiles = UINT(hd.SizeInBytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
    const D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
    const UINT heap_offset = 0;
    r.queue()->UpdateTileMappings(shared.Get(), 1, &coord, &size, heap.Get(), 1, &flags,
                                  &heap_offset, &size.NumTiles, D3D12_TILE_MAPPING_FLAG_NONE);
    shared_heaps.push_back(std::move(heap));
    for (uint32_t m = c; m < end; ++m)
      shared_mapped[m >> 6].fetch_or(1ull << (m & 63), std::memory_order_release);
    const uint32_t before = shared_mapped_chunks;
    shared_mapped_chunks += chunks;
    // Logged per 64 MB.
    if ((before >> 4) != (shared_mapped_chunks >> 4))
      REXLOG_INFO("PGR4 native draw: shared memory {} MB mapped",
                  shared_mapped_chunks << (kSharedChunkShift - 20));
    c = end - 1;
  }
  return true;
}

void DrawRenderer::Impl::FlushCopies(ID3D12GraphicsCommandList* list) {
  if (copies.empty()) {
    TransitionShared(list, kSharedReadState);
    return;
  }
  ++diag.flushes;
  ++stats.copy_batches;
  TransitionShared(list, D3D12_RESOURCE_STATE_COPY_DEST);
  for (const PendingCopy& c : copies)
    list->CopyBufferRegion(shared.Get(), c.dst, c.src, c.src_offset, c.size);
  copies.clear();
  TransitionShared(list, kSharedReadState);
}

bool DrawRenderer::Impl::CommitCbv(CachedCbv& cb, const void* data, uint32_t root_index,
                                   ID3D12GraphicsCommandList* list) {
  if (!cb.gpu) {
    UploadPool::Alloc a;
    if (!constant_upload.Allocate(std::max<uint32_t>(cb.size, 16), 256, r.frame_fence(), &a))
      return false;
    std::memcpy(a.cpu, data, cb.size);
    cb.cpu = a.cpu;
    cb.gpu = a.gpu;
    ++diag.cbv_uploads[root_index & 15];
    diag.cbv_bytes[root_index & 15] += cb.size;
  }
  if (cb.bound != cb.gpu) {
    list->SetGraphicsRootConstantBufferView(root_index, cb.gpu);
    cb.bound = cb.gpu;
  }
  return true;
}

uint32_t DrawRenderer::Impl::TextureDescriptor(const TextureBinding& b,
                                               const DxbcShader::TextureBinding& tb) {
  ++stats.srv_lookups;
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = b.srv;
  // The translator's space for the binding: 2D array (1D/2D), cube, or 3D-or-stacked (either).
  const D3D12_SRV_DIMENSION dim = desc.ViewDimension;
  switch (tb.dimension) {
    case xenos::FetchOpDimension::kCube:
      if (dim != D3D12_SRV_DIMENSION_TEXTURECUBE) return kNullCube;
      break;
    case xenos::FetchOpDimension::k3DOrStacked:
      if (dim != D3D12_SRV_DIMENSION_TEXTURE3D && dim != D3D12_SRV_DIMENSION_TEXTURE2DARRAY)
        return kNull3D;
      break;
    default:
      if (dim != D3D12_SRV_DIMENSION_TEXTURE2DARRAY) return kNull2DArray;
      break;
  }
  if (tb.is_signed) {
    if (b.srv_signed_format == DXGI_FORMAT_UNKNOWN)
      return dim == D3D12_SRV_DIMENSION_TEXTURECUBE ? kNullCube
             : dim == D3D12_SRV_DIMENSION_TEXTURE3D ? kNull3D
                                                    : kNull2DArray;
    desc.Format = b.srv_signed_format;
  }
  struct {
    ID3D12Resource* res;
    D3D12_SHADER_RESOURCE_VIEW_DESC d;
  } key_data;
  std::memset(&key_data, 0, sizeof(key_data));
  key_data.res = b.resource;
  key_data.d = desc;
  const uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));
  auto it = srv_indices.find(key);
  if (it != srv_indices.end()) return it->second;
  if (views_allocated >= kViewHeapSize) {
    DRAW_LOG_ONCE(log_heap, "PGR4 native draw: view heap full ({} descriptors)", kViewHeapSize);
    return kNull2DArray;
  }
  const uint32_t heap_index = views_allocated++;
  D3D12_CPU_DESCRIPTOR_HANDLE h = view_cpu;
  h.ptr += size_t(heap_index) * view_stride;
  device->CreateShaderResourceView(b.resource, &desc, h);
  const uint32_t index = heap_index - kViewTableStart;
  srv_indices.emplace(key, index);
  ++stats.srv_created;
  return index;
}

uint32_t DrawRenderer::Impl::SamplerDescriptor(const SamplerKey& k) {
  const uint64_t key = SamplerKeyValue(k);
  auto it = sampler_indices.find(key);
  if (it != sampler_indices.end()) return it->second;
  if (samplers_allocated >= kSamplerHeapSize) {
    DRAW_LOG_ONCE(log_heap, "PGR4 native draw: sampler heap full");
    return 0;
  }
  const uint32_t index = samplers_allocated++;
  D3D12_CPU_DESCRIPTOR_HANDLE h = sampler_cpu;
  h.ptr += size_t(index) * sampler_stride;
  D3D12_SAMPLER_DESC d = SamplerDesc(k);
  device->CreateSampler(&d, h);
  sampler_indices.emplace(key, index);
  ++stats.samplers_created;
  return index;
}

DrawRenderer::Impl::Pipeline* DrawRenderer::Impl::GetPipeline(uint64_t hash,
                                                              const PipelineKey& key,
                                                              const DxbcShader::Translation& vs,
                                                              const DxbcShader::Translation* ps) {
  auto it = pipelines.find(hash);
  Pipeline* p;
  if (it != pipelines.end()) {
    p = it->second.get();
  } else {
    p = CreatePipeline(hash, key, vs, ps, false);
    ++stats.pso_created;
    ++frame_new_pipelines;
    if (store.is_open()) {
      PipelineKey stored = key;
      stored.vs = stored.ps = nullptr;
      const Shader& vss = vs.shader();
      store.RecordShader(uint32_t(vss.type()), vss.ucode_data_hash(), vss.ucode_dwords(),
                         vss.ucode_dword_count());
      if (ps) {
        const Shader& pss = ps->shader();
        store.RecordShader(uint32_t(pss.type()), pss.ucode_data_hash(), pss.ucode_dwords(),
                           pss.ucode_dword_count());
      }
      store.RecordPipeline(vss.ucode_data_hash(), vs.modification(),
                           ps ? ps->shader().ucode_data_hash() : 0, ps ? ps->modification() : 0,
                           &stored, sizeof(stored));
    }
  }
  return p;
}

void DrawRenderer::Impl::PromotePipeline(Pipeline* p) {
  p->prewarm = false;
  ++frame_new_pipelines;
  std::lock_guard<std::mutex> lock(pso_mutex);
  auto it = std::find_if(prewarm_jobs.begin(), prewarm_jobs.end(),
                         [p](const PipelineJob& j) { return j.target == p; });
  if (it == prewarm_jobs.end()) return;  // being compiled
  pso_jobs.push_back(*it);
  prewarm_jobs.erase(it);
}

bool DrawRenderer::Impl::WaitForPipeline(const Pipeline& p) {
  if (!pipeline_burst ||
      std::max(frame_new_pipelines, last_frame_new_pipelines) < pipeline_burst ||
      frame_pipeline_wait >= kPipelineWaitBudget)
    return false;
  const auto t0 = std::chrono::steady_clock::now();
  {
    std::unique_lock<std::mutex> lock(pso_mutex);
    pso_ready_cv.wait_for(lock, kPipelineWaitBudget - frame_pipeline_wait, [&p] {
      return p.state.load(std::memory_order_acquire) != kCompiling;
    });
  }
  const auto waited = std::chrono::steady_clock::now() - t0;
  frame_pipeline_wait += waited;
  stats.pso_wait_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(waited).count());
  return true;
}

DrawRenderer::Impl::Pipeline* DrawRenderer::Impl::CreatePipeline(
    uint64_t hash, const PipelineKey& key, const DxbcShader::Translation& vs,
    const DxbcShader::Translation* ps, bool prewarm) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
  std::memset(&d, 0, sizeof(d));  // padding too: PipelineLibrary hashes the bytes
  d.pRootSignature = root_signature.Get();
  if (dynamic_depth_bias) d.Flags = D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS;
  d.IBStripCutValue = key.strip_cut == 1   ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                      : key.strip_cut == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                           : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  d.VS.pShaderBytecode = vs.translated_binary().data();
  d.VS.BytecodeLength = vs.translated_binary().size();
  if (ps && !key.disable_raster) {
    d.PS.pShaderBytecode = ps->translated_binary().data();
    d.PS.BytecodeLength = ps->translated_binary().size();
  }
  if (key.gs_key) {
    auto g = geometry_shaders.find(key.gs_key);
    if (g == geometry_shaders.end()) {
      GeometryShaderKey gk;
      gk.key = key.gs_key;
      g = geometry_shaders.emplace(key.gs_key, CreateGeometryShader(gk)).first;
    }
    d.GS.pShaderBytecode = g->second.data();
    d.GS.BytecodeLength = g->second.size() * sizeof(uint32_t);
  }
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology_type);
  d.RasterizerState.FillMode = key.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE(key.cull);
  d.RasterizerState.FrontCounterClockwise = key.front_ccw;
  d.RasterizerState.DepthBias = key.depth_bias;
  d.RasterizerState.SlopeScaledDepthBias = key.slope_scaled_depth_bias;
  d.RasterizerState.DepthClipEnable = key.depth_clip;
  d.SampleMask = UINT_MAX;
  d.SampleDesc.Count = key.samples;
  if (!key.disable_raster) {
    d.DepthStencilState.DepthEnable = key.depth_enable;
    d.DepthStencilState.DepthWriteMask =
        key.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(key.depth_func);
    if (!key.depth_enable) d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    d.DepthStencilState.StencilEnable = key.stencil_enable;
    d.DepthStencilState.StencilReadMask = key.stencil_read_mask;
    d.DepthStencilState.StencilWriteMask = key.stencil_write_mask;
    auto face = [](const uint8_t f[4]) {
      D3D12_DEPTH_STENCILOP_DESC o;
      o.StencilFailOp = f[0] ? D3D12_STENCIL_OP(f[0]) : D3D12_STENCIL_OP_KEEP;
      o.StencilDepthFailOp = f[1] ? D3D12_STENCIL_OP(f[1]) : D3D12_STENCIL_OP_KEEP;
      o.StencilPassOp = f[2] ? D3D12_STENCIL_OP(f[2]) : D3D12_STENCIL_OP_KEEP;
      o.StencilFunc = f[3] ? D3D12_COMPARISON_FUNC(f[3]) : D3D12_COMPARISON_FUNC_ALWAYS;
      return o;
    };
    d.DepthStencilState.FrontFace = face(key.front);
    d.DepthStencilState.BackFace = face(key.back);
    d.DSVFormat = key.dsv;
  }
  d.BlendState.IndependentBlendEnable = TRUE;
  d.NumRenderTargets = key.num_rts;
  for (uint32_t i = 0; i < 4; ++i) {
    d.RTVFormats[i] = key.rtv[i];
    const PipelineKey::Rt& rt = key.rt[i];
    D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[i];
    b.BlendEnable = rt.blend;
    b.SrcBlend = kD3DBlend[rt.src];
    b.DestBlend = kD3DBlend[rt.dst];
    b.BlendOp = kD3DBlendOp[rt.op & 7];
    b.SrcBlendAlpha = kD3DBlend[rt.src_a];
    b.DestBlendAlpha = kD3DBlend[rt.dst_a];
    b.BlendOpAlpha = kD3DBlendOp[rt.op_a & 7];
    b.LogicOp = D3D12_LOGIC_OP_NOOP;
    b.RenderTargetWriteMask = rt.mask;
  }
  Pipeline* p = pipelines.emplace(hash, std::make_unique<Pipeline>()).first->second.get();
  if (!pso_threads.empty()) {
    p->prewarm = prewarm;
    {
      std::lock_guard<std::mutex> lock(pso_mutex);
      (prewarm ? prewarm_jobs : pso_jobs).push_back({p, d});
    }
    pso_cv.notify_one();
    return p;
  }
  HRESULT hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p->pso));
  if (FAILED(hr)) {
    DRAW_LOG_ONCE(log_pso, "PGR4 native draw: CreateGraphicsPipelineState failed {:08X}",
                  uint32_t(hr));
    p->state.store(kFailed, std::memory_order_relaxed);
    return p;
  }
  p->state.store(kReady, std::memory_order_relaxed);
  return p;
}

void DrawRenderer::Impl::Prewarm() {
  const std::vector<PipelineStore::Pipeline>& list = store.loaded_pipelines();
  if (prewarm_next >= list.size()) return;
  const auto t0 = std::chrono::steady_clock::now();
  auto load = [&](uint64_t hash, xenos::ShaderType type) -> DxbcShader* {
    const PipelineStore::Shader* s = store.loaded_shader(hash);
    if (!s || s->type != uint32_t(type) || s->ucode.empty()) return nullptr;
    return shaders.Load(type, s->ucode.data(), uint32_t(s->ucode.size()));
  };
  do {
    const PipelineStore::Pipeline& sp = list[prewarm_next++];
    DxbcShader* vs = load(sp.vs_hash, xenos::ShaderType::kVertex);
    DxbcShader* ps = sp.ps_hash ? load(sp.ps_hash, xenos::ShaderType::kPixel) : nullptr;
    DxbcShader::Translation* vst = vs ? shaders.Translate(*vs, sp.vs_mod) : nullptr;
    DxbcShader::Translation* pst = ps ? shaders.Translate(*ps, sp.ps_mod) : nullptr;
    if (!vst || (sp.ps_hash && !pst) || sp.key.size() != sizeof(PipelineKey)) {
      ++prewarm_failed;
      continue;
    }
    PipelineKey key;
    std::memcpy(&key, sp.key.data(), sizeof(key));
    key.vs = vst;
    key.ps = pst;
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));
    if (!pipelines.count(hash)) CreatePipeline(hash, key, *vst, pst, true);
  } while (prewarm_next < list.size() &&
           std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2));
  prewarm_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  ++prewarm_frames;
  if (prewarm_next == list.size())
    REXLOG_INFO("PGR4 native pipelines: prewarm queued {} pipelines ({} failed) in {:.1f} ms "
                "over {} frames",
                list.size() - prewarm_failed, prewarm_failed, prewarm_ms, prewarm_frames);
}

void DrawRenderer::Impl::WriteFanIndices(const DrawState& s, uint32_t count, bool wide,
                                         uint8_t* out) const {
  auto guest = [&](uint32_t i) -> uint32_t {
    if (!s.indexed) return i;
    const uint8_t* p = membase + s.index_address;
    if (wide) {
      uint32_t v;
      std::memcpy(&v, p + size_t(i) * 4, 4);
      return v;
    }
    uint16_t v;
    std::memcpy(&v, p + size_t(i) * 2, 2);
    return v;
  };
  for (uint32_t t = 0; t + 2 < count; ++t) {
    const uint32_t tri[3] = {guest(t + 1), guest(t + 2), guest(0)};
    for (uint32_t k = 0; k < 3; ++k) {
      if (wide)
        reinterpret_cast<uint32_t*>(out)[t * 3 + k] = tri[k];
      else
        reinterpret_cast<uint16_t*>(out)[t * 3 + k] = uint16_t(tri[k]);
    }
  }
}

DxbcShader* DrawRenderer::Impl::LoadShader(ShaderMemo* table, ShaderMemo*& last,
                                           const ShaderRef& ref, xenos::ShaderType type) {
  if (!ref.ucode || !ref.dwords) return nullptr;
  // The previous draw's shader again (most draws): no guest memory read, as before the table.
  if (last && last->shader && last->ucode == ref.ucode && last->dwords == ref.dwords &&
      last->immediate_seq == ref.immediate_seq)
    return last->shader;
  // Declaration-patched vertex shaders (IM_LOAD_IMMEDIATE) come from the tracker's copy.
  const uint32_t* ucode =
      ref.immediate ? ref.immediate : reinterpret_cast<const uint32_t*>(membase + ref.ucode);
  uint32_t sample[ShaderMemo::kSample];
  for (uint32_t i = 0; i < ShaderMemo::kSample / 2; ++i) {
    sample[i] = ucode[std::min(i, ref.dwords - 1)];
    sample[ShaderMemo::kSample / 2 + i] = ucode[ref.dwords - 1 - std::min(i, ref.dwords - 1)];
  }
  ShaderMemo& memo = table[((ref.ucode >> 4) ^ ref.dwords) & (ShaderMemo::kEntries - 1)];
  last = &memo;
  if (memo.shader && memo.ucode == ref.ucode && memo.dwords == ref.dwords &&
      memo.immediate_seq == ref.immediate_seq &&
      std::memcmp(memo.sample, sample, sizeof(sample)) == 0)
    return memo.shader;
  memo.ucode = ref.ucode;
  memo.dwords = ref.dwords;
  memo.immediate_seq = ref.immediate_seq;
  std::memcpy(memo.sample, sample, sizeof(sample));
  ++diag.shader_loads;
  memo.shader = shaders.Load(type, ucode, ref.dwords);
  return memo.shader;
}

// Vertex ranges the VS fetches and the index range (not for fans: their indices are read
// directly) into `out`.
bool DrawRenderer::Impl::RequestVertices(const DrawState& s, const DxbcShader& vs,
                                         std::vector<PendingCopy>& out) {
  const RegisterFile& regs = *reinterpret_cast<const RegisterFile*>(s.regs);
  const Shader::ConstantRegisterMap& vmap = vs.constant_register_map();
  for (uint32_t w = 0; w < 3; ++w) {
    uint32_t bits = vmap.vertex_fetch_bitmap[w];
    while (bits) {
      uint32_t b = uint32_t(std::countr_zero(bits));
      bits &= bits - 1;
      const uint32_t index = w * 32 + b;
      xenos::xe_gpu_vertex_fetch_t f = regs.GetVertexFetch(index);
      if (f.type != xenos::FetchConstantType::kVertex) {
        DRAW_LOG_ONCE(log_vfetch, "PGR4 native draw: vertex fetch {} has type {} ({:08X} {:08X})",
                      index, uint32_t(f.type), f.dword_0, f.dword_1);
        if (f.type != xenos::FetchConstantType::kInvalidVertex) return false;
      }
      const uint32_t start = f.address << 2, size = f.size << 2;
      const bool inline_data = s.inline_vertices && start == s.inline_vertices;
      if (!RequestRange(start, size, !inline_data, out)) return false;
    }
  }
  if (s.indexed && xenos::PrimitiveType(s.primitive) != xenos::PrimitiveType::kTriangleFan)
    return RequestRange(s.index_address, s.count * (s.index_32bit ? 4u : 2u), true, out);
  return true;
}

void DrawRenderer::Impl::Prepare(const DrawState& s, uint64_t tag, DrawPrep* prep,
                                 std::vector<VertexCopy>* out) {
  *prep = DrawPrep{};
  if (!s.vs.ucode || !s.vs.dwords) return;
  prep->vs = LoadShader(vs_memo, vs_last, s.vs, xenos::ShaderType::kVertex);
  prep->ps = LoadShader(ps_memo, ps_last, s.ps, xenos::ShaderType::kPixel);
  profile::Mark(profile::kCapShaders);
  if (!prep->vs) return;
  upload_fence = tag;
  const bool indexed_ok = !s.indexed || s.index_address;
  // Straight into the batch: copies made before a failed range still go out (their pages
  // are marked valid already).
  preparing = true;
  segment_conflict = false;
  draw_reads.clear();
  prep->upload_ok = indexed_ok && RequestVertices(s, *prep->vs, *out);
  preparing = false;
  if (segment_conflict || segment_exact.size() >= kSegmentExactLimit) {
    NewSegment();
    prep->copy_segment = true;
  }
  for (const Read& read : draw_reads) SegmentAdd(read);
  // Triangle fans: host indices from the guest's now (Draw reads them from here).
  const uint32_t count = s.count;
  if (prep->upload_ok && xenos::PrimitiveType(s.primitive) == xenos::PrimitiveType::kTriangleFan &&
      count >= 3) {
    const uint32_t host_count = (count - 2) * 3;
    const bool wide = s.indexed ? s.index_32bit : count > 0xFFFF;
    UploadPool::Alloc a;
    if (vertex_upload.Allocate(uint64_t(host_count) * (wide ? 4 : 2), 16, tag, &a)) {
      WriteFanIndices(s, count, wide, a.cpu);
      prep->fan_indices = a.gpu;
      prep->fan_count = host_count;
      prep->fan_wide = wide;
    }
  }
  profile::Mark(profile::kCapVertices);
}

void DrawRenderer::Impl::Draw(const uint8_t* base, const DrawState& s, const DrawPrep* prep) {
  (void)base;
  ID3D12GraphicsCommandList* list = r.command_list();
  if (list_frame != r.frame_index()) {
    // A new frame's list: nothing is bound, cached constants of older frames may be reused.
    list_frame = r.frame_index();
    ResetListState();
    for (CachedCbv* cb : {&cb_system, &cb_float_vs, &cb_float_ps, &cb_bool_loop, &cb_fetch,
                          &cb_desc_vs, &cb_desc_ps})
      cb->Reset();
  }
  if (prep) {
    // The guest thread copied the ranges (and marked their pages valid): they go into shared
    // memory whether or not the draw is issued.
    if (prep->copy_count) {
      ++stats.copy_batches;
      TransitionShared(list, D3D12_RESOURCE_STATE_COPY_DEST);
      for (uint32_t i = 0; i < prep->copy_count; ++i) {
        const VertexCopy& c = prep->copies[i];
        list->CopyBufferRegion(shared.Get(), c.dst, c.src, c.src_offset, c.size);
      }
    }
    if (!prep->upload_ok) {
      ++stats.failed;
      return;
    }
  }
  const RegisterFile& regs = *reinterpret_cast<const RegisterFile*>(s.regs);
  const xenos::PrimitiveType prim = xenos::PrimitiveType(s.primitive);

  // Shaders.
  if (!s.vs.ucode || !s.vs.dwords) {
    ++stats.skipped;
    return;
  }
  DxbcShader* vs =
      prep ? prep->vs : LoadShader(vs_memo, vs_last, s.vs, xenos::ShaderType::kVertex);
  DxbcShader* ps =
      prep ? prep->ps : LoadShader(ps_memo, ps_last, s.ps, xenos::ShaderType::kPixel);
  if (!vs) {
    ++stats.failed;
    return;
  }
  if (bool_specialize) {
    const uint32_t* bools = &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031];
    vs = vs_bools.Get(vs, bools);
    ps = ps_bools.Get(ps, bools);
  }

  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = 0;
  initiator.prim_type = prim;
  initiator.source_select = s.indexed ? xenos::SourceSelect::kDMA : xenos::SourceSelect::kAutoIndex;
  initiator.index_size = s.index_32bit ? xenos::IndexFormat::kInt32 : xenos::IndexFormat::kInt16;
  initiator.num_indices = s.count & 0xFFFF;

  ShaderDrawRegs sdr;
  sdr.sq_program_cntl = regs[XE_GPU_REG_SQ_PROGRAM_CNTL];
  sdr.sq_context_misc = regs[XE_GPU_REG_SQ_CONTEXT_MISC];
  sdr.sq_interpolator_cntl = regs[XE_GPU_REG_SQ_INTERPOLATOR_CNTL];
  sdr.pa_cl_clip_cntl = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
  {
    // SelectModifications reads only prim_type; the index count would defeat mods_memo.
    reg::VGT_DRAW_INITIATOR prim_only;
    prim_only.value = 0;
    prim_only.prim_type = prim;
    sdr.vgt_draw_initiator = prim_only.value;
  }
  sdr.vgt_output_path_cntl = regs[XE_GPU_REG_VGT_OUTPUT_PATH_CNTL];
  sdr.pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  sdr.rb_modecontrol = regs[XE_GPU_REG_RB_MODECONTROL];
  sdr.rb_surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
  sdr.rb_colorcontrol = regs[XE_GPU_REG_RB_COLORCONTROL];
  sdr.rb_color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  for (uint32_t i = 0; i < 4; ++i)
    sdr.rb_color_info[i] = regs[kColorInfoRegs[i]];
  if (mods_memo.vs == vs && mods_memo.ps == ps &&
      std::memcmp(&mods_memo.sdr, &sdr, sizeof(sdr)) == 0) {
    ++diag.mods_hits;
  } else {
    mods_memo.vs = vs;
    mods_memo.ps = ps;
    mods_memo.sdr = sdr;
    mods_memo.mods = SelectModifications(shaders.translator(), *vs, ps, sdr);
    mods_memo.ps_textures = 0;
    if (mods_memo.mods.pixel_shader_used)
      for (const auto& tb : ps->texture_bindings()) mods_memo.ps_textures |= 1u << tb.fetch_constant;
  }
  ShaderModifications mods = mods_memo.mods;
  mods.pixel = SpecializePixelTextures(mods.pixel, s.fetch(0), mods_memo.ps_textures,
                                       signed_is_unsigned_formats);

  if (!mods.draw) {
    ++stats.skipped;
    return;
  }
  if (!mods.pixel_shader_used) ps = nullptr;
  if (vs->memexport_eM_written() || (ps && ps->memexport_eM_written())) {
    DRAW_LOG_ONCE(log_memexport, "PGR4 native draw: memexport unsupported (draw skipped)");
    ++stats.unsupported;
    return;
  }
  const bool tess =
      xenos::IsMajorModeExplicit(initiator.major_mode, initiator.prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select == xenos::VGTOutputPath::kTessellationEnable;
  if (tess) {
    DRAW_LOG_ONCE(log_tess, "PGR4 native draw: tessellation unsupported");
    ++stats.unsupported;
    return;
  }
  profile::Mark(profile::kShaders);
  const bool polygonal = rex::graphics::draw_util::IsPrimitivePolygonal(false, prim);
  const bool rasterization = IsRasterizationPotentiallyDone(regs, polygonal);

  // Host primitive (the plugin's primitive processor on D3D12: points, rectangles and quads go
  // through a geometry shader; fans and line loops need converted indices).
  D3D_PRIMITIVE_TOPOLOGY topology;
  switch (prim) {
    case xenos::PrimitiveType::kPointList: topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case xenos::PrimitiveType::kLineList: topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case xenos::PrimitiveType::kLineStrip: topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kRectangleList:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      break;
    case xenos::PrimitiveType::kQuadList: topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ; break;
    case xenos::PrimitiveType::kTriangleFan:
      // As a triangle list with host indices (v1, v2, v0), (v2, v3, v0)... like the plugin's
      // primitive processor; guest indices are copied raw (the VS still swaps them).
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    default:
      DRAW_LOG_ONCE(log_prim, "PGR4 native draw: primitive type {} unsupported (fans/loops need "
                              "converted indices)", s.primitive);
      ++stats.unsupported;
      return;
  }
  uint32_t count = s.count;
  if (!count) {
    ++stats.skipped;
    return;
  }

  // Index buffer (kGuestDMA) and primitive reset.
  xenos::Endian index_endian = xenos::Endian::kNone;
  uint8_t strip_cut = 0;
  if (s.indexed) {
    index_endian = xenos::Endian(s.index_endian);
    if (!s.index_32bit && index_endian != xenos::Endian::kNone &&
        index_endian != xenos::Endian::k8in16)
      index_endian = index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16
                                                           : xenos::Endian::kNone;
    if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
        (prim == xenos::PrimitiveType::kLineStrip || prim == xenos::PrimitiveType::kTriangleStrip)) {
      uint32_t reset = xenos::GpuSwap(regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx,
                                      index_endian);
      if (!s.index_32bit) {
        if (reset <= 0xFFFF) {
          if (reset != 0xFFFF) {
            DRAW_LOG_ONCE(log_reset, "PGR4 native draw: primitive reset index {:X} unsupported",
                          reset);
            ++stats.unsupported;
            return;
          }
          strip_cut = 1;
        }
      } else {
        // The host compares all 32 bits; the guest only the low 24. Fine when the guest uses
        // 0xFFFFFFFF (or the index never appears).
        strip_cut = 2;
      }
    }
    if (!s.index_address) {
      ++stats.failed;
      return;
    }
  }

  // Render targets.
  const reg::RB_DEPTHCONTROL depth_control = GetNormalizedDepthControl(regs);
  const uint32_t color_mask = ps ? GetNormalizedColorMask(regs, ps->writes_color_targets()) : 0;
  const bool depth_used = rasterization && (depth_control.z_enable || depth_control.stencil_enable);
  Scissor scissor;
  HostScissor(regs, scissor);
  ViewportInfo vp;
  HostViewportInfo(regs, ps && ps->writes_depth(), vp);
  uint32_t rt_mask = 0;
  for (uint32_t i = 0; i < 4; ++i)
    if ((color_mask >> (4 * i)) & 0xF) rt_mask |= 1u << i;
  if (!rt_mask && !depth_used) {
    ++stats.skipped;  // nothing to write (no occlusion query emulation yet)
    return;
  }
  const uint32_t height = std::clamp(scissor.offset[1] + scissor.extent[1], 1u, 2048u);
  profile::Mark(profile::kSetup);
  BoundTargets targets;
  if (!r.render_targets().Bind(list, SurfaceRegsFromRegisters(s.regs), rt_mask, depth_used,
                               height, &targets)) {
    DRAW_LOG_ONCE(log_rt, "PGR4 native draw: RenderTargets::Bind failed (surface {:08X})",
                  regs[XE_GPU_REG_RB_SURFACE_INFO]);
    ++stats.failed;
    return;
  }
  profile::Mark(profile::kRtBind);

  // Translations.
  auto translate = [&](DxbcShader& shader, uint64_t modification) -> DxbcShader::Translation* {
    TranslationMemo& m = translation_memo[(reinterpret_cast<uintptr_t>(&shader) >> 6) & 255];
    if (m.shader == &shader && m.modification == modification) {
      ++diag.translation_hits;
      return m.translation;
    }
    DxbcShader::Translation* t = shaders.Translate(shader, modification);
    if (t) m = {&shader, modification, t};
    return t;
  };
  DxbcShader::Translation* vst = translate(*vs, mods.vertex);
  DxbcShader::Translation* pst = ps ? translate(*ps, mods.pixel) : nullptr;
  if (!vst || (ps && !pst)) {
    DRAW_LOG_ONCE(log_shader, "PGR4 native draw: shader translation failed");
    ++stats.failed;
    return;
  }
  profile::Mark(profile::kTranslate);

  // Pipeline description (GetCurrentStateDescription, host render targets).
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vs = vst;
  key.ps = pst;
  GeometryShaderKey gs_key;
  if (GetGeometryShaderKey(GeometryShaderForPrimitive(prim), mods.vertex, pst ? mods.pixel : 0,
                           gs_key))
    key.gs_key = gs_key.key;
  key.topology_type = uint8_t(TopologyTypeForPrimitive(prim));
  key.strip_cut = strip_cut;
  key.samples = 1;
  auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (!rasterization) {
    key.disable_raster = 1;
    key.cull = uint8_t(D3D12_CULL_MODE_NONE);
    key.depth_clip = 1;
  } else {
    bool cull_front = false, cull_back = false;
    key.cull = uint8_t(D3D12_CULL_MODE_NONE);
    if (polygonal) {
      key.front_ccw = sc.face == 0;
      cull_front = sc.cull_front != 0;
      cull_back = sc.cull_back != 0;
      if (cull_front) key.cull = uint8_t(D3D12_CULL_MODE_FRONT);
      else if (cull_back) key.cull = uint8_t(D3D12_CULL_MODE_BACK);
      if (!cull_front && sc.polymode_front_ptype != xenos::PolygonType::kTriangles) key.wireframe = 1;
      if (!cull_back && sc.polymode_back_ptype != xenos::PolygonType::kTriangles) key.wireframe = 1;
      if (sc.poly_mode != xenos::PolygonModeEnable::kDualMode) key.wireframe = 0;
    }
    float poly_offset, poly_offset_scale;
    GetPreferredFacePolygonOffset(regs, polygonal, poly_offset_scale, poly_offset);
    key.depth_bias = rex::graphics::draw_util::GetD3D10IntegerPolygonOffset(
        regs.Get<reg::RB_DEPTH_INFO>().depth_format, poly_offset);
    key.slope_scaled_depth_bias = poly_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit;
    key.depth_clip = !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
    if (targets.depth) {
      uint8_t func = uint8_t(xenos::CompareFunction::kAlways);
      if (depth_control.z_enable) {
        func = uint8_t(depth_control.zfunc);
        key.depth_write = depth_control.z_write_enable;
      }
      key.depth_func = uint8_t(D3D12_COMPARISON_FUNC_NEVER + func);
      key.depth_enable = func != uint8_t(xenos::CompareFunction::kAlways) || key.depth_write;
      if (depth_control.stencil_enable) {
        key.stencil_enable = 1;
        const bool bf = polygonal && depth_control.backface_enable;
        auto ref = regs.Get<reg::RB_STENCILREFMASK>(bf && cull_front
                                                        ? XE_GPU_REG_RB_STENCILREFMASK_BF
                                                        : XE_GPU_REG_RB_STENCILREFMASK);
        key.stencil_read_mask = uint8_t(ref.stencilmask);
        key.stencil_write_mask = uint8_t(ref.stencilwritemask);
        key.front[0] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilfail));
        key.front[1] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzfail));
        key.front[2] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzpass));
        key.front[3] = uint8_t(D3D12_COMPARISON_FUNC_NEVER + uint32_t(depth_control.stencilfunc));
        if (bf) {
          key.back[0] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilfail_bf));
          key.back[1] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzfail_bf));
          key.back[2] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzpass_bf));
          key.back[3] =
              uint8_t(D3D12_COMPARISON_FUNC_NEVER + uint32_t(depth_control.stencilfunc_bf));
        } else {
          std::memcpy(key.back, key.front, 4);
        }
      }
      if (key.depth_enable || key.stencil_enable) key.dsv = targets.depth->view_format;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (!targets.color[i]) continue;
      key.num_rts = uint8_t(i + 1);
      key.rtv[i] = targets.color[i]->view_format;
      PipelineKey::Rt& rt = key.rt[i];
      rt.mask = uint8_t((color_mask >> (i * 4)) & 0xF);
      BlendFactor src = BlendFactor::kOne, dst = BlendFactor::kZero, src_a = BlendFactor::kOne,
                  dst_a = BlendFactor::kZero;
      uint32_t op = 0, op_a = 0;
      if (rt.mask) {
        auto bc = regs.Get<reg::RB_BLENDCONTROL>(kBlendControlRegs[i]);
        src = kBlendFactorMap[uint32_t(bc.color_srcblend) & 31];
        dst = kBlendFactorMap[uint32_t(bc.color_destblend) & 31];
        op = uint32_t(bc.color_comb_fcn);
        src_a = kBlendFactorAlphaMap[uint32_t(bc.alpha_srcblend) & 31];
        dst_a = kBlendFactorAlphaMap[uint32_t(bc.alpha_destblend) & 31];
        op_a = uint32_t(bc.alpha_comb_fcn);
      }
      if (src != BlendFactor::kOne || dst != BlendFactor::kZero || op != 0 ||
          src_a != BlendFactor::kOne || dst_a != BlendFactor::kZero || op_a != 0) {
        rt.blend = 1;
        rt.src = uint8_t(src);
        rt.dst = uint8_t(dst);
        rt.op = uint8_t(op);
        rt.src_a = uint8_t(src_a);
        rt.dst_a = uint8_t(dst_a);
        rt.op_a = uint8_t(op_a);
      } else {
        rt.src = uint8_t(BlendFactor::kOne);
        rt.src_a = uint8_t(BlendFactor::kOne);
      }
    }
    if (key.num_rts || key.dsv != DXGI_FORMAT_UNKNOWN) key.samples = uint8_t(targets.samples);
  }
  if (debug_mask & 1) {
    key.depth_enable = key.depth_write = key.stencil_enable = 0;
    key.dsv = DXGI_FORMAT_UNKNOWN;
    if (!key.num_rts) key.samples = 1;
  }
  if (debug_mask & 2) key.cull = uint8_t(D3D12_CULL_MODE_NONE);
  if (debug_mask & 4)
    for (auto& rt : key.rt) rt.blend = 0;
  if (debug_mask & 64) {
    key.depth_bias = 0;
    key.slope_scaled_depth_bias = 0;
  }
  const uint32_t draw_index = frame_draws++;
  bool debug_skip = (debug_max_draws && draw_index >= debug_max_draws) ||
                    (draw_index >= debug_skip_from && draw_index < debug_skip_to);
  uint32_t snap_now = 0;
  if (debug_hash_count) {
    const uint64_t vs_hash = vs->ucode_data_hash(), ps_hash = ps ? ps->ucode_data_hash() : 0;
    uint32_t listed = 0;
    for (uint32_t i = 0; i < debug_hash_count; ++i)
      if (debug_hashes[i] == vs_hash || (ps && debug_hashes[i] == ps_hash)) listed |= 1u << i;
    if (debug_mask & 256) snap_now = listed & snap_pending;
    else debug_skip |= bool(listed) != bool(debug_mask & 128);
  } else if ((snap_pending >> 31) && draw_index == debug_skip_from) {
    snap_now = 1u << 31;
  }
  if (debug_skip) {
    ++stats.skipped;
    return;
  }
  if (draw_log) {
    std::fprintf(draw_log,
                 "%u fn=%08X prim=%u n=%u idx=%u vs=%016llX:%llX ps=%016llX:%llX rt=%u:%d,%d dsv=%d "
                 "z=%u/%u/%u st=%u cull=%u blend=%u:%u/%u/%u,%u/%u/%u mask=%X bc=%08X cc=%08X "
                 "f=%.2f,%.2f,%.2f,%.2f samples=%u vp=%u,%u,%u,%u z=%.3f-%.3f sc=%u,%u,%u,%u "
                 "tex=%zu ci=%08X si=%08X di=%08X su=%08X dc=%08X po=%g/%g,%g/%g bias=%d/%g\n",
                 draw_index, uint32_t(s.fn), s.primitive, count, uint32_t(s.indexed),
                 (unsigned long long)vs->ucode_data_hash(), (unsigned long long)mods.vertex,
                 ps ? (unsigned long long)ps->ucode_data_hash() : 0ull,
                 (unsigned long long)mods.pixel, key.num_rts, int(key.rtv[0]), int(key.rtv[1]),
                 int(key.dsv), key.depth_enable, key.depth_write, key.depth_func,
                 key.stencil_enable, key.cull, key.rt[0].blend, key.rt[0].src, key.rt[0].dst,
                 key.rt[0].op, key.rt[0].src_a, key.rt[0].dst_a, key.rt[0].op_a, key.rt[0].mask,
                 regs[XE_GPU_REG_RB_BLENDCONTROL0], regs[XE_GPU_REG_RB_COLORCONTROL],
                 regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
                 regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
                 key.samples, vp.xy_offset[0], vp.xy_offset[1], vp.xy_extent[0], vp.xy_extent[1],
                 vp.z_min, vp.z_max, scissor.offset[0], scissor.offset[1], scissor.extent[0],
                 scissor.extent[1],
                 ps ? ps->GetTextureBindingsAfterTranslation().size() : size_t(0),
                 regs[XE_GPU_REG_RB_COLOR_INFO], regs[XE_GPU_REG_RB_SURFACE_INFO],
                 regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
                 regs[XE_GPU_REG_RB_DEPTHCONTROL],
                 regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE),
                 regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET),
                 regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE),
                 regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET), key.depth_bias,
                 key.slope_scaled_depth_bias);
  }
  // With dynamic depth bias the polygon offset is not part of the pipeline.
  const float depth_bias = float(key.depth_bias);
  const float slope_bias = key.slope_scaled_depth_bias;
  if (dynamic_depth_bias) {
    key.depth_bias = 0;
    key.slope_scaled_depth_bias = 0;
  }
  const uint64_t key_hash = XXH3_64bits(&key, sizeof(key));
  PsoMemo& pso_slot = pso_memo[key_hash & 1023];
  ID3D12PipelineState* pso = nullptr;
  if (pso_slot.pso && pso_slot.hash == key_hash) {
    pso = pso_slot.pso;
    ++diag.pso_hits;
  } else {
    Pipeline* p = GetPipeline(key_hash, key, *vst, pst);
    uint8_t state = p->state.load(std::memory_order_acquire);
    if (state == kCompiling) {
      if (p->prewarm) PromotePipeline(p);
      if (WaitForPipeline(*p)) state = p->state.load(std::memory_order_acquire);
    }
    if (state != kReady) {
      if (state == kCompiling) {
        ++stats.pso_pending;
        ++frame_pending_draws;
      } else {
        ++stats.failed;
      }
      return;
    }
    pso = p->pso.Get();
    pso_slot = {key_hash, pso};
  }
  profile::Mark(profile::kPipeline);

  // Textures and samplers (bindless descriptor indices, b4).
  const uint32_t used_textures =
      vs->GetUsedTextureMaskAfterTranslation() | (ps ? ps->GetUsedTextureMaskAfterTranslation() : 0);
  DxbcShaderTranslator::SystemConstants sys;
  std::memset(&sys, 0, sizeof(sys));
  uint32_t desc_vs[64], desc_ps[64];
  uint8_t signs[32] = {};
  TextureCache& texture_cache = r.textures();
  auto bind_textures = [&](const DxbcShader& shader, uint32_t* indices) -> uint32_t {
    const auto& tex = shader.GetTextureBindingsAfterTranslation();
    const auto& smp = shader.GetSamplerBindingsAfterTranslation();
    ShaderSlots& ss = shader_slots[(reinterpret_cast<uintptr_t>(&shader) >> 6) & 511];
    if (ss.shader != &shader) {
      // The translator numbers bindless descriptors from 1 (it counts the new binding before
      // assigning its index), so the table needs max index + 1 entries, not texture + sampler
      // count: the last binding (often a fetch's signed SRV) was left out of the upload.
      uint32_t n = 1, mask = 0;
      for (const auto& tb : tex) {
        n = std::max(n, tb.bindless_descriptor_index + 1);
        mask |= 1u << (tb.fetch_constant & 31);
      }
      for (const auto& sb : smp) {
        n = std::max(n, sb.bindless_descriptor_index + 1);
        mask |= 1u << (sb.fetch_constant & 31);
      }
      ss.shader = &shader;
      ss.mask = mask;
      ss.count = uint32_t(std::popcount(mask));
      ss.n = std::min(n, 64u);
      ss.last = nullptr;
    }
    const uint32_t n = ss.n;
    // The memo entry for (shader, the fetch constants it reads); null if not memoizable.
    BindMemo* bm = nullptr;
    const uint64_t epoch = texture_cache.change_epoch();
    // While the draw log records, every binding goes through Get so each one is logged.
    if (!draw_log && ss.count <= BindMemo::kMaxSlots && tex.size() <= BindMemo::kMaxBound) {
      auto same_fetch = [&](const BindMemo& e) {
        if (e.shader != &shader) return false;
        const uint32_t* stored = e.data;
        for (uint32_t m = ss.mask; m; m &= m - 1, stored += 6)
          if (std::memcmp(stored, s.fetch(uint32_t(std::countr_zero(m))), 24) != 0) return false;
        return true;
      };
      profile::Counter miss_kind = profile::kBindMissEmpty;
      bool found = false;
      if (ss.last && same_fetch(*ss.last)) {
        bm = ss.last;
        found = true;
        profile::Count(profile::kBindMru);
      } else {
        uint64_t h = uint64_t(reinterpret_cast<uintptr_t>(&shader)) * 0x9E3779B97F4A7C15ull;
        for (uint32_t m = ss.mask; m; m &= m - 1) {
          const uint32_t* f = s.fetch(uint32_t(std::countr_zero(m)));
          uint64_t a, b, c;
          std::memcpy(&a, f, 8);
          std::memcpy(&b, f + 2, 8);
          std::memcpy(&c, f + 4, 8);
          h = (h ^ a ^ std::rotl(b, 21) ^ std::rotl(c, 42)) * 0x9E3779B97F4A7C15ull;
        }
        BindMemo* set = &bind_memo[size_t(h >> (64 - kBindMemoSetBits)) << 1];
        for (uint32_t way = 0; way < 2 && !found; ++way) {
          if (same_fetch(set[way])) {
            bm = &set[way];
            found = true;
          } else if (set[way].shader == &shader) {
            miss_kind = profile::kBindMissFetch;
          }
        }
        // Not found: fill the least recently used way.
        if (!found) bm = set[0].used <= set[1].used ? &set[0] : &set[1];
        bm->used = ++bind_clock;
        ss.last = bm;
      }
      if (found) {
        bool same = true;
        if (bm->epoch != epoch) {
          for (uint32_t i = 0; i < bm->bound && same; ++i)
            same = texture_cache.Unchanged(bm->entry[i]);
          if (same) bm->epoch = epoch;
        }
        if (same) {
          const uint32_t* stored = bm->data + ss.count * 6;
          std::memcpy(indices, stored, n * sizeof(uint32_t));
          stored += n;
          for (uint32_t i = 0; i < bm->bound; ++i) signs[stored[i] & 0xFF] = uint8_t(stored[i] >> 8);
          stats.textures += bm->textures;
          ++stats.bind_memo_hits;
          profile::Count(profile::kBindHit);
          return n;
        }
        miss_kind = profile::kBindMissTexture;
      }
      profile::Count(miss_kind);
    }
    profile::Scope miss_scope(profile::kTexMiss);
    ++stats.bind_memo_misses;
    std::fill(indices, indices + n, 0u);
    uint32_t bound = 0, textures = 0, missing = 0;
    for (const auto& tb : tex) {
      ++textures;
      TextureBinding b;
      uint32_t index;
      const uint32_t* fetch = s.fetch(tb.fetch_constant);
      bool got;
      {
        profile::Scope get_scope(profile::kTexGet);
        got = texture_cache.Get(fetch, list, r.frame_fence(), &b) && b.resource;
      }
      if (draw_log)
        std::fprintf(draw_log, "  tf%u %08X fmt=%u %ux%ux%u dim=%u tiled=%u mips=%u%s%s\n",
                     tb.fetch_constant, b.key.base_page << 12, uint32_t(b.key.format), b.key.width,
                     b.key.height, b.key.depth_or_array_size, uint32_t(b.key.dimension),
                     uint32_t(b.key.tiled), b.key.mip_levels(), got ? "" : " missing",
                     got && b.aliased ? " alias" : "");
      if (got) {
        profile::Scope srv_scope(profile::kTexSrv);
        const uint32_t mapping = b.srv.Shader4ComponentMapping;
        const uint64_t sh = (uint64_t(reinterpret_cast<uintptr_t>(b.resource)) ^
                             (uint64_t(mapping) << 32) ^ (uint64_t(tb.is_signed) << 3) ^
                             (uint64_t(tb.dimension) << 4)) *
                            0x9E3779B97F4A7C15ull;
        SrvMemo& m = srv_memo[sh >> (64 - kSrvMemoBits)];
        if (m.resource == b.resource && m.mapping == mapping &&
            m.is_signed == uint8_t(tb.is_signed) && m.dimension == uint8_t(tb.dimension)) {
          index = m.index;
          profile::Count(profile::kSrvMemoHit);
        } else {
          index = TextureDescriptor(b, tb);
          m.resource = b.resource;
          m.mapping = mapping;
          m.is_signed = uint8_t(tb.is_signed);
          m.dimension = uint8_t(tb.dimension);
          m.index = index;
        }
        signs[tb.fetch_constant] = b.swizzled_signs;
        if (bm) {
          bm->entry[bound] = b.entry;
          bm->data[ss.count * 6 + n + bound] =
              uint32_t(uint8_t(tb.fetch_constant)) | uint32_t(b.swizzled_signs) << 8;
        }
        ++bound;
      } else {
        ++missing;
        index = tb.dimension == xenos::FetchOpDimension::kCube          ? kNullCube
                : tb.dimension == xenos::FetchOpDimension::k3DOrStacked ? kNull3D
                                                                        : kNull2DArray;
      }
      if (tb.bindless_descriptor_index < 64) indices[tb.bindless_descriptor_index] = index;
    }
    for (const auto& sb : smp) {
      const uint32_t* fetch = s.fetch(sb.fetch_constant);
      const uint8_t filters[4] = {uint8_t(sb.mag_filter), uint8_t(sb.min_filter),
                                  uint8_t(sb.mip_filter), uint8_t(sb.aniso_filter)};
      uint32_t sh = 0;
      for (int i = 0; i < 6; ++i) sh = (sh ^ fetch[i]) * 0x9E3779B1u;
      uint32_t fw;
      std::memcpy(&fw, filters, 4);
      sh = (sh ^ fw) * 0x9E3779B1u;
      SamplerMemo& m = sampler_memo[sh >> (32 - kSamplerMemoBits)];
      uint32_t index;
      if (std::memcmp(m.filters, filters, 4) == 0 &&
          std::memcmp(m.fetch, fetch, sizeof(m.fetch)) == 0) {
        index = m.index;
      } else {
        index = SamplerDescriptor(GetSamplerKey(regs, sb));
        std::memcpy(m.filters, filters, 4);
        std::memcpy(m.fetch, fetch, sizeof(m.fetch));
        m.index = index;
      }
      if (sb.bindless_descriptor_index < 64) indices[sb.bindless_descriptor_index] = index;
    }
    stats.textures += textures;
    stats.textures_missing += missing;
    if (bm && !missing) {
      bm->shader = &shader;
      bm->epoch = epoch;
      uint32_t* stored = bm->data;
      for (uint32_t m = ss.mask; m; m &= m - 1, stored += 6)
        std::memcpy(stored, s.fetch(uint32_t(std::countr_zero(m))), 24);
      std::memcpy(stored, indices, n * sizeof(uint32_t));
      bm->bound = bound;
      bm->textures = textures;
    } else {
      if (bm) bm->shader = nullptr;
      profile::Count(profile::kBindNoMemo);
    }
    return n;
  };
  const uint32_t n_desc_vs = bind_textures(*vs, desc_vs);
  const uint32_t n_desc_ps = ps ? bind_textures(*ps, desc_ps) : 1;
  if (!ps) desc_ps[0] = 0;
  profile::Mark(profile::kTextures);

  // System constants (UpdateSystemConstantValues, host render targets, no ROV).
  {
    auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
    auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
    auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
    auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
    uint32_t flags = 0;
    if (vte.vtx_xy_fmt) flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
    if (vte.vtx_z_fmt) flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
    if (vte.vtx_w0_fmt) flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
    if (polygonal) flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
    if (rex::graphics::draw_util::IsPrimitiveLine(false, prim))
      flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
    // No kSysFlag_DepthFloat24: without float24 conversion in the shader, its only effect is to
    // scale oDepth into the plugin's host 0...0.5 float24 range, while viewports, clears and
    // resolves here keep D24FS8 in 0...1 (HostViewportInfo). Setting it halves the depth that
    // depth-restoring passes write.
    xenos::CompareFunction alpha_func =
        colorcontrol.alpha_test_enable ? colorcontrol.alpha_func : xenos::CompareFunction::kAlways;
    flags |= uint32_t(alpha_func) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
    // Gamma render targets are unorm16 on the host (the plugin's setting): no conversion flag.
    if (debug_mask & 8)
      flags &= ~(7u << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift),
          flags |= 7u << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
    sys.flags = flags;
    sys.tessellation_factor_range_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
    sys.tessellation_factor_range_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
    sys.line_loop_closing_index = 0;
    sys.vertex_index_endian = index_endian;
    sys.vertex_index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
    sys.vertex_index_min = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
    sys.vertex_index_max = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
    if (!clip.clip_disable) {
      float* ucp = sys.user_clip_planes[0];
      uint32_t remaining = clip.ucp_ena;
      while (remaining) {
        uint32_t i = uint32_t(std::countr_zero(remaining));
        remaining &= remaining - 1;
        std::memcpy(ucp, &regs[XE_GPU_REG_PA_CL_UCP_0_X + i * 4], 4 * sizeof(float));
        ucp += 4;
      }
    }
    for (uint32_t i = 0; i < 3; ++i) {
      sys.ndc_scale[i] = vp.ndc_scale[i];
      sys.ndc_offset[i] = vp.ndc_offset[i];
    }
    if (prim == xenos::PrimitiveType::kPointList) {
      auto minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
      auto size = regs.Get<reg::PA_SU_POINT_SIZE>();
      sys.point_vertex_diameter_min = float(minmax.min_size) * (2.0f / 16.0f);
      sys.point_vertex_diameter_max = float(minmax.max_size) * (2.0f / 16.0f);
      sys.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
      sys.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
      sys.point_screen_diameter_to_ndc_radius[0] = 1.0f / float(std::max(vp.xy_extent[0], 1u));
      sys.point_screen_diameter_to_ndc_radius[1] = 1.0f / float(std::max(vp.xy_extent[1], 1u));
    }
    uint32_t remaining = used_textures;
    while (remaining) {
      uint32_t i = uint32_t(std::countr_zero(remaining));
      remaining &= remaining - 1;
      sys.texture_swizzled_signs[i >> 2] |= uint32_t(signs[i]) << ((i & 3) * 8);
      if (profile::g.on) {
        const uint32_t* f = s.fetch(i);
        const uint8_t sg = signs[i];
        ++diag.fetch_classes[sg == 0 ? 0 : sg == 0x3F ? 1 : sg == 0xFF ? 2 : sg == 0x55 ? 3 : 4];
        if ((f[3] >> 13) & 0x3F) ++diag.fetch_classes[5];
        if ((f[4] >> 12) & 0x3FF) ++diag.fetch_classes[6];
      }
    }
    sys.textures_resolution_scaled = 0;
    sys.sample_count_log2[0] = surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
    sys.sample_count_log2[1] = surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
    sys.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
    sys.alpha_to_mask =
        colorcontrol.alpha_to_mask_enable ? (colorcontrol.value >> 24) | (1 << 8) : 0;
    for (uint32_t i = 0; i < 4; ++i) {
      auto ci = regs.Get<reg::RB_COLOR_INFO>(kColorInfoRegs[i]);
      int32_t bias = ci.color_exp_bias;
      // snorm16 render targets use the full -32...32 range (the plugin's default).
      if (ci.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
          ci.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16)
        bias -= 5;
      uint32_t bits = uint32_t(0x3F800000 + (bias << 23));
      std::memcpy(&sys.color_exp_bias[i], &bits, 4);
    }
  }
  profile::Mark(profile::kSysConst);

  // Float constants packed by the shaders' maps (UpdateBindings), compared and staged in place.
  PackFloats(cb_float_vs, vs, s.regs, XE_GPU_REG_SHADER_CONSTANT_000_X);
  PackFloats(cb_float_ps, ps, s.regs, XE_GPU_REG_SHADER_CONSTANT_256_X);
  profile::Mark(profile::kPack);

  // Vertex data and indices into shared memory (made by Prepare with `prep`).
  const uint32_t index_bytes = count * (s.index_32bit ? 4u : 2u);
  const bool fan = prim == xenos::PrimitiveType::kTriangleFan;
  if (!prep) {
    upload_fence = r.frame_fence();
    if (!RequestVertices(s, *vs, copies)) {
      copies.clear();
      ++stats.failed;
      return;
    }
  }
  FlushCopies(list);
  profile::Mark(profile::kVertices);

  // Fixed-function state.
  D3D12_VIEWPORT viewport = {float(vp.xy_offset[0]), float(vp.xy_offset[1]),
                             float(vp.xy_extent[0]),  float(vp.xy_extent[1]),
                             vp.z_min,                vp.z_max};
  D3D12_RECT scissor_rect = {LONG(scissor.offset[0]), LONG(scissor.offset[1]),
                             LONG(scissor.offset[0] + scissor.extent[0]),
                             LONG(scissor.offset[1] + scissor.extent[1])};

  // Root state.
  if (list_dirty) {
    ID3D12DescriptorHeap* heaps[] = {view_heap.Get(), sampler_heap.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetGraphicsRootSignature(root_signature.Get());
    list->SetGraphicsRootDescriptorTable(kBindlessRoot_SharedMemory, view_gpu);
    list->SetGraphicsRootDescriptorTable(kBindlessRoot_SamplerHeap, sampler_gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE table = view_gpu;
    table.ptr += uint64_t(kViewTableStart) * view_stride;
    list->SetGraphicsRootDescriptorTable(kBindlessRoot_ViewHeap, table);
    list_dirty = false;
  }
  bool ok = UploadCbv(cb_system, &sys, sizeof(sys), kBindlessRoot_SystemConstants, list) &&
            CommitCbv(cb_float_vs, cb_float_vs.last, kBindlessRoot_FloatConstantsVertex, list) &&
            CommitCbv(cb_float_ps, cb_float_ps.last, kBindlessRoot_FloatConstantsPixel, list) &&
            UploadCbv(cb_bool_loop, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                      (8 + 32) * 4, kBindlessRoot_BoolLoopConstants, list) &&
            UploadCbv(cb_fetch, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 32 * 6 * 4,
                      kBindlessRoot_FetchConstants, list) &&
            UploadCbv(cb_desc_vs, desc_vs, n_desc_vs * 4, kBindlessRoot_DescriptorIndicesVertex,
                      list) &&
            UploadCbv(cb_desc_ps, desc_ps, n_desc_ps * 4, kBindlessRoot_DescriptorIndicesPixel,
                      list);
  if (!ok) {
    ++stats.failed;
    return;
  }
  profile::Mark(profile::kCbv);
  if (bound_pso != pso) {
    list->SetPipelineState(pso);
    bound_pso = pso;
    // SetPipelineState loads the pipeline's depth bias, dynamic or not (DirectX-Specs
    // VulkanOn12, "initial dynamic depth bias state"). These pipelines are keyed with zero bias.
    if (dynamic_depth_bias) bound_depth_bias = bound_slope_bias = 0.0f;
  }
  if (bound_topology != topology) {
    list->IASetPrimitiveTopology(topology);
    bound_topology = topology;
  }
  if (std::memcmp(&bound_viewport, &viewport, sizeof(viewport))) {
    list->RSSetViewports(1, &viewport);
    bound_viewport = viewport;
  }
  if (std::memcmp(&bound_scissor, &scissor_rect, sizeof(scissor_rect))) {
    list->RSSetScissorRects(1, &scissor_rect);
    bound_scissor = scissor_rect;
  }
  float blend[4] = {regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
                    regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
                    regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
                    regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  if (std::memcmp(bound_blend, blend, sizeof(blend))) {
    list->OMSetBlendFactor(blend);
    std::memcpy(bound_blend, blend, sizeof(blend));
  }
  {
    const bool bf = polygonal && depth_control.backface_enable && sc.cull_front && !sc.cull_back;
    uint32_t ref = regs.Get<reg::RB_STENCILREFMASK>(bf ? XE_GPU_REG_RB_STENCILREFMASK_BF
                                                       : XE_GPU_REG_RB_STENCILREFMASK)
                       .stencilref;
    if (ref != bound_stencil_ref) {
      list->OMSetStencilRef(ref);
      bound_stencil_ref = ref;
    }
  }
  if (dynamic_depth_bias && (depth_bias != bound_depth_bias || slope_bias != bound_slope_bias)) {
    if (list9_of != list) {
      list9.Reset();
      list->QueryInterface(IID_PPV_ARGS(&list9));
      list9_of = list;
    }
    if (list9) list9->RSSetDepthBias(depth_bias, 0.0f, slope_bias);
    bound_depth_bias = depth_bias;
    bound_slope_bias = slope_bias;
  }
  // Render targets: only the ones the pipeline declares, the DSV only when it is used.
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[4] = {};
  for (uint32_t i = 0; i < key.num_rts; ++i) rtv[i] = targets.rtv[i];
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
  if (key.dsv != DXGI_FORMAT_UNKNOWN) dsv = targets.dsv;
  if (bound_rt_count != key.num_rts || std::memcmp(bound_rtv, rtv, sizeof(rtv)) ||
      bound_dsv.ptr != dsv.ptr) {
    list->OMSetRenderTargets(key.num_rts, key.num_rts ? rtv : nullptr, FALSE,
                             dsv.ptr ? &dsv : nullptr);
    std::memcpy(bound_rtv, rtv, sizeof(rtv));
    bound_dsv = dsv;
    bound_rt_count = key.num_rts;
  }

  if (fan) {
    if (count < 3) {
      ++stats.skipped;
      return;
    }
    D3D12_INDEX_BUFFER_VIEW ibv;
    uint32_t host_count;
    bool wide;
    if (prep) {
      if (!prep->fan_count) {
        ++stats.failed;
        return;
      }
      host_count = prep->fan_count;
      wide = prep->fan_wide;
      ibv.BufferLocation = prep->fan_indices;
    } else {
      host_count = (count - 2) * 3;
      wide = s.indexed ? s.index_32bit : count > 0xFFFF;
      UploadPool::Alloc a;
      if (!vertex_upload.Allocate(uint64_t(host_count) * (wide ? 4 : 2), 16, r.frame_fence(), &a)) {
        ++stats.failed;
        return;
      }
      WriteFanIndices(s, count, wide, a.cpu);
      ibv.BufferLocation = a.gpu;
    }
    ibv.SizeInBytes = host_count * (wide ? 4 : 2);
    ibv.Format = wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    list->IASetIndexBuffer(&ibv);
    bound_ibv = ibv;
    list->DrawIndexedInstanced(host_count, 1, 0, 0, 0);
  } else if (s.indexed) {
    // The whole shared memory buffer is the index buffer and the draw starts at the guest
    // address, so consecutive draws only rebind it when the index format changes.
    const uint32_t index_size = s.index_32bit ? 4u : 2u;
    const bool aligned = (s.index_address & (index_size - 1)) == 0;
    D3D12_INDEX_BUFFER_VIEW ibv;
    ibv.BufferLocation = aligned ? shared_gpu : shared_gpu + s.index_address;
    ibv.SizeInBytes = aligned ? kSharedMemorySize : index_bytes;
    ibv.Format = s.index_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    if (std::memcmp(&bound_ibv, &ibv, sizeof(ibv))) {
      list->IASetIndexBuffer(&ibv);
      bound_ibv = ibv;
    }
    list->DrawIndexedInstanced(count, 1, aligned ? s.index_address / index_size : 0, 0, 0);
  } else {
    list->DrawInstanced(count, 1, 0, 0);
  }
  ++stats.draws;
  ++diag.draws;
  if (snap_now) {
    snap_pending &= ~snap_now;
    SnapshotTargets(list, targets, draw_index);
  }
  profile::Mark(profile::kRecord);
}

void DrawRenderer::Impl::SnapshotTargets(ID3D12GraphicsCommandList* list, const BoundTargets& targets,
                                         uint32_t draw_index) {
  const std::string stem = "C:\\recomp\\logs\\pgr4-native-captures\\snap-f" + std::to_string(r.frame_index()) +
                           "-d" + std::to_string(draw_index);
  for (uint32_t i = 0; i < targets.color_count; ++i) Snapshot(list, targets.color[i], stem + "-c" + std::to_string(i));
  Snapshot(list, targets.depth, stem + "-depth");
  snap_fence = r.frame_fence();
  REXLOG_INFO("PGR4 native draw: snapshot of draw {} ({} targets) to {}-*", draw_index, snaps.size(), stem);
  ResetListState();  // the barriers moved the targets out of their bound states
}

void DrawRenderer::Impl::Snapshot(ID3D12GraphicsCommandList* list, RenderTarget* rt, const std::string& stem) {
  if (!rt || rt->samples != 1) return;
  const D3D12_RESOURCE_DESC desc = rt->resource->GetDesc();
  Snap s;
  s.stem = stem;
  uint64_t total = 0;
  // Subresource 0: the color target, or a depth target's depth plane.
  device->GetCopyableFootprints(&desc, 0, 1, 0, &s.layout, &s.rows, &s.row_bytes, &total);
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = total;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&s.readback))))
    return;
  const D3D12_RESOURCE_STATES state = rt->state;
  RenderTargets::Transition(list, rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = rt->resource.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  dst.pResource = s.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = s.layout;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  RenderTargets::Transition(list, rt, state);
  snaps.push_back(std::move(s));
}

void DrawRenderer::Impl::WriteSnapshots(uint64_t completed_fence) {
  if (snaps.empty() || completed_fence < snap_fence) return;
  for (Snap& s : snaps) {
    void* data = nullptr;
    const D3D12_RANGE range = {0, size_t(s.layout.Footprint.RowPitch) * s.rows};
    if (FAILED(s.readback->Map(0, &range, &data))) continue;
    if (std::FILE* f = std::fopen((s.stem + ".bin").c_str(), "wb")) {
      for (uint32_t y = 0; y < s.rows; ++y)
        std::fwrite(static_cast<const uint8_t*>(data) + size_t(y) * s.layout.Footprint.RowPitch, 1,
                    size_t(s.row_bytes), f);
      std::fclose(f);
    }
    const D3D12_RANGE none = {0, 0};
    s.readback->Unmap(0, &none);
    if (std::FILE* f = std::fopen((s.stem + ".txt").c_str(), "w")) {
      std::fprintf(f, "%u %u %llu %u\n", s.layout.Footprint.Width, s.rows, (unsigned long long)s.row_bytes,
                   uint32_t(s.layout.Footprint.Format));
      std::fclose(f);
    }
  }
  REXLOG_INFO("PGR4 native draw: wrote {} snapshot targets", snaps.size());
  snaps.clear();
}

DrawRenderer::DrawRenderer(NativeRenderer& renderer)
    : renderer_(renderer), impl_(std::make_unique<Impl>(renderer, stats_)) {}
DrawRenderer::~DrawRenderer() = default;

bool DrawRenderer::Initialize() { return impl_->Initialize(); }

void DrawRenderer::OnDraw(const uint8_t* base, const DrawState& state, const DrawPrep* prep) {
  impl_->Draw(base, state, prep);
}

void DrawRenderer::Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
                           std::vector<VertexCopy>* copies) {
  impl_->Prepare(state, upload_tag, prep, copies);
}

void DrawRenderer::BeginCopySegment() { impl_->NewSegment(); }

void DrawRenderer::EndFramePrepare(uint64_t done, uint64_t* upload_bytes, uint64_t* uploads) {
  impl_->watch_frame.fetch_add(1, std::memory_order_relaxed);
  impl_->vertex_upload.EndFrame(done);
  *upload_bytes = impl_->prep_upload_bytes;
  *uploads = impl_->prep_uploads;
  impl_->prep_upload_bytes = impl_->prep_uploads = 0;
}

void DrawRenderer::InvalidateListState() {
  impl_->ResetListState();
  if (impl_->draw_log) std::fprintf(impl_->draw_log, "resolve (after draw %u)\n", impl_->frame_draws);
}

std::pair<uint64_t, uint64_t> DrawRenderer::ShaderHashes(const DrawPrep& prep) {
  return {prep.vs ? prep.vs->ucode_data_hash() : 0, prep.ps ? prep.ps->ucode_data_hash() : 0};
}

std::string DrawRenderer::ShaderDisassembly(uint64_t hash) const {
  const DxbcShader* shader = impl_->shaders.Find(hash);
  return shader ? shader->ucode_disassembly() : std::string();
}

void DrawRenderer::LogClear(const ClearCall& c) {
  std::FILE* f = impl_->draw_log;
  if (!f) return;
  std::fprintf(f, "clear (after draw %u) flags=%X colors=%X depth_bound=%d z=%g stencil=%u height=%u si=%08X "
               "ci0=%08X di=%08X rects", impl_->frame_draws, c.p.flags, c.bound_colors, int(c.depth_bound),
               c.p.depth, uint32_t(c.p.stencil), c.height, c.regs.surface_info, c.regs.color_info[0],
               c.regs.depth_info);
  for (const D3D12_RECT& r : c.p.rects) std::fprintf(f, " %ld,%ld-%ld,%ld", r.left, r.top, r.right, r.bottom);
  std::fprintf(f, " edram");
  for (const D3D12_RECT& r : c.p.edram_rects) std::fprintf(f, " %ld,%ld-%ld,%ld", r.left, r.top, r.right, r.bottom);
  std::fprintf(f, "\n");
}

bool DrawRenderer::FrameIncomplete() const {
  const Impl& m = *impl_;
  return m.frame_pending_draws && m.pipeline_burst &&
         std::max(m.frame_new_pipelines, m.last_frame_new_pipelines) >= m.pipeline_burst;
}

void DrawRenderer::EndFrame(uint64_t completed_fence) {
  stats_.watch_faults = impl_->watch_faults.exchange(0, std::memory_order_relaxed);
  stats_.watch_arms = impl_->watch_arms.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_pages = impl_->watch_arm_pages.exchange(0, std::memory_order_relaxed);
  stats_.hash_pages = impl_->hash_pages.exchange(0, std::memory_order_relaxed);
  stats_.hash_copies = impl_->hash_copies.exchange(0, std::memory_order_relaxed);
  if (!impl_->threaded) impl_->watch_frame.fetch_add(1, std::memory_order_relaxed);
  if (impl_->trace.file) {
    {
      std::lock_guard<std::mutex> lock(impl_->trace.mutex);
      ++impl_->trace.frame;
    }
    if ((impl_->trace.frame & 255) == 0) impl_->trace.Flush();
  }
  if (!impl_->threaded) impl_->vertex_upload.EndFrame(completed_fence);
  impl_->constant_upload.EndFrame(completed_fence);
  impl_->Prewarm();
  impl_->store.Flush();
  impl_->frame_draws = 0;
  impl_->last_frame_new_pipelines = std::exchange(impl_->frame_new_pipelines, 0);
  impl_->frame_pipeline_wait = {};
  impl_->frame_pending_draws = 0;
  impl_->WriteSnapshots(completed_fence);
  if (profile::g.on && ++impl_->diag.frames >= 300) {
    Impl::Diag& d = impl_->diag;
    const double n = double(std::max<uint64_t>(d.draws, 1));
    auto per = [&](uint64_t v) { return double(v) / n; };
    REXLOG_INFO(
        "PGR4 native draw diag: {} draws/frame; per draw: shader loads {:.3f}, mods hits {:.3f}, "
        "translation hits {:.3f}, pso hits {:.3f}, ranges {:.3f} (pages {:.1f}, copied runs "
        "{:.3f}), unwatched copies {:.3f}, copy flushes {:.3f}",
        d.draws / d.frames, per(d.shader_loads), per(d.mods_hits), per(d.translation_hits),
        per(d.pso_hits), per(d.ranges), per(d.range_pages), per(d.range_runs), per(d.unwatched),
        per(d.flushes));
    std::string cbv;
    for (uint32_t i = 0; i < 16; ++i) {
      if (!d.cbv_uploads[i]) continue;
      char part[64];
      std::snprintf(part, sizeof(part), " r%u=%.3f/%.0fB", i, per(d.cbv_uploads[i]),
                    double(d.cbv_bytes[i]) / double(d.cbv_uploads[i]));
      cbv += part;
    }
    REXLOG_INFO("PGR4 native draw diag: cbv uploads per draw / bytes per upload:{}", cbv);
    const uint64_t* fc = d.fetch_classes;
    REXLOG_INFO("PGR4 native draw diag: textures per draw by signs: unsigned {:.3f}, gamma rgb "
                "{:.3f}, gamma {:.3f}, signed {:.3f}, other {:.3f}; exp bias {:.3f}, lod bias {:.3f}",
                per(fc[0]), per(fc[1]), per(fc[2]), per(fc[3]), per(fc[4]), per(fc[5]), per(fc[6]));
    d = Impl::Diag{};
  }
  if (++impl_->debug_poll >= 15) {
    impl_->debug_poll = 0;
    uint32_t mask = 0, max_draws = 0, skip_from = 0, skip_to = 0, hash_count = 0;
    std::array<uint64_t, 8> hashes{};
    if (std::FILE* f = std::fopen("C:\\recomp\\logs\\pgr4-native-captures\\draw-debug.txt", "r")) {
      if (std::fscanf(f, "%u %u %u %u", &mask, &max_draws, &skip_from, &skip_to) < 1) mask = 0;
      unsigned long long h;
      while (hash_count < hashes.size() && std::fscanf(f, "%llx", &h) == 1) hashes[hash_count++] = h;
      std::fclose(f);
    }
    if (mask != impl_->debug_mask || max_draws != impl_->debug_max_draws ||
        skip_from != impl_->debug_skip_from || skip_to != impl_->debug_skip_to ||
        hash_count != impl_->debug_hash_count || hashes != impl_->debug_hashes)
      REXLOG_INFO("PGR4 native draw: debug mask {} max draws {} skip [{}, {}), {} shader hashes",
                  mask, max_draws, skip_from, skip_to, hash_count);
    impl_->debug_hashes = hashes;
    impl_->debug_hash_count = hash_count;
    // Bit 16: log the next frame's draws once (to <capture dir>\draws-f<frame>.txt).
    if ((mask & 16) && !(impl_->debug_mask & 16)) impl_->draw_log_pending = true;
    if ((mask & 256) && !(impl_->debug_mask & 256))
      impl_->snap_pending = hash_count ? (1u << hash_count) - 1 : 1u << 31;
    impl_->debug_mask = mask;
    impl_->debug_max_draws = max_draws;
    impl_->debug_skip_from = skip_from;
    impl_->debug_skip_to = skip_to;
  }
  if (impl_->draw_log) {
    std::fclose(impl_->draw_log);
    impl_->draw_log = nullptr;
  }
  if (impl_->draw_log_pending) {
    impl_->draw_log_pending = false;
    const std::string path = "C:\\recomp\\logs\\pgr4-native-captures\\draws-f" +
                             std::to_string(renderer_.frame_index()) + ".txt";
    impl_->draw_log = std::fopen(path.c_str(), "w");
  }
  // The next frame's list starts with the shared buffer in whatever state this frame left it;
  // D3D12 keeps resource states across command lists, so no reset of shared_state here.
}

}  // namespace pgr4::native
