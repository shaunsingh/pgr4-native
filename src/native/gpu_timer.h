// pgr4-native PGR4 native renderer: GPU time per pass of the native frame
// (--pgr4_native_gpu_timing).
//
// The renderer names the pass each command it records belongs to: draws on one set of render
// targets, a clear, a resolve, a pass of the upscaler, the present blit (Key*). Whenever the pass
// changes, a timestamp goes into the frame's command list, so the span between two timestamps is
// the GPU time of the pass recorded in it. EndFrame resolves the frame's timestamps into the
// readback slot of its command allocator; BeginFrame reads them once that allocator comes round
// again (D3D12Device::OpenList, after the slot's fence), so timing never stalls the GPU or the
// recording thread.
//
// Timestamps are written when the preceding work has finished at the bottom of the pipe:
// a span runs from the end of the previous pass to the end of its own. Neighbouring passes
// overlap on the GPU, so one span is approximate; a frame's spans add up to its GPU time.
// Spans are grouped by pass key plus the key's occurrence in the frame (the second run of
// draws on the scene targets after a resolve is its own group), and Log prints the costliest
// groups in frame order. A frame far slower than the recent ones is logged on its own as a
// spike: its costliest spans next to their groups' mean, so a heavier scene can be told from a
// stall inside one pass. Frames are numbered as in the frame CSV (the first one is frame 0).
//
// Threading: the thread that records the frames (the render thread by default).
#pragma once

#include "d3d12_device.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pgr4::native {

struct ClearCall;
struct ResolveParams;

class GpuTimer {
 public:
  // Spans per frame; passes past it are charged to the last span.
  static constexpr uint32_t kMaxSpans = 2047;

  // `slots`: command allocators the frames rotate through (D3D12Device::kFramesInFlight).
  bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t slots);

  // The frame's command list was opened on allocator `slot`: collects the frame that slot
  // held last (complete, its fence was waited for) and writes the new frame's first timestamp.
  void BeginFrame(ID3D12GraphicsCommandList* list, uint32_t slot);
  // The commands recorded next belong to pass `key`.
  void Pass(ID3D12GraphicsCommandList* list, uint64_t key) {
    if (key != current_) Switch(list, key);
    ++frames_[slot_].spans.back().commands;
  }
  // Before the list is closed: the last timestamp, and the copy into the readback slot.
  void EndFrame(ID3D12GraphicsCommandList* list);

  // Pass keys. Draws: render target registers and which targets the draw writes.
  static uint64_t KeyDraw(const uint32_t* regs);
  static uint64_t KeyClear(const ClearCall& c);
  // Resolver::Resolve splits its pass into these phases (kSource is the key the renderer
  // passes before calling it): the source's transition and EDRAM fills, the copy dispatch, the
  // destination's transition back to shader reads, and the clear of the source rectangle.
  enum ResolvePhase : uint32_t { kResolveSource, kResolveCopy, kResolveRelease, kResolveClear };
  static uint64_t KeyResolve(const ResolveParams& p, ResolvePhase phase = kResolveSource);
  // Upscaler::Upscale's passes, inside the resolve that upscales: the clamp of FSR's input, the
  // FidelityFX dispatch (or the upscaler OptiScaler puts in its place), and frame generation's
  // inputs. Keyed with the render size, which changes with dynamic resolution.
  enum UpscalePhase : uint32_t { kUpscaleInput, kUpscaleDispatch, kUpscaleFrameGeneration };
  static uint64_t KeyUpscale(UpscalePhase phase, uint32_t render_width, uint32_t render_height);
  static uint64_t KeyPresent();
  // The end of a list submitted mid-frame (D3D12Device::Submit): its span runs to the first
  // timestamp in the next list, the GPU's wait for that list.
  static uint64_t KeySubmit();

  // --pgr4_native_gpu_timing_shaders: draws are timed per (vertex shader, pixel shader) pair on
  // a surface pitch and MSAA mode instead of per render target set (KeyDrawShaders), and all
  // of a pair's spans in a frame form one group.
  void set_per_shader(bool on) { per_shader_ = on; }
  bool per_shader() const { return per_shader_; }
  uint64_t KeyDrawShaders(const uint32_t* regs, uint64_t vs_hash, uint64_t ps_hash);
  // Ucode hashes of the shaders in the costliest groups of the last Log not returned before.
  std::vector<uint64_t> TakeNewTopShaders() { return std::exchange(new_top_shaders_, {}); }

  // Logs the frames collected since the last call: GPU time per frame, the GPU's idle time
  // between frames, and the costliest pass groups. Resets the window.
  void Log();

  // GPU time of the frame collected last, in ms: after D3D12Device::EndFrame, the frame before
  // the one just submitted (its slot's fence was waited for when the next list opened).
  double last_frame_ms() const { return double(last_frame_ticks_) * 1000.0 / double(frequency_); }

 private:
  struct Span {
    uint64_t key;
    uint32_t commands;
  };
  struct Frame {
    std::vector<Span> spans;
    uint64_t number = 0;
    uint32_t queries = 0;
    bool pending = false;  // resolved into the readback slot, not collected yet
  };
  struct SpikeSpan {
    uint64_t ticks;
    uint32_t index;
  };
  struct Group {
    uint64_t ticks = 0, spans = 0, commands = 0, position = 0;
  };

  void Switch(ID3D12GraphicsCommandList* list, uint64_t key);
  void Stamp(ID3D12GraphicsCommandList* list);
  void Collect(uint32_t slot);
  bool IsSpike(uint64_t ticks);
  void LogSpike(const Frame& f, uint64_t ticks);
  std::string Describe(uint64_t key) const;

  ComPtr<ID3D12QueryHeap> heap_;
  ComPtr<ID3D12Resource> readback_;
  uint64_t frequency_ = 1;
  std::vector<Frame> frames_;
  uint32_t slot_ = 0;
  uint64_t current_ = 0;
  std::unordered_map<uint64_t, uint32_t> occurrences_;  // this frame's spans per key

  bool per_shader_ = false;
  std::vector<std::pair<uint64_t, uint64_t>> pairs_;  // (VS, PS) hashes by KeyDrawShaders id
  std::unordered_map<uint64_t, uint32_t> pair_ids_;
  std::unordered_set<uint64_t> shaders_reported_;
  std::vector<uint64_t> new_top_shaders_;

  // The window Log reports.
  std::unordered_map<uint64_t, Group> groups_;
  uint64_t frames_collected_ = 0, frame_ticks_ = 0, frame_ticks_max_ = 0, idle_ticks_ = 0;
  uint64_t spans_collected_ = 0;
  uint64_t last_end_ = 0;  // end timestamp of the last collected frame
  uint64_t last_frame_ticks_ = 0;

  uint64_t next_number_ = 0;
  uint64_t typical_ticks_ = 0;  // running mean of frame GPU time, spikes clamped
  uint32_t spikes_logged_ = 0;  // in the current Log window
  std::vector<SpikeSpan> spike_spans_;
};

}  // namespace pgr4::native
