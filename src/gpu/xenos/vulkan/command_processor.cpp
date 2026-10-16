/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ShaderLang.h>
#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include "gpu/xenos/draw_state.h"
#include "gpu/xenos/pipeline/shader/shader.h"
#include "gpu/xenos/pipeline/shader/spirv_translator.h"
#include <rex/xenos/registers.h>
#include "gpu/xenos/vulkan/command_processor.h"
#include "gpu/xenos/vulkan/pipeline_cache.h"
#include "gpu/xenos/vulkan/render_target_cache.h"
#include "gpu/xenos/vulkan/shader.h"
#include "gpu/xenos/vulkan/shared_memory.h"
#include <rex/xenos/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/types.h>
#include <rex/memory/byte_swap.h>
#include "gpu/xenos/host/vulkan/presenter.h"
#include "gpu/xenos/host/vulkan/objects.h"

#include "gpu/xenos/cvar_table.h"

namespace rex::gpu::vulkan {

namespace {

constexpr TBuiltInResource kGlslangDefaultTBuiltInResource = {
    /* .maxLights = */ 32,
    /* .maxClipPlanes = */ 6,
    /* .maxTextureUnits = */ 32,
    /* .maxTextureCoords = */ 32,
    /* .maxVertexAttribs = */ 64,
    /* .maxVertexUniformComponents = */ 4096,
    /* .maxVaryingFloats = */ 64,
    /* .maxVertexTextureImageUnits = */ 32,
    /* .maxCombinedTextureImageUnits = */ 80,
    /* .maxTextureImageUnits = */ 32,
    /* .maxFragmentUniformComponents = */ 4096,
    /* .maxDrawBuffers = */ 32,
    /* .maxVertexUniformVectors = */ 128,
    /* .maxVaryingVectors = */ 8,
    /* .maxFragmentUniformVectors = */ 16,
    /* .maxVertexOutputVectors = */ 16,
    /* .maxFragmentInputVectors = */ 15,
    /* .minProgramTexelOffset = */ -8,
    /* .maxProgramTexelOffset = */ 7,
    /* .maxClipDistances = */ 8,
    /* .maxComputeWorkGroupCountX = */ 65535,
    /* .maxComputeWorkGroupCountY = */ 65535,
    /* .maxComputeWorkGroupCountZ = */ 65535,
    /* .maxComputeWorkGroupSizeX = */ 1024,
    /* .maxComputeWorkGroupSizeY = */ 1024,
    /* .maxComputeWorkGroupSizeZ = */ 64,
    /* .maxComputeUniformComponents = */ 1024,
    /* .maxComputeTextureImageUnits = */ 16,
    /* .maxComputeImageUniforms = */ 8,
    /* .maxComputeAtomicCounters = */ 8,
    /* .maxComputeAtomicCounterBuffers = */ 1,
    /* .maxVaryingComponents = */ 60,
    /* .maxVertexOutputComponents = */ 64,
    /* .maxGeometryInputComponents = */ 64,
    /* .maxGeometryOutputComponents = */ 128,
    /* .maxFragmentInputComponents = */ 128,
    /* .maxImageUnits = */ 8,
    /* .maxCombinedImageUnitsAndFragmentOutputs = */ 8,
    /* .maxCombinedShaderOutputResources = */ 8,
    /* .maxImageSamples = */ 0,
    /* .maxVertexImageUniforms = */ 0,
    /* .maxTessControlImageUniforms = */ 0,
    /* .maxTessEvaluationImageUniforms = */ 0,
    /* .maxGeometryImageUniforms = */ 0,
    /* .maxFragmentImageUniforms = */ 8,
    /* .maxCombinedImageUniforms = */ 8,
    /* .maxGeometryTextureImageUnits = */ 16,
    /* .maxGeometryOutputVertices = */ 256,
    /* .maxGeometryTotalOutputComponents = */ 1024,
    /* .maxGeometryUniformComponents = */ 1024,
    /* .maxGeometryVaryingComponents = */ 64,
    /* .maxTessControlInputComponents = */ 128,
    /* .maxTessControlOutputComponents = */ 128,
    /* .maxTessControlTextureImageUnits = */ 16,
    /* .maxTessControlUniformComponents = */ 1024,
    /* .maxTessControlTotalOutputComponents = */ 4096,
    /* .maxTessEvaluationInputComponents = */ 128,
    /* .maxTessEvaluationOutputComponents = */ 128,
    /* .maxTessEvaluationTextureImageUnits = */ 16,
    /* .maxTessEvaluationUniformComponents = */ 1024,
    /* .maxTessPatchComponents = */ 120,
    /* .maxPatchVertices = */ 32,
    /* .maxTessGenLevel = */ 64,
    /* .maxViewports = */ 16,
    /* .maxVertexAtomicCounters = */ 0,
    /* .maxTessControlAtomicCounters = */ 0,
    /* .maxTessEvaluationAtomicCounters = */ 0,
    /* .maxGeometryAtomicCounters = */ 0,
    /* .maxFragmentAtomicCounters = */ 8,
    /* .maxCombinedAtomicCounters = */ 8,
    /* .maxAtomicCounterBindings = */ 1,
    /* .maxVertexAtomicCounterBuffers = */ 0,
    /* .maxTessControlAtomicCounterBuffers = */ 0,
    /* .maxTessEvaluationAtomicCounterBuffers = */ 0,
    /* .maxGeometryAtomicCounterBuffers = */ 0,
    /* .maxFragmentAtomicCounterBuffers = */ 1,
    /* .maxCombinedAtomicCounterBuffers = */ 1,
    /* .maxAtomicCounterBufferSize = */ 16384,
    /* .maxTransformFeedbackBuffers = */ 4,
    /* .maxTransformFeedbackInterleavedComponents = */ 64,
    /* .maxCullDistances = */ 8,
    /* .maxCombinedClipAndCullDistances = */ 8,
    /* .maxSamples = */ 4,
    /* .maxMeshOutputVerticesNV = */ 256,
    /* .maxMeshOutputPrimitivesNV = */ 512,
    /* .maxMeshWorkGroupSizeX_NV = */ 32,
    /* .maxMeshWorkGroupSizeY_NV = */ 1,
    /* .maxMeshWorkGroupSizeZ_NV = */ 1,
    /* .maxTaskWorkGroupSizeX_NV = */ 32,
    /* .maxTaskWorkGroupSizeY_NV = */ 1,
    /* .maxTaskWorkGroupSizeZ_NV = */ 1,
    /* .maxMeshViewCountNV = */ 4,
    /* .maxDualSourceDrawBuffersEXT = */ 1,
    /* .limits = */
    {
        /* .nonInductiveForLoops = */ 1,
        /* .whileLoops = */ 1,
        /* .doWhileLoops = */ 1,
        /* .generalUniformIndexing = */ 1,
        /* .generalAttributeMatrixVectorIndexing = */ 1,
        /* .generalVaryingIndexing = */ 1,
        /* .generalSamplerIndexing = */ 1,
        /* .generalVariableIndexing = */ 1,
        /* .generalConstantMatrixVectorIndexing = */ 1,
    },
};

const char* GetSwapFxaaComputeSource(bool extreme_quality) {
  return extreme_quality ? R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_fxaa_size;
  vec2 xe_fxaa_size_inv;
};

layout(set = 0, binding = 0) uniform sampler2D xe_fxaa_source;
layout(set = 1, binding = 0, rgb10_a2) writeonly uniform image2D xe_fxaa_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);
const float kEdgeThreshold = 0.063;
const float kEdgeThresholdMin = 0.0312;
const float kSpanMax = 12.0;
const float kDirReduceMul = 0.125;
const float kDirReduceMin = 1.0 / 128.0;

float SampleLuma(vec2 uv) {
  return textureLod(xe_fxaa_source, uv, 0.0).a;
}

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, xe_fxaa_size))) {
    return;
  }

  vec2 uv = (vec2(pixel) + vec2(0.5)) * xe_fxaa_size_inv;
  vec2 texel = xe_fxaa_size_inv;
  vec4 rgbm = textureLod(xe_fxaa_source, uv, 0.0);

  float lumaM = rgbm.a;
  float lumaNW = SampleLuma(uv + vec2(-texel.x, -texel.y));
  float lumaNE = SampleLuma(uv + vec2( texel.x, -texel.y));
  float lumaSW = SampleLuma(uv + vec2(-texel.x,  texel.y));
  float lumaSE = SampleLuma(uv + vec2( texel.x,  texel.y));

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  float lumaRange = lumaMax - lumaMin;

  vec3 result = rgbm.rgb;
  if (lumaRange >= max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max(
        (lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kDirReduceMul),
        kDirReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * texel;

    vec3 rgbA =
        0.5 *
        (textureLod(xe_fxaa_source, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
         textureLod(xe_fxaa_source, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 rgbB =
        rgbA * 0.5 +
        0.25 *
            (textureLod(xe_fxaa_source, uv + dir * -0.5, 0.0).rgb +
             textureLod(xe_fxaa_source, uv + dir * 0.5, 0.0).rgb);
    float lumaB = dot(rgbB, kLumaWeights);
    result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  }

  imageStore(xe_fxaa_dest, ivec2(pixel), vec4(result, 1.0));
}
)"
                         : R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_fxaa_size;
  vec2 xe_fxaa_size_inv;
};

layout(set = 0, binding = 0) uniform sampler2D xe_fxaa_source;
layout(set = 1, binding = 0, rgb10_a2) writeonly uniform image2D xe_fxaa_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);
const float kEdgeThreshold = 0.166;
const float kEdgeThresholdMin = 0.0833;
const float kSpanMax = 8.0;
const float kDirReduceMul = 0.125;
const float kDirReduceMin = 1.0 / 128.0;

float SampleLuma(vec2 uv) {
  return textureLod(xe_fxaa_source, uv, 0.0).a;
}

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, xe_fxaa_size))) {
    return;
  }

  vec2 uv = (vec2(pixel) + vec2(0.5)) * xe_fxaa_size_inv;
  vec2 texel = xe_fxaa_size_inv;
  vec4 rgbm = textureLod(xe_fxaa_source, uv, 0.0);

  float lumaM = rgbm.a;
  float lumaNW = SampleLuma(uv + vec2(-texel.x, -texel.y));
  float lumaNE = SampleLuma(uv + vec2( texel.x, -texel.y));
  float lumaSW = SampleLuma(uv + vec2(-texel.x,  texel.y));
  float lumaSE = SampleLuma(uv + vec2( texel.x,  texel.y));

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  float lumaRange = lumaMax - lumaMin;

  vec3 result = rgbm.rgb;
  if (lumaRange >= max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max(
        (lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kDirReduceMul),
        kDirReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * texel;

    vec3 rgbA =
        0.5 *
        (textureLod(xe_fxaa_source, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
         textureLod(xe_fxaa_source, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 rgbB =
        rgbA * 0.5 +
        0.25 *
            (textureLod(xe_fxaa_source, uv + dir * -0.5, 0.0).rgb +
             textureLod(xe_fxaa_source, uv + dir * 0.5, 0.0).rgb);
    float lumaB = dot(rgbB, kLumaWeights);
    result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  }

  imageStore(xe_fxaa_dest, ivec2(pixel), vec4(result, 1.0));
}
)";
}

const char* GetSwapApplyGammaTablePixelRbSwapSource() {
  return R"(#version 450
layout(set = 0, binding = 0) uniform textureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;

layout(location = 0) out vec4 xe_apply_gamma_color;

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 255.0 + vec3(0.5)).bgr;
  xe_apply_gamma_color = vec4(
      texelFetch(xe_apply_gamma_ramp, int(source.r)).b,
      texelFetch(xe_apply_gamma_ramp, int(source.g)).g,
      texelFetch(xe_apply_gamma_ramp, int(source.b)).r,
      1.0);
}
)";
}

const char* GetSwapApplyGammaPwlPixelRbSwapSource() {
  return R"(#version 450
layout(set = 0, binding = 0) uniform utextureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;

layout(location = 0) out vec4 xe_apply_gamma_color;

float ApplyPwl(uint source_value_10b, uint channel) {
  uint source_value_base = source_value_10b >> 3u;
  uvec4 pwl = texelFetch(xe_apply_gamma_ramp, int(source_value_base * 3u + channel));
  float value = float(pwl.x) + float((source_value_10b & 7u) * pwl.y) * 0.125;
  return clamp(value * 1.52737048e-05, 0.0, 1.0);
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 1023.0 + vec3(0.5)).bgr;
  xe_apply_gamma_color = vec4(
      ApplyPwl(source.r, 0u),
      ApplyPwl(source.g, 1u),
      ApplyPwl(source.b, 2u),
      1.0);
}
)";
}

const char* GetSwapApplyGammaTableComputeRbSwapSource() {
  return R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_apply_gamma_size;
};

layout(set = 0, binding = 0) uniform textureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;
layout(set = 2, binding = 0, rgb10_a2) writeonly uniform image2D xe_apply_gamma_dest;

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), xe_apply_gamma_size))) {
    return;
  }

  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 255.0 + vec3(0.5)).bgr;
  imageStore(
      xe_apply_gamma_dest, pixel,
      vec4(texelFetch(xe_apply_gamma_ramp, int(source.r)).b,
           texelFetch(xe_apply_gamma_ramp, int(source.g)).g,
           texelFetch(xe_apply_gamma_ramp, int(source.b)).r, 1.0));
}
)";
}

const char* GetSwapApplyGammaPwlComputeRbSwapSource() {
  return R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_apply_gamma_size;
};

layout(set = 0, binding = 0) uniform utextureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;
layout(set = 2, binding = 0, rgb10_a2) writeonly uniform image2D xe_apply_gamma_dest;

float ApplyPwl(uint source_value_10b, uint channel) {
  uint source_value_base = source_value_10b >> 3u;
  uvec4 pwl = texelFetch(xe_apply_gamma_ramp, int(source_value_base * 3u + channel));
  float value = float(pwl.x) + float((source_value_10b & 7u) * pwl.y) * 0.125;
  return clamp(value * 1.52737048e-05, 0.0, 1.0);
}

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), xe_apply_gamma_size))) {
    return;
  }

  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 1023.0 + vec3(0.5)).bgr;
  imageStore(xe_apply_gamma_dest, pixel,
             vec4(ApplyPwl(source.r, 0u), ApplyPwl(source.g, 1u),
                  ApplyPwl(source.b, 2u), 1.0));
}
)";
}

const char* GetSwapApplyGammaTableFxaaLumaComputeRbSwapSource() {
  return R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_apply_gamma_size;
};

layout(set = 0, binding = 0) uniform textureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;
layout(set = 2, binding = 0, rgba16f) writeonly uniform image2D xe_apply_gamma_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), xe_apply_gamma_size))) {
    return;
  }

  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 255.0 + vec3(0.5)).bgr;
  vec3 rgb = vec3(texelFetch(xe_apply_gamma_ramp, int(source.r)).b,
                  texelFetch(xe_apply_gamma_ramp, int(source.g)).g,
                  texelFetch(xe_apply_gamma_ramp, int(source.b)).r);
  imageStore(xe_apply_gamma_dest, pixel, vec4(rgb, dot(rgb, kLumaWeights)));
}
)";
}

const char* GetSwapApplyGammaPwlFxaaLumaComputeRbSwapSource() {
  return R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_apply_gamma_size;
};

layout(set = 0, binding = 0) uniform utextureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;
layout(set = 2, binding = 0, rgba16f) writeonly uniform image2D xe_apply_gamma_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

float ApplyPwl(uint source_value_10b, uint channel) {
  uint source_value_base = source_value_10b >> 3u;
  uvec4 pwl = texelFetch(xe_apply_gamma_ramp, int(source_value_base * 3u + channel));
  float value = float(pwl.x) + float((source_value_10b & 7u) * pwl.y) * 0.125;
  return clamp(value * 1.52737048e-05, 0.0, 1.0);
}

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), xe_apply_gamma_size))) {
    return;
  }

  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 1023.0 + vec3(0.5)).bgr;
  vec3 rgb = vec3(ApplyPwl(source.r, 0u), ApplyPwl(source.g, 1u),
                  ApplyPwl(source.b, 2u));
  imageStore(xe_apply_gamma_dest, pixel, vec4(rgb, dot(rgb, kLumaWeights)));
}
)";
}

bool CompileGLSLToSPIRVInternal(EShLanguage stage, std::string_view source,
                                std::vector<u32>& spirv_out, std::string& error_out) {
  static std::once_flag glslang_initialize_once;
  std::call_once(glslang_initialize_once, []() { glslang::InitializeProcess(); });

  const char* source_c_str = source.data();
  glslang::TShader shader(stage);
  shader.setStrings(&source_c_str, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

  EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&kGlslangDefaultTBuiltInResource, 450, false, messages)) {
    error_out = "glslang shader parse failed";
    if (const char* shader_log = shader.getInfoLog();
        shader_log != nullptr && shader_log[0] != '\0') {
      error_out += ": ";
      error_out += shader_log;
    }
    return false;
  }

  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    error_out = "glslang program link failed";
    if (const char* program_log = program.getInfoLog();
        program_log != nullptr && program_log[0] != '\0') {
      error_out += ": ";
      error_out += program_log;
    }
    return false;
  }

  const glslang::TIntermediate* intermediate = program.getIntermediate(stage);
  if (intermediate == nullptr) {
    error_out = "glslang produced no stage intermediate";
    return false;
  }

  glslang::SpvOptions spv_options = {};
  spv_options.disableOptimizer = true;
  spv_options.optimizeSize = false;
  glslang::GlslangToSpv(*intermediate, spirv_out, &spv_options);
  if (spirv_out.empty()) {
    error_out = "glslang produced empty SPIR-V";
    return false;
  }
  return true;
}

}  // namespace

namespace shaders {
#include "../shaders/vulkan_spirv/apply_gamma_pwl_cs.h"
#include "../shaders/vulkan_spirv/apply_gamma_pwl_fxaa_luma_ps.h"
#include "../shaders/vulkan_spirv/apply_gamma_pwl_fxaa_luma_cs.h"
#include "../shaders/vulkan_spirv/apply_gamma_pwl_ps.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_cs.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_fxaa_luma_ps.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_fxaa_luma_cs.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_ps.h"
#include "../shaders/vulkan_spirv/fullscreen_cw_vs.h"
#include "../shaders/vulkan_spirv/resolve_downscale_cs.h"
}  // namespace shaders

const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeUniformBuffer = {
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    SPIRVShaderTranslator::kConstantBufferCount* kLinkedTypeDescriptorPoolSetCount};

const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeStorageBuffer = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kLinkedTypeDescriptorPoolSetCount};

// 2x descriptors for texture images because of unsigned and signed bindings.
const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeTextures[2] = {
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kLinkedTypeDescriptorPoolSetCount},
    {VK_DESCRIPTOR_TYPE_SAMPLER, kLinkedTypeDescriptorPoolSetCount},
};

VulkanCommandProcessor::VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system,
                                               Runtime& runtime)
    : CommandProcessor(graphics_system, runtime),
      deferred_command_buffer_(*this),
      transient_descriptor_allocator_uniform_buffer_(
          static_cast<const gpu::vulkan::VulkanProvider*>(graphics_system->GetProvider())
              ->GetVulkanDevice(),
          &kDescriptorPoolSizeUniformBuffer, 1, kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_storage_buffer_(
          static_cast<const gpu::vulkan::VulkanProvider*>(graphics_system->GetProvider())
              ->GetVulkanDevice(),
          &kDescriptorPoolSizeStorageBuffer, 1, kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_textures_(
          static_cast<const gpu::vulkan::VulkanProvider*>(graphics_system->GetProvider())
              ->GetVulkanDevice(),
          kDescriptorPoolSizeTextures, static_cast<u32>(rex::CountOf(kDescriptorPoolSizeTextures)),
          kLinkedTypeDescriptorPoolSetCount) {
  legacy_readback_memexport_cvar_ = &::rex::gpu::cvars::vulkan_readback_memexport;
}

VulkanCommandProcessor::~VulkanCommandProcessor() = default;

void VulkanCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  InvalidateAllVertexBufferResidency();
  cache_clear_requested_ = true;
}

void VulkanCommandProcessor::InvalidateGPUMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void VulkanCommandProcessor::InvalidateAllVertexBufferResidency() {
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
}

void VulkanCommandProcessor::InvalidateVertexBufferResidency(u32 vfetch_index) {
  if (vfetch_index >= kVertexFetchCount) {
    return;
  }
  vertex_buffers_in_sync_[vfetch_index >> 6] &= ~(static_cast<u64>(1) << (vfetch_index & 63));
}

void VulkanCommandProcessor::InvalidateVertexBufferResidencyRange(u32 first_vfetch,
                                                                  u32 last_vfetch) {
  if (first_vfetch > last_vfetch) {
    std::swap(first_vfetch, last_vfetch);
  }
  if (first_vfetch >= kVertexFetchCount) {
    return;
  }
  last_vfetch = std::min(last_vfetch, static_cast<u32>(kVertexFetchCount - 1));
  for (u32 vfetch_index = first_vfetch; vfetch_index <= last_vfetch; ++vfetch_index) {
    InvalidateVertexBufferResidency(vfetch_index);
  }
}

void VulkanCommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                     u32 title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
}

bool VulkanCommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                                u32 packet, u32 count) {
  if (!REXCVAR_GET(::rex::gpu::cvars::occlusion_query_enable) ||
      !occlusion_query_resources_available_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }

  constexpr u32 kQueryFinished = std::byteswap(0xFFFFFEEDu);
  REX_ASSERT(count == 1);
  u32 initiator = reader->ReadAndSwap<u32>();
  WriteRegister(std::to_underlying(xenos::Register::VGT_EVENT_INITIATOR), initiator & 0x3F);

  u32 sample_count_addr =
      register_file_->values[std::to_underlying(xenos::Register::RB_SAMPLE_COUNT_ADDR)];
  auto* sample_counts = memory_->TranslatePhysical<xenos::DepthSampleCounts*>(sample_count_addr);
  if (!sample_counts) {
    DisableHostOcclusionQueries();
    return true;
  }

  auto write_fallback_result = [sample_counts]() -> bool {
    auto fake_sample_count = REXCVAR_GET(::rex::gpu::cvars::query_occlusion_fake_sample_count);
    if (fake_sample_count < 0) {
      return true;
    }
    bool is_end_via_z_pass =
        sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
    bool is_end_via_z_fail =
        sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
    WriteSampleCounts(*sample_counts, is_end_via_z_pass || is_end_via_z_fail
                                          ? static_cast<u32>(fake_sample_count)
                                          : 0);
    return true;
  };

  bool is_end_via_z_pass =
      sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
  bool is_end_via_z_fail =
      sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
  bool is_end = is_end_via_z_pass || is_end_via_z_fail;

  if (!is_end) {
    if (active_occlusion_query_.valid &&
        active_occlusion_query_.sample_count_address != sample_count_addr) {
      DisableHostOcclusionQueries();
      return write_fallback_result();
    }
    if (!BeginGuestOcclusionQuery(sample_count_addr)) {
      return write_fallback_result();
    }
    return true;
  }

  if (!active_occlusion_query_.valid ||
      active_occlusion_query_.sample_count_address != sample_count_addr) {
    DisableHostOcclusionQueries();
    return write_fallback_result();
  }

  if (!EndGuestOcclusionQuery(sample_count_addr)) {
    return write_fallback_result();
  }
  return true;
}

bool VulkanCommandProcessor::CompileGLSLToSPIRV(VkShaderStageFlagBits stage,
                                                std::string_view source,
                                                std::vector<u32>& spirv_out,
                                                std::string& error_out) const {
  EShLanguage glslang_stage;
  switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT:
      glslang_stage = EShLangVertex;
      break;
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
      glslang_stage = EShLangTessControl;
      break;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
      glslang_stage = EShLangTessEvaluation;
      break;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
      glslang_stage = EShLangGeometry;
      break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
      glslang_stage = EShLangFragment;
      break;
    case VK_SHADER_STAGE_COMPUTE_BIT:
      glslang_stage = EShLangCompute;
      break;
    default:
      error_out = std::format("Unsupported Vulkan shader stage mask {}", static_cast<u32>(stage));
      return false;
  }
  return CompileGLSLToSPIRVInternal(glslang_stage, source, spirv_out, error_out);
}

bool VulkanCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    REX_FAIL(gpu, false, "Failed to initialize base command processor context");
  }
  InvalidateAllVertexBufferResidency();

  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  const gpu::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->GetProperties();

  // The vertex stage stays for manual index or factor fetch without fullDrawIndexUint32.
  guest_shader_pipeline_stages_ =
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  guest_shader_vertex_stages_ = VK_SHADER_STAGE_VERTEX_BIT;
  if (device_properties.tessellationShader) {
    guest_shader_pipeline_stages_ |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                                     VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    guest_shader_vertex_stages_ |=
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }
  if (!device_properties.vertexPipelineStoresAndAtomics) {
    // For memory export from vertex shaders converted to compute shaders.
    guest_shader_pipeline_stages_ |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    guest_shader_vertex_stages_ |= VK_SHADER_STAGE_COMPUTE_BIT;
  }

  // 16384 is the minimum maxUniformBufferRange and exceeds any single uniform buffer needed.
  uniform_buffer_pool_ = std::make_unique<gpu::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      rex::Align(
          std::max(gpu::GraphicsUploadBufferPool::kDefaultPageSize, static_cast<size_t>(16384)),
          static_cast<size_t>(device_properties.minUniformBufferOffsetAlignment)));

  VkShaderStageFlags guest_shader_stages =
      guest_shader_vertex_stages_ | VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 0;
  descriptor_set_layout_create_info.pBindings = nullptr;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_empty_) != VK_SUCCESS) {
    REX_FAIL(gpu, false, "Failed to create an empty Vulkan descriptor set layout");
  }
  VkDescriptorSetLayoutBinding
      descriptor_set_layout_bindings_constants[SPIRVShaderTranslator::kConstantBufferCount] = {};
  for (u32 i = 0; i < SPIRVShaderTranslator::kConstantBufferCount; ++i) {
    VkDescriptorSetLayoutBinding& constants_binding = descriptor_set_layout_bindings_constants[i];
    constants_binding.binding = i;
    constants_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constants_binding.descriptorCount = 1;
    constants_binding.pImmutableSamplers = nullptr;
  }
  descriptor_set_layout_bindings_constants[SPIRVShaderTranslator::kConstantBufferSystem]
      .stageFlags =
      guest_shader_stages |
      (device_properties.tessellationShader ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : 0) |
      (device_properties.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
  descriptor_set_layout_bindings_constants[SPIRVShaderTranslator::kConstantBufferFloatVertex]
      .stageFlags = guest_shader_vertex_stages_;
  descriptor_set_layout_bindings_constants[SPIRVShaderTranslator::kConstantBufferFloatPixel]
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  descriptor_set_layout_bindings_constants[SPIRVShaderTranslator::kConstantBufferBoolLoop]
      .stageFlags = guest_shader_stages;
  descriptor_set_layout_bindings_constants[SPIRVShaderTranslator::kConstantBufferFetch].stageFlags =
      guest_shader_stages;
  descriptor_set_layout_create_info.bindingCount =
      static_cast<u32>(rex::CountOf(descriptor_set_layout_bindings_constants));
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings_constants;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_constants_) != VK_SUCCESS) {
    REX_FAIL(gpu, false,
             "Failed to create a Vulkan descriptor set layout for guest draw "
             "constant buffers");
  }
  VkDescriptorSetLayoutBinding descriptor_set_layout_binding_transient;
  descriptor_set_layout_binding_transient.binding = 0;
  descriptor_set_layout_binding_transient.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_binding_transient.descriptorCount = 1;
  descriptor_set_layout_binding_transient.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_binding_transient.pImmutableSamplers = nullptr;
  descriptor_set_layout_create_info.bindingCount = 1;
  descriptor_set_layout_create_info.pBindings = &descriptor_set_layout_binding_transient;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[static_cast<size_t>(
              SingleTransientDescriptorLayout::kStorageBufferCompute)]) != VK_SUCCESS) {
    REX_FAIL(gpu, false,
             "Failed to create a Vulkan descriptor set layout for a storage buffer "
             "bound to the compute shader");
  }
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings_transient_pair[2];
  descriptor_set_layout_bindings_transient_pair[0] = descriptor_set_layout_binding_transient;
  descriptor_set_layout_bindings_transient_pair[1] = descriptor_set_layout_binding_transient;
  descriptor_set_layout_bindings_transient_pair[1].binding = 1;
  descriptor_set_layout_create_info.bindingCount = 2;
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings_transient_pair;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[static_cast<size_t>(
              SingleTransientDescriptorLayout::kStorageBufferPairCompute)]) != VK_SUCCESS) {
    REX_FAIL(gpu, false,
             "Failed to create a Vulkan descriptor set layout for two storage "
             "buffers bound to the compute shader");
  }

  shared_memory_ =
      std::make_unique<VulkanSharedMemory>(*this, *memory_, guest_shader_pipeline_stages_);
  if (!shared_memory_->Initialize()) {
    REX_FAIL(gpu, false, "Failed to initialize shared memory");
  }

  primitive_processor_ =
      std::make_unique<VulkanPrimitiveProcessor>(*register_file_, *memory_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REX_FAIL(gpu, false, "Failed to initialize the geometric primitive processor");
  }

  u32 shared_memory_binding_count_log2 =
      SPIRVShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
          device_properties.maxStorageBufferRange);
  u32 shared_memory_binding_count = UINT32_C(1) << shared_memory_binding_count_log2;

  u32 draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x, draw_resolution_scale_y);
  if (!draw_resolution_scale_not_clamped) {
    REXLOG_WARN(gpu,
                "The requested draw resolution scale is not supported by the "
                "emulator, reducing to {}x{}",
                draw_resolution_scale_x, draw_resolution_scale_y);
  }
  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    REXLOG_WARN(gpu,
                "Vulkan draw resolution scaling is experimental and may not affect all "
                "titles correctly");
  }
  if (!device_properties.fragmentStoresAndAtomics) {
    REX_FAIL(gpu, false,
             "Vulkan fragmentStoresAndAtomics is required for GPU emulation and "
             "D3D12 parity, but unsupported by the selected device");
  }
  if (!device_properties.vertexPipelineStoresAndAtomics) {
    REX_FAIL(gpu, false,
             "Vulkan vertexPipelineStoresAndAtomics is required for GPU emulation and "
             "D3D12 parity, but unsupported by the selected device");
  }
  if (!device_properties.geometryShader) {
    if (vulkan_device->RequiresGeometryShader()) {
      REX_FAIL(gpu, false,
               "Vulkan geometryShader is required for GPU emulation "
               "(vulkan.require_geometry_shader=true), but unsupported by the "
               "selected device");
    }
    REXLOG_WARN(gpu,
                "Vulkan geometryShader is not supported by the device; primitive "
                "fallback conversion/expansion paths will be used");
  }
  if (!device_properties.fillModeNonSolid) {
    if (vulkan_device->RequiresFillModeNonSolid()) {
      REX_FAIL(gpu, false,
               "Vulkan fillModeNonSolid is required for GPU emulation "
               "(vulkan.require_fill_mode_non_solid=true), but unsupported by the "
               "selected device");
    }
    REXLOG_WARN(gpu,
                "Vulkan fillModeNonSolid is not supported by the device; line/point "
                "polygon modes will fall back to solid fill");
  }

  // Requires the transient descriptor set layouts.
  render_target_cache_ = std::make_unique<VulkanRenderTargetCache>(
      *register_file_, *memory_, draw_resolution_scale_x, draw_resolution_scale_y, *this);
  if (!render_target_cache_->Initialize(shared_memory_binding_count)) {
    REX_FAIL(gpu, false, "Failed to initialize the render target cache");
  }

  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  VkDescriptorSetLayoutBinding shared_memory_and_edram_descriptor_set_layout_bindings[2];
  shared_memory_and_edram_descriptor_set_layout_bindings[0].binding = 0;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorCount =
      shared_memory_binding_count;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].stageFlags = guest_shader_stages;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo shared_memory_and_edram_descriptor_set_layout_create_info;
  shared_memory_and_edram_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  shared_memory_and_edram_descriptor_set_layout_create_info.pNext = nullptr;
  shared_memory_and_edram_descriptor_set_layout_create_info.flags = 0;
  shared_memory_and_edram_descriptor_set_layout_create_info.pBindings =
      shared_memory_and_edram_descriptor_set_layout_bindings;
  if (edram_fragment_shader_interlock) {
    shared_memory_and_edram_descriptor_set_layout_bindings[1].binding = 1;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].descriptorCount = 1;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].pImmutableSamplers = nullptr;
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 2;
  } else {
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 1;
  }
  if (dfn.vkCreateDescriptorSetLayout(
          device, &shared_memory_and_edram_descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_shared_memory_and_edram_) != VK_SUCCESS) {
    REX_FAIL(gpu, false,
             "Failed to create a Vulkan descriptor set layout for the shared memory "
             "and the EDRAM");
  }

  pipeline_cache_ = std::make_unique<VulkanPipelineCache>(
      *this, *register_file_, *render_target_cache_, guest_shader_vertex_stages_);
  if (!pipeline_cache_->Initialize()) {
    REX_FAIL(gpu, false, "Failed to initialize the graphics pipeline cache");
  }

  // Requires the transient descriptor set layouts.
  texture_cache_ =
      VulkanTextureCache::Create(*register_file_, *shared_memory_, draw_resolution_scale_x,
                                 draw_resolution_scale_y, *this, guest_shader_pipeline_stages_);
  if (!texture_cache_) {
    REX_FAIL(gpu, false, "Failed to initialize the texture cache");
  }

  VkDescriptorPoolSize descriptor_pool_sizes[1];
  descriptor_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_pool_sizes[0].descriptorCount =
      shared_memory_binding_count + static_cast<u32>(edram_fragment_shader_interlock);
  VkDescriptorPoolCreateInfo descriptor_pool_create_info;
  descriptor_pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptor_pool_create_info.pNext = nullptr;
  descriptor_pool_create_info.flags = 0;
  descriptor_pool_create_info.maxSets = 1;
  descriptor_pool_create_info.poolSizeCount = 1;
  descriptor_pool_create_info.pPoolSizes = descriptor_pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &descriptor_pool_create_info, nullptr,
                                 &shared_memory_and_edram_descriptor_pool_) != VK_SUCCESS) {
    REX_FAIL(gpu, false,
             "Failed to create the Vulkan descriptor pool for shared memory and "
             "EDRAM");
  }
  VkDescriptorSetAllocateInfo descriptor_set_allocate_info;
  descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  descriptor_set_allocate_info.pNext = nullptr;
  descriptor_set_allocate_info.descriptorPool = shared_memory_and_edram_descriptor_pool_;
  descriptor_set_allocate_info.descriptorSetCount = 1;
  descriptor_set_allocate_info.pSetLayouts = &descriptor_set_layout_shared_memory_and_edram_;
  if (dfn.vkAllocateDescriptorSets(device, &descriptor_set_allocate_info,
                                   &shared_memory_and_edram_descriptor_set_) != VK_SUCCESS) {
    REX_FAIL(gpu, false,
             "Failed to allocate the Vulkan descriptor set for shared memory and "
             "EDRAM");
  }
  VkDescriptorBufferInfo
      shared_memory_descriptor_buffers_info[SharedMemory::kBufferSize / (128 << 20)];
  VkDeviceSize shared_memory_binding_range =
      SharedMemory::kBufferSize >> shared_memory_binding_count_log2;
  for (u32 i = 0; i < shared_memory_binding_count; ++i) {
    VkDescriptorBufferInfo& shared_memory_descriptor_buffer_info =
        shared_memory_descriptor_buffers_info[i];
    shared_memory_descriptor_buffer_info.buffer = shared_memory_->GetBuffer();
    shared_memory_descriptor_buffer_info.offset = shared_memory_binding_range * i;
    shared_memory_descriptor_buffer_info.range = shared_memory_binding_range;
  }
  VkWriteDescriptorSet write_descriptor_sets[2];
  VkWriteDescriptorSet& write_descriptor_set_shared_memory = write_descriptor_sets[0];
  write_descriptor_set_shared_memory.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write_descriptor_set_shared_memory.pNext = nullptr;
  write_descriptor_set_shared_memory.dstSet = shared_memory_and_edram_descriptor_set_;
  write_descriptor_set_shared_memory.dstBinding = 0;
  write_descriptor_set_shared_memory.dstArrayElement = 0;
  write_descriptor_set_shared_memory.descriptorCount = shared_memory_binding_count;
  write_descriptor_set_shared_memory.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write_descriptor_set_shared_memory.pImageInfo = nullptr;
  write_descriptor_set_shared_memory.pBufferInfo = shared_memory_descriptor_buffers_info;
  write_descriptor_set_shared_memory.pTexelBufferView = nullptr;
  VkDescriptorBufferInfo edram_descriptor_buffer_info;
  if (edram_fragment_shader_interlock) {
    edram_descriptor_buffer_info.buffer = render_target_cache_->GetEDRAMBuffer();
    edram_descriptor_buffer_info.offset = 0;
    edram_descriptor_buffer_info.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet& write_descriptor_set_edram = write_descriptor_sets[1];
    write_descriptor_set_edram.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_descriptor_set_edram.pNext = nullptr;
    write_descriptor_set_edram.dstSet = shared_memory_and_edram_descriptor_set_;
    write_descriptor_set_edram.dstBinding = 1;
    write_descriptor_set_edram.dstArrayElement = 0;
    write_descriptor_set_edram.descriptorCount = 1;
    write_descriptor_set_edram.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write_descriptor_set_edram.pImageInfo = nullptr;
    write_descriptor_set_edram.pBufferInfo = &edram_descriptor_buffer_info;
    write_descriptor_set_edram.pTexelBufferView = nullptr;
  }
  dfn.vkUpdateDescriptorSets(device, 1 + static_cast<u32>(edram_fragment_shader_interlock),
                             write_descriptor_sets, 0, nullptr);

  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
  constexpr u32 kGammaRampSize256EntryTable = sizeof(u32) * 256;
  constexpr u32 kGammaRampSizePWL = sizeof(u16) * 2 * 3 * 128;
  constexpr VkDeviceSize kGammaRampSize = kGammaRampSize256EntryTable + kGammaRampSizePWL;
  VkBufferCreateInfo gamma_ramp_host_visible_buffer_create_info;
  gamma_ramp_host_visible_buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  gamma_ramp_host_visible_buffer_create_info.pNext = nullptr;
  gamma_ramp_host_visible_buffer_create_info.flags = 0;
  gamma_ramp_host_visible_buffer_create_info.size = kGammaRampSize * kMaxFramesInFlight;
  gamma_ramp_host_visible_buffer_create_info.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
  gamma_ramp_host_visible_buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  gamma_ramp_host_visible_buffer_create_info.queueFamilyIndexCount = 0;
  gamma_ramp_host_visible_buffer_create_info.pQueueFamilyIndices = nullptr;
  if (dfn.vkCreateBuffer(device, &gamma_ramp_host_visible_buffer_create_info, nullptr,
                         &gamma_ramp_buffer_) == VK_SUCCESS) {
    bool use_gamma_ramp_host_visible_buffer = false;
    VkMemoryRequirements gamma_ramp_host_visible_buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, gamma_ramp_buffer_,
                                      &gamma_ramp_host_visible_buffer_memory_requirements);
    u32 gamma_ramp_host_visible_buffer_memory_types =
        gamma_ramp_host_visible_buffer_memory_requirements.memoryTypeBits &
        (vulkan_device->GetMemoryTypes().device_local &
         vulkan_device->GetMemoryTypes().host_visible);
    VkMemoryAllocateInfo gamma_ramp_host_visible_buffer_memory_allocate_info;
    // Prefer host uncached memory since it is write only, host cached host visible also works.
    if (rex::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types &
                ~vulkan_device->GetMemoryTypes().host_cached,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info.memoryTypeIndex)) ||
        rex::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info.memoryTypeIndex))) {
      VkMemoryAllocateInfo* gamma_ramp_host_visible_buffer_memory_allocate_info_last =
          &gamma_ramp_host_visible_buffer_memory_allocate_info;
      gamma_ramp_host_visible_buffer_memory_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      gamma_ramp_host_visible_buffer_memory_allocate_info.pNext = nullptr;
      gamma_ramp_host_visible_buffer_memory_allocate_info.allocationSize =
          gamma_ramp_host_visible_buffer_memory_requirements.size;
      VkMemoryDedicatedAllocateInfo gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
      if (vulkan_device->GetExtensions().ext_1_1_KHR_dedicated_allocation) {
        gamma_ramp_host_visible_buffer_memory_allocate_info_last->pNext =
            &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
        gamma_ramp_host_visible_buffer_memory_allocate_info_last =
            reinterpret_cast<VkMemoryAllocateInfo*>(
                &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info);
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.sType =
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.pNext = nullptr;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.buffer = gamma_ramp_buffer_;
      }
      if (dfn.vkAllocateMemory(device, &gamma_ramp_host_visible_buffer_memory_allocate_info,
                               nullptr, &gamma_ramp_buffer_memory_) == VK_SUCCESS) {
        if (dfn.vkBindBufferMemory(device, gamma_ramp_buffer_, gamma_ramp_buffer_memory_, 0) ==
            VK_SUCCESS) {
          if (dfn.vkMapMemory(device, gamma_ramp_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
                              &gamma_ramp_upload_mapping_) == VK_SUCCESS) {
            use_gamma_ramp_host_visible_buffer = true;
            gamma_ramp_upload_memory_size_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info.allocationSize;
            gamma_ramp_upload_memory_type_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info.memoryTypeIndex;
          }
        }
        if (!use_gamma_ramp_host_visible_buffer) {
          dfn.vkFreeMemory(device, gamma_ramp_buffer_memory_, nullptr);
          gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
        }
      }
    }
    if (!use_gamma_ramp_host_visible_buffer) {
      dfn.vkDestroyBuffer(device, gamma_ramp_buffer_, nullptr);
      gamma_ramp_buffer_ = VK_NULL_HANDLE;
    }
  }
  if (gamma_ramp_buffer_ == VK_NULL_HANDLE) {
    if (!gpu::vulkan::objects::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
            gpu::vulkan::objects::MemoryPurpose::kDeviceLocal, gamma_ramp_buffer_,
            gamma_ramp_buffer_memory_)) {
      REX_FAIL(gpu, false, "Failed to create the gamma ramp buffer");
    }
    if (!gpu::vulkan::objects::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize * kMaxFramesInFlight, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            gpu::vulkan::objects::MemoryPurpose::kUpload, gamma_ramp_upload_buffer_,
            gamma_ramp_upload_buffer_memory_, &gamma_ramp_upload_memory_type_,
            &gamma_ramp_upload_memory_size_)) {
      REX_FAIL(gpu, false, "Failed to create the gamma ramp upload buffer");
    }
    if (dfn.vkMapMemory(device, gamma_ramp_upload_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
                        &gamma_ramp_upload_mapping_) != VK_SUCCESS) {
      REX_FAIL(gpu, false, "Failed to map the gamma ramp upload buffer");
    }
  }

  u32 gamma_ramp_frame_count = gamma_ramp_upload_buffer_ == VK_NULL_HANDLE ? kMaxFramesInFlight : 1;
  VkBufferViewCreateInfo gamma_ramp_buffer_view_create_info;
  gamma_ramp_buffer_view_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
  gamma_ramp_buffer_view_create_info.pNext = nullptr;
  gamma_ramp_buffer_view_create_info.flags = 0;
  gamma_ramp_buffer_view_create_info.buffer = gamma_ramp_buffer_;
  gamma_ramp_buffer_view_create_info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSize256EntryTable;
  for (u32 i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset = kGammaRampSize * i;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info, nullptr,
                               &gamma_ramp_buffer_views_[size_t{i} * 2]) != VK_SUCCESS) {
      REX_FAIL(gpu, false, "Failed to create a 256-entry table gamma ramp buffer view");
    }
  }
  gamma_ramp_buffer_view_create_info.format = VK_FORMAT_R16G16_UINT;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSizePWL;
  for (u32 i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset = kGammaRampSize * i + kGammaRampSize256EntryTable;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info, nullptr,
                               &gamma_ramp_buffer_views_[i * 2 + 1]) != VK_SUCCESS) {
      REX_FAIL(gpu, false, "Failed to create a PWL gamma ramp buffer view");
    }
  }

  VkDescriptorSetLayoutBinding swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.binding = 0;
  swap_descriptor_set_layout_binding.descriptorCount = 1;
  swap_descriptor_set_layout_binding.stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  swap_descriptor_set_layout_binding.pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo swap_descriptor_set_layout_create_info;
  swap_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  swap_descriptor_set_layout_create_info.pNext = nullptr;
  swap_descriptor_set_layout_create_info.flags = 0;
  swap_descriptor_set_layout_create_info.bindingCount = 1;
  swap_descriptor_set_layout_create_info.pBindings = &swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    REX_FAIL(gpu, false,
             "Failed to create the presentation sampled image descriptor set "
             "layout");
  }
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_uniform_texel_buffer_) !=
      VK_SUCCESS) {
    REX_FAIL(gpu, false,
             "Failed to create the presentation uniform texel buffer descriptor set "
             "layout");
  }
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  swap_descriptor_set_layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_combined_image_sampler_) !=
      VK_SUCCESS) {
    REX_FAIL(gpu, false,
             "Failed to create the presentation combined image sampler descriptor "
             "set layout");
  }
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_storage_image_) != VK_SUCCESS) {
    REX_FAIL(gpu, false,
             "Failed to create the presentation storage image descriptor set "
             "layout");
  }

  std::array<VkDescriptorPoolSize, 4> swap_descriptor_pool_sizes;
  VkDescriptorPoolCreateInfo swap_descriptor_pool_create_info;
  swap_descriptor_pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  swap_descriptor_pool_create_info.pNext = nullptr;
  swap_descriptor_pool_create_info.flags = 0;
  swap_descriptor_pool_create_info.maxSets = 0;
  swap_descriptor_pool_create_info.poolSizeCount = 0;
  swap_descriptor_pool_create_info.pPoolSizes = swap_descriptor_pool_sizes.data();
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_sampled_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_sampled_image.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    swap_descriptor_pool_size_sampled_image.descriptorCount = kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_combined_image_sampler =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_combined_image_sampler.type =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    swap_descriptor_pool_size_combined_image_sampler.descriptorCount = kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_storage_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_storage_image.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    swap_descriptor_pool_size_storage_image.descriptorCount = kMaxFramesInFlight * 2;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight * 2;
  }
  u32 gamma_ramp_buffer_view_count = 2 * gamma_ramp_frame_count;
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_uniform_texel_buffer =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_uniform_texel_buffer.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    swap_descriptor_pool_size_uniform_texel_buffer.descriptorCount = gamma_ramp_buffer_view_count;
    swap_descriptor_pool_create_info.maxSets += gamma_ramp_buffer_view_count;
  }
  if (dfn.vkCreateDescriptorPool(device, &swap_descriptor_pool_create_info, nullptr,
                                 &swap_descriptor_pool_) != VK_SUCCESS) {
    REX_FAIL(gpu, false, "Failed to create the presentation descriptor pool");
  }

  VkDescriptorSetAllocateInfo swap_descriptor_set_allocate_info;
  swap_descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  swap_descriptor_set_allocate_info.pNext = nullptr;
  swap_descriptor_set_allocate_info.descriptorPool = swap_descriptor_pool_;
  swap_descriptor_set_allocate_info.descriptorSetCount = 1;
  swap_descriptor_set_allocate_info.pSetLayouts = &swap_descriptor_set_layout_uniform_texel_buffer_;
  for (u32 i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_gamma_ramp_[i]) != VK_SUCCESS) {
      REX_FAIL(gpu, false, "Failed to allocate the gamma ramp descriptor sets");
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts = &swap_descriptor_set_layout_sampled_image_;
  for (u32 i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_source_[i]) != VK_SUCCESS) {
      REX_FAIL(gpu, false, "Failed to allocate the presentation source image descriptor sets");
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_combined_image_sampler_;
  for (u32 i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_fxaa_source_[i]) != VK_SUCCESS) {
      REX_FAIL(gpu, false,
               "Failed to allocate the presentation FXAA source image descriptor "
               "sets");
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts = &swap_descriptor_set_layout_storage_image_;
  for (u32 i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_destination_storage_[i]) != VK_SUCCESS) {
      REX_FAIL(gpu, false,
               "Failed to allocate the presentation destination storage image "
               "descriptor sets");
    }
  }
  for (u32 i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_fxaa_destination_storage_[i]) !=
        VK_SUCCESS) {
      REX_FAIL(gpu, false,
               "Failed to allocate the presentation FXAA destination storage image "
               "descriptor sets");
    }
  }

  VkWriteDescriptorSet gamma_ramp_write_descriptor_set;
  gamma_ramp_write_descriptor_set.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  gamma_ramp_write_descriptor_set.pNext = nullptr;
  gamma_ramp_write_descriptor_set.dstBinding = 0;
  gamma_ramp_write_descriptor_set.dstArrayElement = 0;
  gamma_ramp_write_descriptor_set.descriptorCount = 1;
  gamma_ramp_write_descriptor_set.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  gamma_ramp_write_descriptor_set.pImageInfo = nullptr;
  gamma_ramp_write_descriptor_set.pBufferInfo = nullptr;
  for (u32 i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    gamma_ramp_write_descriptor_set.dstSet = swap_descriptors_gamma_ramp_[i];
    gamma_ramp_write_descriptor_set.pTexelBufferView = &gamma_ramp_buffer_views_[i];
    dfn.vkUpdateDescriptorSets(device, 1, &gamma_ramp_write_descriptor_set, 0, nullptr);
  }

  VkSamplerCreateInfo swap_sampler_create_info;
  swap_sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  swap_sampler_create_info.pNext = nullptr;
  swap_sampler_create_info.flags = 0;
  swap_sampler_create_info.magFilter = VK_FILTER_LINEAR;
  swap_sampler_create_info.minFilter = VK_FILTER_LINEAR;
  swap_sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  swap_sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  swap_sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  swap_sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  swap_sampler_create_info.mipLodBias = 0.0f;
  swap_sampler_create_info.anisotropyEnable = VK_FALSE;
  swap_sampler_create_info.maxAnisotropy = 1.0f;
  swap_sampler_create_info.compareEnable = VK_FALSE;
  swap_sampler_create_info.compareOp = VK_COMPARE_OP_NEVER;
  swap_sampler_create_info.minLod = 0.0f;
  swap_sampler_create_info.maxLod = 0.0f;
  swap_sampler_create_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  swap_sampler_create_info.unnormalizedCoordinates = VK_FALSE;
  if (dfn.vkCreateSampler(device, &swap_sampler_create_info, nullptr,
                          &swap_sampler_linear_clamp_) != VK_SUCCESS) {
    REX_FAIL(gpu, false, "Failed to create the presentation FXAA sampler");
  }

  std::array<VkDescriptorSetLayout, kSwapApplyGammaDescriptorSetCount>
      swap_apply_gamma_descriptor_set_layouts{};
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetRamp] =
      swap_descriptor_set_layout_uniform_texel_buffer_;
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetSource] =
      swap_descriptor_set_layout_sampled_image_;
  VkPipelineLayoutCreateInfo swap_apply_gamma_pipeline_layout_create_info;
  swap_apply_gamma_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  swap_apply_gamma_pipeline_layout_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_layout_create_info.flags = 0;
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      static_cast<u32>(swap_apply_gamma_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_apply_gamma_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 0;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges = nullptr;
  if (dfn.vkCreatePipelineLayout(device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
                                 &swap_apply_gamma_pipeline_layout_) != VK_SUCCESS) {
    REX_FAIL(gpu, false, "Failed to create the gamma ramp application pipeline layout");
  }

  std::array<VkDescriptorSetLayout, kSwapApplyGammaComputeDescriptorSetCount>
      swap_apply_gamma_compute_descriptor_set_layouts{};
  swap_apply_gamma_compute_descriptor_set_layouts[kSwapApplyGammaComputeDescriptorSetRamp] =
      swap_descriptor_set_layout_uniform_texel_buffer_;
  swap_apply_gamma_compute_descriptor_set_layouts[kSwapApplyGammaComputeDescriptorSetSource] =
      swap_descriptor_set_layout_sampled_image_;
  swap_apply_gamma_compute_descriptor_set_layouts[kSwapApplyGammaComputeDescriptorSetDestination] =
      swap_descriptor_set_layout_storage_image_;
  VkPushConstantRange swap_apply_gamma_compute_push_constant_range;
  swap_apply_gamma_compute_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  swap_apply_gamma_compute_push_constant_range.offset = 0;
  swap_apply_gamma_compute_push_constant_range.size = sizeof(SwapApplyGammaConstants);
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      static_cast<u32>(swap_apply_gamma_compute_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_apply_gamma_compute_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 1;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges =
      &swap_apply_gamma_compute_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
                                 &swap_apply_gamma_compute_pipeline_layout_) != VK_SUCCESS) {
    REX_FAIL(gpu, false, "Failed to create the gamma ramp application compute pipeline layout");
  }

  std::array<VkDescriptorSetLayout, kSwapFxaaDescriptorSetCount> swap_fxaa_descriptor_set_layouts{};
  swap_fxaa_descriptor_set_layouts[kSwapFxaaDescriptorSetSource] =
      swap_descriptor_set_layout_combined_image_sampler_;
  swap_fxaa_descriptor_set_layouts[kSwapFxaaDescriptorSetDestination] =
      swap_descriptor_set_layout_storage_image_;
  VkPushConstantRange swap_fxaa_push_constant_range;
  swap_fxaa_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  swap_fxaa_push_constant_range.offset = 0;
  swap_fxaa_push_constant_range.size = sizeof(SwapFxaaConstants);
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      static_cast<u32>(swap_fxaa_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_fxaa_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 1;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges = &swap_fxaa_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
                                 &swap_fxaa_pipeline_layout_) != VK_SUCCESS) {
    REX_FAIL(gpu, false, "Failed to create the FXAA compute pipeline layout");
  }

  // No assumptions about outer usage, explicit barriers are required.
  VkAttachmentDescription swap_apply_gamma_render_pass_attachment;
  swap_apply_gamma_render_pass_attachment.flags = 0;
  swap_apply_gamma_render_pass_attachment.format = gpu::vulkan::VulkanPresenter::kGuestOutputFormat;
  swap_apply_gamma_render_pass_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  swap_apply_gamma_render_pass_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swap_apply_gamma_render_pass_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  swap_apply_gamma_render_pass_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swap_apply_gamma_render_pass_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  swap_apply_gamma_render_pass_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  swap_apply_gamma_render_pass_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference swap_apply_gamma_render_pass_color_attachment;
  swap_apply_gamma_render_pass_color_attachment.attachment = 0;
  swap_apply_gamma_render_pass_color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkSubpassDescription swap_apply_gamma_render_pass_subpass = {};
  swap_apply_gamma_render_pass_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  swap_apply_gamma_render_pass_subpass.colorAttachmentCount = 1;
  swap_apply_gamma_render_pass_subpass.pColorAttachments =
      &swap_apply_gamma_render_pass_color_attachment;
  VkSubpassDependency swap_apply_gamma_render_pass_dependencies[2];
  for (u32 i = 0; i < 2; ++i) {
    VkSubpassDependency& swap_apply_gamma_render_pass_dependency =
        swap_apply_gamma_render_pass_dependencies[i];
    swap_apply_gamma_render_pass_dependency.srcSubpass = i ? 0 : VK_SUBPASS_EXTERNAL;
    swap_apply_gamma_render_pass_dependency.dstSubpass = i ? VK_SUBPASS_EXTERNAL : 0;
    swap_apply_gamma_render_pass_dependency.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    swap_apply_gamma_render_pass_dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    swap_apply_gamma_render_pass_dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    swap_apply_gamma_render_pass_dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    swap_apply_gamma_render_pass_dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
  }
  VkRenderPassCreateInfo swap_apply_gamma_render_pass_create_info;
  swap_apply_gamma_render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  swap_apply_gamma_render_pass_create_info.pNext = nullptr;
  swap_apply_gamma_render_pass_create_info.flags = 0;
  swap_apply_gamma_render_pass_create_info.attachmentCount = 1;
  swap_apply_gamma_render_pass_create_info.pAttachments = &swap_apply_gamma_render_pass_attachment;
  swap_apply_gamma_render_pass_create_info.subpassCount = 1;
  swap_apply_gamma_render_pass_create_info.pSubpasses = &swap_apply_gamma_render_pass_subpass;
  swap_apply_gamma_render_pass_create_info.dependencyCount =
      static_cast<u32>(rex::CountOf(swap_apply_gamma_render_pass_dependencies));
  swap_apply_gamma_render_pass_create_info.pDependencies =
      swap_apply_gamma_render_pass_dependencies;
  if (dfn.vkCreateRenderPass(device, &swap_apply_gamma_render_pass_create_info, nullptr,
                             &swap_apply_gamma_render_pass_) != VK_SUCCESS) {
    REX_FAIL(gpu, false, "Failed to create the gamma ramp application render pass");
  }

  // A graphics pipeline, not compute, because storage image support is optional for A2B10G10R10.

  enum SwapApplyGammaPixelShader : u8 {
    kSwapApplyGammaPixelShader256EntryTable,
    kSwapApplyGammaPixelShaderPWL,
    kSwapApplyGammaPixelShader256EntryTableFxaaLuma,
    kSwapApplyGammaPixelShaderPWLFxaaLuma,

    kSwapApplyGammaPixelShaderCount,
  };
  std::array<VkShaderModule, kSwapApplyGammaPixelShaderCount> swap_apply_gamma_pixel_shaders{};
  bool swap_apply_gamma_pixel_shaders_created =
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTable] =
           gpu::vulkan::objects::CreateShaderModule(vulkan_device, shaders::kApplyGammaTablePS,
                                                    sizeof(shaders::kApplyGammaTablePS))) !=
          VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWL] =
           gpu::vulkan::objects::CreateShaderModule(vulkan_device, shaders::kApplyGammaPWLPS,
                                                    sizeof(shaders::kApplyGammaPWLPS))) !=
          VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTableFxaaLuma] =
           gpu::vulkan::objects::CreateShaderModule(
               vulkan_device, shaders::kApplyGammaTableFXAALumaPS,
               sizeof(shaders::kApplyGammaTableFXAALumaPS))) != VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWLFxaaLuma] =
           gpu::vulkan::objects::CreateShaderModule(
               vulkan_device, shaders::kApplyGammaPWLFXAALumaPS,
               sizeof(shaders::kApplyGammaPWLFXAALumaPS))) != VK_NULL_HANDLE;
  if (!swap_apply_gamma_pixel_shaders_created) {
    REXLOG_ERROR(gpu, "Failed to create the gamma ramp application pixel shader modules");
    for (VkShaderModule swap_apply_gamma_pixel_shader : swap_apply_gamma_pixel_shaders) {
      if (swap_apply_gamma_pixel_shader != VK_NULL_HANDLE) {
        dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader, nullptr);
      }
    }
    return false;
  }

  VkPipelineShaderStageCreateInfo swap_apply_gamma_pipeline_stages[2];
  swap_apply_gamma_pipeline_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  swap_apply_gamma_pipeline_stages[0].pNext = nullptr;
  swap_apply_gamma_pipeline_stages[0].flags = 0;
  swap_apply_gamma_pipeline_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  swap_apply_gamma_pipeline_stages[0].module = gpu::vulkan::objects::CreateShaderModule(
      vulkan_device, shaders::kFullscreenCWVS, sizeof(shaders::kFullscreenCWVS));
  if (swap_apply_gamma_pipeline_stages[0].module == VK_NULL_HANDLE) {
    REXLOG_ERROR(gpu, "Failed to create the gamma ramp application vertex shader module");
    for (VkShaderModule swap_apply_gamma_pixel_shader : swap_apply_gamma_pixel_shaders) {
      REX_ASSERT(swap_apply_gamma_pixel_shader != VK_NULL_HANDLE);
      dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader, nullptr);
    }
    return false;
  }
  swap_apply_gamma_pipeline_stages[0].pName = "main";
  swap_apply_gamma_pipeline_stages[0].pSpecializationInfo = nullptr;
  swap_apply_gamma_pipeline_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  swap_apply_gamma_pipeline_stages[1].pNext = nullptr;
  swap_apply_gamma_pipeline_stages[1].flags = 0;
  swap_apply_gamma_pipeline_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  // The fragment shader module will be specified later.
  swap_apply_gamma_pipeline_stages[1].pName = "main";
  swap_apply_gamma_pipeline_stages[1].pSpecializationInfo = nullptr;

  VkPipelineVertexInputStateCreateInfo swap_apply_gamma_pipeline_vertex_input_state = {};
  swap_apply_gamma_pipeline_vertex_input_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo swap_apply_gamma_pipeline_input_assembly_state;
  swap_apply_gamma_pipeline_input_assembly_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_input_assembly_state.pNext = nullptr;
  swap_apply_gamma_pipeline_input_assembly_state.flags = 0;
  swap_apply_gamma_pipeline_input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  swap_apply_gamma_pipeline_input_assembly_state.primitiveRestartEnable = VK_FALSE;

  VkPipelineViewportStateCreateInfo swap_apply_gamma_pipeline_viewport_state;
  swap_apply_gamma_pipeline_viewport_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_viewport_state.pNext = nullptr;
  swap_apply_gamma_pipeline_viewport_state.flags = 0;
  swap_apply_gamma_pipeline_viewport_state.viewportCount = 1;
  swap_apply_gamma_pipeline_viewport_state.pViewports = nullptr;
  swap_apply_gamma_pipeline_viewport_state.scissorCount = 1;
  swap_apply_gamma_pipeline_viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo swap_apply_gamma_pipeline_rasterization_state = {};
  swap_apply_gamma_pipeline_rasterization_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  swap_apply_gamma_pipeline_rasterization_state.cullMode = VK_CULL_MODE_NONE;
  swap_apply_gamma_pipeline_rasterization_state.frontFace = VK_FRONT_FACE_CLOCKWISE;
  swap_apply_gamma_pipeline_rasterization_state.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo swap_apply_gamma_pipeline_multisample_state =
      objects::BuildMultisampleState(VK_SAMPLE_COUNT_1_BIT);

  VkPipelineColorBlendAttachmentState swap_apply_gamma_pipeline_color_blend_attachment_state = {};
  swap_apply_gamma_pipeline_color_blend_attachment_state.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
      VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo swap_apply_gamma_pipeline_color_blend_state = {};
  swap_apply_gamma_pipeline_color_blend_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_color_blend_state.attachmentCount = 1;
  swap_apply_gamma_pipeline_color_blend_state.pAttachments =
      &swap_apply_gamma_pipeline_color_blend_attachment_state;

  static const VkDynamicState kSwapApplyGammaPipelineDynamicStates[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  VkPipelineDynamicStateCreateInfo swap_apply_gamma_pipeline_dynamic_state;
  swap_apply_gamma_pipeline_dynamic_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_dynamic_state.pNext = nullptr;
  swap_apply_gamma_pipeline_dynamic_state.flags = 0;
  swap_apply_gamma_pipeline_dynamic_state.dynamicStateCount =
      static_cast<u32>(rex::CountOf(kSwapApplyGammaPipelineDynamicStates));
  swap_apply_gamma_pipeline_dynamic_state.pDynamicStates = kSwapApplyGammaPipelineDynamicStates;

  VkGraphicsPipelineCreateInfo swap_apply_gamma_pipeline_create_info;
  swap_apply_gamma_pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  swap_apply_gamma_pipeline_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_create_info.flags = 0;
  swap_apply_gamma_pipeline_create_info.stageCount =
      static_cast<u32>(rex::CountOf(swap_apply_gamma_pipeline_stages));
  swap_apply_gamma_pipeline_create_info.pStages = swap_apply_gamma_pipeline_stages;
  swap_apply_gamma_pipeline_create_info.pVertexInputState =
      &swap_apply_gamma_pipeline_vertex_input_state;
  swap_apply_gamma_pipeline_create_info.pInputAssemblyState =
      &swap_apply_gamma_pipeline_input_assembly_state;
  swap_apply_gamma_pipeline_create_info.pTessellationState = nullptr;
  swap_apply_gamma_pipeline_create_info.pViewportState = &swap_apply_gamma_pipeline_viewport_state;
  swap_apply_gamma_pipeline_create_info.pRasterizationState =
      &swap_apply_gamma_pipeline_rasterization_state;
  swap_apply_gamma_pipeline_create_info.pMultisampleState =
      &swap_apply_gamma_pipeline_multisample_state;
  swap_apply_gamma_pipeline_create_info.pDepthStencilState = nullptr;
  swap_apply_gamma_pipeline_create_info.pColorBlendState =
      &swap_apply_gamma_pipeline_color_blend_state;
  swap_apply_gamma_pipeline_create_info.pDynamicState = &swap_apply_gamma_pipeline_dynamic_state;
  swap_apply_gamma_pipeline_create_info.layout = swap_apply_gamma_pipeline_layout_;
  swap_apply_gamma_pipeline_create_info.renderPass = swap_apply_gamma_render_pass_;
  swap_apply_gamma_pipeline_create_info.subpass = 0;
  swap_apply_gamma_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  swap_apply_gamma_pipeline_create_info.basePipelineIndex = -1;
  std::array<VkShaderModule, 4> swap_apply_gamma_pipeline_pixel_shaders = {
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTable],
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWL],
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTableFxaaLuma],
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWLFxaaLuma],
  };
  std::array<VkPipeline*, 4> swap_apply_gamma_pipelines = {
      &swap_apply_gamma_256_entry_table_pipeline_,
      &swap_apply_gamma_pwl_pipeline_,
      &swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_,
      &swap_apply_gamma_pwl_fxaa_luma_pipeline_,
  };
  std::array<VkResult, 4> swap_apply_gamma_pipeline_create_results;
  for (size_t i = 0; i < swap_apply_gamma_pipelines.size(); ++i) {
    swap_apply_gamma_pipeline_stages[1].module = swap_apply_gamma_pipeline_pixel_shaders[i];
    swap_apply_gamma_pipeline_create_results[i] = dfn.vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &swap_apply_gamma_pipeline_create_info, nullptr,
        swap_apply_gamma_pipelines[i]);
  }
  if (!vulkan_device->GetProperties().imageViewFormatSwizzle) {
    auto create_rb_swap_pipeline = [&](const char* source, VkPipeline& pipeline_out,
                                       const char* pipeline_name) {
      std::vector<u32> pixel_spirv;
      std::string compile_error;
      if (!CompileGLSLToSPIRV(VK_SHADER_STAGE_FRAGMENT_BIT, source, pixel_spirv, compile_error)) {
        REXLOG_WARN(gpu, "Failed to compile {} shader to SPIR-V: {}", pipeline_name, compile_error);
        return;
      }
      VkShaderModule pixel_shader_module = gpu::vulkan::objects::CreateShaderModule(
          vulkan_device, pixel_spirv.data(), sizeof(u32) * pixel_spirv.size());
      if (pixel_shader_module == VK_NULL_HANDLE) {
        REXLOG_WARN(gpu, "Failed to create {} shader module", pipeline_name);
        return;
      }
      swap_apply_gamma_pipeline_stages[1].module = pixel_shader_module;
      if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                        &swap_apply_gamma_pipeline_create_info, nullptr,
                                        &pipeline_out) != VK_SUCCESS) {
        REXLOG_WARN(gpu, "Failed to create {} pipeline", pipeline_name);
        pipeline_out = VK_NULL_HANDLE;
      }
      dfn.vkDestroyShaderModule(device, pixel_shader_module, nullptr);
    };
    create_rb_swap_pipeline(GetSwapApplyGammaTablePixelRbSwapSource(),
                            swap_apply_gamma_256_entry_table_rb_swap_pipeline_,
                            "swap gamma table red/blue fallback");
    create_rb_swap_pipeline(GetSwapApplyGammaPwlPixelRbSwapSource(),
                            swap_apply_gamma_pwl_rb_swap_pipeline_,
                            "swap gamma PWL red/blue fallback");
  }
  dfn.vkDestroyShaderModule(device, swap_apply_gamma_pipeline_stages[0].module, nullptr);
  for (VkShaderModule swap_apply_gamma_pixel_shader : swap_apply_gamma_pixel_shaders) {
    REX_ASSERT(swap_apply_gamma_pixel_shader != VK_NULL_HANDLE);
    dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader, nullptr);
  }
  if (std::any_of(swap_apply_gamma_pipeline_create_results.begin(),
                  swap_apply_gamma_pipeline_create_results.end(),
                  [](VkResult result) { return result != VK_SUCCESS; })) {
    REX_FAIL(gpu, false, "Failed to create the gamma ramp application pipelines");
  }

  swap_apply_gamma_compute_256_entry_table_pipeline_ = gpu::vulkan::objects::CreateComputePipeline(
      vulkan_device, swap_apply_gamma_compute_pipeline_layout_, shaders::kApplyGammaTableCS,
      sizeof(shaders::kApplyGammaTableCS));
  if (swap_apply_gamma_compute_256_entry_table_pipeline_ == VK_NULL_HANDLE) {
    REXLOG_WARN(gpu,
                "Failed to create the 256-entry table gamma ramp application compute "
                "pipeline, keeping graphics fallback");
  }
  swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_ =
      gpu::vulkan::objects::CreateComputePipeline(
          vulkan_device, swap_apply_gamma_compute_pipeline_layout_,
          shaders::kApplyGammaTableFXAALumaCS, sizeof(shaders::kApplyGammaTableFXAALumaCS));
  if (swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_ == VK_NULL_HANDLE) {
    REXLOG_WARN(gpu,
                "Failed to create the 256-entry table gamma ramp application compute "
                "pipeline with luma output");
  }
  swap_apply_gamma_compute_pwl_pipeline_ = gpu::vulkan::objects::CreateComputePipeline(
      vulkan_device, swap_apply_gamma_compute_pipeline_layout_, shaders::kApplyGammaPWLCS,
      sizeof(shaders::kApplyGammaPWLCS));
  if (swap_apply_gamma_compute_pwl_pipeline_ == VK_NULL_HANDLE) {
    REXLOG_WARN(gpu, "Failed to create the PWL gamma ramp application compute pipeline");
  }
  swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_ = gpu::vulkan::objects::CreateComputePipeline(
      vulkan_device, swap_apply_gamma_compute_pipeline_layout_, shaders::kApplyGammaPWLFXAALumaCS,
      sizeof(shaders::kApplyGammaPWLFXAALumaCS));
  if (swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_ == VK_NULL_HANDLE) {
    REXLOG_WARN(gpu,
                "Failed to create the PWL gamma ramp application compute pipeline with "
                "luma output");
  }
  if (!vulkan_device->GetProperties().imageViewFormatSwizzle) {
    auto create_rb_swap_compute_pipeline = [&](const char* source, VkPipeline& pipeline_out,
                                               const char* pipeline_name) {
      std::vector<u32> compute_spirv;
      std::string compile_error;
      if (!CompileGLSLToSPIRV(VK_SHADER_STAGE_COMPUTE_BIT, source, compute_spirv, compile_error)) {
        REXLOG_WARN(gpu, "Failed to compile {} shader to SPIR-V: {}", pipeline_name, compile_error);
        return;
      }
      pipeline_out = gpu::vulkan::objects::CreateComputePipeline(
          vulkan_device, swap_apply_gamma_compute_pipeline_layout_, compute_spirv.data(),
          sizeof(u32) * compute_spirv.size());
      if (pipeline_out == VK_NULL_HANDLE) {
        REXLOG_WARN(gpu, "Failed to create {} pipeline", pipeline_name);
      }
    };
    create_rb_swap_compute_pipeline(GetSwapApplyGammaTableComputeRbSwapSource(),
                                    swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_,
                                    "swap gamma table compute red/blue fallback");
    create_rb_swap_compute_pipeline(GetSwapApplyGammaPwlComputeRbSwapSource(),
                                    swap_apply_gamma_compute_pwl_rb_swap_pipeline_,
                                    "swap gamma PWL compute red/blue fallback");
    create_rb_swap_compute_pipeline(
        GetSwapApplyGammaTableFxaaLumaComputeRbSwapSource(),
        swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_,
        "swap gamma table FXAA-luma compute red/blue fallback");
    create_rb_swap_compute_pipeline(GetSwapApplyGammaPwlFxaaLumaComputeRbSwapSource(),
                                    swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_,
                                    "swap gamma PWL FXAA-luma compute red/blue fallback");
  }

  std::vector<u32> swap_fxaa_spirv;
  std::string swap_fxaa_compile_error;
  if (!CompileGLSLToSPIRV(VK_SHADER_STAGE_COMPUTE_BIT, GetSwapFxaaComputeSource(false),
                          swap_fxaa_spirv, swap_fxaa_compile_error)) {
    REXLOG_WARN(gpu, "Failed to compile FXAA compute shader to SPIR-V: {}",
                swap_fxaa_compile_error);
  } else {
    swap_fxaa_pipeline_ = gpu::vulkan::objects::CreateComputePipeline(
        vulkan_device, swap_fxaa_pipeline_layout_, swap_fxaa_spirv.data(),
        sizeof(u32) * swap_fxaa_spirv.size());
    if (swap_fxaa_pipeline_ == VK_NULL_HANDLE) {
      REXLOG_WARN(gpu, "Failed to create the FXAA compute pipeline");
    }
  }

  std::vector<u32> swap_fxaa_extreme_spirv;
  std::string swap_fxaa_extreme_compile_error;
  if (!CompileGLSLToSPIRV(VK_SHADER_STAGE_COMPUTE_BIT, GetSwapFxaaComputeSource(true),
                          swap_fxaa_extreme_spirv, swap_fxaa_extreme_compile_error)) {
    REXLOG_WARN(gpu, "Failed to compile extreme FXAA compute shader to SPIR-V: {}",
                swap_fxaa_extreme_compile_error);
  } else {
    swap_fxaa_extreme_pipeline_ = gpu::vulkan::objects::CreateComputePipeline(
        vulkan_device, swap_fxaa_pipeline_layout_, swap_fxaa_extreme_spirv.data(),
        sizeof(u32) * swap_fxaa_extreme_spirv.size());
    if (swap_fxaa_extreme_pipeline_ == VK_NULL_HANDLE) {
      REXLOG_WARN(gpu, "Failed to create the extreme-quality FXAA compute pipeline");
    }
  }

  VkPipelineLayoutCreateInfo resolve_downscale_layout_create_info = {};
  resolve_downscale_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkDescriptorSetLayout resolve_downscale_set_layout =
      descriptor_set_layouts_single_transient_[static_cast<size_t>(
          SingleTransientDescriptorLayout::kStorageBufferPairCompute)];
  resolve_downscale_layout_create_info.setLayoutCount = 1;
  resolve_downscale_layout_create_info.pSetLayouts = &resolve_downscale_set_layout;
  VkPushConstantRange resolve_downscale_push_constant_range = {};
  resolve_downscale_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  resolve_downscale_push_constant_range.offset = 0;
  resolve_downscale_push_constant_range.size = sizeof(ResolveDownscaleConstants);
  resolve_downscale_layout_create_info.pushConstantRangeCount = 1;
  resolve_downscale_layout_create_info.pPushConstantRanges = &resolve_downscale_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &resolve_downscale_layout_create_info, nullptr,
                                 &resolve_downscale_pipeline_layout_) == VK_SUCCESS) {
    resolve_downscale_pipeline_ = gpu::vulkan::objects::CreateComputePipeline(
        vulkan_device, resolve_downscale_pipeline_layout_, shaders::kResolveDownscaleCS,
        sizeof(shaders::kResolveDownscaleCS));
    if (resolve_downscale_pipeline_ == VK_NULL_HANDLE) {
      REXLOG_WARN(gpu, "Failed to create Vulkan resolve-downscale readback pipeline");
      gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                                 resolve_downscale_pipeline_layout_);
    }
  } else {
    REXLOG_WARN(gpu, "Failed to create Vulkan resolve-downscale pipeline layout");
  }

  occlusion_query_resources_available_ = InitializeOcclusionQueryResources();

  // Not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));

  return true;
}

void VulkanCommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();
  InvalidateAllVertexBufferResidency();
  ShutdownOcclusionQueryResources();

  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  DestroyScratchBuffer();

  for (auto& readback_pair : readback_buffers_) {
    ReadbackBuffer& readback = readback_pair.second;
    for (u32 i = 0; i < 2; ++i) {
      if (readback.mapped_data[i] && readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[i]);
      }
      gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, readback.buffers[i]);
      gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device, readback.memories[i]);
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  readback_buffers_.clear();
  for (auto& readback_pair : memexport_readback_buffers_) {
    ReadbackBuffer& readback = readback_pair.second;
    for (u32 i = 0; i < 2; ++i) {
      if (readback.mapped_data[i] && readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[i]);
      }
      gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, readback.buffers[i]);
      gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device, readback.memories[i]);
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  memexport_readback_buffers_.clear();

  resolve_downscale_buffer_size_ = 0;
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                             resolve_downscale_buffer_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                             resolve_downscale_buffer_memory_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             resolve_downscale_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             resolve_downscale_pipeline_layout_);

  for (SwapFramebuffer& swap_framebuffer : swap_framebuffers_) {
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                               swap_framebuffer.framebuffer);
  }
  DestroySwapFxaaSourceImage();

  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             swap_fxaa_extreme_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, swap_fxaa_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             swap_apply_gamma_compute_pwl_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device, swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             swap_apply_gamma_compute_pwl_rb_swap_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device, swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device,
      swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             swap_apply_gamma_compute_256_entry_table_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device, swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             swap_apply_gamma_pwl_rb_swap_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             swap_apply_gamma_256_entry_table_rb_swap_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             swap_apply_gamma_pwl_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             swap_apply_gamma_pwl_fxaa_luma_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             swap_apply_gamma_256_entry_table_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                                             swap_apply_gamma_render_pass_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             swap_fxaa_pipeline_layout_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             swap_apply_gamma_compute_pipeline_layout_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             swap_apply_gamma_pipeline_layout_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroySampler, device,
                                             swap_sampler_linear_clamp_);

  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                             swap_descriptor_pool_);

  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                             swap_descriptor_set_layout_uniform_texel_buffer_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                             swap_descriptor_set_layout_storage_image_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                             swap_descriptor_set_layout_combined_image_sampler_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                             swap_descriptor_set_layout_sampled_image_);
  for (VkBufferView& gamma_ramp_buffer_view : gamma_ramp_buffer_views_) {
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBufferView, device,
                                               gamma_ramp_buffer_view);
  }
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                             gamma_ramp_upload_buffer_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                             gamma_ramp_upload_buffer_memory_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, gamma_ramp_buffer_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device, gamma_ramp_buffer_memory_);

  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                             shared_memory_and_edram_descriptor_pool_);

  texture_cache_.reset();

  pipeline_cache_.reset();

  render_target_cache_.reset();

  primitive_processor_.reset();

  shared_memory_.reset();

  ClearTransientDescriptorPools();

  for (const auto& pipeline_layout_pair : pipeline_layouts_) {
    dfn.vkDestroyPipelineLayout(device, pipeline_layout_pair.second.GetPipelineLayout(), nullptr);
  }
  pipeline_layouts_.clear();
  for (const auto& descriptor_set_layout_pair : descriptor_set_layouts_textures_) {
    dfn.vkDestroyDescriptorSetLayout(device, descriptor_set_layout_pair.second, nullptr);
  }
  descriptor_set_layouts_textures_.clear();

  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                             descriptor_set_layout_shared_memory_and_edram_);
  for (VkDescriptorSetLayout& descriptor_set_layout_single_transient :
       descriptor_set_layouts_single_transient_) {
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                               descriptor_set_layout_single_transient);
  }
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                             descriptor_set_layout_constants_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                             descriptor_set_layout_empty_);

  uniform_buffer_pool_.reset();

  sparse_bind_wait_stage_mask_ = 0;
  sparse_buffer_binds_.clear();
  sparse_memory_binds_.clear();

  deferred_command_buffer_.Reset();
  for (const auto& command_buffer_pair : command_buffers_submitted_) {
    dfn.vkDestroyCommandPool(device, command_buffer_pair.second.pool, nullptr);
  }
  command_buffers_submitted_.clear();
  for (const CommandBuffer& command_buffer : command_buffers_writable_) {
    dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
  }
  command_buffers_writable_.clear();

  for (const auto& destroy_pair : destroy_framebuffers_) {
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
  }
  destroy_framebuffers_.clear();
  for (const auto& destroy_pair : destroy_image_views_) {
    dfn.vkDestroyImageView(device, destroy_pair.second, nullptr);
  }
  destroy_image_views_.clear();
  for (const auto& destroy_pair : destroy_buffers_) {
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
  }
  destroy_buffers_.clear();
  for (const auto& destroy_pair : destroy_images_) {
    dfn.vkDestroyImage(device, destroy_pair.second, nullptr);
  }
  destroy_images_.clear();
  for (const auto& destroy_pair : destroy_memory_) {
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
  }
  destroy_memory_.clear();

  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));
  frame_completed_ = 0;
  frame_current_ = 1;
  frame_open_ = false;

  for (const auto& semaphore : submissions_in_flight_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore.second, nullptr);
  }
  submissions_in_flight_semaphores_.clear();
  for (VkFence& fence : submissions_in_flight_fences_) {
    dfn.vkDestroyFence(device, fence, nullptr);
  }
  submissions_in_flight_fences_.clear();
  current_submission_wait_stage_masks_.clear();
  for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  current_submission_wait_semaphores_.clear();
  submission_completed_ = 0;
  submission_open_ = false;

  for (VkSemaphore semaphore : semaphores_free_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  semaphores_free_.clear();
  for (VkFence fence : fences_free_) {
    dfn.vkDestroyFence(device, fence, nullptr);
  }
  fences_free_.clear();

  device_lost_ = false;

  CommandProcessor::ShutdownContext();
}

void VulkanCommandProcessor::WriteRegister(u32 index, u32 value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X) &&
      index <= std::to_underlying(xenos::Register::SHADER_CONSTANT_511_W)) {
    if (frame_open_) {
      u32 float_constant_index =
          (index - std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X)) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferFloatPixel);
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferFloatVertex);
        }
      }
    }
  } else if (index >= std::to_underlying(xenos::Register::SHADER_CONSTANT_BOOL_000_031) &&
             index <= std::to_underlying(xenos::Register::SHADER_CONSTANT_LOOP_31)) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferBoolLoop);
  } else if (index >= std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0) &&
             index <= std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_31_5)) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferFetch);
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantWritten(
          (index - std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0)) / 6);
    }
    InvalidateVertexBufferResidency(
        (index - std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0)) / 2);
  }
}

void VulkanCommandProcessor::WriteRegistersFromMem(u32 start_index, u32* base, u32 num_registers) {
  if (!num_registers) {
    return;
  }
  u32 end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage = [](const u64* usage_map, u32 first_constant,
                                         u32 last_constant) -> bool {
    if (first_constant > last_constant) {
      return false;
    }
    u32 first_word = first_constant >> 6;
    u32 last_word = last_constant >> 6;
    u32 first_bit = first_constant & 63;
    u32 last_bit = last_constant & 63;
    if (first_word == last_word) {
      u32 bit_count = last_bit - first_bit + 1;
      u64 mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << first_bit;
      return (usage_map[first_word] & mask) != 0;
    }
    if (usage_map[first_word] & (UINT64_MAX << first_bit)) {
      return true;
    }
    for (u32 word = first_word + 1; word < last_word; ++word) {
      if (usage_map[word]) {
        return true;
      }
    }
    u64 last_mask = last_bit == 63 ? UINT64_MAX : ((UINT64_C(1) << (last_bit + 1)) - 1);
    return (usage_map[last_word] & last_mask) != 0;
  };

  if (start_index >= std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X) &&
      end_index <= std::to_underlying(xenos::Register::SHADER_CONSTANT_511_W)) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (frame_open_) {
      u32 first_float_constant =
          (start_index - std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X)) >> 2;
      u32 last_float_constant =
          (end_index - std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X)) >> 2;
      if (first_float_constant < 256) {
        u32 last_vertex_constant = std::min(last_float_constant, 255u);
        if (range_has_any_constant_usage(current_float_constant_map_vertex_, first_float_constant,
                                         last_vertex_constant)) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferFloatVertex);
        }
      }
      if (last_float_constant >= 256) {
        u32 first_pixel_constant = first_float_constant >= 256 ? first_float_constant - 256 : 0;
        u32 last_pixel_constant = last_float_constant - 256;
        if (range_has_any_constant_usage(current_float_constant_map_pixel_, first_pixel_constant,
                                         last_pixel_constant)) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferFloatPixel);
        }
      }
    }
    return;
  }

  if (start_index >= std::to_underlying(xenos::Register::SHADER_CONSTANT_BOOL_000_031) &&
      end_index <= std::to_underlying(xenos::Register::SHADER_CONSTANT_LOOP_31)) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferBoolLoop);
    return;
  }

  if (start_index >= std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0) &&
      end_index <= std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_31_5)) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferFetch);
    u32 first_fetch_dword =
        start_index - std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0);
    u32 last_fetch_dword =
        end_index - std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0);
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
    }
    InvalidateVertexBufferResidencyRange(first_fetch_dword / 2, last_fetch_dword / 2);
    return;
  }

  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void VulkanCommandProcessor::SparseBindBuffer(VkBuffer buffer, u32 bind_count,
                                              const VkSparseMemoryBind* binds,
                                              VkPipelineStageFlags wait_stage_mask) {
  if (!bind_count) {
    return;
  }
  SparseBufferBind& buffer_bind = sparse_buffer_binds_.emplace_back();
  buffer_bind.buffer = buffer;
  buffer_bind.bind_offset = sparse_memory_binds_.size();
  buffer_bind.bind_count = bind_count;
  sparse_memory_binds_.reserve(sparse_memory_binds_.size() + bind_count);
  sparse_memory_binds_.insert(sparse_memory_binds_.end(), binds, binds + bind_count);
  sparse_bind_wait_stage_mask_ |= wait_stage_mask;
}

void VulkanCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
}

void VulkanCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
}

void VulkanCommandProcessor::IssueSwap([[maybe_unused]] u32 frontbuffer_ptr, u32 frontbuffer_width,
                                       u32 frontbuffer_height) {
  REX_PROFILE_SCOPE("gpu");
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;

  if (!graphics_system_)
    return;
  gpu::Presenter* presenter = graphics_system_->GetHostPresenter();
  if (!presenter) {
    REXLOG_ERROR(gpu, "XELOG_GPU PRESENT: NO PRESENTER");
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    REXLOG_ERROR(gpu, "XELOG_GPU PRESENT: BeginSubmission FAILED");
    return;
  }

  bool skip_present_due_async_placeholder =
      REXCVAR_GET(::rex::gpu::cvars::async_shader_compilation) &&
      REXCVAR_GET(::rex::gpu::cvars::vulkan_async_skip_incomplete_frames) &&
      frame_used_async_placeholder_pipeline_;
  if (skip_present_due_async_placeholder) {
    static bool skipped_incomplete_frame_logged = false;
    if (!skipped_incomplete_frame_logged) {
      skipped_incomplete_frame_logged = true;
      REXLOG_WARN(gpu,
                  "Skipping Vulkan frame presentation due to async placeholder draw "
                  "usage in this frame");
    }
    EndSubmission(true);
    return;
  }

  SwapPostEffect swap_post_effect = GetActualSwapPostEffect();

  u32 frontbuffer_width_scaled, frontbuffer_height_scaled;
  u32 frontbuffer_width_unscaled = 0, frontbuffer_height_unscaled = 0;
  xenos::TextureFormat frontbuffer_format;
  bool swap_source_needs_rb_swap = false;
  VkImageView swap_texture_view = texture_cache_->RequestSwapTexture(
      frontbuffer_width_scaled, frontbuffer_height_scaled, frontbuffer_format,
      &frontbuffer_width_unscaled, &frontbuffer_height_unscaled, &swap_source_needs_rb_swap);
  if (swap_texture_view == VK_NULL_HANDLE) {
    REXLOG_ERROR(gpu, "XELOG_GPU PRESENT: swap_texture_view=NULL");
    return;
  }
  // Swap textures may be allocation padded, so the swap packet frontbuffer region is preferred.
  auto get_active_swap_dimension = [](u32 packet_unscaled, u32 source_unscaled,
                                      u32 source_scaled) -> u32 {
    if (!source_scaled) {
      return 0;
    }
    u32 active_unscaled = packet_unscaled ? packet_unscaled : source_unscaled;
    if (!active_unscaled) {
      return source_scaled;
    }
    if (source_unscaled) {
      active_unscaled = std::min(active_unscaled, source_unscaled);
      u64 active_scaled =
          (static_cast<u64>(active_unscaled) * source_scaled + (source_unscaled >> 1)) /
          source_unscaled;
      return static_cast<u32>(std::clamp<u64>(active_scaled, 1, source_scaled));
    }
    return std::min(active_unscaled, source_scaled);
  };
  u32 guest_output_width = get_active_swap_dimension(frontbuffer_width, frontbuffer_width_unscaled,
                                                     frontbuffer_width_scaled);
  u32 guest_output_height = get_active_swap_dimension(
      frontbuffer_height, frontbuffer_height_unscaled, frontbuffer_height_scaled);
  if (!guest_output_width) {
    guest_output_width = frontbuffer_width_scaled
                             ? frontbuffer_width_scaled
                             : (frontbuffer_width ? frontbuffer_width : frontbuffer_width_unscaled);
  }
  if (!guest_output_height) {
    guest_output_height =
        frontbuffer_height_scaled
            ? frontbuffer_height_scaled
            : (frontbuffer_height ? frontbuffer_height : frontbuffer_height_unscaled);
  }
  bool swap_source_scaled = frontbuffer_width_unscaled && frontbuffer_height_unscaled &&
                            (frontbuffer_width_scaled != frontbuffer_width_unscaled ||
                             frontbuffer_height_scaled != frontbuffer_height_unscaled);
  if (texture_cache_->IsDrawResolutionScaled()) {
    static bool draw_scale_cache_mismatch_logged = false;
    u32 texture_scale_x = texture_cache_->GetDrawResolutionScaleX();
    u32 texture_scale_y = texture_cache_->GetDrawResolutionScaleY();
    u32 rt_scale_x =
        render_target_cache_ ? render_target_cache_->GetDrawResolutionScaleX() : texture_scale_x;
    u32 rt_scale_y =
        render_target_cache_ ? render_target_cache_->GetDrawResolutionScaleY() : texture_scale_y;
    if (!draw_scale_cache_mismatch_logged &&
        (texture_scale_x != rt_scale_x || texture_scale_y != rt_scale_y)) {
      draw_scale_cache_mismatch_logged = true;
      REXLOG_WARN(gpu,
                  "Vulkan draw-scale mismatch: texture cache is {}x{}, render target "
                  "cache is {}x{}",
                  texture_scale_x, texture_scale_y, rt_scale_x, rt_scale_y);
    }
    static bool draw_scale_swap_sizes_logged = false;
    if (!draw_scale_swap_sizes_logged) {
      draw_scale_swap_sizes_logged = true;
      REXLOG_WARN(gpu,
                  "Vulkan draw-scale swap sizing: packet={}x{}, src_scaled={}x{}, "
                  "src_unscaled={}x{}, active={}x{}",
                  frontbuffer_width, frontbuffer_height, frontbuffer_width_scaled,
                  frontbuffer_height_scaled, frontbuffer_width_unscaled,
                  frontbuffer_height_unscaled, guest_output_width, guest_output_height);
    }
  }
  if (texture_cache_->IsDrawResolutionScaled() && !swap_source_scaled) {
    static bool draw_scale_swap_unscaled_logged = false;
    if (!draw_scale_swap_unscaled_logged) {
      draw_scale_swap_unscaled_logged = true;
      REXLOG_WARN(gpu,
                  "Vulkan draw resolution scaling is enabled, but the swap source is "
                  "unscaled ({}x{}). This title may be presenting from an unscaled "
                  "resolve path.",
                  frontbuffer_width_scaled, frontbuffer_height_scaled);
    }
  }
  REXLOG_DEBUG(gpu,
               "XELOG_GPU PRESENT: swap_texture_view={:p} packet_size={}x{} src_size={}x{} "
               "src_unscaled={}x{} guest_output_size={}x{} format={}",
               static_cast<void*>(swap_texture_view), frontbuffer_width, frontbuffer_height,
               frontbuffer_width_scaled, frontbuffer_height_scaled, frontbuffer_width_unscaled,
               frontbuffer_height_unscaled, guest_output_width, guest_output_height,
               static_cast<u32>(frontbuffer_format));

  runtime::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  u32 display_width = std::max(static_cast<u32>(1), static_cast<u32>(video_mode.display_width));
  u32 display_height = std::max(static_cast<u32>(1), static_cast<u32>(video_mode.display_height));

  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,
      [this, guest_output_width, guest_output_height, frontbuffer_format, swap_texture_view,
       swap_post_effect,
       swap_source_needs_rb_swap](gpu::Presenter::GuestOutputRefreshContext& context) -> bool {
        // In case the swap command is the only one in the frame.
        if (!BeginSubmission(true)) {
          return false;
        }

        auto& vulkan_context =
            static_cast<gpu::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        u64 guest_output_image_version = vulkan_context.GetImageVersion();

        const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
        const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
        const VkDevice device = vulkan_device->GetDevice();

        u32 swap_frame_index = static_cast<u32>(frame_current_ % kMaxFramesInFlight);
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;

        // The guest initializes the 256 entry table for 8_8_8_8 output and PWL for 2_10_10_10.
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
        bool swap_source_requires_compute_rb_swap =
            !vulkan_device->GetProperties().imageViewFormatSwizzle && swap_source_needs_rb_swap;
        auto select_swap_apply_gamma_compute_pipeline = [&](bool use_pwl,
                                                            bool use_fxaa_luma) -> VkPipeline {
          if (use_pwl) {
            if (use_fxaa_luma) {
              return swap_source_requires_compute_rb_swap
                         ? swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_
                         : swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_;
            }
            return swap_source_requires_compute_rb_swap
                       ? swap_apply_gamma_compute_pwl_rb_swap_pipeline_
                       : swap_apply_gamma_compute_pwl_pipeline_;
          }
          if (use_fxaa_luma) {
            return swap_source_requires_compute_rb_swap
                       ? swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_
                       : swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_;
          }
          return swap_source_requires_compute_rb_swap
                     ? swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_
                     : swap_apply_gamma_compute_256_entry_table_pipeline_;
        };
        VkPipeline swap_apply_gamma_compute_pipeline =
            select_swap_apply_gamma_compute_pipeline(use_pwl_gamma_ramp, use_fxaa);

        if (use_fxaa) {
          if (swap_apply_gamma_compute_pipeline == VK_NULL_HANDLE ||
              swap_fxaa_pipeline_ == VK_NULL_HANDLE ||
              swap_fxaa_extreme_pipeline_ == VK_NULL_HANDLE) {
            static bool fxaa_pipelines_unavailable_logged = false;
            if (!fxaa_pipelines_unavailable_logged) {
              if (swap_source_requires_compute_rb_swap) {
                REXLOG_WARN(gpu,
                            "Vulkan FXAA swap effect requested but FXAA compute "
                            "pipelines (including RB-swap fallback) are unavailable, "
                            "falling back to gamma only");
              } else {
                REXLOG_WARN(gpu,
                            "Vulkan FXAA swap effect requested but FXAA compute "
                            "pipelines are unavailable, falling back to gamma only");
              }
              fxaa_pipelines_unavailable_logged = true;
            }
            use_fxaa = false;
          } else if (!EnsureSwapFxaaSourceImage(guest_output_width, guest_output_height)) {
            static bool fxaa_source_image_failed_logged = false;
            if (!fxaa_source_image_failed_logged) {
              REXLOG_WARN(gpu,
                          "Failed to create the Vulkan FXAA source image, falling "
                          "back to gamma-only presentation");
              fxaa_source_image_failed_logged = true;
            }
            use_fxaa = false;
          }
        }
        if (!use_fxaa) {
          swap_apply_gamma_compute_pipeline =
              select_swap_apply_gamma_compute_pipeline(use_pwl_gamma_ramp, false);
        }
        if (swap_source_requires_compute_rb_swap &&
            swap_apply_gamma_compute_pipeline == VK_NULL_HANDLE) {
          static bool compute_rb_swap_fallback_missing_logged = false;
          if (!compute_rb_swap_fallback_missing_logged) {
            compute_rb_swap_fallback_missing_logged = true;
            REXLOG_WARN(gpu,
                        "Vulkan imageViewFormatSwizzle is unavailable and the swap "
                        "source needs red/blue swizzle, but the compute fallback "
                        "pipeline is unavailable; falling back to graphics "
                        "presentation path");
          }
        }
        bool use_compute_gamma = swap_apply_gamma_compute_pipeline != VK_NULL_HANDLE;

        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        u32& gamma_ramp_frame_index_ref = use_pwl_gamma_ramp
                                              ? gamma_ramp_pwl_current_frame_
                                              : gamma_ramp_256_entry_table_current_frame_;
        if (gamma_ramp_frame_index_ref == UINT32_MAX) {
          constexpr u32 kGammaRampSize256EntryTable = sizeof(u32) * 256;
          constexpr u32 kGammaRampSizePWL = sizeof(u16) * 2 * 3 * 128;
          constexpr u32 kGammaRampSize = kGammaRampSize256EntryTable + kGammaRampSizePWL;
          u32 gamma_ramp_offset_in_frame = use_pwl_gamma_ramp ? kGammaRampSize256EntryTable : 0;
          u32 gamma_ramp_upload_offset =
              kGammaRampSize * swap_frame_index + gamma_ramp_offset_in_frame;
          u32 gamma_ramp_size =
              use_pwl_gamma_ramp ? kGammaRampSizePWL : kGammaRampSize256EntryTable;
          void* gamma_ramp_frame_upload =
              reinterpret_cast<u8*>(gamma_ramp_upload_mapping_) + gamma_ramp_upload_offset;
          if (std::endian::native != std::endian::little && use_pwl_gamma_ramp) {
            // R16G16 expects base then delta, the registers are 32 bit, so swap 16 bits in each 32.
            auto gamma_ramp_pwl_upload =
                reinterpret_cast<xenos::reg::DC_LUT_PWL_DATA*>(gamma_ramp_frame_upload);
            const xenos::reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = GetGammaRampPwlRgb();
            for (size_t i = 0; i < size_t{128} * 3; ++i) {
              xenos::reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_entry = gamma_ramp_pwl_upload[i];
              xenos::reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(gamma_ramp_frame_upload,
                        use_pwl_gamma_ramp ? static_cast<const void*>(GetGammaRampPwlRgb())
                                           : static_cast<const void*>(GetGammaRamp256EntryTable()),
                        gamma_ramp_size);
          }
          bool gamma_ramp_has_upload_buffer = gamma_ramp_upload_buffer_memory_ != VK_NULL_HANDLE;
          VkPipelineStageFlags gamma_ramp_read_stage_mask =
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
          gpu::vulkan::objects::FlushMappedMemoryRange(
              vulkan_device,
              gamma_ramp_has_upload_buffer ? gamma_ramp_upload_buffer_memory_
                                           : gamma_ramp_buffer_memory_,
              gamma_ramp_upload_memory_type_, gamma_ramp_upload_offset,
              gamma_ramp_upload_memory_size_, gamma_ramp_size);
          if (gamma_ramp_has_upload_buffer) {
            PushBufferMemoryBarrier(gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                                    gamma_ramp_read_stage_mask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
            SubmitBarriers(true);
            VkBufferCopy gamma_ramp_buffer_copy;
            gamma_ramp_buffer_copy.srcOffset = gamma_ramp_upload_offset;
            gamma_ramp_buffer_copy.dstOffset = gamma_ramp_offset_in_frame;
            gamma_ramp_buffer_copy.size = gamma_ramp_size;
            deferred_command_buffer_.CmdVkCopyBuffer(gamma_ramp_upload_buffer_, gamma_ramp_buffer_,
                                                     1, &gamma_ramp_buffer_copy);
            PushBufferMemoryBarrier(gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, gamma_ramp_read_stage_mask,
                                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
          }
          // A device local buffer that is not host visible has no per frame ramp sets.
          gamma_ramp_frame_index_ref = gamma_ramp_has_upload_buffer ? 0 : swap_frame_index;
        }

        VkDescriptorSet swap_descriptor_source = swap_descriptors_source_[swap_frame_index];
        VkDescriptorImageInfo swap_descriptor_source_image_info;
        swap_descriptor_source_image_info.sampler = VK_NULL_HANDLE;
        swap_descriptor_source_image_info.imageView = swap_texture_view;
        swap_descriptor_source_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet swap_descriptor_source_write;
        swap_descriptor_source_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        swap_descriptor_source_write.pNext = nullptr;
        swap_descriptor_source_write.dstSet = swap_descriptor_source;
        swap_descriptor_source_write.dstBinding = 0;
        swap_descriptor_source_write.dstArrayElement = 0;
        swap_descriptor_source_write.descriptorCount = 1;
        swap_descriptor_source_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        swap_descriptor_source_write.pImageInfo = &swap_descriptor_source_image_info;
        swap_descriptor_source_write.pBufferInfo = nullptr;
        swap_descriptor_source_write.pTexelBufferView = nullptr;
        dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_source_write, 0, nullptr);

        VkImageSubresourceRange guest_output_subresource_range =
            gpu::vulkan::objects::InitializeSubresourceRange();
        if (use_compute_gamma) {
          // Contents are fully overwritten, so the old layout is always UNDEFINED.
          PushImageMemoryBarrier(vulkan_context.GetImage(), guest_output_subresource_range,
                                 vulkan_context.GetImageEverWrittenPreviously()
                                     ? gpu::vulkan::VulkanPresenter::kGuestOutputInternalStageMask
                                     : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 vulkan_context.GetImageEverWrittenPreviously()
                                     ? gpu::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask
                                     : 0,
                                 VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_GENERAL);
          if (use_fxaa) {
            PushImageMemoryBarrier(swap_fxaa_source_image_, guest_output_subresource_range,
                                   swap_fxaa_source_stage_mask_ ? swap_fxaa_source_stage_mask_
                                                                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   swap_fxaa_source_access_mask_, VK_ACCESS_SHADER_WRITE_BIT,
                                   swap_fxaa_source_layout_, VK_IMAGE_LAYOUT_GENERAL);
          }
          SubmitBarriers(true);

          if (use_fxaa) {
            swap_fxaa_source_stage_mask_ = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            swap_fxaa_source_access_mask_ = VK_ACCESS_SHADER_WRITE_BIT;
            swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_GENERAL;
            swap_fxaa_source_image_submission_ = GetCurrentSubmission();
          }

          VkDescriptorSet swap_descriptor_destination_storage =
              swap_descriptors_destination_storage_[swap_frame_index];
          VkDescriptorImageInfo swap_descriptor_destination_storage_image_info;
          swap_descriptor_destination_storage_image_info.sampler = VK_NULL_HANDLE;
          swap_descriptor_destination_storage_image_info.imageView =
              use_fxaa ? swap_fxaa_source_image_view_ : vulkan_context.GetImageView();
          swap_descriptor_destination_storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
          VkWriteDescriptorSet swap_descriptor_destination_storage_write;
          swap_descriptor_destination_storage_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          swap_descriptor_destination_storage_write.pNext = nullptr;
          swap_descriptor_destination_storage_write.dstSet = swap_descriptor_destination_storage;
          swap_descriptor_destination_storage_write.dstBinding = 0;
          swap_descriptor_destination_storage_write.dstArrayElement = 0;
          swap_descriptor_destination_storage_write.descriptorCount = 1;
          swap_descriptor_destination_storage_write.descriptorType =
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          swap_descriptor_destination_storage_write.pImageInfo =
              &swap_descriptor_destination_storage_image_info;
          swap_descriptor_destination_storage_write.pBufferInfo = nullptr;
          swap_descriptor_destination_storage_write.pTexelBufferView = nullptr;
          dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_destination_storage_write, 0,
                                     nullptr);

          std::array<VkDescriptorSet, kSwapApplyGammaComputeDescriptorSetCount>
              swap_apply_gamma_compute_descriptor_sets{};
          swap_apply_gamma_compute_descriptor_sets[kSwapApplyGammaComputeDescriptorSetRamp] =
              swap_descriptors_gamma_ramp_[2 * gamma_ramp_frame_index_ref +
                                           static_cast<u32>(use_pwl_gamma_ramp)];
          swap_apply_gamma_compute_descriptor_sets[kSwapApplyGammaComputeDescriptorSetSource] =
              swap_descriptor_source;
          swap_apply_gamma_compute_descriptor_sets[kSwapApplyGammaComputeDescriptorSetDestination] =
              swap_descriptor_destination_storage;
          deferred_command_buffer_.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_COMPUTE, swap_apply_gamma_compute_pipeline_layout_, 0,
              static_cast<u32>(swap_apply_gamma_compute_descriptor_sets.size()),
              swap_apply_gamma_compute_descriptor_sets.data(), 0, nullptr);
          SwapApplyGammaConstants swap_apply_gamma_constants = {
              {guest_output_width, guest_output_height}};
          deferred_command_buffer_.CmdVkPushConstants(
              swap_apply_gamma_compute_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
              sizeof(swap_apply_gamma_constants), &swap_apply_gamma_constants);
          BindExternalComputePipeline(swap_apply_gamma_compute_pipeline);
          u32 group_count_x = (guest_output_width + 15) / 16;
          u32 group_count_y = (guest_output_height + 7) / 8;
          deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);

          if (use_fxaa) {
            PushImageMemoryBarrier(swap_fxaa_source_image_, guest_output_subresource_range,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            SubmitBarriers(true);
            swap_fxaa_source_stage_mask_ = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            swap_fxaa_source_access_mask_ = VK_ACCESS_SHADER_READ_BIT;
            swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            swap_fxaa_source_image_submission_ = GetCurrentSubmission();

            VkDescriptorSet swap_descriptor_fxaa_source =
                swap_descriptors_fxaa_source_[swap_frame_index];
            VkDescriptorImageInfo swap_descriptor_fxaa_source_image_info;
            swap_descriptor_fxaa_source_image_info.sampler = swap_sampler_linear_clamp_;
            swap_descriptor_fxaa_source_image_info.imageView = swap_fxaa_source_image_view_;
            swap_descriptor_fxaa_source_image_info.imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet swap_descriptor_fxaa_source_write;
            swap_descriptor_fxaa_source_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            swap_descriptor_fxaa_source_write.pNext = nullptr;
            swap_descriptor_fxaa_source_write.dstSet = swap_descriptor_fxaa_source;
            swap_descriptor_fxaa_source_write.dstBinding = 0;
            swap_descriptor_fxaa_source_write.dstArrayElement = 0;
            swap_descriptor_fxaa_source_write.descriptorCount = 1;
            swap_descriptor_fxaa_source_write.descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            swap_descriptor_fxaa_source_write.pImageInfo = &swap_descriptor_fxaa_source_image_info;
            swap_descriptor_fxaa_source_write.pBufferInfo = nullptr;
            swap_descriptor_fxaa_source_write.pTexelBufferView = nullptr;
            dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_fxaa_source_write, 0, nullptr);

            VkDescriptorSet swap_descriptor_fxaa_destination_storage =
                swap_descriptors_fxaa_destination_storage_[swap_frame_index];
            VkDescriptorImageInfo swap_descriptor_fxaa_destination_storage_image_info;
            swap_descriptor_fxaa_destination_storage_image_info.sampler = VK_NULL_HANDLE;
            swap_descriptor_fxaa_destination_storage_image_info.imageView =
                vulkan_context.GetImageView();
            swap_descriptor_fxaa_destination_storage_image_info.imageLayout =
                VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet swap_descriptor_fxaa_destination_storage_write =
                swap_descriptor_destination_storage_write;
            swap_descriptor_fxaa_destination_storage_write.dstSet =
                swap_descriptor_fxaa_destination_storage;
            swap_descriptor_fxaa_destination_storage_write.pImageInfo =
                &swap_descriptor_fxaa_destination_storage_image_info;
            dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_fxaa_destination_storage_write,
                                       0, nullptr);

            std::array<VkDescriptorSet, kSwapFxaaDescriptorSetCount> swap_fxaa_descriptor_sets{};
            swap_fxaa_descriptor_sets[kSwapFxaaDescriptorSetSource] = swap_descriptor_fxaa_source;
            swap_fxaa_descriptor_sets[kSwapFxaaDescriptorSetDestination] =
                swap_descriptor_fxaa_destination_storage;
            deferred_command_buffer_.CmdVkBindDescriptorSets(
                VK_PIPELINE_BIND_POINT_COMPUTE, swap_fxaa_pipeline_layout_, 0,
                static_cast<u32>(swap_fxaa_descriptor_sets.size()),
                swap_fxaa_descriptor_sets.data(), 0, nullptr);
            SwapFxaaConstants swap_fxaa_constants = {
                {guest_output_width, guest_output_height},
                {1.0f / static_cast<f32>(guest_output_width),
                 1.0f / static_cast<f32>(guest_output_height)}};
            deferred_command_buffer_.CmdVkPushConstants(
                swap_fxaa_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(swap_fxaa_constants), &swap_fxaa_constants);
            BindExternalComputePipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme
                                            ? swap_fxaa_extreme_pipeline_
                                            : swap_fxaa_pipeline_);
            deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);
          }

          PushImageMemoryBarrier(vulkan_context.GetImage(), guest_output_subresource_range,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 gpu::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                 VK_ACCESS_SHADER_WRITE_BIT,
                                 gpu::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                                 VK_IMAGE_LAYOUT_GENERAL,
                                 gpu::vulkan::VulkanPresenter::kGuestOutputInternalLayout);
        } else {
          size_t swap_framebuffer_index = SIZE_MAX;
          size_t swap_framebuffer_new_index = SIZE_MAX;
          for (size_t i = 0; i < swap_framebuffers_.size(); ++i) {
            const SwapFramebuffer& existing_swap_framebuffer = swap_framebuffers_[i];
            if (existing_swap_framebuffer.framebuffer != VK_NULL_HANDLE &&
                existing_swap_framebuffer.version == guest_output_image_version) {
              swap_framebuffer_index = i;
              break;
            }
            if (existing_swap_framebuffer.framebuffer == VK_NULL_HANDLE ||
                !existing_swap_framebuffer.last_submission) {
              swap_framebuffer_new_index = i;
            }
          }
          if (swap_framebuffer_index == SIZE_MAX) {
            if (swap_framebuffer_new_index == SIZE_MAX) {
              swap_framebuffer_new_index = 0;
              for (size_t i = 1; i < swap_framebuffers_.size(); ++i) {
                if (swap_framebuffers_[i].last_submission <
                    swap_framebuffers_[swap_framebuffer_new_index].last_submission) {
                  swap_framebuffer_new_index = i;
                }
              }
            }
            swap_framebuffer_index = swap_framebuffer_new_index;
            SwapFramebuffer& new_swap_framebuffer = swap_framebuffers_[swap_framebuffer_new_index];
            if (new_swap_framebuffer.framebuffer != VK_NULL_HANDLE) {
              if (submission_completed_ >= new_swap_framebuffer.last_submission) {
                dfn.vkDestroyFramebuffer(device, new_swap_framebuffer.framebuffer, nullptr);
              } else {
                destroy_framebuffers_.emplace_back(new_swap_framebuffer.last_submission,
                                                   new_swap_framebuffer.framebuffer);
              }
              new_swap_framebuffer.framebuffer = VK_NULL_HANDLE;
            }
            VkImageView guest_output_image_view = vulkan_context.GetImageView();
            VkFramebufferCreateInfo swap_framebuffer_create_info;
            swap_framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            swap_framebuffer_create_info.pNext = nullptr;
            swap_framebuffer_create_info.flags = 0;
            swap_framebuffer_create_info.renderPass = swap_apply_gamma_render_pass_;
            swap_framebuffer_create_info.attachmentCount = 1;
            swap_framebuffer_create_info.pAttachments = &guest_output_image_view;
            swap_framebuffer_create_info.width = guest_output_width;
            swap_framebuffer_create_info.height = guest_output_height;
            swap_framebuffer_create_info.layers = 1;
            if (dfn.vkCreateFramebuffer(device, &swap_framebuffer_create_info, nullptr,
                                        &new_swap_framebuffer.framebuffer) != VK_SUCCESS) {
              REX_FAIL(gpu, false, "Failed to create the Vulkan framebuffer for presentation");
            }
            new_swap_framebuffer.version = guest_output_image_version;
            // The submission index is set once the framebuffer is used.
            new_swap_framebuffer.last_submission = 0;
          }

          if (vulkan_context.GetImageEverWrittenPreviously()) {
            // oldLayout is UNDEFINED since contents are overwritten, but newLayout cannot be.
            PushImageMemoryBarrier(vulkan_context.GetImage(), guest_output_subresource_range,
                                   gpu::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   gpu::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
          }

          SubmitBarriers(true);

          SwapFramebuffer& swap_framebuffer = swap_framebuffers_[swap_framebuffer_index];
          swap_framebuffer.last_submission = GetCurrentSubmission();

          VkRenderPassBeginInfo render_pass_begin_info;
          render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
          render_pass_begin_info.pNext = nullptr;
          render_pass_begin_info.renderPass = swap_apply_gamma_render_pass_;
          render_pass_begin_info.framebuffer = swap_framebuffer.framebuffer;
          render_pass_begin_info.renderArea.offset.x = 0;
          render_pass_begin_info.renderArea.offset.y = 0;
          render_pass_begin_info.renderArea.extent.width = guest_output_width;
          render_pass_begin_info.renderArea.extent.height = guest_output_height;
          render_pass_begin_info.clearValueCount = 0;
          render_pass_begin_info.pClearValues = nullptr;
          deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                        VK_SUBPASS_CONTENTS_INLINE);

          VkViewport viewport;
          viewport.x = 0.0f;
          viewport.y = 0.0f;
          viewport.width = static_cast<f32>(guest_output_width);
          viewport.height = static_cast<f32>(guest_output_height);
          viewport.minDepth = 0.0f;
          viewport.maxDepth = 1.0f;
          SetViewport(viewport);
          VkRect2D scissor;
          scissor.offset.x = 0;
          scissor.offset.y = 0;
          scissor.extent.width = guest_output_width;
          scissor.extent.height = guest_output_height;
          SetScissor(scissor);

          VkPipeline swap_apply_gamma_pipeline = use_pwl_gamma_ramp
                                                     ? swap_apply_gamma_pwl_pipeline_
                                                     : swap_apply_gamma_256_entry_table_pipeline_;
          if (!vulkan_device->GetProperties().imageViewFormatSwizzle && swap_source_needs_rb_swap) {
            VkPipeline swap_apply_gamma_rb_swap_pipeline =
                use_pwl_gamma_ramp ? swap_apply_gamma_pwl_rb_swap_pipeline_
                                   : swap_apply_gamma_256_entry_table_rb_swap_pipeline_;
            if (swap_apply_gamma_rb_swap_pipeline != VK_NULL_HANDLE) {
              swap_apply_gamma_pipeline = swap_apply_gamma_rb_swap_pipeline;
            } else {
              static bool swap_rb_swap_fallback_missing_logged = false;
              if (!swap_rb_swap_fallback_missing_logged) {
                swap_rb_swap_fallback_missing_logged = true;
                REXLOG_WARN(gpu,
                            "Vulkan imageViewFormatSwizzle is unavailable and the "
                            "swap source needs red/blue swizzle, but the graphics "
                            "fallback RB-swap shader pipeline is unavailable");
              }
            }
          }
          BindExternalGraphicsPipeline(swap_apply_gamma_pipeline);

          std::array<VkDescriptorSet, kSwapApplyGammaDescriptorSetCount> swap_descriptor_sets{};
          swap_descriptor_sets[kSwapApplyGammaDescriptorSetRamp] =
              swap_descriptors_gamma_ramp_[2 * gamma_ramp_frame_index_ref +
                                           static_cast<u32>(use_pwl_gamma_ramp)];
          swap_descriptor_sets[kSwapApplyGammaDescriptorSetSource] = swap_descriptor_source;
          deferred_command_buffer_.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, swap_apply_gamma_pipeline_layout_, 0,
              static_cast<u32>(swap_descriptor_sets.size()), swap_descriptor_sets.data(), 0,
              nullptr);

          deferred_command_buffer_.CmdVkDraw(3, 1, 0, 0);
          deferred_command_buffer_.CmdVkEndRenderPass();

          PushImageMemoryBarrier(vulkan_context.GetImage(), guest_output_subresource_range,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 gpu::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 gpu::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 gpu::vulkan::VulkanPresenter::kGuestOutputInternalLayout);
        }

        // Everything submits before the image returns to the presenter, release barrier included.
        EndSubmission(true);
        return true;
      });

  // The frame ends even without presenting, otherwise per frame resources leak.
  EndSubmission(true);
}

bool VulkanCommandProcessor::PushBufferMemoryBarrier(
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkPipelineStageFlags src_stage_mask,
    VkPipelineStageFlags dst_stage_mask, VkAccessFlags src_access_mask,
    VkAccessFlags dst_access_mask, u32 src_queue_family_index, u32 dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask && src_access_mask == dst_access_mask &&
      src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Overlapping buffer ranges go into separate pipeline barrier commands.
  for (const VkBufferMemoryBarrier& other_buffer_memory_barrier :
       pending_barriers_buffer_memory_barriers_) {
    if (other_buffer_memory_barrier.buffer != buffer ||
        (size != VK_WHOLE_SIZE && offset + size <= other_buffer_memory_barrier.offset) ||
        (other_buffer_memory_barrier.size != VK_WHOLE_SIZE &&
         other_buffer_memory_barrier.offset + other_buffer_memory_barrier.size <= offset)) {
      continue;
    }
    if (other_buffer_memory_barrier.offset == offset && other_buffer_memory_barrier.size == size &&
        other_buffer_memory_barrier.srcAccessMask == src_access_mask &&
        other_buffer_memory_barrier.dstAccessMask == dst_access_mask &&
        other_buffer_memory_barrier.srcQueueFamilyIndex == src_queue_family_index &&
        other_buffer_memory_barrier.dstQueueFamilyIndex == dst_queue_family_index) {
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  VkBufferMemoryBarrier& buffer_memory_barrier =
      pending_barriers_buffer_memory_barriers_.emplace_back();
  buffer_memory_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  buffer_memory_barrier.pNext = nullptr;
  buffer_memory_barrier.srcAccessMask = src_access_mask;
  buffer_memory_barrier.dstAccessMask = dst_access_mask;
  buffer_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  buffer_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  buffer_memory_barrier.buffer = buffer;
  buffer_memory_barrier.offset = offset;
  buffer_memory_barrier.size = size;
  return true;
}

bool VulkanCommandProcessor::PushImageMemoryBarrier(
    VkImage image, const VkImageSubresourceRange& subresource_range,
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask, VkImageLayout old_layout,
    VkImageLayout new_layout, u32 src_queue_family_index, u32 dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask && src_access_mask == dst_access_mask &&
      old_layout == new_layout && src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Overlapping image subresource ranges go into separate pipeline barrier commands.
  for (const VkImageMemoryBarrier& other_image_memory_barrier :
       pending_barriers_image_memory_barriers_) {
    if (other_image_memory_barrier.image != image ||
        !(other_image_memory_barrier.subresourceRange.aspectMask & subresource_range.aspectMask) ||
        (subresource_range.levelCount != VK_REMAINING_MIP_LEVELS &&
         subresource_range.baseMipLevel + subresource_range.levelCount <=
             other_image_memory_barrier.subresourceRange.baseMipLevel) ||
        (other_image_memory_barrier.subresourceRange.levelCount != VK_REMAINING_MIP_LEVELS &&
         other_image_memory_barrier.subresourceRange.baseMipLevel +
                 other_image_memory_barrier.subresourceRange.levelCount <=
             subresource_range.baseMipLevel) ||
        (subresource_range.layerCount != VK_REMAINING_ARRAY_LAYERS &&
         subresource_range.baseArrayLayer + subresource_range.layerCount <=
             other_image_memory_barrier.subresourceRange.baseArrayLayer) ||
        (other_image_memory_barrier.subresourceRange.layerCount != VK_REMAINING_ARRAY_LAYERS &&
         other_image_memory_barrier.subresourceRange.baseArrayLayer +
                 other_image_memory_barrier.subresourceRange.layerCount <=
             subresource_range.baseArrayLayer)) {
      continue;
    }
    if (other_image_memory_barrier.subresourceRange.aspectMask == subresource_range.aspectMask &&
        other_image_memory_barrier.subresourceRange.baseMipLevel ==
            subresource_range.baseMipLevel &&
        other_image_memory_barrier.subresourceRange.levelCount == subresource_range.levelCount &&
        other_image_memory_barrier.subresourceRange.baseArrayLayer ==
            subresource_range.baseArrayLayer &&
        other_image_memory_barrier.subresourceRange.layerCount == subresource_range.layerCount &&
        other_image_memory_barrier.srcAccessMask == src_access_mask &&
        other_image_memory_barrier.dstAccessMask == dst_access_mask &&
        other_image_memory_barrier.oldLayout == old_layout &&
        other_image_memory_barrier.newLayout == new_layout &&
        other_image_memory_barrier.srcQueueFamilyIndex == src_queue_family_index &&
        other_image_memory_barrier.dstQueueFamilyIndex == dst_queue_family_index) {
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  VkImageMemoryBarrier& image_memory_barrier =
      pending_barriers_image_memory_barriers_.emplace_back();
  image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  image_memory_barrier.pNext = nullptr;
  image_memory_barrier.srcAccessMask = src_access_mask;
  image_memory_barrier.dstAccessMask = dst_access_mask;
  image_memory_barrier.oldLayout = old_layout;
  image_memory_barrier.newLayout = new_layout;
  image_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  image_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  image_memory_barrier.image = image;
  image_memory_barrier.subresourceRange = subresource_range;
  return true;
}

bool VulkanCommandProcessor::SubmitBarriers(bool force_end_render_pass) {
  REX_ASSERT(submission_open_);
  SplitPendingBarrier();
  if (pending_barriers_.empty()) {
    if (force_end_render_pass) {
      EndRenderPass();
    }
    return false;
  }
  EndRenderPass();
  for (auto it = pending_barriers_.cbegin(); it != pending_barriers_.cend(); ++it) {
    auto it_next = std::next(it);
    bool is_last = it_next == pending_barriers_.cend();
    // .data() + offset, not &[offset], because offsets can equal the vector sizes.
    deferred_command_buffer_.CmdVkPipelineBarrier(
        it->src_stage_mask ? it->src_stage_mask : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        it->dst_stage_mask ? it->dst_stage_mask : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
        nullptr,
        static_cast<u32>((is_last ? pending_barriers_buffer_memory_barriers_.size()
                                  : it_next->buffer_memory_barriers_offset) -
                         it->buffer_memory_barriers_offset),
        pending_barriers_buffer_memory_barriers_.data() + it->buffer_memory_barriers_offset,
        static_cast<u32>((is_last ? pending_barriers_image_memory_barriers_.size()
                                  : it_next->image_memory_barriers_offset) -
                         it->image_memory_barriers_offset),
        pending_barriers_image_memory_barriers_.data() + it->image_memory_barriers_offset);
  }
  pending_barriers_.clear();
  pending_barriers_buffer_memory_barriers_.clear();
  pending_barriers_image_memory_barriers_.clear();
  current_pending_barrier_.buffer_memory_barriers_offset = 0;
  current_pending_barrier_.image_memory_barriers_offset = 0;
  return true;
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer) {
  SubmitBarriers(false);
  const gpu::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering = REXCVAR_GET(::rex::gpu::cvars::vulkan_dynamic_rendering) &&
                               vulkan_device->GetProperties().dynamicRendering;

  if (use_dynamic_rendering) {
    if (in_render_pass_ && current_framebuffer_ == framebuffer &&
        current_render_pass_ == VK_NULL_HANDLE) {
      return;
    }
  } else {
    if (current_render_pass_ == render_pass && current_framebuffer_ == framebuffer) {
      return;
    }
  }

  if (in_render_pass_) {
    if (use_dynamic_rendering) {
      deferred_command_buffer_.CmdVkEndRendering();
    } else {
      deferred_command_buffer_.CmdVkEndRenderPass();
    }
    in_render_pass_ = false;
  }

  current_render_pass_ = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  current_framebuffer_ = framebuffer;

  if (use_dynamic_rendering) {
    VkRenderingAttachmentInfo color_attachments[xenos::kMaxColorRenderTargets];
    VkRenderingAttachmentInfo depth_attachment;
    VkRenderingAttachmentInfo stencil_attachment;
    u32 color_attachment_count = 0;
    render_target_cache_->GetLastUpdateRenderingAttachments(
        color_attachments, &color_attachment_count, &depth_attachment, &stencil_attachment);
    bool has_depth = depth_attachment.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    bool has_stencil = stencil_attachment.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.pNext = nullptr;
    rendering_info.flags = 0;
    rendering_info.renderArea.offset.x = 0;
    rendering_info.renderArea.offset.y = 0;
    rendering_info.renderArea.extent = framebuffer->host_extent;
    rendering_info.layerCount = 1;
    rendering_info.viewMask = 0;
    rendering_info.colorAttachmentCount = color_attachment_count;
    rendering_info.pColorAttachments = color_attachment_count ? color_attachments : nullptr;
    rendering_info.pDepthAttachment = has_depth ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment = has_stencil ? &stencil_attachment : nullptr;
    deferred_command_buffer_.CmdVkBeginRendering(&rendering_info);
  } else {
    VkRenderPassBeginInfo render_pass_begin_info;
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = nullptr;
    render_pass_begin_info.renderPass = render_pass;
    render_pass_begin_info.framebuffer = framebuffer->framebuffer;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
    render_pass_begin_info.clearValueCount = 0;
    render_pass_begin_info.pClearValues = nullptr;
    deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                  VK_SUBPASS_CONTENTS_INLINE);
  }
  in_render_pass_ = true;
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer,
    VkImageView transfer_dest_view, bool transfer_dest_is_depth) {
  SubmitBarriers(false);
  const gpu::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering = REXCVAR_GET(::rex::gpu::cvars::vulkan_dynamic_rendering) &&
                               vulkan_device->GetProperties().dynamicRendering;

  if (use_dynamic_rendering) {
    if (in_render_pass_ && current_framebuffer_ == framebuffer &&
        current_render_pass_ == VK_NULL_HANDLE) {
      return;
    }
  } else {
    if (current_render_pass_ == render_pass && current_framebuffer_ == framebuffer) {
      return;
    }
  }

  if (in_render_pass_) {
    if (use_dynamic_rendering) {
      deferred_command_buffer_.CmdVkEndRendering();
    } else {
      deferred_command_buffer_.CmdVkEndRenderPass();
    }
    in_render_pass_ = false;
  }

  current_render_pass_ = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  current_framebuffer_ = framebuffer;

  if (use_dynamic_rendering) {
    VkRenderingAttachmentInfo color_attachment = {};
    VkRenderingAttachmentInfo depth_attachment = {};
    VkRenderingAttachmentInfo stencil_attachment = {};

    if (transfer_dest_is_depth) {
      depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      depth_attachment.pNext = nullptr;
      depth_attachment.imageView = transfer_dest_view;
      depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      depth_attachment.resolveImageView = VK_NULL_HANDLE;
      depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      depth_attachment.clearValue = {};
      stencil_attachment = depth_attachment;
    } else {
      color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_attachment.pNext = nullptr;
      color_attachment.imageView = transfer_dest_view;
      color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      color_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      color_attachment.resolveImageView = VK_NULL_HANDLE;
      color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_attachment.clearValue = {};
    }

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.pNext = nullptr;
    rendering_info.flags = 0;
    rendering_info.renderArea.offset.x = 0;
    rendering_info.renderArea.offset.y = 0;
    rendering_info.renderArea.extent = framebuffer->host_extent;
    rendering_info.layerCount = 1;
    rendering_info.viewMask = 0;
    rendering_info.colorAttachmentCount = transfer_dest_is_depth ? 0 : 1;
    rendering_info.pColorAttachments = transfer_dest_is_depth ? nullptr : &color_attachment;
    rendering_info.pDepthAttachment = transfer_dest_is_depth ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment = transfer_dest_is_depth ? &stencil_attachment : nullptr;
    deferred_command_buffer_.CmdVkBeginRendering(&rendering_info);
  } else {
    VkRenderPassBeginInfo render_pass_begin_info;
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = nullptr;
    render_pass_begin_info.renderPass = render_pass;
    render_pass_begin_info.framebuffer = framebuffer->framebuffer;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
    render_pass_begin_info.clearValueCount = 0;
    render_pass_begin_info.pClearValues = nullptr;
    deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                  VK_SUBPASS_CONTENTS_INLINE);
  }
  in_render_pass_ = true;
}

void VulkanCommandProcessor::EndRenderPass() {
  REX_ASSERT(submission_open_);
  if (!in_render_pass_) {
    return;
  }
  if (current_render_pass_ == VK_NULL_HANDLE) {
    deferred_command_buffer_.CmdVkEndRendering();
  } else {
    deferred_command_buffer_.CmdVkEndRenderPass();
  }
  current_render_pass_ = VK_NULL_HANDLE;
  current_framebuffer_ = nullptr;
  in_render_pass_ = false;
}

VkDescriptorSet VulkanCommandProcessor::AllocateSingleTransientDescriptor(
    SingleTransientDescriptorLayout transient_descriptor_layout) {
  REX_ASSERT(frame_open_);
  VkDescriptorSet descriptor_set;
  std::vector<VkDescriptorSet>& transient_descriptors_free =
      single_transient_descriptors_free_[static_cast<size_t>(transient_descriptor_layout)];
  if (!transient_descriptors_free.empty()) {
    descriptor_set = transient_descriptors_free.back();
    transient_descriptors_free.pop_back();
  } else {
    const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    [[maybe_unused]] const gpu::vulkan::VulkanDevice::Functions& dfn =
        vulkan_device->GetFunctions();
    [[maybe_unused]] const VkDevice device = vulkan_device->GetDevice();
    bool is_storage_buffer =
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageBufferCompute ||
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageBufferPairCompute;
    gpu::vulkan::LinkedTypeDescriptorSetAllocator& transient_descriptor_allocator =
        is_storage_buffer ? transient_descriptor_allocator_storage_buffer_
                          : transient_descriptor_allocator_uniform_buffer_;
    VkDescriptorPoolSize descriptor_count;
    descriptor_count.type =
        is_storage_buffer ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor_count.descriptorCount =
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageBufferPairCompute
            ? 2
            : 1;
    descriptor_set = transient_descriptor_allocator.Allocate(
        GetSingleTransientDescriptorLayout(transient_descriptor_layout), &descriptor_count, 1);
    if (descriptor_set == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
  }
  UsedSingleTransientDescriptor used_descriptor;
  used_descriptor.frame = frame_current_;
  used_descriptor.layout = transient_descriptor_layout;
  used_descriptor.set = descriptor_set;
  single_transient_descriptors_used_.emplace_back(used_descriptor);
  return descriptor_set;
}

VkDescriptorSetLayout VulkanCommandProcessor::GetTextureDescriptorSetLayout(bool is_vertex,
                                                                            size_t texture_count,
                                                                            size_t sampler_count) {
  size_t binding_count = texture_count + sampler_count;
  if (!binding_count) {
    return descriptor_set_layout_empty_;
  }

  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = static_cast<u32>(texture_count);
  texture_descriptor_set_layout_key.sampler_count = static_cast<u32>(sampler_count);
  texture_descriptor_set_layout_key.is_vertex = static_cast<u32>(is_vertex);
  auto it_existing = descriptor_set_layouts_textures_.find(texture_descriptor_set_layout_key);
  if (it_existing != descriptor_set_layouts_textures_.end()) {
    return it_existing->second;
  }

  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  descriptor_set_layout_bindings_.clear();
  descriptor_set_layout_bindings_.reserve(binding_count);
  VkShaderStageFlags stage_flags =
      is_vertex ? guest_shader_vertex_stages_ : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (size_t i = 0; i < texture_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = static_cast<u32>(i);
    descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  for (size_t i = 0; i < sampler_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = static_cast<u32>(texture_count + i);
    descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = static_cast<u32>(binding_count);
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings_.data();
  VkDescriptorSetLayout texture_descriptor_set_layout;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &texture_descriptor_set_layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  descriptor_set_layouts_textures_.emplace(texture_descriptor_set_layout_key,
                                           texture_descriptor_set_layout);
  return texture_descriptor_set_layout;
}

const VulkanPipelineCache::PipelineLayoutProvider* VulkanCommandProcessor::GetPipelineLayout(
    size_t texture_count_pixel, size_t sampler_count_pixel, size_t texture_count_vertex,
    size_t sampler_count_vertex) {
  PipelineLayoutKey pipeline_layout_key;
  pipeline_layout_key.texture_count_pixel = static_cast<u16>(texture_count_pixel);
  pipeline_layout_key.sampler_count_pixel = static_cast<u16>(sampler_count_pixel);
  pipeline_layout_key.texture_count_vertex = static_cast<u16>(texture_count_vertex);
  pipeline_layout_key.sampler_count_vertex = static_cast<u16>(sampler_count_vertex);
  {
    auto it = pipeline_layouts_.find(pipeline_layout_key);
    if (it != pipeline_layouts_.end()) {
      return &it->second;
    }
  }

  VkDescriptorSetLayout descriptor_set_layout_textures_vertex =
      GetTextureDescriptorSetLayout(true, texture_count_vertex, sampler_count_vertex);
  if (descriptor_set_layout_textures_vertex == VK_NULL_HANDLE) {
    REX_FAIL(gpu, nullptr,
             "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
             "and {} samplers for guest vertex shaders",
             texture_count_vertex, sampler_count_vertex);
  }
  VkDescriptorSetLayout descriptor_set_layout_textures_pixel =
      GetTextureDescriptorSetLayout(false, texture_count_pixel, sampler_count_pixel);
  if (descriptor_set_layout_textures_pixel == VK_NULL_HANDLE) {
    REX_FAIL(gpu, nullptr,
             "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
             "and {} samplers for guest pixel shaders",
             texture_count_pixel, sampler_count_pixel);
  }

  VkDescriptorSetLayout descriptor_set_layouts[SPIRVShaderTranslator::kDescriptorSetCount];
  descriptor_set_layouts[SPIRVShaderTranslator::kDescriptorSetSharedMemoryAndEDRAM] =
      descriptor_set_layout_shared_memory_and_edram_;
  descriptor_set_layouts[SPIRVShaderTranslator::kDescriptorSetConstants] =
      descriptor_set_layout_constants_;
  descriptor_set_layouts[SPIRVShaderTranslator::kDescriptorSetTexturesVertex] =
      descriptor_set_layout_textures_vertex;
  descriptor_set_layouts[SPIRVShaderTranslator::kDescriptorSetTexturesPixel] =
      descriptor_set_layout_textures_pixel;

  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = nullptr;
  pipeline_layout_create_info.flags = 0;
  pipeline_layout_create_info.setLayoutCount =
      static_cast<u32>(rex::CountOf(descriptor_set_layouts));
  pipeline_layout_create_info.pSetLayouts = descriptor_set_layouts;
  pipeline_layout_create_info.pushConstantRangeCount = 0;
  pipeline_layout_create_info.pPushConstantRanges = nullptr;
  VkPipelineLayout pipeline_layout;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr, &pipeline_layout) !=
      VK_SUCCESS) {
    REX_FAIL(gpu, nullptr,
             "Failed to create a Vulkan pipeline layout for guest drawing with {} "
             "pixel shader and {} vertex shader textures",
             texture_count_pixel, texture_count_vertex);
  }
  auto emplaced_pair = pipeline_layouts_.emplace(
      std::piecewise_construct, std::forward_as_tuple(pipeline_layout_key),
      std::forward_as_tuple(pipeline_layout, descriptor_set_layout_textures_vertex,
                            descriptor_set_layout_textures_pixel));
  // unordered_map insertion doesn't invalidate element references.
  return &emplaced_pair.first->second;
}

VulkanCommandProcessor::ScratchBufferAcquisition VulkanCommandProcessor::AcquireScratchGPUBuffer(
    VkDeviceSize size, VkPipelineStageFlags initial_stage_mask, VkAccessFlags initial_access_mask) {
  REX_ASSERT(submission_open_);
  REX_ASSERT(!scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || !size) {
    return ScratchBufferAcquisition();
  }

  u64 submission_current = GetCurrentSubmission();

  if (scratch_buffer_ != VK_NULL_HANDLE && size <= scratch_buffer_size_) {
    PushBufferMemoryBarrier(scratch_buffer_, 0, VK_WHOLE_SIZE, scratch_buffer_last_stage_mask_,
                            initial_stage_mask, scratch_buffer_last_access_mask_,
                            initial_access_mask);
    scratch_buffer_last_stage_mask_ = initial_stage_mask;
    scratch_buffer_last_access_mask_ = initial_access_mask;
    scratch_buffer_last_usage_submission_ = submission_current;
    scratch_buffer_used_ = true;
    return ScratchBufferAcquisition(*this, scratch_buffer_, initial_stage_mask,
                                    initial_access_mask);
  }

  size = rex::Align(size, kScratchBufferSizeIncrement);

  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();

  VkDeviceMemory new_scratch_buffer_memory;
  VkBuffer new_scratch_buffer;
  // VK_BUFFER_USAGE_TRANSFER_SRC_BIT and _STORAGE_BUFFER_BIT serve texture loading.
  if (!gpu::vulkan::objects::CreateDedicatedAllocationBuffer(
          vulkan_device, size,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          gpu::vulkan::objects::MemoryPurpose::kDeviceLocal, new_scratch_buffer,
          new_scratch_buffer_memory)) {
    REX_FAIL(gpu, ScratchBufferAcquisition(),
             "VulkanCommandProcessor: Failed to create a {} MB scratch GPU buffer", size >> 20);
  }

  if (submission_completed_ >= scratch_buffer_last_usage_submission_) {
    const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
    const VkDevice device = vulkan_device->GetDevice();
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      dfn.vkDestroyBuffer(device, scratch_buffer_, nullptr);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, scratch_buffer_memory_, nullptr);
    }
  } else {
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      destroy_buffers_.emplace_back(scratch_buffer_last_usage_submission_, scratch_buffer_);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      destroy_memory_.emplace_back(scratch_buffer_last_usage_submission_, scratch_buffer_memory_);
    }
  }

  scratch_buffer_memory_ = new_scratch_buffer_memory;
  scratch_buffer_ = new_scratch_buffer;
  scratch_buffer_size_ = size;
  // Not used yet, no need for a barrier.
  scratch_buffer_last_stage_mask_ = initial_access_mask;
  scratch_buffer_last_access_mask_ = initial_stage_mask;
  scratch_buffer_last_usage_submission_ = submission_current;
  scratch_buffer_used_ = true;
  return ScratchBufferAcquisition(*this, new_scratch_buffer, initial_stage_mask,
                                  initial_access_mask);
}

void VulkanCommandProcessor::BindExternalGraphicsPipeline(VkPipeline pipeline,
                                                          bool keep_dynamic_depth_bias,
                                                          bool keep_dynamic_blend_constants,
                                                          bool keep_dynamic_stencil_mask_ref) {
  if (!keep_dynamic_depth_bias) {
    dynamic_depth_bias_update_needed_ = true;
  }
  if (!keep_dynamic_blend_constants) {
    dynamic_blend_constants_update_needed_ = true;
  }
  if (!keep_dynamic_stencil_mask_ref) {
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
  }
  if (current_external_graphics_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  current_external_graphics_pipeline_ = pipeline;
  current_guest_graphics_pipeline_ = VK_NULL_HANDLE;
  current_guest_graphics_pipeline_layout_ = VK_NULL_HANDLE;
}

void VulkanCommandProcessor::BindExternalComputePipeline(VkPipeline pipeline) {
  if (current_external_compute_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  current_external_compute_pipeline_ = pipeline;
}

void VulkanCommandProcessor::SetViewport(const VkViewport& viewport) {
  if (!dynamic_viewport_update_needed_) {
    dynamic_viewport_update_needed_ |= dynamic_viewport_.x != viewport.x;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.y != viewport.y;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.width != viewport.width;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.height != viewport.height;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.minDepth != viewport.minDepth;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.maxDepth != viewport.maxDepth;
  }
  if (dynamic_viewport_update_needed_) {
    dynamic_viewport_ = viewport;
    deferred_command_buffer_.CmdVkSetViewport(0, 1, &dynamic_viewport_);
    dynamic_viewport_update_needed_ = false;
  }
}

void VulkanCommandProcessor::SetScissor(const VkRect2D& scissor) {
  if (!dynamic_scissor_update_needed_) {
    dynamic_scissor_update_needed_ |= dynamic_scissor_.offset.x != scissor.offset.x;
    dynamic_scissor_update_needed_ |= dynamic_scissor_.offset.y != scissor.offset.y;
    dynamic_scissor_update_needed_ |= dynamic_scissor_.extent.width != scissor.extent.width;
    dynamic_scissor_update_needed_ |= dynamic_scissor_.extent.height != scissor.extent.height;
  }
  if (dynamic_scissor_update_needed_) {
    dynamic_scissor_ = scissor;
    deferred_command_buffer_.CmdVkSetScissor(0, 1, &dynamic_scissor_);
    dynamic_scissor_update_needed_ = false;
  }
}

void VulkanCommandProcessor::OnPrimaryBufferEnd() {
  if (REXCVAR_GET(::rex::gpu::cvars::vulkan_submit_on_primary_buffer_end) && submission_open_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

Shader* VulkanCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                           [[maybe_unused]] u32 guest_address,
                                           const u32* host_address, u32 dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool VulkanCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, u32 index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
#if REXGPU_FINE_GRAINED_DRAW_SCOPES
  REX_PROFILE_SCOPE("gpu");
#endif  // REXGPU_FINE_GRAINED_DRAW_SCOPES

  const xenos::RegisterFile& regs = *register_file_;
  (void)index_buffer_info;
  auto draw_fail = [&](const char* stage) {
    auto vgt_draw_initiator = regs.Get<xenos::reg::VGT_DRAW_INITIATOR>();
    REX_FAIL(gpu, false,
             "Vulkan IssueDraw failed at {} "
             "(prim_type={}, index_count={}, source_select={}, major_mode={}, explicit_major={}, "
             "path_select={}, tess_mode={}, edram_mode={})",
             stage, static_cast<u32>(prim_type), index_count,
             static_cast<u32>(vgt_draw_initiator.source_select),
             static_cast<u32>(vgt_draw_initiator.major_mode), static_cast<u32>(major_mode_explicit),
             static_cast<u32>(regs.Get<xenos::reg::VGT_OUTPUT_PATH_CNTL>().path_select),
             static_cast<u32>(regs.Get<xenos::reg::VGT_HOS_CNTL>().tess_mode),
             static_cast<u32>(regs.Get<xenos::reg::RB_MODECONTROL>().edram_mode));
  };

  xenos::EDRAMMode edram_mode = regs.Get<xenos::reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EDRAMMode::kCopy) {
    return IssueCopy();
  }

  bool surface_pitch_is_zero = regs.Get<xenos::reg::RB_SURFACE_INFO>().surface_pitch == 0;

  const gpu::vulkan::VulkanDevice::Properties& device_properties =
      GetVulkanDevice()->GetProperties();

  memexport_ranges_.clear();

  auto vertex_shader = static_cast<VulkanShader*>(GetActiveVertexShader());
  if (!vertex_shader) {
    return draw_fail("missing_vertex_shader");
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->GetMemExportEMWritten() != 0;
  if (memexport_used_vertex) {
    if (!device_properties.vertexPipelineStoresAndAtomics) {
      REX_FAIL(gpu, false,
               "Vertex shader memexport draw encountered without "
               "vertexPipelineStoresAndAtomics support");
    }
    draw_state::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }

  bool primitive_polygonal = draw_state::IsPrimitivePolygonal(regs);
  bool is_rasterization_done =
      draw_state::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    return true;
  }
  VulkanShader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // The pixel shader is used only in kColorDepth mode, see xenos::EDRAMMode.
    if (edram_mode == xenos::EDRAMMode::kColorDepth) {
      pixel_shader = static_cast<VulkanShader*>(GetActivePixelShader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_state::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // The pipeline cache also requires the pixel shader disabled here.
    if (!memexport_used_vertex) {
      return true;
    }
  }
  bool memexport_used_pixel = pixel_shader && (pixel_shader->GetMemExportEMWritten() != 0);
  if (is_rasterization_done &&
      !draw_state::IsRasterizationDone(regs, primitive_polygonal, memexport_used_pixel)) {
    is_rasterization_done = false;
    pixel_shader = nullptr;
    memexport_used_pixel = false;
    if (!memexport_used_vertex) {
      return true;
    }
  }
  if (memexport_used_pixel) {
    if (!device_properties.fragmentStoresAndAtomics) {
      REX_FAIL(gpu, false,
               "Pixel shader memexport draw encountered without "
               "fragmentStoresAndAtomics support");
    }
    draw_state::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  xenos::reg::RB_DEPTHCONTROL normalized_depth_control =
      draw_state::GetNormalizedDepthControl(regs);

  u32 ps_param_gen_pos = UINT32_MAX;
  u32 interpolator_mask = pixel_shader
                              ? (vertex_shader->GetWritesInterpolators() &
                                 pixel_shader->GetInterpolatorInputMask(
                                     regs.Get<xenos::reg::SQ_PROGRAM_CNTL>(),
                                     regs.Get<xenos::reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos))
                              : 0;

  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  SPIRVShaderTranslator::Modification vertex_shader_modification;
  SPIRVShaderTranslator::Modification pixel_shader_modification;
  VulkanShader::VulkanTranslation* vertex_shader_translation;
  VulkanShader::VulkanTranslation* pixel_shader_translation;
  bool memexport_writes_possible = memexport_used_vertex || memexport_used_pixel;

  // 2 iterations because awaiting a submission on sampler overflow invalidates earlier updates.
  for (u32 i = 0; i < 2; ++i) {
    if (!BeginSubmission(true)) {
      return draw_fail("begin_submission");
    }

    if (!primitive_processor_->Process(primitive_processing_result)) {
      return draw_fail("primitive_processing");
    }
    if (!primitive_processing_result.host_draw_vertex_count) {
      return true;
    }
    if (primitive_processing_result.host_primitive_type == xenos::PrimitiveType::kTriangleFan) {
      REXLOG_ERROR(gpu,
                   "PrimitiveProcessor returned triangle fan for Vulkan draw; expected "
                   "triangle list conversion for D3D12 parity");
      REX_ASSERT_ALWAYS();
      return false;
    }
    Shader::HostVertexShaderType host_vertex_shader_type =
        primitive_processing_result.host_vertex_shader_type;
    if (host_vertex_shader_type != Shader::HostVertexShaderType::kVertex &&
        host_vertex_shader_type != Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        host_vertex_shader_type != Shader::HostVertexShaderType::kRectangleListAsTriangleStrip &&
        !Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
      REX_FAIL(gpu, false, "Unsupported Vulkan host vertex shader type {}",
               static_cast<u32>(host_vertex_shader_type));
    }

    vertex_shader_modification = pipeline_cache_->GetCurrentVertexShaderModification(
        *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask,
        ps_param_gen_pos != UINT32_MAX);
    pixel_shader_modification = pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                                                   *pixel_shader, interpolator_mask,
                                                   ps_param_gen_pos, normalized_depth_control)
                                             : SPIRVShaderTranslator::Modification(0);

    // Translate the shaders now to obtain the sampler bindings.
    vertex_shader_translation = static_cast<VulkanShader::VulkanTranslation*>(
        vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
    pixel_shader_translation =
        pixel_shader ? static_cast<VulkanShader::VulkanTranslation*>(
                           pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value))
                     : nullptr;
    if (!pipeline_cache_->EnsureShadersTranslated(vertex_shader_translation,
                                                  pixel_shader_translation)) {
      return draw_fail("shader_translation");
    }

    // Sampler bindings do not depend on the shader modification, so they repeat on iteration 2.
    u32 samplers_overflowed_count = 0;
    for (u32 j = 0; j < 2; ++j) {
      std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>& shader_samplers =
          j ? current_samplers_pixel_ : current_samplers_vertex_;
      if (!i) {
        shader_samplers.clear();
      }
      const VulkanShader* shader = j ? pixel_shader : vertex_shader;
      if (!shader) {
        continue;
      }
      const std::vector<VulkanShader::SamplerBinding>& shader_sampler_bindings =
          shader->GetSamplerBindingsAfterTranslation();
      if (!i) {
        shader_samplers.reserve(shader_sampler_bindings.size());
        for (const VulkanShader::SamplerBinding& shader_sampler_binding : shader_sampler_bindings) {
          shader_samplers.emplace_back(texture_cache_->GetSamplerParameters(shader_sampler_binding),
                                       VK_NULL_HANDLE);
        }
      }
      for (std::pair<VulkanTextureCache::SamplerParameters, VkSampler>& shader_sampler_pair :
           shader_samplers) {
        // UseSampler repeats on iteration 2 as a split submission needs fresh last usage indices.
        bool sampler_overflowed;
        VkSampler shader_sampler =
            texture_cache_->UseSampler(shader_sampler_pair.first, sampler_overflowed);
        shader_sampler_pair.second = shader_sampler;
        if (shader_sampler == VK_NULL_HANDLE) {
          if (!sampler_overflowed || i) {
            // A second overflow cannot recover, the attempts are limited to 2.
            return draw_fail("sampler_acquisition");
          }
          ++samplers_overflowed_count;
        }
      }
    }
    if (!samplers_overflowed_count) {
      break;
    }
    REX_ASSERT(i == 0);
    // Runs after all UseSampler calls because earlier calls may mark samplers later calls need.
    u64 sampler_overflow_await_submission =
        texture_cache_->GetSubmissionToAwaitOnSamplerOverflow(samplers_overflowed_count);
    REX_ASSERT(sampler_overflow_await_submission <= GetCurrentSubmission());
    CheckSubmissionFenceAndDeviceLoss(sampler_overflow_await_submission);
  }

  u32 normalized_color_mask =
      pixel_shader ? draw_state::GetNormalizedColorMask(regs, pixel_shader->GetWritesColorTargets())
                   : 0;

  // Textures update before most other work because samplers depend on them.
  u32 used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  texture_cache_->RequestTextures(used_texture_mask);

  const VulkanPipelineCache::PipelineLayoutProvider* pipeline_layout_provider;
  if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                    normalized_color_mask, *vertex_shader)) {
    return draw_fail("render_target_update");
  }

  // The pipeline needs the render target cache render pass and translates the shaders.
  VkPipeline pipeline;
  void* pipeline_handle = nullptr;
  if (!pipeline_cache_->ConfigurePipeline(vertex_shader_translation, pixel_shader_translation,
                                          primitive_processing_result, normalized_depth_control,
                                          normalized_color_mask,
                                          render_target_cache_->GetLastUpdateRenderPassKey(),
                                          pipeline, pipeline_layout_provider, &pipeline_handle)) {
    return draw_fail("configure_pipeline");
  }
  bool pipeline_is_placeholder = false;
  // Reload the handle state to see async hot swaps completed since configuration.
  pipeline_cache_->GetPipelineAndLayoutByHandle(pipeline_handle, pipeline, pipeline_layout_provider,
                                                &pipeline_is_placeholder);
  if (REXCVAR_GET(::rex::gpu::cvars::async_shader_compilation) && pipeline_is_placeholder) {
    frame_used_async_placeholder_pipeline_ = true;
    return true;
  }
  if (pipeline == VK_NULL_HANDLE || pipeline_layout_provider == nullptr) {
    return draw_fail("pipeline_lookup");
  }
  if (current_guest_graphics_pipeline_ != pipeline) {
    deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    current_guest_graphics_pipeline_ = pipeline;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
  }

  // A layout change invalidates incompatible descriptor sets before the layout pointer updates.
  auto pipeline_layout = static_cast<const PipelineLayout*>(pipeline_layout_provider);
  if (current_guest_graphics_pipeline_layout_ != pipeline_layout) {
    if (current_guest_graphics_pipeline_layout_) {
      // Pipeline layouts are compatible for set N when set layouts 0 through N are compatible.
      u32 descriptor_sets_kept = static_cast<u32>(SPIRVShaderTranslator::kDescriptorSetCount);
      if (current_guest_graphics_pipeline_layout_->GetDescriptorSetLayoutTexturesVertexRef() !=
          pipeline_layout->GetDescriptorSetLayoutTexturesVertexRef()) {
        descriptor_sets_kept =
            std::min(descriptor_sets_kept,
                     static_cast<u32>(SPIRVShaderTranslator::kDescriptorSetTexturesVertex));
      }
      if (current_guest_graphics_pipeline_layout_->GetDescriptorSetLayoutTexturesPixelRef() !=
          pipeline_layout->GetDescriptorSetLayoutTexturesPixelRef()) {
        descriptor_sets_kept =
            std::min(descriptor_sets_kept,
                     static_cast<u32>(SPIRVShaderTranslator::kDescriptorSetTexturesPixel));
      }
    } else {
      // With no previous pipeline layout all bindings are indeterminate.
      current_graphics_descriptor_sets_bound_up_to_date_ = 0;
    }
    current_guest_graphics_pipeline_layout_ = pipeline_layout;
  }

  bool host_render_targets_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
  u32 draw_resolution_scale_x = texture_cache_->GetDrawResolutionScaleX();
  u32 draw_resolution_scale_y = texture_cache_->GetDrawResolutionScaleY();

  draw_state::ViewportInfo viewport_info;
  // maxViewportDimensions alone suffices since viewportBoundsRange[1] is at least twice as large.
  draw_state::GetHostViewportInfo(
      regs, draw_resolution_scale_x, draw_resolution_scale_y, false,
      device_properties.maxViewportDimensions[0], device_properties.maxViewportDimensions[1], true,
      normalized_depth_control,
      host_render_targets_used && render_target_cache_->GetDepthFloat24ConvertInPixelShader(),
      host_render_targets_used, pixel_shader && pixel_shader->GetWritesDepth(), viewport_info);

  UpdateDynamicState(viewport_info, primitive_polygonal, normalized_depth_control);

  auto vgt_draw_initiator = regs.Get<xenos::reg::VGT_DRAW_INITIATOR>();

  // The shader loads the guest 32 bit vertex index itself when the host lacks 32 bit indices.
  bool shader_32bit_index_dma =
      !device_properties.fullDrawIndexUint32 &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32 &&
      primitive_processing_result.host_vertex_shader_type == Shader::HostVertexShaderType::kVertex;
  if (!device_properties.fullDrawIndexUint32 &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32 &&
      primitive_processing_result.host_vertex_shader_type !=
          Shader::HostVertexShaderType::kVertex) {
    // PrimitiveProcessor must pre-convert this case to kHostConverted host endian 24 bit indices.
    static bool non_vertex_32bit_guest_dma_no_full_uint32_logged = false;
    if (!non_vertex_32bit_guest_dma_no_full_uint32_logged) {
      REXLOG_WARN(gpu,
                  "Vulkan draw has host vertex shader type {} with guest DMA 32-bit "
                  "indices on a device without fullDrawIndexUint32; expected "
                  "PrimitiveProcessor to return host-converted indices",
                  static_cast<u32>(primitive_processing_result.host_vertex_shader_type));
      non_vertex_32bit_guest_dma_no_full_uint32_logged = true;
    }
  }

  UpdateSystemConstantValues(primitive_polygonal, primitive_processing_result,
                             shader_32bit_index_dma, 0, viewport_info, used_texture_mask,
                             normalized_depth_control, normalized_color_mask);

  // Uniform buffers and descriptor sets update after binding the pipeline with the new layout.
  if (!UpdateBindings(vertex_shader, pixel_shader)) {
    return draw_fail("update_bindings");
  }

  const Shader::ConstantRegisterMap& constant_map_vertex = vertex_shader->GetConstantRegisterMap();
  for (u32 i = 0; i < rex::CountOf(constant_map_vertex.vertex_fetch_bitmap); ++i) {
    u32 vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    u32 j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(static_cast<u32>(1) << j);
      u32 vfetch_index = i * 32 + j;
      u64 vfetch_bit = static_cast<u64>(1) << (vfetch_index & 63);
      if (vertex_buffers_in_sync_[vfetch_index >> 6] & vfetch_bit) {
        continue;
      }
      xenos::VertexFetch vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(::rex::gpu::cvars::allow_invalid_fetch_constants)) {
            break;
          }
          REXLOG_WARN(gpu,
                      "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
                      "This is incorrect behavior, but you can try bypassing this by "
                      "setting xenos.allow_invalid_fetch_constants=true.",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
        default:
          REXLOG_WARN(gpu, "Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
      }
      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {
        REX_FAIL(gpu, false,
                 "Failed to request vertex buffer at 0x{:08X} (size {}) in the shared "
                 "memory",
                 vfetch_constant.address << 2, vfetch_constant.size << 2);
      }
      vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
    }
  }

  u32 memexport_extent_start = UINT32_MAX, memexport_extent_end = 0;
  for (const draw_state::MemExportRange& memexport_range : memexport_ranges_) {
    u32 memexport_range_base_bytes = memexport_range.base_address_dwords << 2;
    if (!shared_memory_->RequestRange(memexport_range_base_bytes, memexport_range.size_bytes)) {
      REX_FAIL(gpu, false,
               "Failed to request memexport stream at 0x{:08X} (size {}) in the "
               "shared memory",
               memexport_range_base_bytes, memexport_range.size_bytes);
    }
    memexport_extent_start = std::min(memexport_extent_start, memexport_range_base_bytes);
    memexport_extent_end =
        std::max(memexport_extent_end, memexport_range_base_bytes + memexport_range.size_bytes);
  }
  if (memexport_writes_possible && memexport_ranges_.empty()) {
    if (!shared_memory_->RequestRange(0, SharedMemory::kBufferSize)) {
      REX_FAIL(gpu, false,
               "Failed to request full shared memory residency for unresolved "
               "memexport destinations");
    }
  }

  ScratchBufferAcquisition guest_dma_index_scratch_buffer;
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      !shader_32bit_index_dma && memexport_writes_possible) {
    // Memexporting draws with guest DMA indices read an index snapshot to avoid aliasing.
    VkDeviceSize guest_dma_index_size =
        VkDeviceSize(primitive_processing_result.host_draw_vertex_count) *
        (primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16 ? sizeof(u16)
                                                                                     : sizeof(u32));
    if (guest_dma_index_size) {
      guest_dma_index_scratch_buffer = AcquireScratchGPUBuffer(
          guest_dma_index_size, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      if (guest_dma_index_scratch_buffer.GetBuffer() == VK_NULL_HANDLE) {
        return draw_fail("guest_dma_index_scratch_buffer");
      }

      shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
      SubmitBarriers(true);

      VkBufferCopy guest_dma_index_copy_region = {};
      guest_dma_index_copy_region.srcOffset = primitive_processing_result.guest_index_base;
      guest_dma_index_copy_region.dstOffset = 0;
      guest_dma_index_copy_region.size = guest_dma_index_size;
      deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->GetBuffer(),
                                               guest_dma_index_scratch_buffer.GetBuffer(), 1,
                                               &guest_dma_index_copy_region);
      PushBufferMemoryBarrier(
          guest_dma_index_scratch_buffer.GetBuffer(), 0, guest_dma_index_size,
          guest_dma_index_scratch_buffer.GetStageMask(), VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
          guest_dma_index_scratch_buffer.GetAccessMask(), VK_ACCESS_INDEX_READ_BIT);
      guest_dma_index_scratch_buffer.SetStageMask(VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
      guest_dma_index_scratch_buffer.SetAccessMask(VK_ACCESS_INDEX_READ_BIT);
    }
  }

  if (memexport_extent_start < memexport_extent_end) {
    shared_memory_->Use(
        VulkanSharedMemory::Usage::kGuestDrawReadWrite,
        std::make_pair(memexport_extent_start, memexport_extent_end - memexport_extent_start));
  } else if (memexport_writes_possible) {
    // Stream constants may be invalid or dynamic, so the whole buffer is used.
    shared_memory_->Use(VulkanSharedMemory::Usage::kGuestDrawReadWrite,
                        std::make_pair(0u, SharedMemory::kBufferSize));
  } else {
    shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  }

  // Barriers submit after all commands that may dispatch or copy, then the render pass re-enters.
  SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      render_target_cache_->GetLastUpdateRenderPass(),
      render_target_cache_->GetLastUpdateFramebuffer());

  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kNone ||
      shader_32bit_index_dma) {
    deferred_command_buffer_.CmdVkDraw(primitive_processing_result.host_draw_vertex_count, 1, 0, 0);
  } else {
    std::pair<VkBuffer, VkDeviceSize> index_buffer;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        if (guest_dma_index_scratch_buffer.GetBuffer() != VK_NULL_HANDLE) {
          index_buffer.first = guest_dma_index_scratch_buffer.GetBuffer();
          index_buffer.second = 0;
        } else {
          index_buffer.first = shared_memory_->GetBuffer();
          index_buffer.second = primitive_processing_result.guest_index_base;
        }
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer = primitive_processor_->GetConvertedIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer = primitive_processor_->GetBuiltinIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        REX_UNHANDLED_CASE(primitive_processing_result.index_buffer_type);
        return draw_fail("unexpected_index_buffer_type");
    }
    deferred_command_buffer_.CmdVkBindIndexBuffer(
        index_buffer.first, index_buffer.second,
        primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
            ? VK_INDEX_TYPE_UINT16
            : VK_INDEX_TYPE_UINT32);
    deferred_command_buffer_.CmdVkDrawIndexed(primitive_processing_result.host_draw_vertex_count, 1,
                                              0, 0, 0);
  }

  if (!memexport_ranges_.empty()) {
    for (const draw_state::MemExportRange& memexport_range : memexport_ranges_) {
      shared_memory_->RangeWrittenByGPU(memexport_range.base_address_dwords << 2,
                                        memexport_range.size_bytes);
    }
  } else if (memexport_writes_possible) {
    // Stream constants may be invalid or dynamic, so invalidation covers unknown destinations.
    shared_memory_->RangeWrittenByGPU(0, SharedMemory::kBufferSize);
  }

  if (IsReadbackMemexportEnabled(REXCVAR_GET(::rex::gpu::cvars::vulkan_readback_memexport)) &&
      !memexport_ranges_.empty()) {
    u32 memexport_total_size = 0;
    for (const draw_state::MemExportRange& memexport_range : memexport_ranges_) {
      memexport_total_size += memexport_range.size_bytes;
    }
    if (memexport_total_size) {
      if (REXCVAR_GET(::rex::gpu::cvars::readback_memexport_fast)) {
        IssueDrawMemexportReadbackFastPath(memexport_total_size);
      } else {
        IssueDrawMemexportReadbackFullPath(memexport_total_size);
      }
    }
  }

  return true;
}

bool VulkanCommandProcessor::IssueDrawMemexportReadbackFullPath(u32 total_size) {
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  VkBuffer readback_buffer = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory = VK_NULL_HANDLE;
  u32 readback_memory_type = UINT32_MAX;
  VkDeviceSize readback_memory_size = 0;
  if (!gpu::vulkan::objects::CreateDedicatedAllocationBuffer(
          vulkan_device, total_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          gpu::vulkan::objects::MemoryPurpose::kReadback, readback_buffer, readback_memory,
          &readback_memory_type, &readback_memory_size)) {
    REX_FAIL(gpu, true, "Failed to create a Vulkan memexport readback buffer");
  }

  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  SubmitBarriers(true);

  u32 readback_offset = 0;
  for (const draw_state::MemExportRange& memexport_range : memexport_ranges_) {
    VkBufferCopy readback_region = {};
    readback_region.srcOffset = memexport_range.base_address_dwords << 2;
    readback_region.dstOffset = readback_offset;
    readback_region.size = memexport_range.size_bytes;
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->GetBuffer(), readback_buffer, 1,
                                             &readback_region);
    readback_offset += memexport_range.size_bytes;
  }
  PushBufferMemoryBarrier(readback_buffer, 0, total_size, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_HOST_READ_BIT);

  if (!AwaitAllQueueOperationsCompletion()) {
    REXLOG_ERROR(gpu, "Failed to await completion of Vulkan memexport readback");
    dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
    dfn.vkFreeMemory(device, readback_memory, nullptr);
    return true;
  }

  void* readback_mapping = nullptr;
  if (dfn.vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, &readback_mapping) !=
      VK_SUCCESS) {
    REXLOG_ERROR(gpu, "Failed to map a Vulkan memexport readback buffer");
    dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
    dfn.vkFreeMemory(device, readback_memory, nullptr);
    return true;
  }

  if (!(vulkan_device->GetMemoryTypes().host_coherent &
        (static_cast<u32>(1) << readback_memory_type))) {
    VkMappedMemoryRange readback_memory_range = {};
    readback_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    readback_memory_range.memory = readback_memory;
    readback_memory_range.offset = 0;
    readback_memory_range.size = std::min(
        rex::round_up(VkDeviceSize(total_size), vulkan_device->GetProperties().nonCoherentAtomSize),
        readback_memory_size);
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &readback_memory_range);
  }

  const u8* readback_bytes = reinterpret_cast<const u8*>(readback_mapping);
  for (const draw_state::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }
  dfn.vkUnmapMemory(device, readback_memory);
  dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
  dfn.vkFreeMemory(device, readback_memory, nullptr);
  return true;
}

bool VulkanCommandProcessor::IssueDrawMemexportReadbackFastPath(u32 total_size) {
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  const u64 readback_key =
      BuildMemexportReadbackKey(memexport_ranges_.front().base_address_dwords, total_size);
  ReadbackBuffer& readback = memexport_readback_buffers_[readback_key];
  readback.last_used_frame = frame_current_;

  auto ensure_readback_slot = [&](u32 index, u32 size) -> bool {
    if (readback.buffers[index] != VK_NULL_HANDLE && readback.memories[index] != VK_NULL_HANDLE &&
        readback.mapped_data[index] != nullptr && size <= readback.sizes[index]) {
      return true;
    }

    VkBuffer new_buffer = VK_NULL_HANDLE;
    VkDeviceMemory new_memory = VK_NULL_HANDLE;
    if (!gpu::vulkan::objects::CreateDedicatedAllocationBuffer(
            vulkan_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            gpu::vulkan::objects::MemoryPurpose::kReadback, new_buffer, new_memory)) {
      return false;
    }

    void* new_mapping = nullptr;
    if (dfn.vkMapMemory(device, new_memory, 0, VK_WHOLE_SIZE, 0, &new_mapping) != VK_SUCCESS) {
      dfn.vkDestroyBuffer(device, new_buffer, nullptr);
      dfn.vkFreeMemory(device, new_memory, nullptr);
      return false;
    }

    if (readback.buffers[index] != VK_NULL_HANDLE || readback.memories[index] != VK_NULL_HANDLE) {
      if (!AwaitAllQueueOperationsCompletion()) {
        dfn.vkUnmapMemory(device, new_memory);
        dfn.vkDestroyBuffer(device, new_buffer, nullptr);
        dfn.vkFreeMemory(device, new_memory, nullptr);
        return false;
      }
      if (readback.mapped_data[index] != nullptr && readback.memories[index] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[index]);
      }
      if (readback.buffers[index] != VK_NULL_HANDLE) {
        dfn.vkDestroyBuffer(device, readback.buffers[index], nullptr);
      }
      if (readback.memories[index] != VK_NULL_HANDLE) {
        dfn.vkFreeMemory(device, readback.memories[index], nullptr);
      }
    }

    readback.buffers[index] = new_buffer;
    readback.memories[index] = new_memory;
    readback.mapped_data[index] = new_mapping;
    readback.sizes[index] = size;
    readback.submission_written[index] = 0;
    readback.written_size[index] = 0;
    return true;
  };

  const u32 write_index = readback.current_index;
  const u32 read_index = 1 - write_index;
  const u32 readback_size = AlignReadbackBufferSize(total_size);
  if (!ensure_readback_slot(write_index, readback_size)) {
    return IssueDrawMemexportReadbackFullPath(total_size);
  }

  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  SubmitBarriers(true);

  u32 readback_offset = 0;
  for (const draw_state::MemExportRange& memexport_range : memexport_ranges_) {
    VkBufferCopy readback_region = {};
    readback_region.srcOffset = memexport_range.base_address_dwords << 2;
    readback_region.dstOffset = readback_offset;
    readback_region.size = memexport_range.size_bytes;
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->GetBuffer(),
                                             readback.buffers[write_index], 1, &readback_region);
    readback_offset += memexport_range.size_bytes;
  }
  PushBufferMemoryBarrier(readback.buffers[write_index], 0, total_size,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
  readback.submission_written[write_index] = GetCurrentSubmission();
  readback.written_size[write_index] = total_size;

  CheckSubmissionFenceAndDeviceLoss(0);
  bool previous_slot_ready =
      readback.buffers[read_index] != VK_NULL_HANDLE &&
      readback.memories[read_index] != VK_NULL_HANDLE &&
      readback.mapped_data[read_index] != nullptr && total_size <= readback.sizes[read_index] &&
      total_size <= readback.written_size[read_index] && readback.submission_written[read_index] &&
      readback.submission_written[read_index] <= submission_completed_;
  if (!previous_slot_ready) {
    IssueDrawMemexportReadbackFullPath(total_size);
    readback.current_index = read_index;
    return true;
  }

  VkMappedMemoryRange readback_memory_range = {};
  readback_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  readback_memory_range.memory = readback.memories[read_index];
  readback_memory_range.offset = 0;
  readback_memory_range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(device, 1, &readback_memory_range);

  const u8* readback_bytes = reinterpret_cast<const u8*>(readback.mapped_data[read_index]);
  for (const draw_state::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }

  readback.current_index = read_index;
  return true;
}

bool VulkanCommandProcessor::IssueCopy() {
#if REXGPU_FINE_GRAINED_DRAW_SCOPES
  REX_PROFILE_SCOPE("gpu");
#endif  // REXGPU_FINE_GRAINED_DRAW_SCOPES

  if (!BeginSubmission(true)) {
    return false;
  }

  ReadbackResolveMode readback_mode =
      GetReadbackResolveMode(REXCVAR_GET(::rex::gpu::cvars::vulkan_readback_resolve));
  if (readback_mode == ReadbackResolveMode::kDisabled) {
    u32 written_address, written_length;
    return render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                         written_address, written_length);
  }

  return IssueCopyReadbackResolvePath();
}

bool VulkanCommandProcessor::IssueCopyReadbackResolvePath() {
  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  u32 written_address, written_length;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, written_address,
                                     written_length)) {
    return false;
  }

  if (!written_length) {
    return true;
  }

  if (!memory_->TranslatePhysical(written_address)) {
    return true;
  }

  ReadbackResolveMode readback_mode =
      GetReadbackResolveMode(REXCVAR_GET(::rex::gpu::cvars::vulkan_readback_resolve));
  if (readback_mode == ReadbackResolveMode::kDisabled) {
    return true;
  }

  auto ensure_readback_slot = [&](ReadbackBuffer& readback, u32 index, u32 size) -> bool {
    if (readback.buffers[index] != VK_NULL_HANDLE && size <= readback.sizes[index] &&
        readback.mapped_data[index] != nullptr) {
      return true;
    }

    VkBuffer new_buffer = VK_NULL_HANDLE;
    VkDeviceMemory new_memory = VK_NULL_HANDLE;
    if (!gpu::vulkan::objects::CreateDedicatedAllocationBuffer(
            vulkan_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            gpu::vulkan::objects::MemoryPurpose::kReadback, new_buffer, new_memory)) {
      REX_FAIL(gpu, false, "Failed to create a {} MB Vulkan resolve readback buffer", size >> 20);
    }

    void* new_mapping = nullptr;
    if (dfn.vkMapMemory(device, new_memory, 0, VK_WHOLE_SIZE, 0, &new_mapping) != VK_SUCCESS) {
      REXLOG_ERROR(gpu, "Failed to map a Vulkan resolve readback buffer");
      dfn.vkDestroyBuffer(device, new_buffer, nullptr);
      dfn.vkFreeMemory(device, new_memory, nullptr);
      return false;
    }

    if (readback.buffers[index] != VK_NULL_HANDLE || readback.memories[index] != VK_NULL_HANDLE) {
      if (!AwaitAllQueueOperationsCompletion()) {
        dfn.vkUnmapMemory(device, new_memory);
        dfn.vkDestroyBuffer(device, new_buffer, nullptr);
        dfn.vkFreeMemory(device, new_memory, nullptr);
        return false;
      }
      if (readback.mapped_data[index] != nullptr && readback.memories[index] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[index]);
      }
      if (readback.buffers[index] != VK_NULL_HANDLE) {
        dfn.vkDestroyBuffer(device, readback.buffers[index], nullptr);
      }
      if (readback.memories[index] != VK_NULL_HANDLE) {
        dfn.vkFreeMemory(device, readback.memories[index], nullptr);
      }
    }

    readback.buffers[index] = new_buffer;
    readback.memories[index] = new_memory;
    readback.mapped_data[index] = new_mapping;
    readback.sizes[index] = size;
    return true;
  };

  bool is_scaled = texture_cache_->IsDrawResolutionScaled();
  u64 resolve_key = BuildReadbackResolveKey(written_address, written_length);
  ReadbackBuffer& readback = readback_buffers_[resolve_key];
  readback.last_used_frame = frame_current_;
  u32 write_index = readback.current_index;
  u32 readback_size = AlignReadbackBufferSize(written_length);
  if (!ensure_readback_slot(readback, write_index, readback_size)) {
    return true;
  }

  if (is_scaled) {
    if (!resolve_downscale_pipeline_ || !resolve_downscale_pipeline_layout_) {
      return true;
    }

    xenos::reg::RB_COPY_DEST_INFO copy_dest_info =
        register_file_->Get<xenos::reg::RB_COPY_DEST_INFO>();
    const xenos::FormatInfo* format_info =
        xenos::FormatInfo::Get(static_cast<u32>(copy_dest_info.copy_dest_format));
    u32 bits_per_pixel = format_info->bits_per_pixel;
    if (bits_per_pixel != 8 && bits_per_pixel != 16 && bits_per_pixel != 32 &&
        bits_per_pixel != 64) {
      return true;
    }

    u32 pixel_size_log2;
    if (!rex::bit_scan_forward(bits_per_pixel >> 3, &pixel_size_log2)) {
      return true;
    }
    u32 tile_size_1x = 32 * 32 * (static_cast<u32>(1) << pixel_size_log2);
    u32 tile_count = written_length / tile_size_1x;
    if (!tile_count) {
      return true;
    }

    u64 scaled_start = 0, scaled_length = 0;
    if (!texture_cache_->GetScaledResolveRange(written_address, written_length, 0, scaled_start,
                                               scaled_length)) {
      return true;
    }
    if (!scaled_length) {
      return true;
    }

    VkBuffer scaled_resolve_buffer = texture_cache_->GetScaledResolveBuffer();
    if (scaled_resolve_buffer == VK_NULL_HANDLE || scaled_start > static_cast<u64>(UINT32_MAX)) {
      return true;
    }

    u32 downscale_buffer_size = AlignReadbackBufferSize(written_length);
    if (downscale_buffer_size > resolve_downscale_buffer_size_) {
      VkBuffer new_buffer = VK_NULL_HANDLE;
      VkDeviceMemory new_memory = VK_NULL_HANDLE;
      if (!gpu::vulkan::objects::CreateDedicatedAllocationBuffer(
              vulkan_device, downscale_buffer_size,
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              gpu::vulkan::objects::MemoryPurpose::kDeviceLocal, new_buffer, new_memory)) {
        REX_FAIL(gpu, true, "Failed to create a {} MB Vulkan resolve downscale buffer",
                 downscale_buffer_size >> 20);
      }
      if (resolve_downscale_buffer_ != VK_NULL_HANDLE ||
          resolve_downscale_buffer_memory_ != VK_NULL_HANDLE) {
        if (!AwaitAllQueueOperationsCompletion()) {
          dfn.vkDestroyBuffer(device, new_buffer, nullptr);
          dfn.vkFreeMemory(device, new_memory, nullptr);
          return true;
        }
        if (resolve_downscale_buffer_ != VK_NULL_HANDLE) {
          dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
        }
        if (resolve_downscale_buffer_memory_ != VK_NULL_HANDLE) {
          dfn.vkFreeMemory(device, resolve_downscale_buffer_memory_, nullptr);
        }
      }
      resolve_downscale_buffer_ = new_buffer;
      resolve_downscale_buffer_memory_ = new_memory;
      resolve_downscale_buffer_size_ = downscale_buffer_size;
    }
    if (resolve_downscale_buffer_ == VK_NULL_HANDLE) {
      return true;
    }

    VkDescriptorSet descriptor_set = AllocateSingleTransientDescriptor(
        SingleTransientDescriptorLayout::kStorageBufferPairCompute);
    if (descriptor_set == VK_NULL_HANDLE) {
      return true;
    }

    VkDescriptorBufferInfo buffer_infos[2] = {};
    buffer_infos[0].buffer = scaled_resolve_buffer;
    buffer_infos[0].offset = 0;
    buffer_infos[0].range = VK_WHOLE_SIZE;
    buffer_infos[1].buffer = resolve_downscale_buffer_;
    buffer_infos[1].offset = 0;
    buffer_infos[1].range = written_length;

    VkWriteDescriptorSet descriptor_writes[2] = {};
    for (u32 i = 0; i < 2; ++i) {
      descriptor_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptor_writes[i].dstSet = descriptor_set;
      descriptor_writes[i].dstBinding = i;
      descriptor_writes[i].descriptorCount = 1;
      descriptor_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      descriptor_writes[i].pBufferInfo = &buffer_infos[i];
    }
    dfn.vkUpdateDescriptorSets(device, 2, descriptor_writes, 0, nullptr);

    texture_cache_->UseScaledResolveBufferForRead();
    SubmitBarriers(true);

    VkBufferMemoryBarrier pre_barrier = {};
    pre_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    pre_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    pre_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    pre_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_barrier.buffer = resolve_downscale_buffer_;
    pre_barrier.offset = 0;
    pre_barrier.size = written_length;
    deferred_command_buffer_.CmdVkPipelineBarrier(
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &pre_barrier, 0, nullptr);

    BindExternalComputePipeline(resolve_downscale_pipeline_);

    ResolveDownscaleConstants constants;
    constants.scale_x = texture_cache_->GetDrawResolutionScaleX();
    constants.scale_y = texture_cache_->GetDrawResolutionScaleY();
    constants.pixel_size_log2 = pixel_size_log2;
    constants.tile_count = tile_count;
    constants.source_offset_bytes = static_cast<u32>(scaled_start);
    constants.half_pixel_offset =
        (REXCVAR_GET(::rex::gpu::cvars::readback_resolve_half_pixel_offset) &&
         (constants.scale_x > 1 || constants.scale_y > 1))
            ? 1u
            : 0u;
    deferred_command_buffer_.CmdVkPushConstants(resolve_downscale_pipeline_layout_,
                                                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                                                &constants);
    deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                     resolve_downscale_pipeline_layout_, 0, 1,
                                                     &descriptor_set, 0, nullptr);
    deferred_command_buffer_.CmdVkDispatch(tile_count, 1, 1);

    VkBufferMemoryBarrier downscale_barrier = {};
    downscale_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    downscale_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    downscale_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    downscale_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    downscale_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    downscale_barrier.buffer = resolve_downscale_buffer_;
    downscale_barrier.offset = 0;
    downscale_barrier.size = written_length;
    deferred_command_buffer_.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                                                  &downscale_barrier, 0, nullptr);

    VkBufferCopy readback_region = {};
    readback_region.srcOffset = 0;
    readback_region.dstOffset = 0;
    readback_region.size = written_length;
    deferred_command_buffer_.CmdVkCopyBuffer(resolve_downscale_buffer_,
                                             readback.buffers[write_index], 1, &readback_region);
  } else {
    shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
    SubmitBarriers(true);

    VkBufferCopy readback_region = {};
    readback_region.srcOffset = written_address;
    readback_region.dstOffset = 0;
    readback_region.size = written_length;
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->GetBuffer(),
                                             readback.buffers[write_index], 1, &readback_region);
  }

  PushBufferMemoryBarrier(readback.buffers[write_index], 0, written_length,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);

  bool use_delayed_sync =
      readback_mode == ReadbackResolveMode::kFast || readback_mode == ReadbackResolveMode::kSome;
  u32 read_index = write_index;
  if (use_delayed_sync) {
    read_index = 1 - write_index;
  } else if (!AwaitAllQueueOperationsCompletion()) {
    return true;
  }

  bool is_cache_miss = false;
  if (use_delayed_sync && (readback.buffers[read_index] == VK_NULL_HANDLE ||
                           written_length > readback.sizes[read_index] ||
                           readback.mapped_data[read_index] == nullptr)) {
    is_cache_miss = true;
    read_index = write_index;
    if (!AwaitAllQueueOperationsCompletion()) {
      return true;
    }
  }

  bool should_copy = (readback_mode == ReadbackResolveMode::kSome) ? is_cache_miss : true;
  if (should_copy && readback.buffers[read_index] != VK_NULL_HANDLE &&
      written_length <= readback.sizes[read_index] && readback.mapped_data[read_index] != nullptr) {
    VkMappedMemoryRange readback_memory_range = {};
    readback_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    readback_memory_range.memory = readback.memories[read_index];
    readback_memory_range.offset = 0;
    readback_memory_range.size = VK_WHOLE_SIZE;
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &readback_memory_range);

    u8* destination = memory_->TranslatePhysical(written_address);
    if (destination) {
      std::memcpy(destination, readback.mapped_data[read_index], written_length);
    }
  }

  readback.current_index = 1 - readback.current_index;
  return true;
}

void VulkanCommandProcessor::EvictOldReadbackBuffers(
    std::unordered_map<u64, ReadbackBuffer>& buffer_map) const {
  if (buffer_map.empty()) {
    return;
  }

  const u64 eviction_frame_floor = (frame_current_ > kReadbackBufferEvictionAgeFrames)
                                       ? (frame_current_ - kReadbackBufferEvictionAgeFrames)
                                       : 0;
  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  for (auto it = buffer_map.begin(); it != buffer_map.end();) {
    ReadbackBuffer& readback = it->second;
    bool evict =
        buffer_map.size() > kMaxReadbackBuffers || readback.last_used_frame < eviction_frame_floor;
    if (!evict) {
      ++it;
      continue;
    }
    for (u32 i = 0; i < 2; ++i) {
      if (readback.mapped_data[i] && readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[i]);
      }
      if (readback.buffers[i] != VK_NULL_HANDLE) {
        dfn.vkDestroyBuffer(device, readback.buffers[i], nullptr);
      }
      if (readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkFreeMemory(device, readback.memories[i], nullptr);
      }
      readback.buffers[i] = VK_NULL_HANDLE;
      readback.memories[i] = VK_NULL_HANDLE;
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
    it = buffer_map.erase(it);
  }
}

bool VulkanCommandProcessor::InitializeOcclusionQueryResources() {
  active_occlusion_query_ = {};
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
  occlusion_query_pool_ = VK_NULL_HANDLE;
  occlusion_query_readback_buffer_ = VK_NULL_HANDLE;
  occlusion_query_readback_memory_ = VK_NULL_HANDLE;
  occlusion_query_readback_memory_type_ = UINT32_MAX;
  occlusion_query_readback_memory_size_ = 0;
  occlusion_query_readback_mapping_ = nullptr;

  const gpu::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  VkDevice device = vulkan_device->GetDevice();

  VkQueryPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  pool_info.queryType = VK_QUERY_TYPE_OCCLUSION;
  pool_info.queryCount = kMaxOcclusionQueries;
  if (dfn.vkCreateQueryPool(device, &pool_info, nullptr, &occlusion_query_pool_) != VK_SUCCESS) {
    REXLOG_WARN(
        gpu,
        "VulkanCommandProcessor: Failed to create occlusion query pool, using fake sample counts");
    return false;
  }

  if (!gpu::vulkan::objects::CreateDedicatedAllocationBuffer(
          vulkan_device, sizeof(u64) * kMaxOcclusionQueries, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          gpu::vulkan::objects::MemoryPurpose::kReadback, occlusion_query_readback_buffer_,
          occlusion_query_readback_memory_, &occlusion_query_readback_memory_type_,
          &occlusion_query_readback_memory_size_)) {
    REXLOG_WARN(
        gpu,
        "VulkanCommandProcessor: Failed to create occlusion query readback buffer, using fake "
        "sample counts");
    ShutdownOcclusionQueryResources();
    return false;
  }

  if (dfn.vkMapMemory(device, occlusion_query_readback_memory_, 0, VK_WHOLE_SIZE, 0,
                      reinterpret_cast<void**>(&occlusion_query_readback_mapping_)) != VK_SUCCESS) {
    REXLOG_WARN(
        gpu,
        "VulkanCommandProcessor: Failed to map occlusion query readback buffer, using fake sample "
        "counts");
    ShutdownOcclusionQueryResources();
    return false;
  }

  occlusion_query_resources_available_ = true;
  return true;
}

void VulkanCommandProcessor::ShutdownOcclusionQueryResources() {
  DisableHostOcclusionQueries();

  const gpu::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  VkDevice device = vulkan_device->GetDevice();

  if (occlusion_query_readback_mapping_ && occlusion_query_readback_memory_ != VK_NULL_HANDLE) {
    dfn.vkUnmapMemory(device, occlusion_query_readback_memory_);
  }
  occlusion_query_readback_mapping_ = nullptr;
  if (occlusion_query_readback_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, occlusion_query_readback_buffer_, nullptr);
  }
  if (occlusion_query_readback_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, occlusion_query_readback_memory_, nullptr);
  }
  if (occlusion_query_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyQueryPool(device, occlusion_query_pool_, nullptr);
  }

  occlusion_query_pool_ = VK_NULL_HANDLE;
  occlusion_query_readback_buffer_ = VK_NULL_HANDLE;
  occlusion_query_readback_memory_ = VK_NULL_HANDLE;
  occlusion_query_readback_memory_type_ = UINT32_MAX;
  occlusion_query_readback_memory_size_ = 0;
}

bool VulkanCommandProcessor::AcquireOcclusionQueryIndex(u32& host_index_out) {
  if (occlusion_query_cursor_ >= kMaxOcclusionQueries) {
    occlusion_query_cursor_ = 0;
  }
  host_index_out = occlusion_query_cursor_++;
  return true;
}

void VulkanCommandProcessor::DisableHostOcclusionQueries() {
  if (active_occlusion_query_.valid && occlusion_query_pool_ != VK_NULL_HANDLE) {
    if (BeginSubmission(true)) {
      deferred_command_buffer_.CmdVkEndQuery(occlusion_query_pool_,
                                             active_occlusion_query_.host_index);
      EndSubmission(false);
    }
  }
  active_occlusion_query_ = {};
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
}

bool VulkanCommandProcessor::BeginGuestOcclusionQuery(u32 sample_count_address) {
  if (!REXCVAR_GET(::rex::gpu::cvars::occlusion_query_enable) ||
      !occlusion_query_resources_available_ || occlusion_query_pool_ == VK_NULL_HANDLE ||
      occlusion_query_readback_mapping_ == nullptr) {
    return false;
  }
  if (active_occlusion_query_.valid) {
    REXLOG_WARN(
        gpu, "VulkanCommandProcessor: Occlusion query begin issued while another query is active");
    DisableHostOcclusionQueries();
    return false;
  }

  u32 host_index = 0;
  if (!AcquireOcclusionQueryIndex(host_index)) {
    return false;
  }
  if (!BeginSubmission(true)) {
    return false;
  }

  EndRenderPass();

  DeferredCommandBuffer& command_buffer = GetDeferredCommandBuffer();
  command_buffer.CmdVkResetQueryPool(occlusion_query_pool_, host_index, 1);
  command_buffer.CmdVkBeginQuery(occlusion_query_pool_, host_index, 0);
  active_occlusion_query_.sample_count_address = sample_count_address;
  active_occlusion_query_.host_index = host_index;
  active_occlusion_query_.valid = true;
  return true;
}

bool VulkanCommandProcessor::EndGuestOcclusionQuery(u32 sample_count_address) {
  if (!REXCVAR_GET(::rex::gpu::cvars::occlusion_query_enable) ||
      !occlusion_query_resources_available_ || !active_occlusion_query_.valid ||
      occlusion_query_pool_ == VK_NULL_HANDLE || occlusion_query_readback_mapping_ == nullptr) {
    return false;
  }

  u32 host_index = active_occlusion_query_.host_index;
  active_occlusion_query_ = {};

  if (!BeginSubmission(true)) {
    return false;
  }

  EndRenderPass();

  DeferredCommandBuffer& command_buffer = GetDeferredCommandBuffer();
  command_buffer.CmdVkEndQuery(occlusion_query_pool_, host_index);
  command_buffer.CmdVkCopyQueryPoolResults(
      occlusion_query_pool_, host_index, 1, occlusion_query_readback_buffer_,
      sizeof(u64) * host_index, sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

  if (!EndSubmission(false)) {
    return false;
  }
  if (!AwaitAllQueueOperationsCompletion()) {
    return false;
  }

  const gpu::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  if (!(vulkan_device->GetMemoryTypes().host_coherent &
        (static_cast<u32>(1) << occlusion_query_readback_memory_type_))) {
    VkMappedMemoryRange memory_range = {};
    memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    memory_range.memory = occlusion_query_readback_memory_;
    memory_range.offset = 0;
    memory_range.size = std::min(rex::round_up(VkDeviceSize(sizeof(u64) * kMaxOcclusionQueries),
                                               vulkan_device->GetProperties().nonCoherentAtomSize),
                                 occlusion_query_readback_memory_size_);
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &memory_range);
  }

  const u64* results = reinterpret_cast<const u64*>(occlusion_query_readback_mapping_);
  u64 samples = NormalizeOcclusionSamples(results[host_index]);
  WriteGuestOcclusionResult(sample_count_address, samples);
  return true;
}

u64 VulkanCommandProcessor::NormalizeOcclusionSamples(u64 samples) const {
  if (samples == 0 || !texture_cache_) {
    return samples;
  }
  u64 scale_x = texture_cache_->GetDrawResolutionScaleX();
  u64 scale_y = texture_cache_->GetDrawResolutionScaleY();
  u64 scale = scale_x * scale_y;
  if (scale <= 1) {
    return samples;
  }
  return (samples + (scale >> 1)) / scale;
}

void VulkanCommandProcessor::WriteGuestOcclusionResult(u32 sample_count_address, u64 samples) {
  auto* sample_counts = memory_->TranslatePhysical<xenos::DepthSampleCounts*>(sample_count_address);
  if (!sample_counts) {
    return;
  }
  u32 clamped = samples > static_cast<u64>(UINT32_MAX) ? UINT32_MAX : static_cast<u32>(samples);
  WriteSampleCounts(*sample_counts, clamped);
}

void VulkanCommandProcessor::CheckSubmissionFenceAndDeviceLoss(u64 await_submission) {
  // Reported once, retrying a wait that cannot succeed is pointless.
  if (device_lost_) {
    return;
  }

  if (await_submission >= GetCurrentSubmission()) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // A submission does not end if never started or if ending failed, so clamp the index.
    await_submission = GetCurrentSubmission() - 1;
  }

  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  size_t fences_total = submissions_in_flight_fences_.size();
  size_t fences_awaited = 0;
  if (await_submission > submission_completed_) {
    VkResult wait_result =
        dfn.vkWaitForFences(device, static_cast<u32>(await_submission - submission_completed_),
                            submissions_in_flight_fences_.data(), VK_TRUE, UINT64_MAX);
    if (wait_result == VK_SUCCESS) {
      fences_awaited += await_submission - submission_completed_;
    } else {
      REXLOG_ERROR(gpu, "Failed to await submission completion Vulkan fences");
      if (wait_result == VK_ERROR_DEVICE_LOST) {
        device_lost_ = true;
      }
    }
  }
  // Checked in order because submissions may execute out of order but are serialized here.
  while (fences_awaited < fences_total) {
    VkResult fence_status =
        dfn.vkWaitForFences(device, 1, &submissions_in_flight_fences_[fences_awaited], VK_TRUE, 0);
    if (fence_status != VK_SUCCESS) {
      if (fence_status == VK_ERROR_DEVICE_LOST) {
        device_lost_ = true;
      }
      break;
    }
    ++fences_awaited;
  }
  if (device_lost_) {
    if (graphics_system_) {
      graphics_system_->OnHostGPULossFromAnyThread(true);
    }
    return;
  }
  if (!fences_awaited) {
    return;
  }
  fences_free_.reserve(fences_free_.size() + fences_awaited);
  auto submissions_in_flight_fences_awaited_end = submissions_in_flight_fences_.cbegin();
  std::advance(submissions_in_flight_fences_awaited_end, fences_awaited);
  fences_free_.insert(fences_free_.cend(), submissions_in_flight_fences_.cbegin(),
                      submissions_in_flight_fences_awaited_end);
  submissions_in_flight_fences_.erase(submissions_in_flight_fences_.cbegin(),
                                      submissions_in_flight_fences_awaited_end);
  submission_completed_ += fences_awaited;

  while (!submissions_in_flight_semaphores_.empty()) {
    const auto& semaphore_submission = submissions_in_flight_semaphores_.front();
    if (semaphore_submission.first > submission_completed_) {
      break;
    }
    semaphores_free_.push_back(semaphore_submission.second);
    submissions_in_flight_semaphores_.pop_front();
  }

  while (!command_buffers_submitted_.empty()) {
    const auto& command_buffer_pair = command_buffers_submitted_.front();
    if (command_buffer_pair.first > submission_completed_) {
      break;
    }
    command_buffers_writable_.push_back(command_buffer_pair.second);
    command_buffers_submitted_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(submission_completed_);

  while (!destroy_framebuffers_.empty()) {
    const auto& destroy_pair = destroy_framebuffers_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
    destroy_framebuffers_.pop_front();
  }
  while (!destroy_image_views_.empty()) {
    const auto& destroy_pair = destroy_image_views_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyImageView(device, destroy_pair.second, nullptr);
    destroy_image_views_.pop_front();
  }
  while (!destroy_buffers_.empty()) {
    const auto& destroy_pair = destroy_buffers_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
    destroy_buffers_.pop_front();
  }
  while (!destroy_images_.empty()) {
    const auto& destroy_pair = destroy_images_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyImage(device, destroy_pair.second, nullptr);
    destroy_images_.pop_front();
  }
  while (!destroy_memory_.empty()) {
    const auto& destroy_pair = destroy_memory_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
    destroy_memory_.pop_front();
  }
}

bool VulkanCommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || (!scratch_buffer_used_ && !pipeline_cache_->IsCreatingPipelines());
}

bool VulkanCommandProcessor::BeginSubmission(bool is_guest_command) {
#if REXGPU_FINE_GRAINED_DRAW_SCOPES
  REX_PROFILE_SCOPE("gpu");
#endif  // REXGPU_FINE_GRAINED_DRAW_SCOPES

  if (device_lost_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // The fence check reclaims transient resources early and limits queued frames.
  u64 await_submission =
      is_opening_frame ? closed_frame_submissions_[frame_current_ % kMaxFramesInFlight] : 0;
  CheckSubmissionFenceAndDeviceLoss(await_submission);
  if (device_lost_ || submission_completed_ < await_submission) {
    return false;
  }

  if (is_opening_frame) {
    // The completed frame index updates first because the CPU may be under 3 frames behind.
    frame_completed_ =
        std::max(frame_current_, static_cast<u64>(kMaxFramesInFlight)) - kMaxFramesInFlight;
    for (u64 frame = frame_completed_ + 1; frame < frame_current_; ++frame) {
      if (closed_frame_submissions_[frame % kMaxFramesInFlight] > submission_completed_) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;

    // The deferred command buffer submits to the real one at the end, after async pipelines finish.
    deferred_command_buffer_.Reset();

    dynamic_viewport_update_needed_ = true;
    dynamic_scissor_update_needed_ = true;
    dynamic_depth_bias_update_needed_ = true;
    dynamic_blend_constants_update_needed_ = true;
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
    current_render_pass_ = VK_NULL_HANDLE;
    current_framebuffer_ = nullptr;
    in_render_pass_ = false;
    current_guest_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_compute_pipeline_ = VK_NULL_HANDLE;
    current_guest_graphics_pipeline_layout_ = nullptr;
    current_graphics_descriptor_sets_bound_up_to_date_ = 0;

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(GetCurrentSubmission());
  }

  if (is_opening_frame) {
    frame_open_ = true;
    frame_used_async_placeholder_pipeline_ = false;

    std::memset(current_float_constant_map_vertex_, 0, sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
    std::ranges::fill(current_graphics_descriptor_sets_, VK_NULL_HANDLE);
    current_constant_buffers_up_to_date_ = 0;
    current_graphics_descriptor_sets_[SPIRVShaderTranslator::kDescriptorSetSharedMemoryAndEDRAM] =
        shared_memory_and_edram_descriptor_set_;
    current_graphics_descriptor_set_values_up_to_date_ =
        UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetSharedMemoryAndEDRAM;

    // Pool pages reclaim only on frames since small submissions may reuse them.
    uniform_buffer_pool_->Reclaim(frame_completed_);
    while (!single_transient_descriptors_used_.empty()) {
      const UsedSingleTransientDescriptor& used_transient_descriptor =
          single_transient_descriptors_used_.front();
      if (used_transient_descriptor.frame > frame_completed_) {
        break;
      }
      single_transient_descriptors_free_[static_cast<size_t>(used_transient_descriptor.layout)]
          .push_back(used_transient_descriptor.set);
      single_transient_descriptors_used_.pop_front();
    }
    while (!constants_transient_descriptors_used_.empty()) {
      const std::pair<u64, VkDescriptorSet>& used_transient_descriptor =
          constants_transient_descriptors_used_.front();
      if (used_transient_descriptor.first > frame_completed_) {
        break;
      }
      constants_transient_descriptors_free_.push_back(used_transient_descriptor.second);
      constants_transient_descriptors_used_.pop_front();
    }
    while (!texture_transient_descriptor_sets_used_.empty()) {
      const UsedTextureTransientDescriptorSet& used_transient_descriptor_set =
          texture_transient_descriptor_sets_used_.front();
      if (used_transient_descriptor_set.frame > frame_completed_) {
        break;
      }
      auto it = texture_transient_descriptor_sets_free_.find(used_transient_descriptor_set.layout);
      if (it == texture_transient_descriptor_sets_free_.end()) {
        it = texture_transient_descriptor_sets_free_
                 .emplace(std::piecewise_construct,
                          std::forward_as_tuple(used_transient_descriptor_set.layout),
                          std::forward_as_tuple())
                 .first;
      }
      it->second.push_back(used_transient_descriptor_set.set);
      texture_transient_descriptor_sets_used_.pop_front();
    }

    EvictOldReadbackBuffers(readback_buffers_);
    EvictOldReadbackBuffers(memexport_readback_buffers_);

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
  }

  return true;
}

bool VulkanCommandProcessor::EndSubmission(bool is_swap) {
  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  if (submission_open_) {
    if (fences_free_.empty()) {
      VkFenceCreateInfo fence_create_info;
      fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      fence_create_info.pNext = nullptr;
      fence_create_info.flags = 0;
      VkFence fence;
      if (dfn.vkCreateFence(device, &fence_create_info, nullptr, &fence) != VK_SUCCESS) {
        REXLOG_ERROR(gpu, "Failed to create a Vulkan fence");
        // Dropping the submission would leave resources undefined, so retry later.
        return false;
      }
      fences_free_.push_back(fence);
    }
    if (!sparse_memory_binds_.empty() && semaphores_free_.empty()) {
      VkSemaphoreCreateInfo semaphore_create_info;
      semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      semaphore_create_info.pNext = nullptr;
      semaphore_create_info.flags = 0;
      VkSemaphore semaphore;
      if (dfn.vkCreateSemaphore(device, &semaphore_create_info, nullptr, &semaphore) !=
          VK_SUCCESS) {
        REX_FAIL(gpu, false, "Failed to create a Vulkan semaphore");
      }
      semaphores_free_.push_back(semaphore);
    }
    if (command_buffers_writable_.empty()) {
      CommandBuffer command_buffer;
      VkCommandPoolCreateInfo command_pool_create_info;
      command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      command_pool_create_info.pNext = nullptr;
      command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      command_pool_create_info.queueFamilyIndex = vulkan_device->GetQueueFamilyGraphicsCompute();
      if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr,
                                  &command_buffer.pool) != VK_SUCCESS) {
        REX_FAIL(gpu, false, "Failed to create a Vulkan command pool");
      }
      VkCommandBufferAllocateInfo command_buffer_allocate_info;
      command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      command_buffer_allocate_info.pNext = nullptr;
      command_buffer_allocate_info.commandPool = command_buffer.pool;
      command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_buffer_allocate_info.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info,
                                       &command_buffer.buffer) != VK_SUCCESS) {
        REXLOG_ERROR(gpu, "Failed to allocate a Vulkan command buffer");
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
        return false;
      }
      command_buffers_writable_.push_back(command_buffer);
    }
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    texture_cache_->EndFrame();

    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    REX_ASSERT(!scratch_buffer_used_);

    if (active_occlusion_query_.valid && occlusion_query_pool_ != VK_NULL_HANDLE) {
      deferred_command_buffer_.CmdVkEndQuery(occlusion_query_pool_,
                                             active_occlusion_query_.host_index);
      active_occlusion_query_ = {};
    }

    EndRenderPass();

    pipeline_cache_->EndSubmission();

    render_target_cache_->EndSubmission();

    primitive_processor_->EndSubmission();

    shared_memory_->EndSubmission();

    uniform_buffer_pool_->FlushWrites();

    if (!sparse_memory_binds_.empty()) {
      sparse_buffer_bind_infos_temp_.clear();
      sparse_buffer_bind_infos_temp_.reserve(sparse_buffer_binds_.size());
      for (const SparseBufferBind& sparse_buffer_bind : sparse_buffer_binds_) {
        VkSparseBufferMemoryBindInfo& sparse_buffer_bind_info =
            sparse_buffer_bind_infos_temp_.emplace_back();
        sparse_buffer_bind_info.buffer = sparse_buffer_bind.buffer;
        sparse_buffer_bind_info.bindCount = sparse_buffer_bind.bind_count;
        sparse_buffer_bind_info.pBinds =
            sparse_memory_binds_.data() + sparse_buffer_bind.bind_offset;
      }
      REX_ASSERT(!semaphores_free_.empty());
      VkSemaphore bind_sparse_semaphore = semaphores_free_.back();
      VkBindSparseInfo bind_sparse_info;
      bind_sparse_info.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
      bind_sparse_info.pNext = nullptr;
      bind_sparse_info.waitSemaphoreCount = 0;
      bind_sparse_info.pWaitSemaphores = nullptr;
      bind_sparse_info.bufferBindCount = static_cast<u32>(sparse_buffer_bind_infos_temp_.size());
      bind_sparse_info.pBufferBinds =
          !sparse_buffer_bind_infos_temp_.empty() ? sparse_buffer_bind_infos_temp_.data() : nullptr;
      bind_sparse_info.imageOpaqueBindCount = 0;
      bind_sparse_info.pImageOpaqueBinds = nullptr;
      bind_sparse_info.imageBindCount = 0;
      bind_sparse_info.pImageBinds = 0;
      bind_sparse_info.signalSemaphoreCount = 1;
      bind_sparse_info.pSignalSemaphores = &bind_sparse_semaphore;
      VkResult bind_sparse_result;
      {
        gpu::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
            vulkan_device->AcquireQueue(vulkan_device->GetQueueFamilySparseBinding(), 0);
        bind_sparse_result = dfn.vkQueueBindSparse(queue_acquisition.GetQueue(), 1,
                                                   &bind_sparse_info, VK_NULL_HANDLE);
      }
      if (bind_sparse_result != VK_SUCCESS) {
        REX_FAIL(gpu, false, "Failed to submit Vulkan sparse binds");
      }
      current_submission_wait_semaphores_.push_back(bind_sparse_semaphore);
      semaphores_free_.pop_back();
      current_submission_wait_stage_masks_.push_back(sparse_bind_wait_stage_mask_);
      sparse_bind_wait_stage_mask_ = 0;
      sparse_buffer_binds_.clear();
      sparse_memory_binds_.clear();
    }

    SubmitBarriers(true);

    REX_ASSERT(!command_buffers_writable_.empty());
    CommandBuffer command_buffer = command_buffers_writable_.back();
    if (dfn.vkResetCommandPool(device, command_buffer.pool, 0) != VK_SUCCESS) {
      REX_FAIL(gpu, false, "Failed to reset a Vulkan command pool");
    }
    VkCommandBufferBeginInfo command_buffer_begin_info;
    command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_buffer_begin_info.pNext = nullptr;
    command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    command_buffer_begin_info.pInheritanceInfo = nullptr;
    if (dfn.vkBeginCommandBuffer(command_buffer.buffer, &command_buffer_begin_info) != VK_SUCCESS) {
      REX_FAIL(gpu, false, "Failed to begin a Vulkan command buffer");
    }
    deferred_command_buffer_.Execute(command_buffer.buffer);
    if (dfn.vkEndCommandBuffer(command_buffer.buffer) != VK_SUCCESS) {
      REX_FAIL(gpu, false, "Failed to end a Vulkan command buffer");
    }

    VkSubmitInfo submit_info;
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = nullptr;
    if (!current_submission_wait_semaphores_.empty()) {
      submit_info.waitSemaphoreCount = static_cast<u32>(current_submission_wait_semaphores_.size());
      submit_info.pWaitSemaphores = current_submission_wait_semaphores_.data();
      submit_info.pWaitDstStageMask = current_submission_wait_stage_masks_.data();
    } else {
      submit_info.waitSemaphoreCount = 0;
      submit_info.pWaitSemaphores = nullptr;
      submit_info.pWaitDstStageMask = nullptr;
    }
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer.buffer;
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = nullptr;
    REX_ASSERT(!fences_free_.empty());
    VkFence fence = fences_free_.back();
    if (dfn.vkResetFences(device, 1, &fence) != VK_SUCCESS) {
      REX_FAIL(gpu, false, "Failed to reset a Vulkan submission fence");
    }
    VkResult submit_result;
    {
      gpu::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
          vulkan_device->AcquireQueue(vulkan_device->GetQueueFamilyGraphicsCompute(), 0);
      submit_result = dfn.vkQueueSubmit(queue_acquisition.GetQueue(), 1, &submit_info, fence);
    }
    if (submit_result != VK_SUCCESS) {
      REXLOG_ERROR(gpu, "Failed to submit a Vulkan command buffer");
      if (submit_result == VK_ERROR_DEVICE_LOST && !device_lost_) {
        device_lost_ = true;
        if (graphics_system_) {
          graphics_system_->OnHostGPULossFromAnyThread(true);
        }
      }
      return false;
    }
    u64 submission_current = GetCurrentSubmission();
    current_submission_wait_stage_masks_.clear();
    for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
      submissions_in_flight_semaphores_.emplace_back(submission_current, semaphore);
    }
    current_submission_wait_semaphores_.clear();
    command_buffers_submitted_.emplace_back(submission_current, command_buffer);
    command_buffers_writable_.pop_back();
    submissions_in_flight_fences_.push_back(fence);
    fences_free_.pop_back();

    submission_open_ = false;
  }

  if (is_closing_frame) {
    if (REXCVAR_GET(::rex::gpu::cvars::clear_memory_page_state) && shared_memory_) {
      shared_memory_->SetSystemPageBlocksValidWithGPUDataWritten();
    }
    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kMaxFramesInFlight] = GetCurrentSubmission() - 1;

    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      DestroyScratchBuffer();

      for (SwapFramebuffer& swap_framebuffer : swap_framebuffers_) {
        gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                                   swap_framebuffer.framebuffer);
      }

      REX_ASSERT(command_buffers_submitted_.empty());
      for (const CommandBuffer& command_buffer : command_buffers_writable_) {
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
      }
      command_buffers_writable_.clear();

      ClearTransientDescriptorPools();

      uniform_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      render_target_cache_->ClearCache();

      // Pipeline layouts and descriptor set layouts stay because pipelines reference them.

      primitive_processor_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

void VulkanCommandProcessor::ClearTransientDescriptorPools() {
  texture_transient_descriptor_sets_free_.clear();
  texture_transient_descriptor_sets_used_.clear();
  transient_descriptor_allocator_textures_.Reset();

  constants_transient_descriptors_free_.clear();
  constants_transient_descriptors_used_.clear();
  for (std::vector<VkDescriptorSet>& transient_descriptors_free :
       single_transient_descriptors_free_) {
    transient_descriptors_free.clear();
  }
  single_transient_descriptors_used_.clear();
  transient_descriptor_allocator_storage_buffer_.Reset();
  transient_descriptor_allocator_uniform_buffer_.Reset();
}

void VulkanCommandProcessor::SplitPendingBarrier() {
  size_t pending_buffer_memory_barrier_count = pending_barriers_buffer_memory_barriers_.size();
  size_t pending_image_memory_barrier_count = pending_barriers_image_memory_barriers_.size();
  if (!current_pending_barrier_.src_stage_mask && !current_pending_barrier_.dst_stage_mask &&
      current_pending_barrier_.buffer_memory_barriers_offset >=
          pending_buffer_memory_barrier_count &&
      current_pending_barrier_.image_memory_barriers_offset >= pending_image_memory_barrier_count) {
    return;
  }
  pending_barriers_.emplace_back(current_pending_barrier_);
  current_pending_barrier_.src_stage_mask = 0;
  current_pending_barrier_.dst_stage_mask = 0;
  current_pending_barrier_.buffer_memory_barriers_offset = pending_buffer_memory_barrier_count;
  current_pending_barrier_.image_memory_barriers_offset = pending_image_memory_barrier_count;
}

void VulkanCommandProcessor::DestroyScratchBuffer() {
  REX_ASSERT(!scratch_buffer_used_);

  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  scratch_buffer_last_usage_submission_ = 0;
  scratch_buffer_last_access_mask_ = 0;
  scratch_buffer_last_stage_mask_ = 0;
  scratch_buffer_size_ = 0;
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, scratch_buffer_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device, scratch_buffer_memory_);
}

bool VulkanCommandProcessor::EnsureSwapFxaaSourceImage(u32 width, u32 height) {
  REX_ASSERT(submission_open_);
  if (!width || !height) {
    return false;
  }
  if (swap_fxaa_source_image_ != VK_NULL_HANDLE && swap_fxaa_source_image_width_ == width &&
      swap_fxaa_source_image_height_ == height) {
    return true;
  }

  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  if (swap_fxaa_source_image_ != VK_NULL_HANDLE) {
    const u64 destroy_submission = swap_fxaa_source_image_submission_;
    const u64 deferred_destroy_submission = GetCurrentSubmission();
    if (submission_completed_ >= destroy_submission) {
      gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyImageView, device,
                                                 swap_fxaa_source_image_view_);
      gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyImage, device,
                                                 swap_fxaa_source_image_);
      gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                                 swap_fxaa_source_image_memory_);
    } else {
      if (swap_fxaa_source_image_view_ != VK_NULL_HANDLE) {
        destroy_image_views_.emplace_back(deferred_destroy_submission,
                                          swap_fxaa_source_image_view_);
        swap_fxaa_source_image_view_ = VK_NULL_HANDLE;
      }
      if (swap_fxaa_source_image_ != VK_NULL_HANDLE) {
        destroy_images_.emplace_back(deferred_destroy_submission, swap_fxaa_source_image_);
        swap_fxaa_source_image_ = VK_NULL_HANDLE;
      }
      if (swap_fxaa_source_image_memory_ != VK_NULL_HANDLE) {
        destroy_memory_.emplace_back(deferred_destroy_submission, swap_fxaa_source_image_memory_);
        swap_fxaa_source_image_memory_ = VK_NULL_HANDLE;
      }
    }
  }
  swap_fxaa_source_image_width_ = 0;
  swap_fxaa_source_image_height_ = 0;
  swap_fxaa_source_image_submission_ = 0;
  swap_fxaa_source_stage_mask_ = 0;
  swap_fxaa_source_access_mask_ = 0;
  swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  image_create_info.extent.width = width;
  image_create_info.extent.height = height;
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!gpu::vulkan::objects::CreateDedicatedAllocationImage(
          vulkan_device, image_create_info, gpu::vulkan::objects::MemoryPurpose::kDeviceLocal,
          swap_fxaa_source_image_, swap_fxaa_source_image_memory_)) {
    REX_FAIL(gpu, false, "Failed to create the FXAA source image");
  }

  VkImageViewCreateInfo image_view_create_info;
  image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_create_info.pNext = nullptr;
  image_view_create_info.flags = 0;
  image_view_create_info.image = swap_fxaa_source_image_;
  image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image_view_create_info.format = image_create_info.format;
  image_view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.subresourceRange = gpu::vulkan::objects::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &image_view_create_info, nullptr,
                            &swap_fxaa_source_image_view_) != VK_SUCCESS) {
    REXLOG_ERROR(gpu, "Failed to create the FXAA source image view");
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyImage, device, swap_fxaa_source_image_);
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                               swap_fxaa_source_image_memory_);
    return false;
  }

  swap_fxaa_source_image_width_ = width;
  swap_fxaa_source_image_height_ = height;
  swap_fxaa_source_image_submission_ = 0;
  swap_fxaa_source_stage_mask_ = 0;
  swap_fxaa_source_access_mask_ = 0;
  swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  return true;
}

void VulkanCommandProcessor::DestroySwapFxaaSourceImage() {
  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyImageView, device,
                                             swap_fxaa_source_image_view_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyImage, device, swap_fxaa_source_image_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                             swap_fxaa_source_image_memory_);
  swap_fxaa_source_image_width_ = 0;
  swap_fxaa_source_image_height_ = 0;
  swap_fxaa_source_image_submission_ = 0;
  swap_fxaa_source_stage_mask_ = 0;
  swap_fxaa_source_access_mask_ = 0;
  swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanCommandProcessor::UpdateDynamicState(
    const draw_state::ViewportInfo& viewport_info, bool primitive_polygonal,
    xenos::reg::RB_DEPTHCONTROL normalized_depth_control) {
#if REXGPU_FINE_GRAINED_DRAW_SCOPES
  REX_PROFILE_SCOPE("gpu");
#endif  // REXGPU_FINE_GRAINED_DRAW_SCOPES

  const xenos::RegisterFile& regs = *register_file_;
  u32 draw_resolution_scale_x = texture_cache_ ? texture_cache_->GetDrawResolutionScaleX() : 1;
  u32 draw_resolution_scale_y = texture_cache_ ? texture_cache_->GetDrawResolutionScaleY() : 1;

  // Window parameters follow r200UpdateWindow in Mesa's r200_state.c.

  VkViewport viewport;
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    viewport.x = static_cast<f32>(viewport_info.xy_offset[0]);
    viewport.y = static_cast<f32>(viewport_info.xy_offset[1]);
    viewport.width = static_cast<f32>(viewport_info.xy_extent[0]);
    viewport.height = static_cast<f32>(viewport_info.xy_extent[1]);
  } else {
    // Vulkan viewport width must be above 0, so an empty viewport moves outside the framebuffer.
    viewport.x = -1.0f;
    viewport.y = -1.0f;
    viewport.width = 1.0f;
    viewport.height = 1.0f;
  }
  viewport.minDepth = viewport_info.z_min;
  viewport.maxDepth = viewport_info.z_max;
  SetViewport(viewport);

  draw_state::Scissor scissor;
  draw_state::GetScissor(regs, scissor);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;
  VkRect2D scissor_rect;
  scissor_rect.offset.x = static_cast<i32>(scissor.offset[0]);
  scissor_rect.offset.y = static_cast<i32>(scissor.offset[1]);
  scissor_rect.extent.width = scissor.extent[0];
  scissor_rect.extent.height = scissor.extent[1];
  SetScissor(scissor_rect);

  if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    f32 depth_bias_constant_factor, depth_bias_slope_factor;
    draw_state::GetPreferredFacePolygonOffset(regs, primitive_polygonal, depth_bias_slope_factor,
                                              depth_bias_constant_factor);
    depth_bias_constant_factor *=
        regs.Get<xenos::reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8
            ? draw_state::kD3D10PolygonOffsetFactorUnorm24
            : draw_state::kD3D10PolygonOffsetFactorFloat24;
    // Non square resolution scaling takes the max scale, as extra bias beats Z fighting.
    depth_bias_slope_factor *=
        xenos::kPolygonOffsetScaleSubpixelUnit *
        static_cast<f32>(std::max(draw_resolution_scale_x, draw_resolution_scale_y));
    // memcmp instead of != so NaN does not invalidate on every draw.
    dynamic_depth_bias_update_needed_ |= std::bit_cast<u32>(dynamic_depth_bias_constant_factor_) !=
                                         std::bit_cast<u32>(depth_bias_constant_factor);
    dynamic_depth_bias_update_needed_ |= std::bit_cast<u32>(dynamic_depth_bias_slope_factor_) !=
                                         std::bit_cast<u32>(depth_bias_slope_factor);
    if (dynamic_depth_bias_update_needed_) {
      dynamic_depth_bias_constant_factor_ = depth_bias_constant_factor;
      dynamic_depth_bias_slope_factor_ = depth_bias_slope_factor;
      deferred_command_buffer_.CmdVkSetDepthBias(dynamic_depth_bias_constant_factor_, 0.0f,
                                                 dynamic_depth_bias_slope_factor_);
      dynamic_depth_bias_update_needed_ = false;
    }

    f32 blend_constants[] = {
        regs.Get<f32>(xenos::Register::RB_BLEND_RED),
        regs.Get<f32>(xenos::Register::RB_BLEND_GREEN),
        regs.Get<f32>(xenos::Register::RB_BLEND_BLUE),
        regs.Get<f32>(xenos::Register::RB_BLEND_ALPHA),
    };
    dynamic_blend_constants_update_needed_ |=
        !draw_state::AreBitsEqual(dynamic_blend_constants_, blend_constants);
    if (dynamic_blend_constants_update_needed_) {
      std::memcpy(dynamic_blend_constants_, blend_constants, sizeof(f32) * 4);
      deferred_command_buffer_.CmdVkSetBlendConstants(dynamic_blend_constants_);
      dynamic_blend_constants_update_needed_ = false;
    }

    // Masks and references are always dynamic in guest pipelines, so they are set before any draw.
    if (normalized_depth_control.stencil_enable) {
      xenos::Register stencil_ref_mask_front_reg, stencil_ref_mask_back_reg;
      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        if (GetVulkanDevice()->GetProperties().separateStencilMaskRef) {
          stencil_ref_mask_front_reg = xenos::Register::RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = xenos::Register::RB_STENCILREFMASK_BF;
        } else {
          // Choose the back face values only if drawing only back faces.
          stencil_ref_mask_front_reg = regs.Get<xenos::reg::PA_SU_SC_MODE_CNTL>().cull_front
                                           ? xenos::Register::RB_STENCILREFMASK_BF
                                           : xenos::Register::RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = stencil_ref_mask_front_reg;
        }
      } else {
        stencil_ref_mask_front_reg = xenos::Register::RB_STENCILREFMASK;
        stencil_ref_mask_back_reg = xenos::Register::RB_STENCILREFMASK;
      }
      auto stencil_ref_mask_front =
          regs.Get<xenos::reg::RB_STENCILREFMASK>(stencil_ref_mask_front_reg);
      auto stencil_ref_mask_back =
          regs.Get<xenos::reg::RB_STENCILREFMASK>(stencil_ref_mask_back_reg);
      dynamic_stencil_compare_mask_front_update_needed_ |=
          dynamic_stencil_compare_mask_front_ != stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_front_ = stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_back_update_needed_ |=
          dynamic_stencil_compare_mask_back_ != stencil_ref_mask_back.stencilmask;
      dynamic_stencil_compare_mask_back_ = stencil_ref_mask_back.stencilmask;
      dynamic_stencil_write_mask_front_update_needed_ |=
          dynamic_stencil_write_mask_front_ != stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_front_ = stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_back_update_needed_ |=
          dynamic_stencil_write_mask_back_ != stencil_ref_mask_back.stencilwritemask;
      dynamic_stencil_write_mask_back_ = stencil_ref_mask_back.stencilwritemask;
      dynamic_stencil_reference_front_update_needed_ |=
          dynamic_stencil_reference_front_ != stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_front_ = stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_back_update_needed_ |=
          dynamic_stencil_reference_back_ != stencil_ref_mask_back.stencilref;
      dynamic_stencil_reference_back_ = stencil_ref_mask_back.stencilref;
    }
    // FRONT_AND_BACK keeps the portability subset without separateStencilMaskRef safe.
    if (dynamic_stencil_compare_mask_front_update_needed_ ||
        dynamic_stencil_compare_mask_back_update_needed_) {
      if (dynamic_stencil_compare_mask_front_ == dynamic_stencil_compare_mask_back_) {
        deferred_command_buffer_.CmdVkSetStencilCompareMask(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                            dynamic_stencil_compare_mask_front_);
      } else {
        if (dynamic_stencil_compare_mask_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilCompareMask(VK_STENCIL_FACE_FRONT_BIT,
                                                              dynamic_stencil_compare_mask_front_);
        }
        if (dynamic_stencil_compare_mask_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilCompareMask(VK_STENCIL_FACE_BACK_BIT,
                                                              dynamic_stencil_compare_mask_back_);
        }
      }
      dynamic_stencil_compare_mask_front_update_needed_ = false;
      dynamic_stencil_compare_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_write_mask_front_update_needed_ ||
        dynamic_stencil_write_mask_back_update_needed_) {
      if (dynamic_stencil_write_mask_front_ == dynamic_stencil_write_mask_back_) {
        deferred_command_buffer_.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                          dynamic_stencil_write_mask_front_);
      } else {
        if (dynamic_stencil_write_mask_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_FRONT_BIT,
                                                            dynamic_stencil_write_mask_front_);
        }
        if (dynamic_stencil_write_mask_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_BACK_BIT,
                                                            dynamic_stencil_write_mask_back_);
        }
      }
      dynamic_stencil_write_mask_front_update_needed_ = false;
      dynamic_stencil_write_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_reference_front_update_needed_ ||
        dynamic_stencil_reference_back_update_needed_) {
      if (dynamic_stencil_reference_front_ == dynamic_stencil_reference_back_) {
        deferred_command_buffer_.CmdVkSetStencilReference(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                          dynamic_stencil_reference_front_);
      } else {
        if (dynamic_stencil_reference_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilReference(VK_STENCIL_FACE_FRONT_BIT,
                                                            dynamic_stencil_reference_front_);
        }
        if (dynamic_stencil_reference_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilReference(VK_STENCIL_FACE_BACK_BIT,
                                                            dynamic_stencil_reference_back_);
        }
      }
      dynamic_stencil_reference_front_update_needed_ = false;
      dynamic_stencil_reference_back_update_needed_ = false;
    }
  }
}

void VulkanCommandProcessor::UpdateSystemConstantValues(
    bool primitive_polygonal,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool shader_32bit_index_dma, u32 compute_memexport_vertex_count,
    const draw_state::ViewportInfo& viewport_info, u32 used_texture_mask,
    xenos::reg::RB_DEPTHCONTROL normalized_depth_control, u32 normalized_color_mask) {
#if REXGPU_FINE_GRAINED_DRAW_SCOPES
  REX_PROFILE_SCOPE("gpu");
#endif  // REXGPU_FINE_GRAINED_DRAW_SCOPES

  const xenos::RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<xenos::reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<xenos::reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<xenos::reg::PA_SU_SC_MODE_CNTL>();
  auto rb_alpha_ref = regs.Get<f32>(xenos::Register::RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<xenos::reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<xenos::reg::RB_DEPTH_INFO>();
  auto rb_stencilrefmask = regs.Get<xenos::reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf =
      regs.Get<xenos::reg::RB_STENCILREFMASK>(xenos::Register::RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<xenos::reg::RB_SURFACE_INFO>();
  auto vgt_dma_size = regs.Get<xenos::reg::VGT_DMA_SIZE>();
  auto vgt_draw_initiator = regs.Get<xenos::reg::VGT_DRAW_INITIATOR>();
  auto vgt_indx_offset = regs.Get<i32>(xenos::Register::VGT_INDX_OFFSET);
  auto vgt_max_vtx_indx = regs.Get<u32>(xenos::Register::VGT_MAX_VTX_INDX);
  auto vgt_min_vtx_indx = regs.Get<u32>(xenos::Register::VGT_MIN_VTX_INDX);

  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  u32 draw_resolution_scale_x = texture_cache_->GetDrawResolutionScaleX();
  u32 draw_resolution_scale_y = texture_cache_->GetDrawResolutionScaleY();

  // Overlapping render targets stay because shaders may pick one with dynamic control flow.
  xenos::reg::RB_COLOR_INFO color_infos[xenos::kMaxColorRenderTargets];
  f32 rt_clamp[4][4];
  // Two UINT32_MAX if no components existing in the RT are written.
  u32 rt_keep_masks[4][2];
  for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info =
        regs.Get<xenos::reg::RB_COLOR_INFO>(xenos::reg::RB_COLOR_INFO::kRtRegisterIndices[i]);
    color_infos[i] = color_info;
    if (edram_fragment_shader_interlock) {
      RenderTargetCache::GetPSIColorFormatInfo(
          color_info.color_format, (normalized_color_mask >> (i * 4)) & 0b1111, rt_clamp[i][0],
          rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3], rt_keep_masks[i][0], rt_keep_masks[i][1]);
    }
  }

  // Depth and stencil are disabled when aliasing a color render target.
  bool depth_stencil_enabled =
      normalized_depth_control.stencil_enable || normalized_depth_control.z_enable;
  if (edram_fragment_shader_interlock && depth_stencil_enabled) {
    for (u32 i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  bool dirty = false;

  u32 flags = 0;
  if (shader_32bit_index_dma) {
    flags |= SPIRVShaderTranslator::kSysFlagVertexIndexLoad;
  }
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA ||
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA ||
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted) {
    flags |= SPIRVShaderTranslator::kSysFlagComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SPIRVShaderTranslator ::kSysFlagComputeOrPrimitiveVertexIndexLoad32Bit;
    }
    // Point and rectangle expansion resets only in the host strip index buffer, not guest indices.
    bool compute_or_primitive_vertex_index_reset =
        primitive_processing_result.host_primitive_reset_enabled &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
    if (compute_or_primitive_vertex_index_reset) {
      flags |= SPIRVShaderTranslator::kSysFlagComputeOrPrimitiveVertexIndexReset;
    }
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kTriangleDomainPatchIndexed) {
    flags |= SPIRVShaderTranslator::kSysFlagComputeMemExportPatchIndexInRegister1;
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kTriangleDomainCPIndexed) {
    flags |= SPIRVShaderTranslator::kSysFlagComputeMemExportTriangleCPIndexed;
  } else if (primitive_processing_result.host_vertex_shader_type ==
             Shader::HostVertexShaderType::kQuadDomainCPIndexed) {
    flags |= SPIRVShaderTranslator::kSysFlagComputeMemExportQuadCPIndexed;
  }
  if (primitive_processing_result.tessellation_mode == xenos::TessellationMode::kAdaptive &&
      (primitive_processing_result.host_vertex_shader_type ==
           Shader::HostVertexShaderType::kTriangleDomainPatchIndexed ||
       primitive_processing_result.host_vertex_shader_type ==
           Shader::HostVertexShaderType::kQuadDomainPatchIndexed)) {
    flags |= SPIRVShaderTranslator::kSysFlagComputeMemExportPatchIndexFromInvocation;
  }
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SPIRVShaderTranslator::kSysFlagXYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SPIRVShaderTranslator::kSysFlagZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SPIRVShaderTranslator::kSysFlagWNotReciprocal;
  }
  if (primitive_polygonal) {
    flags |= SPIRVShaderTranslator::kSysFlagPrimitivePolygonal;
  }
  if (draw_state::IsPrimitiveLine(regs)) {
    flags |= SPIRVShaderTranslator::kSysFlagPrimitiveLine;
  }
  flags |= static_cast<u32>(rb_surface_info.msaa_samples)
           << SPIRVShaderTranslator::kSysFlagMSAASamplesShift;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SPIRVShaderTranslator::kSysFlagDepthFloat24;
  }
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= static_cast<u32>(alpha_test_function)
           << SPIRVShaderTranslator::kSysFlagAlphaPassIfLessShift;
  if (!render_target_cache_->GetGammaRenderTargetAsUnorm16()) {
    // Gamma targets convert through Xenos PWL gamma logic in shaders, not host sRGB conversion.
    for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= SPIRVShaderTranslator::kSysFlagConvertColor0ToGamma << i;
      }
    }
  }
  if (edram_fragment_shader_interlock && depth_stencil_enabled) {
    flags |= SPIRVShaderTranslator::kSysFlagFSIDepthStencil;
    if (normalized_depth_control.z_enable) {
      flags |= static_cast<u32>(normalized_depth_control.zfunc)
               << SPIRVShaderTranslator::kSysFlagFSIDepthPassIfLessShift;
      if (normalized_depth_control.z_write_enable) {
        flags |= SPIRVShaderTranslator::kSysFlagFSIDepthWrite;
      }
    } else {
      // Stencil without depth testing always passes and leaves the stored depth unmodified.
      flags |= SPIRVShaderTranslator::kSysFlagFSIDepthPassIfLess |
               SPIRVShaderTranslator::kSysFlagFSIDepthPassIfEqual |
               SPIRVShaderTranslator::kSysFlagFSIDepthPassIfGreater;
    }
    if (normalized_depth_control.stencil_enable) {
      flags |= SPIRVShaderTranslator::kSysFlagFSIStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= SPIRVShaderTranslator::kSysFlagFSIDepthStencilEarlyWrite;
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  if (flags & (SPIRVShaderTranslator::kSysFlagVertexIndexLoad |
               SPIRVShaderTranslator::kSysFlagComputeOrPrimitiveVertexIndexLoad)) {
    dirty |=
        system_constants_.vertex_index_load_address != primitive_processing_result.guest_index_base;
    system_constants_.vertex_index_load_address = primitive_processing_result.guest_index_base;
  }

  u32 vertex_index_reset =
      (flags & SPIRVShaderTranslator::kSysFlagComputeOrPrimitiveVertexIndexReset)
          ? regs.Get<xenos::reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx
          : 0;
  dirty |= system_constants_.vertex_index_reset != vertex_index_reset;
  system_constants_.vertex_index_reset = vertex_index_reset;
  dirty |= system_constants_.compute_memexport_vertex_count != compute_memexport_vertex_count;
  system_constants_.compute_memexport_vertex_count = compute_memexport_vertex_count;

  xenos::Endian guest_index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16 &&
      guest_index_endian != xenos::Endian::kNone && guest_index_endian != xenos::Endian::k8in16) {
    guest_index_endian =
        guest_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
  }
  xenos::Endian index_endian =
      (flags & SPIRVShaderTranslator::kSysFlagComputeOrPrimitiveVertexIndexLoad)
          ? guest_index_endian
          : primitive_processing_result.host_shader_index_endian;
  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  dirty |= system_constants_.line_loop_closing_index !=
           primitive_processing_result.line_loop_closing_index;
  system_constants_.line_loop_closing_index = primitive_processing_result.line_loop_closing_index;

  dirty |= system_constants_.vertex_base_index != vgt_indx_offset;
  system_constants_.vertex_base_index = vgt_indx_offset;

  dirty |= system_constants_.vertex_index_min != vgt_min_vtx_indx;
  dirty |= system_constants_.vertex_index_max != vgt_max_vtx_indx;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // The shader knows only the total plane count, so used user clip planes pack tightly.
  if (!pa_cl_clip_cntl.clip_disable) {
    f32* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    u32 user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    u32 user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const u32 user_clip_plane_reg =
          std::to_underlying(xenos::Register::PA_CL_UCP_0_X) + user_clip_plane_index * 4;
      const f32 user_clip_plane_regs[4] = {
          regs.Get<f32>(user_clip_plane_reg),
          regs.Get<f32>(user_clip_plane_reg + 1),
          regs.Get<f32>(user_clip_plane_reg + 2),
          regs.Get<f32>(user_clip_plane_reg + 3),
      };
      if (!draw_state::AreBitsEqual(std::span<const f32>(user_clip_plane_write_ptr, 4),
                                    user_clip_plane_regs)) {
        dirty = true;
        std::ranges::copy(user_clip_plane_regs, user_clip_plane_write_ptr);
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  // Tessellation factor range plus 1.0.
  f32 tessellation_factor_min = regs.Get<f32>(xenos::Register::VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  f32 tessellation_factor_max = regs.Get<f32>(xenos::Register::VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min != tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max != tessellation_factor_max;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  for (u32 i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<xenos::reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<xenos::reg::PA_SU_POINT_SIZE>();
    f32 point_vertex_diameter_min = static_cast<f32>(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    f32 point_vertex_diameter_max = static_cast<f32>(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    f32 point_constant_diameter_x = static_cast<f32>(pa_su_point_size.width) * (2.0f / 16.0f);
    f32 point_constant_diameter_y = static_cast<f32>(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min != point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max != point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] != point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] != point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in NDC is half the viewport axis, and 0.5 turns diameter into radius.
    f32 point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ static_cast<f32>(draw_resolution_scale_x)) /
        static_cast<f32>(std::max(viewport_info.xy_extent[0], u32{1}));
    f32 point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ static_cast<f32>(draw_resolution_scale_y)) /
        static_cast<f32>(std::max(viewport_info.xy_extent[1], u32{1}));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  u32 textures_resolution_scaled = 0;
  {
    u32 textures_remaining = used_texture_mask;
    u32 texture_index;
    while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      u32& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
      u32 texture_signs_shift = 8 * (texture_index & 3);
      u8 texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      u32 texture_signs_shifted = static_cast<u32>(texture_signs) << texture_signs_shift;
      u32 texture_signs_mask = ((UINT32_C(1) << 8) - 1) << texture_signs_shift;
      dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
      texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
      textures_resolution_scaled |=
          static_cast<u32>(texture_cache_->IsActiveTextureResolutionScaled(texture_index))
          << texture_index;
    }
  }
  dirty |= system_constants_.textures_resolution_scaled != textures_resolution_scaled;
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;

  if (!GetVulkanDevice()->GetProperties().imageViewFormatSwizzle) {
    u32 textures_remaining = used_texture_mask;
    u32 texture_index;
    while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      u32& texture_swizzles_uint = system_constants_.texture_swizzles[texture_index >> 1];
      u32 texture_swizzle_shift = 12 * (texture_index & 1);
      u32 texture_swizzle = texture_cache_->GetActiveTextureHostSwizzle(texture_index);
      u32 texture_swizzle_shifted = texture_swizzle << texture_swizzle_shift;
      u32 texture_swizzle_mask = ((UINT32_C(1) << 12) - 1) << texture_swizzle_shift;
      dirty |= (texture_swizzles_uint & texture_swizzle_mask) != texture_swizzle_shifted;
      texture_swizzles_uint =
          (texture_swizzles_uint & ~texture_swizzle_mask) | texture_swizzle_shifted;
    }
  }

  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  u32 alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8) : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  u32 edram_tile_dwords_scaled = xenos::kEDRAMTileWidthSamples * xenos::kEDRAMTileHeightSamples *
                                 (draw_resolution_scale_x * draw_resolution_scale_y);

  if (edram_fragment_shader_interlock) {
    // Align, then multiply by 32bpp tile size in dwords.
    u32 edram_32bpp_tile_pitch_dwords_scaled =
        ((rb_surface_info.surface_pitch *
          (rb_surface_info.msaa_samples >= xenos::MSAASamples::k4X ? 2 : 1)) +
         (xenos::kEDRAMTileWidthSamples - 1)) /
        xenos::kEDRAMTileWidthSamples * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_32bpp_tile_pitch_dwords_scaled !=
             edram_32bpp_tile_pitch_dwords_scaled;
    system_constants_.edram_32bpp_tile_pitch_dwords_scaled = edram_32bpp_tile_pitch_dwords_scaled;
  }

  for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    xenos::reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    i32 color_exp_bias = color_info.color_exp_bias;
    if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets &&
        ((color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 &&
          !render_target_cache_->IsFixedRG16TruncatedToMinus1To1()) ||
         (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
          !render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1()))) {
      // Remaps -32...32 to -1...1 by dividing by 32, losing blending correctness for full range.
      color_exp_bias -= 5;
    }
    const f32 color_exp_bias_scale =
        std::bit_cast<f32>(UINT32_C(0x3F800000) + (color_exp_bias << 23));
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
    if (edram_fragment_shader_interlock) {
      dirty |= system_constants_.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
      system_constants_.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      dirty |= system_constants_.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
      system_constants_.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX) {
        u32 rt_base_dwords_scaled = color_info.color_base * edram_tile_dwords_scaled;
        dirty |= system_constants_.edram_rt_base_dwords_scaled[i] != rt_base_dwords_scaled;
        system_constants_.edram_rt_base_dwords_scaled[i] = rt_base_dwords_scaled;
        u32 format_flags = RenderTargetCache::AddPSIColorFormatFlags(color_info.color_format);
        dirty |= system_constants_.edram_rt_format_flags[i] != format_flags;
        system_constants_.edram_rt_format_flags[i] = format_flags;
        u32 blend_factors_ops =
            regs[xenos::reg::RB_BLENDCONTROL::kRtRegisterIndices[i]] & 0x1FFF1FFF;
        dirty |= system_constants_.edram_rt_blend_factors_ops[i] != blend_factors_ops;
        system_constants_.edram_rt_blend_factors_ops[i] = blend_factors_ops;
        // Float comparisons are avoided because NaN would always set the dirty flag.
        dirty |= !draw_state::AreBitsEqual(system_constants_.edram_rt_clamp[i], rt_clamp[i]);
        std::ranges::copy(rt_clamp[i], system_constants_.edram_rt_clamp[i]);
      }
    }
  }

  if (edram_fragment_shader_interlock) {
    u32 depth_base_dwords_scaled = rb_depth_info.depth_base * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_depth_base_dwords_scaled != depth_base_dwords_scaled;
    system_constants_.edram_depth_base_dwords_scaled = depth_base_dwords_scaled;

    // Non polygons use the front offset enabled by POLY_OFFSET_PARA_ENABLED, polygons use both.
    f32 poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    f32 poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset_front_scale = regs.Get<f32>(xenos::Register::PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<f32>(xenos::Register::PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
        poly_offset_back_scale = regs.Get<f32>(xenos::Register::PA_SU_POLY_OFFSET_BACK_SCALE);
        poly_offset_back_offset = regs.Get<f32>(xenos::Register::PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else {
      if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
        poly_offset_front_scale = regs.Get<f32>(xenos::Register::PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<f32>(xenos::Register::PA_SU_POLY_OFFSET_FRONT_OFFSET);
        poly_offset_back_scale = poly_offset_front_scale;
        poly_offset_back_offset = poly_offset_front_offset;
      }
    }
    // Non square resolution scaling takes the max scale, as extra bias beats Z fighting.
    f32 poly_offset_scale_factor =
        xenos::kPolygonOffsetScaleSubpixelUnit *
        static_cast<f32>(std::max(draw_resolution_scale_x, draw_resolution_scale_y));
    poly_offset_front_scale *= poly_offset_scale_factor;
    poly_offset_back_scale *= poly_offset_scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale != poly_offset_front_scale;
    system_constants_.edram_poly_offset_front_scale = poly_offset_front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset != poly_offset_front_offset;
    system_constants_.edram_poly_offset_front_offset = poly_offset_front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale != poly_offset_back_scale;
    system_constants_.edram_poly_offset_back_scale = poly_offset_back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset != poly_offset_back_offset;
    system_constants_.edram_poly_offset_back_offset = poly_offset_back_offset;

    if (depth_stencil_enabled && normalized_depth_control.stencil_enable) {
      u32 stencil_front_reference_masks = rb_stencilrefmask.value & 0xFFFFFF;
      dirty |=
          system_constants_.edram_stencil_front_reference_masks != stencil_front_reference_masks;
      system_constants_.edram_stencil_front_reference_masks = stencil_front_reference_masks;
      u32 stencil_func_ops = (normalized_depth_control.value >> 8) & ((1 << 12) - 1);
      dirty |= system_constants_.edram_stencil_front_func_ops != stencil_func_ops;
      system_constants_.edram_stencil_front_func_ops = stencil_func_ops;

      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        u32 stencil_back_reference_masks = rb_stencilrefmask_bf.value & 0xFFFFFF;
        dirty |=
            system_constants_.edram_stencil_back_reference_masks != stencil_back_reference_masks;
        system_constants_.edram_stencil_back_reference_masks = stencil_back_reference_masks;
        u32 stencil_func_ops_bf = (normalized_depth_control.value >> 20) & ((1 << 12) - 1);
        dirty |= system_constants_.edram_stencil_back_func_ops != stencil_func_ops_bf;
        system_constants_.edram_stencil_back_func_ops = stencil_func_ops_bf;
      } else {
        dirty |= !std::ranges::equal(system_constants_.edram_stencil_back,
                                     system_constants_.edram_stencil_front);
        std::memcpy(system_constants_.edram_stencil_back, system_constants_.edram_stencil_front,
                    2 * sizeof(u32));
      }
    }

    dirty |=
        system_constants_.edram_blend_constant[0] != regs.Get<f32>(xenos::Register::RB_BLEND_RED);
    system_constants_.edram_blend_constant[0] = regs.Get<f32>(xenos::Register::RB_BLEND_RED);
    dirty |=
        system_constants_.edram_blend_constant[1] != regs.Get<f32>(xenos::Register::RB_BLEND_GREEN);
    system_constants_.edram_blend_constant[1] = regs.Get<f32>(xenos::Register::RB_BLEND_GREEN);
    dirty |=
        system_constants_.edram_blend_constant[2] != regs.Get<f32>(xenos::Register::RB_BLEND_BLUE);
    system_constants_.edram_blend_constant[2] = regs.Get<f32>(xenos::Register::RB_BLEND_BLUE);
    dirty |=
        system_constants_.edram_blend_constant[3] != regs.Get<f32>(xenos::Register::RB_BLEND_ALPHA);
    system_constants_.edram_blend_constant[3] = regs.Get<f32>(xenos::Register::RB_BLEND_ALPHA);
  }

  if (dirty) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferSystem);
  }
}

bool VulkanCommandProcessor::UpdateBindings(const VulkanShader* vertex_shader,
                                            const VulkanShader* pixel_shader) {
#if REXGPU_FINE_GRAINED_DRAW_SCOPES
  REX_PROFILE_SCOPE("gpu");
#endif  // REXGPU_FINE_GRAINED_DRAW_SCOPES

  const xenos::RegisterFile& regs = *register_file_;

  const gpu::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  // The float constant base addresses and ranges are hardcoded, none differ on the console.
  REX_ASSERT(regs[std::to_underlying(xenos::Register::SQ_VS_CONST)] == 0x000FF000 ||
             regs[std::to_underlying(xenos::Register::SQ_VS_CONST)] == 0x00000000);
  REX_ASSERT(regs[std::to_underlying(xenos::Register::SQ_PS_CONST)] == 0x000FF100 ||
             regs[std::to_underlying(xenos::Register::SQ_PS_CONST)] == 0x00000000);
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->GetConstantRegisterMap();
  u32 float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (u32 i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_constant_map_vertex.float_bitmap[i];
      // With no float constants any buffer is reusable, so nothing invalidates.
      if (float_constant_count_vertex) {
        current_constant_buffers_up_to_date_ &=
            ~(UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferFloatVertex);
      }
    }
  }
  u32 float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->GetConstantRegisterMap();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (u32 i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferFloatPixel);
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }

  constexpr u32 kAllConstantBuffersMask =
      (UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferCount) - 1;
  REX_ASSERT((current_constant_buffers_up_to_date_ & ~kAllConstantBuffersMask) == 0);
  if ((current_constant_buffers_up_to_date_ & kAllConstantBuffersMask) != kAllConstantBuffersMask) {
    current_graphics_descriptor_set_values_up_to_date_ &=
        ~(UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetConstants);
    size_t uniform_buffer_alignment =
        static_cast<size_t>(vulkan_device->GetProperties().minUniformBufferOffsetAlignment);
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferSystem))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SPIRVShaderTranslator::kConstantBufferSystem];
      u8* mapping = uniform_buffer_pool_->Request(
          frame_current_, sizeof(SPIRVShaderTranslator::SystemConstants), uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = sizeof(SPIRVShaderTranslator::SystemConstants);
      std::memcpy(mapping, &system_constants_, sizeof(SPIRVShaderTranslator::SystemConstants));
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SPIRVShaderTranslator::kConstantBufferSystem;
    }
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferFloatVertex))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SPIRVShaderTranslator::kConstantBufferFloatVertex];
      // A valid float constants binding is always required, so allocate a dummy buffer if needed.
      size_t float_constants_size =
          sizeof(f32) * 4 * std::max(float_constant_count_vertex, UINT32_C(1));
      u8* mapping = uniform_buffer_pool_->Request(frame_current_, float_constants_size,
                                                  uniform_buffer_alignment, buffer_info.buffer,
                                                  buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_size);
      for (u32 i = 0; i < 4; ++i) {
        u64 float_constant_map_entry = current_float_constant_map_vertex_[i];
        u32 float_constant_index;
        while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(mapping,
                      &regs[std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X) + (i << 8) +
                            (float_constant_index << 2)],
                      sizeof(f32) * 4);
          mapping += sizeof(f32) * 4;
        }
      }
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SPIRVShaderTranslator::kConstantBufferFloatVertex;
    }
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferFloatPixel))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SPIRVShaderTranslator::kConstantBufferFloatPixel];
      size_t float_constants_size =
          sizeof(f32) * 4 * std::max(float_constant_count_pixel, UINT32_C(1));
      u8* mapping = uniform_buffer_pool_->Request(frame_current_, float_constants_size,
                                                  uniform_buffer_alignment, buffer_info.buffer,
                                                  buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_size);
      for (u32 i = 0; i < 4; ++i) {
        u64 float_constant_map_entry = current_float_constant_map_pixel_[i];
        u32 float_constant_index;
        while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(mapping,
                      &regs[std::to_underlying(xenos::Register::SHADER_CONSTANT_256_X) + (i << 8) +
                            (float_constant_index << 2)],
                      sizeof(f32) * 4);
          mapping += sizeof(f32) * 4;
        }
      }
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SPIRVShaderTranslator::kConstantBufferFloatPixel;
    }
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferBoolLoop))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SPIRVShaderTranslator::kConstantBufferBoolLoop];
      constexpr size_t kBoolLoopConstantsSize = sizeof(u32) * (8 + 32);
      u8* mapping = uniform_buffer_pool_->Request(frame_current_, kBoolLoopConstantsSize,
                                                  uniform_buffer_alignment, buffer_info.buffer,
                                                  buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kBoolLoopConstantsSize);
      std::memcpy(mapping, &regs[std::to_underlying(xenos::Register::SHADER_CONSTANT_BOOL_000_031)],
                  kBoolLoopConstantsSize);
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SPIRVShaderTranslator::kConstantBufferBoolLoop;
    }
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SPIRVShaderTranslator::kConstantBufferFetch))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SPIRVShaderTranslator::kConstantBufferFetch];
      constexpr size_t kFetchConstantsSize = sizeof(u32) * 6 * 32;
      u8* mapping = uniform_buffer_pool_->Request(frame_current_, kFetchConstantsSize,
                                                  uniform_buffer_alignment, buffer_info.buffer,
                                                  buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kFetchConstantsSize);
      std::memcpy(mapping, &regs[std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0)],
                  kFetchConstantsSize);
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SPIRVShaderTranslator::kConstantBufferFetch;
    }
  }

  const std::vector<VulkanShader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  const std::vector<VulkanShader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  u32 sampler_count_vertex = static_cast<u32>(samplers_vertex.size());
  u32 texture_count_vertex = static_cast<u32>(textures_vertex.size());
  const std::vector<VulkanShader::SamplerBinding>* samplers_pixel;
  const std::vector<VulkanShader::TextureBinding>* textures_pixel;
  u32 sampler_count_pixel, texture_count_pixel;
  if (pixel_shader) {
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    sampler_count_pixel = static_cast<u32>(samplers_pixel->size());
    texture_count_pixel = static_cast<u32>(textures_pixel->size());
  } else {
    samplers_pixel = nullptr;
    textures_pixel = nullptr;
    sampler_count_pixel = 0;
    texture_count_pixel = 0;
  }
  current_graphics_descriptor_set_values_up_to_date_ &=
      ~((UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetTexturesVertex) |
        (UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetTexturesPixel));

  current_graphics_descriptor_sets_bound_up_to_date_ &=
      current_graphics_descriptor_set_values_up_to_date_;

  bool write_vertex_textures =
      (texture_count_vertex || sampler_count_vertex) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetTexturesVertex));
  bool write_pixel_textures =
      (texture_count_pixel || sampler_count_pixel) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetTexturesPixel));
  descriptor_write_image_info_.clear();
  descriptor_write_image_info_.reserve(
      (write_vertex_textures ? texture_count_vertex + sampler_count_vertex : 0) +
      (write_pixel_textures ? texture_count_pixel + sampler_count_pixel : 0));
  size_t vertex_texture_image_info_offset = descriptor_write_image_info_.size();
  if (write_vertex_textures && texture_count_vertex) {
    for (const VulkanShader::TextureBinding& texture_binding : textures_vertex) {
      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();
      descriptor_image_info.imageView = texture_cache_->GetActiveBindingOrNullImageView(
          texture_binding.fetch_constant, texture_binding.dimension,
          bool(texture_binding.is_signed));
      descriptor_image_info.imageLayout = descriptor_image_info.imageView != VK_NULL_HANDLE
                                              ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                              : VK_IMAGE_LAYOUT_UNDEFINED;
    }
  }
  size_t vertex_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (write_vertex_textures && sampler_count_vertex) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>& sampler_pair :
         current_samplers_vertex_) {
      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }
  size_t pixel_texture_image_info_offset = descriptor_write_image_info_.size();
  if (write_pixel_textures && texture_count_pixel) {
    for (const VulkanShader::TextureBinding& texture_binding : *textures_pixel) {
      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();
      descriptor_image_info.imageView = texture_cache_->GetActiveBindingOrNullImageView(
          texture_binding.fetch_constant, texture_binding.dimension,
          bool(texture_binding.is_signed));
      descriptor_image_info.imageLayout = descriptor_image_info.imageView != VK_NULL_HANDLE
                                              ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                              : VK_IMAGE_LAYOUT_UNDEFINED;
    }
  }
  size_t pixel_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (write_pixel_textures && sampler_count_pixel) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>& sampler_pair :
         current_samplers_pixel_) {
      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }

  // Bindings in one VkWriteDescriptorSet need identical stage flags, constants vary by stage.
  std::array<VkWriteDescriptorSet, SPIRVShaderTranslator::kConstantBufferCount + 2 * 2>
      write_descriptor_sets;
  u32 write_descriptor_set_count = 0;
  u32 write_descriptor_set_bits = 0;
  REX_ASSERT((current_graphics_descriptor_set_values_up_to_date_ &
              (UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetSharedMemoryAndEDRAM)) != 0);
  if (!(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetConstants))) {
    VkDescriptorSet constants_descriptor_set;
    if (!constants_transient_descriptors_free_.empty()) {
      constants_descriptor_set = constants_transient_descriptors_free_.back();
      constants_transient_descriptors_free_.pop_back();
    } else {
      VkDescriptorPoolSize constants_descriptor_count;
      constants_descriptor_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      constants_descriptor_count.descriptorCount = SPIRVShaderTranslator::kConstantBufferCount;
      constants_descriptor_set = transient_descriptor_allocator_uniform_buffer_.Allocate(
          descriptor_set_layout_constants_, &constants_descriptor_count, 1);
      if (constants_descriptor_set == VK_NULL_HANDLE) {
        return false;
      }
    }
    constants_transient_descriptors_used_.emplace_back(frame_current_, constants_descriptor_set);
    // Bindings in one VkWriteDescriptorSet need identical stage flags, constants vary by stage.
    for (u32 i = 0; i < SPIRVShaderTranslator::kConstantBufferCount; ++i) {
      VkWriteDescriptorSet& write_constants = write_descriptor_sets[write_descriptor_set_count++];
      write_constants.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_constants.pNext = nullptr;
      write_constants.dstSet = constants_descriptor_set;
      write_constants.dstBinding = i;
      write_constants.dstArrayElement = 0;
      write_constants.descriptorCount = 1;
      write_constants.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      write_constants.pImageInfo = nullptr;
      write_constants.pBufferInfo = &current_constant_buffer_infos_[i];
      write_constants.pTexelBufferView = nullptr;
    }
    write_descriptor_set_bits |= UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetConstants;
    current_graphics_descriptor_sets_[SPIRVShaderTranslator::kDescriptorSetConstants] =
        constants_descriptor_set;
  }
  if (write_vertex_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    u32 texture_descriptor_set_write_count = WriteTransientTextureBindings(
        true, texture_count_vertex, sampler_count_vertex,
        current_guest_graphics_pipeline_layout_->GetDescriptorSetLayoutTexturesVertexRef(),
        descriptor_write_image_info_.data() + vertex_texture_image_info_offset,
        descriptor_write_image_info_.data() + vertex_sampler_image_info_offset, write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |= UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetTexturesVertex;
    current_graphics_descriptor_sets_[SPIRVShaderTranslator::kDescriptorSetTexturesVertex] =
        write_textures[0].dstSet;
  }
  if (write_pixel_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    u32 texture_descriptor_set_write_count = WriteTransientTextureBindings(
        false, texture_count_pixel, sampler_count_pixel,
        current_guest_graphics_pipeline_layout_->GetDescriptorSetLayoutTexturesPixelRef(),
        descriptor_write_image_info_.data() + pixel_texture_image_info_offset,
        descriptor_write_image_info_.data() + pixel_sampler_image_info_offset, write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |= UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetTexturesPixel;
    current_graphics_descriptor_sets_[SPIRVShaderTranslator::kDescriptorSetTexturesPixel] =
        write_textures[0].dstSet;
  }
  if (write_descriptor_set_count) {
    dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count, write_descriptor_sets.data(), 0,
                               nullptr);
  }
  // Only make valid if all descriptor sets have been allocated and written successfully.
  current_graphics_descriptor_set_values_up_to_date_ |= write_descriptor_set_bits;

  u32 descriptor_sets_needed = (UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetCount) - 1;
  if (!texture_count_vertex && !sampler_count_vertex) {
    descriptor_sets_needed &= ~(UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetTexturesVertex);
  }
  if (!texture_count_pixel && !sampler_count_pixel) {
    descriptor_sets_needed &= ~(UINT32_C(1) << SPIRVShaderTranslator::kDescriptorSetTexturesPixel);
  }
  u32 descriptor_sets_remaining =
      descriptor_sets_needed & ~current_graphics_descriptor_sets_bound_up_to_date_;
  u32 descriptor_set_index;
  while (rex::bit_scan_forward(descriptor_sets_remaining, &descriptor_set_index)) {
    u32 descriptor_set_mask_tzcnt =
        rex::tzcnt(~(descriptor_sets_remaining | ((UINT32_C(1) << descriptor_set_index) - 1)));
    deferred_command_buffer_.CmdVkBindDescriptorSets(
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        current_guest_graphics_pipeline_layout_->GetPipelineLayout(), descriptor_set_index,
        descriptor_set_mask_tzcnt - descriptor_set_index,
        current_graphics_descriptor_sets_ + descriptor_set_index, 0, nullptr);
    if (descriptor_set_mask_tzcnt >= 32) {
      break;
    }
    descriptor_sets_remaining &= ~((UINT32_C(1) << descriptor_set_mask_tzcnt) - 1);
  }
  current_graphics_descriptor_sets_bound_up_to_date_ |= descriptor_sets_needed;

  return true;
}

u32 VulkanCommandProcessor::WriteTransientTextureBindings(
    bool is_vertex, u32 texture_count, u32 sampler_count,
    VkDescriptorSetLayout descriptor_set_layout, const VkDescriptorImageInfo* texture_image_info,
    const VkDescriptorImageInfo* sampler_image_info,
    VkWriteDescriptorSet* descriptor_set_writes_out) {
  REX_ASSERT(frame_open_);
  if (!texture_count && !sampler_count) {
    return 0;
  }
  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = texture_count;
  texture_descriptor_set_layout_key.sampler_count = sampler_count;
  texture_descriptor_set_layout_key.is_vertex = static_cast<u32>(is_vertex);
  VkDescriptorSet texture_descriptor_set;
  auto textures_free_it =
      texture_transient_descriptor_sets_free_.find(texture_descriptor_set_layout_key);
  if (textures_free_it != texture_transient_descriptor_sets_free_.end() &&
      !textures_free_it->second.empty()) {
    texture_descriptor_set = textures_free_it->second.back();
    textures_free_it->second.pop_back();
  } else {
    std::array<VkDescriptorPoolSize, 2> texture_descriptor_counts;
    u32 texture_descriptor_counts_count = 0;
    if (texture_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      texture_descriptor_count.descriptorCount = texture_count;
    }
    if (sampler_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLER;
      texture_descriptor_count.descriptorCount = sampler_count;
    }
    REX_ASSERT(texture_descriptor_counts_count != 0);
    texture_descriptor_set = transient_descriptor_allocator_textures_.Allocate(
        descriptor_set_layout, texture_descriptor_counts.data(), texture_descriptor_counts_count);
    if (texture_descriptor_set == VK_NULL_HANDLE) {
      return 0;
    }
  }
  UsedTextureTransientDescriptorSet& used_texture_descriptor_set =
      texture_transient_descriptor_sets_used_.emplace_back();
  used_texture_descriptor_set.frame = frame_current_;
  used_texture_descriptor_set.layout = texture_descriptor_set_layout_key;
  used_texture_descriptor_set.set = texture_descriptor_set;
  u32 descriptor_set_write_count = 0;
  if (texture_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = 0;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = texture_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_write.pImageInfo = texture_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  if (sampler_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = texture_count;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = sampler_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_write.pImageInfo = sampler_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  REX_ASSERT(descriptor_set_write_count != 0);
  return descriptor_set_write_count;
}

}  // namespace rex::gpu::vulkan
