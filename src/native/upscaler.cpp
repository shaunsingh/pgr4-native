// pgr4-native PGR4 native renderer: FidelityFX Super Resolution of the scene. See upscaler.h.
#include "upscaler.h"

#include "clock.h"
#include "log.h"

#include <ffx_api.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <string>

REXCVAR_DEFINE_BOOL(pgr4_native_upscaler, false, "PGR4 native",
                    "Render the 3D scene without MSAA and antialias it with AMD FidelityFX Super "
                    "Resolution (amd_fidelityfx_loader_dx12.dll and "
                    "amd_fidelityfx_upscaler_dx12.dll beside the executable, or what OptiScaler "
                    "puts behind them) from the start; bind_upscaler (F7) switches it");
REXCVAR_DEFINE_INT32(pgr4_native_upscaler_width, 1280, "PGR4 native",
                     "Width the upscaler's scene renders at, upscaled to 1280 (320-1280; 1280 "
                     "with the height at 720: antialiasing only)");
REXCVAR_DEFINE_INT32(pgr4_native_upscaler_height, 720, "PGR4 native",
                     "Height the upscaler's scene renders at, upscaled to 720 (180-720)");
REXCVAR_DEFINE_INT32(pgr4_native_upscaler_target_fps, 0, "PGR4 native",
                     "Vary the upscaler's render size each frame, from the width and height "
                     "down to --pgr4_native_upscaler_min_height, so that the GPU keeps up with "
                     "this frame rate (0: a fixed size)");
REXCVAR_DEFINE_INT32(pgr4_native_upscaler_min_height, 360, "PGR4 native",
                     "With --pgr4_native_upscaler_target_fps: the least height the scene renders "
                     "at (180-720)");

namespace pgr4::native {

struct Upscaler::Api {
  PfnFfxCreateContext create = nullptr;
  PfnFfxDestroyContext destroy = nullptr;
  PfnFfxQuery query = nullptr;
  PfnFfxDispatch dispatch = nullptr;
};

namespace {

constexpr wchar_t kLoaderDll[] = L"amd_fidelityfx_loader_dx12.dll";
constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr D3D12_RESOURCE_STATES kInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// The camera's planes and vertical field of view are not known to the renderer. FSR uses them
// only to scale its disocclusion threshold with view-space depth; these are a chase camera's.
constexpr float kCameraNear = 0.1f, kCameraFar = 10000.0f, kCameraFovY = 1.0f;

void Message(uint32_t type, const wchar_t* message) {
  std::string s;
  const int n = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
  if (n > 1) {
    s.resize(size_t(n));
    WideCharToMultiByte(CP_UTF8, 0, message, -1, s.data(), n, nullptr, nullptr);
    s.resize(size_t(n - 1));
  }
  if (type == FFX_API_MESSAGE_TYPE_ERROR)
    PGR4_LOG_FIRST(16, ERROR, "PGR4 native upscaler: {}", s);
  else
    PGR4_LOG_FIRST(16, WARN, "PGR4 native upscaler: {}", s);
}

// The scene's alpha into the upscaled color, which FSR leaves without it (the motion blur pass,
// pixel shader 27A3898925DDCAF7, masks the player's car by it): nearest to each output pixel.
constexpr char kAlphaHlsl[] = R"(
Texture2D<float4> scene : register(t0);
RWTexture2D<float4> output : register(u0);
cbuffer Constants : register(b0) { float2 scale; uint2 size; };
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  float4 c = output[p];
  c.a = scene.Load(int3(uint2((float2(p) + 0.5) * scale), 0)).a;
  output[p] = c;
}
)";
struct AlphaConstants {
  float scale[2];
  uint32_t size[2];
};
constexpr uint32_t kAlphaViews = 2;  // per frame in flight: the scene's SRV, the output's UAV

// The render size of one axis from its setting, at least a quarter of the scene's.
uint32_t RenderExtent(int32_t setting, uint32_t scene) {
  return uint32_t(std::clamp<int32_t>(setting, int32_t(scene / 4), int32_t(scene)));
}

}  // namespace

bool Upscaler::Requested() { return REXCVAR_GET(pgr4_native_upscaler); }

const Upscaler::Api* Upscaler::LoadApi() {
  static const Api api = [] {
    Api a;
    HMODULE dll = LoadLibraryW(kLoaderDll);
    if (!dll) {
      REXLOG_ERROR("PGR4 native upscaler: cannot load amd_fidelityfx_loader_dx12.dll ({})",
                   uint32_t(GetLastError()));
      return a;
    }
    a.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(dll, "ffxCreateContext"));
    a.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(dll, "ffxDestroyContext"));
    a.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(dll, "ffxQuery"));
    a.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(dll, "ffxDispatch"));
    if (!a.create || !a.destroy || !a.query || !a.dispatch) {
      REXLOG_ERROR("PGR4 native upscaler: amd_fidelityfx_loader_dx12.dll lacks the ffx API");
      a = {};
    }
    return a;
  }();
  return api.create ? &api : nullptr;
}

Upscaler::~Upscaler() {
  if (context_) api_->destroy(&context_, nullptr);
}

bool Upscaler::Initialize(ID3D12Device* device) {
  device_ = device;
  max_width_ = RenderExtent(REXCVAR_GET(pgr4_native_upscaler_width), kSceneWidth);
  max_height_ = RenderExtent(REXCVAR_GET(pgr4_native_upscaler_height), kSceneHeight);
  if (const int32_t fps = REXCVAR_GET(pgr4_native_upscaler_target_fps); fps > 0) {
    const uint32_t min_height =
        RenderExtent(REXCVAR_GET(pgr4_native_upscaler_min_height), kSceneHeight);
    const uint32_t min_level =
        (min_height * DynamicResolution::kSteps + max_height_ - 1) / max_height_;
    dynamic_.emplace(min_level, uint64_t(1'000'000'000 / fps));
  }
  api_ = LoadApi();
  if (!api_ || !CreateContext(false)) return false;
  ffxQueryGetProviderVersion provider = {};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  if (api_->query(&context_, &provider.header) == FFX_API_RETURN_OK && provider.versionName)
    REXLOG_INFO("PGR4 native upscaler: {} from {}x{}{} to {}x{}", provider.versionName, max_width_,
                max_height_, dynamic_ ? " or less" : "", kSceneWidth, kSceneHeight);
  return SetLevel(DynamicResolution::kSteps) && CreateTargets() && CreateAlphaPass();
}

bool Upscaler::CreateAlphaPass() {
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {kOutputFormat};
  if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) ||
      !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD)) {
    REXLOG_WARN(
        "PGR4 native upscaler: no typed UAV loads of the output; the motion blur loses "
        "its mask");
    return true;
  }
  const pD3DCompile compile = D3DCompiler();
  ComPtr<ID3DBlob> code, err;
  if (!compile ||
      FAILED(compile(kAlphaHlsl, sizeof(kAlphaHlsl) - 1, "pgr4_upscaler_alpha.hlsl", nullptr,
                     nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
    REXLOG_ERROR("PGR4 native upscaler: alpha shader: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  }
  const D3D12_DESCRIPTOR_RANGE ranges[2] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1},
  };
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {2, ranges};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, sizeof(AlphaConstants) / 4};
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob;
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
             "upscaler alpha root signature serialize") ||
      Failed(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&alpha_signature_)),
             "upscaler alpha root signature"))
    return false;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = alpha_signature_.Get();
  pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  const D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         kAlphaViews * D3D12Device::kFramesInFlight,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&alpha_pso_)),
             "upscaler alpha pipeline") ||
      Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&alpha_heap_)), "upscaler alpha heap"))
    return false;
  alpha_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return true;
}

void Upscaler::CopyAlpha(ID3D12GraphicsCommandList* list, const RenderTarget& color) {
  if (!alpha_pso_) return;
  // The slot of a frame kFramesInFlight back, whose fence the device waited for.
  const uint32_t first = uint32_t(frame_ % D3D12Device::kFramesInFlight) * kAlphaViews;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = alpha_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(first) * alpha_stride_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = kOutputFormat;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(color.resource.Get(), &sd, cpu);
  cpu.ptr += alpha_stride_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = kOutputFormat;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(output_.resource.Get(), nullptr, &ud, cpu);
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = alpha_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(first) * alpha_stride_;

  // After the upscaler's writes to the output.
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  b.UAV.pResource = output_.resource.Get();
  list->ResourceBarrier(1, &b);
  const AlphaConstants c = {
      {float(render_width_) / float(kSceneWidth), float(render_height_) / float(kSceneHeight)},
      {kSceneWidth, kSceneHeight}};
  ID3D12DescriptorHeap* heaps[] = {alpha_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(alpha_signature_.Get());
  list->SetPipelineState(alpha_pso_.Get());
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  list->Dispatch((kSceneWidth + 7) / 8, (kSceneHeight + 7) / 8, 1);
}

bool Upscaler::SetLevel(uint32_t level) {
  constexpr uint32_t n = DynamicResolution::kSteps;
  render_width_ = std::max(1u, (max_width_ * level + n / 2) / n);
  render_height_ = std::max(1u, (max_height_ * level + n / 2) / n);
  // AMD's recommended texture LOD bias for the render size (FSR docs, "Mipmap biasing"),
  // in the sampler key's quarter steps.
  const float ratio = std::min(float(render_width_) / float(kSceneWidth),
                               float(render_height_) / float(kSceneHeight));
  mip_bias_quarters_ = uint32_t(std::lround(-4.0f * (std::log2(ratio) - 1.0f)));
  ffxQueryDescUpscaleGetJitterPhaseCount phases = {};
  phases.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
  phases.renderWidth = render_width_;
  phases.displayWidth = kSceneWidth;
  int32_t count = 0;
  phases.pOutPhaseCount = &count;
  if (api_->query(&context_, &phases.header) != FFX_API_RETURN_OK || count <= 0) {
    PGR4_WARN_ONCE("PGR4 native upscaler: no jitter phase count for {}x{}", render_width_,
                   render_height_);
    return false;
  }
  phase_count_ = count;
  return true;
}

bool Upscaler::CreateContext(bool debug) {
  if (context_) {
    api_->destroy(&context_, nullptr);
    context_ = nullptr;
  }
  ffxCreateBackendDX12Desc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.device = device_;
  ffxCreateContextDescUpscaleVersion version = {};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
  version.header.pNext = &backend.header;
  version.version = FFX_UPSCALER_VERSION;
  ffxCreateContextDescUpscale desc = {};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  desc.header.pNext = &version.header;
  // The scene is linear light in a float target, before the game's exposure and tone mapping,
  // and its depth is reversed (cleared to 0, tested GEQUAL).
  desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
               FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
               (dynamic_ ? FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION : 0u) |
               (debug ? FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION : 0u);
  desc.maxRenderSize = {max_width_, max_height_};
  desc.maxUpscaleSize = {kSceneWidth, kSceneHeight};
  desc.fpMessage = Message;
  if (const ffxReturnCode_t rc = api_->create(&context_, &desc.header, nullptr);
      rc != FFX_API_RETURN_OK) {
    REXLOG_ERROR("PGR4 native upscaler: ffxCreateContext failed ({}){}", rc,
                 debug ? " with debug visualization" : "");
    context_ = nullptr;
    return false;
  }
  debug_context_ = debug;
  reset_ = true;
  return true;
}

bool Upscaler::CreateTargets() {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = kSceneWidth;
  d.Height = kSceneHeight;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = kOutputFormat;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&output_.resource)),
             "upscaler output"))
    return false;
  output_.resource_format = output_.view_format = output_.srv_format = kOutputFormat;
  output_.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  output_.width = kSceneWidth;
  output_.height = kSceneHeight;

  d.Width = max_width_;
  d.Height = max_height_;
  d.Format = kMotionFormat;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE zero = {};
  zero.Format = kMotionFormat;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, motion_state_, &zero,
                                              IID_PPV_ARGS(&motion_)),
             "upscaler motion vectors"))
    return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 1;
  if (Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&motion_rtv_)), "upscaler RTV heap"))
    return false;
  device_->CreateRenderTargetView(motion_.Get(), nullptr,
                                  motion_rtv_->GetCPUDescriptorHandleForHeapStart());
  return true;
}

bool Upscaler::BeginFrame(uint64_t frame, uint64_t gpu_busy_ns) {
  frame_ = frame;
  bool resized = false;
  if (dynamic_) {
    const uint32_t level = dynamic_->level();
    if (dynamic_->Update(gpu_busy_ns) != level) {
      SetLevel(dynamic_->level());  // without a phase count, the jitter keeps the old size's
      resized = true;
    }
  }
  const uint64_t now = NowNs();
  frame_ms_ = last_ns_ ? float(double(now - last_ns_) * 1e-6) : 0.0f;
  last_ns_ = now;
  ffxQueryDescUpscaleGetJitterOffset q = {};
  q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
  q.index = int32_t(frame % uint64_t(phase_count_));
  q.phaseCount = phase_count_;
  q.pOutX = &jitter_[0];
  q.pOutY = &jitter_[1];
  if (api_->query(&context_, &q.header) != FFX_API_RETURN_OK) jitter_[0] = jitter_[1] = 0.0f;
  return resized;
}

bool Upscaler::SetDebugView(bool on) {
  if (on && !debug_context_ && !CreateContext(true)) return false;
  debug_view_ = on;
  return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE Upscaler::MotionTarget(ID3D12GraphicsCommandList* list) {
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = motion_rtv_->GetCPUDescriptorHandleForHeapStart();
  if (motion_state_ != D3D12_RESOURCE_STATE_RENDER_TARGET) {
    const D3D12_RESOURCE_BARRIER b =
        TransitionBarrier(motion_.Get(), motion_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &b);
    motion_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  if (motion_frame_ != frame_) {
    motion_frame_ = frame_;
    const float zero[4] = {};
    list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  }
  return rtv;
}

RenderTarget* Upscaler::Upscale(ID3D12GraphicsCommandList* list, RenderTargets& targets,
                                const SurfaceRegs& regs) {
  if (upscaled_frame_ == frame_) return upscaled_ ? &output_ : nullptr;
  upscaled_frame_ = frame_;
  upscaled_ = false;

  RenderTarget* color = targets.GetColor(regs, 0, kSceneHeight);
  RenderTarget* depth = targets.GetDepth(regs, kSceneHeight);
  if (!color || !depth || color->samples != 1 || depth->samples != 1 || color->scale != scale() ||
      depth->scale != scale() || color->srv_format != kOutputFormat) {
    PGR4_WARN_ONCE("PGR4 native upscaler: unexpected scene targets (surface {:08X} color {:08X})",
                   regs.surface_info, regs.color_info[0]);
    return nullptr;
  }
  const D3D12_RECT all = {0, 0, LONG(kSceneWidth), LONG(kSceneHeight)};
  targets.PrepareRead(list, color, all);
  targets.PrepareRead(list, depth, all);

  D3D12_RESOURCE_BARRIER bs[4];
  uint32_t nb = 0;
  auto to = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES* state, D3D12_RESOURCE_STATES after) {
    if (*state == after) return;
    bs[nb++] = TransitionBarrier(r, *state, after);
    *state = after;
  };
  MotionTarget(list);  // cleared if no draw wrote motion this frame
  to(color->resource.Get(), &color->state, kInputState);
  to(depth->resource.Get(), &depth->state, kInputState);
  to(motion_.Get(), &motion_state_, kInputState);
  to(output_.resource.Get(), &output_.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (nb) list->ResourceBarrier(nb, bs);

  ffxDispatchDescUpscale d = {};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  d.commandList = list;
  d.color = ffxApiGetResourceDX12(color->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.depth = ffxApiGetResourceDX12(depth->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.motionVectors = ffxApiGetResourceDX12(motion_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.output = ffxApiGetResourceDX12(output_.resource.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  d.jitterOffset = {jitter_[0], jitter_[1]};
  d.motionVectorScale = {float(render_width_), float(render_height_)};
  d.renderSize = {render_width_, render_height_};
  d.upscaleSize = {kSceneWidth, kSceneHeight};
  d.enableSharpening = false;
  if (debug_view_) d.flags = FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
  d.frameTimeDelta = frame_ms_;
  d.preExposure = 1.0f;
  d.reset = reset_;
  d.cameraNear = kCameraNear;
  d.cameraFar = kCameraFar;
  d.cameraFovAngleVertical = kCameraFovY;
  d.viewSpaceToMetersFactor = 1.0f;
  if (const ffxReturnCode_t rc = api_->dispatch(&context_, &d.header); rc != FFX_API_RETURN_OK) {
    PGR4_WARN_ONCE("PGR4 native upscaler: ffxDispatch failed ({})", rc);
    return nullptr;
  }
  CopyAlpha(list, *color);
  reset_ = false;
  upscaled_ = true;
  output_.guest_format = color->guest_format;
  return &output_;
}

}  // namespace pgr4::native
