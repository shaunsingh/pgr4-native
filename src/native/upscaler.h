// pgr4-native PGR4 native renderer: temporal upscaling of the 3D scene with AMD FidelityFX Super
// Resolution, through the FidelityFX API (ffx_api) of amd_fidelityfx_loader_dx12.dll beside the
// executable, loaded at run time. OptiScaler hooks the same calls (it loads as dxgi.dll and
// intercepts the loader), which is how other upscalers such as FSR 4 replace this one.
//
// The scene is PGR4's 1280x720 2x MSAA surface: RB_SURFACE_INFO pitch 1280 with 2x MSAA, the
// only surface of that pitch and MSAA in the race and the menus (draw logs of td-265). With
// --pgr4_native_upscaler its host targets are single-sampled and of the render size
// (--pgr4_native_upscaler_width/height, RenderTargets::OverrideScene: the draws' viewports,
// scissors and clears scale with them), every draw into it is offset by the frame's sub-pixel
// jitter and samples its mipmapped textures with AMD's LOD bias for the render size (DrawRenderer,
// the jitter through the system constants' NDC offset), the draws that write depth also write their
// motion into MotionTarget
// (vertex shaders that run again with the previous frame's constants, DxbcShaderTranslator's
// motion_vectors), and the first resolve out of it in a frame (EndTiling's depth resolve, before
// its color resolves clear the surface) upscales its color, depth and motion into the output,
// which the color resolves copy from instead of the target.
//
// Threading: everything on the thread that records the frames (NativeRenderer::Execute*).
#pragma once

#include "d3d12_device.h"
#include "render_targets.h"

#include <cstdint>

namespace pgr4::native {

class Upscaler {
 public:
  // Whether --pgr4_native_upscaler asks for the upscaler.
  static bool Requested();

  // The scene surface, from its RB_SURFACE_INFO (pitch and MSAA; the guest's value whatever the
  // host targets' sample count).
  static constexpr uint32_t kSceneSurfaceMask = 0x33FFF;
  static constexpr uint32_t kSceneSurface = 0x10500;  // pitch 1280, 2x MSAA
  static constexpr bool IsScene(uint32_t surface_info) {
    return (surface_info & kSceneSurfaceMask) == kSceneSurface;
  }
  static constexpr uint32_t kSceneWidth = 1280, kSceneHeight = 720;
  // Whether a span of the scene surface (a resolve's, a viewport's) is the scene's width: the
  // scene resolves in tiles of its whole width, while the rear-view mirror renders into a corner
  // of the scene surface and resolves that corner on its own (neither upscaled nor jittered).
  static constexpr bool IsSceneWidth(int32_t x0, int32_t x1) {
    return x0 <= 0 && x1 >= int32_t(kSceneWidth);
  }

  Upscaler() = default;
  ~Upscaler();
  Upscaler(const Upscaler&) = delete;
  Upscaler& operator=(const Upscaler&) = delete;

  // Loads the FidelityFX API and creates the upscaling context; false (logged) when either fails.
  bool Initialize(ID3D12Device* device);

  // Once per frame before recording it: the frame's jitter.
  void BeginFrame(uint64_t frame);
  // The history is not this scene's (the upscaler was off): the next upscaling starts over.
  void Reset() {
    reset_ = true;
    last_ns_ = 0;
  }
  // FSR's debug view in place of the upscaled scene: its inputs (color, depth, motion vectors,
  // reactiveness) and what it made of them, in a grid. The first use creates the context again
  // with debug visualization (more memory), which then stays: between frames, with the GPU idle
  // when debug_context() is false. False (logged) when that failed.
  bool debug_view() const { return debug_view_; }
  bool debug_context() const { return debug_context_; }
  bool SetDebugView(bool on);
  // This frame's sub-pixel jitter in render pixels (x right, y down), for the scene's draws.
  float jitter_x() const { return jitter_[0]; }
  float jitter_y() const { return jitter_[1]; }
  // The scene's render size (--pgr4_native_upscaler_width/height) on its guest size.
  RenderScale scale() const {
    return {{render_width_, render_height_}, {kSceneWidth, kSceneHeight}};
  }
  // The scene's texture LOD bias, in negative quarter levels (SamplerKey::lod_bias).
  uint32_t mip_bias_quarters() const { return mip_bias_quarters_; }

  // The motion vector target of the scene's draws at the render size (DxbcShaderTranslator's
  // kMotionVectorRenderTarget, kMotionFormat), cleared to no motion at the first call of a frame.
  static constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
  D3D12_CPU_DESCRIPTOR_HANDLE MotionTarget(ID3D12GraphicsCommandList* list);

  // The upscaled scene color of this frame: at the first call of a frame (a resolve out of the
  // scene surface `regs`), records the upscaling of the surface's color and depth into `list`.
  // nullptr (logged once) when that failed; the resolves then copy the target as it is.
  RenderTarget* Upscale(ID3D12GraphicsCommandList* list, RenderTargets& targets,
                        const SurfaceRegs& regs);

 private:
  struct Api;
  // The loader's entry points, loaded once; nullptr (logged) without them.
  static const Api* LoadApi();
  // The upscaling context, with debug visualization or not; false (logged) on failure.
  bool CreateContext(bool debug);
  bool CreateTargets();

  ID3D12Device* device_ = nullptr;
  const Api* api_ = nullptr;
  void* context_ = nullptr;  // ffxContext
  // The upscaled color (an R16G16B16A16_FLOAT UAV: the scene's host format), in the fields the
  // resolve reads; and the motion vectors.
  RenderTarget output_;
  ComPtr<ID3D12Resource> motion_;
  ComPtr<ID3D12DescriptorHeap> motion_rtv_;
  D3D12_RESOURCE_STATES motion_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
  uint64_t motion_frame_ = UINT64_MAX;  // the frame motion_ was cleared for
  uint32_t render_width_ = kSceneWidth, render_height_ = kSceneHeight;
  uint32_t mip_bias_quarters_ = 0;
  uint64_t frame_ = 0;
  uint64_t upscaled_frame_ = UINT64_MAX;
  bool upscaled_ = false;  // the frame's Upscale succeeded
  uint64_t last_ns_ = 0;
  float frame_ms_ = 0.0f;
  int32_t phase_count_ = 0;
  float jitter_[2] = {};
  bool reset_ = true;
  bool debug_view_ = false;
  bool debug_context_ = false;  // context_ has debug visualization
};

}  // namespace pgr4::native
