// pgr4-native PGR4 native renderer: texture cache (see textures.h).
#include "textures.h"

#include "clock.h"
#include "profile.h"

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#ifdef PGR4_TEXTURES_OFFLINE
// Standalone harness build (retired/native/tools/texture_harness): no ReXGlue runtime, no write
// tracking.
#include <fmt/format.h>
#include <cstdio>
#define TEXLOG(level, ...) std::fprintf(stderr, "%s: %s\n", level, fmt::format(__VA_ARGS__).c_str())
#define REXLOG_INFO(...) TEXLOG("info", __VA_ARGS__)
#define REXLOG_WARN(...) TEXLOG("warn", __VA_ARGS__)
#define REXLOG_ERROR(...) TEXLOG("error", __VA_ARGS__)
#include "log.h"
#else
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "log.h"

REXCVAR_DEFINE_STRING(pgr4_native_texture_trace, "", "PGR4 native",
                      "Log every TextureCache::Get of the texture at this guest physical address "
                      "(hex): dirty upload, resolved image seq, copy");
REXCVAR_DEFINE_INT32(pgr4_native_texture_memory_soft_mb, 512, "PGR4 native",
                     "Texture memory above which textures unused for "
                     "--pgr4_native_texture_memory_soft_lifetime_s are evicted (0: no limit)");
REXCVAR_DEFINE_INT32(pgr4_native_texture_memory_soft_lifetime_s, 30, "PGR4 native",
                     "See --pgr4_native_texture_memory_soft_mb");
REXCVAR_DEFINE_INT32(pgr4_native_texture_memory_hard_mb, 1024, "PGR4 native",
                     "Texture memory above which the least recently used textures are evicted, "
                     "once no frame in flight uses them (0: no limit)");
#endif

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string_view>

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
  bool failed = false;                 // unsupported or creation failed; logged once, never retried
  uint64_t stale_gets = 0;             // for the log
  std::vector<uint64_t> resolved_seq;  // per subresource; 0 = holds guest-memory data
  // A resolve may have changed an image at one of its subresource addresses (OnResolved), or
  // the texture was uploaded: the next Get runs CopyResolved.
  bool resolve_pending = true;
  // The resolved image bound in place of `resource` (CopyResolved), or null.
  ID3D12Resource* alias = nullptr;
  uint32_t uploads = 0;  // Upload calls so far (the slow-upload log)
  uint32_t slot = 0;
  uint64_t placed = kCommitted, bytes = 0;  // CreateTextureResource's placement and size
  uint64_t used_fence = 0, used_ms = 0;     // the last frame that bound it (MarkUsed)
  Texture* lru_prev = nullptr;
  Texture* lru_next = nullptr;
};

namespace {

constexpr D3D12_RESOURCE_STATES kShaderRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Typeless family for CopyTextureRegion compatibility.
DXGI_FORMAT Family(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SINT:
      return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
      return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
      return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_SNORM:
      return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_SNORM:
      return DXGI_FORMAT_R8_TYPELESS;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:
      return DXGI_FORMAT_R8G8_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
      return DXGI_FORMAT_R32G32_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
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
  upload_lists_.resize(1);
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
  invalidation_handle_ =
      memory_->RegisterPhysicalMemoryInvalidationCallback(InvalidationThunk, this);
  auto limit = [](int32_t mb) { return mb > 0 ? uint64_t(mb) << 20 : ~0ull; };
  budget_.soft_bytes = limit(REXCVAR_GET(pgr4_native_texture_memory_soft_mb));
  budget_.hard_bytes = limit(REXCVAR_GET(pgr4_native_texture_memory_hard_mb));
  budget_.soft_lifetime_ms =
      uint64_t(std::max(REXCVAR_GET(pgr4_native_texture_memory_soft_lifetime_s), 0)) * 1000;
  std::string_view trace = REXCVAR_GET(pgr4_native_texture_trace);
  if (trace.starts_with("0x") || trace.starts_with("0X")) trace.remove_prefix(2);
  std::from_chars(trace.data(), trace.data() + trace.size(), trace_address_, 16);
  // A quarter of the hardware threads, 1-4: 4 on an i9-13900K, 2 on a Steam Deck.
  const uint32_t n = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
  upload_lists_.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    workers_.emplace_back([this, uploads = &upload_lists_[i]] { WorkerMain(uploads); });
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
  if (invalidation_handle_)
    memory_->UnregisterPhysicalMemoryInvalidationCallback(invalidation_handle_);
#endif
}

void TextureCache::WorkerMain(UploadList* uploads) {
  std::vector<uint8_t> scratch;
  std::unique_lock<std::mutex> lock(jobs_mutex_);
  for (;;) {
    jobs_cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
    if (jobs_.empty()) return;  // stopping, queue drained
    UntileJob job = std::move(jobs_.front());
    jobs_.pop_front();
    lock.unlock();
    ArmWatches(job.key, job.base_bytes, job.mips_bytes);
    worker_untile_ns_.fetch_add(RunUntile(job, &scratch), std::memory_order_relaxed);
    if (job.deferred)
      worker_record_ns_.fetch_add(RecordDeferredUpload(job, uploads), std::memory_order_relaxed);
    lock.lock();
    if (--jobs_busy_ == 0) idle_cv_.notify_all();
  }
}

void TextureCache::FinishUploads() {
  if (workers_.empty()) return;
  std::unique_lock<std::mutex> lock(jobs_mutex_);
  idle_cv_.wait(lock, [this] { return jobs_busy_ == 0; });
  stats_.untile_ns += worker_untile_ns_.exchange(0, std::memory_order_relaxed);
  stats_.upload_record_ns += worker_record_ns_.exchange(0, std::memory_order_relaxed);
}

bool TextureCache::UploadsDone() {
  if (workers_.empty()) return true;
  std::lock_guard<std::mutex> lock(jobs_mutex_);
  return jobs_busy_ == 0;
}

uint64_t TextureCache::RunUntile(const UntileJob& job, std::vector<uint8_t>* scratch) {
  const uint64_t t0 = NowNs();
  UntileTexture(job.key, job.layout, job.guest, job.footprints, job.dst, scratch);
  const uint64_t ns = NowNs() - t0;
  if (ns > 500000) {
    const TextureKey& key = job.key;
    PGR4_LOG_FIRST(
        40, INFO,
        "PGR4 native textures: slow upload #{} of {:08X} {}x{}x{} {} dim{} tiled{} endian{} "
        "mips{} packed{} conv{}: host {} B, guest base {} B mips {} B, untile {} us",
        job.upload_number, key.base_page << 12, key.width, key.height, key.depth_or_array_size,
        rex::graphics::FormatInfo::Get(key.format)->name, uint32_t(key.dimension),
        uint32_t(key.tiled), uint32_t(key.endianness), key.mip_levels(), uint32_t(key.packed_mips),
        uint32_t(job.conversion), job.size, job.base_bytes, job.mips_bytes, ns / 1000);
  }
  return ns;
}

bool TextureCache::ClampRange(uint32_t start, uint32_t length, uint32_t* end) {
  if (!length || start >= kMemorySize) return false;
  *end = start + std::min(length, kMemorySize - start);
  return true;
}

void TextureCache::ArmWatches(const TextureKey& key, uint32_t base_bytes, uint32_t mips_bytes) {
#ifndef PGR4_TEXTURES_OFFLINE
  if (!memory_) return;
  const uint32_t base = key.base_page << 12, mips = key.mip_page << 12;
  // Mips from the page after the base's last: one protection change for both.
  if (base_bytes && mips_bytes && mips == base + ((base_bytes + kPageMask) & ~kPageMask)) {
    memory_->EnablePhysicalMemoryAccessCallbacks(base, mips - base + mips_bytes, true, false);
    return;
  }
  if (base_bytes) memory_->EnablePhysicalMemoryAccessCallbacks(base, base_bytes, true, false);
  if (mips_bytes) memory_->EnablePhysicalMemoryAccessCallbacks(mips, mips_bytes, true, false);
#endif
}

std::pair<uint32_t, uint32_t> TextureCache::InvalidationThunk(void* context, uint32_t start,
                                                              uint32_t length, bool exact) {
  return static_cast<TextureCache*>(context)->Invalidate(start, length, exact);
}

std::pair<uint32_t, uint32_t> TextureCache::Invalidate(uint32_t start, uint32_t length,
                                                       bool exact) {
  uint32_t end;
  if (!ClampRange(start, length, &end)) return {start, length};
  uint32_t lo = start, hi = end;
  bool any = false;
  {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    const uint64_t mark = capture_seq_.value.load(std::memory_order_acquire);
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

bool TextureCache::Clean(const Texture& t) {
  return !t.dirty.load(std::memory_order_acquire) && !t.resolve_pending;
}

void TextureCache::SetAlias(Texture& t, ID3D12Resource* alias) {
  if (t.alias == alias) return;
  t.alias = alias;
  // Bindings of the texture memoized by other fetch constants (GetMemo, callers' memos through
  // Unchanged) still name the previous resource.
  ++slots_[t.slot].generation;
  change_epoch_.fetch_add(1, std::memory_order_acq_rel);
}

void TextureCache::ClearDirty(Texture& t) {
  // Clear the flag, re-arm the page watches, then read: a write after the watch is armed marks
  // the texture stale again, a write before it is already in the data the untile reads.
  t.dirty.store(false, std::memory_order_release);
  ArmWatches(t.key, t.base_bytes, t.mips_bytes);
}

void TextureCache::ResetResolved(Texture& t) {
  std::fill(t.resolved_seq.begin(), t.resolved_seq.end(), 0);
  t.resolve_pending = true;
  change_epoch_.fetch_add(1, std::memory_order_acq_rel);
}

bool TextureCache::Unchanged(TextureHandle entry) const {
  const Slot& s = slots_[entry.slot];
  return s.generation == entry.generation && Clean(*s.texture);
}

void TextureCache::Touch(TextureHandle entry, uint64_t frame_fence) {
  const Slot& s = slots_[entry.slot];
  if (s.generation == entry.generation) MarkUsed(*s.texture, frame_fence);
}

void TextureCache::MarkUsed(Texture& t, uint64_t frame_fence) {
  if (t.used_fence == frame_fence) return;
  t.used_fence = frame_fence;
  t.used_ms = now_ms_;
  if (lru_last_ == &t) return;
  if (t.lru_prev || lru_first_ == &t) {
    (t.lru_prev ? t.lru_prev->lru_next : lru_first_) = t.lru_next;
    t.lru_next->lru_prev = t.lru_prev;  // not the last, so it has a next
  }
  t.lru_prev = lru_last_;
  t.lru_next = nullptr;
  (lru_last_ ? lru_last_->lru_next : lru_first_) = &t;
  lru_last_ = &t;
}

void TextureCache::WatchRange(Texture& t, uint32_t start, uint32_t length) {
  uint32_t end;
  if (!ClampRange(start, length, &end)) return;
  std::lock_guard<std::mutex> lock(watch_mutex_);
  for (uint32_t b = start >> kWatchBucketShift; b <= (end - 1) >> kWatchBucketShift; ++b)
    watch_buckets_[b].push_back({start, end, &t});
}

TextureCache::Texture* TextureCache::NewTexture(const TextureKey& key) {
  uint32_t slot;
  if (!free_slots_.empty()) {
    slot = free_slots_.back();
    free_slots_.pop_back();
  } else {
    slot = uint32_t(slots_.size());
    slots_.emplace_back();
  }
  Slot& s = slots_[slot];
  s.texture = std::make_unique<Texture>();
  Texture* t = s.texture.get();
  t->key = key;
  t->slot = slot;
  textures_.emplace(key, t);
  return t;
}

TextureCache::Texture* TextureCache::Create(const TextureKey& key, const HostFormat& host) {
  Texture& t = *NewTexture(key);
  t.host = &host;
  t.layout = GetGuestLayout(key);
  GetGuestExtents(key, t.layout, &t.base_bytes, &t.mips_bytes);
  t.resolved_seq.assign(size_t(key.mip_levels()) * key.array_size(), 0);

  const rex::graphics::FormatInfo* info = rex::graphics::FormatInfo::Get(key.format);
  bool unaligned = (key.width % info->block_width) || (key.height % info->block_height);
  if (host.block_compressed && unaligned && !unaligned_bc_supported_) {
    PGR4_WARN_ONCE(
        "PGR4 native textures: {}x{} {} needs decompression (no unaligned BC support); "
        "not supported (logged once)",
        key.width, key.height, info->name);
    t.failed = true;
  } else {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension =
        key.is_3d() ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = key.width;
    desc.Height = key.height;
    desc.DepthOrArraySize = uint16_t(key.depth_or_array_size);
    desc.MipLevels = uint16_t(key.mip_levels());
    desc.Format = host.resource;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // D3D12 allows 4 KB placement only for a texture whose most detailed slice takes at most
    // 64 KB (D3D12_RESOURCE_DESC, small resources); vkd3d-proton applies the same estimate.
    const uint32_t bw = host.block_compressed ? info->block_width : 1;
    const uint32_t bh = host.block_compressed ? info->block_height : 1;
    const bool small_placement =
        uint64_t(key.width) * key.height * info->bytes_per_block() / (bw * bh) <=
        D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    HRESULT hr = CreateTextureResource(desc, small_placement, &t.resource, &t.placed, &t.bytes);
    if (FAILED(hr)) {
      PGR4_LOG_FIRST(1, ERROR,
                     "PGR4 native textures: texture resource {}x{}x{} {} failed: {:08X} (logged "
                     "once)",
                     key.width, key.height, key.depth_or_array_size, info->name, uint32_t(hr));
      t.failed = true;
    } else {
      resident_bytes_ += t.bytes;
      t.state = D3D12_RESOURCE_STATE_COPY_DEST;
      WatchRange(t, key.base_page << 12, t.base_bytes);
      WatchRange(t, key.mip_page << 12, t.mips_bytes);
      ForEachResolveAddress(key, t.layout, [&](size_t, uint32_t address) {
        resolve_targets_[address].push_back(&t);
      });
    }
  }
  return &t;
}

void TextureCache::UnwatchRange(Texture& t, uint32_t start, uint32_t length) {
  uint32_t end;
  if (!ClampRange(start, length, &end)) return;
  std::lock_guard<std::mutex> lock(watch_mutex_);
  for (uint32_t b = start >> kWatchBucketShift; b <= (end - 1) >> kWatchBucketShift; ++b) {
    std::vector<Watch>& v = watch_buckets_[b];
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Watch& w) { return w.texture == &t; }),
            v.end());
  }
}

void TextureCache::Destroy(Texture& t) {
  // The pages stay armed: a later write faults once, finds no watch and unwatches them.
  UnwatchRange(t, t.key.base_page << 12, t.base_bytes);
  UnwatchRange(t, t.key.mip_page << 12, t.mips_bytes);
  ForEachResolveAddress(t.key, t.layout, [&](size_t, uint32_t address) {
    auto it = resolve_targets_.find(address);
    if (it == resolve_targets_.end()) return;
    std::vector<Texture*>& v = it->second;
    v.erase(std::remove(v.begin(), v.end(), &t), v.end());
    if (v.empty()) resolve_targets_.erase(it);
  });
  (t.lru_prev ? t.lru_prev->lru_next : lru_first_) = t.lru_next;
  (t.lru_next ? t.lru_next->lru_prev : lru_last_) = t.lru_prev;
  textures_.erase(t.key);
  resident_bytes_ -= t.bytes;
  if (t.placed != kCommitted) FreePlaced(t.placed, t.bytes);
  Slot& s = slots_[t.slot];
  ++s.generation;
  free_slots_.push_back(t.slot);
  s.texture.reset();  // releases the resource
}

void TextureCache::EvictOverBudget(uint64_t completed_fence) {
  if (resident_bytes_ <= budget_.soft_bytes) return;
  const uint64_t t0 = NowNs();
  evict_batch_.clear();
  evict_resources_.clear();
  uint64_t bytes = resident_bytes_;
  for (Texture* t = lru_first_; t && evict_batch_.size() < kMaxEvictionsPerFrame; t = t->lru_next) {
    // A frame in flight may read it, and every texture used after it.
    if (t->used_fence > completed_fence) break;
    if (bytes <= budget_.hard_bytes &&
        (bytes <= budget_.soft_bytes || now_ms_ - t->used_ms < budget_.soft_lifetime_ms))
      break;
    evict_batch_.push_back(t);
    evict_resources_.push_back(t->resource.Get());
    bytes -= t->bytes;
  }
  if (evict_batch_.empty()) return;
  if (!stats_.evicted)
    REXLOG_INFO(
        "PGR4 native textures: {} MB in {} textures, over the budget (soft {} MB for {} s "
        "unused, hard {} MB): evicting",
        resident_bytes_ >> 20, textures_.size(), budget_.soft_bytes >> 20,
        budget_.soft_lifetime_ms / 1000, budget_.hard_bytes >> 20);
  if (evict_callback_) evict_callback_(evict_resources_);
  std::sort(evict_batch_.begin(), evict_batch_.end());
  for (size_t i = 0; i < (size_t(1) << kGetMemoBits); ++i) {
    GetMemo& m = get_memo_[i];
    if (m.texture && std::binary_search(evict_batch_.begin(), evict_batch_.end(), m.texture))
      m.texture = nullptr;
  }
  for (Texture* t : evict_batch_) Destroy(*t);
  if (heap_failed_) {
    heap_failed_ = false;
    if (!spare_heap_.valid()) StartSpareHeap();
  }
  stats_.evicted += evict_batch_.size();
  change_epoch_.fetch_add(1, std::memory_order_acq_rel);
  stats_.evict_ns += NowNs() - t0;
}

void TextureCache::AddFreeRange(uint64_t offset, uint64_t size) {
  free_ranges_.emplace(offset, size);
  free_by_size_.emplace(size, offset);
}

uint64_t TextureCache::AllocatePlaced(uint64_t size, uint64_t alignment) {
  for (auto it = free_by_size_.lower_bound({size, 0}); it != free_by_size_.end(); ++it) {
    const auto [range_size, start] = *it;
    const uint64_t at = (start + alignment - 1) & ~(alignment - 1);
    const uint64_t end = start + range_size;
    if (at + size > end) continue;
    free_by_size_.erase(it);
    free_ranges_.erase(start);
    if (at > start) AddFreeRange(start, at - start);
    if (at + size < end) AddFreeRange(at + size, end - (at + size));
    return at;
  }
  return kCommitted;
}

void TextureCache::FreePlaced(uint64_t offset, uint64_t size) {
  uint64_t end = offset + size;
  // Coalesce with the free neighbours in the same heap.
  auto next = free_ranges_.lower_bound(offset);
  if (next != free_ranges_.end() && next->first == end && end % kTextureHeapSize) {
    end += next->second;
    free_by_size_.erase({next->second, next->first});
    next = free_ranges_.erase(next);
  }
  if (next != free_ranges_.begin() && offset % kTextureHeapSize) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == offset) {
      offset = prev->first;
      free_by_size_.erase({prev->second, prev->first});
      free_ranges_.erase(prev);
    }
  }
  AddFreeRange(offset, end - offset);
}

bool TextureCache::AddHeap() {
  if (!spare_heap_.valid()) StartSpareHeap();
  const uint64_t t0 = NowNs();
  ComPtr<ID3D12Heap> heap = spare_heap_.get();
  const uint64_t wait_ns = NowNs() - t0;
  if (wait_ns >= 1000000)
    REXLOG_INFO("PGR4 native textures: waited {:.1f} ms for texture heap {}", wait_ns / 1e6,
                heaps_.size() + 1);
  if (!heap) {
    heap_failed_ = true;
    PGR4_WARN_ONCE("PGR4 native textures: heap creation failed; committed textures");
    return false;
  }
  AddFreeRange(heaps_.size() * kTextureHeapSize, kTextureHeapSize);
  heaps_.push_back(std::move(heap));
  StartSpareHeap();
  return true;
}

HRESULT TextureCache::CreateTextureResource(D3D12_RESOURCE_DESC& desc, bool small_placement,
                                            ComPtr<ID3D12Resource>* out, uint64_t* placed,
                                            uint64_t* bytes) {
  // 4 KB placement when the driver allows it for this texture, else 64 KB. Outside the size
  // rule the 4 KB query only fails (and vkd3d-proton logs it twice): 343 textures of a race run.
  D3D12_RESOURCE_ALLOCATION_INFO info = {};
  if (small_placement) {
    desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
    info = device_->GetResourceAllocationInfo(0, 1, &desc);
  }
  if (info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT) {
    desc.Alignment = 0;
    info = device_->GetResourceAllocationInfo(0, 1, &desc);
  }
  *placed = kCommitted;
  *bytes = info.SizeInBytes == UINT64_MAX ? 0 : info.SizeInBytes;
  if (info.SizeInBytes != UINT64_MAX && info.SizeInBytes <= kTextureHeapSize) {
    uint64_t offset = AllocatePlaced(info.SizeInBytes, info.Alignment);
    if (offset == kCommitted && !heap_failed_ && AddHeap())
      offset = AllocatePlaced(info.SizeInBytes, info.Alignment);
    if (offset != kCommitted) {
      if (SUCCEEDED(device_->CreatePlacedResource(heaps_[offset / kTextureHeapSize].Get(),
                                                  offset % kTextureHeapSize, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(out->ReleaseAndGetAddressOf())))) {
        *placed = offset;
        return S_OK;
      }
      FreePlaced(offset, info.SizeInBytes);
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
    const uint64_t t0 = NowNs();
    D3D12_HEAP_DESC hd = {};
    hd.SizeInBytes = kTextureHeapSize;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    ComPtr<ID3D12Heap> heap;
    if (FAILED(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)))) return ComPtr<ID3D12Heap>();
    REXLOG_INFO("PGR4 native textures: texture heap {} created in {:.1f} ms (background)", n,
                (NowNs() - t0) / 1e6);
    return heap;
  });
}

void TextureCache::Transition(Texture& t, ID3D12GraphicsCommandList* list,
                              D3D12_RESOURCE_STATES to) {
  if (t.state == to) return;
  const D3D12_RESOURCE_BARRIER b = TransitionBarrier(t.resource.Get(), t.state, to);
  list->ResourceBarrier(1, &b);
  t.state = to;
}

bool TextureCache::Upload(Texture& t, ID3D12GraphicsCommandList* list, uint64_t frame_fence,
                          bool defer) {
  const TextureKey& key = t.key;
  uint64_t size = GetHostFootprints(key, true, &footprints_);
  if (!size) return false;
  if (ResolvesCoverAll(t)) {
    // The CopyResolved that follows overwrites every subresource in full (it copies even over
    // fresh guest data), so the untile and its copies would be dead work: 1280x720 RGBA8
    // resolve destinations took 1.6-3.4 ms of untile each.
    ClearDirty(t);
    ResetResolved(t);
    profile::Count(profile::kUploadSkipped);
    return true;
  }
  UploadPool::Alloc a;
  if (!upload_.Allocate(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, frame_fence, &a))
    return false;

  // ClearDirty's order, with the watches armed by the untile job just before it reads: their
  // protection changes (about 20 µs each under Wine) run off the render thread.
  t.dirty.store(false, std::memory_order_release);
  UntileJob job;
  UploadCopies& copies = job.copies;
  copies.texture = t.resource.Get();
  copies.source = a.resource;
  copies.source_offset = a.offset;
  copies.format = t.host->resource;
  if (t.host->block_compressed) {
    const rex::graphics::FormatInfo* info = rex::graphics::FormatInfo::Get(key.format);
    copies.block_width = info->block_width;
    copies.block_height = info->block_height;
  }
  copies.mip_levels = key.mip_levels();
  if (defer) {
    job.deferred = true;
    job.state = t.state;
    job.frame_fence = frame_fence;
    t.state = kShaderRead;
    ++stats_.deferred_uploads;
  } else {
    // The copies read the upload memory only once the list is submitted, after FinishUploads.
    Transition(t, list, D3D12_RESOURCE_STATE_COPY_DEST);
    RecordCopies(list, copies, footprints_);
  }
  job.key = key;
  job.layout = t.layout;
  if (key.base_page) {
    job.guest.base = membase_ + (key.base_page << 12);
    job.guest.base_size = t.base_bytes;
  }
  if (key.mip_page) {
    job.guest.mips = membase_ + (key.mip_page << 12);
    job.guest.mips_size = t.mips_bytes;
  }
  job.footprints = std::move(footprints_);
  job.dst = a.cpu;
  job.size = size;
  job.upload_number = ++t.uploads;
  job.base_bytes = t.base_bytes;
  job.mips_bytes = t.mips_bytes;
  job.conversion = t.host->conversion;
  if (workers_.empty()) {
    ArmWatches(job.key, job.base_bytes, job.mips_bytes);
    stats_.untile_ns += RunUntile(job, &untile_scratch_);
    if (job.deferred) stats_.upload_record_ns += RecordDeferredUpload(job, &upload_lists_[0]);
    footprints_ = std::move(job.footprints);
  } else {
    {
      std::lock_guard<std::mutex> lock(jobs_mutex_);
      jobs_.push_back(std::move(job));
      ++jobs_busy_;
    }
    jobs_cv_.notify_one();
  }

  SetAlias(t, nullptr);  // the resource holds the newer data now
  ResetResolved(t);
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
    ForEachResolveAddress(key, t.layout,
                          [&](size_t, uint32_t address) { image = resolved_lookup_(address); });
    ID3D12Resource* source = image ? ResolvedSource(t, *image) : nullptr;
    if (source) {
      const D3D12_RESOURCE_DESC sd = source->GetDesc();
      if (sd.Width == key.width && sd.Height == key.height) {
        SetAlias(t, source);
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
    SetAlias(t, nullptr);
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
  // The generation also changes when the texture's alias does, which Clean does not see.
  if (same_fetch && Clean(*memo.texture) &&
      memo.binding.entry.generation == slots_[memo.texture->slot].generation) {
    MarkUsed(*memo.texture, frame_fence);
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
    t = it->second;
  } else {
    const HostFormat* host = GetHostFormat(key.format);
    if (!host) {
      // Remembered so the key is not looked up again; the warning goes once per format.
      t = NewTexture(key);
      t->failed = true;
      const uint64_t bit = uint64_t(1) << (uint32_t(key.format) & 63);
      if (!(unsupported_logged_ & bit)) {
        unsupported_logged_ |= bit;
        REXLOG_WARN("PGR4 native textures: unsupported format {} (first {}x{} at {:08X})",
                    rex::graphics::FormatInfo::Get(key.format)->name, key.width, key.height,
                    key.base_page << 12);
      }
    } else {
      const uint64_t c0 = NowNs();
      t = Create(key, *host);
      stats_.create_ns += NowNs() - c0;
      ++stats_.created;
    }
  }
  if (t->failed) return false;
  const bool first_use = t->used_fence != frame_fence;
  MarkUsed(*t, frame_fence);
  const bool was_dirty = t->dirty.load(std::memory_order_acquire);
  const bool stale =
      was_dirty && t->uploads && captured <= t->write_mark.load(std::memory_order_relaxed);
  // Logged at its 1st, 256th, 512th, 1024th... stale Get.
  if (stale &&
      (++t->stale_gets == 1 || (t->stale_gets >= 256 && !(t->stale_gets & (t->stale_gets - 1))))) {
    PGR4_LOG_FIRST(96, INFO,
                   "PGR4 native textures: {:08X} {}x{}x{} {} mips {} rewritten after a draw that "
                   "reads it was queued: {} times (upload {}, get {})",
                   key.base_page << 12, key.width, key.height, key.depth_or_array_size,
                   rex::graphics::FormatInfo::Get(key.format)->name, key.mip_levels(),
                   t->stale_gets, t->uploads, stats_.gets);
  }
  if (was_dirty && !(stale && capture_order_)) {
    const uint64_t u0 = NowNs();
    const bool uploaded = Upload(*t, list, frame_fence, first_use);
    stats_.upload_ns += NowNs() - u0;
    if (!uploaded) return false;
  }
#ifndef PGR4_TEXTURES_OFFLINE
  const uint64_t seq_before = t->resolved_seq.empty() ? 0 : t->resolved_seq[0];
#endif
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
  if (trace_address_ && (key.base_page << 12) == trace_address_) [[unlikely]] {
    const ResolvedImage* image = resolved_lookup_ ? resolved_lookup_(trace_address_) : nullptr;
    REXLOG_INFO(
        "PGR4 native texture trace {:08X} {}x{} fmt{}: get #{} dirty_upload={} image_seq={} "
        "level0_seq {} -> {} image_fmt={} image_size={}x{}",
        trace_address_, key.width, key.height, uint32_t(key.format), stats_.gets, was_dirty,
        image ? image->seq : 0, seq_before, t->resolved_seq.empty() ? 0 : t->resolved_seq[0],
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
  out->entry = {t->slot, slots_[t->slot].generation};
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

void TextureCache::RecordCopies(ID3D12GraphicsCommandList* list, const UploadCopies& copies,
                                const std::vector<HostSubresource>& footprints) {
  for (const HostSubresource& s : footprints) {
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = copies.texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = s.level + s.slice * copies.mip_levels;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = copies.source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = copies.source_offset + s.offset;
    src.PlacedFootprint.Footprint.Format = copies.format;
    src.PlacedFootprint.Footprint.Width = s.block_columns * copies.block_width;
    src.PlacedFootprint.Footprint.Height = s.block_rows * copies.block_height;
    src.PlacedFootprint.Footprint.Depth = s.depth;
    src.PlacedFootprint.Footprint.RowPitch = s.row_pitch;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
}

uint64_t TextureCache::RecordDeferredUpload(const UntileJob& job, UploadList* uploads) {
  const uint64_t t0 = NowNs();
  ID3D12GraphicsCommandList* list = OpenUploadList(uploads, job.frame_fence);
  if (!list) {
    upload_list_failed_.store(true, std::memory_order_relaxed);
    return 0;
  }
  // One transition per call: vkd3d-proton folds transitions between COPY_DEST and shader read
  // into the next copy's barrier and the list's last one (its trivial copy barrier path).
  ID3D12Resource* r = job.copies.texture;
  if (job.state != D3D12_RESOURCE_STATE_COPY_DEST) {
    const D3D12_RESOURCE_BARRIER b =
        TransitionBarrier(r, job.state, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &b);
  }
  RecordCopies(list, job.copies, job.footprints);
  const D3D12_RESOURCE_BARRIER b =
      TransitionBarrier(r, D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
  list->ResourceBarrier(1, &b);
  return NowNs() - t0;
}

ID3D12GraphicsCommandList* TextureCache::OpenUploadList(UploadList* uploads, uint64_t frame_fence) {
  UploadList& u = *uploads;
  if (u.open) return u.list.Get();
  if (u.allocator && u.allocator_fence != frame_fence)
    u.retired.emplace_back(u.allocator_fence, std::move(u.allocator));
  HRESULT hr = S_OK;
  if (!u.allocator) {
    if (!u.retired.empty() &&
        u.retired.front().first <= completed_fence_.load(std::memory_order_relaxed)) {
      u.allocator = std::move(u.retired.front().second);
      u.retired.pop_front();
      hr = u.allocator->Reset();
    } else {
      hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(u.allocator.GetAddressOf()));
    }
    if (FAILED(hr)) {
      REXLOG_ERROR("PGR4 native textures: upload list allocator failed: {:08X}", uint32_t(hr));
      u.allocator.Reset();
      return nullptr;
    }
    u.allocator_fence = frame_fence;
  }
  if (u.list) {
    hr = u.list->Reset(u.allocator.Get(), nullptr);
  } else {
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, u.allocator.Get(), nullptr,
                                    IID_PPV_ARGS(u.list.GetAddressOf()));
    if (SUCCEEDED(hr)) u.list->SetName(L"pgr4 native uploads");
  }
  if (FAILED(hr)) {
    REXLOG_ERROR("PGR4 native textures: upload list reset failed: {:08X}", uint32_t(hr));
    return nullptr;
  }
  u.open = true;
  return u.list.Get();
}

bool TextureCache::CloseUploadLists(std::vector<ID3D12CommandList*>* lists) {
  for (UploadList& u : upload_lists_) {
    if (!u.open) continue;
    u.open = false;
    const HRESULT hr = u.list->Close();
    if (FAILED(hr)) {
      REXLOG_ERROR("PGR4 native textures: upload list close failed: {:08X}", uint32_t(hr));
      return false;
    }
    lists->push_back(u.list.Get());
  }
  return !upload_list_failed_.load(std::memory_order_relaxed);
}

void TextureCache::Retire(uint64_t completed_fence) {
  completed_fence_.store(completed_fence, std::memory_order_relaxed);
  upload_.EndFrame(completed_fence);
  now_ms_ = NowNs() / 1000000;
  EvictOverBudget(completed_fence);
}

}  // namespace pgr4::native
