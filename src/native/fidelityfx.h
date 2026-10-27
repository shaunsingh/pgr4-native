// pgr4-native PGR4 native renderer: the FidelityFX API (ffx_api) of amd_fidelityfx_loader_dx12.dll
// beside the executable, loaded at run time, which the upscaler (upscaler.h) and the frame
// generation (frame_generation.h) share. OptiScaler hooks the same calls (it loads as dxgi.dll and
// intercepts the loader), which is how other upscalers and frame generators replace AMD's.
#pragma once

#include <ffx_api.h>

#include <cstdint>

namespace pgr4::native {

struct FfxApi {
  PfnFfxCreateContext create = nullptr;
  PfnFfxDestroyContext destroy = nullptr;
  PfnFfxConfigure configure = nullptr;
  PfnFfxQuery query = nullptr;
  PfnFfxDispatch dispatch = nullptr;
};

// The loader's entry points, loaded once; nullptr (logged) without them.
const FfxApi* LoadFfxApi();

// The scene both effects take: PGR4's 1280x720 (Upscaler::kSceneWidth/Height).
inline constexpr uint32_t kFfxSceneWidth = 1280, kFfxSceneHeight = 720;
// The camera's planes and vertical field of view are not known to the renderer. FidelityFX uses
// them only to scale its disocclusion thresholds with view-space depth; these are a chase camera's.
inline constexpr float kFfxCameraNear = 0.1f, kFfxCameraFar = 10000.0f, kFfxCameraFovY = 1.0f;

}  // namespace pgr4::native
