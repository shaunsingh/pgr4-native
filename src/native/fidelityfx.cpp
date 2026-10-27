// pgr4-native PGR4 native renderer: the FidelityFX API loader. See fidelityfx.h.
#include "fidelityfx.h"

#include "d3d12_device.h"  // windows.h

#include <rex/logging.h>

namespace pgr4::native {

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

}  // namespace pgr4::native
