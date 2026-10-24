// Shader modification selection and root signatures for the DXBC translator.
//
// SelectModifications and the root signature builder follow ReXGlue's
// src/graphics/d3d12/command_processor.cpp and pipeline_cache.cpp (Xenia, Copyright Ben Vanik,
// BSD-3; adapted for ReXGlue by Tom Clay), reduced to the host-render-target path.
#include "shader_translate.h"

#include <array>
#include <bit>
#include <string>

#include <xxhash.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

// The SDK translator sources read these GPU cvars, which the rexgpu-xenos plugin DLL defines and
// registers. The app keeps unregistered storage with the plugin's defaults (see
// sdk_cvar_shim.h for the ones the translator sources define themselves).
std::string& FLAGS_dump_shaders_storage_() {
  static std::string storage;
  return storage;
}
bool& FLAGS_use_fuzzy_alpha_epsilon_storage_() {
  static bool storage = false;
  return storage;
}

// util/draw.cpp is plugin code (it needs the register file); the translator only needs these.
namespace rex::graphics::draw_util {
const int8_t kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
const int8_t kD3D10StandardSamplePositions4x[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
}  // namespace rex::graphics::draw_util

namespace pgr4::native {

namespace reg = rex::graphics::reg;
namespace xenos = rex::graphics::xenos;
using Microsoft::WRL::ComPtr;
using Modification = DxbcShaderTranslator::Modification;

namespace {

template <typename T>
T Reg(uint32_t value) {
  T r;
  r.value = value;
  return r;
}

bool CoverageDependsOnAlpha(reg::RB_COLORCONTROL c) {
  return (c.alpha_test_enable && c.alpha_func != xenos::CompareFunction::kAlways) ||
         c.alpha_to_mask_enable;
}

// draw_util::IsPixelShaderNeededWithRasterization.
bool PixelShaderNeeded(const DxbcShader& ps, const ShaderDrawRegs& regs) {
  if (Reg<reg::RB_MODECONTROL>(regs.rb_modecontrol).edram_mode != xenos::EdramMode::kColorDepth) {
    return false;
  }
  if (ps.kills_pixels() || ps.writes_depth() || ps.memexport_eM_written() ||
      (ps.writes_color_target(0) &&
       CoverageDependsOnAlpha(Reg<reg::RB_COLORCONTROL>(regs.rb_colorcontrol)))) {
    return true;
  }
  uint32_t rts = ps.writes_color_targets();
  while (rts) {
    uint32_t i = uint32_t(std::countr_zero(rts));
    rts &= rts - 1;
    uint32_t components = xenos::GetColorRenderTargetFormatComponentCount(
        Reg<reg::RB_COLOR_INFO>(regs.rb_color_info[i]).color_format);
    if ((regs.rb_color_mask >> (i * 4)) & ((1u << components) - 1)) return true;
  }
  return false;
}

D3D12_ROOT_PARAMETER Cbv(DxbcShaderTranslator::CbufferRegister reg,
                         D3D12_SHADER_VISIBILITY visibility) {
  D3D12_ROOT_PARAMETER p = {};
  p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  p.Descriptor.ShaderRegister = uint32_t(reg);
  p.Descriptor.RegisterSpace = 0;
  p.ShaderVisibility = visibility;
  return p;
}

D3D12_ROOT_PARAMETER Table(const D3D12_DESCRIPTOR_RANGE* ranges, uint32_t count,
                           D3D12_SHADER_VISIBILITY visibility) {
  D3D12_ROOT_PARAMETER p = {};
  p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p.DescriptorTable.NumDescriptorRanges = count;
  p.DescriptorTable.pDescriptorRanges = ranges;
  p.ShaderVisibility = visibility;
  return p;
}

D3D12_DESCRIPTOR_RANGE Range(D3D12_DESCRIPTOR_RANGE_TYPE type, uint32_t count, uint32_t base,
                             uint32_t space, uint32_t offset) {
  return {type, count, base, space, offset};
}

ComPtr<ID3D12RootSignature> Create(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc,
                                   uint64_t* blob_hash) {
  ComPtr<ID3DBlob> blob, error;
  HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
  if (FAILED(hr)) {
    REXLOG_ERROR("pgr4 native: root signature serialization failed ({:08X}): {}", uint32_t(hr),
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "");
    return nullptr;
  }
  if (blob_hash) *blob_hash = XXH3_64bits(blob->GetBufferPointer(), blob->GetBufferSize());
  ComPtr<ID3D12RootSignature> rs;
  hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                   IID_PPV_ARGS(&rs));
  if (FAILED(hr)) {
    REXLOG_ERROR("pgr4 native: CreateRootSignature failed ({:08X})", uint32_t(hr));
    return nullptr;
  }
  return rs;
}

constexpr std::array<D3D12_DESCRIPTOR_RANGE, 2> kSharedMemoryRanges = {{
    {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1,
     uint32_t(DxbcShaderTranslator::SRVMainRegister::kSharedMemory),
     uint32_t(DxbcShaderTranslator::SRVSpace::kMain), 0},
    {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, uint32_t(DxbcShaderTranslator::UAVRegister::kSharedMemory),
     0, 1},
}};

}  // namespace

ShaderModifications SelectModifications(const DxbcShaderTranslator& translator,
                                        const DxbcShader& vs, const DxbcShader* ps,
                                        const ShaderDrawRegs& regs) {
  ShaderModifications out;
  auto program_cntl = Reg<reg::SQ_PROGRAM_CNTL>(regs.sq_program_cntl);
  auto draw_initiator = Reg<reg::VGT_DRAW_INITIATOR>(regs.vgt_draw_initiator);
  bool points = draw_initiator.prim_type == xenos::PrimitiveType::kPointList;

  // D3D12CommandProcessor::IssueDraw: which draws reach the pipeline at all.
  auto mode = Reg<reg::RB_MODECONTROL>(regs.rb_modecontrol).edram_mode;
  if (mode == xenos::EdramMode::kCopy) return out;
  bool polygonal = rex::graphics::draw_util::IsPrimitivePolygonal(
      Reg<reg::VGT_OUTPUT_PATH_CNTL>(regs.vgt_output_path_cntl).path_select ==
          xenos::VGTOutputPath::kTessellationEnable,
      draw_initiator.prim_type);
  // draw_util::IsRasterizationPotentiallyDone.
  auto sc_mode = Reg<reg::PA_SU_SC_MODE_CNTL>(regs.pa_su_sc_mode_cntl);
  bool pitch_zero = !Reg<reg::RB_SURFACE_INFO>(regs.rb_surface_info).surface_pitch;
  bool rasterization =
      (mode == xenos::EdramMode::kColorDepth || mode == xenos::EdramMode::kDepthOnly) &&
      program_cntl.vs_export_mode != xenos::VertexShaderExportMode::kMultipass && !pitch_zero &&
      !(polygonal && sc_mode.cull_front && sc_mode.cull_back);
  if (rasterization) {
    if (ps && !PixelShaderNeeded(*ps, regs)) ps = nullptr;  // also drops it outside kColorDepth
  } else {
    if (!vs.memexport_eM_written()) return out;
    ps = nullptr;
  }
  out.draw = true;
  out.pixel_shader_used = ps != nullptr;

  uint32_t param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      ps ? vs.writes_interpolators() &
               ps->GetInterpolatorInputMask(
                   program_cntl, Reg<reg::SQ_CONTEXT_MISC>(regs.sq_context_misc), param_gen_pos)
         : 0;

  // Vertex shader (always a plain host vertex shader: PGR4 does not tessellate).
  Modification vm(translator.GetDefaultVertexShaderModification(
      vs.GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg),
      rex::graphics::Shader::HostVertexShaderType::kVertex));
  vm.vertex.interpolator_mask = interpolator_mask;
  auto clip = Reg<reg::PA_CL_CLIP_CNTL>(regs.pa_cl_clip_cntl);
  uint32_t ucp = clip.clip_disable ? 0 : clip.ucp_ena;
  vm.vertex.user_clip_plane_count = uint32_t(std::popcount(ucp));
  vm.vertex.user_clip_plane_cull = uint32_t(ucp && clip.ucp_cull_only_ena);
  vm.vertex.point_ps_ucp_mode = clip.ps_ucp_mode;
  vm.vertex.vertex_kill_and =
      uint32_t((vs.writes_point_size_edge_flag_kill_vertex() & 0b100) && !clip.vtx_kill_or);
  vm.vertex.output_point_size =
      uint32_t((vs.writes_point_size_edge_flag_kill_vertex() & 0b001) && points);
  out.vertex = vm.value;

  if (!ps) return out;

  // Pixel shader, host render targets, no float24 conversion in the shader (the plugin's
  // default: depth_float24_convert_in_pixel_shader off).
  Modification pm(translator.GetDefaultPixelShaderModification(
      ps->GetDynamicAddressableRegisterCount(program_cntl.ps_num_reg)));
  pm.pixel.interpolator_mask = interpolator_mask;
  pm.pixel.interpolators_centroid =
      interpolator_mask &
      ~xenos::GetInterpolatorSamplingPattern(
          Reg<reg::RB_SURFACE_INFO>(regs.rb_surface_info).msaa_samples,
          Reg<reg::SQ_CONTEXT_MISC>(regs.sq_context_misc).sc_sample_cntl,
          Reg<reg::SQ_INTERPOLATOR_CNTL>(regs.sq_interpolator_cntl).sampling_pattern);
  if (param_gen_pos < xenos::kMaxInterpolators) {
    pm.pixel.param_gen_enable = 1;
    pm.pixel.param_gen_interpolator = param_gen_pos;
    pm.pixel.param_gen_point = uint32_t(points);
  } else {
    pm.pixel.param_gen_enable = 0;
    pm.pixel.param_gen_interpolator = 0;
    pm.pixel.param_gen_point = 0;
  }
  using DepthStencilMode = Modification::DepthStencilMode;
  pm.pixel.depth_stencil_mode =
      ps->implicit_early_z_write_allowed() &&
              (!ps->writes_color_target(0) ||
               !CoverageDependsOnAlpha(Reg<reg::RB_COLORCONTROL>(regs.rb_colorcontrol)))
          ? DepthStencilMode::kEarlyHint
          : DepthStencilMode::kNoModifiers;
  out.pixel = pm.value;
  return out;
}

ComPtr<ID3D12RootSignature> CreateBindlessRootSignature(ID3D12Device* device, uint64_t* blob_hash) {
  using CB = DxbcShaderTranslator::CbufferRegister;
  using Space = DxbcShaderTranslator::SRVSpace;
  D3D12_ROOT_PARAMETER p[kBindlessRoot_Count];
  p[kBindlessRoot_FetchConstants] = Cbv(CB::kFetchConstants, D3D12_SHADER_VISIBILITY_ALL);
  p[kBindlessRoot_FloatConstantsVertex] = Cbv(CB::kFloatConstants, D3D12_SHADER_VISIBILITY_VERTEX);
  p[kBindlessRoot_FloatConstantsPixel] = Cbv(CB::kFloatConstants, D3D12_SHADER_VISIBILITY_PIXEL);
  p[kBindlessRoot_DescriptorIndicesPixel] =
      Cbv(CB::kDescriptorIndices, D3D12_SHADER_VISIBILITY_PIXEL);
  p[kBindlessRoot_DescriptorIndicesVertex] =
      Cbv(CB::kDescriptorIndices, D3D12_SHADER_VISIBILITY_VERTEX);
  p[kBindlessRoot_SystemConstants] = Cbv(CB::kSystemConstants, D3D12_SHADER_VISIBILITY_ALL);
  p[kBindlessRoot_BoolLoopConstants] = Cbv(CB::kBoolLoopConstants, D3D12_SHADER_VISIBILITY_ALL);
  p[kBindlessRoot_SharedMemory] =
      Table(kSharedMemoryRanges.data(), uint32_t(kSharedMemoryRanges.size()),
            D3D12_SHADER_VISIBILITY_ALL);
  D3D12_DESCRIPTOR_RANGE samplers = Range(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 0, 0);
  p[kBindlessRoot_SamplerHeap] = Table(&samplers, 1, D3D12_SHADER_VISIBILITY_ALL);
  // The three texture dimensions alias the same unbounded range of the view heap; the bindless
  // descriptor indices in b4 index from its start.
  D3D12_DESCRIPTOR_RANGE views[3] = {
      Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, uint32_t(Space::kBindlessTextures2DArray),
            0),
      Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, uint32_t(Space::kBindlessTextures3D), 0),
      Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, uint32_t(Space::kBindlessTexturesCube),
            0),
  };
  p[kBindlessRoot_ViewHeap] = Table(views, 3, D3D12_SHADER_VISIBILITY_ALL);
  D3D12_ROOT_SIGNATURE_DESC desc = {kBindlessRoot_Count, p, 0, nullptr,
                                    D3D12_ROOT_SIGNATURE_FLAG_NONE};
  return Create(device, desc, blob_hash);
}

}  // namespace pgr4::native
