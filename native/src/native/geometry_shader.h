// Geometry shaders the DXBC translator's vertex shaders need for Xenos primitive types D3D12 does
// not have: point sprites (point lists), rectangle lists (3 vertices -> 4) and quad lists (drawn
// as lines with adjacency). Same keys and output as the Xenos plugin's pipeline cache.
#pragma once

#include <cstdint>
#include <vector>

#include <d3d12.h>

#include <rex/graphics/xenos.h>

namespace pgr4::native {

enum class GeometryShaderType : uint32_t { kNone, kPointList, kRectangleList, kQuadList };

// d3d12::PipelineCache::GeometryShaderKey.
union GeometryShaderKey {
  uint32_t key = 0;
  struct {
    GeometryShaderType type : 2;
    uint32_t interpolator_count : 5;
    uint32_t user_clip_plane_count : 3;
    uint32_t user_clip_plane_cull : 1;
    uint32_t has_vertex_kill_and : 1;
    uint32_t has_point_size : 1;
    uint32_t has_point_coordinates : 1;
    uint32_t point_ps_ucp_mode : 2;  // PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives
  };
};
// Stored in pipelines.bin inside the pipeline key.
static_assert(sizeof(GeometryShaderKey) == sizeof(uint32_t));

// Host input topology for a guest primitive type (after the primitive processor: triangle
// fans/strips etc. need no geometry shader).
D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyTypeForPrimitive(rex::graphics::xenos::PrimitiveType type);

// GeometryShaderKey::key of the geometry shader a draw of `type` needs, 0 for none (a key's type
// is never kNone, so a valid key is nonzero). pixel_modification is 0 when the draw has no pixel
// shader.
uint32_t GeometryShaderKeyFor(rex::graphics::xenos::PrimitiveType type,
                              uint64_t vertex_modification, uint64_t pixel_modification);

// DXBC of the geometry shader for a key (cache the result per key).
std::vector<uint32_t> CreateGeometryShader(GeometryShaderKey key);

}  // namespace pgr4::native
