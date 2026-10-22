// pgr4-native PGR4 native renderer (M2 owner #4): guest texture fetch constants -> D3D12 textures.
//
// TextureCache keeps one host texture per TextureKey (texture_layout.h: base/mip address,
// size, pitch, format, endianness, tiling, packed mips, mip count). Get() returns the texture
// for a fetch constant, uploading it first when it is new or stale:
//   - guest memory: untiled on the CPU (UntileTexture) into an upload buffer, copied on the
//     frame's command list;
//   - resolve destinations: the resolve module's host image (ResolvedLookup) instead. A 2D
//     texture with one subresource that an image covers exactly binds the image itself;
//     others copy it per level/slice when it is newer than the texture's copy.
// Write tracking: the guest pages of every uploaded texture are watched with ReXGlue's
// physical memory access callbacks (the mechanism the Xenos plugin's shared memory uses); a
// CPU write to a watched page marks the textures on it stale.
//
// Threading: Get/Retire run on the guest render thread (the renderer's On* calls); the
// invalidation callback runs on any guest thread that faults on a watched page.
#pragma once

#include "resolve.h"  // ResolvedImage (resolve module, M2 owner #5)
#include "texture_layout.h"
#include "upload_pool.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace pgr4::native {

// Returns the image last resolved to a guest physical address (Resolver::Find), or nullptr.
using ResolvedLookup = std::function<const ResolvedImage*(uint32_t guest_physical)>;

// What a draw binds for one texture fetch constant.
struct TextureBinding {
  ID3D12Resource* resource = nullptr;      // in PIXEL|NON_PIXEL_SHADER_RESOURCE state
  // SRV for unsigned components (Format, ViewDimension TEXTURE2DARRAY for 1D/2D/stacked,
  // TEXTURE3D, TEXTURECUBE; Shader4ComponentMapping = the fetch swizzle on the host format).
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  // SNORM view format for signed components (DXGI_FORMAT_UNKNOWN if the format has none).
  DXGI_FORMAT srv_signed_format = DXGI_FORMAT_UNKNOWN;
  uint8_t swizzled_signs = 0;  // texture_util::SwizzleSigns (4 x 2-bit xenos::TextureSign)
  TextureKey key;
  const void* entry = nullptr;  // the cache's texture, for TextureCache::Unchanged
  bool aliased = false;  // `resource` is a resolved image bound in place of the texture's own
};

class TextureCache {
 public:
  struct Stats {
    uint64_t gets = 0, uploads = 0, upload_bytes = 0, resolved_copies = 0, resolved_aliases = 0;
    uint64_t unsupported = 0, untile_ns = 0;
    // Profile counters (cumulative): memo misses, textures created, time in Create (resource
    // creation), Upload (buffer creation + untile + copies) and CopyResolved.
    uint64_t memo_misses = 0, created = 0, create_ns = 0, upload_ns = 0, copy_resolved_ns = 0;
  };

  // In the game: memory = rex::system::kernel_memory(); textures are read from its physical
  // memory and their pages watched through it.
  TextureCache(ID3D12Device* device, rex::memory::Memory* memory);
  // Offline (tools/texture_harness): guest physical memory at physical_membase, no write
  // tracking (a texture uploads once).
  TextureCache(ID3D12Device* device, const uint8_t* physical_membase);
  ~TextureCache();
  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  void SetResolvedLookup(ResolvedLookup lookup) { resolved_lookup_ = std::move(lookup); }
  // The resolver's Find result for this guest physical address may have changed
  // (Resolver::SetChangedCallback): textures with a subresource there recheck it at their next
  // Get.
  void OnResolved(uint32_t guest_physical);

  // The texture for a fetch constant (6 dwords as in the register file), uploaded or copied on
  // `list` first if needed. frame_fence: the fence value that completes the frame `list`
  // belongs to (upload buffers are kept until then). False: no data, unsupported format, or
  // a D3D12 failure (each logged once per key).
  bool Get(const uint32_t fetch[6], ID3D12GraphicsCommandList* list, uint64_t frame_fence,
           TextureBinding* out);

  // In the game, untiling runs on worker threads: Upload records the GPU copies at once and
  // queues the untile into their upload memory (offline it runs inline). Call this before the
  // command list Get recorded into is submitted; the GPU reads the upload memory only then.
  void FinishUploads();

  // Recycles upload memory of frames the GPU finished. Once per Swap, after the submission.
  void Retire(uint64_t completed_fence);

  // Whether the texture behind a binding Get returned can be bound again without a Get: no
  // guest write and no resolve into it since, so its data, view and shader-read state are
  // unchanged. Callers that memoize bindings check this per texture.
  bool Unchanged(const void* entry) const;
  // Goes up whenever a texture may stop being Unchanged (guest write, resolve into it, upload).
  // A caller that checked Unchanged on its textures after reading the epoch may skip the checks
  // while the epoch still has that value.
  uint64_t change_epoch() const { return change_epoch_.load(std::memory_order_acquire); }

  const Stats& stats() const { return stats_; }
  size_t texture_count() const { return textures_.size(); }

 private:
  struct Texture;
  struct Watch {
    uint32_t start, end;  // physical [start, end)
    Texture* texture;
  };

  Texture* Create(const TextureKey& key, const HostFormat& host);
  bool Upload(Texture& t, ID3D12GraphicsCommandList* list, uint64_t frame_fence);
  // A texture resource in COPY_DEST state, placed in a pooled heap (committed if it does not
  // fit one or a heap cannot be created).
  HRESULT CreateTextureResource(D3D12_RESOURCE_DESC& desc,
                                Microsoft::WRL::ComPtr<ID3D12Resource>* out);
  bool CopyResolved(Texture& t, ID3D12GraphicsCommandList* list);
  // The resolved image CopyResolved copies into t from `image`, or null if its format family
  // does not match.
  ID3D12Resource* ResolvedSource(const Texture& t, const ResolvedImage& image) const;
  // Whether CopyResolved would overwrite every subresource of t in full, so an upload's data
  // would never be read.
  bool ResolvesCoverAll(const Texture& t) const;
  void Transition(Texture& t, ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES to);
  void WatchRange(Texture& t, uint32_t start, uint32_t length);
  void ArmWatch(uint32_t physical, uint32_t length);
  static std::pair<uint32_t, uint32_t> InvalidationThunk(void* context, uint32_t start,
                                                         uint32_t length, bool exact);
  // Marks the textures on [start, start + length) stale and returns the range the memory may
  // unwatch: unless exact, widened over the pages of stale textures around it.
  std::pair<uint32_t, uint32_t> Invalidate(uint32_t start, uint32_t length, bool exact);
  struct UntileJob {
    TextureKey key;
    GuestTextureMemory guest;
    std::vector<HostSubresource> footprints;
    uint8_t* dst = nullptr;
    uint64_t size = 0;
    uint32_t upload_number = 0;  // the texture's Upload count (the slow-upload log)
    uint32_t base_bytes = 0, mips_bytes = 0;
    TextureConversion conversion = TextureConversion::kCopy;
  };
  // Untiles one job; returns its time in ns.
  static uint64_t RunUntile(const UntileJob& job, std::vector<uint8_t>* scratch);
  void WorkerMain();

  ID3D12Device* device_;
  const uint8_t* membase_;                 // guest physical address 0
  rex::memory::Memory* memory_ = nullptr;  // null offline
  void* invalidation_handle_ = nullptr;
  bool unaligned_bc_supported_ = false;
  ResolvedLookup resolved_lookup_;
  // Direct-mapped memo of the last Get per fetch-dword hash: a hit (same 6 dwords, texture
  // unchanged) skips the key decode, the map lookup and the SRV desc build. Textures are never
  // destroyed, so the pointer stays valid. 1,024 entries: with 64, the race missed ~1,600 times
  // per frame.
  struct GetMemo {
    uint32_t fetch[6] = {};
    Texture* texture = nullptr;
    TextureBinding binding;
  };
  static constexpr uint32_t kGetMemoBits = 10;
  std::unique_ptr<GetMemo[]> get_memo_{new GetMemo[size_t(1) << kGetMemoBits]};
  std::atomic<uint64_t> change_epoch_{1};

  std::unordered_map<TextureKey, std::unique_ptr<Texture>, TextureKeyHasher> textures_;
  // The watched ranges of every texture, listed in each 64 KB bucket of guest physical memory
  // they touch (textures are never destroyed while the cache lives).
  static constexpr uint32_t kWatchBucketShift = 16;
  static constexpr uint32_t kPageMask = 4095;
  // The memory unwatches at most the 4 MB-aligned block around a write fault
  // (PhysicalHeap::TriggerCallbacks).
  static constexpr uint32_t kUnwatchWindow = 4u << 20;
  static constexpr uint32_t kWatchBucketCount = (512u << 20) >> kWatchBucketShift;
  std::mutex watch_mutex_;  // guards watch_buckets_ (callback vs render thread)
  std::vector<std::vector<Watch>> watch_buckets_{kWatchBucketCount};
  // Guest physical address of every subresource CopyResolved looks up -> the textures there.
  std::unordered_map<uint32_t, std::vector<Texture*>> resolve_targets_;
  UploadPool upload_{32u << 20};  // untiled data, copied into the textures on the frame's list
  // Texture memory: textures are never destroyed while the cache lives, so they are placed in
  // large heaps by a bump allocator (CreatePlacedResource costs a fraction of
  // CreateCommittedResource: 627 textures took 138 ms at race start as committed resources).
  static constexpr uint64_t kTextureHeapSize = 128ull << 20;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Heap>> heaps_;
  ID3D12Heap* heap_ = nullptr;  // the heap being filled (heaps_.back())
  uint64_t heap_used_ = 0;
  // The next heap, created on another thread while heap_ fills: creating a 128 MB heap on the
  // game thread took 20-130 ms in the frames where a heap ran full (HitchFix).
  std::future<Microsoft::WRL::ComPtr<ID3D12Heap>> spare_heap_;
  void StartSpareHeap();
  std::vector<HostSubresource> footprints_;  // scratch
  std::vector<uint8_t> untile_scratch_;      // inline untiles (offline)
  std::vector<std::thread> workers_;         // empty offline
  std::mutex jobs_mutex_;
  std::condition_variable jobs_cv_, idle_cv_;
  std::deque<UntileJob> jobs_;
  uint32_t jobs_busy_ = 0;  // queued + running
  bool stopping_ = false;
  std::atomic<uint64_t> worker_untile_ns_{0};  // folded into stats_ by FinishUploads
  Stats stats_;
};

}  // namespace pgr4::native
