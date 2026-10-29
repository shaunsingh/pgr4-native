// pgr4-native PGR4 native renderer: the presenter's debug tools (present_debug.cpp), on the
// presenter's thread unless noted. Only present.cpp uses them.
#pragma once

#include "present.h"

#include <cstdint>
#include <memory>
#include <vector>

struct ID3D12Device;
struct ID3D12Resource;

namespace pgr4::native {
class CommandStream;
}

namespace pgr4::native::present_debug {

// The blit shader's root constants (present.cpp's kBlitHlsl, b0).
struct BlitConstants {
  float image_size[2];
  uint32_t ramp_mode, point_sampled;
  uint32_t hdr;
  float paper_white, peak, knee;
  float white[3];  // HDR: the ramp's white in linear light
};

// 32bpp host texels (row pitch; RGBA8 or RGB10A2 unorm) -> 8-bit RGB with a D3D12 component
// mapping (the fetch swizzle).
void ToRgb(const uint8_t* src, uint32_t pitch, uint32_t width, uint32_t height, uint32_t mapping,
           bool rgb10a2, std::vector<uint8_t>* rgb);
// rgb: width * height * 3 bytes, written as stored deflate blocks (no compression library).
bool WritePng(const wchar_t* path, const std::vector<uint8_t>& rgb, uint32_t width,
              uint32_t height);

// --pgr4_native_glitch_catch: the blit drawn again at half the image's size (a 2x2 box filter),
// copied to a readback slot per frame in flight and handed, when the slot comes round, to a
// thread of its own. That thread keeps the last frames in a ring and writes .rec files of the
// frames around a short jump away from the image and back, and of the whole ring at a mark.
class GlitchCatch {
 public:
  GlitchCatch();
  ~GlitchCatch();
  GlitchCatch(const GlitchCatch&) = delete;
  GlitchCatch& operator=(const GlitchCatch&) = delete;

  // Reads --pgr4_native_glitch_catch; when it is empty the catcher stays off.
  void Initialize();
  bool on() const { return state_ != nullptr; }
  // Every Swap, before Record: hands the slot's previous frame (its fence was waited for) over.
  void Collect(uint32_t slot);
  // In the SDR blit's state (root signature, pipeline, source and ramp bound): records this
  // frame.
  void Record(ID3D12Device* device, CommandStream* list, uint32_t slot, float iw, float ih,
              uint32_t ramp_mode, uint64_t frame);
  // A mark at `frame`: the ring is written with the next frame handed over.
  void Mark(uint64_t frame);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

// --pgr4_native_luma_probe: LumaProbe::kRows rows of each front buffer, copied to a readback
// slot per frame in flight and read when the slot comes round again (its frame has completed).
class LumaProbeReadback {
 public:
  LumaProbeReadback();
  ~LumaProbeReadback();
  LumaProbeReadback(const LumaProbeReadback&) = delete;
  LumaProbeReadback& operator=(const LumaProbeReadback&) = delete;

  // Reads the slot's previous rows into result(), then records the copies of this frame's
  // `width` x `height` image at the top left of `texture` (in COPY_SOURCE).
  void Record(ID3D12Device* device, CommandStream* list, uint32_t slot, ID3D12Resource* texture,
              uint32_t width, uint32_t height, uint32_t mapping, bool rgb10a2, uint64_t frame);
  const LumaProbe& result() const { return result_; }

 private:
  struct State;
  std::unique_ptr<State> state_;
  LumaProbe result_;
};

}  // namespace pgr4::native::present_debug
