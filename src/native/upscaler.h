// pgr4-native PGR4 native renderer: temporal upscaling of the 3D scene with AMD FidelityFX Super
// Resolution, through the FidelityFX API (fidelityfx.h), which OptiScaler hooks to put other
// upscalers such as FSR 4 in its place.
//
// The scene is PGR4's 1280x720 2x MSAA surface: RB_SURFACE_INFO pitch 1280 with 2x MSAA, the
// only surface of that pitch and MSAA in the race and the menus (draw logs of td-265). With
// --pgr4_native_upscaler its host targets are single-sampled and of the render size
// (--pgr4_native_upscaler_width/height, RenderTargets::OverrideScene: the draws' viewports,
// scissors and clears scale with them; with --pgr4_native_upscaler_target_fps a size up to that
// chosen each frame, DynamicResolution and RenderTargets::SetSceneScale), every draw into it is
// offset by the frame's sub-pixel jitter and samples its mipmapped textures with AMD's LOD bias for
// the render size (DrawRenderer, the jitter through the system constants' NDC offset), the draws
// that write depth also write their motion into MotionTarget (vertex shaders that run again with
// the previous frame's constants, DxbcShaderTranslator's motion_vectors), and the first resolve out
// of it in a frame (EndTiling's depth resolve, before its color resolves clear the surface)
// upscales its color, depth and motion into the output, which the color resolves copy from instead
// of the target. FSR reads the color clamped to the range of its 7e3 EDRAM format, as the resolves
// do, and the output then takes the scene's alpha, which FSR does not upscale: the motion blur's
// mask, which keeps the player's car sharp. With frame generation (frame_generation.h) the same
// depth and motion are its inputs.
//
// Threading: everything on the thread that records the frames (NativeRenderer::Execute*).
#pragma once

#include "d3d12_device.h"
#include "dynamic_resolution.h"
#include "fidelityfx.h"
#include "render_targets.h"

#include <cstdint>
#include <optional>

namespace pgr4::native {

class FrameGeneration;

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
  static constexpr uint32_t kSceneWidth = kFfxSceneWidth, kSceneHeight = kFfxSceneHeight;
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

  // Once per frame before recording it: the frame's jitter, and with
  // --pgr4_native_upscaler_target_fps its render size from the GPU's time (D3D12Device::
  // gpu_busy_ns). True when the render size changed: the scene's targets take the new scale()
  // (RenderTargets::SetSceneScale) before the frame's draws.
  bool BeginFrame(uint64_t frame, uint64_t gpu_busy_ns);
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
  // The scene's render size this frame on its guest size, and its largest
  // (--pgr4_native_upscaler_width/height: the size of the scene's targets).
  RenderScale scale() const {
    return {{render_width_, render_height_}, {kSceneWidth, kSceneHeight}};
  }
  RenderScale max_scale() const { return {{max_width_, max_height_}, {kSceneWidth, kSceneHeight}}; }
  uint32_t render_height() const { return render_height_; }
  // The scene's texture LOD bias, in negative quarter levels (SamplerKey::lod_bias).
  uint32_t mip_bias_quarters() const { return mip_bias_quarters_; }

  // The motion vector target of the scene's draws at the largest render size
  // (DxbcShaderTranslator's kMotionVectorRenderTarget, kMotionFormat), cleared to no motion at the
  // first call of a frame.
  static constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
  D3D12_CPU_DESCRIPTOR_HANDLE MotionTarget(ID3D12GraphicsCommandList* list);

  // The upscaled scene color of this frame: at the first call of a frame (a resolve out of the
  // scene surface `regs`), records the upscaling of the surface's color and depth into `list`.
  // nullptr (logged once) when that failed; the resolves then copy the target as it is.
  RenderTarget* Upscale(ID3D12GraphicsCommandList* list, RenderTargets& targets,
                        const SurfaceRegs& regs);
  // Frame generation's inputs, recorded by each Upscale; nullptr: none.
  void set_frame_generation(FrameGeneration* fg) { frame_generation_ = fg; }

 private:
  // The upscaling context, with debug visualization or not; false (logged) on failure.
  bool CreateContext(bool debug);
  bool CreateTargets();
  // The render size, LOD bias and jitter phases of a DynamicResolution level; false (logged)
  // without a phase count.
  bool SetLevel(uint32_t level);
  // The compute passes around FSR: the clamp of its input (kInputHlsl), and the alpha of the
  // output (kAlphaHlsl), left out (logged) without typed UAV loads of the output. False (logged)
  // on failure.
  bool CreatePasses();
  // Records a pass that reads `src` and writes `dst` over a width x height grid, with the views in
  // the frame's slot `slot` (of kViewsPerFrame / 2).
  void RunPass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, ID3D12Resource* src,
               ID3D12Resource* dst, uint32_t slot, float scale_x, float scale_y, uint32_t width,
               uint32_t height);

  ID3D12Device* device_ = nullptr;
  const FfxApi* api_ = nullptr;
  FrameGeneration* frame_generation_ = nullptr;
  void* context_ = nullptr;  // ffxContext
  // The upscaled color (an R16G16B16A16_FLOAT UAV: the scene's host format), in the fields the
  // resolve reads; and the motion vectors.
  RenderTarget output_;
  ComPtr<ID3D12Resource> motion_;
  ComPtr<ID3D12DescriptorHeap> motion_rtv_;
  D3D12_RESOURCE_STATES motion_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
  uint64_t motion_frame_ = UINT64_MAX;  // the frame motion_ was cleared for
  // FSR's color input: the scene's color clamped, at the largest render size.
  ComPtr<ID3D12Resource> input_;
  D3D12_RESOURCE_STATES input_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ComPtr<ID3D12RootSignature> pass_signature_;
  ComPtr<ID3D12PipelineState> input_pso_;
  ComPtr<ID3D12PipelineState> alpha_pso_;  // null without typed UAV loads
  ComPtr<ID3D12DescriptorHeap> pass_heap_;
  uint32_t pass_stride_ = 0;
  uint32_t max_width_ = kSceneWidth, max_height_ = kSceneHeight;
  uint32_t render_width_ = kSceneWidth, render_height_ = kSceneHeight;
  std::optional<DynamicResolution> dynamic_;  // with --pgr4_native_upscaler_target_fps
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
