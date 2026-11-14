/**
 * @file        gpu/xenos/vulkan/tessellation_glsl.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include "gpu/xenos/pipeline/shader/shader.h"
#include <rex/xenos/state.h>

namespace rex::gpu::vulkan {

constexpr bool IsTriangleDomainHostVertexShaderType(
    Shader::HostVertexShaderType host_vertex_shader_type) {
  return host_vertex_shader_type == Shader::HostVertexShaderType::kTriangleDomainCPIndexed ||
         host_vertex_shader_type == Shader::HostVertexShaderType::kTriangleDomainPatchIndexed;
}

constexpr bool IsQuadDomainHostVertexShaderType(
    Shader::HostVertexShaderType host_vertex_shader_type) {
  return host_vertex_shader_type == Shader::HostVertexShaderType::kQuadDomainCPIndexed ||
         host_vertex_shader_type == Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
}

constexpr bool IsPatchIndexedHostVertexShaderType(
    Shader::HostVertexShaderType host_vertex_shader_type) {
  return host_vertex_shader_type == Shader::HostVertexShaderType::kTriangleDomainPatchIndexed ||
         host_vertex_shader_type == Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
}

/**
 * @return The GLSL source.
 */
std::string GetTessellationSystemConstantsBlockGLSL();

/**
 * Returns the vertex shader that passes the endian swapped, biased and clamped control point index.
 */
std::string GetTessellationIndexedVertexShaderGLSL();

/**
 * @return The GLSL source.
 */
std::string GetTessellationAdaptiveVertexShaderGLSL();

/**
 * @param input_control_points Control points the shader reads per patch.
 * @param output_control_points Control points the shader writes per patch.
 * @return The GLSL source.
 */
std::string GetTessellationControlShaderGLSL(Shader::HostVertexShaderType host_vertex_shader_type,
                                             xenos::TessellationMode tessellation_mode,
                                             u32 input_control_points, u32 output_control_points);

}  // namespace rex::gpu::vulkan
