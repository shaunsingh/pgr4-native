// pgr4-native PGR4 native renderer: pipelines that survive runs.
//
// PipelineStore: the shaders (guest ucode) and pipeline keys DrawRenderer has created
// pipelines for, appended to <cache_root>\pgr4-native\pipelines.bin as they are first seen.
// At start-up DrawRenderer reads them back and translates and compiles them ahead of their
// first draw (prewarm), so a second run of the same route has no draws waiting for pipelines.
// Records are appended and flushed once per frame: the game is usually killed, not shut down.
// Threading: the renderer's thread (DrawRenderer's On* calls) only.
//
// PipelineLibrary: an ID3D12PipelineLibrary in <cache_root>\pgr4-native\pipelines.d3d12lib, so
// the driver's compiled pipelines are loaded instead of compiled again. Saved by a pipeline
// worker when they have been idle for a while. Threading: Open once, before any other call;
// Create from any thread, Save from one at a time.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pgr4::native {

class PipelineStore {
 public:
  struct Shader {
    uint32_t type;                // xenos::ShaderType
    std::vector<uint32_t> ucode;  // as in guest memory (big-endian)
  };
  struct Pipeline {
    uint64_t vs_hash, vs_mod, ps_hash, ps_mod;  // ucode_data_hash and modification; ps_hash 0: none
    std::vector<uint8_t> key;                   // the pipeline key with its shader pointers zeroed
  };

  ~PipelineStore();

  // Reads <dir>\pipelines.bin (started over if its header does not match key_size and
  // key_version, or cut at the last whole record) and opens it for appending. Stays closed if
  // key_size is over kMaxKeySize.
  void Open(const std::filesystem::path& dir, uint32_t key_size, uint32_t key_version);
  bool is_open() const { return file_ != nullptr; }

  // Records from earlier runs, in the order they were first seen.
  const std::vector<Pipeline>& loaded_pipelines() const { return loaded_pipelines_; }
  const Shader* loaded_shader(uint64_t hash) const {
    auto it = loaded_shaders_.find(hash);
    return it == loaded_shaders_.end() ? nullptr : &it->second;
  }

  // Appends what is not in the file yet. host_ucode: Shader::ucode_dwords() (host order). key:
  // the key_size bytes given to Open.
  void RecordShader(uint32_t type, uint64_t hash, const uint32_t* host_ucode, size_t dwords);
  void RecordPipeline(uint64_t vs_hash, uint64_t vs_mod, uint64_t ps_hash, uint64_t ps_mod,
                      const void* key);
  // Once per frame: makes the records appended since the last call durable.
  void Flush();

  static constexpr uint32_t kMaxKeySize = 256;

 private:
  std::FILE* file_ = nullptr;
  uint32_t key_size_ = 0;
  bool dirty_ = false;
  std::unordered_map<uint64_t, Shader> loaded_shaders_;
  std::vector<Pipeline> loaded_pipelines_;
  std::unordered_set<uint64_t> shaders_written_, pipelines_written_;
};

class PipelineLibrary {
 public:
  // root_signature_hash: CreateBindlessRootSignature's blob_hash. A pipeline stored with another
  // root signature does not load with this one, so it is part of every name.
  void Open(ID3D12Device* device, const std::filesystem::path& path, uint64_t root_signature_hash);
  // Loads the pipeline from the library, or creates it and stores it there. *loaded: whether
  // it came from the library.
  HRESULT Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, ID3D12PipelineState** out,
                 bool* loaded);
  bool dirty() const { return dirty_.load(std::memory_order_relaxed); }
  // Writes the library if a pipeline was stored since the last save.
  void Save();

 private:
  ID3D12Device* device_ = nullptr;
  std::vector<uint8_t> blob_;  // must outlive library_, so it is declared first
  Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> library_;
  std::filesystem::path path_;
  uint64_t root_signature_hash_ = 0;
  std::atomic<bool> dirty_{false};
  std::atomic<uint32_t> loaded_{0}, stored_{0};
  // Names being loaded or stored: the library does not synchronize two loads of one pipeline.
  std::mutex busy_mutex_;
  std::unordered_set<uint64_t> busy_;
};

}  // namespace pgr4::native
