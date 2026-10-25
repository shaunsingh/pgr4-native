// shader_xlate_test: offline oracle for the native renderer's shader translation (M2, owner 3).
//
// Translates every vertex/pixel shader that the M1 frame traces reference, with the
// modifications the Xenos plugin used for them, through pgr4::native::ShaderCache, and compiles
// each pipeline pair into a D3D12 pipeline state object on the local GPU (the driver's DXBC
// compile). Inputs, all from one game run:
//   --xsh   the Xenos shader storage (userdata\cache\shaders\shareable\4D5307F9.xsh): guest
//           ucode of every shader the plugin saw;
//   --xpso  the pipeline storage (4D5307F9.rtv.d3d12.xpso): every (vs, modification, ps,
//           modification) pair plus render-target formats the plugin created a pipeline for;
//   --trace a pgr4_d3d_trace frame trace; its "# shader" lines (FNV-1a of the ucode) select the
//           shaders. Repeatable.
// Optional:
//   --dump  a Xenos --dump_shaders directory: compares each translation byte for byte with the
//           plugin's shader_<hash>_<modification>.d3d12*.bin.{vert,frag};
//   --csv   per-translation results; --debug enables the D3D12 debug layer.
// Exit code 0 only if every traced shader was found, translated and compiled.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "native/geometry_shader.h"
#include "native/shader_cache.h"

using Microsoft::WRL::ComPtr;
using pgr4::native::DxbcShader;
using pgr4::native::ShaderCache;
namespace xenos = rex::graphics::xenos;

namespace {

uint64_t Fnv(const std::vector<uint32_t>& le) {  // pgr4_d3d_trace.cpp's shader hash
  uint64_t h = 1469598103934665603ull;
  for (uint32_t d : le) {
    h ^= d;
    h *= 1099511628211ull;
  }
  return h;
}

std::vector<uint8_t> Slurp(const std::filesystem::path& p) {
  std::error_code ec;
  auto size = std::filesystem::file_size(p, ec);
  std::vector<uint8_t> bytes(ec ? 0 : size_t(size));
  std::ifstream f(p, std::ios::binary);
  f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
  if (!f) std::fprintf(stderr, "cannot read %s\n", p.string().c_str());
  return bytes;
}

struct StoredShader {
  uint64_t hash = 0;  // XXH3 of the guest ucode (Xenos ucode_data_hash)
  xenos::ShaderType type = xenos::ShaderType::kVertex;
  std::vector<uint32_t> guest;  // big-endian, as in guest memory
  uint64_t fnv = 0;
};

#pragma pack(push, 1)
struct StoredPipeline {  // d3d12::PipelineCache::PipelineStoredDescription (kVersion 0x20210425)
  uint64_t description_hash;
  uint64_t vs_hash, vs_mod, ps_hash, ps_mod;
  int32_t depth_bias;
  float depth_bias_slope_scaled;
  uint32_t bits0;  // strip cut 2, topology/tess 2, gs 2, wire 1, cull 2, ccw 1, depth clip 1,
                   // msaa 2, depth format 1, depth func 3, depth write 1, stencil 1, read mask 8
  uint32_t bits1;  // stencil
  uint32_t rt[4];  // used 1, format 4, blend..., write mask at bit 27
};
#pragma pack(pop)
static_assert(sizeof(StoredPipeline) == 72);

DXGI_FORMAT ColorFormat(uint32_t f) {  // D3D12RenderTargetCache::GetColorResourceDXGIFormat
  using F = xenos::ColorRenderTargetFormat;
  switch (F(f)) {
    case F::k_8_8_8_8:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case F::k_8_8_8_8_GAMMA:
      return DXGI_FORMAT_R16G16B16A16_UNORM;  // gamma as unorm16
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case F::k_16_16:
      return DXGI_FORMAT_R16G16_SNORM;
    case F::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_SNORM;
    case F::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_FLOAT;
    case F::k_32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case F::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_FLOAT;
  }
  return DXGI_FORMAT_R8G8B8A8_UNORM;
}

double Ms(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

struct Result {
  std::set<std::string> traces;
  bool found = false;
  uint32_t translations = 0, translate_failures = 0;
  uint32_t pso_ok = 0, pso_failed = 0;
  uint32_t dump_same = 0, dump_diff = 0, dump_missing = 0;
};

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path xsh, xpso, dump, csv;
  std::vector<std::filesystem::path> traces;
  bool debug = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() {
      return i + 1 < argc ? std::filesystem::path(argv[++i]) : std::filesystem::path();
    };
    if (a == "--xsh")
      xsh = next();
    else if (a == "--xpso")
      xpso = next();
    else if (a == "--trace")
      traces.push_back(next());
    else if (a == "--dump")
      dump = next();
    else if (a == "--csv")
      csv = next();
    else if (a == "--debug")
      debug = true;
    else {
      std::fprintf(stderr, "unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  if (xsh.empty() || xpso.empty() || traces.empty()) {
    std::fprintf(stderr,
                 "usage: shader_xlate_test --xsh F --xpso F --trace F [--trace F] [--dump DIR] "
                 "[--csv F] [--debug]\n");
    return 2;
  }

  // Shader storage: header 'XESH' + version, then {hash, dword count : 31, type : 1, ucode}.
  std::vector<StoredShader> stored;
  {
    auto bytes = Slurp(xsh);
    size_t at = 8;
    while (at + 12 <= bytes.size()) {
      StoredShader s;
      uint32_t count_type;
      std::memcpy(&s.hash, &bytes[at], 8);
      std::memcpy(&count_type, &bytes[at + 8], 4);
      at += 12;
      uint32_t count = count_type & 0x7FFFFFFF;
      s.type = xenos::ShaderType(count_type >> 31);
      if (at + size_t(count) * 4 > bytes.size()) break;
      s.guest.resize(count);
      std::memcpy(s.guest.data(), &bytes[at], size_t(count) * 4);
      at += size_t(count) * 4;
      std::vector<uint32_t> le(count);
      for (uint32_t k = 0; k < count; ++k) le[k] = _byteswap_ulong(s.guest[k]);
      s.fnv = Fnv(le);
      stored.push_back(std::move(s));
    }
  }
  std::map<std::pair<int, uint64_t>, const StoredShader*> by_fnv;
  for (auto& s : stored) by_fnv[{int(s.type), s.fnv}] = &s;

  // Traced shaders: "# shader vs|ps ADDR dwords=N hash=FNV ..." lists the shaders loaded from
  // memory; the draw lines ("D ... vs=ADDR:DWORDS:FNV ps=...") add the ones the D3D loads with
  // IM_LOAD_IMMEDIATE (address 0: its clear and resolve shaders).
  std::map<std::pair<int, uint64_t>, Result> results;  // key: type, FNV
  for (auto& path : traces) {
    std::ifstream f(path);
    std::string line, tag = path.stem().string();
    while (std::getline(f, line)) {
      char type[4];
      unsigned addr, dwords;
      unsigned long long fnv;
      if (std::sscanf(line.c_str(), "# shader %2s %x dwords=%u hash=%llx", type, &addr, &dwords,
                      &fnv) == 4) {
        results[{std::strcmp(type, "ps") == 0 ? 1 : 0, fnv}].traces.insert(tag);
        continue;
      }
      if (line.rfind("D ", 0) != 0) continue;
      for (int t = 0; t < 2; ++t) {
        auto at = line.find(t ? " ps=" : " vs=");
        if (at != std::string::npos &&
            std::sscanf(line.c_str() + at + 4, "%x:%u:%llx", &addr, &dwords, &fnv) == 3 && dwords)
          results[{t, fnv}].traces.insert(tag);
      }
    }
  }

  // Load the traced shaders.
  ShaderCache cache;
  std::map<uint64_t, std::pair<DxbcShader*, Result*>> by_hash;  // XXH3 -> shader
  for (auto& [key, r] : results) {
    auto it = by_fnv.find(key);
    if (it == by_fnv.end()) continue;
    r.found = true;
    const StoredShader& s = *it->second;
    by_hash[s.hash] = {cache.Load(s.type, s.guest.data(), uint32_t(s.guest.size())), &r};
  }

  // Pipelines that use a traced shader; their other shader is loaded from the storage too.
  std::map<uint64_t, const StoredShader*> stored_by_hash;
  for (auto& s : stored) stored_by_hash[s.hash] = &s;
  std::vector<StoredPipeline> pipelines;
  std::vector<std::unique_ptr<Result>> partners;  // results of untraced partner shaders
  {
    auto bytes = Slurp(xpso);
    for (size_t at = 12; at + sizeof(StoredPipeline) <= bytes.size();
         at += sizeof(StoredPipeline)) {
      StoredPipeline p;
      std::memcpy(&p, &bytes[at], sizeof(p));
      if (by_hash.count(p.vs_hash) || (p.ps_hash && by_hash.count(p.ps_hash)))
        pipelines.push_back(p);
    }
    for (auto& p : pipelines) {
      for (uint64_t h : {p.vs_hash, p.ps_hash}) {
        if (!h || by_hash.count(h) || !stored_by_hash.count(h)) continue;
        const StoredShader& s = *stored_by_hash[h];
        partners.push_back(std::make_unique<Result>());
        by_hash[h] = {cache.Load(s.type, s.guest.data(), uint32_t(s.guest.size())),
                      partners.back().get()};
      }
    }
  }

  // D3D12 device and the bindless root signature all translations share.
  if (debug) {
    ComPtr<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
  }
  ComPtr<ID3D12Device> device;
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
    std::fprintf(stderr, "D3D12CreateDevice failed\n");
    return 1;
  }
  auto root = pgr4::native::CreateBindlessRootSignature(device.Get());
  if (!root) return 1;

  FILE* out = csv.empty() ? nullptr : _wfopen(csv.c_str(), L"w");
  if (out)
    std::fprintf(out,
                 "stage,ucode_hash,modification,dwords,dxbc_bytes,translate_us,textures,samplers,"
                 "float_consts,traces,dump\n");

  // Translate (vs, mod) / (ps, mod) once each, compare with the plugin's dump.
  std::map<std::pair<uint64_t, uint64_t>, DxbcShader::Translation*> translated;
  auto translate = [&](uint64_t hash, uint64_t mod) -> DxbcShader::Translation* {
    auto key = std::make_pair(hash, mod);
    auto done = translated.find(key);
    if (done != translated.end()) return done->second;
    auto [shader, r] = by_hash.at(hash);
    auto t = std::chrono::steady_clock::now();
    DxbcShader::Translation* tr = cache.Translate(*shader, mod);
    double us = Ms(t) * 1000.0;
    ++r->translations;
    if (!tr) ++r->translate_failures;
    const char* ext = shader->type() == xenos::ShaderType::kVertex ? "vert" : "frag";
    const char* dump_state = "";
    if (tr && !dump.empty()) {
      char name[96];
      std::snprintf(name, sizeof(name), "shader_%016llX_%016llX.d3d12", (unsigned long long)hash,
                    (unsigned long long)mod);
      std::filesystem::path match;
      for (const char* suffix : {"", "_rtv"}) {
        auto p = dump / (std::string(name) + suffix + ".bin." + ext);
        if (std::filesystem::exists(p)) match = p;
      }
      if (match.empty()) {
        ++r->dump_missing;
        dump_state = "missing";
      } else if (Slurp(match) == tr->translated_binary()) {
        ++r->dump_same;
        dump_state = "same";
      } else {
        ++r->dump_diff;
        dump_state = "different";
      }
    }
    if (out) {
      std::string tags;
      for (auto& s : r->traces) tags += (tags.empty() ? "" : "+") + s;
      uint32_t floats = 0;
      for (uint64_t w : shader->constant_register_map().float_bitmap)
        floats += uint32_t(std::popcount(w));
      std::fprintf(out, "%s,%016llX,%016llX,%zu,%zu,%.1f,%zu,%zu,%u,%s,%s\n",
                   shader->type() == xenos::ShaderType::kVertex ? "vs" : "ps",
                   (unsigned long long)hash, (unsigned long long)mod, shader->ucode_dword_count(),
                   tr ? tr->translated_binary().size() : 0, us,
                   shader->GetTextureBindingsAfterTranslation().size(),
                   shader->GetSamplerBindingsAfterTranslation().size(), floats, tags.c_str(),
                   dump_state);
    }
    translated.emplace(key, tr);
    return tr;
  };

  // Compile a VS (+ GS) (+ PS) into a PSO; p supplies topology, geometry shader type, MSAA and
  // render-target formats.
  double pso_ms_total = 0, pso_ms_max = 0;
  uint32_t pso_ok = 0, pso_failed = 0, pso_skipped = 0, pso_with_gs = 0;
  std::map<uint32_t, std::vector<uint32_t>> geometry_shaders;
  auto compile = [&](const StoredPipeline& p, DxbcShader::Translation* vs,
                     DxbcShader::Translation* ps) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = root.Get();
    d.VS = {vs->translated_binary().data(), vs->translated_binary().size()};
    if (ps) d.PS = {ps->translated_binary().data(), ps->translated_binary().size()};
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    uint32_t topology = (p.bits0 >> 2) & 3, gs = (p.bits0 >> 4) & 3;
    d.PrimitiveTopologyType = topology == 0   ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                              : topology == 1 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                              : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // A stored key holds the geometry shader type; any primitive needing that type rebuilds it.
    static constexpr xenos::PrimitiveType kPrimitiveForType[] = {
        xenos::PrimitiveType::kTriangleList, xenos::PrimitiveType::kPointList,
        xenos::PrimitiveType::kRectangleList, xenos::PrimitiveType::kQuadList};
    pgr4::native::GeometryShaderKey gs_key;
    gs_key.key = pgr4::native::GeometryShaderKeyFor(kPrimitiveForType[gs], vs->modification(),
                                                    ps ? ps->modification() : 0);
    if (gs_key.key) {
      auto it = geometry_shaders.find(gs_key.key);
      if (it == geometry_shaders.end())
        it = geometry_shaders.emplace(gs_key.key, pgr4::native::CreateGeometryShader(gs_key)).first;
      d.GS = {it->second.data(), it->second.size() * sizeof(uint32_t)};
      ++pso_with_gs;
    }
    uint32_t msaa = (p.bits0 >> 11) & 3;
    d.SampleDesc.Count = 1u << msaa;
    d.DSVFormat =
        ((p.bits0 >> 13) & 1) ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(p.rt[i] & 1)) continue;
      d.NumRenderTargets = i + 1;
      d.RTVFormats[i] = ColorFormat((p.rt[i] >> 1) & 15);
      d.BlendState.RenderTarget[i].RenderTargetWriteMask = UINT8((p.rt[i] >> 27) & 15);
    }
    auto t = std::chrono::steady_clock::now();
    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    double ms = Ms(t);
    pso_ms_total += ms;
    pso_ms_max = std::max(pso_ms_max, ms);
    bool ok = SUCCEEDED(hr);
    (ok ? pso_ok : pso_failed)++;
    if (!ok)
      std::printf("PSO failed %08X: vs %016llX/%016llX ps %016llX/%016llX gs=%u topo=%u msaa=%u\n",
                  unsigned(hr), (unsigned long long)vs->shader().ucode_data_hash(),
                  (unsigned long long)vs->modification(),
                  ps ? (unsigned long long)ps->shader().ucode_data_hash() : 0ull,
                  ps ? (unsigned long long)ps->modification() : 0ull, gs, topology, msaa);
    return ok;
  };

  // Every pipeline the plugin created with a traced shader, with its modifications.
  for (auto& p : pipelines) {
    if (!by_hash.count(p.vs_hash) || (p.ps_hash && !by_hash.count(p.ps_hash))) {
      ++pso_skipped;
      continue;
    }
    DxbcShader::Translation* vs = translate(p.vs_hash, p.vs_mod);
    DxbcShader::Translation* ps = p.ps_hash ? translate(p.ps_hash, p.ps_mod) : nullptr;
    Result* rv = by_hash.at(p.vs_hash).second;
    Result* rp = p.ps_hash ? by_hash.at(p.ps_hash).second : nullptr;
    bool ok = vs && (!p.ps_hash || ps) && compile(p, vs, ps);
    if (!vs || (p.ps_hash && !ps)) ++pso_failed;
    for (Result* r : {rv, rp})
      if (r) (ok ? r->pso_ok : r->pso_failed)++;
  }

  // Traced shaders the plugin never built a pipeline with (it drops a pixel shader that cannot
  // affect the frame, e.g. no color write in a depth-only pass): default modification, no
  // interpolators; a pixel shader is paired with a traced vertex shader translated the same way.
  StoredPipeline plain = {};
  plain.bits0 = 2u << 2;           // triangles, no GS, 1x MSAA, D24S8
  plain.rt[0] = 1u | (15u << 27);  // RT0 k_8_8_8_8, all channels
  DxbcShader::Translation* default_vs = nullptr;
  for (auto& [hash, sr] : by_hash) {
    if (sr.first->type() != xenos::ShaderType::kVertex || sr.second->traces.empty()) continue;
    default_vs = translate(hash, cache.translator().GetDefaultVertexShaderModification(
                                     sr.first->GetDynamicAddressableRegisterCount(0)));
    if (default_vs) break;
  }
  uint32_t default_mod_shaders = 0;
  for (auto& [hash, sr] : by_hash) {
    auto [shader, r] = sr;
    if (r->traces.empty() || r->translations) continue;
    ++default_mod_shaders;
    bool vertex = shader->type() == xenos::ShaderType::kVertex;
    uint32_t regs = shader->GetDynamicAddressableRegisterCount(0);
    DxbcShader::Translation* tr =
        translate(hash, vertex ? cache.translator().GetDefaultVertexShaderModification(regs)
                               : cache.translator().GetDefaultPixelShaderModification(regs));
    bool ok =
        tr && (vertex ? compile(plain, tr, nullptr) : default_vs && compile(plain, default_vs, tr));
    (ok ? r->pso_ok : r->pso_failed)++;
  }
  if (out) std::fclose(out);

  // Per-draw selection: replay each trace's register state (R = state at frame start, W = writes
  // in stream order), compute the modifications of every draw with SelectModifications and check
  // that the plugin built a pipeline with exactly that (vs, modification, ps, modification).
  std::set<std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>> plugin_pipelines;
  for (auto& p : pipelines) plugin_pipelines.emplace(p.vs_hash, p.vs_mod, p.ps_hash, p.ps_mod);
  for (auto& path : traces) {
    std::vector<uint32_t> regs(0x10000);
    std::ifstream f(path);
    std::string line;
    uint32_t draws = 0, matched = 0, unmatched = 0, unknown = 0, dropped = 0, shown = 0;
    while (std::getline(f, line)) {
      if (line.size() > 2 && (line[0] == 'R' || line[0] == 'W') && line[1] == ' ') {
        const char* s = line.c_str() + 2;
        char* end;
        uint32_t reg = uint32_t(std::strtoul(s, &end, 16));
        while (reg < regs.size()) {
          s = end;
          while (*s == ' ') ++s;
          if (!std::isxdigit(uint8_t(*s))) break;
          uint32_t v = uint32_t(std::strtoul(s, &end, 16));
          if (end == s || (*end && *end != ' ')) break;  // the R line's trailing register name
          regs[reg++] = v;
          if (line[0] == 'R') break;
        }
        continue;
      }
      if (line.rfind("D ", 0) != 0 || line.find(" draw ") == std::string::npos) continue;
      ++draws;
      unsigned prim = 0;
      unsigned long long vs_fnv = 0, ps_fnv = 0;
      unsigned a, n;
      if (auto at = line.find(" prim="); at != std::string::npos)
        std::sscanf(line.c_str() + at, " prim=%u", &prim);
      if (auto at = line.find(" vs="); at != std::string::npos)
        std::sscanf(line.c_str() + at, " vs=%x:%u:%llx", &a, &n, &vs_fnv);
      if (auto at = line.find(" ps="); at != std::string::npos)
        std::sscanf(line.c_str() + at, " ps=%x:%u:%llx", &a, &n, &ps_fnv);
      auto vs_it = by_fnv.find({0, vs_fnv});
      auto ps_it = by_fnv.find({1, ps_fnv});
      if (vs_it == by_fnv.end() || ps_it == by_fnv.end()) {
        ++unknown;
        continue;
      }
      auto load = [&](const StoredShader& s) {
        return cache.Load(s.type, s.guest.data(), uint32_t(s.guest.size()));
      };
      DxbcShader* vs = load(*vs_it->second);
      DxbcShader* ps = load(*ps_it->second);
      using namespace rex::graphics;
      pgr4::native::ShaderDrawRegs r;
      r.sq_program_cntl = regs[XE_GPU_REG_SQ_PROGRAM_CNTL];
      r.sq_context_misc = regs[XE_GPU_REG_SQ_CONTEXT_MISC];
      r.sq_interpolator_cntl = regs[XE_GPU_REG_SQ_INTERPOLATOR_CNTL];
      r.pa_cl_clip_cntl = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
      r.vgt_draw_initiator = prim & 0x3F;
      r.vgt_output_path_cntl = regs[XE_GPU_REG_VGT_OUTPUT_PATH_CNTL];
      r.pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
      r.rb_modecontrol = regs[XE_GPU_REG_RB_MODECONTROL];
      r.rb_surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
      r.rb_colorcontrol = regs[XE_GPU_REG_RB_COLORCONTROL];
      r.rb_color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
      r.rb_color_info[0] = regs[XE_GPU_REG_RB_COLOR_INFO];
      r.rb_color_info[1] = regs[XE_GPU_REG_RB_COLOR1_INFO];
      r.rb_color_info[2] = regs[XE_GPU_REG_RB_COLOR2_INFO];
      r.rb_color_info[3] = regs[XE_GPU_REG_RB_COLOR3_INFO];
      auto m = pgr4::native::SelectModifications(cache.translator(), *vs, ps, r);
      if (!m.draw) {
        ++dropped;
        continue;
      }
      auto key = std::make_tuple(vs->ucode_data_hash(), m.vertex,
                                 m.pixel_shader_used ? ps->ucode_data_hash() : 0ull,
                                 m.pixel_shader_used ? m.pixel : 0ull);
      if (plugin_pipelines.count(key)) {
        ++matched;
        continue;
      }
      ++unmatched;
      if (shown++ < 8) {
        std::printf(
            "draw %s: vs %016llX/%016llX ps %016llX/%016llX not among the plugin's pipelines"
            " (modecontrol %08X surface %08X ctx_misc %08X program %08X prim %u); plugin has:",
            line.substr(2, line.find(' ', 2) - 2).c_str(), (unsigned long long)std::get<0>(key),
            (unsigned long long)std::get<1>(key), (unsigned long long)std::get<2>(key),
            (unsigned long long)std::get<3>(key), r.rb_modecontrol, r.rb_surface_info,
            r.sq_context_misc, r.sq_program_cntl, prim);
        for (auto& [pv, pvm, pp, ppm] : plugin_pipelines)
          if (pv == vs->ucode_data_hash())
            std::printf(" %016llX/%016llX/%016llX", (unsigned long long)pvm, (unsigned long long)pp,
                        (unsigned long long)ppm);
        std::printf("\n");
      }
    }
    std::printf(
        "replay %s: %u draws, %u dropped as the plugin does, %u with the plugin's modifications, "
        "%u without, %u unknown shader\n",
        path.stem().string().c_str(), draws, dropped, matched, unmatched, unknown);
  }

  // Report.
  uint32_t traced = 0, missing = 0, not_translated = 0, no_pso = 0, failed = 0;
  std::map<std::string, std::pair<uint32_t, uint32_t>> per_trace;  // ok, total
  uint32_t dump_same = 0, dump_diff = 0, dump_missing = 0;
  for (auto& [key, r] : results) {
    ++traced;
    bool ok = r.found && r.translations && !r.translate_failures && r.pso_ok && !r.pso_failed;
    if (!r.found)
      ++missing;
    else if (!r.translations)
      ++not_translated;
    else if (r.translate_failures || r.pso_failed)
      ++failed;
    else if (!r.pso_ok)
      ++no_pso;
    for (auto& t : r.traces) {
      per_trace[t].second++;
      if (ok) per_trace[t].first++;
    }
    if (!ok)
      std::printf("NOT OK %s %016llX: found=%d translations=%u failures=%u pso ok=%u failed=%u\n",
                  key.first ? "ps" : "vs", (unsigned long long)key.second, r.found, r.translations,
                  r.translate_failures, r.pso_ok, r.pso_failed);
    dump_same += r.dump_same;
    dump_diff += r.dump_diff;
    dump_missing += r.dump_missing;
  }
  const auto& st = cache.stats();
  std::printf("storage: %zu shaders in xsh, %zu pipelines use traced shaders\n", stored.size(),
              pipelines.size());
  for (auto& [t, n] : per_trace)
    std::printf("trace %s: %u/%u shaders ok\n", t.c_str(), n.first, n.second);
  std::printf(
      "traced shaders: %u; not in storage %u, never translated %u, failed %u, translated but no "
      "pipeline %u;"
      " %u had no plugin pipeline (default modification)\n",
      traced, missing, not_translated, failed, no_pso, default_mod_shaders);
  std::printf("untraced partner shaders loaded from storage: %zu\n", partners.size());
  std::printf(
      "translations: %llu (%llu failed), analyze %.1f ms total, translate %.1f ms total, %.1f us "
      "mean, %.1f us max\n",
      (unsigned long long)st.translations, (unsigned long long)st.failures, st.analyze_us / 1000.0,
      st.translate_us / 1000.0, st.translations ? st.translate_us / double(st.translations) : 0.0,
      st.max_translate_us);
  std::printf(
      "pipelines: %u compiled, %u failed, %u skipped (a shader not in storage), %u with a geometry "
      "shader (%zu keys);"
      " create %.1f ms total, %.2f ms mean, %.1f ms max\n",
      pso_ok, pso_failed, pso_skipped, pso_with_gs, geometry_shaders.size(), pso_ms_total,
      pso_ok + pso_failed ? pso_ms_total / (pso_ok + pso_failed) : 0.0, pso_ms_max);
  if (!dump.empty())
    std::printf("xenos dump compare: %u identical, %u different, %u not in dump\n", dump_same,
                dump_diff, dump_missing);
  return (missing || not_translated || failed || no_pso || pso_failed) ? 1 : 0;
}
