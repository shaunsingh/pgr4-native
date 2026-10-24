// pgr4-native DrawCounter: counts calls to PGR4's guest D3D packet builders.
//
// Built only into the instrumented MTR-PGR4 copy (build.ps1 adds it to the target). Each hook
// overrides the weak sub_X symbol from the generated code, bumps a counter and forwards to the
// original __imp__sub_X. The SDK side (rexglue-drawcount.patch, --gpu_stream_stats) finds
// rex_drawcount_api_counters in this executable and prints per-second deltas next to the PM4
// counts. Names starting with "draw" are summed into api_total.
//
// Addresses are for the base-disc default.xex (sha256 f9a411d8...f423e) as recompiled by MTR-PGR4
// 2db3edb. They were found by scanning the generated C++ for `lis rX,0xC00n` followed by
// `ori` with a PM4 type-3 low half (opcode << 8 | predicate bit); see the development notes,
// "Draw-path measurement", for the table and evidence.

#include <rex/hook.h>

#include <atomic>
#include <cstdint>

namespace {

enum Hook : uint32_t {
  kDrawIndxAuto_826CFA60,     // 0xC0012201 x2: DRAW_INDX, 2 dwords (auto index), predicated
  kDrawIndxAuto_826CFF58,     // 0xC0012201: DRAW_INDX, 2 dwords, predicated
  kDrawIndxDma_826D0340,      // 0xC0032201: DRAW_INDX, 4 dwords (index buffer), predicated
  kDrawIndx2_826D0798,        // 0xC0003601 | n<<16: DRAW_INDX_2, predicated
  kDrawIndx2_826DC888,        // 0xC0003601, initiator count<<16 | 0x81, split at 65534: predicated
  kDrawIndx2Np_826C97C0,      // 0xC0003600 / 0x00010081 in a 24x loop: not predicated (aux)
  kDrawIndx2Np_826CBEE0,      // 0xC0003600 | n<<16, initiator 0x00030088 (3-vertex rect): aux,
                              // likely the D3D clear/resolve rectangle
  kIndirectBuffer_826CDB80,   // 0xC0013F00: INDIRECT_BUFFER
  kBinAll_826C6008,           // SET_BIN_MASK_LO/HI + SET_BIN_SELECT_LO/HI
  kBinSelectLo_826E02B8,      // SET_BIN_SELECT_LO
  kBinSelHiMask_826D6610,     // SET_BIN_SELECT_HI + SET_BIN_MASK_LO/HI
  kBinMask_826D62C8,          // SET_BIN_MASK_LO/HI
  kCount
};

const char* const kNames[kCount] = {
    "draw_826CFA60",  "draw_826CFF58",  "draw_826D0340", "draw_826D0798",
    "draw_826DC888",  "aux_826C97C0",   "aux_826CBEE0", "ib_826CDB80",
    "bin_826C6008",   "binsel_826E02B8", "binsel_826D6610", "binmask_826D62C8",
};

std::atomic<uint64_t> g_counts[kCount];
uint64_t g_snapshot[kCount];

}  // namespace

extern "C" __declspec(dllexport) uint32_t rex_drawcount_api_counters(
    const uint64_t** values, const char* const** names) {
  for (uint32_t i = 0; i < kCount; ++i) g_snapshot[i] = g_counts[i].load(std::memory_order_relaxed);
  *values = g_snapshot;
  *names = kNames;
  return kCount;
}

#define DRAWCOUNT_HOOK(addr, index)                               \
  REX_EXTERN(__imp__sub_##addr);                                  \
  REX_HOOK_RAW(sub_##addr) {                                      \
    g_counts[index].fetch_add(1, std::memory_order_relaxed);      \
    __imp__sub_##addr(ctx, base);                                 \
  }

DRAWCOUNT_HOOK(826CFA60, kDrawIndxAuto_826CFA60)
DRAWCOUNT_HOOK(826CFF58, kDrawIndxAuto_826CFF58)
DRAWCOUNT_HOOK(826D0340, kDrawIndxDma_826D0340)
DRAWCOUNT_HOOK(826D0798, kDrawIndx2_826D0798)
DRAWCOUNT_HOOK(826DC888, kDrawIndx2_826DC888)
DRAWCOUNT_HOOK(826C97C0, kDrawIndx2Np_826C97C0)
DRAWCOUNT_HOOK(826CBEE0, kDrawIndx2Np_826CBEE0)
DRAWCOUNT_HOOK(826CDB80, kIndirectBuffer_826CDB80)
DRAWCOUNT_HOOK(826C6008, kBinAll_826C6008)
DRAWCOUNT_HOOK(826E02B8, kBinSelectLo_826E02B8)
DRAWCOUNT_HOOK(826D6610, kBinSelHiMask_826D6610)
DRAWCOUNT_HOOK(826D62C8, kBinMask_826D62C8)
