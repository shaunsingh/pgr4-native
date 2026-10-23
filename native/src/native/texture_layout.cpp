// pgr4-native PGR4 native renderer: guest texture layout, host formats and CPU untiling.
// See texture_layout.h. The host format choices, the fetch constant -> key derivation and the
// swizzle composition follow ReXGlue's (Xenia's, BSD-3) TextureCache/D3D12TextureCache so the
// untiled data matches the Xenos plugin's uploads byte for byte.
#include "texture_layout.h"

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>

#include <algorithm>
#include <cstring>

namespace pgr4::native {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
using rex::graphics::FormatInfo;
using xenos::TextureFormat;

bool TextureKey::operator==(const TextureKey& o) const {
  return base_page == o.base_page && mip_page == o.mip_page && width == o.width &&
         height == o.height && depth_or_array_size == o.depth_or_array_size && pitch == o.pitch &&
         mip_max_level == o.mip_max_level && format == o.format && endianness == o.endianness &&
         dimension == o.dimension && tiled == o.tiled && packed_mips == o.packed_mips;
}

size_t TextureKey::Hash() const {
  uint64_t a = uint64_t(base_page) | uint64_t(mip_page) << 20 |
               uint64_t(uint32_t(format)) << 40 | uint64_t(uint32_t(endianness)) << 46 |
               uint64_t(uint32_t(dimension)) << 48 | uint64_t(tiled) << 50 |
               uint64_t(packed_mips) << 51 | uint64_t(mip_max_level) << 52;
  uint64_t b = uint64_t(width) | uint64_t(height) << 24 | uint64_t(depth_or_array_size) << 48 |
               uint64_t(pitch) << 54;
  uint64_t h = a * 0x9E3779B97F4A7C15ull;
  h ^= (b + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2));
  return size_t(h ^ (h >> 31));
}

bool TextureKeyFromFetch(const uint32_t fetch_dwords[6], TextureKey* key) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  // ReXGlue rejects kInvalidTexture unless --gpu_allow_invalid_fetch_constants.
  if (fetch.type != xenos::FetchConstantType::kTexture) return false;
  uint32_t w1, h1, d1, base_page, mip_page, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &w1, &h1, &d1, &base_page, &mip_page,
                                                 nullptr, &mip_max_level);
  if (base_page == 0 && mip_page == 0) return false;
  if (fetch.dimension == xenos::DataDimension::k1D &&
      (w1 >= xenos::kTexture2DCubeMaxWidthHeight || fetch.tiled || fetch.packed_mips)) {
    return false;
  }
  TextureKey k;
  k.base_page = base_page;
  k.mip_page = mip_page;
  k.width = w1 + 1;
  k.height = h1 + 1;
  k.depth_or_array_size = d1 + 1;
  k.pitch = fetch.pitch;
  k.mip_max_level = mip_max_level;
  k.format = rex::graphics::GetBaseFormat(fetch.format);
  k.endianness = fetch.endianness;
  k.dimension = fetch.dimension;
  k.tiled = fetch.tiled;
  k.packed_mips = fetch.packed_mips;
  *key = k;
  return true;
}

namespace {

constexpr uint32_t kRGBA = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
constexpr uint32_t kRGBB = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB;
constexpr uint32_t kRGGG = xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;
constexpr uint32_t kRRRR = xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
constexpr uint32_t kRBGG = XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G);

using C = TextureConversion;
// Same DXGI formats and load conversions as D3D12TextureCache::host_formats_ (non-decompressing
// path; BCn with unaligned sizes needs D3D12 UnalignedBlockTexturesSupported, checked by the
// cache). Formats not listed are not used by PGR4's menus and race (tap traces) and are
// reported as unsupported.
const HostFormat kFormats[] = {
    // k_8
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, C::kCopy, false, kRRRR},
    // k_5_6_5
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_UNKNOWN, C::kR5G6B5ToB5G6R5,
     false, kRGBB},
    // k_6_5_5
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_UNKNOWN,
     C::kR5G5B6ToB5G6R5WithRBGASwizzle, false, kRBGG},
    // k_8_8_8_8
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM,
     C::kCopy, false, kRGBA},
    // k_2_10_10_10
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN,
     C::kCopy, false, kRGBA},
    // k_8_8
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM, C::kCopy, false,
     kRGGG},
    // k_DXT1
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_UNKNOWN, C::kCopy, true, kRGBA},
    // k_DXT2_3
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_UNKNOWN, C::kCopy, true, kRGBA},
    // k_DXT4_5
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_UNKNOWN, C::kCopy, true, kRGBA},
    // k_24_8 (R32_FLOAT: shaders get no stencil, as in ReXGlue)
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
     C::kDepthUnorm24ToFloat32, false, kRRRR},
    // k_24_8_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
     C::kDepthFloat24ToFloat32, false, kRRRR},
    // k_16
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM, C::kCopy, false,
     kRRRR},
    // k_16_16
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM, C::kCopy,
     false, kRGGG},
    // k_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     DXGI_FORMAT_R16G16B16A16_SNORM, C::kCopy, false, kRGBA},
    // k_16_FLOAT (also k_16_EXPAND)
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, C::kCopy, false, kRRRR},
    // k_16_16_FLOAT (also k_16_16_EXPAND)
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, C::kCopy, false,
     kRGGG},
    // k_16_16_16_16_FLOAT (also k_16_16_16_16_EXPAND)
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
     DXGI_FORMAT_R16G16B16A16_FLOAT, C::kCopy, false, kRGBA},
    // k_32_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, C::kCopy, false, kRRRR},
    // k_32_32_FLOAT
    {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, C::kCopy, false,
     kRGGG},
    // k_32_32_32_32_FLOAT
    {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
     DXGI_FORMAT_R32G32B32A32_FLOAT, C::kCopy, false, kRGBA},
    // k_DXN
    {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_UNKNOWN, C::kCopy, true, kRGGG},
};

uint32_t Float20e4To32Bits(uint32_t f24) {
  // xenos::Float20e4To32 (Xenia, BSD-3); that definition lives in rexgpu-xenos only.
  f24 &= 0xFFFFFF;
  if (!f24) return 0;
  uint32_t mantissa = f24 & 0xFFFFF;
  uint32_t exponent = f24 >> 20;
  if (!exponent) {
    uint32_t mantissa_lzcnt = rex::lzcnt(mantissa) - (32 - 21);
    exponent = uint32_t(1 - int32_t(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0xFFFFF;
  }
  return ((exponent + 112) << 23) | (mantissa << 3);
}

void Convert(TextureConversion conversion, uint8_t* row, uint32_t bytes) {
  switch (conversion) {
    case C::kCopy:
      return;
    case C::kR5G6B5ToB5G6R5:
      for (uint32_t i = 0; i + 2 <= bytes; i += 2) {
        uint16_t v;
        std::memcpy(&v, row + i, 2);
        v = uint16_t((v & 0x07E0) | ((v & 0x001F) << 11) | ((v & 0xF800) >> 11));
        std::memcpy(row + i, &v, 2);
      }
      return;
    case C::kR5G5B6ToB5G6R5WithRBGASwizzle:
      for (uint32_t i = 0; i + 2 <= bytes; i += 2) {
        uint16_t v;
        std::memcpy(&v, row + i, 2);
        v = uint16_t((v & 0x001F) | ((v & 0x03E0) << 6) | ((v & 0xFC00) >> 5));
        std::memcpy(row + i, &v, 2);
      }
      return;
    case C::kDepthUnorm24ToFloat32:
      for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
        uint32_t v;
        std::memcpy(&v, row + i, 4);
        float f = xenos::UNorm24To32(v >> 8);
        std::memcpy(row + i, &f, 4);
      }
      return;
    case C::kDepthFloat24ToFloat32:
      for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
        uint32_t v;
        std::memcpy(&v, row + i, 4);
        v = Float20e4To32Bits(v >> 8);
        std::memcpy(row + i, &v, 4);
      }
      return;
  }
}

// Copies `size` bytes of guest memory with the fetch constant's endian swap applied to every
// aligned 32-bit word (the source regions start at 4 KB pages).
void SwapCopy(uint8_t* dst, const uint8_t* src, size_t size, xenos::Endian endian) {
  if (endian == xenos::Endian::kNone) {
    std::memcpy(dst, src, size);
    return;
  }
  size_t words = size / 4;
  for (size_t i = 0; i < words; ++i) {
    uint32_t v;
    std::memcpy(&v, src + i * 4, 4);
    switch (endian) {
      case xenos::Endian::k8in16:
        v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
        break;
      case xenos::Endian::k8in32:
        v = (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
        break;
      case xenos::Endian::k16in32:
        v = (v << 16) | (v >> 16);
        break;
      default:
        break;
    }
    std::memcpy(dst + i * 4, &v, 4);
  }
  // A trailing partial word cannot be swapped without the bytes past it; leave it zero.
  std::memset(dst + words * 4, 0, size - words * 4);
}

}  // namespace

const HostFormat* GetHostFormat(TextureFormat f) {
  switch (f) {
    case TextureFormat::k_8: return &kFormats[0];
    case TextureFormat::k_5_6_5: return &kFormats[1];
    case TextureFormat::k_6_5_5: return &kFormats[2];
    case TextureFormat::k_8_8_8_8: return &kFormats[3];
    case TextureFormat::k_2_10_10_10: return &kFormats[4];
    case TextureFormat::k_8_8: return &kFormats[5];
    case TextureFormat::k_DXT1: return &kFormats[6];
    case TextureFormat::k_DXT2_3: return &kFormats[7];
    case TextureFormat::k_DXT4_5: return &kFormats[8];
    case TextureFormat::k_24_8: return &kFormats[9];
    case TextureFormat::k_24_8_FLOAT: return &kFormats[10];
    case TextureFormat::k_16: return &kFormats[11];
    case TextureFormat::k_16_16: return &kFormats[12];
    case TextureFormat::k_16_16_16_16: return &kFormats[13];
    case TextureFormat::k_16_FLOAT: return &kFormats[14];
    case TextureFormat::k_16_16_FLOAT: return &kFormats[15];
    case TextureFormat::k_16_16_16_16_FLOAT: return &kFormats[16];
    case TextureFormat::k_32_FLOAT: return &kFormats[17];
    case TextureFormat::k_32_32_FLOAT: return &kFormats[18];
    case TextureFormat::k_32_32_32_32_FLOAT: return &kFormats[19];
    case TextureFormat::k_DXN: return &kFormats[20];
    default: return nullptr;
  }
}

uint64_t SignedViewIsUnsignedFormats() {
  uint64_t formats = 0;
  for (uint32_t f = 0; f < 64; ++f) {
    const HostFormat* h = GetHostFormat(rex::graphics::GetBaseFormat(TextureFormat(f)));
    if (h && h->snorm == h->unorm) formats |= uint64_t(1) << f;
  }
  return formats;
}

uint32_t GuestToHostComponentMapping(uint32_t guest_swizzle, uint32_t host_swizzle) {
  uint32_t mapping = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t c = (guest_swizzle >> (3 * i)) & 0b111;
    // 4/5 are the constants 0/1 (D3D12 FORCE_VALUE_0/1 have the same values); 6/7 become 4/5.
    uint32_t h = c >= xenos::XE_GPU_TEXTURE_SWIZZLE_0 ? (c & 0b101) : (host_swizzle >> (3 * c)) & 0b111;
    mapping |= h << (3 * i);
  }
  return mapping | (1u << 12);  // D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES
}

void GetGuestExtents(const TextureKey& key, uint32_t* base_bytes, uint32_t* mips_bytes) {
  texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      key.dimension, key.pitch, key.width, key.height, key.depth_or_array_size, key.tiled,
      key.format, key.packed_mips, key.base_page != 0, key.mip_max_level);
  *base_bytes = key.base_page ? layout.base.level_data_extent_bytes : 0;
  *mips_bytes = key.mip_page ? layout.mips_total_extent_bytes : 0;
}

uint64_t GetHostFootprints(const TextureKey& key, bool d3d12_placement,
                           std::vector<HostSubresource>* subresources) {
  subresources->clear();
  const HostFormat* host = GetHostFormat(key.format);
  if (!host) return 0;
  const FormatInfo* info = FormatInfo::Get(key.format);
  uint32_t bw = host->block_compressed ? info->block_width : 1;
  uint32_t bh = host->block_compressed ? info->block_height : 1;
  // Host bytes per block equal the guest ones for every format in the table (the conversions
  // keep the size).
  uint32_t bpb = info->bytes_per_block();
  uint64_t offset = 0;
  for (uint32_t slice = 0; slice < key.array_size(); ++slice) {
    for (uint32_t level = 0; level <= key.mip_max_level; ++level) {
      HostSubresource s;
      s.level = level;
      s.slice = slice;
      s.width = std::max(key.width >> level, 1u);
      s.height = std::max(key.height >> level, 1u);
      s.depth = std::max(key.depth() >> level, 1u);
      s.block_columns = (s.width + bw - 1) / bw;
      s.block_rows = (s.height + bh - 1) / bh;
      s.row_bytes = s.block_columns * bpb;
      if (d3d12_placement) {
        offset = (offset + 511) & ~uint64_t(511);   // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
        s.row_pitch = (s.row_bytes + 255) & ~255u;  // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
      } else {
        s.row_pitch = s.row_bytes;
      }
      s.offset = offset;
      offset += s.size();
      subresources->push_back(s);
    }
  }
  return offset;
}

bool UntileTexture(const TextureKey& key, const GuestTextureMemory& memory,
                   const std::vector<HostSubresource>& subresources, uint8_t* dst,
                   std::vector<uint8_t>* scratch, UntileStats* stats) {
  const HostFormat* host = GetHostFormat(key.format);
  if (!host) return false;
  const FormatInfo* info = FormatInfo::Get(key.format);
  const uint32_t bpb = info->bytes_per_block();
  const uint32_t bpb_log2 = rex::log2_floor(bpb);
  texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      key.dimension, key.pitch, key.width, key.height, key.depth_or_array_size, key.tiled,
      key.format, key.packed_mips, key.base_page != 0, key.mip_max_level);
  const bool is_3d = key.is_3d();

  // Endian-swapped copies of the base and mip regions, then one row of scratch: rows that need
  // a conversion are built and converted there and copied out whole, because `dst` is usually
  // write-combined upload memory, where every read of Convert costs an uncached access
  // (~55 ns per byte measured: 14 ms for a 256x512 R5G6B5 texture).
  size_t base_size = std::min<size_t>(memory.base ? memory.base_size : 0,
                                      key.base_page ? layout.base.level_data_extent_bytes : 0);
  size_t mips_size = std::min<size_t>(memory.mips ? memory.mips_size : 0,
                                      key.mip_page ? layout.mips_total_extent_bytes : 0);
  const bool convert = host->conversion != TextureConversion::kCopy;
  size_t row_scratch = 0;
  if (convert)
    for (const HostSubresource& s : subresources) row_scratch = std::max<size_t>(row_scratch, s.row_bytes);
  scratch->resize(base_size + mips_size + row_scratch);
  uint8_t* base = scratch->data();
  uint8_t* mips = base + base_size;
  uint8_t* row_tmp = mips + mips_size;
  if (base_size) SwapCopy(base, memory.base, base_size, key.endianness);
  if (mips_size) SwapCopy(mips, memory.mips, mips_size, key.endianness);

  UntileStats st;
  for (const HostSubresource& s : subresources) {
    uint8_t* out = dst + s.offset;
    const bool from_base = s.level == 0;
    const uint32_t stored_level = std::min(s.level, layout.packed_level);
    const texture_util::TextureGuestLayout::Level& level_layout =
        from_base ? layout.base : layout.mips[stored_level];
    const uint8_t* src = from_base ? base : mips;
    const size_t src_size = from_base ? base_size : mips_size;
    if (from_base ? key.base_page == 0 : key.mip_page == 0) {
      for (uint32_t z = 0; z < s.depth; ++z)
        for (uint32_t y = 0; y < s.block_rows; ++y)
          std::memset(out + (size_t(z) * s.block_rows + y) * s.row_pitch, 0, s.row_bytes);
      continue;
    }
    uint64_t level_offset = from_base ? 0 : layout.mip_offsets_bytes[stored_level];
    level_offset += uint64_t(level_layout.array_slice_stride_bytes) * s.slice;
    uint32_t ox = 0, oy = 0, oz = 0;
    if (s.level >= layout.packed_level) {
      texture_util::GetPackedMipOffset(key.width, key.height, key.depth(), key.format, s.level,
                                       ox, oy, oz);
    }
    const uint32_t row_pitch = level_layout.row_pitch_bytes;
    const uint32_t pitch_blocks = row_pitch / bpb;  // tiled: pitch in blocks
    const uint32_t z_rows = level_layout.z_slice_stride_block_rows;
    // Blocks contiguous in tiled 2D memory along X (texture_util.h): 8 at 1 byte per block,
    // otherwise 16 bytes' worth, starting at a multiple of the run length. 3D tiling: per block.
    const uint32_t tiled_run = is_3d ? 1 : (bpb == 1 ? 8 : std::max(16u / bpb, 1u));
    for (uint32_t z = 0; z < s.depth; ++z) {
      for (uint32_t y = 0; y < s.block_rows; ++y) {
        uint8_t* const row_out = out + (size_t(z) * s.block_rows + y) * s.row_pitch;
        uint8_t* const row = convert ? row_tmp : row_out;
        const uint32_t gy = oy + y, gz = oz + z;
        for (uint32_t x = 0; x < s.block_columns;) {
          const uint32_t gx = ox + x;
          uint32_t run;
          int64_t off;
          if (key.tiled) {
            run = gx % tiled_run ? 1 : std::min(tiled_run, s.block_columns - x);
            off = is_3d ? texture_util::GetTiledOffset3D(int32_t(gx), int32_t(gy), int32_t(gz),
                                                         pitch_blocks, z_rows, bpb_log2)
                        : texture_util::GetTiledOffset2D(int32_t(gx), int32_t(gy), pitch_blocks,
                                                         bpb_log2);
          } else {
            run = s.block_columns - x;
            off = (int64_t(gz) * z_rows + gy) * row_pitch + int64_t(gx) * bpb;
          }
          off += int64_t(level_offset);
          const size_t bytes = size_t(run) * bpb;
          if (off >= 0 && uint64_t(off) + bytes <= src_size) {
            std::memcpy(row + size_t(x) * bpb, src + off, bytes);
          } else {
            // Past the guest extent: copy what is inside, zero the rest.
            for (uint32_t i = 0; i < run; ++i) {
              int64_t o = off + int64_t(i) * bpb;
              uint8_t* d = row + size_t(x + i) * bpb;
              if (o >= 0 && uint64_t(o) + bpb <= src_size) {
                std::memcpy(d, src + o, bpb);
              } else {
                std::memset(d, 0, bpb);
                ++st.blocks_out_of_range;
              }
            }
          }
          x += run;
        }
        if (convert) {
          Convert(host->conversion, row, s.row_bytes);
          std::memcpy(row_out, row, s.row_bytes);
        }
        st.blocks += s.block_columns;
      }
    }
  }
  if (stats) *stats = st;
  return true;
}

}  // namespace pgr4::native
