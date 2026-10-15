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
#include <rex/xenos/xenos.h>

namespace rex::gpu::vulkan {

/**
 * Reports whether the type tessellates over the triangle domain.
 *
 * @param host_vertex_shader_type The type to test.
 * @return True when the type tessellates over the triangle domain.
 */
constexpr bool IsTriangleDomainHostVertexShaderType(
    Shader::HostVertexShaderType host_vertex_shader_type) {
  return host_vertex_shader_type == Shader::HostVertexShaderType::kTriangleDomainCPIndexed ||
         host_vertex_shader_type == Shader::HostVertexShaderType::kTriangleDomainPatchIndexed;
}

/**
 * Reports whether the type tessellates over the quad domain.
 *
 * @param host_vertex_shader_type The type to test.
 * @return True when the type tessellates over the quad domain.
 */
constexpr bool IsQuadDomainHostVertexShaderType(
    Shader::HostVertexShaderType host_vertex_shader_type) {
  return host_vertex_shader_type == Shader::HostVertexShaderType::kQuadDomainCPIndexed ||
         host_vertex_shader_type == Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
}

/**
 * Reports whether the type indexes by patch rather than by control point.
 *
 * @param host_vertex_shader_type The type to test.
 * @return True when the type indexes by patch rather than by control point.
 */
constexpr bool IsPatchIndexedHostVertexShaderType(
    Shader::HostVertexShaderType host_vertex_shader_type) {
  return host_vertex_shader_type == Shader::HostVertexShaderType::kTriangleDomainPatchIndexed ||
         host_vertex_shader_type == Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
}

/**
 * Returns the declaration of the system constants block, bound at the constants descriptor set.
 *
 * @return The GLSL source.
 */
std::string GetTessellationSystemConstantsBlockGLSL();

/**
 * Returns the vertex shader that passes the endian swapped, biased and clamped index of a patch
 * control point.
 *
 * @return The GLSL source.
 */
std::string GetTessellationIndexedVertexShaderGLSL();

/**
 * Returns the vertex shader that passes the tessellation factor of an adaptive patch.
 *
 * @return The GLSL source.
 */
std::string GetTessellationAdaptiveVertexShaderGLSL();

/**
 * Returns the tessellation control shader.
 *
 * @param host_vertex_shader_type The host vertex shader type of the patch.
 * @param tessellation_mode The tessellation mode of the draw.
 * @param input_control_points Control points the shader reads per patch.
 * @param output_control_points Control points the shader writes per patch.
 * @return The GLSL source.
 */
std::string GetTessellationControlShaderGLSL(Shader::HostVertexShaderType host_vertex_shader_type,
                                             xenos::TessellationMode tessellation_mode,
                                             u32 input_control_points, u32 output_control_points);

}  // namespace rex::gpu::vulkan
