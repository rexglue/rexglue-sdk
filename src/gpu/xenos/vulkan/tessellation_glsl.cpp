#include "gpu/xenos/vulkan/tessellation_glsl.h"

#include <cstddef>
#include <format>

#include "gpu/xenos/pipeline/shader/spirv_translator.h"

namespace rex::gpu::vulkan {

std::string GetTessellationSystemConstantsBlockGLSL() {
  std::string source;
  source += std::format("layout(set = {}, binding = {}, std140) uniform XeSystemConstants {{\n",
                        static_cast<i32>(SPIRVShaderTranslator::kDescriptorSetConstants),
                        static_cast<i32>(SPIRVShaderTranslator::kConstantBufferSystem));
  source += std::format("  layout(offset = {}) uint xe_vertex_index_endian;\n",
                        offsetof(SPIRVShaderTranslator::SystemConstants, vertex_index_endian));
  source += std::format("  layout(offset = {}) int xe_vertex_base_index;\n",
                        offsetof(SPIRVShaderTranslator::SystemConstants, vertex_base_index));
  source += std::format("  layout(offset = {}) uint xe_vertex_index_min;\n",
                        offsetof(SPIRVShaderTranslator::SystemConstants, vertex_index_min));
  source += std::format("  layout(offset = {}) uint xe_vertex_index_max;\n",
                        offsetof(SPIRVShaderTranslator::SystemConstants, vertex_index_max));
  source +=
      std::format("  layout(offset = {}) float xe_tessellation_factor_range_min;\n",
                  offsetof(SPIRVShaderTranslator::SystemConstants, tessellation_factor_range_min));
  source +=
      std::format("  layout(offset = {}) float xe_tessellation_factor_range_max;\n",
                  offsetof(SPIRVShaderTranslator::SystemConstants, tessellation_factor_range_max));
  source += "} xe_system_cbuffer;\n";
  return source;
}

std::string GetTessellationIndexedVertexShaderGLSL() {
  std::string source;
  source += "#version 450\n";
  source += GetTessellationSystemConstantsBlockGLSL();
  source += R"(
layout(location = 0) out float xe_out_value;

uint xe_swap_8_in_16(uint value) {
  return ((value << 8u) & 0xFF00FF00u) | ((value >> 8u) & 0x00FF00FFu);
}

uint xe_swap_16_in_32(uint value) {
  return ((value & 0x0000FFFFu) << 16u) | (value >> 16u);
}

void main() {
  uint value = uint(gl_VertexIndex);
  uint endian = xe_system_cbuffer.xe_vertex_index_endian;
  if (endian == 1u || endian == 2u || endian == 3u) {
    value = xe_swap_8_in_16(value);
  }
  if (endian == 2u || endian == 3u) {
    value = xe_swap_16_in_32(value);
  }
  value = uint(int(value) + xe_system_cbuffer.xe_vertex_base_index);
  value &= 0x00FFFFFFu;
  value = max(value, xe_system_cbuffer.xe_vertex_index_min);
  value = min(value, xe_system_cbuffer.xe_vertex_index_max);
  xe_out_value = float(value);
}
)";
  return source;
}

std::string GetTessellationAdaptiveVertexShaderGLSL() {
  std::string source;
  source += "#version 450\n";
  source += GetTessellationSystemConstantsBlockGLSL();
  source += R"(
layout(location = 0) out float xe_out_value;

uint xe_swap_8_in_16(uint value) {
  return ((value << 8u) & 0xFF00FF00u) | ((value >> 8u) & 0x00FF00FFu);
}

uint xe_swap_16_in_32(uint value) {
  return ((value & 0x0000FFFFu) << 16u) | (value >> 16u);
}

void main() {
  uint value = uint(gl_VertexIndex);
  uint endian = xe_system_cbuffer.xe_vertex_index_endian;
  if (endian == 1u || endian == 2u || endian == 3u) {
    value = xe_swap_8_in_16(value);
  }
  if (endian == 2u || endian == 3u) {
    value = xe_swap_16_in_32(value);
  }
  float tessellation_factor = float(value) + 1.0;
  tessellation_factor = max(
      tessellation_factor, xe_system_cbuffer.xe_tessellation_factor_range_min);
  tessellation_factor = min(
      tessellation_factor, xe_system_cbuffer.xe_tessellation_factor_range_max);
  xe_out_value = tessellation_factor;
}
)";
  return source;
}

std::string GetTessellationControlShaderGLSL(Shader::HostVertexShaderType host_vertex_shader_type,
                                             xenos::TessellationMode tessellation_mode,
                                             u32 input_control_points, u32 output_control_points) {
  std::string source;
  source += "#version 450\n";
  source += GetTessellationSystemConstantsBlockGLSL();
  source += "layout(location = 0) in float xe_input_value[];\n";
  source += "layout(location = 0) patch out vec4 xe_out_patch_control_point_indices;\n";
  source += std::format("layout(vertices = {}) out;\n", output_control_points);

  source += R"(
void main() {
  gl_out[gl_InvocationID].gl_Position = vec4(0.0);
  if (gl_InvocationID != 0u) {
    return;
  }

  vec4 patch_control_point_indices = vec4(0.0);
)";
  if (tessellation_mode == xenos::TessellationMode::kAdaptive) {
    source += R"(
  uint patch_index = uint(int(gl_PrimitiveID) + xe_system_cbuffer.xe_vertex_base_index);
  patch_index &= 0x00FFFFFFu;
  patch_index = max(patch_index, xe_system_cbuffer.xe_vertex_index_min);
  patch_index = min(patch_index, xe_system_cbuffer.xe_vertex_index_max);
  patch_control_point_indices.x = float(patch_index);
)";
    if (IsTriangleDomainHostVertexShaderType(host_vertex_shader_type)) {
      source += R"(
  gl_TessLevelOuter[0] = xe_input_value[1];
  gl_TessLevelOuter[1] = xe_input_value[2];
  gl_TessLevelOuter[2] = xe_input_value[0];
  gl_TessLevelInner[0] = min(min(xe_input_value[2], xe_input_value[1]),
                             xe_input_value[0]);
)";
    } else {
      source += R"(
  gl_TessLevelOuter[0] = xe_input_value[3];
  gl_TessLevelOuter[1] = xe_input_value[0];
  gl_TessLevelOuter[2] = xe_input_value[1];
  gl_TessLevelOuter[3] = xe_input_value[2];
  gl_TessLevelInner[0] = min(xe_input_value[2], xe_input_value[0]);
  gl_TessLevelInner[1] = min(xe_input_value[1], xe_input_value[3]);
)";
    }
  } else {
    if (IsPatchIndexedHostVertexShaderType(host_vertex_shader_type)) {
      source += "  patch_control_point_indices.x = xe_input_value[0];\n";
    } else {
      source += "  patch_control_point_indices.x = xe_input_value[0];\n";
      if (input_control_points >= 2) {
        source += "  patch_control_point_indices.y = xe_input_value[1];\n";
      }
      if (input_control_points >= 3) {
        source += "  patch_control_point_indices.z = xe_input_value[2];\n";
      }
      if (input_control_points >= 4) {
        source += "  patch_control_point_indices.w = xe_input_value[3];\n";
      }
    }
    source += R"(
  float tessellation_factor = xe_system_cbuffer.xe_tessellation_factor_range_max;
)";
    if (IsTriangleDomainHostVertexShaderType(host_vertex_shader_type)) {
      source += R"(
  gl_TessLevelOuter[0] = tessellation_factor;
  gl_TessLevelOuter[1] = tessellation_factor;
  gl_TessLevelOuter[2] = tessellation_factor;
  gl_TessLevelInner[0] = tessellation_factor;
)";
    } else {
      source += R"(
  gl_TessLevelOuter[0] = tessellation_factor;
  gl_TessLevelOuter[1] = tessellation_factor;
  gl_TessLevelOuter[2] = tessellation_factor;
  gl_TessLevelOuter[3] = tessellation_factor;
  gl_TessLevelInner[0] = tessellation_factor;
  gl_TessLevelInner[1] = tessellation_factor;
)";
    }
  }
  source += R"(
  xe_out_patch_control_point_indices = patch_control_point_indices;
}
)";
  return source;
}

}  // namespace rex::gpu::vulkan
