// pgr4-native PGR4 native renderer: the FidelityFX API loader. See fidelityfx.h.
#include "fidelityfx.h"

#include "d3d12_device.h"  // windows.h

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>

REXCVAR_DEFINE_BOOL(pgr4_native_fsr_debug, false, "PGR4 native",
                    "FidelityFX's debug views in place of the upscaled scene (its inputs and "
                    "history) and of the generated frames, and with FSR 4 AMD's watermark of the "
                    "model it runs; bind_upscaler_debug (F8) switches it");

namespace pgr4::native {

bool FfxDebug() { return REXCVAR_GET(pgr4_native_fsr_debug); }

void SetFfxWatermark(const wchar_t* name, bool on) {
  // The driver asks only whether the variable exists, of the process's environment or of its
  // CRT's copy (ucrtbase's, which SetEnvironmentVariableW leaves as it was): both are set, as
  // OptiScaler's Util::SetFlagEnvVar sets them.
  SetEnvironmentVariableW(name, on ? L"1" : nullptr);
  using Putenv = int(__cdecl*)(const wchar_t*, const wchar_t*);
  if (HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll"))
    if (const auto putenv = reinterpret_cast<Putenv>(GetProcAddress(ucrt, "_wputenv_s")))
      putenv(name, on ? L"1" : L"");
}

const FfxApi* LoadFfxApi() {
  static const FfxApi api = [] {
    FfxApi a;
    HMODULE dll = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
    if (!dll) {
      REXLOG_ERROR("PGR4 native: cannot load amd_fidelityfx_loader_dx12.dll ({})",
                   uint32_t(GetLastError()));
      return a;
    }
    a.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(dll, "ffxCreateContext"));
    a.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(dll, "ffxDestroyContext"));
    a.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(dll, "ffxConfigure"));
    a.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(dll, "ffxQuery"));
    a.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(dll, "ffxDispatch"));
    if (!a.create || !a.destroy || !a.configure || !a.query || !a.dispatch) {
      REXLOG_ERROR("PGR4 native: amd_fidelityfx_loader_dx12.dll lacks the ffx API");
      a = {};
    }
    return a;
  }();
  return api.create ? &api : nullptr;
}

bool CameraFromClipRows(const float* m, FfxCamera& out) {
  const float *x = m, *y = m + 4, *z = m + 8, *w = m + 12;
  const float wn2 = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
  if (wn2 < 1e-8f) return false;
  // z row = a * w row + (0, 0, 0, b): z_ndc = a + b / view depth.
  const float a = (z[0] * w[0] + z[1] * w[1] + z[2] * w[2]) / wn2;
  const float b = z[3] - a * w[3];
  if (b == 0.0f) return false;
  const float ex = z[0] - a * w[0], ey = z[1] - a * w[1], ez = z[2] - a * w[2];
  const float scale = std::max(std::abs(a), 1e-4f) * std::sqrt(wn2);
  if (ex * ex + ey * ey + ez * ez > 1e-6f * scale * scale) return false;
  const float near_plane = b / (1.0f - a), far_plane = -b / a;
  if (!(near_plane > 0.0f && near_plane <= 1.0f && far_plane >= 100.0f)) return false;
  const float fy = std::sqrt((y[0] * y[0] + y[1] * y[1] + y[2] * y[2]) / wn2);
  const float fx = std::sqrt((x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) / wn2);
  const float fov_y = 2.0f * std::atan(1.0f / fy);
  if (!(fov_y > 0.15f && fov_y < 2.6f && fy / fx > 1.2f && fy / fx < 2.6f)) return false;
  out = {near_plane, far_plane, fov_y};
  return true;
}

}  // namespace pgr4::native
