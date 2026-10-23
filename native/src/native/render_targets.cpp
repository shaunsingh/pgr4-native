// pgr4-native PGR4 native renderer (M2 owner #5): host render targets and Clear. See
// render_targets.h for the model (one host target per guest surface, no EDRAM emulation).
#include "render_targets.h"

#include "renderer.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pgr4::native {
namespace {

constexpr uint32_t kMaxViews = 256;   // RTVs and DSVs each; PGR4 uses ~20 surfaces per frame
constexpr uint32_t kNullRtvs = 3;     // null RTVs for 1, 2 and 4 samples, at the heap's start
constexpr uint32_t kEdramTiles = 2048;  // 10 MiB of 80x16-sample tiles
constexpr uint32_t kMaxHeight = 2048;

// Same translation as the generated code: the 0xE0000000 view sits 4 KiB higher on the host.
inline uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a + (a >= 0xE0000000u ? 0x1000u : 0u), 4);
  return __builtin_bswap32(v);
}
inline float LoadF(const uint8_t* base, uint32_t a) {
  uint32_t v = Load32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

uint32_t SamplesOf(uint32_t surface_info) {
  uint32_t m = (surface_info >> 16) & 3;
  return m >= 2 ? 4 : m == 1 ? 2 : 1;
}

bool Is64bpp(uint32_t color_format) {
  // 16_16_16_16, 16_16_16_16_FLOAT, 32_32_FLOAT (xenos::IsColorRenderTargetFormat64bpp).
  return color_format == 5 || color_format == 7 || color_format == 15;
}

// Rows of pixels the EDRAM holds from `base` for this surface (what a non-tiling guest could
// render); the host target is at least this tall so it rarely has to grow.
uint32_t EdramRows(uint32_t base, uint32_t pitch, uint32_t samples, bool wide) {
  const uint32_t sx = samples >= 4 ? 2 : 1, sy = samples >= 2 ? 2 : 1;
  const uint32_t tiles_x = std::max(1u, (pitch * sx * (wide ? 2 : 1) + 79) / 80);
  const uint32_t free_tiles = base < kEdramTiles ? kEdramTiles - base : 0;
  return (free_tiles / tiles_x) * 16 / sy;
}

// A surface's EDRAM layout: tiles of 80 dwords x 16 sample rows; a pixel takes 2 x 2 samples
// with 4x MSAA and 1 x 2 with 2x, and a 64bpp sample takes 2 dwords.
struct EdramGrid {
  uint32_t base;   // tile
  uint32_t pitch;  // tiles per row
  uint32_t dx;     // dwords per pixel
  uint32_t dy;     // sample rows per pixel
};

EdramGrid GridOf(const RenderTarget& rt) {
  EdramGrid g;
  g.base = rt.edram_base;
  g.dx = (rt.samples >= 4 ? 2 : 1) * (!rt.depth && Is64bpp(rt.guest_format) ? 2 : 1);
  g.dy = rt.samples >= 2 ? 2 : 1;
  g.pitch = std::max(1u, (rt.pitch * g.dx + 79) / 80);
  return g;
}

// Tiles [base, end) under a surface's first `rows` rows of pixels.
uint32_t GridEnd(const EdramGrid& g, uint32_t rows) { return g.base + (rows * g.dy + 15) / 16 * g.pitch; }

// The EDRAM format a color format stores as (the _AS_ formats only change blending).
uint32_t StorageFormat(uint32_t color_format) {
  return color_format == 10 ? 2u : color_format == 12 ? 3u : color_format;
}

// Dword columns and sample rows on a tile grid (an EDRAM fill's rectangle, render_targets.h).
struct GridSpan {
  int64_t x0, x1, y0, y1;
};

// The spans of grid `g` that `s` on the grid at tile `base` with `pitch` covers: one on the
// same pitch at a whole number of rows from g's base, up to three (a row's tail, whole rows, a
// row's head) for whole tile rows on another pitch. 0 when it covers nothing of g, -1 when its
// shape does not map (part of a row on another pitch).
int32_t MapSpan(uint32_t base, uint32_t pitch, const GridSpan& s, const EdramGrid& g, GridSpan* out) {
  const int64_t d = int64_t(base) - int64_t(g.base);  // tiles
  const int64_t p = g.pitch;
  if (pitch == g.pitch && d % p == 0) {
    out[0] = {s.x0, s.x1, s.y0 + d / p * 16, s.y1 + d / p * 16};
    return 1;
  }
  if (s.x0 != 0 || s.x1 != int64_t(pitch) * 80 || s.y0 % 16 || s.y1 % 16) return -1;
  const int64_t a = std::max<int64_t>(d + s.y0 / 16 * pitch, 0);
  const int64_t b = d + s.y1 / 16 * pitch;
  if (b <= a) return 0;
  const int64_t ra = a / p, rb = b / p;
  if (ra == rb) {
    out[0] = {a % p * 80, b % p * 80, ra * 16, ra * 16 + 16};
    return 1;
  }
  int32_t n = 0;
  int64_t full = ra;
  if (a % p) {
    out[n++] = {a % p * 80, p * 80, ra * 16, ra * 16 + 16};
    ++full;
  }
  if (rb > full) out[n++] = {0, p * 80, full * 16, rb * 16};
  if (b % p) out[n++] = {0, b % p * 80, rb * 16, rb * 16 + 16};
  return n;
}

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

}  // namespace

SurfaceRegs SurfaceRegsFromRegisters(const uint32_t* regs) {
  SurfaceRegs s;
  s.surface_info = regs[0x2000];
  s.color_info[0] = regs[0x2001];
  s.depth_info = regs[0x2002];
  s.color_info[1] = regs[0x2003];
  s.color_info[2] = regs[0x2004];
  s.color_info[3] = regs[0x2005];
  return s;
}

SurfaceRegs SurfaceRegsFromDevice(const uint8_t* base, uint32_t device) {
  const uint32_t b = device + kDevShadow2000;
  SurfaceRegs s;
  s.surface_info = Load32(base, b + 0 * 4);
  s.color_info[0] = Load32(base, b + 1 * 4);
  s.depth_info = Load32(base, b + 2 * 4);
  s.color_info[1] = Load32(base, b + 3 * 4);
  s.color_info[2] = Load32(base, b + 4 * 4);
  s.color_info[3] = Load32(base, b + 5 * 4);
  return s;
}

DXGI_FORMAT RenderTargets::ColorFormat(uint32_t f) {
  switch (f) {
    case 0:   // k_8_8_8_8
    case 1:   // k_8_8_8_8_GAMMA (gamma not applied on the host yet)
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 2:   // k_2_10_10_10
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 3:   // k_2_10_10_10_FLOAT (7e3)
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16: same EDRAM data, 16-bit blending
    case 7:   // k_16_16_16_16_FLOAT
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 4: return DXGI_FORMAT_R16G16_SNORM;        // k_16_16 (-32..32 on Xenos; scaled)
    case 5: return DXGI_FORMAT_R16G16B16A16_SNORM;  // k_16_16_16_16
    case 6: return DXGI_FORMAT_R16G16_FLOAT;
    case 14: return DXGI_FORMAT_R32_FLOAT;
    case 15: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

RenderTargets::RenderTargets() = default;
RenderTargets::~RenderTargets() = default;

bool RenderTargets::Initialize(ID3D12Device* device) {
  device_ = device;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = kMaxViews + kNullRtvs;
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)), "RT CreateDescriptorHeap(RTV)"))
    return false;
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  hd.NumDescriptors = kMaxViews;
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap_)), "RT CreateDescriptorHeap(DSV)"))
    return false;
  rtv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  dsv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  rtv_start_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  for (uint32_t i = kMaxViews; i-- > 0;) {
    free_rtv_.push_back(kNullRtvs + i);
    free_dsv_.push_back(i);
  }
  // Null RTVs fill the gaps below the highest bound color target (D3D12 needs a descriptor in
  // every slot of OMSetRenderTargets; the sample count of a null view must match the others).
  for (uint32_t i = 0; i < kNullRtvs; ++i) {
    D3D12_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    v.ViewDimension = i == 0 ? D3D12_RTV_DIMENSION_TEXTURE2D : D3D12_RTV_DIMENSION_TEXTURE2DMS;
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(i) * rtv_stride_;
    device->CreateRenderTargetView(nullptr, &v, h);
  }
  return true;
}

uint32_t RenderTargets::AllocView(bool depth) {
  auto& list = depth ? free_dsv_ : free_rtv_;
  if (list.empty()) return UINT32_MAX;
  uint32_t i = list.back();
  list.pop_back();
  return i;
}

void RenderTargets::Retire(ComPtr<ID3D12Resource> resource) {
  if (resource) retired_.push_back({std::move(resource), frame_fence_});
}

bool RenderTargets::Create(RenderTarget* rt, uint32_t height) {
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = std::max(1u, rt->pitch);
  d.Height = height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = rt->resource_format;
  d.SampleDesc.Count = rt->samples;
  d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  d.Flags = rt->depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  const D3D12_RESOURCE_STATES initial =
      rt->depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  ComPtr<ID3D12Resource> res;
  HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initial, nullptr,
                                                IID_PPV_ARGS(&res));
  if (FAILED(hr)) {
    REXLOG_ERROR("PGR4 native RT: CreateCommittedResource {}x{} fmt {} x{} failed: {:08X}", d.Width,
                 d.Height, int(d.Format), rt->samples, uint32_t(hr));
    return false;
  }
  if (log_)
    std::fprintf(log_, "rt created %s base %u pitch %u x%u format %d %ux%u%s\n", rt->depth ? "depth" : "color",
                 rt->edram_base, rt->pitch, rt->samples, int(rt->view_format), uint32_t(d.Width), height,
                 rt->resource ? " (grown)" : "");
  Retire(std::move(rt->resource));
  rt->resource = std::move(res);
  rt->state = initial;
  rt->width = uint32_t(d.Width);
  rt->height = height;
  if (rt->view_index == UINT32_MAX) {
    rt->view_index = AllocView(rt->depth);
    if (rt->view_index == UINT32_MAX) {
      REXLOG_ERROR("PGR4 native RT: out of {} views", rt->depth ? "depth" : "color");
      return false;
    }
  }
  if (rt->depth) {
    D3D12_DEPTH_STENCIL_VIEW_DESC v = {};
    v.Format = rt->view_format;
    v.ViewDimension = rt->samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    rt->view = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    rt->view.ptr += size_t(rt->view_index) * dsv_stride_;
    device_->CreateDepthStencilView(rt->resource.Get(), &v, rt->view);
  } else {
    D3D12_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = rt->view_format;
    v.ViewDimension = rt->samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
    rt->view = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rt->view.ptr += size_t(rt->view_index) * rtv_stride_;
    device_->CreateRenderTargetView(rt->resource.Get(), &v, rt->view);
  }
  return true;
}

RenderTarget* RenderTargets::Get(bool depth, uint32_t surface_info, uint32_t info, uint32_t height) {
  const uint32_t pitch = surface_info & 0x3FFF;
  const uint32_t samples = SamplesOf(surface_info);
  const uint32_t base = info & 0xFFF;
  DXGI_FORMAT resource, view, srv, stencil_srv = DXGI_FORMAT_UNKNOWN;
  uint32_t guest_format;
  bool wide = false;
  if (depth) {
    guest_format = (info >> 16) & 1;
    if (guest_format) {  // kD24FS8: 20e4 float depth, kept as 32-bit float on the host
      resource = DXGI_FORMAT_R32G8X24_TYPELESS;
      view = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
      srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
      stencil_srv = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    } else {  // kD24S8
      resource = DXGI_FORMAT_R24G8_TYPELESS;
      view = DXGI_FORMAT_D24_UNORM_S8_UINT;
      srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      stencil_srv = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    }
  } else {
    guest_format = (info >> 16) & 0xF;
    view = ColorFormat(guest_format);
    if (view == DXGI_FORMAT_UNKNOWN) {
      static uint32_t logged = 0;
      if (logged++ < 8) REXLOG_WARN("PGR4 native RT: unsupported color format {}", guest_format);
      return nullptr;
    }
    resource = srv = view;
    wide = Is64bpp(guest_format);
  }
  if (pitch == 0) return nullptr;
  const uint64_t key = (uint64_t(depth) << 63) | (uint64_t(samples >> 1) << 60) | (uint64_t(base) << 44) |
                       (uint64_t(pitch) << 30) | uint64_t(view);
  GetMemo& memo = get_memo_[(key ^ (key >> 30) ^ (key >> 44) ^ (key >> 60)) & 15];
  if (memo.rt && memo.key == key && std::min(height, kMaxHeight) <= memo.rt->height) {
    memo.rt->guest_format = guest_format;
    memo.rt->last_frame = frame_;
    return memo.rt;
  }
  uint32_t want = std::max(std::min(height, kMaxHeight), EdramRows(base, pitch, samples, wide));
  want = std::min((std::max(want, 8u) + 7) & ~7u, kMaxHeight);
  auto it = targets_.find(key);
  RenderTarget* rt;
  if (it == targets_.end()) {
    auto owned = std::make_unique<RenderTarget>();
    rt = owned.get();
    rt->key = key;
    rt->depth = depth;
    rt->edram_base = base;
    rt->pitch = pitch;
    rt->samples = samples;
    rt->resource_format = resource;
    rt->view_format = view;
    rt->srv_format = srv;
    rt->stencil_srv_format = stencil_srv;
    rt->view_index = UINT32_MAX;
    if (!Create(rt, want)) {
      if (rt->view_index != UINT32_MAX) (depth ? free_dsv_ : free_rtv_).push_back(rt->view_index);
      return nullptr;
    }
    targets_.emplace(key, std::move(owned));
  } else {
    rt = it->second.get();
    if (std::min(height, kMaxHeight) > rt->height) {
      // Taller than anything this surface had: a new, taller target. The old content is lost
      // (not seen in PGR4's traces: every surface is created at its EDRAM height or more).
      ++recreated_;
      REXLOG_WARN("PGR4 native RT: growing {} base {} pitch {} x{} from {} to {} rows", depth ? "depth" : "color",
                  base, pitch, samples, rt->height, want);
      if (!Create(rt, want)) return nullptr;
    }
  }
  rt->guest_format = guest_format;
  rt->last_frame = frame_;
  memo = {key, rt};
  return rt;
}

RenderTarget* RenderTargets::GetColor(const SurfaceRegs& regs, uint32_t index, uint32_t height) {
  return Get(false, regs.surface_info, regs.color_info[index & 3], height);
}

RenderTarget* RenderTargets::GetDepth(const SurfaceRegs& regs, uint32_t height) {
  return Get(true, regs.surface_info, regs.depth_info, height);
}

void RenderTargets::Transition(ID3D12GraphicsCommandList* list, RenderTarget* rt,
                               D3D12_RESOURCE_STATES state) {
  if (!rt || rt->state == state) return;
  D3D12_RESOURCE_BARRIER b = TransitionBarrier(rt->resource.Get(), rt->state, state);
  list->ResourceBarrier(1, &b);
  rt->state = state;
}

bool RenderTargets::Bind(ID3D12GraphicsCommandList* list, const SurfaceRegs& regs, uint32_t color_mask,
                         bool depth_used, uint32_t height, BoundTargets* out) {
  *out = BoundTargets{};
  const uint32_t samples = SamplesOf(regs.surface_info);
  out->samples = samples;
  out->width = UINT32_MAX;
  out->height = UINT32_MAX;
  D3D12_CPU_DESCRIPTOR_HANDLE null_rtv = rtv_start_;
  null_rtv.ptr += size_t(samples == 4 ? 2 : samples == 2 ? 1 : 0) * rtv_stride_;
  for (uint32_t i = 0; i < 4; ++i) {
    out->rtv[i] = null_rtv;
    if (!(color_mask >> i & 1)) continue;
    RenderTarget* rt = GetColor(regs, i, height);
    if (!rt) return false;
    Transition(list, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (rt->fill_seq != fill_seq_) SyncEdram(list, rt);
    out->color[i] = rt;
    out->rtv[i] = rt->view;
    out->color_count = i + 1;
    out->width = std::min(out->width, rt->width);
    out->height = std::min(out->height, rt->height);
  }
  if (depth_used) {
    RenderTarget* rt = GetDepth(regs, height);
    if (!rt) return false;
    Transition(list, rt, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    out->depth = rt;
    out->dsv = rt->view;
    out->width = std::min(out->width, rt->width);
    out->height = std::min(out->height, rt->height);
  }
  if (out->width == UINT32_MAX) out->width = out->height = 0;
  // The draw renders over these tiles (conservatively: its targets' rows above `height`).
  if (!fills_.empty()) {
    for (uint32_t i = 0; i < out->color_count; ++i)
      if (out->color[i]) DropFills(*out->color[i], height);
    if (out->depth) DropFills(*out->depth, height);
  }
  return true;
}

void RenderTargets::OnClear(ID3D12GraphicsCommandList* list, const uint8_t* base, const HookArgs& args) {
  ClearCall c;
  if (DecodeClear(base, args, &c)) Clear(list, c.regs, c.bound_colors, c.depth_bound, c.height, c.p);
}

bool RenderTargets::DecodeClear(const uint8_t* base, const HookArgs& args, ClearCall* out) {
  ClearParams& p = out->p;
  auto read_rect = [&](uint32_t a) {
    // D3DRECT {x1, y1, x2, y2}, big-endian LONGs.
    D3D12_RECT r;
    r.left = LONG(int32_t(Load32(base, a)));
    r.top = LONG(int32_t(Load32(base, a + 4)));
    r.right = LONG(int32_t(Load32(base, a + 8)));
    r.bottom = LONG(int32_t(Load32(base, a + 12)));
    if (r.right > r.left && r.bottom > r.top) p.rects.push_back(r);
  };
  if (args.fn == HookFn::kClear) {
    // Clear(dev, Count r4, pRects r5, Flags r6, Color r7, Z f1, Stencil r9): Z takes r8's
    // argument slot (sub_8269C4A0 passes r9 on to ClearF as its stencil).
    const uint32_t count = args.r[1], rects = args.r[2], color = args.r[4];
    p.flags = args.r[3];
    p.color[0] = float((color >> 16) & 0xFF) / 255.0f;
    p.color[1] = float((color >> 8) & 0xFF) / 255.0f;
    p.color[2] = float(color & 0xFF) / 255.0f;
    p.color[3] = float(color >> 24) / 255.0f;
    p.depth = float(args.f[0]);
    p.stencil = uint8_t(args.r[6]);
    if (rects)
      for (uint32_t i = 0; i < count && i < 64; ++i) read_rect(rects + i * 16);
    if (rects && count && p.rects.empty()) return false;  // only empty rectangles
  } else if (args.fn == HookFn::kClearF) {
    // ClearF(dev, Flags r4, pRect r5, pColor r6 (D3DVECTOR4), Z f1, Stencil r8): Z takes r7's
    // argument slot (sub_8269BCE8 reads the stencil from r8).
    p.flags = args.r[1];
    if (args.r[3])
      for (int i = 0; i < 4; ++i) p.color[i] = LoadF(base, args.r[3] + 4 * i);
    p.depth = float(args.f[0]);
    p.stencil = uint8_t(args.r[5]);
    if (args.r[2]) {
      read_rect(args.r[2]);
      if (p.rects.empty()) return false;
    }
    // Inside BeginTiling (fn_8269C6D0) the rectangle is the largest tile's extent at the EDRAM
    // base. Every tile starts from these values: the first from this clear, each later one
    // from the previous tile's resolve, which EndTiling clears with BeginTiling's color, Z and
    // stencil. The host surface holds all tiles, so it clears each of them.
    if (args.tiling == TilingCall::kBegin && !p.rects.empty()) {
      const uint32_t tiles = std::min(Load32(base, args.device + kDevTileCount), kMaxTiles);
      if (tiles) {
        p.edram_rects = std::move(p.rects);
        p.rects.clear();
        for (uint32_t i = 0; i < tiles; ++i) read_rect(args.device + kDevTileRects + 16 * i);
      }
    }
  } else {
    return false;
  }
  const uint32_t device = args.device;
  out->regs = SurfaceRegsFromDevice(base, device);
  uint32_t bound = 0;
  for (uint32_t i = 0; i < 4; ++i)
    if (Load32(base, device + kDevColorSurfaces + 4 * i)) bound |= 1u << i;
  out->bound_colors = bound;
  out->depth_bound = Load32(base, device + kDevDepthSurface) != 0;
  // The surfaces' height is not in the registers; the viewport bottom is the best hint
  // (PA_CL_VPORT_YSCALE 0x2111, YOFFSET 0x2112). Targets are at least their EDRAM height.
  const float ys = LoadF(base, device + kDevShadow2100 + 0x11 * 4);
  const float yo = LoadF(base, device + kDevShadow2100 + 0x12 * 4);
  float bottom = std::fabs(ys) + yo;
  uint32_t height = (bottom > 0.0f && bottom <= float(kMaxHeight)) ? uint32_t(std::ceil(bottom)) : 0;
  for (const D3D12_RECT& r : p.rects) height = std::max(height, uint32_t(std::max<LONG>(r.bottom, 0)));
  out->height = height;
  return true;
}

void RenderTargets::Clear(ID3D12GraphicsCommandList* list, const SurfaceRegs& regs, uint32_t bound_colors,
                          bool depth_bound, uint32_t height, const ClearParams& p) {
  const UINT nrects = UINT(p.rects.size());
  const D3D12_RECT* rects = nrects ? p.rects.data() : nullptr;
  // What the clear leaves in the EDRAM, in pixels from the surface's base. A whole-surface
  // clear covers the rows of the call's height hint (the target itself may be taller).
  const std::vector<D3D12_RECT>& edram = p.edram_rects.empty() ? p.rects : p.edram_rects;
  auto edram_rows = [&](const RenderTarget& rt) {
    if (edram.empty()) return height ? std::min(height, rt.height) : rt.height;
    uint32_t rows = 0;
    for (const D3D12_RECT& r : edram) rows = std::max(rows, uint32_t(std::max<LONG>(r.bottom, 0)));
    return rows;
  };
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(p.flags >> i & 1) || !(bound_colors >> i & 1)) continue;
    RenderTarget* rt = GetColor(regs, i, height);
    if (!rt) continue;
    Transition(list, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // Outside the rectangles the target shows what the EDRAM holds there.
    if (nrects && rt->fill_seq != fill_seq_) SyncEdram(list, rt);
    list->ClearRenderTargetView(rt->view, p.color, nrects, rects);
    if (edram.empty()) {
      RecordFill(*rt, {0, 0, LONG(rt->pitch), LONG(edram_rows(*rt))}, p.color);
    } else {
      for (const D3D12_RECT& r : edram) RecordFill(*rt, r, p.color);
    }
    rt->fill_seq = fill_seq_;
  }
  D3D12_CLEAR_FLAGS df = D3D12_CLEAR_FLAGS(0);
  if (p.flags & 0x10) df |= D3D12_CLEAR_FLAG_DEPTH;
  if (p.flags & 0x20) df |= D3D12_CLEAR_FLAG_STENCIL;
  if (df && depth_bound) {
    RenderTarget* rt = GetDepth(regs, height);
    if (rt) {
      Transition(list, rt, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      list->ClearDepthStencilView(rt->view, df, std::clamp(p.depth, 0.0f, 1.0f), p.stencil, nrects, rects);
      if (!fills_.empty()) DropFills(*rt, edram_rows(*rt));
    }
  }
}

void RenderTargets::RecordFill(const RenderTarget& rt, const D3D12_RECT& r, const float color[4]) {
  const EdramGrid g = GridOf(rt);
  EdramFill f;
  f.base = g.base;
  f.pitch = g.pitch;
  f.x0 = uint32_t(std::max<LONG>(r.left, 0)) * g.dx;
  f.x1 = std::min(uint32_t(std::max<LONG>(r.right, 0)) * g.dx, g.pitch * 80);
  f.y0 = uint32_t(std::max<LONG>(r.top, 0)) * g.dy;
  f.y1 = uint32_t(std::max<LONG>(r.bottom, 0)) * g.dy;
  if (f.x1 <= f.x0 || f.y1 <= f.y0) return;
  f.format = StorageFormat(rt.guest_format);
  std::memcpy(f.color, color, sizeof(f.color));
  // Older fills inside this one are gone from the EDRAM: on the same grid, or anywhere in its
  // run of tiles when it covers whole tile rows.
  const bool whole_rows = f.x0 == 0 && f.x1 == f.pitch * 80 && f.y0 % 16 == 0 && f.y1 % 16 == 0;
  auto covered = [&](const EdramFill& o) {
    if (o.base == f.base && o.pitch == f.pitch)
      return o.x0 >= f.x0 && o.x1 <= f.x1 && o.y0 >= f.y0 && o.y1 <= f.y1;
    return whole_rows && o.first_tile() >= f.first_tile() && o.end_tile() <= f.end_tile();
  };
  fills_.erase(std::remove_if(fills_.begin(), fills_.end(), covered), fills_.end());
  if (fills_.size() >= kMaxFills) fills_.erase(fills_.begin());
  f.seq = ++fill_seq_;
  fills_.push_back(f);
}

void RenderTargets::DropFills(const RenderTarget& rt, uint32_t rows) {
  const EdramGrid g = GridOf(rt);
  const uint32_t begin = g.base, end = GridEnd(g, rows);
  fills_.erase(std::remove_if(fills_.begin(), fills_.end(),
                              [&](const EdramFill& f) { return f.first_tile() < end && f.end_tile() > begin; }),
               fills_.end());
}

void RenderTargets::SyncEdram(ID3D12GraphicsCommandList* list, RenderTarget* rt) {
  static uint32_t log_applied = 0, log_format = 0, log_shape = 0;
  const uint64_t since = rt->fill_seq;
  rt->fill_seq = fill_seq_;
  if (rt->depth || since == fill_seq_) return;
  const EdramGrid g = GridOf(*rt);
  const uint32_t end = GridEnd(g, rt->height);
  const uint32_t format = StorageFormat(rt->guest_format);
  for (const EdramFill& f : fills_) {
    if (f.seq <= since || f.first_tile() >= end || f.end_tile() <= g.base) continue;
    GridSpan spans[3];
    const int32_t n = MapSpan(f.base, f.pitch, {f.x0, f.x1, f.y0, f.y1}, g, spans);
    if (n < 0) {
      if (log_shape++ < 4)
        REXLOG_INFO("PGR4 native RT: EDRAM fill at tile {} pitch {} ({}x{} dwords x rows) does not map onto base {} "
                    "pitch {}", f.base, f.pitch, f.x1 - f.x0, f.y1 - f.y0, g.base, g.pitch);
      continue;
    }
    // The whole pixels inside each span, clipped to the target.
    D3D12_RECT rects[3];
    UINT nr = 0;
    for (int32_t i = 0; i < n; ++i) {
      const GridSpan& s = spans[i];
      const int64_t x0 = (s.x0 + g.dx - 1) / g.dx, x1 = std::min<int64_t>(s.x1 / g.dx, rt->width);
      const int64_t y0 = (std::max<int64_t>(s.y0, 0) + g.dy - 1) / g.dy;
      const int64_t y1 = std::min<int64_t>(s.y1 / g.dy, rt->height);
      if (x1 > x0 && y1 > y0) rects[nr++] = {LONG(x0), LONG(y0), LONG(x1), LONG(y1)};
    }
    if (!nr) continue;
    if (f.format != format) {
      if (log_format++ < 4)
        REXLOG_INFO("PGR4 native RT: EDRAM fill of format {} at tile {} not applied to format {} at base {}", f.format,
                    f.base, format, g.base);
      continue;
    }
    Transition(list, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(rt->view, f.color, nr, rects);
    if (log_)
      std::fprintf(log_, "edram fill seq %llu at tile %u pitch %u color %g,%g,%g,%g applied to base %u pitch %u x%u "
                   "rows %ld-%ld (%u rects)\n", (unsigned long long)f.seq, f.base, f.pitch, f.color[0], f.color[1],
                   f.color[2], f.color[3], g.base, g.pitch, rt->samples, rects[0].top, rects[nr - 1].bottom, nr);
    if (log_applied++ < 4)
      REXLOG_INFO("PGR4 native RT: EDRAM fill at tile {} pitch {} (alpha {}) applied to base {} pitch {} x{} rows "
                  "{}-{}", f.base, f.pitch, f.color[3], g.base, g.pitch, rt->samples, rects[0].top, rects[nr - 1].bottom);
  }
}

void RenderTargets::BeginFrame(uint64_t frame, uint64_t frame_fence, uint64_t completed_fence) {
  frame_ = frame;
  frame_fence_ = frame_fence;
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                [&](const Retired& r) { return r.fence <= completed_fence; }),
                 retired_.end());
  for (auto it = targets_.begin(); it != targets_.end();) {
    RenderTarget* rt = it->second.get();
    if (rt->last_frame + kMaxAge < frame) {
      // Last used kMaxAge frames ago: no list in flight references it, but retire it with the
      // current fence anyway (cheap) so the rule is the same everywhere.
      if (log_)
        std::fprintf(log_, "rt dropped %s base %u pitch %u x%u (last used frame %llu)\n",
                     rt->depth ? "depth" : "color", rt->edram_base, rt->pitch, rt->samples,
                     (unsigned long long)rt->last_frame);
      Retire(std::move(rt->resource));
      (rt->depth ? free_dsv_ : free_rtv_).push_back(rt->view_index);
      it = targets_.erase(it);
      for (GetMemo& m : get_memo_) m = {};
    } else {
      ++it;
    }
  }
}

}  // namespace pgr4::native
