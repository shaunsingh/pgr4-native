// pgr4-native PGR4 native renderer: D3D12 device, frame command lists, swapchain and the debug
// window.
#include "d3d12_device.h"

#include "clock.h"
#include "gpu_timer.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <simde/x86/sse.h>

#include <algorithm>
#include <condition_variable>
#include <cwchar>
#include <mutex>

REXCVAR_DEFINE_BOOL(pgr4_native_gpu_serialize, false, "PGR4 native",
                    "Debug: wait for the GPU at the end of every frame, so no frame is in flight "
                    "while the next one is recorded (slow)");

namespace pgr4::native {

namespace {
constexpr UINT_PTR kTitleTimer = 1;
const wchar_t kClassName[] = L"pgr4_native_debug";
}  // namespace

bool Failed(HRESULT hr, const char* what) {
  if (SUCCEEDED(hr)) return false;
  REXLOG_ERROR("PGR4 native: {} failed: HRESULT {:08X}", what, uint32_t(hr));
  return true;
}

bool CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE type, uint64_t size,
                  D3D12_RESOURCE_STATES state, const char* what, ComPtr<ID3D12Resource>* out) {
  const D3D12_HEAP_PROPERTIES heap = {type};
  const D3D12_RESOURCE_DESC desc = BufferDesc(size);
  return !Failed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                 IID_PPV_ARGS(out->ReleaseAndGetAddressOf())),
                 what);
}

pD3DCompile D3DCompiler() {
  static const pD3DCompile compile = [] {
    const HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto fn =
        dll ? reinterpret_cast<pD3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    if (!fn) REXLOG_ERROR("PGR4 native: d3dcompiler_47.dll / D3DCompile not found");
    return fn;
  }();
  return compile;
}

D3D12Device::~D3D12Device() {
  WaitIdle();
  if (HWND h = hwnd_.load()) PostMessageW(h, WM_CLOSE, 1, 0);
  if (window_thread_.joinable()) window_thread_.join();
}

LRESULT CALLBACK D3D12Device::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<D3D12Device*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;  // the swapchain covers the client area
    case WM_TIMER:
      if (self && wp == kTitleTimer) {
        wchar_t title[96];
        std::swprintf(title, 96, L"PGR4 native (debug) - frame %llu",
                      (unsigned long long)self->title_frame_.load(std::memory_order_relaxed));
        SetWindowTextW(hwnd, title);
      }
      return 0;
    case WM_CLOSE:
      // The window lives as long as the renderer: the user's close button is ignored, the
      // destructor's WM_CLOSE (wp == 1) destroys it.
      if (wp == 1) DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

bool D3D12Device::CreateWindowThread(const wchar_t* title) {
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  window_thread_ = std::thread([&, title] {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW (no UNICODE here)
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    RECT r = {0, 0, LONG(width_), LONG(height_)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    // The debug window is owned by a hidden window, so it never becomes the process's "main
    // window" (.NET MainWindowHandle: unowned and visible), which the input scripts
    // (instrument/keys.ps1, run/input-player.ps1) focus and send keys to. It never activates
    // either: a window manager that focuses the newest window (gamescope) would otherwise hand
    // it the game's keyboard input (Wine activates a window on WM_TAKE_FOCUS unless it is
    // WS_EX_NOACTIVATE).
    HWND owner =
        CreateWindowExW(0, kClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_NOACTIVATE, kClassName, title,
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                r.bottom - r.top, owner, nullptr, inst, nullptr);
    if (hwnd) {
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      SetTimer(hwnd, kTitleTimer, 500, nullptr);
    }
    {
      // Notified under the lock: once it sees done, the waiter returns and destroys cv.
      std::lock_guard lock(m);
      hwnd_.store(hwnd);
      done = true;
      cv.notify_one();
    }
    if (!hwnd) return;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    hwnd_.store(nullptr);
  });
  std::unique_lock lock(m);
  cv.wait(lock, [&] { return done; });
  if (!hwnd_.load()) {
    REXLOG_ERROR("PGR4 native: CreateWindowExW failed ({})", GetLastError());
    return false;
  }
  return true;
}

bool D3D12Device::Initialize(uint32_t width, uint32_t height, const wchar_t* title,
                             HWND game_window) {
  width_ = width;
  height_ = height;
  game_window_ = game_window;
  if (Failed(CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf())),
             "CreateDXGIFactory2"))
    return false;
  ComPtr<IDXGIAdapter1> adapter;
  if (Failed(factory_->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                  IID_PPV_ARGS(adapter.GetAddressOf())),
             "EnumAdapterByGpuPreference"))
    return false;
  if (Failed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                               IID_PPV_ARGS(device_.ReleaseAndGetAddressOf())),
             "D3D12CreateDevice"))
    return false;
  DXGI_ADAPTER_DESC1 ad = {};
  adapter->GetDesc1(&ad);
  char name[128] = {};
  WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);

  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (Failed(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(queue_.GetAddressOf())),
             "CreateCommandQueue"))
    return false;
  queue_->SetName(L"pgr4 native queue");
  if (Failed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf())),
             "CreateFence") ||
      Failed(
          device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(idle_fence_.GetAddressOf())),
          "CreateFence(idle)"))
    return false;
  fence_event_ = CreateAutoResetEvent();
  idle_event_ = CreateAutoResetEvent();
  if (!fence_event_ || !idle_event_) {
    REXLOG_ERROR("PGR4 native: CreateEvent failed: error {}", GetLastError());
    return false;
  }
  for (auto& a : allocators_)
    if (Failed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(a.GetAddressOf())),
               "CreateCommandAllocator"))
      return false;
  if (Failed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(),
                                        nullptr, IID_PPV_ARGS(list_.GetAddressOf())),
             "CreateCommandList"))
    return false;
  list_->SetName(L"pgr4 native frame");

  BOOL tearing = FALSE;
  factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
  tearing_ = tearing != FALSE;
  swapchain_flags_ = (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) |
                     (latency_waitable_ ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0);
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = kBackBuffers;
  if (Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(rtv_heap_.GetAddressOf())),
             "CreateDescriptorHeap(RTV)"))
    return false;
  rtv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  if (!game_window_ && !(CreateWindowThread(title) && CreateSwapchain(hwnd_.load()))) return false;
  REXLOG_INFO("PGR4 native: D3D12 on {} (feature level 11_0+), presenting into the {}, tearing {}",
              name, game_window_ ? "game window" : "debug window", tearing_ ? "on" : "off");
  return true;
}

bool D3D12Device::AttachSwapchain() {
  RECT r = {};
  GetClientRect(game_window_, &r);
  width_ = uint32_t((std::max)(LONG(64), r.right - r.left));
  height_ = uint32_t((std::max)(LONG(64), r.bottom - r.top));
  if (!CreateSwapchain(game_window_)) return false;
  REXLOG_INFO("PGR4 native: swapchain {}x{} in the game window", width_, height_);
  return true;
}

void D3D12Device::DetachSwapchain() {
  WaitIdle();
  for (auto& b : back_buffers_) b.Reset();
  swapchain_.Reset();
  REXLOG_INFO("PGR4 native: swapchain released");
}

bool D3D12Device::CreateSwapchain(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = width_;
  sd.Height = height_;
  sd.Format = kBackBufferFormat;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = kBackBuffers;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  sd.Flags = swapchain_flags_;
  ComPtr<IDXGISwapChain1> sc1;
  if (Failed(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr,
                                              sc1.GetAddressOf()),
             "CreateSwapChainForHwnd"))
    return false;
  // Fullscreen is the window's own (SDL's in the game window).
  factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  if (Failed(sc1.As(&swapchain_), "IDXGISwapChain3")) return false;
  return GetBackBuffers();
}

HANDLE D3D12Device::TakeFrameLatencyWaitable(uint32_t max_latency) {
  if (!latency_waitable_) return nullptr;
  // The count must hold every release the waiter has not taken yet (DXGI bounds it by this).
  if (Failed(swapchain_->SetMaximumFrameLatency(max_latency), "SetMaximumFrameLatency"))
    return nullptr;
  return swapchain_->GetFrameLatencyWaitableObject();
}

bool D3D12Device::GetBackBuffers() {
  for (uint32_t i = 0; i < kBackBuffers; ++i) {
    if (Failed(swapchain_->GetBuffer(i, IID_PPV_ARGS(back_buffers_[i].ReleaseAndGetAddressOf())),
               "GetBuffer"))
      return false;
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(i) * rtv_stride_;
    device_->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, h);
  }
  back_index_ = swapchain_->GetCurrentBackBufferIndex();
  return true;
}

// The game window's client size changed (window resize, fullscreen toggle): the swapchain
// buffers follow; a minimized window (0x0) is skipped.
bool D3D12Device::FollowWindowSize() {
  if (!game_window_ || !swapchain_) return true;
  RECT r = {};
  if (!GetClientRect(game_window_, &r)) return true;
  const uint32_t w = uint32_t(r.right - r.left), h = uint32_t(r.bottom - r.top);
  if (w < 64 || h < 64 || (w == width_ && h == height_)) return true;
  WaitIdle();
  for (auto& b : back_buffers_) b.Reset();
  if (Failed(swapchain_->ResizeBuffers(kBackBuffers, w, h, kBackBufferFormat, swapchain_flags_),
             "ResizeBuffers"))
    return false;
  REXLOG_INFO("PGR4 native: game window client {}x{} -> {}x{}", width_, height_, w, h);
  width_ = w;
  height_ = h;
  return GetBackBuffers();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::back_buffer_rtv() const {
  D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  h.ptr += size_t(back_index_) * rtv_stride_;
  return h;
}

void D3D12Device::WaitFence(uint64_t value) {
  if (fence_->GetCompletedValue() >= value) return;
  fence_->SetEventOnCompletion(value, fence_event_.get());
  WaitForSingleObject(fence_event_.get(), INFINITE);
}

void D3D12Device::WaitIdle() {
  // A fence of its own: the frame fence's next value belongs to the frame still being recorded,
  // whose resources would count as done before it is submitted.
  if (!queue_ || !idle_fence_ || !idle_event_) return;
  const uint64_t v = ++idle_fence_value_;
  if (FAILED(queue_->Signal(idle_fence_.Get(), v)) || idle_fence_->GetCompletedValue() >= v) return;
  idle_fence_->SetEventOnCompletion(v, idle_event_.get());
  WaitForSingleObject(idle_event_.get(), INFINITE);
}

bool D3D12Device::EnableGpuTimer() {
  auto timer = std::make_unique<GpuTimer>();
  if (!timer->Initialize(device_.Get(), queue_.Get(), kFramesInFlight)) return false;
  timer->BeginFrame(list_.Get(), slot_);
  gpu_timer_ = std::move(timer);
  return true;
}

bool D3D12Device::OpenList() {
  const uint64_t t0 = NowNs();
  WaitFence(slot_fence_[slot_]);
  end_frame_times_.fence_ns = NowNs() - t0;
  if (Failed(allocators_[slot_]->Reset(), "CommandAllocator::Reset") ||
      Failed(list_->Reset(allocators_[slot_].Get(), nullptr), "CommandList::Reset"))
    return false;
  if (gpu_timer_) gpu_timer_->BeginFrame(list_.Get(), slot_);
  return true;
}

bool D3D12Device::CloseAndExecute() {
  if (Failed(list_->Close(), "CommandList::Close")) return false;
  // The constant uploads are streaming stores, which are weakly ordered: make them visible
  // before the GPU can read them.
  simde_mm_sfence();
  ID3D12CommandList* lists[] = {list_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  return true;
}

bool D3D12Device::Submit() {
  if (gpu_timer_) gpu_timer_->Pass(list_.Get(), GpuTimer::KeySubmit());
  // A list may be reset while its submission runs; the allocator only once the frame's fence
  // completed (OpenList).
  return CloseAndExecute() &&
         !Failed(list_->Reset(allocators_[slot_].Get(), nullptr), "CommandList::Reset");
}

bool D3D12Device::EndFrame(bool present) {
  const uint64_t t0 = NowNs();
  if (gpu_timer_) gpu_timer_->EndFrame(list_.Get());
  if (!CloseAndExecute()) return false;
  const uint64_t t1 = NowNs();
  const bool shown = swapchain_ && present;
  HRESULT hr = !shown   ? S_OK
               : vsync_ ? swapchain_->Present(1, 0)
                        : swapchain_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
  end_frame_times_.submit_ns = t1 - t0;
  end_frame_times_.present_ns = NowNs() - t1;
  if (Failed(hr, "Present")) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
      Failed(device_->GetDeviceRemovedReason(), "device removed, reason");
    return false;
  }
  end_frame_times_.presented = shown;
  end_frame_times_.presents += end_frame_times_.presented;
  uint64_t v = next_fence_++;
  if (Failed(queue_->Signal(fence_.Get(), v), "CommandQueue::Signal")) return false;
  slot_fence_[slot_] = v;
  if (REXCVAR_GET(pgr4_native_gpu_serialize)) WaitFence(v);
  slot_ = (slot_ + 1) % kFramesInFlight;
  if (swapchain_) back_index_ = swapchain_->GetCurrentBackBufferIndex();
  if (!FollowWindowSize()) return false;
  return OpenList();
}

}  // namespace pgr4::native
