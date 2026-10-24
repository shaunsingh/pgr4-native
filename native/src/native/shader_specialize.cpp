#include "shader_specialize.h"

#include <cstring>
#include <vector>

#include <rex/graphics/format/ucode.h>
#include <rex/types.h>

#include <simde/x86/sse2.h>

#include "log.h"

namespace pgr4::native {

namespace {

namespace ucode = rex::graphics::ucode;
using ucode::ControlFlowInstruction;
using Op = ucode::ControlFlowOpcode;

constexpr uint32_t kLogged = 64;

void PackControlFlowInstructions(const ControlFlowInstruction* ab, uint32_t* dwords) {
  dwords[0] = ab[0].dword_0;
  dwords[1] = (ab[0].dword_1 & 0xFFFFu) | (ab[1].dword_0 << 16);
  dwords[2] = (ab[1].dword_0 >> 16) | (ab[1].dword_1 << 16);
}

bool BoolValue(const uint32_t* bools, uint32_t index) {
  return (bools[index >> 5] >> (index & 31)) & 1;
}

// Raw fields (ucode.h's bit fields are private). dword_1 holds the upper 16 bits of a control
// flow instruction: the opcode in 12-15, the addressing mode in 11, bool address and condition
// in 2-10 (cexec, cjmp, ccall), the vertex cache bits in 0-1 (exec).
constexpr uint32_t kOpcodeShift = 12;
constexpr uint32_t kExecKeepDword1 = 0x0803u;  // vertex cache bits and addressing mode
constexpr uint32_t kExecAddressMask = 0xFFFu;
constexpr uint32_t kJumpUnconditional = 1u << 13;
constexpr uint32_t kJumpConditionBits = 0x7FCu;  // bool address and condition

void SetExec(ControlFlowInstruction& cf, bool end) {
  cf.dword_1 =
      (cf.dword_1 & kExecKeepDword1) | (uint32_t(end ? Op::kExecEnd : Op::kExec) << kOpcodeShift);
}

// An exec of no instructions at the same address: the control flow bound stays.
void SetEmptyExec(ControlFlowInstruction& cf) {
  cf.dword_0 &= kExecAddressMask;
  SetExec(cf, false);
}

void SetNop(ControlFlowInstruction& cf) {
  cf.dword_0 = 0;
  cf.dword_1 = 0;
}

}  // namespace

bool SpecializeBoolControlFlow(uint32_t* ucode, uint32_t cf_pair_bound, const uint32_t bools[8]) {
  const uint32_t n = cf_pair_bound * 2;
  std::vector<ControlFlowInstruction> cf(n);
  for (uint32_t i = 0; i < cf_pair_bound; ++i)
    ucode::UnpackControlFlowInstructions(ucode + i * 3, cf.data() + i * 2);

  // Bool tests.
  for (ControlFlowInstruction& c : cf) {
    switch (c.opcode()) {
      case Op::kCondExec:
      case Op::kCondExecEnd:
      case Op::kCondExecPredClean:
      case Op::kCondExecPredCleanEnd:
        if (BoolValue(bools, c.cond_exec.bool_address()) == c.cond_exec.condition())
          SetExec(c, ucode::DoesControlFlowOpcodeEndShader(c.opcode()));
        else
          SetEmptyExec(c);  // a failed cexece does not end the shader
        break;
      case Op::kCondJmp:
      case Op::kCondCall:
        // Same layout of the fields read here (address, unconditional, predicated, bool test).
        if (c.cond_jmp.is_unconditional() || c.cond_jmp.is_predicated()) break;
        if (BoolValue(bools, c.cond_jmp.bool_address()) == c.cond_jmp.condition()) {
          c.dword_0 |= kJumpUnconditional;
          c.dword_1 &= ~kJumpConditionBits;
        } else {
          SetNop(c);
        }
        break;
      default:
        break;
    }
  }

  // Control flow reachable from the start.
  std::vector<uint8_t> reached(n, 0);
  std::vector<uint32_t> pending{0};
  auto follow = [&](uint32_t index) {
    if (index < n && !reached[index]) pending.push_back(index);
  };
  while (!pending.empty()) {
    const uint32_t i = pending.back();
    pending.pop_back();
    if (reached[i]) continue;
    reached[i] = 1;
    const ControlFlowInstruction& c = cf[i];
    switch (c.opcode()) {
      case Op::kExecEnd:
        break;
      case Op::kCondJmp:
        follow(c.cond_jmp.address());
        if (!c.cond_jmp.is_unconditional()) follow(i + 1);
        break;
      case Op::kCondCall:
        follow(c.cond_call.address());
        follow(i + 1);
        break;
      case Op::kLoopStart:
        follow(c.loop_start.address());
        follow(i + 1);
        break;
      case Op::kLoopEnd:
        follow(c.loop_end.address());
        follow(i + 1);
        break;
      default:
        // Execs (a predicated exece may not end), nops, allocs, and returns, which fall through
        // outside a call.
        follow(i + 1);
        break;
    }
  }
  for (uint32_t i = 0; i < n; ++i) {
    if (reached[i]) continue;
    if (ucode::IsControlFlowOpcodeExec(cf[i].opcode()))
      SetEmptyExec(cf[i]);
    else
      SetNop(cf[i]);
  }

  // Forward jumps over emptied control flow only.
  for (uint32_t i = 0; i < n; ++i) {
    ControlFlowInstruction& c = cf[i];
    if (!reached[i] || c.opcode() != Op::kCondJmp || !c.cond_jmp.is_unconditional()) continue;
    const uint32_t target = c.cond_jmp.address();
    if (target <= i || target > n) continue;
    bool skips_reachable = false;
    for (uint32_t j = i + 1; j < target && !skips_reachable; ++j) skips_reachable = reached[j];
    if (!skips_reachable) SetNop(c);
  }

  bool changed = false;
  for (uint32_t i = 0; i < cf_pair_bound; ++i) {
    uint32_t packed[3];
    PackControlFlowInstructions(cf.data() + i * 2, packed);
    if (std::memcmp(packed, ucode + i * 3, sizeof(packed)) != 0) {
      std::memcpy(ucode + i * 3, packed, sizeof(packed));
      changed = true;
    }
  }
  return changed;
}

bool BoolSpecializer::Key::operator==(const Key& o) const {
  return shader == o.shader && std::memcmp(bools, o.bools, sizeof(bools)) == 0;
}

size_t BoolSpecializer::KeyHash::operator()(const Key& k) const {
  uint64_t h = reinterpret_cast<uintptr_t>(k.shader) * 0x9E3779B97F4A7C15ull;
  for (uint32_t b : k.bools) h = (h ^ b) * 0x100000001B3ull;
  return size_t(h ^ (h >> 29));
}

DxbcShader* BoolSpecializer::Get(DxbcShader* shader, const uint32_t* bools) {
  if (!shader) return nullptr;
  // 128 bits at a time: the compiler makes one 256-bit test of an OR loop over the 8 words, and
  // a function that writes a 256-bit register runs VZEROUPPER before every call and return
  // (build.sh).
  const auto load = [](const uint32_t* p) {
    return simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(p));
  };
  const uint32_t* tested = shader->constant_register_map().bool_bitmap;
  const simde__m128i tested_lo = load(tested), tested_hi = load(tested + 4);
  const simde__m128i any = simde_mm_or_si128(tested_lo, tested_hi);
  if (simde_mm_movemask_epi8(simde_mm_cmpeq_epi8(any, simde_mm_setzero_si128())) == 0xFFFF)
    return shader;
  Key key{shader, {}};
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(key.bools),
                        simde_mm_and_si128(load(bools), tested_lo));
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(key.bools + 4),
                        simde_mm_and_si128(load(bools + 4), tested_hi));
  if (last_result_ && key == last_) return last_result_;
  last_ = key;
  auto [it, inserted] = specialized_.try_emplace(key, shader);
  if (inserted) it->second = Create(*shader, key.bools);
  last_result_ = it->second;
  return last_result_;
}

DxbcShader* BoolSpecializer::Create(DxbcShader& shader, const uint32_t* bools) {
  std::vector<uint32_t> ucode = shader.ucode_data();
  if (!SpecializeBoolControlFlow(ucode.data(), shader.cf_pair_index_bound(), bools)) return &shader;
  // ShaderCache::Load takes guest (big-endian) ucode, which its hash is of.
  for (uint32_t& d : ucode) d = rex::byte_swap(d);
  DxbcShader* s = cache_.Load(shader.type(), ucode.data(), uint32_t(ucode.size()));
  // The draw's pipeline, color mask and host vertex shader type follow from what a shader
  // writes. An export only on a path the bools exclude would change them, so such a shader
  // stays generic.
  const bool same_outputs = s->writes_color_targets() == shader.writes_color_targets() &&
                            s->writes_depth() == shader.writes_depth() &&
                            s->writes_interpolators() == shader.writes_interpolators() &&
                            s->writes_point_size_edge_flag_kill_vertex() ==
                                shader.writes_point_size_edge_flag_kill_vertex() &&
                            s->memexport_eM_written() == shader.memexport_eM_written();
  if (!same_outputs) {
    PGR4_LOG_FIRST(kLogged, INFO,
                   "PGR4 native: shader {:016X} stays generic for bools {:08X}{:08X}{:08X}{:08X} "
                   "{:08X}{:08X}{:08X}{:08X} (outputs differ)",
                   shader.ucode_data_hash(), bools[0], bools[1], bools[2], bools[3], bools[4],
                   bools[5], bools[6], bools[7]);
    return &shader;
  }
  PGR4_LOG_FIRST(kLogged, INFO,
                 "PGR4 native: shader {:016X} for bools {:08X}{:08X}{:08X}{:08X} "
                 "{:08X}{:08X}{:08X}{:08X} is {:016X}",
                 shader.ucode_data_hash(), bools[0], bools[1], bools[2], bools[3], bools[4],
                 bools[5], bools[6], bools[7], s->ucode_data_hash());
  return s;
}

}  // namespace pgr4::native
