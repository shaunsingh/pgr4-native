// pgr4-native PGR4 native renderer: texture cache (see textures.h).
#include "textures.h"

#include "profile.h"
#include "renderer.h"  // NowNs

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#ifdef PGR4_TEXTURES_OFFLINE
// Standalone harness build (tools/texture_harness): no ReXGlue runtime, no write tracking.
#include <fmt/format.h>
#include <cstdio>
#define TEXLOG(level, ...) std::fprintf(stderr, "%s: %s\n", level, fmt::format(__VA_ARGS__).c_str())
#define REXLOG_INFO(...) TEXLOG("info", __VA_ARGS__)
#define REXLOG_WARN(...) TEXLOG("warn", __VA_ARGS__)
#define REXLOG_ERROR(...) TEXLOG("error", __VA_ARGS__)
#else
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(pgr4_native_texture_trace, "", "PGR4 native",
                      "Log every TextureCache::Get of the texture at this guest physical address "
                      "(hex): dirty upload, resolved image seq, copy");
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace pgr4::native {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
using Microsoft::WRL::ComPtr;

struct TextureCache::Texture {
  TextureKey key;
  const HostFormat* host = nullptr;
  ComPtr<ID3D12Resource> resource;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
  texture_util::TextureGuestLayout layout{};
  uint32_t base_bytes = 0, mips_bytes = 0;
  // Set by the invalidation callback (any thread), cleared before the guest memory is read.
  std::atomic<bool> dirty{true};
  // The last MarkCapture stamp when a write set `dirty` (stored before it, under watch_mutex_).
  std::atomic<uint64_t> write_mark{0};
  bool failed = false;  // unsupported or creation failed; logged once, never retried
  uint64_t stale_gets = 0;  // for the log
  std::vector<uint64_t> resolved_seq;  // per subresource; 0 = holds guest-memory data
  // A resolve may have changed an image at one of its subresource addresses (OnResolved), or
  // the texture was uploaded: the next Get runs CopyResolved.
  bool resolve_pending = true;
  // The resolved image bound in place of `resource` (CopyResolved), or null.
  ID3D12Resource* alias = nullptr;
  uint32_t uploads = 0;                // Upload calls so far (the slow-upload log)
};

namespace {

constexpr D3D12_RESOURCE_STATES kShaderRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Typeless family for CopyTextureRegion compatibility.
DXGI_FORMAT Family(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SINT:
      return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
      return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SINT:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
      return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_SNORM:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_SNORM:
      return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM:
      return DXGI_FORMAT_R8_TYPELESS;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM:
      return DXGI_FORMAT_R8G8_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
      return DXGI_FORMAT_R32G32_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
      return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    default:
      return f;
  }
}

// Calls f(subresource index, guest physical address) for each subresource a resolve can target
// on its own: level 0 and the stored mips of each slice (not levels inside a packed mip tail).
template <typename F>
void ForEachResolveAddress(const TextureKey& key, const texture_util::TextureGuestLayout& layout,
                           F&& f) {
  for (uint32_t slice = 0; slice < key.array_size(); ++slice) {
    for (uint32_t level = 0; level <= key.mip_max_level; ++level) {
      uint32_t address;
      if (level == 0) {
        if (!key.base_page) continue;
        address = (key.base_page << 12) + layout.base.array_slice_stride_bytes * slice;
      } else {
        if (!key.mip_page) continue;
        uint32_t stored = std::min(level, layout.packed_level);
        if (level > stored) continue;  // inside a packed tail: no resolve targets it alone
        address = (key.mip_page << 12) + layout.mip_offsets_bytes[stored] +
                  layout.mips[stored].array_slice_stride_bytes * slice;
      }
      f(size_t(level) + size_t(slice) * key.mip_levels(), address);
    }
  }
}

}  // namespace

TextureCache::TextureCache(ID3D12Device* device, const uint8_t* physical_membase)
    : device_(device), membase_(physical_membase) {
  upload_.SetDevice(device_);
  upload_.Reserve(4);  // the race start keeps 3 in flight (HitchFix)
  StartSpareHeap();
  D3D12_FEATURE_DATA_D3D12_OPTIONS8 options8 = {};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &options8,
                                             sizeof(options8)))) {
    unaligned_bc_supported_ = options8.UnalignedBlockTexturesSupported;
  }
  REXLOG_INFO("PGR4 native textures: cache up (unaligned BC textures {})",
              unaligned_bc_supported_ ? "supported" : "not supported");
}

#ifndef PGR4_TEXTURES_OFFLINE
TextureCache::TextureCache(ID3D12Device* device, rex::memory::Memory* memory)
    : TextureCache(device, memory->physical_membase()) {
  memory_ = memory;
  invalidation_handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(InvalidationThunk, this);
  // A quarter of the hardware threads, 1-4: 4 on an i9-13900K, 2 on a Steam Deck.
  const uint32_t n = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
  for (uint32_t i = 0; i < n; ++i) {
    workers_.emplace_back([this] { WorkerMain(); });
    SetThreadDescription(workers_.back().native_handle(), L"PGR4 native untile");
  }
}
#endif

TextureCache::~TextureCache() {
  {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    stopping_ = true;
  }
  jobs_cv_.notify_all();
  for (std::thread& w : workers_) w.join();  // after the queued jobs ran
#ifndef PGR4_TEXTURES_OFFLINE
  if (invalidation_handle_) memory_->UnregisterPhysicalMemoryInvalidationCallback(invalidation_handle_);
#endif
}

void TextureCache::WorkerMain() {
  std::vector<uint8_t> scratch;
  std::unique_lock<std::mutex> lock(jobs_mutex_);
  for (;;) {
    jobs_cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
    if (jobs_.empty()) return;  // stopping, queue drained
    UntileJob job = std::move(jobs_.front());
    jobs_.pop_front();
    lock.unlock();
    worker_untile_ns_.fetch_add(RunUntile(job, &scratch), std::memory_order_relaxed);
    lock.lock();
    if (--jobs_busy_ == 0) idle_cv_.notify_all();
  }
}

void TextureCache::FinishUploads() {
  if (workers_.empty()) return;
  std::unique_lock<std::mutex> lock(jobs_mutex_);
  idle_cv_.wait(lock, [this] { return jobs_busy_ == 0; });
  stats_.untile_ns += worker_untile_ns_.exchange(0, std::memory_order_relaxed);
}

uint64_t TextureCache::RunUntile(const UntileJob& job, std::vector<uint8_t>* scratch) {
  const uint64_t t0 = NowNs();
  UntileTexture(job.key, job.guest, job.footprints, job.dst, scratch);
  const uint64_t ns = NowNs() - t0;
  static std::atomic<uint32_t> slow_logged{0};
  if (ns > 500000 && slow_logged.fetch_add(1, std::memory_order_relaxed) < 40) {
    const TextureKey& key = job.key;
    REXLOG_INFO("PGR4 native textures: slow upload #{} of {:08X} {}x{}x{} {} dim{} tiled{} endian{} "
                "mips{} packed{} conv{}: host {} B, guest base {} B mips {} B, untile {} us",
                job.upload_number, key.base_page << 12, key.width, key.height,
                key.depth_or_array_size, rex::graphics::FormatInfo::Get(key.format)->name,
                uint32_t(key.dimension), uint32_t(key.tiled), uint32_t(key.endianness),
                key.mip_levels(), uint32_t(key.packed_mips), uint32_t(job.conversion), job.size,
                job.base_bytes, job.mips_bytes, ns / 1000);
  }
  return ns;
}

void TextureCache::ArmWatch(uint32_t physical, uint32_t length) {
#ifndef PGR4_TEXTURES_OFFLINE
  if (memory_ && length) memory_->EnablePhysicalMemoryAccessCallbacks(physical, length, true, false);
#endif
}

std::pair<uint32_t, uint32_t> TextureCache::InvalidationThunk(void* context, uint32_t start,
                                                              uint32_t length, bool exact) {
  return static_cast<TextureCache*>(context)->Invalidate(start, length, exact);
}

std::pair<uint32_t, uint32_t> TextureCache::Invalidate(uint32_t start, uint32_t length,
                                                       bool exact) {
  constexpr uint32_t kMemorySize = kWatchBucketCount << kWatchBucketShift;
  if (start >= kMemorySize || !length) return {start, length};
  const uint32_t end = start + std::min(length, kMemorySize - start);
  uint32_t lo = start, hi = end;
  bool any = false;
  {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    const uint64_t mark = capture_seq_.load(std::memory_order_acquire);
    for (uint32_t b = start >> kWatchBucketShift; b <= (end - 1) >> kWatchBucketShift; ++b) {
      for (const Watch& w : watch_buckets_[b]) {
        if (w.start < end && start < w.end) {
          Texture& t = *w.texture;
          // The first write since the data was read: draws captured until now predate it.
          if (!t.dirty.load(std::memory_order_acquire)) {
            t.write_mark.store(mark, std::memory_order_relaxed);
            t.dirty.store(true, std::memory_order_release);
          }
          any = true;
        }
      }
    }
    if (!exact) {
      // Out to the nearest clean texture on each side: the pages of stale textures need no
      // watch (their next upload re-arms all of them), so a texture being streamed in faults
      // once rather than once per 4 KB page. Any texture overlapping [start, end) is dirty now,
      // and a watch lies in every bucket it touches, so the scan stops at the first bucket
      // holding a clean one. A texture cleared meanwhile re-arms after the unwatch (both hold
      // the global lock).
      auto clean = [](const Watch& w) { return !w.texture->dirty.load(std::memory_order_acquire); };
      const uint32_t window_lo = start & ~(kUnwatchWindow - 1);
      const uint32_t window_hi = ((end - 1) | (kUnwatchWindow - 1)) + 1;
      lo = window_lo;
      for (uint32_t b = start >> kWatchBucketShift;; --b) {
        for (const Watch& w : watch_buckets_[b])
          if (w.end <= start && clean(w)) lo = std::max(lo, w.end);
        if (lo != window_lo || b == window_lo >> kWatchBucketShift) break;
      }
      hi = window_hi;
      for (uint32_t b = (end - 1) >> kWatchBucketShift;; ++b) {
        for (const Watch& w : watch_buckets_[b])
          if (w.start >= end && clean(w)) hi = std::min(hi, w.start);
        if (hi != window_hi || b == (window_hi - 1) >> kWatchBucketShift) break;
      }
      // The page holding a clean texture's last byte stays watched.
      lo = std::min(start, (lo + kPageMask) & ~kPageMask);
      hi = std::max(end, hi & ~kPageMask);
    }
  }
  if (any) change_epoch_.fetch_add(1, std::memory_order_acq_rel);
  return {lo, hi - lo};
}

void TextureCache::OnResolved(uint32_t guest_physical) {
  auto it = resolve_targets_.find(guest_physical);
  if (it == resolve_targets_.end()) return;
  for (Texture* t : it->second) t->resolve_pending = true;
  change_epoch_.fetch_add(1, std::memory_order_acq_rel);
}

bool TextureCache::Unchanged(const void* entry) const {
  const Texture* t = static_cast<const Texture*>(entry);
  return !t->dirty.load(std::memory_order_acquire) && !t->resolve_pending;
}

void TextureCache::WatchRange(Texture& t, uint32_t start, uint32_t length) {
  constexpr uint32_t kMemorySize = kWatchBucketCount << kWatchBucketShift;
  if (!length || start >= kMemorySize) return;
  const uint32_t end = start + std::min(length, kMemorySize - start);
  std::lock_guard<std::mutex> lock(watch_mutex_);
  for (uint32_t b = start >> kWatchBucketShift; b <= (end - 1) >> kWatchBucketShift; ++b)
    watch_buckets_[b].push_back({start, end, &t});
}

TextureCache::Texture* TextureCache::Create(const TextureKey& key, const HostFormat& host) {
  auto owned = std::make_unique<Texture>();
  Texture& t = *owned;
  t.key = key;
  t.host = &host;
  t.layout = texture_util::GetGuestTextureLayout(key.dimension, key.pitch, key.width, key.height,
                                                 key.depth_or_array_size, key.tiled, key.format,
                                                 key.packed_mips, key.base_page != 0,
                                                 key.mip_max_level);
  GetGuestExtents(key, &t.base_bytes, &t.mips_bytes);
  t.resolved_seq.assign(size_t(key.mip_levels()) * key.array_size(), 0);

  const rex::graphics::FormatInfo* info = rex::graphics::FormatInfo::Get(key.format);
  bool unaligned = (key.width % info->block_width) || (key.height % info->block_height);
  if (host.block_compressed && unaligned && !unaligned_bc_supported_) {
    REXLOG_WARN("PGR4 native textures: {}x{} {} needs decompression (no unaligned BC support); "
                "not supported", key.width, key.height, info->name);
    t.failed = true;
  } else {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = key.is_3d() ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                 : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = key.width;
    desc.Height = key.height;
    desc.DepthOrArraySize = uint16_t(key.depth_or_array_size);
    desc.MipLevels = uint16_t(key.mip_levels());
    desc.Format = host.resource;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    HRESULT hr = CreateTextureResource(desc, &t.resource);
    if (FAILED(hr)) {
      REXLOG_ERROR("PGR4 native textures: texture resource {}x{}x{} {} failed: {:08X}",
                   key.width, key.height, key.depth_or_array_size, info->name, uint32_t(hr));
      t.failed = true;
    } else {
      t.state = D3D12_RESOURCE_STATE_COPY_DEST;
      WatchRange(t, key.base_page << 12, t.base_bytes);
      WatchRange(t, key.mip_page << 12, t.mips_bytes);
      ForEachResolveAddress(key, t.layout,
                            [&](size_t, uint32_t address) { resolve_targets_[address].push_back(&t); });
    }
  }
  Texture* raw = owned.get();
  textures_.emplace(key, std::move(owned));
  return raw;
}

HRESULT TextureCache::CreateTextureResource(D3D12_RESOURCE_DESC& desc,
                                            ComPtr<ID3D12Resource>* out) {
  // 4 KB placement when the driver allows it for this texture, else 64 KB.
  desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
  D3D12_RESOURCE_ALLOCATION_INFO info = device_->GetResourceAllocationInfo(0, 1, &desc);
  if (info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT) {
    desc.Alignment = 0;
    info = device_->GetResourceAllocationInfo(0, 1, &desc);
  }
  if (info.SizeInBytes != UINT64_MAX && info.SizeInBytes <= kTextureHeapSize) {
    uint64_t offset = (heap_used_ + info.Alignment - 1) & ~(info.Alignment - 1);
    if (!heap_ || offset + info.SizeInBytes > kTextureHeapSize) {
      if (!spare_heap_.valid()) StartSpareHeap();
      const auto t0 = std::chrono::steady_clock::now();
      ComPtr<ID3D12Heap> heap = spare_heap_.get();
      const double wait_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (wait_ms >= 1.0)
        REXLOG_INFO("PGR4 native textures: waited {:.1f} ms for texture heap {}", wait_ms,
                    heaps_.size() + 1);
      heap_ = nullptr;
      if (heap) {
        heap_ = heap.Get();
        heaps_.push_back(std::move(heap));
        heap_used_ = offset = 0;
        StartSpareHeap();
      } else {
        static bool logged = false;
        if (!logged) REXLOG_WARN("PGR4 native textures: heap creation failed; committed textures");
        logged = true;
      }
    }
    if (heap_ && SUCCEEDED(device_->CreatePlacedResource(heap_, offset, &desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(out->ReleaseAndGetAddressOf())))) {
      heap_used_ = offset + info.SizeInBytes;
      return S_OK;
    }
  }
  desc.Alignment = 0;
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
  return device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(out->ReleaseAndGetAddressOf()));
}

void TextureCache::StartSpareHeap() {
  spare_heap_ = std::async(std::launch::async, [device = device_, n = heaps_.size() + 1] {
    const auto t0 = std::chrono::steady_clock::now();
    D3D12_HEAP_DESC hd = {};
    hd.SizeInBytes = kTextureHeapSize;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    ComPtr<ID3D12Heap> heap;
    if (FAILED(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)))) return ComPtr<ID3D12Heap>();
    REXLOG_INFO("PGR4 native textures: texture heap {} created in {:.1f} ms (background)", n,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count());
    return heap;
  });
}

void TextureCache::Transition(Texture& t, ID3D12GraphicsCommandList* list,
                              D3D12_RESOURCE_STATES to) {
  if (t.state == to) return;
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = t.resource.Get();
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = t.state;
  b.Transition.StateAfter = to;
  list->ResourceBarrier(1, &b);
  t.state = to;
}

bool TextureCache::Upload(Texture& t, ID3D12GraphicsCommandList* list, uint64_t frame_fence) {
  const TextureKey& key = t.key;
  uint64_t size = GetHostFootprints(key, true, &footprints_);
  if (!size) return false;
  if (ResolvesCoverAll(t)) {
    // The CopyResolved that follows overwrites every subresource in full (it copies even over
    // fresh guest data), so the untile and its copies would be dead work: 1280x720 RGBA8
    // resolve destinations took 1.6-3.4 ms of untile each.
    t.dirty.store(false, std::memory_order_release);
    if (t.base_bytes) ArmWatch(key.base_page << 12, t.base_bytes);
    if (t.mips_bytes) ArmWatch(key.mip_page << 12, t.mips_bytes);
    std::fill(t.resolved_seq.begin(), t.resolved_seq.end(), 0);
    t.resolve_pending = true;
    change_epoch_.fetch_add(1, std::memory_order_acq_rel);
    profile::Count(profile::kUploadSkipped);
    return true;
  }
  UploadPool::Alloc a;
  if (!upload_.Allocate(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, frame_fence, &a))
    return false;

  // Clear the flag, re-arm the page watches, then read: a write after the watch is armed marks
  // the texture stale again, a write before it is already in the data the untile reads.
  t.dirty.store(false, std::memory_order_release);
  if (t.base_bytes) ArmWatch(key.base_page << 12, t.base_bytes);
  if (t.mips_bytes) ArmWatch(key.mip_page << 12, t.mips_bytes);
  UntileJob job;
  job.key = key;
  if (key.base_page) {
    job.guest.base = membase_ + (key.base_page << 12);
    job.guest.base_size = t.base_bytes;
  }
  if (key.mip_page) {
    job.guest.mips = membase_ + (key.mip_page << 12);
    job.guest.mips_size = t.mips_bytes;
  }
  job.footprints = footprints_;
  job.dst = a.cpu;
  job.size = size;
  job.upload_number = ++t.uploads;
  job.base_bytes = t.base_bytes;
  job.mips_bytes = t.mips_bytes;
  job.conversion = t.host->conversion;
  if (workers_.empty()) {
    stats_.untile_ns += RunUntile(job, &untile_scratch_);
  } else {
    {
      std::lock_guard<std::mutex> lock(jobs_mutex_);
      jobs_.push_back(std::move(job));
      ++jobs_busy_;
    }
    jobs_cv_.notify_one();
  }

  Transition(t, list, D3D12_RESOURCE_STATE_COPY_DEST);
  const uint32_t bw = t.host->block_compressed ? rex::graphics::FormatInfo::Get(key.format)->block_width : 1;
  const uint32_t bh = t.host->block_compressed ? rex::graphics::FormatInfo::Get(key.format)->block_height : 1;
  for (const HostSubresource& s : footprints_) {
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = t.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = s.level + s.slice * key.mip_levels();
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = a.resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = a.offset + s.offset;
    src.PlacedFootprint.Footprint.Format = t.host->resource;
    src.PlacedFootprint.Footprint.Width = s.block_columns * bw;
    src.PlacedFootprint.Footprint.Height = s.block_rows * bh;
    src.PlacedFootprint.Footprint.Depth = s.depth;
    src.PlacedFootprint.Footprint.RowPitch = s.row_pitch;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  std::fill(t.resolved_seq.begin(), t.resolved_seq.end(), 0);
  t.resolve_pending = true;
  t.alias = nullptr;  // the resource holds the newer data now
  change_epoch_.fetch_add(1, std::memory_order_acq_rel);
  ++stats_.uploads;
  stats_.upload_bytes += size;
  return true;
}

ID3D12Resource* TextureCache::ResolvedSource(const Texture& t, const ResolvedImage& image) const {
  if (image.texture && Family(image.texture->GetDesc().Format) == Family(t.host->resource))
    return image.texture;
  if (image.depth_r32f && Family(t.host->resource) == DXGI_FORMAT_R32_TYPELESS)
    return image.depth_r32f;
  return nullptr;  // incompatible view of the destination: keep the memory copy
}

bool TextureCache::ResolvesCoverAll(const Texture& t) const {
  const TextureKey& key = t.key;
  if (!resolved_lookup_ || key.is_3d()) return false;
  size_t covered = 0;
  bool all = true;
  ForEachResolveAddress(key, t.layout, [&](size_t index, uint32_t address) {
    if (!all) return;
    const ResolvedImage* image = resolved_lookup_(address);
    ID3D12Resource* source = image ? ResolvedSource(t, *image) : nullptr;
    if (!source) {
      all = false;
      return;
    }
    const D3D12_RESOURCE_DESC sd = source->GetDesc();
    const uint32_t level = uint32_t(index % key.mip_levels());
    all = sd.Width >= std::max(key.width >> level, 1u) &&
          sd.Height >= std::max(key.height >> level, 1u);
    ++covered;
  });
  return all && covered == t.resolved_seq.size();
}

bool TextureCache::CopyResolved(Texture& t, ID3D12GraphicsCommandList* list) {
  const TextureKey& key = t.key;
  // A 2D texture with one subresource that a resolved image of its format family covers
  // exactly binds that image in place of its resource: no copy, and the draws read the texels
  // the resolve wrote. Most resolve destinations (full-screen buffers, shadow maps) are such
  // textures.
  if (t.resolved_seq.size() == 1 && !key.is_3d()) {
    const ResolvedImage* image = nullptr;
    ForEachResolveAddress(key, t.layout, [&](size_t, uint32_t address) { image = resolved_lookup_(address); });
    ID3D12Resource* source = image ? ResolvedSource(t, *image) : nullptr;
    if (source) {
      const D3D12_RESOURCE_DESC sd = source->GetDesc();
      if (sd.Width == key.width && sd.Height == key.height) {
        t.alias = source;
        t.resolved_seq[0] = image->seq;
        ++stats_.resolved_aliases;
        return true;
      }
    }
  }
  if (t.alias) {
    // Leaving the alias (the address now holds an image of another size or format): the
    // resource takes over the aliased image's texels, which resolved_seq describes, before the
    // copies below update it. Images outlive the cache's use of them (Resolver).
    Transition(t, list, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = t.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = t.alias;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    ++stats_.resolved_copies;
    t.alias = nullptr;
  }
  bool any = false;
  ForEachResolveAddress(key, t.layout, [&](size_t index, uint32_t address) {
    const ResolvedImage* image = resolved_lookup_(address);
    if (!image) return;
    if (image->seq == t.resolved_seq[index]) {
      any = true;
      return;
    }
    ID3D12Resource* source = ResolvedSource(t, *image);
    if (!source) return;
    D3D12_RESOURCE_DESC sd = source->GetDesc();
    D3D12_BOX box = {0, 0, 0, 0, 0, 1};
    const uint32_t level = uint32_t(index % key.mip_levels());
    box.right = std::min<uint32_t>(uint32_t(sd.Width), std::max(key.width >> level, 1u));
    box.bottom = std::min<uint32_t>(sd.Height, std::max(key.height >> level, 1u));
    Transition(t, list, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = t.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = UINT(index);
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    t.resolved_seq[index] = image->seq;
    ++stats_.resolved_copies;
    any = true;
  });
  return any;
}

bool TextureCache::Get(const uint32_t fetch_dwords[6], ID3D12GraphicsCommandList* list,
                       uint64_t frame_fence, TextureBinding* out, uint64_t captured) {
  ++stats_.gets;
  uint32_t hash = 0;
  for (int i = 0; i < 6; ++i) hash = (hash ^ fetch_dwords[i]) * 0x9E3779B1u;
  GetMemo& memo = get_memo_[hash >> (32 - kGetMemoBits)];
  const bool same_fetch =
      memo.texture && std::memcmp(memo.fetch, fetch_dwords, sizeof(memo.fetch)) == 0;
  if (same_fetch && Unchanged(memo.texture)) {
    if (!memo.texture->alias) Transition(*memo.texture, list, kShaderRead);
    *out = memo.binding;
    return true;
  }
  ++stats_.memo_misses;
  profile::Count(same_fetch ? profile::kGetMissChanged : profile::kGetMissFetch);
  TextureKey key;
  if (!TextureKeyFromFetch(fetch_dwords, &key)) return false;
  auto it = textures_.find(key);
  Texture* t;
  if (it != textures_.end()) {
    t = it->second.get();
  } else {
    const HostFormat* host = GetHostFormat(key.format);
    if (!host) {
      // Remember the key so the warning is logged once.
      auto owned = std::make_unique<Texture>();
      owned->key = key;
      owned->failed = true;
      REXLOG_WARN("PGR4 native textures: unsupported format {} ({}x{} at {:08X})",
                  rex::graphics::FormatInfo::Get(key.format)->name, key.width, key.height,
                  key.base_page << 12);
      t = owned.get();
      textures_.emplace(key, std::move(owned));
    } else {
      const uint64_t c0 = NowNs();
      t = Create(key, *host);
      stats_.create_ns += NowNs() - c0;
      ++stats_.created;
    }
  }
  if (t->failed) {
    ++stats_.unsupported;
    return false;
  }
  const bool was_dirty = t->dirty.load(std::memory_order_acquire);
  const bool stale =
      was_dirty && t->uploads && captured <= t->write_mark.load(std::memory_order_relaxed);
  stats_.stale += stale;
  // Logged at its 1st, 256th, 512th, 1024th... stale Get.
  if (stale && stale_logged_ < kStaleLogLimit &&
      (++t->stale_gets == 1 || (t->stale_gets >= 256 && !(t->stale_gets & (t->stale_gets - 1))))) {
    ++stale_logged_;
    REXLOG_INFO("PGR4 native textures: {:08X} {}x{}x{} {} mips {} rewritten after a draw that "
                "reads it was queued: {} times (upload {}, get {})",
                key.base_page << 12, key.width, key.height, key.depth_or_array_size,
                rex::graphics::FormatInfo::Get(key.format)->name, key.mip_levels(), t->stale_gets,
                t->uploads, stats_.gets);
  }
  if (was_dirty && !(stale && capture_order_)) {
    const uint64_t u0 = NowNs();
    const bool uploaded = Upload(*t, list, frame_fence);
    stats_.upload_ns += NowNs() - u0;
    if (!uploaded) return false;
  }
  const uint64_t seq_before = t->resolved_seq.empty() ? 0 : t->resolved_seq[0];
  // CopyResolved only changes anything after an Upload or a resolve into one of the texture's
  // subresource addresses (OnResolved); both set resolve_pending.
  if (t->resolve_pending) {
    if (resolved_lookup_) {
      const uint64_t r0 = NowNs();
      CopyResolved(*t, list);
      stats_.copy_resolved_ns += NowNs() - r0;
    }
    t->resolve_pending = false;
  }
#ifndef PGR4_TEXTURES_OFFLINE
  static const uint32_t trace_address = [] {
    std::string s = REXCVAR_GET(pgr4_native_texture_trace);
    return s.empty() ? 0u : uint32_t(std::stoul(s, nullptr, 16));
  }();
  if (trace_address && (key.base_page << 12) == trace_address) {
    const ResolvedImage* image = resolved_lookup_ ? resolved_lookup_(trace_address) : nullptr;
    REXLOG_INFO("PGR4 native texture trace {:08X} {}x{} fmt{}: get #{} dirty_upload={} image_seq={} "
                "level0_seq {} -> {} image_fmt={} image_size={}x{}",
                trace_address, key.width, key.height, uint32_t(key.format), stats_.gets, was_dirty,
                image ? image->seq : 0, seq_before,
                t->resolved_seq.empty() ? 0 : t->resolved_seq[0],
                image && image->texture ? uint32_t(image->texture->GetDesc().Format) : 0,
                image ? image->width : 0, image ? image->height : 0);
  }
#endif
  // An alias is in the Resolver's read state, which includes kShaderRead.
  if (!t->alias) Transition(*t, list, kShaderRead);

  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  out->resource = t->alias ? t->alias : t->resource.Get();
  out->key = key;
  out->entry = t;
  out->aliased = t->alias != nullptr;
  out->stale = stale;
  out->srv_signed_format = t->host->snorm;
  out->swizzled_signs = texture_util::SwizzleSigns(fetch);
  D3D12_SHADER_RESOURCE_VIEW_DESC& srv = out->srv;
  srv = {};
  srv.Format = t->host->unorm;
  srv.Shader4ComponentMapping = GuestToHostComponentMapping(fetch.swizzle, t->host->host_swizzle);
  switch (key.dimension) {
    case xenos::DataDimension::k3D:
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      srv.Texture3D.MipLevels = key.mip_levels();
      break;
    case xenos::DataDimension::kCube:
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      srv.TextureCube.MipLevels = key.mip_levels();
      break;
    default:
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      srv.Texture2DArray.MipLevels = key.mip_levels();
      srv.Texture2DArray.ArraySize = key.array_size();
      break;
  }
  std::memcpy(memo.fetch, fetch_dwords, sizeof(memo.fetch));
  memo.texture = t;
  memo.binding = *out;
  memo.binding.stale = false;  // a memo hit needs the texture clean
  return true;
}

void TextureCache::Retire(uint64_t completed_fence) { upload_.EndFrame(completed_fence); }

}  // namespace pgr4::native
