// pgr4-native PGR4 native renderer: pipelines that survive runs.
//
// PipelineStore: the shaders (guest ucode) and pipeline keys DrawRenderer has created
// pipelines for, in <cache_root>\pgr4-native\pipelines.bin. Every pipeline record carries the
// session that last created or used it; a later session's use is a 16-byte touch record,
// appended and flushed once per frame (the game is usually killed, not shut down). At
// start-up the store counts its session up, drops the records no recent session used
// (kKeepSessions) and rewrites the file, so the store stays the size of what the player plays
// instead of everything they ever played, and DrawRenderer prewarms (translates and creates
// ahead of the first draw) only what the last kHotSessions sessions used, most recently used
// first. Threading: the renderer's thread (DrawRenderer's On* calls) only.
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
    uint32_t last_session;                      // the session that last created or used it
    std::vector<uint8_t> key;                   // the pipeline key with its shader pointers zeroed
  };
  static constexpr uint32_t kNoEntry = ~0u;

  ~PipelineStore();

  // Reads <dir>\pipelines.bin (started over if its header does not match the format, key_size
  // and key_version, or cut at the last whole record), counts its session up, evicts what no
  // recent session used, rewrites the file and opens it for appending. Stays closed if
  // key_size is over kMaxKeySize.
  void Open(const std::filesystem::path& dir, uint32_t key_size, uint32_t key_version);
  bool is_open() const { return file_ != nullptr; }

  // Records from earlier runs, most recently used first. The first hot_count() (the last
  // kHotSessions sessions', at most kPrewarmMax) are what a run prewarms; the rest compile on
  // demand if they come up.
  const std::vector<Pipeline>& loaded_pipelines() const { return loaded_pipelines_; }
  size_t hot_count() const { return hot_count_; }
  const Shader* loaded_shader(uint64_t hash) const {
    auto it = loaded_shaders_.find(hash);
    return it == loaded_shaders_.end() ? nullptr : &it->second;
  }

  // Appends what is not in the file yet. host_ucode: Shader::ucode_dwords() (host order). key:
  // the key_size bytes given to Open. A recorded pipeline's last_session is this session.
  void RecordShader(uint32_t type, uint64_t hash, const uint32_t* host_ucode, size_t dwords);
  void RecordPipeline(uint64_t vs_hash, uint64_t vs_mod, uint64_t ps_hash, uint64_t ps_mod,
                      const void* key);
  // Marks a record used this session: the loaded pipeline with this key (nothing if there is
  // none; a pipeline recorded this session needs none), or the loaded_pipelines() index a
  // prewarmed pipeline carries in DrawRenderer's Pipeline::store_entry. One 16-byte record per
  // pipeline per session.
  void TouchRecord(uint64_t vs_hash, uint64_t vs_mod, uint64_t ps_hash, uint64_t ps_mod,
                   const void* key);
  void TouchEntry(uint32_t index);
  // Once per frame: makes the records appended since the last call durable.
  void Flush();

  static constexpr uint32_t kMaxKeySize = 256;
  // Sessions a record survives without a use, and how far back prewarm covers.
  static constexpr uint32_t kKeepSessions = 8, kHotSessions = 3;
  static constexpr size_t kPrewarmMax = 4096;

 private:
  // The record's identity: over the hashes, modifications and key, not the session.
  static uint64_t RecordHash(uint64_t vs_hash, uint64_t vs_mod, uint64_t ps_hash, uint64_t ps_mod,
                             const void* key, uint32_t key_size);
  void AppendTouch(uint64_t record_hash);

  std::FILE* file_ = nullptr;
  uint32_t key_size_ = 0, session_ = 0;
  bool dirty_ = false;
  std::unordered_map<uint64_t, Shader> loaded_shaders_;
  std::vector<Pipeline> loaded_pipelines_;
  std::vector<uint64_t> record_hashes_;                   // per loaded pipeline
  std::unordered_map<uint64_t, uint32_t> entry_by_hash_;  // record hash -> loaded index
  size_t hot_count_ = 0;
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
