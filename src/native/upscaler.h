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
// do, and those copies take the alpha of the scene's target, which FSR does not upscale: the
// motion blur's mask (pixel shader 27A3898925DDCAF7), which keeps the player's car sharp. With
// frame generation (frame_generation.h) the same depth and motion are its inputs.
//
// Threading: everything on the thread that records the frames (NativeRenderer::Execute*) but the
// FidelityFX dispatch, which runs on the device's command stream (command_stream.h); the context is
// made again only with the stream idle (D3D12Device::WaitIdle).
#pragma once

#include "d3d12_device.h"
#include "dynamic_resolution.h"
#include "fidelityfx.h"
#include "gpu_timer.h"
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
  static constexpr uint32_t kSceneWidth = kFfxSceneWidth;
  static uint32_t SceneHeight() { return FrameHeight(); }
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
  // gpu_busy_ns) and the time in ns of a frame at the pace the display shows them
  // (DynamicResolution::Update). True when the render size changed: the scene's targets take the
  // new scale() (RenderTargets::SetSceneScale) before the frame's draws.
  bool BeginFrame(uint64_t frame, uint64_t gpu_busy_ns, uint64_t paced_ns);
  // The history is not this scene's (the upscaler was off): the next upscaling starts over, and
  // the GPU times of the frames before are not counted toward the render size.
  void Reset() {
    reset_ = true;
    last_ns_ = 0;
    if (dynamic_) dynamic_->Restart();
  }
  // FSR's debug view in place of the upscaled scene (FfxDebug): its inputs (color, depth, motion
  // vectors, reactiveness) and what it made of them, in a grid, and with FSR 4 AMD's watermark.
  // Either way the context is created again, debug visualization taking more memory and the
  // driver reading the watermark's variable at creation: between frames, with the GPU idle. False
  // (logged) when that failed. OptiScaler upscales in a context of its own, from the inputs alone:
  // through it the watermark shows, and the debug view is its menu's (Upscaler Debug View).
  bool debug() const { return debug_; }
  bool SetDebug(bool on) { return CreateContext(on); }
  // This frame's sub-pixel jitter in render pixels (x right, y down), for the scene's draws.
  float jitter_x() const { return jitter_[0]; }
  float jitter_y() const { return jitter_[1]; }
  // A scene draw's alpha to coverage dither offsets this frame, from the guest's (RB_COLORCONTROL
  // bits 24:31, a 2-bit threshold offset for each pixel of a 2x2 quad). Single-sampled, alpha to
  // coverage is that ordered dither alone, the same in every frame, and the upscaler's history
  // keeps it as a screen door in the color, depth and motion. Rotated one place a frame, each
  // pixel takes the four thresholds in turn, and the history averages them to the coverage.
  uint32_t AlphaToMaskOffsets(uint32_t offsets) const {
    const uint32_t shift = 2 * uint32_t(frame_ & 3);
    return ((offsets >> shift) | (offsets << (8 - shift))) & 0xFF;
  }
  // The scene's render size this frame on its guest size, and its largest
  // (--pgr4_native_upscaler_width/height: the size of the scene's targets).
  RenderScale scale() const {
    return {{render_width_, render_height_}, {kSceneWidth, SceneHeight()}};
  }
  RenderScale max_scale() const {
    return {{max_width_, max_height_}, {kSceneWidth, SceneHeight()}};
  }
  uint32_t render_height() const { return render_height_; }
  // The scene's texture LOD bias, in negative quarter levels (SamplerKey::lod_bias).
  uint32_t mip_bias_quarters() const { return mip_bias_quarters_; }

  // The motion vector target of the scene's draws at the largest render size
  // (DxbcShaderTranslator's kMotionVectorRenderTarget, kMotionFormat), cleared to no motion at the
  // first call of a frame.
  static constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
  D3D12_CPU_DESCRIPTOR_HANDLE MotionTarget(CommandStream* list);

  // The upscaled scene color of this frame: at the first call of a frame (a resolve out of the
  // scene surface `regs`), records the upscaling of the surface's color and depth into `list`.
  // nullptr (logged once) when that failed; the resolves then copy the target as it is.
  RenderTarget* Upscale(CommandStream* list, RenderTargets& targets, const SurfaceRegs& regs);
  // Frame generation's inputs, recorded by each Upscale; nullptr: none.
  void set_frame_generation(FrameGeneration* fg) { frame_generation_ = fg; }
  // The scene's camera, the renderer's, read at each Upscale.
  void set_camera(const FfxCamera* camera) { camera_ = camera; }
  // With GPU timing: Upscale marks its passes (GpuTimer::UpscalePhase).
  void set_gpu_timer(GpuTimer* timer) { timer_ = timer; }

 private:
  // The upscaling context, with debug visualization and the watermark or not; false (logged) on
  // failure.
  bool CreateContext(bool debug);
  bool CreateTargets();
  // The commands recorded next belong to the upscaling pass `phase` (with GPU timing).
  void Time(CommandStream* list, GpuTimer::UpscalePhase phase);
  // The render size, LOD bias and jitter phases of a DynamicResolution level; false (logged)
  // without a phase count.
  bool SetLevel(uint32_t level);
  // The clamp of FSR's input (kInputHlsl); false (logged) on failure.
  bool CreateInputPass();
  // Records the clamp of the scene's `color` into input_ at the render size.
  void ClampInput(CommandStream* list, ID3D12Resource* color);

  ID3D12Device* device_ = nullptr;
  const FfxApi* api_ = nullptr;
  FrameGeneration* frame_generation_ = nullptr;
  const FfxCamera* camera_ = nullptr;  // the renderer's
  GpuTimer* timer_ = nullptr;
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
  ComPtr<ID3D12RootSignature> input_signature_;
  ComPtr<ID3D12PipelineState> input_pso_;
  ComPtr<ID3D12DescriptorHeap> input_heap_;
  uint32_t input_stride_ = 0;
  uint32_t max_width_ = kSceneWidth, max_height_ = 0;  // Initialize's, from the render height
  uint32_t render_width_ = kSceneWidth, render_height_ = 0;
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
  bool debug_ = false;  // context_ has debug visualization and the watermark
};

}  // namespace pgr4::native
