// pgr4-native PGR4 native renderer: the app's own D3D12 queue, fence, frame command lists and
// swapchain, separate from ReXGlue's presenter and the Xenos plugin.
//
// Two window modes: the debug window (a top-level window of its own beside the game's), or, with
// --pgr4_native_primary, the game window itself (the SDK's). There the swapchain exists only
// between AttachSwapchain and DetachSwapchain, while the renderer owns the window; the rest of
// the time ReXGlue's presenter has its swapchain there (renderer.h, OwnGameWindow). The game
// window and not a child window over it: gamescope's WSI layer flips only the swapchain of a
// top-level window with no child window above it, and sends any other through Xwayland's copy,
// which has neither FIFO nor the frame limiter and drops all but the newest frame per refresh.
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
#include <span>
#include <thread>
#include <vector>

#include "command_stream.h"
#include "win_handle.h"

namespace pgr4::native {

using Microsoft::WRL::ComPtr;

class GpuTimer;

class D3D12Device {
 public:
  static constexpr uint32_t kFramesInFlight = 2;
  static constexpr uint32_t kBackBuffers = 3;
  static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  // With HDR output (--pgr4_native_hdr): HDR10, BT.2020 primaries and the PQ curve.
  static constexpr DXGI_FORMAT kHdrBackBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

  ~D3D12Device();

  // Creates the device objects and opens the first frame's command list; without
  // `game_window`, also the debug window (on its own message thread) and its swapchain. With it,
  // the frames are presented there, at its client size, once AttachSwapchain created the
  // swapchain (width/height are ignored). Logs and returns false on failure.
  bool Initialize(uint32_t width, uint32_t height, const wchar_t* title,
                  HWND game_window = nullptr);

  // Present with sync interval 1 (vsync) instead of 0 (tearing when supported).
  void set_vsync(bool vsync) { vsync_ = vsync; }
  // With vsync, between frames: each frame without a generated one is shown for `refreshes`
  // display refreshes (1 to kMaxPresentInterval): it draws into an image of the device's, which
  // EndFrame presents that many times, FIFO. DXVK presents a SyncInterval above 1 the same way;
  // vkd3d-proton honours one only through VK_EXT_present_timing. Logs and returns false when
  // the image cannot be created (each frame is then shown once).
  bool set_present_interval(uint32_t refreshes);
  uint32_t present_interval() const { return vsync_ ? present_interval_ : 1; }
  static constexpr uint32_t kMaxPresentInterval = 3;
  // The most presents a frame takes: its interval, or 2 with a generated frame.
  static constexpr uint32_t kMaxFramePresents = kMaxPresentInterval > 2 ? kMaxPresentInterval : 2;
  // Before Initialize: every swapchain gets a frame latency waitable object (frame_pacer.h).
  void set_frame_latency_waitable(bool on) { latency_waitable_ = on; }
  // The current swapchain's frame latency waitable object, with room for `max_latency` presents
  // in flight; the caller owns it. Take it before the swapchain's first Present. nullptr without
  // set_frame_latency_waitable, or on failure (logged).
  HANDLE TakeFrameLatencyWaitable(uint32_t max_latency);

  // Game window mode, between frames (no back buffer use recorded in the open list):
  // AttachSwapchain creates the swapchain at the window's client size; no other swapchain may
  // exist on the window. Logs and returns false on failure. DetachSwapchain waits for the GPU
  // and releases it. Frames recorded without a swapchain are submitted but not presented.
  bool AttachSwapchain();
  void DetachSwapchain();
  bool has_swapchain() const { return swapchain_ != nullptr; }

  ID3D12Device* device() const { return device_.Get(); }
  ID3D12CommandQueue* command_queue() const { return queue_.Get(); }
  // The frame's command list, recorded on the stream's thread (command_stream.h). The queue takes
  // no work but the stream's from the thread recording: what it submits runs after the calls
  // recorded before it, once the stream has made them.
  CommandStream* command_list() { return &stream_; }

  // The swapchain's current back buffer (state PRESENT outside EndFrame) and its RTV, or the
  // frame's image (SetFrameImage, or the present interval's).
  ID3D12Resource* back_buffer() const {
    return frame_image_ ? frame_image_
           : held()     ? held_image_.Get()
                        : back_buffers_[back_index_].Get();
  }
  D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtv() const;
  // Frame generation (frame_generation.h), for the frame being recorded. SetFrameImage, before
  // the frame draws into the back buffer: until EndFrame, back_buffer() is `image` (the back
  // buffers' size and format, PRESENT state outside EndFrame), which EndFrame copies into the back
  // buffer. SetGeneratedFrame, before EndFrame: EndFrame presents `generated` (the same kind of
  // image) before the frame, so the frame takes two presents.
  void SetFrameImage(ID3D12Resource* image, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    frame_image_ = image;
    frame_image_rtv_ = rtv;
  }
  void SetGeneratedFrame(ID3D12Resource* generated) { generated_frame_ = generated; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  // The swapchain presents HDR10 (kHdrBackBufferFormat): --pgr4_native_hdr is on and the display
  // is in HDR mode (Windows' HDR setting; under Proton, gamescope with HDR on and DXVK_HDR=1, as
  // in Steam's Game Mode).
  bool hdr() const { return hdr_; }
  // With hdr(): the luminance, in nits, that the presented image peaks at (the HDR10 metadata).
  float hdr_peak_nits() const { return hdr_peak_nits_; }

  // Fence value EndFrame signals for the frame being recorded; last value the GPU reached. The
  // stream's thread signals it after the frame's calls, so a wait (WaitFence) flushes the stream.
  uint64_t frame_fence() const { return next_fence_; }
  uint64_t completed_fence() const { return fence_->GetCompletedValue(); }
  ID3D12Fence* fence() const { return fence_.Get(); }
  // The frame being recorded's slot in [0, kFramesInFlight): its previous frame has completed,
  // so per-frame upload memory indexed by it is free to write.
  uint32_t frame_slot() const { return slot_; }
  void WaitFence(uint64_t value);

  // Closes and submits the frame's command list (the caller recorded into the back buffer;
  // EndFrame adds no barriers but the frame image's copy), presents (unless `present` is false or
  // there is no swapchain: the window keeps the previous frame; after the generated frame, if
  // any), signals the frame fence and opens the next frame's list, all on the stream's thread
  // (FinishFrame); then follows the game window's client size and waits for the slot
  // kFramesInFlight frames back.
  // `first`: closed lists that run ahead of the frame's in the same ExecuteCommandLists, for
  // work that would otherwise split its render passes (TextureCache::CloseUploadLists); the
  // frame's fence covers them, GpuTimer passes do not.
  // Returns false once the device or swapchain failed (logged once; on the stream's thread, seen
  // at the next EndFrame or Submit); the caller stops.
  bool EndFrame(bool present = true, std::span<ID3D12CommandList* const> first = {});
  // Submits what the frame recorded so far (after `first`, as in EndFrame) and continues it in
  // the same list, reset on the frame's allocator, so the GPU starts on it while the rest of the
  // frame is recorded. The frame fence and slot stay the frame's (EndFrame signals once for all
  // of its submissions). Nothing bound on the list carries over. Returns false on failure
  // (logged).
  bool Submit(std::span<ID3D12CommandList* const> first = {});
  // Where the last EndFrame spent its time.
  struct EndFrameTimes {
    // On the stream's thread, for the last frame it finished.
    uint64_t submit_ns = 0;   // Close + ExecuteCommandLists
    uint64_t present_ns = 0;  // Present
    uint64_t fence_ns = 0;    // waiting for the GPU to finish the slot's last frame
    // The stream's thread since the last EndFrame: its time making calls (submits and presents
    // included), and the part of it waiting for the GPU to free the next frame's allocator.
    uint64_t stream_ns = 0, stream_wait_ns = 0;
    bool presented = false;       // Present ran (a swapchain and a complete frame)
    uint32_t frame_presents = 0;  // the frame's Presents: its interval, or 2 with a generated frame
    uint64_t presents = 0;        // Presents so far, the frame's included
  };
  const EndFrameTimes& end_frame_times() const { return end_frame_times_; }

  // Shows a frame counter in the debug window's title (a 500 ms timer on the window thread
  // reads it).
  void SetTitleFrame(uint64_t frame) { title_frame_.store(frame, std::memory_order_relaxed); }

  // Waits for the GPU to finish everything submitted so far, mid-frame Submits included (and the
  // stream to submit what it holds); the frame being recorded keeps its frame_fence.
  void WaitIdle();

  // The GPU's time on the last frame it completed (kFramesInFlight frames back): the time from
  // the start to the end of each of the frame's submissions, summed, so not the time it waited
  // for the CPU between them; in ns, 0 until one completed (or without timestamps).
  uint64_t gpu_busy_ns() const { return gpu_busy_ns_; }

  // Timestamps per pass in every frame from now on (gpu_timer.h), starting with the open list.
  // Logs and returns false on failure.
  bool EnableGpuTimer();
  // nullptr unless EnableGpuTimer succeeded.
  GpuTimer* gpu_timer() const { return gpu_timer_.get(); }

 private:
  bool CreateWindowThread(const wchar_t* title);
  bool CreateSwapchain(HWND hwnd);
  // Switches the new swapchain to HDR10 if wanted and supported, else to kBackBufferFormat.
  bool SetUpColorSpace();
  using List = ID3D12GraphicsCommandList;
  // The stream's thread: closes `list` and submits it after `first`.
  bool ExecuteLists(List* list, std::span<ID3D12CommandList* const> first);
  // What EndFrame leaves to the stream's thread.
  struct FrameEnd {
    std::vector<ID3D12CommandList*> first;
    ID3D12Resource* image = nullptr;      // copied into the back buffers, or null
    ID3D12Resource* generated = nullptr;  // presented before it, or null
    uint32_t presents = 0;
    uint32_t drawn = 0;  // the back buffer the frame drew into (without an image)
    uint64_t fence = 0;
    uint32_t slot = 0, next_slot = 0;
    uint64_t next_slot_fence = 0;  // the next slot's last frame's
  };
  void FinishFrame(List* list, const FrameEnd& f);
  // gpu_busy_ns: the timestamps around the frame's submissions. False (logged) on failure.
  bool CreateBusyTimer();
  void StampBusy();
  void ReadBusy();
  bool GetBackBuffers();
  bool FollowWindowSize();
  // The stream's thread: the copy of `image` (PRESENT state) into the current back buffer.
  void CopyToBackBuffer(List* list, ID3D12Resource* image);
  // The frame being recorded draws into held_image_ (set_present_interval).
  bool held() const { return vsync_ && present_interval_ > 1 && held_image_; }
  // held_image_ like the back buffers, when the present interval wants it; false (logged) on
  // failure.
  bool CreateHeldImage();
  // The stream's thread: presents the current back buffer; false (logged) on failure.
  bool Present();
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

  uint32_t width_ = 0, height_ = 0;
  ComPtr<IDXGIFactory6> factory_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12Fence> fence_;
  UniqueHandle fence_event_;
  uint64_t next_fence_ = 1;
  ComPtr<ID3D12Fence> idle_fence_;  // WaitIdle's
  UniqueHandle idle_event_;
  uint64_t idle_fence_value_ = 0;
  ComPtr<ID3D12CommandAllocator> allocators_[kFramesInFlight];
  uint64_t slot_fence_[kFramesInFlight] = {};
  uint32_t slot_ = 0;
  ComPtr<ID3D12GraphicsCommandList> list_;
  // Records list_ (after it: stopped first).
  CommandStream stream_;
  // The stream's thread's.
  std::vector<ID3D12CommandList*> submit_lists_;  // ExecuteLists'
  UniqueHandle stream_event_;
  std::atomic<bool> stream_failed_{false};
  std::atomic<uint64_t> submit_ns_{0}, present_ns_{0}, stream_wait_ns_{0};
  EndFrameTimes end_frame_times_;

  ComPtr<IDXGISwapChain3> swapchain_;
  ComPtr<ID3D12Resource> back_buffers_[kBackBuffers];
  ComPtr<ID3D12DescriptorHeap> rtv_heap_;
  uint32_t rtv_stride_ = 0;
  uint32_t back_index_ = 0;                // the back buffer the frame being recorded draws into
  ID3D12Resource* frame_image_ = nullptr;  // SetFrameImage's, until EndFrame
  D3D12_CPU_DESCRIPTOR_HANDLE frame_image_rtv_ = {};
  ID3D12Resource* generated_frame_ = nullptr;  // SetGeneratedFrame's, until EndFrame
  uint32_t present_interval_ = 1;
  ComPtr<ID3D12Resource> held_image_;  // the back buffers' size and format; its RTV after theirs
  UINT swapchain_flags_ = 0;           // DXGI_SWAP_CHAIN_FLAG_*: creation and every ResizeBuffers
  bool tearing_ = false;
  bool vsync_ = false;
  bool hdr_ = false;
  float hdr_peak_nits_ = 0.0f;
  bool latency_waitable_ = false;
  HWND game_window_ = nullptr;

  std::unique_ptr<GpuTimer> gpu_timer_;

  // Two timestamps per submission, up to kBusySpans submissions per frame (later ones join the
  // last), per slot.
  static constexpr uint32_t kBusySpans = 16;
  ComPtr<ID3D12QueryHeap> busy_heap_;
  ComPtr<ID3D12Resource> busy_readback_;
  uint32_t busy_stamps_[kFramesInFlight] = {};
  double busy_ns_per_tick_ = 0.0;
  uint64_t gpu_busy_ns_ = 0;

  // The debug window and its message thread.
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
