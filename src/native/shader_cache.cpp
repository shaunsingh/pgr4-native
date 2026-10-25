#include "shader_cache.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(pgr4_native_shader_dump_dir, "", "PGR4 native",
                      "Write every translation (DXBC) to <dir>/<ucode hash>_<modification>.<vs|ps>"
                      ".dxbc as it is made, and every shader's ucode disassembly to "
                      "<dir>/<ucode hash>.<vs|ps>.txt (empty = off)");

namespace pgr4::native {

namespace {
double Us(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - since)
      .count();
}

const char* Stage(const DxbcShader& shader) {
  return shader.type() == rex::graphics::xenos::ShaderType::kVertex ? "vs" : "ps";
}

void DumpFile(const char* name, const void* data, size_t size) {
  const std::filesystem::path dir(REXCVAR_GET(pgr4_native_shader_dump_dir));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (std::FILE* out = _wfopen((dir / name).c_str(), L"wb")) {
    std::fwrite(data, 1, size, out);
    std::fclose(out);
  }
}
}  // namespace

// The Xenos plugin's settings on the RTX 3090 (bindless on, host render targets, gamma as
// unorm16, 2x MSAA supported, no resolution scale), so a translation here is byte-identical to
// the plugin's for the same modification. The vendor only changes code for Intel (no switch
// statements).
ShaderCache::ShaderCache()
    : translator_(rex::ui::GraphicsProvider::GpuVendorID(0x10DE), /*bindless=*/true,
                  /*edram_rov_used=*/false, /*gamma_render_target_as_unorm8=*/false,
                  /*msaa_2x_supported=*/true) {}

ShaderCache::~ShaderCache() = default;

DxbcShader* ShaderCache::Load(rex::graphics::xenos::ShaderType type, const uint32_t* guest_ucode,
                              uint32_t dword_count) {
  uint64_t hash = XXH3_64bits(guest_ucode, size_t(dword_count) * sizeof(uint32_t));
  std::lock_guard<std::mutex> lock(load_mutex_);
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) return it->second.get();
  auto t = std::chrono::steady_clock::now();
  auto shader = std::make_unique<DxbcShader>(type, hash, guest_ucode, dword_count);
  shader->AnalyzeUcode(disasm_buffer_);
  stats_.analyze_us += Us(t);
  if (!REXCVAR_GET(pgr4_native_shader_dump_dir).empty()) {
    char name[40];
    std::snprintf(name, sizeof(name), "%016llX.%s.txt", static_cast<unsigned long long>(hash),
                  Stage(*shader));
    const std::string& text = shader->ucode_disassembly();
    DumpFile(name, text.data(), text.size());
  }
  ++stats_.shaders;
  return shaders_.emplace(hash, std::move(shader)).first->second.get();
}

const DxbcShader* ShaderCache::Find(uint64_t hash) {
  std::lock_guard<std::mutex> lock(load_mutex_);
  auto it = shaders_.find(hash);
  return it != shaders_.end() ? it->second.get() : nullptr;
}

DxbcShader::Translation* ShaderCache::Translate(DxbcShader& shader, uint64_t modification) {
  bool is_new = false;
  DxbcShader::Translation* translation = shader.GetOrCreateTranslation(modification, &is_new);
  if (is_new) {
    auto t = std::chrono::steady_clock::now();
    translator_.TranslateAnalyzedShader(*translation);
    double us = Us(t);
    stats_.translate_us += us;
    stats_.max_translate_us = std::max(stats_.max_translate_us, us);
    ++stats_.translations;
    if (!REXCVAR_GET(pgr4_native_shader_dump_dir).empty() && translation->is_valid()) {
      char name[64];
      std::snprintf(name, sizeof(name), "%016llX_%016llX.%s.dxbc",
                    static_cast<unsigned long long>(shader.ucode_data_hash()),
                    static_cast<unsigned long long>(modification), Stage(shader));
      const std::vector<uint8_t>& binary = translation->translated_binary();
      DumpFile(name, binary.data(), binary.size());
    }
    if (!translation->is_valid()) {
      ++stats_.failures;
      REXLOG_ERROR("pgr4 native: {} shader {:016X} modification {:016X} failed to translate",
                   shader.type() == rex::graphics::xenos::ShaderType::kVertex ? "vertex" : "pixel",
                   shader.ucode_data_hash(), modification);
    }
  }
  return translation->is_valid() ? translation : nullptr;
}

}  // namespace pgr4::native
