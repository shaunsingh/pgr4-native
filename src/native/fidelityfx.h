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

// The scene's camera: near and far planes and vertical field of view, by which FidelityFX scales
// its disocclusion thresholds. NativeRenderer reads it each frame from the clip transform in the
// scene draws' vertex shader constants (CameraFromClipRows); the defaults are that camera as
// measured there (td-323).
struct FfxCamera {
  float near_plane = 0.05f;
  float far_plane = 10000.0f;
  float fov_y = 1.0248f;  // 58.7 degrees
};

// If the four row vectors at `rows` are a perspective clip transform with inverted depth (row 2
// a multiple of row 3, the view direction: z_ndc = A + B / view depth is 1 at the near plane and
// 0 at the far), writes its camera to `out` and returns true. PGR4's scene vertex shaders keep
// the transform in c4-c7, and again in c20-c23 and c28-c31.
bool CameraFromClipRows(const float* rows, FfxCamera& out);

}  // namespace pgr4::native
