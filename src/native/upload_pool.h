// pgr4-native PGR4 native renderer: CPU-to-GPU upload memory in persistently mapped UPLOAD-heap
// blocks, sub-allocated linearly and recycled once the GPU finished the frame that used them.
// Used for vertex/index data and constants (draw.cpp) and texture uploads (textures.cpp).
//
// Threading: one thread per pool (with the render thread, draw.cpp's vertex pool is the guest
// thread's and the others are the render thread's; renderer.h).
#pragma once

#include "clock.h"
#include "d3d12_device.h"

#include <rex/logging.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace pgr4::native {

class UploadPool {
 public:
  struct Alloc {
    ID3D12Resource* resource;
    uint64_t offset;
    uint8_t* cpu;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
  };
  // block_size: the size of the recycled blocks; a larger request gets a block of its own,
  // released (not recycled) when its frame completes. cpu_cached: write-back CPU pages (a
  // CUSTOM heap in the L0 pool) instead of the UPLOAD heap's write-combined ones, for small
  // writes on the draw path that would otherwise wait on write-combining buffers.
  explicit UploadPool(uint64_t block_size, bool cpu_cached = false)
      : block_size_(block_size), cpu_cached_(cpu_cached) {}
  void SetDevice(ID3D12Device* d) { device_ = d; }
  // Keeps each frame's memory until the next frame completes too, so that a frame's draws can
  // read what the previous one uploaded (the motion vector pass's previous constants).
  void set_keep_previous_frame(bool keep) { keep_previous_frame_ = keep; }
  // Creates `blocks` recycled blocks now. A block created during play costs 2-16 ms (16 ms
  // for a 32 MB block at race start, while the driver also creates hundreds of textures), so
  // reserve as many as the race start keeps in flight.
  void Reserve(uint32_t blocks) {
    reserved_ += blocks;
    for (uint32_t i = 0; i < blocks; ++i) {
      std::unique_ptr<Block> b = CreateBlock(block_size_);
      if (!b) return;
      free_.push_back(std::move(b));
    }
  }

  // `fence`: the fence value that completes the frame the memory is used in.
  bool Allocate(uint64_t size, uint64_t align, uint64_t fence, Alloc* out) {
    if (!current_ || ((current_->used + align - 1) & ~(align - 1)) + size > current_->size) {
      if (!NewBlock(std::max(size, block_size_), fence)) return false;
    }
    Block& b = *current_;
    b.used = (b.used + align - 1) & ~(align - 1);
    out->resource = b.resource.Get();
    out->offset = b.used;
    out->cpu = b.cpu + b.used;
    out->gpu = b.gpu + b.used;
    b.used += size;
    b.fence = fence;
    frame_fence_ = fence;
    return true;
  }
  // After the frame's submission: blocks used so far wait for their fence (with
  // keep_previous_frame, the previous frame's wait for this frame's).
  void EndFrame(uint64_t completed) {
    for (auto& b : previous_) {
      b->fence = std::max(b->fence, frame_fence_);
      retired_.push_back(std::move(b));
    }
    previous_.clear();
    for (auto& b : active_) (keep_previous_frame_ ? previous_ : retired_).push_back(std::move(b));
    active_.clear();
    current_ = nullptr;
    for (size_t i = 0; i < retired_.size();) {
      if (retired_[i]->fence <= completed) {
        retired_[i]->used = 0;
        if (retired_[i]->size == block_size_) free_.push_back(std::move(retired_[i]));
        retired_[i] = std::move(retired_.back());
        retired_.pop_back();
      } else {
        ++i;
      }
    }
  }

 private:
  struct Block {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint8_t* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    uint64_t size = 0, used = 0, fence = 0;
  };
  std::unique_ptr<Block> CreateBlock(uint64_t size) {
    const uint64_t t0 = NowNs();
    auto b = std::make_unique<Block>();
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_UPLOAD};
    if (cpu_cached_)
      hp = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0};
    const D3D12_RESOURCE_DESC rd = BufferDesc(size);
    HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&b->resource));
    if (FAILED(hr)) {
      REXLOG_ERROR("PGR4 native: upload block of {} bytes failed: {:08X}", size, uint32_t(hr));
      return nullptr;
    }
    D3D12_RANGE none = {0, 0};
    void* p = nullptr;
    hr = b->resource->Map(0, &none, &p);
    if (FAILED(hr)) {
      REXLOG_ERROR("PGR4 native: upload block map failed: {:08X}", uint32_t(hr));
      return nullptr;
    }
    b->cpu = static_cast<uint8_t*>(p);
    b->gpu = b->resource->GetGPUVirtualAddress();
    b->size = size;
    ++created_;
    const double ms = (NowNs() - t0) / 1e6;
    if (created_ > reserved_ && created_ <= reserved_ + 32)
      REXLOG_INFO(
          "PGR4 native: upload block {} of {} MB created during play in {:.2f} ms (pool "
          "of {} MB blocks, {} reserved)",
          created_, size >> 20, ms, block_size_ >> 20, reserved_);
    return b;
  }
  bool NewBlock(uint64_t size, uint64_t fence) {
    std::unique_ptr<Block> b;
    if (size == block_size_ && !free_.empty()) {
      b = std::move(free_.back());
      free_.pop_back();
    } else {
      b = CreateBlock(size);
      if (!b) return false;
    }
    b->used = 0;
    b->fence = fence;
    active_.push_back(std::move(b));
    current_ = active_.back().get();
    return true;
  }

  ID3D12Device* device_ = nullptr;
  uint64_t block_size_;
  bool cpu_cached_;
  bool keep_previous_frame_ = false;
  Block* current_ = nullptr;
  uint64_t frame_fence_ = 0;  // the latest Allocate's fence
  uint32_t created_ = 0, reserved_ = 0;
  std::vector<std::unique_ptr<Block>> active_, previous_, retired_, free_;
};

}  // namespace pgr4::native
