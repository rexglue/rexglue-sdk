/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_DXBC_GEOMETRY_SHADER_H_
#define XENIA_GPU_DXBC_GEOMETRY_SHADER_H_

#include <cstdint>
#include <functional>
#include <vector>

#include "rex/assert.h"
#include "gpu/xenos/pipeline/shader/dxbc_translator.h"

namespace rex {
namespace graphics {

// Geometry-shader emulation modes shared by the host backends. The Xbox 360
// expands point/rectangle/quad lists that desktop APIs render via a geometry
// shader; this DXBC generator is shared by the D3D12 and Metal backends (Metal
// then converts the DXBC through the Metal Shader Converter). Vulkan uses a
// separate SPIR-V path and is intentionally not part of this module.
enum class PipelineGeometryShader : uint32_t {
  kNone,
  kPointList,
  kRectangleList,
  kQuadList,
};

union GeometryShaderKey {
  uint32_t key;
  struct {
    PipelineGeometryShader type : 2;
    uint32_t interpolator_count : 5;
    uint32_t user_clip_plane_count : 3;
    uint32_t user_clip_plane_cull : 1;
    uint32_t has_vertex_kill_and : 1;
    uint32_t has_point_size : 1;
    uint32_t has_point_coordinates : 1;
  };

  GeometryShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

  struct Hasher {
    size_t operator()(const GeometryShaderKey& key) const {
      return std::hash<uint32_t>{}(key.key);
    }
  };
  bool operator==(const GeometryShaderKey& other_key) const {
    return key == other_key.key;
  }
  bool operator!=(const GeometryShaderKey& other_key) const {
    return !(*this == other_key);
  }
};

// Builds the geometry-shader cache key from the vertex/pixel shader
// modifications. Returns false if no geometry shader is needed for the type.
bool GetGeometryShaderKey(
    PipelineGeometryShader geometry_shader_type,
    ::rex::gpu::DXBCShaderTranslator::Modification vertex_shader_modification,
    ::rex::gpu::DXBCShaderTranslator::Modification pixel_shader_modification,
    GeometryShaderKey& key_out);

// Generates the DXBC geometry shader bytecode for the given key.
void CreateDxbcGeometryShader(GeometryShaderKey key,
                              std::vector<uint32_t>& shader_out);

}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_DXBC_GEOMETRY_SHADER_H_
