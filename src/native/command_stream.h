// pgr4-native PGR4 native renderer: the frame's command list, recorded on a thread of its own.
//
// Recording a draw took the render thread about 3 us, a third of it in vkd3d-proton and RADV
// making the D3D12 calls (td-500's stage profile: record, 1.1 us a draw), and the race's
// heaviest frames (3000 draws) were the render thread's alone: 10.5 ms of draws in a 14 ms
// frame, the GPU busy for 11. The renderer writes its calls into a CommandStream instead, and
// the stream's thread makes them on the D3D12 list, as DXVK's CS thread does for D3D11
// (dxvk_cs.h): the two work side by side, a chunk of calls apart.
//
// The calls are the ID3D12GraphicsCommandList methods the renderer uses, with the same names and
// arguments; what they point to is copied when they are recorded. Call(f) runs f(list) on the
// stream's thread in order with them, for work that needs the list itself (FidelityFX's
// dispatches, a submission, Present).
//
// What a call names must stay valid until the thread made it, as it must anyway until the GPU
// ran it: resources and shader-visible descriptors, which the frame fence frees, and CPU
// descriptors (RTVs, DSVs), which D3D12 reads when the call is made, so these are not rewritten
// before the frame fence either (RenderTargets::Create).
//
// Threading: one thread records at a time. Flush hands what it recorded to the stream's thread,
// Sync also waits until that thread made every call.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace pgr4::native {

class CommandStream {
 public:
  using List = ID3D12GraphicsCommandList;

  CommandStream() = default;
  CommandStream(const CommandStream&) = delete;
  CommandStream& operator=(const CommandStream&) = delete;
  ~CommandStream() { Stop(); }

  // Starts the thread, which makes the calls on `list`; `name` names the thread.
  void Start(List* list, const wchar_t* name);
  // Makes every call recorded and stops the thread.
  void Stop();

  // Hands the calls recorded so far to the thread.
  void Flush();
  // Flush, then waits until the thread made them.
  void Sync();
  // The thread's time making calls since the last call, in ns.
  uint64_t TakeBusyNs() { return busy_ns_.exchange(0, std::memory_order_relaxed); }

  // f(list) on the thread, in order with the calls.
  template <class F>
  void Call(F&& f) {
    Emplace<std::decay_t<F>>(0, nullptr, std::forward<F>(f));
  }

  void ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* barriers) {
    CallWith(barriers, n,
             [n](List* l, const D3D12_RESOURCE_BARRIER* b) { l->ResourceBarrier(n, b); });
  }
  void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                         const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    const D3D12_TEXTURE_COPY_LOCATION d = *dst, s = *src;
    CallWith(box, box ? 1 : 0, [d, s, x, y, z](List* l, const D3D12_BOX* b) {
      l->CopyTextureRegion(&d, x, y, z, &s, b);
    });
  }
  void CopyBufferRegion(ID3D12Resource* dst, UINT64 dst_offset, ID3D12Resource* src,
                        UINT64 src_offset, UINT64 bytes) {
    Call([=](List* l) { l->CopyBufferRegion(dst, dst_offset, src, src_offset, bytes); });
  }
  void CopyResource(ID3D12Resource* dst, ID3D12Resource* src) {
    Call([=](List* l) { l->CopyResource(dst, src); });
  }
  void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT n,
                             const D3D12_RECT* rects) {
    const std::array<FLOAT, 4> c = {color[0], color[1], color[2], color[3]};
    CallWith(rects, n, [rtv, c, n](List* l, const D3D12_RECT* r) {
      l->ClearRenderTargetView(rtv, c.data(), n, r);
    });
  }
  void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth,
                             UINT8 stencil, UINT n, const D3D12_RECT* rects) {
    CallWith(rects, n, [=](List* l, const D3D12_RECT* r) {
      l->ClearDepthStencilView(dsv, flags, depth, stencil, n, r);
    });
  }

  void SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* heaps) {
    CallWith(heaps, n,
             [n](List* l, ID3D12DescriptorHeap* const* h) { l->SetDescriptorHeaps(n, h); });
  }
  void SetPipelineState(ID3D12PipelineState* pso) {
    Call([pso](List* l) { l->SetPipelineState(pso); });
  }
  void SetGraphicsRootSignature(ID3D12RootSignature* s) {
    Call([s](List* l) { l->SetGraphicsRootSignature(s); });
  }
  void SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    Call([=](List* l) { l->SetGraphicsRootDescriptorTable(index, table); });
  }
  void SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    Call([=](List* l) { l->SetGraphicsRootConstantBufferView(index, address); });
  }
  void SetGraphicsRoot32BitConstants(UINT index, UINT n, const void* data, UINT offset) {
    CallWith(static_cast<const uint32_t*>(data), n, [=](List* l, const uint32_t* d) {
      l->SetGraphicsRoot32BitConstants(index, n, d, offset);
    });
  }
  void SetComputeRootSignature(ID3D12RootSignature* s) {
    Call([s](List* l) { l->SetComputeRootSignature(s); });
  }
  void SetComputeRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    Call([=](List* l) { l->SetComputeRootDescriptorTable(index, table); });
  }
  void SetComputeRoot32BitConstants(UINT index, UINT n, const void* data, UINT offset) {
    CallWith(static_cast<const uint32_t*>(data), n, [=](List* l, const uint32_t* d) {
      l->SetComputeRoot32BitConstants(index, n, d, offset);
    });
  }

  void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
    Call([topology](List* l) { l->IASetPrimitiveTopology(topology); });
  }
  void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) {
    const D3D12_INDEX_BUFFER_VIEW v = view ? *view : D3D12_INDEX_BUFFER_VIEW{};
    const bool none = !view;
    Call([v, none](List* l) { l->IASetIndexBuffer(none ? nullptr : &v); });
  }
  void RSSetViewports(UINT n, const D3D12_VIEWPORT* viewports) {
    CallWith(viewports, n, [n](List* l, const D3D12_VIEWPORT* v) { l->RSSetViewports(n, v); });
  }
  void RSSetScissorRects(UINT n, const D3D12_RECT* rects) {
    CallWith(rects, n, [n](List* l, const D3D12_RECT* r) { l->RSSetScissorRects(n, r); });
  }
  // ID3D12GraphicsCommandList9's; nothing on a list without it.
  void RSSetDepthBias(FLOAT bias, FLOAT clamp, FLOAT slope) {
    if (!list9_) return;
    Call(
        [l9 = list9_.Get(), bias, clamp, slope](List*) { l9->RSSetDepthBias(bias, clamp, slope); });
  }
  void OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single_range,
                          const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    struct Targets {
      D3D12_CPU_DESCRIPTOR_HANDLE rtv[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
      D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    } t = {};
    const UINT copied = single_range ? (n ? 1 : 0) : n;
    if (copied) std::memcpy(t.rtv, rtvs, copied * sizeof(*rtvs));
    if (dsv) t.dsv = *dsv;
    const bool has_dsv = dsv != nullptr;
    Call([t, n, single_range, has_dsv](List* l) {
      l->OMSetRenderTargets(n, n ? t.rtv : nullptr, single_range, has_dsv ? &t.dsv : nullptr);
    });
  }
  void OMSetStencilRef(UINT ref) {
    Call([ref](List* l) { l->OMSetStencilRef(ref); });
  }
  void OMSetBlendFactor(const FLOAT factor[4]) {
    const std::array<FLOAT, 4> f = {factor[0], factor[1], factor[2], factor[3]};
    Call([f](List* l) { l->OMSetBlendFactor(f.data()); });
  }

  void DrawInstanced(UINT vertices, UINT instances, UINT first_vertex, UINT first_instance) {
    Call([=](List* l) { l->DrawInstanced(vertices, instances, first_vertex, first_instance); });
  }
  void DrawIndexedInstanced(UINT indices, UINT instances, UINT first_index, INT base_vertex,
                            UINT first_instance) {
    Call([=](List* l) {
      l->DrawIndexedInstanced(indices, instances, first_index, base_vertex, first_instance);
    });
  }
  void Dispatch(UINT x, UINT y, UINT z) {
    Call([=](List* l) { l->Dispatch(x, y, z); });
  }

  void BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    Call([=](List* l) { l->BeginQuery(heap, type, index); });
  }
  void EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    Call([=](List* l) { l->EndQuery(heap, type, index); });
  }
  void ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start, UINT n,
                        ID3D12Resource* dst, UINT64 offset) {
    Call([=](List* l) { l->ResolveQueryData(heap, type, start, n, dst, offset); });
  }

 private:
  static constexpr size_t kAlign = 16;
  static constexpr size_t kChunkBytes = 64 << 10;
  struct Chunk {
    size_t used = 0;
    alignas(kAlign) std::byte data[kChunkBytes];
  };
  // A call in a chunk: this header, its callable, then the data it points to.
  struct Entry {
    void (*run)(void* callable, List* list);
    uint32_t size;  // to the next entry
  };
  static constexpr size_t Align(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }
  static constexpr size_t kEntryBytes = (sizeof(Entry) + kAlign - 1) & ~(kAlign - 1);

  template <class F>
  static void Run(void* p, List* l) {
    F& f = *static_cast<F*>(p);
    f(l);
    f.~F();
  }

  // Room for `size` bytes in the chunk being recorded, flushing it when full.
  std::byte* Reserve(size_t size);

  // F(f) in a new entry with `extra` bytes after it (*extra_out), returned.
  template <class F, class G>
  F* Emplace(size_t extra, std::byte** extra_out, G&& f) {
    static_assert(alignof(F) <= kAlign);
    const size_t body = kEntryBytes + Align(sizeof(F));
    const size_t size = body + Align(extra);
    std::byte* p = Reserve(size);
    new (p) Entry{&Run<F>, uint32_t(size)};
    if (extra_out) *extra_out = p + body;
    return new (p + kEntryBytes) F(std::forward<G>(f));
  }

  // f(list, copy) on the thread, `copy` being the n values at `data` (null for none).
  template <class T, class F>
  void CallWith(const T* data, size_t n, F f) {
    struct Bound {
      F f;
      const T* data;
      void operator()(List* l) { f(l, data); }
    };
    std::byte* extra = nullptr;
    Bound* b = Emplace<Bound>(n * sizeof(T), &extra, Bound{f, nullptr});
    if (n) {
      std::memcpy(extra, data, n * sizeof(T));
      b->data = reinterpret_cast<const T*>(extra);
    }
  }

  void Main();
  Chunk* TakeChunk();  // a free chunk, or a new one (mutex_ held)

  List* list_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList9> list9_;
  Chunk* recording_ = nullptr;

  std::mutex mutex_;
  std::condition_variable work_cv_, done_cv_;
  std::vector<Chunk*> queue_;  // flushed, in order, from queue_head_
  size_t queue_head_ = 0;
  std::vector<Chunk*> free_;
  std::vector<std::unique_ptr<Chunk>> chunks_;
  uint64_t flushed_ = 0, done_ = 0;  // chunks
  bool stop_ = false;
  std::atomic<uint64_t> busy_ns_{0};
  std::thread thread_;
};

}  // namespace pgr4::native
