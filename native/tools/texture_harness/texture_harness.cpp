// pgr4-native PGR4 native renderer: standalone texture untiling harness (M2 owner #4).
//
// Reads texture dumps written by the Xenos plugin with xenos-texture-dump.patch
// (--pgr4_dump_texture_loads=<dir>): per loaded texture, the fetch constant it was bound with,
// ReXGlue's texture key, the guest bytes at the base and mip addresses at load time, and the
// host texture data the Xenos plugin's load shaders produced (read back per subresource).
// For each dump it derives the key from the fetch constant with TextureKeyFromFetch, untiles
// the guest bytes with UntileTexture (texture_layout.cpp) and compares every subresource with
// the Xenos upload byte for byte.
//
// With --d3d12 it also runs the in-game upload path on the GPU: the guest bytes are placed at
// their physical addresses in a 512 MB reserved range, TextureCache::Get (textures.cpp, built
// with PGR4_TEXTURES_OFFLINE) creates and uploads the D3D12 texture from the fetch constant,
// and the texture is read back and compared with the Xenos upload again.
//
//   texture_harness.exe <dump.ptxd | directory>... [--verbose] [--d3d12]
//
// Exit code 0 when every compared dump matches.
#include "../../src/native/texture_layout.h"
#include "../../src/native/textures.h"

#include <windows.h>
#include <memory>

#include <rex/graphics/pipeline/texture/info.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace fs = std::filesystem;
namespace xenos = rex::graphics::xenos;
using namespace pgr4::native;

namespace {

constexpr uint32_t kMagic = 0x44585450;  // "PTXD"

struct Reader {
  const std::vector<uint8_t>& d;
  size_t p = 0;
  bool ok = true;
  uint32_t U32() {
    if (p + 4 > d.size()) { ok = false; return 0; }
    uint32_t v;
    std::memcpy(&v, d.data() + p, 4);
    p += 4;
    return v;
  }
  const uint8_t* Bytes(size_t n) {
    if (p + n > d.size()) { ok = false; return nullptr; }
    const uint8_t* r = d.data() + p;
    p += n;
    return r;
  }
};

struct XenosSub {
  uint32_t width, height, depth, rows, row_size;
  const uint8_t* data;
};

struct Dump {
  uint32_t fetch_found;
  uint32_t fetch[6];
  TextureKey xkey;  // ReXGlue's key
  uint32_t dxgi_format, load_shader;
  std::vector<uint8_t> raw;
  const uint8_t* base = nullptr;
  uint32_t base_bytes = 0;
  const uint8_t* mips = nullptr;
  uint32_t mips_bytes = 0;
  std::vector<XenosSub> subs;
};

bool Parse(const fs::path& path, Dump* d) {
  std::ifstream f(path, std::ios::binary);
  d->raw.assign(std::istreambuf_iterator<char>(f), {});
  Reader r{d->raw};
  if (r.U32() != kMagic || r.U32() != 1) return false;
  d->fetch_found = r.U32();
  for (uint32_t& v : d->fetch) v = r.U32();
  TextureKey& k = d->xkey;
  k.base_page = r.U32();
  k.mip_page = r.U32();
  k.width = r.U32();
  k.height = r.U32();
  k.depth_or_array_size = r.U32();
  k.pitch = r.U32();
  k.mip_max_level = r.U32();
  k.format = xenos::TextureFormat(r.U32());
  k.endianness = xenos::Endian(r.U32());
  k.dimension = xenos::DataDimension(r.U32());
  k.tiled = r.U32() != 0;
  k.packed_mips = r.U32() != 0;
  d->dxgi_format = r.U32();
  d->load_shader = r.U32();
  d->base_bytes = r.U32();
  d->mips_bytes = r.U32();
  uint32_t sub_count = r.U32();
  d->base = r.Bytes(d->base_bytes);
  d->mips = r.Bytes(d->mips_bytes);
  for (uint32_t i = 0; i < sub_count && r.ok; ++i) {
    XenosSub s;
    s.width = r.U32();
    s.height = r.U32();
    s.depth = r.U32();
    s.rows = r.U32();
    s.row_size = r.U32();
    s.data = r.Bytes(size_t(s.rows) * s.depth * s.row_size);
    d->subs.push_back(s);
  }
  return r.ok;
}

std::string KeyText(const TextureKey& k) {
  char b[256];
  std::snprintf(b, sizeof(b),
                "base=%08X mips=%08X %ux%ux%u pitch=%u levels=%u %s endian=%u dim=%u %s%s",
                k.base_page << 12, k.mip_page << 12, k.width, k.height, k.depth_or_array_size,
                k.pitch, k.mip_max_level + 1, rex::graphics::FormatInfo::Get(k.format)->name,
                uint32_t(k.endianness), uint32_t(k.dimension), k.tiled ? "tiled" : "linear",
                k.packed_mips ? " packed" : "");
  return b;
}

// The in-game upload path (TextureCache) on a real D3D12 device, against the Xenos data.
class GpuCheck {
 public:
  bool Init() {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) return false;
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_))) ||
        FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_.Get(), nullptr,
                                          IID_PPV_ARGS(&list_))) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
      return false;
    }
    membase_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, 512u << 20, MEM_RESERVE, PAGE_READWRITE));
    if (!membase_) return false;
    cache_ = std::make_unique<TextureCache>(device_.Get(), static_cast<const uint8_t*>(membase_));
    return true;
  }

  // Returns true when every subresource matches the Xenos data.
  bool Run(const Dump& d, const std::string& name, uint64_t* bytes_out) {
    Place(d.xkey.base_page << 12, d.base, d.base_bytes);
    Place(d.xkey.mip_page << 12, d.mips, d.mips_bytes);
    TextureBinding b;
    ++fence_value_;
    const bool got = cache_->Get(d.fetch, list_.Get(), fence_value_, &b);
    cache_->RecordDeferredUploads(list_.Get());  // a new texture's upload is deferred
    if (!got) {
      std::printf("GPU   %s: TextureCache::Get failed\n", name.c_str());
      Submit();
      return false;
    }
    D3D12_RESOURCE_DESC desc = b.resource->GetDesc();
    uint32_t subs = uint32_t(d.subs.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subs);
    std::vector<UINT> rows(subs);
    std::vector<UINT64> row_sizes(subs);
    UINT64 total = 0;
    device_->GetCopyableFootprints(&desc, 0, subs, 0, layouts.data(), rows.data(), row_sizes.data(), &total);
    ComPtr<ID3D12Resource> readback;
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&readback)))) {
      std::printf("GPU   %s: readback buffer failed\n", name.c_str());
      Submit();
      return false;
    }
    Barrier(b.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (uint32_t i = 0; i < subs; ++i) {
      D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
      src.pResource = b.resource;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = i;
      dst.pResource = readback.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = layouts[i];
      list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Barrier(b.resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!Submit()) {
      std::printf("GPU   %s: command list failed (device removed?)\n", name.c_str());
      return false;
    }
    cache_->Retire(fence_value_);
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, SIZE_T(total)};
    readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
    uint64_t differing = 0, compared = 0;
    for (uint32_t i = 0; i < subs; ++i) {
      const XenosSub& x = d.subs[i];
      if (rows[i] != x.rows || row_sizes[i] != x.row_size) {
        std::printf("GPU   %s: subresource %u is %u rows x %llu bytes, Xenos %u x %u\n", name.c_str(),
                    i, rows[i], (unsigned long long)row_sizes[i], x.rows, x.row_size);
        readback->Unmap(0, nullptr);
        return false;
      }
      for (uint32_t z = 0; z < x.depth; ++z) {
        for (uint32_t y = 0; y < x.rows; ++y) {
          const uint8_t* a = mapped + layouts[i].Offset +
                             (uint64_t(z) * rows[i] + y) * layouts[i].Footprint.RowPitch;
          const uint8_t* bx = x.data + (size_t(z) * x.rows + y) * x.row_size;
          compared += x.row_size;
          if (std::memcmp(a, bx, x.row_size)) ++differing;
        }
      }
    }
    readback->Unmap(0, nullptr);
    *bytes_out = compared;
    if (differing) {
      std::printf("GPU   %s: %llu of the D3D12 texture's rows differ from the Xenos upload\n",
                  name.c_str(), (unsigned long long)differing);
      return false;
    }
    return true;
  }

  const TextureCache::Stats& stats() const { return cache_->stats(); }

 private:
  void Place(uint32_t physical, const uint8_t* data, uint32_t size) {
    if (!size) return;
    VirtualAlloc(membase_ + physical, size, MEM_COMMIT, PAGE_READWRITE);
    std::memcpy(membase_ + physical, data, size);
  }
  void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list_->ResourceBarrier(1, &b);
  }
  bool Submit() {
    if (FAILED(list_->Close())) return false;
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), fence_value_);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence_->SetEventOnCompletion(fence_value_, ev);
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
    alloc_->Reset();
    list_->Reset(alloc_.Get(), nullptr);
    return SUCCEEDED(device_->GetDeviceRemovedReason());
  }

  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12CommandAllocator> alloc_;
  ComPtr<ID3D12GraphicsCommandList> list_;
  ComPtr<ID3D12Fence> fence_;
  uint64_t fence_value_ = 0;
  uint8_t* membase_ = nullptr;
  std::unique_ptr<TextureCache> cache_;
};

GpuCheck* g_gpu = nullptr;
uint32_t g_gpu_match = 0, g_gpu_fail = 0;

enum class Result { kMatch, kMismatch, kSkipped };

Result Check(const fs::path& path, bool verbose, std::vector<uint8_t>* scratch) {
  Dump d;
  std::string name = path.filename().string();
  if (!Parse(path, &d)) {
    std::printf("BAD   %s: truncated or not a PTXD v1 dump\n", name.c_str());
    return Result::kMismatch;
  }
  TextureKey key = d.xkey;
  if (d.fetch_found) {
    TextureKey from_fetch;
    if (!TextureKeyFromFetch(d.fetch, &from_fetch)) {
      std::printf("FAIL  %s: fetch constant gives no texture; Xenos key %s\n", name.c_str(),
                  KeyText(d.xkey).c_str());
      return Result::kMismatch;
    }
    if (from_fetch != d.xkey) {
      std::printf("FAIL  %s: key from fetch %s != Xenos key %s\n", name.c_str(),
                  KeyText(from_fetch).c_str(), KeyText(d.xkey).c_str());
      return Result::kMismatch;
    }
    key = from_fetch;
  }
  const HostFormat* host = GetHostFormat(key.format);
  if (!host) {
    std::printf("SKIP  %s: %s not in the native format table\n", name.c_str(),
                KeyText(key).c_str());
    return Result::kSkipped;
  }
  std::vector<HostSubresource> subs;
  uint64_t size = GetHostFootprints(key, false, &subs);
  if (subs.size() != d.subs.size()) {
    std::printf("FAIL  %s: %zu native subresources vs %zu Xenos (%s)\n", name.c_str(),
                subs.size(), d.subs.size(), KeyText(key).c_str());
    return Result::kMismatch;
  }
  std::vector<uint8_t> out(size);
  GuestTextureMemory mem{d.base, d.base_bytes, d.mips, d.mips_bytes};
  UntileStats stats;
  UntileTexture(key, mem, subs, out.data(), scratch, &stats);

  uint64_t compared = 0, differing = 0;
  std::string first_diff;
  for (size_t i = 0; i < subs.size(); ++i) {
    const HostSubresource& n = subs[i];
    const XenosSub& x = d.subs[i];
    if (n.block_rows != x.rows || n.row_bytes != x.row_size || n.depth != x.depth) {
      std::printf("FAIL  %s: subresource %zu (level %u slice %u) native %u rows x %u bytes x %u "
                  "vs Xenos %u x %u x %u (Xenos DXGI %u, load shader %u; decompressed?)\n",
                  name.c_str(), i, n.level, n.slice, n.block_rows, n.row_bytes, n.depth, x.rows,
                  x.row_size, x.depth, d.dxgi_format, d.load_shader);
      return Result::kMismatch;
    }
    for (uint32_t z = 0; z < n.depth; ++z) {
      for (uint32_t y = 0; y < n.block_rows; ++y) {
        const uint8_t* a = out.data() + n.offset + (size_t(z) * n.block_rows + y) * n.row_pitch;
        const uint8_t* b = x.data + (size_t(z) * x.rows + y) * x.row_size;
        compared += n.row_bytes;
        if (std::memcmp(a, b, n.row_bytes) == 0) continue;
        for (uint32_t c = 0; c < n.row_bytes; ++c) {
          if (a[c] == b[c]) continue;
          ++differing;
          if (first_diff.empty()) {
            char t[160];
            std::snprintf(t, sizeof(t), "level %u slice %u z %u row %u byte %u: native %02X xenos %02X",
                          n.level, n.slice, z, y, c, a[c], b[c]);
            first_diff = t;
          }
        }
      }
    }
  }
  bool host_format_ok = uint32_t(host->resource) == d.dxgi_format;
  if (differing == 0) {
    std::printf("MATCH %s: %s, %zu subresources, %llu bytes identical%s%s\n", name.c_str(),
                KeyText(key).c_str(), subs.size(), (unsigned long long)compared,
                host_format_ok ? "" : " (Xenos resource DXGI format differs!)",
                stats.blocks_out_of_range ? " (some blocks past the dumped guest range)" : "");
    if (g_gpu && d.fetch_found) {
      uint64_t gpu_bytes = 0;
      if (g_gpu->Run(d, name, &gpu_bytes)) {
        ++g_gpu_match;
        if (verbose) std::printf("GPU   %s: D3D12 texture identical (%llu bytes)\n", name.c_str(),
                                 (unsigned long long)gpu_bytes);
      } else {
        ++g_gpu_fail;
        return Result::kMismatch;
      }
    }
    return host_format_ok ? Result::kMatch : Result::kMismatch;
  }
  std::printf("DIFF  %s: %s, %llu of %llu bytes differ; first at %s%s\n", name.c_str(),
              KeyText(key).c_str(), (unsigned long long)differing,
              (unsigned long long)compared, first_diff.c_str(),
              d.fetch_found ? "" : " (no matching fetch constant in the dump)");
  if (verbose) {
    std::printf("      fetch %08X %08X %08X %08X %08X %08X, blocks %llu, out of range %llu\n",
                d.fetch[0], d.fetch[1], d.fetch[2], d.fetch[3], d.fetch[4], d.fetch[5],
                (unsigned long long)stats.blocks, (unsigned long long)stats.blocks_out_of_range);
  }
  return Result::kMismatch;
}

}  // namespace

int main(int argc, char** argv) {
  bool verbose = false, d3d12 = false;
  std::vector<fs::path> files;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--verbose") { verbose = true; continue; }
    if (a == "--d3d12") { d3d12 = true; continue; }
    fs::path p(a);
    if (fs::is_directory(p)) {
      for (const auto& e : fs::directory_iterator(p))
        if (e.path().extension() == ".ptxd") files.push_back(e.path());
    } else {
      files.push_back(p);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: texture_harness <dump.ptxd | dir>... [--verbose] [--d3d12]\n");
    return 2;
  }
  std::sort(files.begin(), files.end());
  GpuCheck gpu;
  if (d3d12) {
    if (!gpu.Init()) {
      std::fprintf(stderr, "D3D12 device setup failed\n");
      return 2;
    }
    g_gpu = &gpu;
  }
  std::vector<uint8_t> scratch;
  uint32_t match = 0, mismatch = 0, skipped = 0;
  for (const fs::path& f : files) {
    switch (Check(f, verbose, &scratch)) {
      case Result::kMatch: ++match; break;
      case Result::kMismatch: ++mismatch; break;
      case Result::kSkipped: ++skipped; break;
    }
  }
  std::printf("summary: %u match, %u differ, %u skipped (unsupported format), %zu dumps\n", match,
              mismatch, skipped, files.size());
  if (g_gpu) {
    const TextureCache::Stats& s = gpu.stats();
    std::printf("d3d12: %u textures identical after TextureCache upload, %u failed; %llu uploads, "
                "%.1f MB, CPU untile %.1f ms total (%.1f us per upload)\n",
                g_gpu_match, g_gpu_fail, (unsigned long long)s.uploads, s.upload_bytes / 1e6,
                s.untile_ns / 1e6, s.uploads ? s.untile_ns / 1e3 / s.uploads : 0.0);
  }
  return mismatch ? 1 : 0;
}
