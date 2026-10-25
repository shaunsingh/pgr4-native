// pgr4-native PGR4 native renderer: guest texture fetch constants -> D3D12 textures.
//
// TextureCache keeps one host texture per TextureKey (texture_layout.h: base/mip address,
// size, pitch, format, endianness, tiling, packed mips, mip count). Get() returns the texture
// for a fetch constant, uploading it first when it is new or stale:
//   - guest memory: untiled on the CPU (UntileTexture) into an upload buffer, copied on the
//     frame's command list, or, for a texture the frame has not bound yet, on an upload list
//     that runs ahead of it (CloseUploadLists);
//   - resolve destinations: the resolve module's host image (ResolvedLookup) instead. A 2D
//     texture with one subresource that an image covers exactly binds the image itself;
//     others copy it per level/slice when it is newer than the texture's copy.
// Write tracking: the guest pages of every uploaded texture are watched with ReXGlue's
// physical memory access callbacks (the mechanism the Xenos plugin's shared memory uses); a
// CPU write to a watched page marks the textures on it stale.
// Lifetime: the game streams textures through the same guest memory under new keys, so the
// cache evicts the least recently used textures over a memory budget (Xenia's texture cache
// policy, see Budget) once the GPU has finished the frames that used them.
//
// Threading: everything but MarkCapture runs on the thread that records the frame (the render
// thread by default); MarkCapture runs on the guest D3D thread, the untile jobs (watch arming,
// untile, upload list recording) on the untile workers, and the invalidation callback on any guest
// thread that faults on a watched page.
#pragma once

#include "resolve.h"  // ResolvedImage
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
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace pgr4::native {

// Returns the image last resolved to a guest physical address (Resolver::Find), or nullptr.
using ResolvedLookup = std::function<const ResolvedImage*(uint32_t guest_physical)>;

// One texture of the cache bound through one resource: the slot's generation goes up when the
// texture is evicted (its slot is reused) and when the resource it binds changes (a resolved
// image aliased in place of its own resource, or the alias dropped), so a handle kept past
// either no longer matches.
struct TextureHandle {
  uint32_t slot = 0, generation = 0;  // generation 0: no texture
};

// What a draw binds for one texture fetch constant.
struct TextureBinding {
  ID3D12Resource* resource = nullptr;  // in PIXEL|NON_PIXEL_SHADER_RESOURCE state
  // SRV for unsigned components (Format, ViewDimension TEXTURE2DARRAY for 1D/2D/stacked,
  // TEXTURE3D, TEXTURECUBE; Shader4ComponentMapping = the fetch swizzle on the host format).
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  // SNORM view format for signed components (DXGI_FORMAT_UNKNOWN if the format has none).
  DXGI_FORMAT srv_signed_format = DXGI_FORMAT_UNKNOWN;
  uint8_t swizzled_signs = 0;  // texture_util::SwizzleSigns (4 x 2-bit xenos::TextureSign)
  TextureKey key;
  TextureHandle entry;   // the cache's texture, for TextureCache::Unchanged and Touch
  bool aliased = false;  // `resource` is a resolved image bound in place of the texture's own
  // The game wrote the texture's memory after it queued the draw. With capture order on, bound
  // with the data from before that write, which is still to be uploaded for later draws (not
  // memoizable).
  bool stale = false;
};

class TextureCache {
 public:
  struct Stats {
    uint64_t gets = 0, uploads = 0, upload_bytes = 0, resolved_copies = 0, resolved_aliases = 0;
    uint64_t untile_ns = 0;
    // Profile counters (cumulative): memo misses, textures created, time in Create (resource
    // creation), Upload (buffer creation + untile + copies) and CopyResolved.
    uint64_t memo_misses = 0, created = 0, create_ns = 0, upload_ns = 0, copy_resolved_ns = 0;
    uint64_t evicted = 0, evict_ns = 0;  // textures evicted over the budget, time in Retire
    uint64_t deferred_uploads = 0;       // uploads on the upload lists (CloseUploadLists)
    uint64_t upload_record_ns = 0;       // recording them (worker CPU, as untile_ns)
  };
  // Texture memory budget, by the resources' allocation sizes. Over soft_bytes, textures unused
  // for soft_lifetime_ms are evicted; over hard_bytes, the least recently used ones regardless.
  // Either way only textures no frame in flight uses. Offline
  // (PGR4_TEXTURES_OFFLINE): unlimited.
  struct Budget {
    uint64_t soft_bytes = ~0ull, hard_bytes = ~0ull;
    uint64_t soft_lifetime_ms = 30000;
  };
  // Called by Retire with the resources of the textures it evicts, before releasing them: the
  // caller's views of them must go (the GPU has finished every frame that used them).
  using EvictCallback = std::function<void(const std::vector<ID3D12Resource*>& resources)>;

  // In the game: memory = rex::system::kernel_memory(); textures are read from its physical
  // memory and their pages watched through it.
  TextureCache(ID3D12Device* device, rex::memory::Memory* memory);
  // Offline (PGR4_TEXTURES_OFFLINE): guest physical memory at physical_membase, no write
  // tracking (a texture uploads once).
  TextureCache(ID3D12Device* device, const uint8_t* physical_membase);
  ~TextureCache();
  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  void SetResolvedLookup(ResolvedLookup lookup) { resolved_lookup_ = std::move(lookup); }
  void SetEvictCallback(EvictCallback callback) { evict_callback_ = std::move(callback); }
  // The resolver's Find result for this guest physical address may have changed
  // (Resolver::SetChangedCallback): textures with a subresource there recheck it at their next
  // Get.
  void OnResolved(uint32_t guest_physical);

  // Capture order. With the render thread, Get runs up to a frame after the game captured the
  // draw, and in primary mode the game may rewrite texture memory (update a dynamic texture or
  // stream another one into it) as soon as it has queued the draws that read it. The guest thread
  // stamps each draw with MarkCapture; the first write that stales an uploaded texture records the
  // last stamp handed out, and a Get for a draw stamped at or before it binds the texture's current
  // data, which predates the write. kCapturedNow: the draw sees guest memory as it is now. With
  // capture order off, such Gets upload the newer data instead (and still count as stale).
  static constexpr uint64_t kCapturedNow = ~0ull;
  // The guest D3D thread's (the only caller), so a plain store hands out the next stamp.
  uint64_t MarkCapture() {
    const uint64_t stamp = capture_seq_.value.load(std::memory_order_relaxed) + 1;
    capture_seq_.value.store(stamp, std::memory_order_release);
    return stamp;
  }
  void SetCaptureOrder(bool on) { capture_order_ = on; }

  // The texture for a fetch constant (6 dwords as in the register file), uploaded or copied on
  // `list` first if needed. frame_fence: the fence value that completes the frame `list`
  // belongs to (upload buffers are kept until then). captured: the draw's MarkCapture stamp.
  // False: no data, unsupported format, or a D3D12 failure (each logged once per format or
  // reason; the key is remembered and not retried).
  bool Get(const uint32_t fetch[6], ID3D12GraphicsCommandList* list, uint64_t frame_fence,
           TextureBinding* out, uint64_t captured = kCapturedNow);

  // In the game, untiling runs on worker threads: Upload records the GPU copies on the frame's
  // list or defers them (CloseUploadLists), and queues the untile into their upload memory
  // (offline it runs inline). Call this before the lists holding the copies are submitted; the
  // GPU reads the upload memory only then.
  void FinishUploads();
  // Without waiting: whether every untile queued so far has finished, so the list may be
  // submitted now.
  bool UploadsDone();

  // The upload of a texture the frame has not bound yet is deferred: the thread that untiles it
  // records its barriers and copies on an upload list of its own (one per untile worker), which
  // runs ahead of the frame's list. On the frame's list each upload is a copy and a barrier
  // between draws, which vkd3d-proton records by ending the render pass (about 54 µs per texture
  // created in the race), and recording the copies takes about 31 µs per texture in vkd3d-proton
  // and RADV (td-170), which the workers take off the render thread. No command the frame
  // recorded reads such a texture, so running its upload first changes nothing it sees. DXVK
  // moves transfers to its init command buffer under the same condition
  // (DxvkContext::prepareOutOfOrderTransfer).
  // Before each submission of the frame's list, once the untiles finished (FinishUploads, or
  // UploadsDone true): closes the upload lists recorded since the last call and appends them to
  // *lists, to be submitted first in the same ExecuteCommandLists. False if recording one failed
  // (logged).
  bool CloseUploadLists(std::vector<ID3D12CommandList*>* lists);

  // Recycles upload memory and upload list allocators of frames the GPU finished and evicts
  // textures over the budget. Once per Swap, after the submission.
  void Retire(uint64_t completed_fence);

  // Whether the binding Get returned for this texture can be bound again without a Get: not
  // evicted, still bound through the same resource (no alias set or dropped), no guest write
  // and no resolve into it since, so its resource, data, view and shader-read state are
  // unchanged. Callers that memoize bindings check this per texture.
  bool Unchanged(TextureHandle entry) const;
  // A binding bound again without a Get is used by the frame completing at frame_fence: keeps
  // its texture from eviction while that frame is in flight. Callers that memoize bindings
  // call this once per frame for each texture they bind that way.
  void Touch(TextureHandle entry, uint64_t frame_fence);
  // Goes up whenever a texture may stop being Unchanged (guest write, resolve into it, upload,
  // alias change, eviction).
  // A caller that checked Unchanged on its textures after reading the epoch may skip the checks
  // while the epoch still has that value.
  uint64_t change_epoch() const { return change_epoch_.load(std::memory_order_acquire); }

  const Stats& stats() const { return stats_; }
  size_t texture_count() const { return textures_.size(); }
  uint64_t resident_bytes() const { return resident_bytes_; }

 private:
  struct Texture;
  struct Slot {
    std::unique_ptr<Texture> texture;  // null while free
    // Goes up when the texture is evicted and when the resource it binds changes (SetAlias):
    // TextureHandle and GetMemo entries of an older generation no longer match.
    uint32_t generation = 1;
  };
  struct Watch {
    uint32_t start, end;  // physical [start, end)
    Texture* texture;
  };

  // A texture for key in a free slot, listed in textures_ (no resource yet).
  Texture* NewTexture(const TextureKey& key);
  Texture* Create(const TextureKey& key, const HostFormat& host);
  static bool Clean(const Texture& t);
  // Binds `alias` (a resolved image, or null for the texture's own resource) for t.
  void SetAlias(Texture& t, ID3D12Resource* alias);
  // Clears t's dirty flag and re-arms the watches of its guest pages, before their data is read.
  void ClearDirty(Texture& t);
  // t's resource holds guest-memory data in every subresource: the next Get runs CopyResolved.
  void ResetResolved(Texture& t);
  // Moves t to the end of the LRU list, once per frame.
  void MarkUsed(Texture& t, uint64_t frame_fence);
  void EvictOverBudget(uint64_t completed_fence);
  // Unlinks t from every structure that refers to it, frees its memory and its slot.
  void Destroy(Texture& t);
  // defer: on an upload list instead of `list` (CloseUploadLists); t ends in shader-read state.
  bool Upload(Texture& t, ID3D12GraphicsCommandList* list, uint64_t frame_fence, bool defer);
  // An upload's copies from its upload memory into the texture, one per footprint.
  struct UploadCopies {
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* source = nullptr;
    uint64_t source_offset = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // the texture's (HostFormat::resource)
    uint32_t block_width = 1, block_height = 1;
    uint32_t mip_levels = 1;
  };
  static void RecordCopies(ID3D12GraphicsCommandList* list, const UploadCopies& copies,
                           const std::vector<HostSubresource>& footprints);
  // A texture resource in COPY_DEST state, placed in a pooled heap at *placed (kCommitted if it
  // does not fit one or a heap cannot be created); *bytes: its allocation size. small_placement:
  // within the size rule for 4 KB placement.
  HRESULT CreateTextureResource(D3D12_RESOURCE_DESC& desc, bool small_placement,
                                Microsoft::WRL::ComPtr<ID3D12Resource>* out, uint64_t* placed,
                                uint64_t* bytes);
  static constexpr uint64_t kCommitted = ~0ull;
  // Placed texture memory: best-fit ranges over all heaps (offset heap * kTextureHeapSize +
  // offset in the heap); kCommitted if none fits.
  uint64_t AllocatePlaced(uint64_t size, uint64_t alignment);
  void FreePlaced(uint64_t offset, uint64_t size);
  bool AddHeap();
  void AddFreeRange(uint64_t offset, uint64_t size);
  void UnwatchRange(Texture& t, uint32_t start, uint32_t length);
  // Guest physical memory the watches cover.
  static constexpr uint32_t kMemorySize = (512u << 20);
  // [start, *end) of [start, start + length) within kMemorySize; false if that is empty.
  static bool ClampRange(uint32_t start, uint32_t length, uint32_t* end);
  bool CopyResolved(Texture& t, ID3D12GraphicsCommandList* list);
  // The resolved image CopyResolved copies into t from `image`, or null if its format family
  // does not match.
  ID3D12Resource* ResolvedSource(const Texture& t, const ResolvedImage& image) const;
  // Whether CopyResolved would overwrite every subresource of t in full, so an upload's data
  // would never be read.
  bool ResolvesCoverAll(const Texture& t) const;
  void Transition(Texture& t, ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES to);
  void WatchRange(Texture& t, uint32_t start, uint32_t length);
  // Arms the watches on the guest pages of a texture's base and mips: after its dirty flag is
  // cleared, before its data is read.
  void ArmWatches(const TextureKey& key, uint32_t base_bytes, uint32_t mips_bytes);
  static std::pair<uint32_t, uint32_t> InvalidationThunk(void* context, uint32_t start,
                                                         uint32_t length, bool exact);
  // Marks the textures on [start, start + length) stale and returns the range the memory may
  // unwatch: unless exact, widened over the pages of stale textures around it.
  std::pair<uint32_t, uint32_t> Invalidate(uint32_t start, uint32_t length, bool exact);
  struct UntileJob {
    TextureKey key;
    rex::graphics::texture_util::TextureGuestLayout layout;
    GuestTextureMemory guest;
    std::vector<HostSubresource> footprints;
    uint8_t* dst = nullptr;
    uint64_t size = 0;
    uint32_t upload_number = 0;  // the texture's Upload count (the slow-upload log)
    uint32_t base_bytes = 0, mips_bytes = 0;
    TextureConversion conversion = TextureConversion::kCopy;
    UploadCopies copies;
    // A deferred upload: the copies are recorded after the untile, on the untiling thread's
    // upload list for the frame completing at frame_fence, with the texture going from `state`
    // to COPY_DEST before them and to shader read after. Otherwise Upload recorded them.
    bool deferred = false;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    uint64_t frame_fence = 0;
  };
  // An untiling thread's upload list: the deferred uploads it untiled since the last
  // CloseUploadLists. Its command allocators are pooled by the frame fence of the lists recorded
  // on them and reset once the GPU passed it, as Xenia's D3D12CommandProcessor pools its own.
  // While untiles run only its thread touches it, CloseUploadLists only while none run.
  struct UploadList {
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;  // the frame's, from the first open
    uint64_t allocator_fence = 0;
    // (frame fence, allocator), oldest first.
    std::deque<std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12CommandAllocator>>> retired;
    bool open = false;
  };
  // Untiles one job; returns its time in ns.
  static uint64_t RunUntile(const UntileJob& job, std::vector<uint8_t>* scratch);
  // Records a deferred job's upload on `uploads`; returns its time in ns. On failure (logged)
  // CloseUploadLists returns false.
  uint64_t RecordDeferredUpload(const UntileJob& job, UploadList* uploads);
  // `uploads` open for the frame completing at frame_fence; null on failure (logged).
  ID3D12GraphicsCommandList* OpenUploadList(UploadList* uploads, uint64_t frame_fence);
  void WorkerMain(UploadList* uploads);

  ID3D12Device* device_;
  const uint8_t* membase_;                 // guest physical address 0
  rex::memory::Memory* memory_ = nullptr;  // null offline
  void* invalidation_handle_ = nullptr;
  bool unaligned_bc_supported_ = false;
  ResolvedLookup resolved_lookup_;
  // Direct-mapped memo of the last Get per fetch-dword hash: a hit (same 6 dwords, texture
  // unchanged) skips the key decode, the map lookup and the SRV desc build. An eviction clears
  // the entries of the textures it destroys. 1,024 entries: with 64, the race missed ~1,600
  // times per frame.
  struct GetMemo {
    uint32_t fetch[6] = {};
    Texture* texture = nullptr;
    TextureBinding binding;
  };
  static constexpr uint32_t kGetMemoBits = 10;
  std::unique_ptr<GetMemo[]> get_memo_ = std::make_unique<GetMemo[]>(size_t(1) << kGetMemoBits);
  std::atomic<uint64_t> change_epoch_{1};
  // MarkCapture stamps handed out, stored once per draw: on a cache line of its own, as the
  // render thread reads the members around it for every texture it binds.
  struct alignas(64) {
    std::atomic<uint64_t> value{0};
  } capture_seq_;
  bool capture_order_ = true;
  uint64_t unsupported_logged_ = 0;  // bit per xenos::TextureFormat
  uint32_t trace_address_ = 0;       // --pgr4_native_texture_trace (0: none)

  std::vector<Slot> slots_;  // owns the textures
  std::vector<uint32_t> free_slots_;
  std::unordered_map<TextureKey, Texture*, TextureKeyHasher> textures_;
  // Textures with a resource, least recently used first (MarkUsed).
  Texture* lru_first_ = nullptr;
  Texture* lru_last_ = nullptr;
  uint64_t now_ms_ = 0;          // at the last Retire: the use time MarkUsed records
  uint64_t resident_bytes_ = 0;  // allocation sizes of the textures with a resource
  Budget budget_;
  EvictCallback evict_callback_;
  static constexpr size_t kMaxEvictionsPerFrame = 128;
  std::vector<Texture*> evict_batch_;             // scratch
  std::vector<ID3D12Resource*> evict_resources_;  // scratch
  // The watched ranges of every texture, listed in each 64 KB bucket of guest physical memory
  // they touch (Destroy removes a texture's).
  static constexpr uint32_t kWatchBucketShift = 16;
  static constexpr uint32_t kPageMask = 4095;
  // The memory unwatches at most the 4 MB-aligned block around a write fault
  // (PhysicalHeap::TriggerCallbacks).
  static constexpr uint32_t kUnwatchWindow = 4u << 20;
  static constexpr uint32_t kWatchBucketCount = kMemorySize >> kWatchBucketShift;
  std::mutex watch_mutex_;  // guards watch_buckets_ (callback vs render thread)
  std::vector<std::vector<Watch>> watch_buckets_{kWatchBucketCount};
  // Guest physical address of every subresource CopyResolved looks up -> the textures there.
  std::unordered_map<uint32_t, std::vector<Texture*>> resolve_targets_;
  UploadPool upload_{32u << 20};  // untiled data, the copies' source
  // One per untile worker, or one for the inline untiles offline; sized before the workers start.
  std::vector<UploadList> upload_lists_;
  std::atomic<uint64_t> completed_fence_{0};  // Retire's: upload list allocators up to it are free
  std::atomic<bool> upload_list_failed_{false};
  // Texture memory: textures are placed in large heaps (CreatePlacedResource costs a fraction of
  // CreateCommittedResource: 627 textures took 138 ms at race start as committed resources),
  // best fit, and an evicted texture's range coalesces with its free neighbours.
  static constexpr uint64_t kTextureHeapSize = 128ull << 20;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Heap>> heaps_;
  std::map<uint64_t, uint64_t> free_ranges_;              // offset -> size
  std::set<std::pair<uint64_t, uint64_t>> free_by_size_;  // (size, offset)
  // The next heap, created on another thread while heap_ fills: creating a 128 MB heap on the
  // game thread took 20-130 ms in the frames where a heap ran full (HitchFix).
  std::future<Microsoft::WRL::ComPtr<ID3D12Heap>> spare_heap_;
  void StartSpareHeap();
  // A heap creation failed: textures that do not fit go committed without trying another
  // (each try blocks for 20-130 ms) until an eviction frees memory.
  bool heap_failed_ = false;
  std::vector<HostSubresource> footprints_;  // scratch
  std::vector<uint8_t> untile_scratch_;      // inline untiles (offline)
  std::vector<std::thread> workers_;         // empty offline
  std::mutex jobs_mutex_;
  std::condition_variable jobs_cv_, idle_cv_;
  std::deque<UntileJob> jobs_;
  uint32_t jobs_busy_ = 0;  // queued + running
  bool stopping_ = false;
  // Folded into stats_ by FinishUploads.
  std::atomic<uint64_t> worker_untile_ns_{0}, worker_record_ns_{0};
  Stats stats_;
};

}  // namespace pgr4::native
