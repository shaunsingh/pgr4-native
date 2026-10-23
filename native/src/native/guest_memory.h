// pgr4-native PGR4 native renderer: guest memory as the generated code addresses it.
#pragma once

#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>
#undef DrawState  // winuser.h (via rex/platform.h) maps it to DrawStateA/W; d3d_state.h's struct

#include <bit>
#include <cstdint>

namespace pgr4::native {

// The host address of guest virtual address `a`, translated like the generated code's loads and
// stores (on Windows the 0xE0000000 view of physical memory sits 4 KiB higher in the mapping).
inline const uint8_t* HostPtr(const uint8_t* base, uint32_t a) {
  return base + a + rex::memory::detail::PhysicalHostOffset(a);
}
inline uint8_t* HostPtr(uint8_t* base, uint32_t a) {
  return base + a + rex::memory::detail::PhysicalHostOffset(a);
}

// The physical address the GPU sees for guest virtual address `a`, as the D3D computes it (the
// index base in sub_826D0340, Resolve's destination in sub_826CBEE0): the low 29 bits, plus 4 KiB
// in the 0xE0000000 view.
constexpr uint32_t GpuAddress(uint32_t a) {
  return (a & 0x1FFFFFFFu) + ((((a >> 20) & 0xFFFu) + 0x200u) & 0x1000u);
}

// Big-endian guest values.
inline uint16_t Load16(const uint8_t* base, uint32_t a) {
  return rex::memory::load_and_swap<uint16_t>(HostPtr(base, a));
}
inline uint32_t Load32(const uint8_t* base, uint32_t a) {
  return rex::memory::load_and_swap<uint32_t>(HostPtr(base, a));
}
inline uint64_t Load64(const uint8_t* base, uint32_t a) {
  return rex::memory::load_and_swap<uint64_t>(HostPtr(base, a));
}
inline float LoadFloat(const uint8_t* base, uint32_t a) {
  return std::bit_cast<float>(Load32(base, a));
}
inline void Store64(uint8_t* base, uint32_t a, uint64_t v) {
  rex::memory::store_and_swap<uint64_t>(HostPtr(base, a), v);
}

}  // namespace pgr4::native
