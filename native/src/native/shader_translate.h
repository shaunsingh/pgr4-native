// Xenos shader translation for the native renderer: ReXGlue's DXBC translator (BSD-3, from
// Xenia) compiled into the app with the host-render-target path (no ROV, no EDRAM buffer).
// Output is DXBC SM 5.1, which D3D12 takes directly; no HLSL step. Decision and reasons:
// the development notes, "#### ShaderXlate".
//
// What the translated shaders expect from the draw code (DxbcShaderTranslator's ABI):
// - cbuffers in space 0 (DxbcShaderTranslator::CbufferRegister): b0 system constants
//   (DxbcShaderTranslator::SystemConstants), b1 float constants (only the constants the shader
//   reads, packed in ascending order: Shader::constant_register_map().float_bitmap), b2 bool and
//   loop constants, b3 the 96 fetch constants (raw guest dwords), b4 bindless descriptor indices;
// - vertex data is not fed through the input assembler: the vertex shader reads guest memory
//   from the "shared memory" ByteAddressBuffer (SRV t0 space 0) at the vfetch constant's address;
// - textures and samplers: bindless (spaces 1-3, indices from b4) or bindful (t10.. space 0,
//   s0..), listed by DxbcShader::Get{Texture,Sampler}BindingsAfterTranslation();
// - one translation per (ucode, modification); the modification comes from a few registers per
//   draw (SelectModifications below), the same values the Xenos plugin uses.
#pragma once

#include <bit>
#include <cstdint>

#include <d3d12.h>
#include <wrl/client.h>

#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>

namespace pgr4::native {

using rex::graphics::DxbcShader;
using rex::graphics::DxbcShaderTranslator;

// Translator settings. The defaults are the Xenos plugin's on the RTX 3090 (bindless on,
// host render targets, gamma as unorm16, 2x MSAA supported, no resolution scale), so a
// translation here is byte-identical to the plugin's for the same modification.
struct ShaderTranslatorConfig {
  uint32_t vendor_id = 0x10DE;  // only changes code for Intel (no switch statements)
  bool bindless = true;
  bool gamma_render_target_as_unorm8 = false;
  bool msaa_2x_supported = true;
};

// Raw guest register values that select the translation of a draw's shaders.
struct ShaderDrawRegs {
  uint32_t sq_program_cntl = 0;
  uint32_t sq_context_misc = 0;
  uint32_t sq_interpolator_cntl = 0;
  uint32_t pa_cl_clip_cntl = 0;
  uint32_t vgt_draw_initiator = 0;  // only prim_type is read
  uint32_t vgt_output_path_cntl = 0;
  uint32_t pa_su_sc_mode_cntl = 0;
  uint32_t rb_modecontrol = 0;
  uint32_t rb_surface_info = 0;
  uint32_t rb_colorcontrol = 0;
  uint32_t rb_color_mask = 0;
  uint32_t rb_color_info[4] = {};  // RB_COLOR_INFO, RB_COLOR1_INFO, RB_COLOR2_INFO, RB_COLOR3_INFO
};

struct ShaderModifications {
  // false: the plugin drops the draw (EDRAM copy mode, i.e. the D3D's own resolve rectangles, or
  // nothing can rasterize and the vertex shader does not memexport); nothing else is valid then.
  bool draw = false;
  bool pixel_shader_used = false;  // false: draw without a pixel shader (depth only)
  uint64_t vertex = 0;
  uint64_t pixel = 0;
};

// Mirrors D3D12CommandProcessor::IssueDraw + PipelineCache::GetCurrent*ShaderModification for
// the host-render-target path. Both shaders must have their ucode analyzed (ShaderCache::Load
// does that); ps may be null.
ShaderModifications SelectModifications(const DxbcShaderTranslator& translator,
                                        const DxbcShader& vs, const DxbcShader* ps,
                                        const ShaderDrawRegs& regs);

// The pixel shader modification specialized for the draw's texture fetch constants: sets
// Modification::pixel.textures_unsigned when every fetch constant in `textures` (bit i: fetch
// constant i) has no exponent adjustment and components that fetch as unsigned: sign_x..sign_w
// all unsigned, or unsigned and signed on a format in `signed_is_unsigned_formats`
// (SignedViewIsUnsignedFormats: the signed view is the unsigned one, and kSigned applies no
// conversion of its own). `fetch` is the first of the 32 six-dword texture fetch constants.
// Signs and exponent adjustment change per draw, so this stays outside SelectModifications and
// its memo.
inline uint64_t SpecializePixelTextures(uint64_t pixel, const uint32_t* fetch, uint32_t textures,
                                        uint64_t signed_is_unsigned_formats) {
  if (!textures) return pixel;
  for (; textures; textures &= textures - 1) {
    const uint32_t* f = fetch + std::countr_zero(textures) * 6;
    const uint32_t signs = f[0] & 0x3FCu;  // 2 bits per component: 0 unsigned, 1 signed
    if (f[3] & 0x7E000u) return pixel;
    if (signs && ((signs & 0x2A8u) || !((signed_is_unsigned_formats >> (f[1] & 63)) & 1)))
      return pixel;
  }
  DxbcShaderTranslator::Modification m(pixel);
  m.pixel.textures_unsigned = 1;
  return m.value;
}
// Root parameter indices of the bindless root signature (the plugin's order).
enum BindlessRootParameter : uint32_t {
  kBindlessRoot_FetchConstants,
  kBindlessRoot_FloatConstantsVertex,
  kBindlessRoot_FloatConstantsPixel,
  kBindlessRoot_DescriptorIndicesPixel,
  kBindlessRoot_DescriptorIndicesVertex,
  kBindlessRoot_SystemConstants,
  kBindlessRoot_BoolLoopConstants,
  kBindlessRoot_SharedMemory,  // table: SRV t0 (shared memory), UAV u0 (shared memory)
  kBindlessRoot_SamplerHeap,   // unbounded sampler table from heap start
  kBindlessRoot_ViewHeap,      // unbounded 2D-array / 3D / cube SRVs (spaces 1-3) from table start
  kBindlessRoot_Count,
};

// Root parameter indices of a bindful root signature: the base ones, then optional tables in
// this order, present only when the shader has bindings (CreateBindfulRootSignature).
enum BindfulRootParameter : uint32_t {
  kBindfulRoot_FetchConstants,
  kBindfulRoot_FloatConstantsVertex,
  kBindfulRoot_FloatConstantsPixel,
  kBindfulRoot_SystemConstants,
  kBindfulRoot_BoolLoopConstants,
  kBindfulRoot_SharedMemory,
  kBindfulRoot_CountBase,  // + pixel textures, pixel samplers, vertex textures, vertex samplers
};

// The one root signature every bindless translation uses.
Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateBindlessRootSignature(ID3D12Device* device);
// Per texture/sampler-count root signature for bindful translations (after translation).
Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateBindfulRootSignature(ID3D12Device* device,
                                                                       const DxbcShader& vs,
                                                                       const DxbcShader* ps);

}  // namespace pgr4::native
