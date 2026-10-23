// pgr4-native PGR4 native renderer: the CPU side of guest textures.
//
// Pure functions with no D3D12 objects and no ReXGlue runtime state, so the standalone harness
// (tools/texture_harness.cpp) can run them on guest-memory dumps:
//   - TextureKeyFromFetch: the storage-relevant part of a texture fetch constant (what the
//     cache keys on), derived the same way as ReXGlue's TextureCache::BindingInfoFromFetchConstant.
//   - GetHostFormat: Xenos format -> DXGI format and the per-block conversion, the same host
//     formats as ReXGlue's D3D12TextureCache::host_formats_ for the formats PGR4 uses.
//   - GetHostFootprints / UntileTexture: guest layout (texture_util::GetGuestTextureLayout,
//     GetTiledOffset2D/3D, GetPackedMipOffset from the SDK's BSD rex/graphics/pipeline/texture/
//     util.h) -> host subresources in D3D12 order (level-major within each array slice).
//
// The texture_util helpers are not exported by rexgpu-xenos.dll; the SDK's util.cpp and
// info_formats.cpp are compiled into the game (build.ps1) and into the harness.
#pragma once

#include <rex/graphics/xenos.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <dxgiformat.h>

namespace pgr4::native {

// Storage-relevant fields of a texture fetch constant. Two fetch constants with the same key
// read the same guest memory the same way, so they share one host texture; the swizzle,
// signedness, filtering and clamping stay per binding.
struct TextureKey {
  uint32_t base_page = 0;   // physical base address >> 12 (0: no base level)
  uint32_t mip_page = 0;    // physical mip address >> 12 (0: no mips)
  uint32_t width = 0;       // texels
  uint32_t height = 0;
  uint32_t depth_or_array_size = 0;  // 3D depth, or array layers (6 for cube)
  uint32_t pitch = 0;       // base row pitch in texels / 32 (fetch dword 0)
  uint32_t mip_max_level = 0;
  rex::graphics::xenos::TextureFormat format{};  // base format (GetBaseFormat applied)
  rex::graphics::xenos::Endian endianness{};
  rex::graphics::xenos::DataDimension dimension{};
  bool tiled = false;
  bool packed_mips = false;

  bool operator==(const TextureKey& o) const;
  bool operator!=(const TextureKey& o) const { return !(*this == o); }
  size_t Hash() const;
  uint32_t mip_levels() const { return mip_max_level + 1; }
  bool is_3d() const { return dimension == rex::graphics::xenos::DataDimension::k3D; }
  uint32_t array_size() const { return is_3d() ? 1 : depth_or_array_size; }
  uint32_t depth() const { return is_3d() ? depth_or_array_size : 1; }
};
struct TextureKeyHasher {
  size_t operator()(const TextureKey& k) const { return k.Hash(); }
};

// False when the fetch constant has no texture data (invalid type, no base and no mips, or a
// 1D texture with tiling/packed mips, which the Xenos plugin also rejects).
bool TextureKeyFromFetch(const uint32_t fetch[6], TextureKey* key);

// Per-block conversion from the endian-swapped guest block to the host block.
enum class TextureConversion : uint8_t {
  kCopy,
  kR5G6B5ToB5G6R5,             // k_5_6_5: swap the 5-bit R and B fields
  kR5G5B6ToB5G6R5WithRBGASwizzle,  // k_6_5_5: host swizzle R,B,G,G restores the order
  kDepthUnorm24ToFloat32,      // k_24_8: R32_FLOAT = unorm24(d >> 8)
  kDepthFloat24ToFloat32,      // k_24_8_FLOAT: R32_FLOAT = float20e4(d >> 8)
};

struct HostFormat {
  DXGI_FORMAT resource;       // resource format (TYPELESS when both views exist)
  DXGI_FORMAT unorm;          // SRV for unsigned (and float) components
  DXGI_FORMAT snorm;          // SRV for signed components, DXGI_FORMAT_UNKNOWN if none
  TextureConversion conversion;
  bool block_compressed;      // BCn: 4x4 blocks are copied as is
  uint32_t host_swizzle;      // XE_GPU_TEXTURE_SWIZZLE_* of the host channels
};

// nullptr for formats PGR4 does not use yet (or that need a decompressing load).
const HostFormat* GetHostFormat(rex::graphics::xenos::TextureFormat base_format);

// Bit f is set when fetch constant format f (before GetBaseFormat) has a host format whose
// signed view is its unsigned view (the float and depth formats): signed components of such a
// texture fetch the same values as unsigned ones.
uint64_t SignedViewIsUnsignedFormats();

// D3D12 Shader4ComponentMapping for a fetch constant swizzle on a host format (ReXGlue's
// TextureCache::GuestToHostSwizzle; Xenos 0/1 constants map to FORCE_VALUE_0/1).
uint32_t GuestToHostComponentMapping(uint32_t guest_swizzle, uint32_t host_swizzle);

// One host subresource (D3D12 subresource index = level + slice * mip_levels).
struct HostSubresource {
  uint32_t level = 0, slice = 0;
  uint32_t width = 0, height = 0, depth = 0;  // texels, max(size >> level, 1)
  uint32_t block_columns = 0, block_rows = 0;  // host blocks (4x4 for BCn, else texels)
  uint32_t row_bytes = 0;                      // block_columns * host bytes per block
  uint32_t row_pitch = 0;                      // >= row_bytes
  uint64_t offset = 0;                         // of the subresource in the host buffer
  uint64_t size() const { return uint64_t(row_pitch) * block_rows * depth; }
};

// Host footprints of every subresource and the total buffer size. With d3d12_placement the
// row pitch is aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT and offsets to
// D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (an upload buffer); without, rows are tight
// (dump comparison). Returns 0 for unsupported formats.
uint64_t GetHostFootprints(const TextureKey& key, bool d3d12_placement,
                           std::vector<HostSubresource>* subresources);

// Guest memory of one texture: the bytes at the physical base and mip addresses. Sizes bound
// the reads (blocks past them read as zero and are counted).
struct GuestTextureMemory {
  const uint8_t* base = nullptr;
  size_t base_size = 0;
  const uint8_t* mips = nullptr;
  size_t mips_size = 0;
};

// Guest byte extents of the base level and the mips (texture_util::GetGuestTextureLayout):
// the ranges a cache must watch and a dump must contain.
void GetGuestExtents(const TextureKey& key, uint32_t* base_bytes, uint32_t* mips_bytes);

struct UntileStats {
  uint64_t blocks = 0;
  uint64_t blocks_out_of_range = 0;  // read past GuestTextureMemory sizes, written as zero
};

// Endian-swaps (per 32-bit word, as the Xenos texture fetch does) and untiles every
// subresource into dst (sized by GetHostFootprints with the same subresources), applying the
// host format conversion. `scratch` is reused between calls to hold the swapped guest data.
// Returns false for unsupported formats.
bool UntileTexture(const TextureKey& key, const GuestTextureMemory& memory,
                   const std::vector<HostSubresource>& subresources, uint8_t* dst,
                   std::vector<uint8_t>* scratch, UntileStats* stats = nullptr);

}  // namespace pgr4::native
