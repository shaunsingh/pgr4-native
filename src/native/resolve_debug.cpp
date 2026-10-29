// pgr4-native PGR4 native renderer: the resolve oracle dumps and the resolve self test
// (resolve.h).
#include "resolve.h"

#include "win_handle.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

REXCVAR_DEFINE_STRING(
    pgr4_native_resolve_dump, "", "PGR4 native",
    "Directory for resolve oracle dumps; create <dir>\\dump.request to dump the next frame");
REXCVAR_DEFINE_BOOL(pgr4_native_resolve_selftest, false, "PGR4 native",
                    "Check the resolve shader's format packing against known encodings at start");
REXCVAR_DECLARE(int32_t, pgr4_native_capture_hold_ms);

namespace pgr4::native {
namespace {

constexpr uint32_t kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

uint32_t g_full_readback_holds = 0;
std::string g_readback_before;

uint32_t CopyControlOf(const ResolveParams& p) {
  return p.source | (p.sample_select << 4) | p.clear_flags | (p.copy_command << 20);
}
uint32_t DestInfoOf(const ResolveParams& p) {
  return p.dest_endian | (p.dest_format << 7) | (p.dest_number << 13) |
         ((uint32_t(p.exp_bias) & 0x3F) << 16) | (uint32_t(p.dest_swap) << 24);
}

}  // namespace

void HoldFullReadback(bool hold) {
  // The plugin reads the cvar per resolve on its GPU thread while SetFlagByName writes the
  // std::string: a data race, known and left in as this path only runs for oracles. "fast",
  // "some" and "full" fit the short-string buffer, so the write never reallocates under the
  // reader, which at worst sees a torn value for one resolve.
  if (hold) {
    if (g_full_readback_holds++ == 0) {
      g_readback_before = rex::cvar::GetFlagByName("readback_resolve");
      if (!rex::cvar::SetFlagByName("readback_resolve", "full"))
        REXLOG_WARN(
            "PGR4 native: could not set readback_resolve=full; guest copies may be a frame old");
    }
  } else if (g_full_readback_holds && --g_full_readback_holds == 0) {
    rex::cvar::SetFlagByName("readback_resolve",
                             g_readback_before.empty() ? "none" : g_readback_before);
  }
}

struct Resolver::Dump {
  std::filesystem::path dir;
  bool recording = false;
  uint64_t frame = 0, fence = 0;  // the dumped frame and the fence it signals
  struct Entry {
    ResolveParams p;
    uint32_t width, height, row_pitch, bpp;
    ComPtr<ID3D12Resource> readback;
    std::vector<uint8_t> guest;  // guest memory at dest_base after the frame (AfterSwap)
  };
  std::vector<Entry> entries;
  bool guest_read = false;
};

void Resolver::DumpDeleter::operator()(Dump* dump) const { delete dump; }

void Resolver::InitializeOracles(ID3D12CommandQueue* queue) {
  const std::string dir = REXCVAR_GET(pgr4_native_resolve_dump);
  if (!dir.empty()) {
    dump_.reset(new Dump());
    dump_->dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(dump_->dir, ec);
    REXLOG_INFO("PGR4 native resolve: dumps to {} (create dump.request there)", dir);
  }
  if (REXCVAR_GET(pgr4_native_resolve_selftest) && queue)
    REXLOG_INFO("PGR4 native resolve selftest: {}", SelfTest(queue) ? "all passed" : "FAILED");
}

// ---------------------------------------------------------------------------------------------
// Oracle dumps

void Resolver::DumpRecord(CommandStream* list, const ResolveParams& p, const ResolvedImage& img,
                          uint32_t width, uint32_t height) {
  Dump& d = *dump_;
  if (!d.recording) return;
  Dump::Entry e;
  e.p = p;
  e.width = width;
  e.height = height;
  e.bpp = DestBytesPerTexel(p.dest_format);
  e.row_pitch = (width * e.bpp + kPitchAlign - 1) & ~(kPitchAlign - 1);
  if (!CreateBuffer(device_, D3D12_HEAP_TYPE_READBACK, uint64_t(e.row_pitch) * height,
                    D3D12_RESOURCE_STATE_COPY_DEST, "resolve dump readback", &e.readback))
    return;
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = img.texture;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = e.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {DestResourceFormat(p.dest_format), width, height, 1,
                                   e.row_pitch};
  D3D12_BOX box = {0, 0, 0, width, height, 1};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  d.entries.push_back(std::move(e));
}

void Resolver::AfterSwap() {
  if (!dump_ || !dump_->recording) return;
  Dump& d = *dump_;
  d.recording = false;
  // Let the Xenos plugin's GPU thread execute the dumped frame (its resolves read back
  // synchronously, HoldFullReadback) before reading memory. This holds the guest in that
  // frame's Swap only when the frame is recorded on the guest thread; on the render thread (the
  // default) the guest runs on, and the copies may hold a later frame's writes.
  Sleep(DWORD(std::max(0, REXCVAR_GET(pgr4_native_capture_hold_ms))));
  const uint8_t* physical = rex::system::kernel_memory()->physical_membase();
  for (Dump::Entry& e : d.entries) {
    const uint8_t* g = physical + e.p.dest_base;
    e.guest.assign(g, g + e.p.dest_extent_bytes);
  }
  d.guest_read = true;
  HoldFullReadback(false);
  REXLOG_INFO("PGR4 native resolve: read guest memory of {} resolves of frame {}", d.entries.size(),
              d.frame);
}

void Resolver::DumpBeginFrame(uint64_t completed_fence) {
  Dump& d = *dump_;
  const std::string prefix = "f" + std::to_string(d.frame);
  if (d.guest_read && completed_fence >= d.fence) {
    std::FILE* meta = std::fopen((d.dir / (prefix + "-resolves.jsonl")).string().c_str(), "wb");
    for (size_t i = 0; i < d.entries.size(); ++i) {
      Dump::Entry& e = d.entries[i];
      const ResolveParams& p = e.p;
      const std::string stem = prefix + "-r" + std::to_string(i);
      void* data = nullptr;
      D3D12_RANGE range = {0, size_t(e.row_pitch) * e.height};
      if (SUCCEEDED(e.readback->Map(0, &range, &data))) {
        std::FILE* f = std::fopen((d.dir / (stem + "-native.bin")).string().c_str(), "wb");
        if (f) {
          for (uint32_t y = 0; y < e.height; ++y)
            std::fwrite(static_cast<const uint8_t*>(data) + size_t(y) * e.row_pitch, 1,
                        size_t(e.width) * e.bpp, f);
          std::fclose(f);
        }
        D3D12_RANGE none = {0, 0};
        e.readback->Unmap(0, &none);
      }
      if (std::FILE* f = std::fopen((d.dir / (stem + "-guest.bin")).string().c_str(), "wb")) {
        std::fwrite(e.guest.data(), 1, e.guest.size(), f);
        std::fclose(f);
      }
      if (meta)
        std::fprintf(meta,
                     "{\"index\":%zu,\"source\":%u,\"surface_info\":%u,\"source_info\":%u,"
                     "\"sample_select\":%u,\"copy_command\":%u,\"rect\":[%d,%d,%d,%d],"
                     "\"dest_point\":[%d,%d],\"dest_base\":%u,\"dest_format\":%u,\"dest_width\":%u,"
                     "\"dest_height\":%u,\"endian\":%u,\"number\":%u,\"swap\":%d,\"exp_bias\":%d,"
                     "\"tiled\":%d,\"pitch_blocks\":%u,\"extent_bytes\":%u,\"bpp\":%u,"
                     "\"native_width\":%u,\"native_height\":%u,\"clear_flags\":%u,"
                     "\"copy_control\":%u,\"dest_info\":%u}\n",
                     i, p.source, p.regs.surface_info,
                     p.source == 4 ? p.regs.depth_info : p.regs.color_info[p.source & 3],
                     p.sample_select, p.copy_command, p.x0, p.y0, p.x1, p.y1, p.dest_x, p.dest_y,
                     p.dest_base, p.dest_format, p.dest_width, p.dest_height, p.dest_endian,
                     p.dest_number, p.dest_swap ? 1 : 0, p.exp_bias, p.dest_tiled ? 1 : 0,
                     p.dest_pitch_blocks, p.dest_extent_bytes, e.bpp, e.width, e.height,
                     p.clear_flags, CopyControlOf(p), DestInfoOf(p));
    }
    if (meta) std::fclose(meta);
    REXLOG_INFO("PGR4 native resolve: dumped {} resolves (native + guest) of frame {} to {}",
                d.entries.size(), d.frame, d.dir.string());
    d.entries.clear();
    d.guest_read = false;
  }
  if (!d.recording && !d.guest_read) {
    std::error_code ec;
    if (std::filesystem::remove(d.dir / "dump.request", ec)) {
      HoldFullReadback(true);  // before any of this frame's resolves reach the plugin
      d.recording = true;
      d.frame = frame_;
      d.fence = frame_fence_;  // signalled when the dumped frame is done on the GPU
      REXLOG_INFO("PGR4 native resolve: dumping the resolves of frame {}", frame_);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Self test

bool Resolver::SelfTest(ID3D12CommandQueue* queue) {
  // The test's clears are no guest EDRAM content: on every return, drop the fills they recorded
  // over tiles that game surfaces use later.
  struct FillsDropper {
    RenderTargets* targets;
    ~FillsDropper() { targets->DropAllFills(); }
  } fills_dropper{targets_};
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  if (Failed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
             "selftest allocator") ||
      Failed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                        IID_PPV_ARGS(&list)),
             "selftest list") ||
      Failed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
             "selftest fence"))
    return false;
  list->Close();
  // The resolver records into a stream, made on the list while this thread waits (Sync).
  CommandStream stream;
  stream.Start(list.Get(), L"PGR4 native resolve self test");
  struct Case {
    const char* name;
    bool depth;
    uint32_t msaa;    // RB_SURFACE_INFO msaa_samples
    uint32_t format;  // color or depth render target format
    float color[4];
    float z;
    uint8_t stencil;
    uint32_t dest_format, copy_command, sample_select;
    bool swap;
    int32_t bias;
    uint32_t expect[2];  // first one or two dwords of every texel
    float expect_r32f;
  };
  // Expected values: the Xenos encodings, worked out by hand (unorm: v * max + 0.5 truncated;
  // 20e4 and 7e3 as xenos.h; halves exact). 7e3 sources go through the EDRAM's 7e3 first:
  // 1e-6 is below 7e3's smallest step and becomes 0, alpha 0.5 becomes 2/3 (2-bit unorm).
  const Case cases[] = {
      {"8888 1x -> k_8_8_8_8",
       false,
       0,
       0,
       {64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f},
       0,
       0,
       6,
       1,
       0,
       false,
       0,
       {0xFFBF8040u, 0},
       0},
      {"8888 1x -> k_8_8_8_8 swap",
       false,
       0,
       0,
       {64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f},
       0,
       0,
       6,
       1,
       0,
       true,
       0,
       {0xFF4080BFu, 0},
       0},
      {"7e3 4x avg4 -> k_16_16_16_16_FLOAT",
       false,
       2,
       3,
       {1.5f, 0.25f, 1e-6f, 0.5f},
       0,
       0,
       32,
       1,
       6,
       false,
       0,
       {0x34003E00u, 0x39550000u},
       0},
      {"7e3 1x -> k_8_8_8_8 (saturate)",
       false,
       0,
       3,
       {1.5f, 0.25f, 0.0f, 0.5f},
       0,
       0,
       6,
       1,
       0,
       false,
       0,
       {0xAA0040FFu, 0},
       0},
      // Ties round to even: 2.5 denormal steps -> 2, 1 + 1/256 -> 1, 1 + 3/256 -> 1 + 2/128.
      {"7e3 1x ties -> k_16_16_16_16_FLOAT",
       false,
       0,
       3,
       {2.5f / 512, 1.00390625f, 1.01171875f, 1.0f},
       0,
       0,
       32,
       1,
       0,
       false,
       0,
       {0x3C001C00u, 0x3C003C10u},
       0},
      {"7e3_AS_16 2x avg2 -> k_2_10_10_10",
       false,
       1,
       12,
       {0.5f, 0.25f, 1.0f, 1.0f},
       0,
       0,
       7,
       1,
       4,
       false,
       0,
       {0xFFF40200u, 0},
       0},
      {"8888 1x -> k_5_6_5",
       false,
       0,
       0,
       {1.0f, 0.0f, 0.0f, 1.0f},
       0,
       0,
       4,
       1,
       0,
       false,
       0,
       {0xF800u, 0},
       0},
      {"8888 1x -> k_6_5_5",
       false,
       0,
       0,
       {0.0f, 1.0f, 0.0f, 1.0f},
       0,
       0,
       5,
       1,
       0,
       false,
       0,
       {0xF800u, 0},
       0},
      {"32F 1x -> k_32_FLOAT",
       false,
       0,
       14,
       {3.0f, 0, 0, 0},
       0,
       0,
       36,
       1,
       0,
       false,
       0,
       {0x40400000u, 0},
       0},
      {"16F 1x bias -1 -> k_16_16_16_16_FLOAT",
       false,
       0,
       7,
       {2.0f, 1.0f, 0.5f, 4.0f},
       0,
       0,
       32,
       1,
       0,
       false,
       -1,
       {0x38003C00u, 0x40003400u},
       0},
      {"D24FS8 2x raw -> k_8_8_8_8",
       true,
       1,
       1,
       {},
       0.5f,
       0x5A,
       6,
       0,
       0,
       false,
       0,
       {0xE000005Au, 0},
       0.5f},
      // 0.5 + 7 ulp: ReXGlue truncates host D32 to 20e4 (round to nearest even would give E00001).
      {"D24FS8 1x truncate -> k_8_8_8_8",
       true,
       0,
       1,
       {},
       std::bit_cast<float>(0x3F000007u),
       0x5A,
       6,
       0,
       0,
       false,
       0,
       {0xE000005Au, 0},
       0.5f},
      {"D24S8 1x raw -> k_8_8_8_8",
       true,
       0,
       0,
       {},
       0.25f,
       0x11,
       6,
       0,
       0,
       false,
       0,
       {0x40000011u, 0},
       float(0x400000) / 16777215.0f},
  };
  constexpr uint32_t kPitch = 64, kDestW = 64, kDestH = 32;
  const int32_t rx0 = 8, ry0 = 8, rx1 = 40, ry1 = 24, px = 16, py = 4;  // rect and dest point
  uint64_t fence_value = 0;
  bool all_ok = true;
  // Every case through the generic pipeline, then through its specialized one.
  for (const bool specialized : {false, true}) {
    const char* const mode_name = specialized ? "specialized" : "generic";
    SetSpecialization(specialized);
    uint32_t index = 0;
    for (const Case& c : cases) {
      ResolveParams p;
      p.regs.surface_info = kPitch | (c.msaa << 16);
      const uint32_t base = 1024 + index * 32;  // EDRAM bases the game does not use at start
      if (c.depth)
        p.regs.depth_info = base | (c.format << 16);
      else
        p.regs.color_info[0] = base | (c.format << 16);
      p.source = c.depth ? 4 : 0;
      p.sample_select = c.sample_select;
      p.copy_command = c.copy_command;
      p.x0 = rx0;
      p.y0 = ry0;
      p.x1 = rx1;
      p.y1 = ry1;
      p.dest_x = px;
      p.dest_y = py;
      p.dest_base = 0xFFF00000u + index * 0x10000u;  // not a guest address: never found by textures
      p.dest_format = c.dest_format;
      p.dest_width = kDestW;
      p.dest_height = kDestH;
      p.dest_number = IsFloatDest(c.dest_format) ? 7 : 0;
      p.dest_swap = c.swap;
      p.exp_bias = c.bias;
      ++index;

      alloc->Reset();
      list->Reset(alloc.Get(), nullptr);
      ClearParams cp;
      cp.flags = c.depth ? 0x30 : 0x1;
      std::memcpy(cp.color, c.color, sizeof(cp.color));
      cp.depth = c.z;
      cp.stencil = c.stencil;
      targets_->Clear(&stream, p.regs, 1, c.depth, kDestH * 2, cp);
      bool ok = Resolve(&stream, p);
      const ResolvedImage* img = ok ? Find(p.dest_base) : nullptr;
      const uint32_t bpp = DestBytesPerTexel(c.dest_format);
      const uint32_t w = uint32_t(rx1 - rx0), h = uint32_t(ry1 - ry0);
      const uint32_t row_pitch = (kDestW * bpp + kPitchAlign - 1) & ~(kPitchAlign - 1);
      ComPtr<ID3D12Resource> readback[2];
      if (img) {
        for (int k = 0; k < (img->depth_r32f ? 2 : 1); ++k) {
          if (!CreateBuffer(device_, D3D12_HEAP_TYPE_READBACK, uint64_t(row_pitch) * kDestH,
                            D3D12_RESOURCE_STATE_COPY_DEST, "selftest readback", &readback[k]))
            return false;
          D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
          src.pResource = k ? img->depth_r32f : img->texture;
          src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          dst.pResource = readback[k].Get();
          dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          dst.PlacedFootprint.Footprint = {
              k ? DXGI_FORMAT_R32_FLOAT : DestResourceFormat(c.dest_format), kDestW, kDestH, 1,
              row_pitch};
          stream.CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
      }
      stream.Sync();
      list->Close();
      ID3D12CommandList* lists[] = {list.Get()};
      queue->ExecuteCommandLists(1, lists);
      queue->Signal(fence.Get(), ++fence_value);
      const UniqueHandle done = CreateAutoResetEvent();
      if (!done || FAILED(fence->SetEventOnCompletion(fence_value, done.get())) ||
          WaitForSingleObject(done.get(), 5000) != WAIT_OBJECT_0) {
        REXLOG_ERROR("PGR4 native resolve selftest: {} {}: the GPU did not finish", mode_name,
                     c.name);
        return false;
      }
      uint32_t bad = 0, first_bad[2] = {}, first_x = 0, first_y = 0;
      float r32f_seen = 0;
      if (img) {
        void* mapped[2] = {};
        if (Failed(readback[0]->Map(0, nullptr, &mapped[0]), "selftest Map") ||
            (readback[1] && Failed(readback[1]->Map(0, nullptr, &mapped[1]), "selftest Map")))
          return false;
        const auto* data = static_cast<const uint8_t*>(mapped[0]);
        const auto* fdata = static_cast<const uint8_t*>(mapped[1]);
        for (uint32_t y = 0; y < h; ++y) {
          for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* t = data + size_t(py + y) * row_pitch + size_t(px + x) * bpp;
            uint32_t v[2] = {};
            std::memcpy(v, t, std::min(bpp, 8u));
            bool texel_ok = v[0] == c.expect[0] && (bpp < 8 || v[1] == c.expect[1]);
            if (fdata) {
              float f;
              std::memcpy(&f, fdata + size_t(py + y) * row_pitch + size_t(px + x) * 4, 4);
              r32f_seen = f;
              texel_ok = texel_ok && std::fabs(f - c.expect_r32f) <= 1e-6f;
            }
            if (!texel_ok && bad++ == 0) {
              first_bad[0] = v[0];
              first_bad[1] = v[1];
              first_x = x;
              first_y = y;
            }
          }
        }
        readback[0]->Unmap(0, nullptr);
        if (readback[1]) readback[1]->Unmap(0, nullptr);
        EraseDestination(p.dest_base);
      }
      const bool pass = img && bad == 0;
      all_ok = all_ok && pass;
      if (pass)
        REXLOG_INFO("PGR4 native resolve selftest: ok   {} {} ({:08X} {:08X})", mode_name, c.name,
                    c.expect[0], c.expect[1]);
      else
        REXLOG_ERROR(
            "PGR4 native resolve selftest: FAIL {} {}: {} of {} texels wrong, first at ({}, {}) = "
            "{:08X} {:08X}, expected {:08X} {:08X}, r32f {} (expected {}){}",
            mode_name, c.name, bad, w * h, first_x, first_y, first_bad[0], first_bad[1],
            c.expect[0], c.expect[1], r32f_seen, c.expect_r32f, img ? "" : ", resolve failed");
    }
  }
  return all_ok;
}

}  // namespace pgr4::native
