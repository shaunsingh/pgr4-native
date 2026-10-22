// Bool-constant specialization of Xenos shaders.
//
// Most of the game's branching shaders branch on bool constants only (cexec b<n>, cjmp b<n>),
// which are uniform per draw. The translator makes each such test a dynamic `if` and each jump
// target a case of the main loop's switch, so the host compiler sees one block per exec and can
// neither schedule nor allocate registers across them. Instead, the ucode's control flow is
// rewritten for the draw's bool constants and loaded as a shader of its own (its own ucode hash,
// so its translations and pipelines persist like any other shader's):
// - a bool-conditional exec whose condition holds becomes a plain exec (exece for cexece); one
//   whose condition fails becomes an exec of no instructions at the same address, which keeps the
//   control flow bound (the lowest exec address) and so the shader's layout unchanged;
// - a bool-conditional jump or call becomes unconditional, or a nop;
// - control flow that can't be reached from the start is emptied the same way, and a forward jump
//   over emptied control flow becomes a nop, which drops its label (a switch case).
// Predicated control flow, loops, calls and returns are kept.
#pragma once

#include <cstdint>
#include <unordered_map>

#include "shader_cache.h"

namespace pgr4::native {

// Rewrites the control flow of `ucode` (host byte order; cf_pair_bound pairs of control flow
// instructions, Shader::cf_pair_index_bound) for the bool constants `bools` (the 256 bits of the
// SHADER_CONSTANT_BOOL registers). Returns whether anything changed.
bool SpecializeBoolControlFlow(uint32_t* ucode, uint32_t cf_pair_bound, const uint32_t bools[8]);

// Specialized shaders by (shader, the bool constants its control flow tests). One per shader
// stage: the last lookup is memoized, and consecutive draws mostly repeat a stage's shader.
// Not thread-safe; ShaderCache::Load is.
class BoolSpecializer {
 public:
  explicit BoolSpecializer(ShaderCache& cache) : cache_(cache) {}

  // `shader` specialized for the draw's bool constants (`bools`: the 8 dwords from
  // SHADER_CONSTANT_BOOL_000_031), or `shader` itself if its control flow tests none.
  DxbcShader* Get(DxbcShader* shader, const uint32_t* bools);

  // Specialized shaders created (distinct shader and bool combinations), and combinations whose
  // specialization would change the shader's outputs (the generic shader is used).
  uint64_t created() const { return created_; }
  uint64_t kept_generic() const { return kept_generic_; }

 private:
  struct Key {
    const DxbcShader* shader;
    uint32_t bools[8];  // masked to the bools the shader tests
    bool operator==(const Key& o) const;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const;
  };

  static constexpr uint64_t kLogged = 64;

  DxbcShader* Create(DxbcShader& shader, const uint32_t* bools);

  ShaderCache& cache_;
  std::unordered_map<Key, DxbcShader*, KeyHash> specialized_;
  Key last_{};
  DxbcShader* last_result_ = nullptr;
  uint64_t created_ = 0;
  uint64_t kept_generic_ = 0;
};

}  // namespace pgr4::native
