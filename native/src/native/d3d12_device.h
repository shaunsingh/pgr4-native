// pgr4-native PGR4 native renderer: the app's own D3D12 queue, fence, frame command lists and a
// window with its own swapchain, separate from ReXGlue's presenter and the Xenos plugin.
//
// Two window modes: the debug window (a top-level window beside the game's), or, with
// --pgr4_native_primary, a disabled child window covering the game window's client area (the
// SDK's own window and swapchain stay underneath; keyboard focus and input stay with the game
// window, and the child follows its client size).
//
// D3D12 returns the same ID3D12Device for the same adapter within a process, so the device may
// be the one the Xenos plugin also uses; everything else here (queue, allocators, lists,
// fence, swapchain) is ours. A device removal caused by the native path would also take down
// the Xenos plugin. The D3D12 debug layer cannot be enabled here (the device usually exists
// already when the native path starts).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#undef DrawState  // winuser.h maps it to DrawStateA/W; d3d_state.h's struct has that name

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace pgr4::native {

using Microsoft::WRL::ComPtr;

class GpuTimer;

class D3D12Device {
 public:
  static constexpr uint32_t kFramesInFlight = 2;
  static constexpr uint32_t kBackBuffers = 3;
  static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

  ~D3D12Device();

  // Creates the device objects, the window (on its own message thread) and the swapchain, and
  // opens the first frame's command list. With `parent`, the window is a child of it, sized to
  // its client area (width/height are ignored). Logs and returns false on failure.
  bool Initialize(uint32_t width, uint32_t height, const wchar_t* title, HWND parent = nullptr);

  // Present with sync interval 1 (vsync) instead of 0 (tearing when supported).
  void set_vsync(bool vsync) { vsync_ = vsync; }

  // Child mode: hides the window (and skips presenting) while false, so the game window's own
  // content, such as the SDK's XAM dialogs or the Xenos plugin's frames, shows instead. No
  // effect on the debug window.
  void SetShown(bool shown);

  ID3D12Device* device() const { return device_.Get(); }
  ID3D12CommandQueue* queue() const { return queue_.Get(); }
  ID3D12GraphicsCommandList* command_list() const { return list_.Get(); }

  // The debug window's current back buffer (state PRESENT outside EndFrame) and its RTV.
  ID3D12Resource* back_buffer() const { return back_buffers_[back_index_].Get(); }
  D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtv() const;
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

  // Fence value EndFrame signals for the frame being recorded; last value the GPU reached.
  uint64_t frame_fence() const { return next_fence_; }
  uint64_t completed_fence() const { return fence_->GetCompletedValue(); }
  // The frame being recorded's slot in [0, kFramesInFlight): its previous frame has completed,
  // so per-frame upload memory indexed by it is free to write.
  uint32_t frame_slot() const { return slot_; }
  void WaitFence(uint64_t value);

  // Closes and submits the frame's command list (the caller recorded into the back buffer;
  // EndFrame adds no barriers), presents (unless `present` is false: the window keeps the
  // previous frame), signals the frame fence, follows the parent window's
  // client size (child mode), then waits for the slot kFramesInFlight frames back and opens the
  // next frame's list.
  // Returns false once the device or swapchain failed (logged once); the caller stops.
  bool EndFrame(bool present = true);
  // Submits what the frame recorded so far and continues it in the same list, reset on the
  // frame's allocator, so the GPU starts on it while the rest of the frame is recorded. The
  // frame fence and slot stay the frame's (EndFrame signals once for all of its submissions).
  // Nothing bound on the list carries over. Returns false on failure (logged).
  bool Submit();
  // Where the last EndFrame spent its time.
  struct EndFrameTimes {
    uint64_t submit_ns = 0;   // Close + ExecuteCommandLists
    uint64_t present_ns = 0;  // Present
    uint64_t fence_ns = 0;    // OpenList: waiting for the GPU to finish the slot's last frame
  };
  const EndFrameTimes& end_frame_times() const { return end_frame_times_; }

  // Shows a frame counter in the window title (posted to the window thread).
  void SetTitleFrame(uint64_t frame) { title_frame_.store(frame, std::memory_order_relaxed); }

  void WaitIdle();

  // Timestamps per pass in every frame from now on (gpu_timer.h), starting with the open list.
  // Logs and returns false on failure.
  bool EnableGpuTimer();
  // nullptr unless EnableGpuTimer succeeded.
  GpuTimer* gpu_timer() const { return gpu_timer_.get(); }

 private:
  bool CreateWindowThread(const wchar_t* title);
  bool OpenList();
  bool CloseAndExecute();
  bool GetBackBuffers();
  bool FollowParentSize();
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

  uint32_t width_ = 0, height_ = 0;
  ComPtr<IDXGIFactory6> factory_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12Fence> fence_;
  HANDLE fence_event_ = nullptr;
  uint64_t next_fence_ = 1;
  ComPtr<ID3D12CommandAllocator> allocators_[kFramesInFlight];
  uint64_t slot_fence_[kFramesInFlight] = {};
  uint32_t slot_ = 0;
  ComPtr<ID3D12GraphicsCommandList> list_;
  EndFrameTimes end_frame_times_;

  ComPtr<IDXGISwapChain3> swapchain_;
  ComPtr<ID3D12Resource> back_buffers_[kBackBuffers];
  ComPtr<ID3D12DescriptorHeap> rtv_heap_;
  uint32_t rtv_stride_ = 0;
  uint32_t back_index_ = 0;
  bool tearing_ = false;
  bool vsync_ = false;
  bool shown_ = true;
  HWND parent_ = nullptr;

  std::unique_ptr<GpuTimer> gpu_timer_;

  std::thread window_thread_;
  std::atomic<HWND> hwnd_{nullptr};
  std::atomic<uint64_t> title_frame_{0};
};

// Logs a failed HRESULT with the call text; returns true when it failed.
bool Failed(HRESULT hr, const char* what);

// A barrier moving every subresource of `resource` from `before` to `after`.
inline D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource,
                                                D3D12_RESOURCE_STATES before,
                                                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

// A buffer of `size` bytes.
constexpr D3D12_RESOURCE_DESC BufferDesc(uint64_t size,
                                         D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  return {D3D12_RESOURCE_DIMENSION_BUFFER, 0,    size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
          D3D12_TEXTURE_LAYOUT_ROW_MAJOR,  flags};
}

// A committed buffer of `size` bytes on a heap of `type`, created in `state`. Logs `what` and
// returns false on failure.
bool CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE type, uint64_t size,
                  D3D12_RESOURCE_STATES state, const char* what, ComPtr<ID3D12Resource>* out);

// D3DCompile from d3dcompiler_47.dll (Windows 10/11 and Proton ship it), loaded on the first call;
// nullptr (logged) without it.
pD3DCompile D3DCompiler();

}  // namespace pgr4::native
