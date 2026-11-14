/**
 * @file        tests/unit/gpu/tessellation_glsl_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <format>
#include <string>

#include "gpu/xenos/pipeline/shader/spirv_translator.h"
#include "gpu/xenos/vulkan/tessellation_glsl.h"

using rex::gpu::Shader;
using rex::gpu::SPIRVShaderTranslator;
using namespace rex::gpu::vulkan;

namespace {
std::string ConstantsLayout() {
  return std::format("layout(set = {}, binding = {}, std140) uniform XeSystemConstants",
                     static_cast<i32>(SPIRVShaderTranslator::kDescriptorSetConstants),
                     static_cast<i32>(SPIRVShaderTranslator::kConstantBufferSystem));
}
}  // namespace

REX_TEST_CASE("the tessellation system constants block binds the constants descriptor set",
              "[gpu][tessellation]") {
  CHECK(GetTessellationSystemConstantsBlockGLSL().find(ConstantsLayout()) != std::string::npos);
}

REX_TEST_CASE("every tessellation helper shader declares the system constants block",
              "[gpu][tessellation]") {
  const std::string block = GetTessellationSystemConstantsBlockGLSL();
  CHECK(GetTessellationIndexedVertexShaderGLSL().find(block) != std::string::npos);
  CHECK(GetTessellationAdaptiveVertexShaderGLSL().find(block) != std::string::npos);
  CHECK(GetTessellationControlShaderGLSL(Shader::HostVertexShaderType::kTriangleDomainCPIndexed,
                                         rex::xenos::TessellationMode::kAdaptive, 3, 3)
            .find(block) != std::string::npos);
}
