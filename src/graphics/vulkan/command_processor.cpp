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
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
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
#include <rex/graphics/util/draw.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/pipeline_cache.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/types.h>
#include <rex/memory/utils.h>
#include <rex/ui/flags.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

// Legacy backend compatibility aliases for shared readback controls.
REXCVAR_DEFINE_BOOL(vulkan_readback_resolve, false, "GPU/Vulkan",
                    "Read render-to-texture results on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_readback_memexport, false, "GPU/Vulkan",
                    "Read data written by memory export in shaders on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_async_skip_incomplete_frames, true, "GPU/Vulkan",
                    "When async shader compilation is enabled, skip presenting frames that "
                    "used placeholder pipelines to avoid visible flashing")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_submit_on_primary_buffer_end, true, "GPU/Vulkan",
                    "Submit command buffer when PM4 primary buffer ends")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_dynamic_rendering, true, "GPU/Vulkan",
                    "Use VK_KHR_dynamic_rendering for Vulkan GPU emulation when supported by the "
                    "device (falls back to render passes otherwise)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::vulkan {

namespace {

// glslang default built-in resource limits.
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

bool CompileGlslToSpirvInternal(EShLanguage stage, std::string_view source,
                                std::vector<uint32_t>& spirv_out, std::string& error_out) {
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

// Generated with `xb buildshaders`.
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
    SpirvShaderTranslator::kConstantBufferCount* kLinkedTypeDescriptorPoolSetCount};

const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeStorageBuffer = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kLinkedTypeDescriptorPoolSetCount};

// 2x descriptors for texture images because of unsigned and signed bindings.
const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeTextures[2] = {
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kLinkedTypeDescriptorPoolSetCount},
    {VK_DESCRIPTOR_TYPE_SAMPLER, kLinkedTypeDescriptorPoolSetCount},
};

VulkanCommandProcessor::VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      deferred_command_buffer_(*this),
      transient_descriptor_allocator_uniform_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeUniformBuffer, 1, kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_storage_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeStorageBuffer, 1, kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_textures_(
          static_cast<const ui::vulkan::VulkanProvider*>(graphics_system->provider())
              ->vulkan_device(),
          kDescriptorPoolSizeTextures, uint32_t(rex::countof(kDescriptorPoolSizeTextures)),
          kLinkedTypeDescriptorPoolSetCount) {
  legacy_readback_memexport_cvar_name_ = "vulkan_readback_memexport";
}

VulkanCommandProcessor::~VulkanCommandProcessor() = default;

void VulkanCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  InvalidateAllVertexBufferResidency();
  cache_clear_requested_ = true;
}

void VulkanCommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void VulkanCommandProcessor::InvalidateAllVertexBufferResidency() {
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  for (VertexBufferState& state : vertex_buffer_states_) {
    state.address = UINT32_MAX;
    state.size = UINT32_MAX;
  }
}

void VulkanCommandProcessor::InvalidateVertexBufferResidency(uint32_t vfetch_index) {
  if (vfetch_index >= vertex_buffer_states_.size()) {
    return;
  }
  vertex_buffers_in_sync_[vfetch_index >> 6] &= ~(uint64_t(1) << (vfetch_index & 63));
}

void VulkanCommandProcessor::InvalidateVertexBufferResidencyRange(uint32_t first_vfetch,
                                                                  uint32_t last_vfetch) {
  if (first_vfetch > last_vfetch) {
    std::swap(first_vfetch, last_vfetch);
  }
  if (first_vfetch >= vertex_buffer_states_.size()) {
    return;
  }
  last_vfetch = std::min(last_vfetch, uint32_t(vertex_buffer_states_.size() - 1));
  for (uint32_t vfetch_index = first_vfetch; vfetch_index <= last_vfetch; ++vfetch_index) {
    InvalidateVertexBufferResidency(vfetch_index);
  }
}

void VulkanCommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                     uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
}

void VulkanCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) {
  shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
}

void VulkanCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  if (!BeginSubmission(true)) {
    return;
  }
  render_target_cache_->RestoreEdramSnapshot(snapshot);
}

bool VulkanCommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                                uint32_t packet, uint32_t count) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }

  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  uint32_t sample_count_addr = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  auto* sample_counts =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_addr);
  if (!sample_counts) {
    DisableHostOcclusionQueries();
    return true;
  }

  auto write_fallback_result = [sample_counts]() -> bool {
    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
    if (fake_sample_count < 0) {
      return true;
    }
    bool is_end_via_z_pass =
        sample_counts->ZPass_A == kQueryFinished && sample_counts->ZPass_B == kQueryFinished;
    bool is_end_via_z_fail =
        sample_counts->ZFail_A == kQueryFinished && sample_counts->ZFail_B == kQueryFinished;
    std::memset(sample_counts, 0, sizeof(xenos::xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      sample_counts->ZPass_A = fake_sample_count;
      sample_counts->Total_A = fake_sample_count;
    }
    return true;
  };

  bool is_end_via_z_pass =
      sample_counts->ZPass_A == kQueryFinished && sample_counts->ZPass_B == kQueryFinished;
  bool is_end_via_z_fail =
      sample_counts->ZFail_A == kQueryFinished && sample_counts->ZFail_B == kQueryFinished;
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

std::string VulkanCommandProcessor::GetWindowTitleText() const {
  std::ostringstream title;
  title << "Vulkan";
  if (render_target_cache_) {
    switch (render_target_cache_->GetPath()) {
      case RenderTargetCache::Path::kHostRenderTargets:
        title << " - FBO";
        break;
      case RenderTargetCache::Path::kPixelShaderInterlock:
        title << " - FSI";
        break;
      default:
        break;
    }
    uint32_t draw_resolution_scale_x =
        texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
    uint32_t draw_resolution_scale_y =
        texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
    if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
      title << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
    }
  }
  title << " - HEAVILY INCOMPLETE, early development";
  return title.str();
}

bool VulkanCommandProcessor::CompileGlslToSpirv(VkShaderStageFlagBits stage,
                                                std::string_view source,
                                                std::vector<uint32_t>& spirv_out,
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
      error_out = fmt::format("Unsupported Vulkan shader stage mask {}", uint32_t(stage));
      return false;
  }
  return CompileGlslToSpirvInternal(glslang_stage, source, spirv_out, error_out);
}

bool VulkanCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    REXGPU_ERROR("Failed to initialize base command processor context");
    return false;
  }
  InvalidateAllVertexBufferResidency();

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();

  // The unconditional inclusion of the vertex shader stage also covers the case
  // of manual index / factor buffer fetch (the system constants and the shared
  // memory are needed for that) in the tessellation vertex shader when
  // fullDrawIndexUint32 is not supported.
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

  // 16384 is bigger than any single uniform buffer that Xenia needs, but is the
  // minimum maxUniformBufferRange, thus the safe minimum amount.
  uniform_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      rex::align(std::max(ui::GraphicsUploadBufferPool::kDefaultPageSize, size_t(16384)),
                 size_t(device_properties.minUniformBufferOffsetAlignment)));

  // Descriptor set layouts that don't depend on the setup of other subsystems.
  VkShaderStageFlags guest_shader_stages =
      guest_shader_vertex_stages_ | VK_SHADER_STAGE_FRAGMENT_BIT;
  // Empty.
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 0;
  descriptor_set_layout_create_info.pBindings = nullptr;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_empty_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create an empty Vulkan descriptor set layout");
    return false;
  }
  // Guest draw constants.
  VkDescriptorSetLayoutBinding
      descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    VkDescriptorSetLayoutBinding& constants_binding = descriptor_set_layout_bindings_constants[i];
    constants_binding.binding = i;
    constants_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constants_binding.descriptorCount = 1;
    constants_binding.pImmutableSamplers = nullptr;
  }
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferSystem]
      .stageFlags =
      guest_shader_stages |
      (device_properties.tessellationShader ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : 0) |
      (device_properties.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferFloatVertex]
      .stageFlags = guest_shader_vertex_stages_;
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferFloatPixel]
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferBoolLoop]
      .stageFlags = guest_shader_stages;
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferFetch].stageFlags =
      guest_shader_stages;
  descriptor_set_layout_create_info.bindingCount =
      uint32_t(rex::countof(descriptor_set_layout_bindings_constants));
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings_constants;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_constants_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan descriptor set layout for guest draw "
        "constant buffers");
    return false;
  }
  // Transient: storage buffer for compute shaders.
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
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageBufferCompute)]) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan descriptor set layout for a storage buffer "
        "bound to the compute shader");
    return false;
  }
  // Transient: two storage buffers for compute shaders.
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings_transient_pair[2];
  descriptor_set_layout_bindings_transient_pair[0] = descriptor_set_layout_binding_transient;
  descriptor_set_layout_bindings_transient_pair[1] = descriptor_set_layout_binding_transient;
  descriptor_set_layout_bindings_transient_pair[1].binding = 1;
  descriptor_set_layout_create_info.bindingCount = 2;
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings_transient_pair;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageBufferPairCompute)]) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan descriptor set layout for two storage "
        "buffers bound to the compute shader");
    return false;
  }

  shared_memory_ = std::make_unique<VulkanSharedMemory>(*this, *memory_, trace_writer_,
                                                        guest_shader_pipeline_stages_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("Failed to initialize shared memory");
    return false;
  }

  primitive_processor_ = std::make_unique<VulkanPrimitiveProcessor>(
      *register_file_, *memory_, trace_writer_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the geometric primitive processor");
    return false;
  }

  uint32_t shared_memory_binding_count_log2 =
      SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
          device_properties.maxStorageBufferRange);
  uint32_t shared_memory_binding_count = UINT32_C(1) << shared_memory_binding_count_log2;

  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x, draw_resolution_scale_y);
  if (!draw_resolution_scale_not_clamped) {
    REXGPU_WARN(
        "The requested draw resolution scale is not supported by the "
        "emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }
  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    REXGPU_WARN(
        "Vulkan draw resolution scaling is experimental and may not affect all "
        "titles correctly");
  }
  if (!device_properties.fragmentStoresAndAtomics) {
    REXGPU_ERROR(
        "Vulkan fragmentStoresAndAtomics is required for GPU emulation and "
        "D3D12 parity, but unsupported by the selected device");
    return false;
  }
  if (!device_properties.vertexPipelineStoresAndAtomics) {
    REXGPU_ERROR(
        "Vulkan vertexPipelineStoresAndAtomics is required for GPU emulation and "
        "D3D12 parity, but unsupported by the selected device");
    return false;
  }
  if (!device_properties.geometryShader) {
    if (REXCVAR_GET(vulkan_require_geometry_shader)) {
      REXGPU_ERROR(
          "Vulkan geometryShader is required for GPU emulation "
          "(vulkan_require_geometry_shader=true), but unsupported by the "
          "selected device");
      return false;
    }
    REXGPU_WARN(
        "Vulkan geometryShader is not supported by the device; primitive "
        "fallback conversion/expansion paths will be used");
  }
  if (!device_properties.fillModeNonSolid) {
    if (REXCVAR_GET(vulkan_require_fill_mode_non_solid)) {
      REXGPU_ERROR(
          "Vulkan fillModeNonSolid is required for GPU emulation "
          "(vulkan_require_fill_mode_non_solid=true), but unsupported by the "
          "selected device");
      return false;
    }
    REXGPU_WARN(
        "Vulkan fillModeNonSolid is not supported by the device; line/point "
        "polygon modes will fall back to solid fill");
  }

  // Requires the transient descriptor set layouts.
  render_target_cache_ = std::make_unique<VulkanRenderTargetCache>(
      *register_file_, *memory_, trace_writer_, draw_resolution_scale_x, draw_resolution_scale_y,
      *this);
  if (!render_target_cache_->Initialize(shared_memory_binding_count)) {
    REXGPU_ERROR("Failed to initialize the render target cache");
    return false;
  }

  // Shared memory and EDRAM descriptor set layout.
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
    // EDRAM.
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
    REXGPU_ERROR(
        "Failed to create a Vulkan descriptor set layout for the shared memory "
        "and the EDRAM");
    return false;
  }

  pipeline_cache_ = std::make_unique<VulkanPipelineCache>(
      *this, *register_file_, *render_target_cache_, guest_shader_vertex_stages_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the graphics pipeline cache");
    return false;
  }

  // Requires the transient descriptor set layouts.
  texture_cache_ =
      VulkanTextureCache::Create(*register_file_, *shared_memory_, draw_resolution_scale_x,
                                 draw_resolution_scale_y, *this, guest_shader_pipeline_stages_);
  if (!texture_cache_) {
    REXGPU_ERROR("Failed to initialize the texture cache");
    return false;
  }

  // Shared memory and EDRAM common bindings.
  VkDescriptorPoolSize descriptor_pool_sizes[1];
  descriptor_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_pool_sizes[0].descriptorCount =
      shared_memory_binding_count + uint32_t(edram_fragment_shader_interlock);
  VkDescriptorPoolCreateInfo descriptor_pool_create_info;
  descriptor_pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptor_pool_create_info.pNext = nullptr;
  descriptor_pool_create_info.flags = 0;
  descriptor_pool_create_info.maxSets = 1;
  descriptor_pool_create_info.poolSizeCount = 1;
  descriptor_pool_create_info.pPoolSizes = descriptor_pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &descriptor_pool_create_info, nullptr,
                                 &shared_memory_and_edram_descriptor_pool_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the Vulkan descriptor pool for shared memory and "
        "EDRAM");
    return false;
  }
  VkDescriptorSetAllocateInfo descriptor_set_allocate_info;
  descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  descriptor_set_allocate_info.pNext = nullptr;
  descriptor_set_allocate_info.descriptorPool = shared_memory_and_edram_descriptor_pool_;
  descriptor_set_allocate_info.descriptorSetCount = 1;
  descriptor_set_allocate_info.pSetLayouts = &descriptor_set_layout_shared_memory_and_edram_;
  if (dfn.vkAllocateDescriptorSets(device, &descriptor_set_allocate_info,
                                   &shared_memory_and_edram_descriptor_set_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to allocate the Vulkan descriptor set for shared memory and "
        "EDRAM");
    return false;
  }
  VkDescriptorBufferInfo
      shared_memory_descriptor_buffers_info[SharedMemory::kBufferSize / (128 << 20)];
  uint32_t shared_memory_binding_range =
      SharedMemory::kBufferSize >> shared_memory_binding_count_log2;
  for (uint32_t i = 0; i < shared_memory_binding_count; ++i) {
    VkDescriptorBufferInfo& shared_memory_descriptor_buffer_info =
        shared_memory_descriptor_buffers_info[i];
    shared_memory_descriptor_buffer_info.buffer = shared_memory_->buffer();
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
    edram_descriptor_buffer_info.buffer = render_target_cache_->edram_buffer();
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
  dfn.vkUpdateDescriptorSets(device, 1 + uint32_t(edram_fragment_shader_interlock),
                             write_descriptor_sets, 0, nullptr);

  // Swap objects.

  // Gamma ramp, either device-local and host-visible at once, or separate
  // device-local texel buffer and host-visible upload buffer.
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
  // Try to create a device-local host-visible buffer first, to skip copying.
  constexpr uint32_t kGammaRampSize256EntryTable = sizeof(uint32_t) * 256;
  constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
  constexpr uint32_t kGammaRampSize = kGammaRampSize256EntryTable + kGammaRampSizePWL;
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
    uint32_t gamma_ramp_host_visible_buffer_memory_types =
        gamma_ramp_host_visible_buffer_memory_requirements.memoryTypeBits &
        (vulkan_device->memory_types().device_local & vulkan_device->memory_types().host_visible);
    VkMemoryAllocateInfo gamma_ramp_host_visible_buffer_memory_allocate_info;
    // Prefer a host-uncached (because it's write-only) memory type, but try a
    // host-cached host-visible device-local one as well.
    if (rex::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types &
                ~vulkan_device->memory_types().host_cached,
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
      if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
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
    // Create separate buffers for the shader and uploading.
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal, gamma_ramp_buffer_,
            gamma_ramp_buffer_memory_)) {
      REXGPU_ERROR("Failed to create the gamma ramp buffer");
      return false;
    }
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize * kMaxFramesInFlight, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            ui::vulkan::util::MemoryPurpose::kUpload, gamma_ramp_upload_buffer_,
            gamma_ramp_upload_buffer_memory_, &gamma_ramp_upload_memory_type_,
            &gamma_ramp_upload_memory_size_)) {
      REXGPU_ERROR("Failed to create the gamma ramp upload buffer");
      return false;
    }
    if (dfn.vkMapMemory(device, gamma_ramp_upload_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
                        &gamma_ramp_upload_mapping_) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to map the gamma ramp upload buffer");
      return false;
    }
  }

  // Gamma ramp buffer views.
  uint32_t gamma_ramp_frame_count =
      gamma_ramp_upload_buffer_ == VK_NULL_HANDLE ? kMaxFramesInFlight : 1;
  VkBufferViewCreateInfo gamma_ramp_buffer_view_create_info;
  gamma_ramp_buffer_view_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
  gamma_ramp_buffer_view_create_info.pNext = nullptr;
  gamma_ramp_buffer_view_create_info.flags = 0;
  gamma_ramp_buffer_view_create_info.buffer = gamma_ramp_buffer_;
  // 256-entry table.
  gamma_ramp_buffer_view_create_info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSize256EntryTable;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset = kGammaRampSize * i;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info, nullptr,
                               &gamma_ramp_buffer_views_[i * 2]) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to create a 256-entry table gamma ramp buffer view");
      return false;
    }
  }
  // Piecewise linear.
  gamma_ramp_buffer_view_create_info.format = VK_FORMAT_R16G16_UINT;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSizePWL;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset = kGammaRampSize * i + kGammaRampSize256EntryTable;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info, nullptr,
                               &gamma_ramp_buffer_views_[i * 2 + 1]) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to create a PWL gamma ramp buffer view");
      return false;
    }
  }

  // Swap descriptor set layouts.
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
    REXGPU_ERROR(
        "Failed to create the presentation sampled image descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_uniform_texel_buffer_) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the presentation uniform texel buffer descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  swap_descriptor_set_layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_combined_image_sampler_) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the presentation combined image sampler descriptor "
        "set layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_storage_image_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the presentation storage image descriptor set "
        "layout");
    return false;
  }

  // Swap descriptor pool.
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
    // Source images.
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
  // 256-entry table and PWL gamma ramps. If the gamma ramp buffer is
  // host-visible, for multiple frames.
  uint32_t gamma_ramp_buffer_view_count = 2 * gamma_ramp_frame_count;
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_uniform_texel_buffer =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_uniform_texel_buffer.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    swap_descriptor_pool_size_uniform_texel_buffer.descriptorCount = gamma_ramp_buffer_view_count;
    swap_descriptor_pool_create_info.maxSets += gamma_ramp_buffer_view_count;
  }
  if (dfn.vkCreateDescriptorPool(device, &swap_descriptor_pool_create_info, nullptr,
                                 &swap_descriptor_pool_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the presentation descriptor pool");
    return false;
  }

  // Swap descriptor set allocation.
  VkDescriptorSetAllocateInfo swap_descriptor_set_allocate_info;
  swap_descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  swap_descriptor_set_allocate_info.pNext = nullptr;
  swap_descriptor_set_allocate_info.descriptorPool = swap_descriptor_pool_;
  swap_descriptor_set_allocate_info.descriptorSetCount = 1;
  swap_descriptor_set_allocate_info.pSetLayouts = &swap_descriptor_set_layout_uniform_texel_buffer_;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_gamma_ramp_[i]) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to allocate the gamma ramp descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts = &swap_descriptor_set_layout_sampled_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_source_[i]) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to allocate the presentation source image descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_combined_image_sampler_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_fxaa_source_[i]) != VK_SUCCESS) {
      REXGPU_ERROR(
          "Failed to allocate the presentation FXAA source image descriptor "
          "sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts = &swap_descriptor_set_layout_storage_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_destination_storage_[i]) != VK_SUCCESS) {
      REXGPU_ERROR(
          "Failed to allocate the presentation destination storage image "
          "descriptor sets");
      return false;
    }
  }
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_fxaa_destination_storage_[i]) !=
        VK_SUCCESS) {
      REXGPU_ERROR(
          "Failed to allocate the presentation FXAA destination storage image "
          "descriptor sets");
      return false;
    }
  }

  // Gamma ramp descriptor sets.
  VkWriteDescriptorSet gamma_ramp_write_descriptor_set;
  gamma_ramp_write_descriptor_set.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  gamma_ramp_write_descriptor_set.pNext = nullptr;
  gamma_ramp_write_descriptor_set.dstBinding = 0;
  gamma_ramp_write_descriptor_set.dstArrayElement = 0;
  gamma_ramp_write_descriptor_set.descriptorCount = 1;
  gamma_ramp_write_descriptor_set.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  gamma_ramp_write_descriptor_set.pImageInfo = nullptr;
  gamma_ramp_write_descriptor_set.pBufferInfo = nullptr;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    gamma_ramp_write_descriptor_set.dstSet = swap_descriptors_gamma_ramp_[i];
    gamma_ramp_write_descriptor_set.pTexelBufferView = &gamma_ramp_buffer_views_[i];
    dfn.vkUpdateDescriptorSets(device, 1, &gamma_ramp_write_descriptor_set, 0, nullptr);
  }

  // Linear sampler for FXAA.
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
    REXGPU_ERROR("Failed to create the presentation FXAA sampler");
    return false;
  }

  // Gamma ramp application pipeline layout.
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
      uint32_t(swap_apply_gamma_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_apply_gamma_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 0;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges = nullptr;
  if (dfn.vkCreatePipelineLayout(device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
                                 &swap_apply_gamma_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the gamma ramp application pipeline layout");
    return false;
  }

  // Gamma ramp application compute pipeline layout.
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
      uint32_t(swap_apply_gamma_compute_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_apply_gamma_compute_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 1;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges =
      &swap_apply_gamma_compute_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
                                 &swap_apply_gamma_compute_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the gamma ramp application compute pipeline layout");
    return false;
  }

  // FXAA compute pipeline layout.
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
      uint32_t(swap_fxaa_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_fxaa_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 1;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges = &swap_fxaa_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
                                 &swap_fxaa_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the FXAA compute pipeline layout");
    return false;
  }

  // Gamma application render pass. Doesn't make assumptions about outer usage
  // (explicit barriers must be used instead) for simplicity of use in different
  // scenarios with different pipelines.
  VkAttachmentDescription swap_apply_gamma_render_pass_attachment;
  swap_apply_gamma_render_pass_attachment.flags = 0;
  swap_apply_gamma_render_pass_attachment.format = ui::vulkan::VulkanPresenter::kGuestOutputFormat;
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
  for (uint32_t i = 0; i < 2; ++i) {
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
      uint32_t(rex::countof(swap_apply_gamma_render_pass_dependencies));
  swap_apply_gamma_render_pass_create_info.pDependencies =
      swap_apply_gamma_render_pass_dependencies;
  if (dfn.vkCreateRenderPass(device, &swap_apply_gamma_render_pass_create_info, nullptr,
                             &swap_apply_gamma_render_pass_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the gamma ramp application render pass");
    return false;
  }

  // Gamma ramp application pipeline.
  // Using a graphics pipeline, not a compute one, because storage image support
  // is optional for VK_FORMAT_A2B10G10R10_UNORM_PACK32.

  enum SwapApplyGammaPixelShader {
    kSwapApplyGammaPixelShader256EntryTable,
    kSwapApplyGammaPixelShaderPWL,
    kSwapApplyGammaPixelShader256EntryTableFxaaLuma,
    kSwapApplyGammaPixelShaderPWLFxaaLuma,

    kSwapApplyGammaPixelShaderCount,
  };
  std::array<VkShaderModule, kSwapApplyGammaPixelShaderCount> swap_apply_gamma_pixel_shaders{};
  bool swap_apply_gamma_pixel_shaders_created =
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTable] =
           ui::vulkan::util::CreateShaderModule(vulkan_device, shaders::apply_gamma_table_ps,
                                                sizeof(shaders::apply_gamma_table_ps))) !=
          VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWL] =
           ui::vulkan::util::CreateShaderModule(vulkan_device, shaders::apply_gamma_pwl_ps,
                                                sizeof(shaders::apply_gamma_pwl_ps))) !=
          VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTableFxaaLuma] =
           ui::vulkan::util::CreateShaderModule(
               vulkan_device, shaders::apply_gamma_table_fxaa_luma_ps,
               sizeof(shaders::apply_gamma_table_fxaa_luma_ps))) != VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWLFxaaLuma] =
           ui::vulkan::util::CreateShaderModule(
               vulkan_device, shaders::apply_gamma_pwl_fxaa_luma_ps,
               sizeof(shaders::apply_gamma_pwl_fxaa_luma_ps))) != VK_NULL_HANDLE;
  if (!swap_apply_gamma_pixel_shaders_created) {
    REXGPU_ERROR("Failed to create the gamma ramp application pixel shader modules");
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
  swap_apply_gamma_pipeline_stages[0].module = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::fullscreen_cw_vs, sizeof(shaders::fullscreen_cw_vs));
  if (swap_apply_gamma_pipeline_stages[0].module == VK_NULL_HANDLE) {
    REXGPU_ERROR("Failed to create the gamma ramp application vertex shader module");
    for (VkShaderModule swap_apply_gamma_pixel_shader : swap_apply_gamma_pixel_shaders) {
      assert_true(swap_apply_gamma_pixel_shader != VK_NULL_HANDLE);
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

  VkPipelineMultisampleStateCreateInfo swap_apply_gamma_pipeline_multisample_state = {};
  swap_apply_gamma_pipeline_multisample_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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
      uint32_t(rex::countof(kSwapApplyGammaPipelineDynamicStates));
  swap_apply_gamma_pipeline_dynamic_state.pDynamicStates = kSwapApplyGammaPipelineDynamicStates;

  VkGraphicsPipelineCreateInfo swap_apply_gamma_pipeline_create_info;
  swap_apply_gamma_pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  swap_apply_gamma_pipeline_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_create_info.flags = 0;
  swap_apply_gamma_pipeline_create_info.stageCount =
      uint32_t(rex::countof(swap_apply_gamma_pipeline_stages));
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
  if (!vulkan_device->properties().imageViewFormatSwizzle) {
    auto create_rb_swap_pipeline = [&](const char* source, VkPipeline& pipeline_out,
                                       const char* pipeline_name) {
      std::vector<uint32_t> pixel_spirv;
      std::string compile_error;
      if (!CompileGlslToSpirv(VK_SHADER_STAGE_FRAGMENT_BIT, source, pixel_spirv, compile_error)) {
        REXGPU_WARN("Failed to compile {} shader to SPIR-V: {}", pipeline_name, compile_error);
        return;
      }
      VkShaderModule pixel_shader_module = ui::vulkan::util::CreateShaderModule(
          vulkan_device, pixel_spirv.data(), sizeof(uint32_t) * pixel_spirv.size());
      if (pixel_shader_module == VK_NULL_HANDLE) {
        REXGPU_WARN("Failed to create {} shader module", pipeline_name);
        return;
      }
      swap_apply_gamma_pipeline_stages[1].module = pixel_shader_module;
      if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                        &swap_apply_gamma_pipeline_create_info, nullptr,
                                        &pipeline_out) != VK_SUCCESS) {
        REXGPU_WARN("Failed to create {} pipeline", pipeline_name);
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
    assert_true(swap_apply_gamma_pixel_shader != VK_NULL_HANDLE);
    dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader, nullptr);
  }
  if (std::any_of(swap_apply_gamma_pipeline_create_results.begin(),
                  swap_apply_gamma_pipeline_create_results.end(),
                  [](VkResult result) { return result != VK_SUCCESS; })) {
    REXGPU_ERROR("Failed to create the gamma ramp application pipelines");
    return false;
  }

  // Gamma ramp application compute pipelines.
  swap_apply_gamma_compute_256_entry_table_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, swap_apply_gamma_compute_pipeline_layout_, shaders::apply_gamma_table_cs,
      sizeof(shaders::apply_gamma_table_cs));
  if (swap_apply_gamma_compute_256_entry_table_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline, keeping graphics fallback");
  }
  swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_ =
      ui::vulkan::util::CreateComputePipeline(
          vulkan_device, swap_apply_gamma_compute_pipeline_layout_,
          shaders::apply_gamma_table_fxaa_luma_cs, sizeof(shaders::apply_gamma_table_fxaa_luma_cs));
  if (swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline with luma output");
  }
  swap_apply_gamma_compute_pwl_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, swap_apply_gamma_compute_pipeline_layout_, shaders::apply_gamma_pwl_cs,
      sizeof(shaders::apply_gamma_pwl_cs));
  if (swap_apply_gamma_compute_pwl_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN("Failed to create the PWL gamma ramp application compute pipeline");
  }
  swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, swap_apply_gamma_compute_pipeline_layout_,
      shaders::apply_gamma_pwl_fxaa_luma_cs, sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs));
  if (swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN(
        "Failed to create the PWL gamma ramp application compute pipeline with "
        "luma output");
  }
  if (!vulkan_device->properties().imageViewFormatSwizzle) {
    auto create_rb_swap_compute_pipeline = [&](const char* source, VkPipeline& pipeline_out,
                                               const char* pipeline_name) {
      std::vector<uint32_t> compute_spirv;
      std::string compile_error;
      if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, source, compute_spirv, compile_error)) {
        REXGPU_WARN("Failed to compile {} shader to SPIR-V: {}", pipeline_name, compile_error);
        return;
      }
      pipeline_out = ui::vulkan::util::CreateComputePipeline(
          vulkan_device, swap_apply_gamma_compute_pipeline_layout_, compute_spirv.data(),
          sizeof(uint32_t) * compute_spirv.size());
      if (pipeline_out == VK_NULL_HANDLE) {
        REXGPU_WARN("Failed to create {} pipeline", pipeline_name);
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

  // FXAA compute pipelines, compiled to SPIR-V at runtime.
  std::vector<uint32_t> swap_fxaa_spirv;
  std::string swap_fxaa_compile_error;
  if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, GetSwapFxaaComputeSource(false),
                          swap_fxaa_spirv, swap_fxaa_compile_error)) {
    REXGPU_WARN("Failed to compile FXAA compute shader to SPIR-V: {}", swap_fxaa_compile_error);
  } else {
    swap_fxaa_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, swap_fxaa_pipeline_layout_, swap_fxaa_spirv.data(),
        sizeof(uint32_t) * swap_fxaa_spirv.size());
    if (swap_fxaa_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_WARN("Failed to create the FXAA compute pipeline");
    }
  }

  std::vector<uint32_t> swap_fxaa_extreme_spirv;
  std::string swap_fxaa_extreme_compile_error;
  if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, GetSwapFxaaComputeSource(true),
                          swap_fxaa_extreme_spirv, swap_fxaa_extreme_compile_error)) {
    REXGPU_WARN("Failed to compile extreme FXAA compute shader to SPIR-V: {}",
                swap_fxaa_extreme_compile_error);
  } else {
    swap_fxaa_extreme_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, swap_fxaa_pipeline_layout_, swap_fxaa_extreme_spirv.data(),
        sizeof(uint32_t) * swap_fxaa_extreme_spirv.size());
    if (swap_fxaa_extreme_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_WARN("Failed to create the extreme-quality FXAA compute pipeline");
    }
  }

  VkPipelineLayoutCreateInfo resolve_downscale_layout_create_info = {};
  resolve_downscale_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkDescriptorSetLayout resolve_downscale_set_layout =
      descriptor_set_layouts_single_transient_[size_t(
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
    resolve_downscale_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_downscale_pipeline_layout_, shaders::resolve_downscale_cs,
        sizeof(shaders::resolve_downscale_cs));
    if (resolve_downscale_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_WARN("Failed to create Vulkan resolve-downscale readback pipeline");
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             resolve_downscale_pipeline_layout_);
    }
  } else {
    REXGPU_WARN("Failed to create Vulkan resolve-downscale pipeline layout");
  }

  occlusion_query_resources_available_ = InitializeOcclusionQueryResources();

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));

  return true;
}

void VulkanCommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();
  InvalidateAllVertexBufferResidency();
  ShutdownOcclusionQueryResources();

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  DestroyScratchBuffer();

  for (auto& readback_pair : readback_buffers_) {
    ReadbackBuffer& readback = readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.mapped_data[i] && readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[i]);
      }
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, readback.buffers[i]);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, readback.memories[i]);
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  readback_buffers_.clear();
  for (auto& readback_pair : memexport_readback_buffers_) {
    ReadbackBuffer& readback = readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.mapped_data[i] && readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[i]);
      }
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, readback.buffers[i]);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, readback.memories[i]);
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  memexport_readback_buffers_.clear();

  resolve_downscale_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, resolve_downscale_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         resolve_downscale_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_downscale_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_downscale_pipeline_layout_);

  for (SwapFramebuffer& swap_framebuffer : swap_framebuffers_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                           swap_framebuffer.framebuffer);
  }
  DestroySwapFxaaSourceImage();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_fxaa_extreme_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, swap_fxaa_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_pwl_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_pwl_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device, swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device,
      swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_256_entry_table_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device, swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_256_entry_table_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_256_entry_table_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                                         swap_apply_gamma_render_pass_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_fxaa_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_apply_gamma_compute_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_apply_gamma_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroySampler, device, swap_sampler_linear_clamp_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         swap_descriptor_pool_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_descriptor_set_layout_uniform_texel_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_descriptor_set_layout_storage_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_descriptor_set_layout_combined_image_sampler_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_descriptor_set_layout_sampled_image_);
  for (VkBufferView& gamma_ramp_buffer_view : gamma_ramp_buffer_views_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBufferView, device, gamma_ramp_buffer_view);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, gamma_ramp_upload_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         gamma_ramp_upload_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, gamma_ramp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, gamma_ramp_buffer_memory_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
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

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_shared_memory_and_edram_);
  for (VkDescriptorSetLayout& descriptor_set_layout_single_transient :
       descriptor_set_layouts_single_transient_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                           descriptor_set_layout_single_transient);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_constants_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
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

void VulkanCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                  6);
    }
    InvalidateVertexBufferResidency((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 2);
  }
}

void VulkanCommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                   uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  uint32_t end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage = [](const uint64_t* usage_map, uint32_t first_constant,
                                         uint32_t last_constant) -> bool {
    if (first_constant > last_constant) {
      return false;
    }
    uint32_t first_word = first_constant >> 6;
    uint32_t last_word = last_constant >> 6;
    uint32_t first_bit = first_constant & 63;
    uint32_t last_bit = last_constant & 63;
    if (first_word == last_word) {
      uint32_t bit_count = last_bit - first_bit + 1;
      uint64_t mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << first_bit;
      return (usage_map[first_word] & mask) != 0;
    }
    if (usage_map[first_word] & (UINT64_MAX << first_bit)) {
      return true;
    }
    for (uint32_t word = first_word + 1; word < last_word; ++word) {
      if (usage_map[word]) {
        return true;
      }
    }
    uint64_t last_mask = last_bit == 63 ? UINT64_MAX : ((UINT64_C(1) << (last_bit + 1)) - 1);
    return (usage_map[last_word] & last_mask) != 0;
  };

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (frame_open_) {
      uint32_t first_float_constant = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      uint32_t last_float_constant = (end_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (first_float_constant < 256) {
        uint32_t last_vertex_constant = std::min(last_float_constant, 255u);
        if (range_has_any_constant_usage(current_float_constant_map_vertex_, first_float_constant,
                                         last_vertex_constant)) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
        }
      }
      if (last_float_constant >= 256) {
        uint32_t first_pixel_constant =
            first_float_constant >= 256 ? first_float_constant - 256 : 0;
        uint32_t last_pixel_constant = last_float_constant - 256;
        if (range_has_any_constant_usage(current_float_constant_map_pixel_, first_pixel_constant,
                                         last_pixel_constant)) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      }
    }
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
    uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    uint32_t last_fetch_dword = end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
    }
    InvalidateVertexBufferResidencyRange(first_fetch_dword / 2, last_fetch_dword / 2);
    return;
  }

  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void VulkanCommandProcessor::SparseBindBuffer(VkBuffer buffer, uint32_t bind_count,
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

void VulkanCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;

  if (!graphics_system_)
    return;
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    REXGPU_ERROR("XELOG_GPU PRESENT: NO PRESENTER");
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    REXGPU_ERROR("XELOG_GPU PRESENT: BeginSubmission FAILED");
    return;
  }

  bool skip_present_due_async_placeholder = REXCVAR_GET(async_shader_compilation) &&
                                            REXCVAR_GET(vulkan_async_skip_incomplete_frames) &&
                                            frame_used_async_placeholder_pipeline_;
  if (skip_present_due_async_placeholder) {
    static bool skipped_incomplete_frame_logged = false;
    if (!skipped_incomplete_frame_logged) {
      skipped_incomplete_frame_logged = true;
      REXGPU_WARN(
          "Skipping Vulkan frame presentation due to async placeholder draw "
          "usage in this frame");
    }
    EndSubmission(true);
    return;
  }

  SwapPostEffect swap_post_effect = GetActualSwapPostEffect();

  // Obtain the actual swap source texture size (resolution-scaled if it's a
  // resolve destination, or not otherwise).
  uint32_t frontbuffer_width_scaled, frontbuffer_height_scaled;
  uint32_t frontbuffer_width_unscaled = 0, frontbuffer_height_unscaled = 0;
  xenos::TextureFormat frontbuffer_format;
  bool swap_source_needs_rb_swap = false;
  VkImageView swap_texture_view = texture_cache_->RequestSwapTexture(
      frontbuffer_width_scaled, frontbuffer_height_scaled, frontbuffer_format,
      &frontbuffer_width_unscaled, &frontbuffer_height_unscaled, &swap_source_needs_rb_swap);
  if (swap_texture_view == VK_NULL_HANDLE) {
    REXGPU_ERROR("XELOG_GPU PRESENT: swap_texture_view=NULL");
    return;
  }
  // The swap gamma / FXAA pass samples source texels by pixel index, but swap
  // textures may be allocation-padded. Prefer the active frontbuffer region
  // from the swap packet, scaled proportionally to the actual source texture.
  auto get_active_swap_dimension = [](uint32_t packet_unscaled, uint32_t source_unscaled,
                                      uint32_t source_scaled) -> uint32_t {
    if (!source_scaled) {
      return 0;
    }
    uint32_t active_unscaled = packet_unscaled ? packet_unscaled : source_unscaled;
    if (!active_unscaled) {
      return source_scaled;
    }
    if (source_unscaled) {
      active_unscaled = std::min(active_unscaled, source_unscaled);
      uint64_t active_scaled =
          (uint64_t(active_unscaled) * source_scaled + (source_unscaled >> 1)) / source_unscaled;
      return uint32_t(std::clamp<uint64_t>(active_scaled, 1, source_scaled));
    }
    return std::min(active_unscaled, source_scaled);
  };
  uint32_t guest_output_width = get_active_swap_dimension(
      frontbuffer_width, frontbuffer_width_unscaled, frontbuffer_width_scaled);
  uint32_t guest_output_height = get_active_swap_dimension(
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
    uint32_t texture_scale_x = texture_cache_->draw_resolution_scale_x();
    uint32_t texture_scale_y = texture_cache_->draw_resolution_scale_y();
    uint32_t rt_scale_x =
        render_target_cache_ ? render_target_cache_->draw_resolution_scale_x() : texture_scale_x;
    uint32_t rt_scale_y =
        render_target_cache_ ? render_target_cache_->draw_resolution_scale_y() : texture_scale_y;
    if (!draw_scale_cache_mismatch_logged &&
        (texture_scale_x != rt_scale_x || texture_scale_y != rt_scale_y)) {
      draw_scale_cache_mismatch_logged = true;
      REXGPU_WARN(
          "Vulkan draw-scale mismatch: texture cache is {}x{}, render target "
          "cache is {}x{}",
          texture_scale_x, texture_scale_y, rt_scale_x, rt_scale_y);
    }
    static bool draw_scale_swap_sizes_logged = false;
    if (!draw_scale_swap_sizes_logged) {
      draw_scale_swap_sizes_logged = true;
      REXGPU_WARN(
          "Vulkan draw-scale swap sizing: packet={}x{}, src_scaled={}x{}, "
          "src_unscaled={}x{}, active={}x{}",
          frontbuffer_width, frontbuffer_height, frontbuffer_width_scaled,
          frontbuffer_height_scaled, frontbuffer_width_unscaled, frontbuffer_height_unscaled,
          guest_output_width, guest_output_height);
    }
  }
  if (texture_cache_->IsDrawResolutionScaled() && !swap_source_scaled) {
    static bool draw_scale_swap_unscaled_logged = false;
    if (!draw_scale_swap_unscaled_logged) {
      draw_scale_swap_unscaled_logged = true;
      REXGPU_WARN(
          "Vulkan draw resolution scaling is enabled, but the swap source is "
          "unscaled ({}x{}). This title may be presenting from an unscaled "
          "resolve path.",
          frontbuffer_width_scaled, frontbuffer_height_scaled);
    }
  }
  REXGPU_DEBUG(
      "XELOG_GPU PRESENT: swap_texture_view={:p} packet_size={}x{} src_size={}x{} "
      "src_unscaled={}x{} guest_output_size={}x{} format={}",
      static_cast<void*>(swap_texture_view), frontbuffer_width, frontbuffer_height,
      frontbuffer_width_scaled, frontbuffer_height_scaled, frontbuffer_width_unscaled,
      frontbuffer_height_unscaled, guest_output_width, guest_output_height,
      static_cast<uint32_t>(frontbuffer_format));

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,
      [this, guest_output_width, guest_output_height, frontbuffer_format, swap_texture_view,
       swap_post_effect,
       swap_source_needs_rb_swap](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        // In case the swap command is the only one in the frame.
        if (!BeginSubmission(true)) {
          return false;
        }

        auto& vulkan_context =
            static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        uint64_t guest_output_image_version = vulkan_context.image_version();

        const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
        const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
        const VkDevice device = vulkan_device->device();

        uint32_t swap_frame_index = uint32_t(frame_current_ % kMaxFramesInFlight);
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
        bool swap_source_requires_compute_rb_swap =
            !vulkan_device->properties().imageViewFormatSwizzle && swap_source_needs_rb_swap;
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
                REXGPU_WARN(
                    "Vulkan FXAA swap effect requested but FXAA compute "
                    "pipelines (including RB-swap fallback) are unavailable, "
                    "falling back to gamma only");
              } else {
                REXGPU_WARN(
                    "Vulkan FXAA swap effect requested but FXAA compute "
                    "pipelines are unavailable, falling back to gamma only");
              }
              fxaa_pipelines_unavailable_logged = true;
            }
            use_fxaa = false;
          } else if (!EnsureSwapFxaaSourceImage(guest_output_width, guest_output_height)) {
            static bool fxaa_source_image_failed_logged = false;
            if (!fxaa_source_image_failed_logged) {
              REXGPU_WARN(
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
            REXGPU_WARN(
                "Vulkan imageViewFormatSwizzle is unavailable and the swap "
                "source needs red/blue swizzle, but the compute fallback "
                "pipeline is unavailable; falling back to graphics "
                "presentation path");
          }
        }
        bool use_compute_gamma = swap_apply_gamma_compute_pipeline != VK_NULL_HANDLE;

        // TODO(Triang3l): FXAA can result in more than 8 bits of precision.
        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        // Update the gamma ramp if it's out of date.
        uint32_t& gamma_ramp_frame_index_ref = use_pwl_gamma_ramp
                                                   ? gamma_ramp_pwl_current_frame_
                                                   : gamma_ramp_256_entry_table_current_frame_;
        if (gamma_ramp_frame_index_ref == UINT32_MAX) {
          constexpr uint32_t kGammaRampSize256EntryTable = sizeof(uint32_t) * 256;
          constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
          constexpr uint32_t kGammaRampSize = kGammaRampSize256EntryTable + kGammaRampSizePWL;
          uint32_t gamma_ramp_offset_in_frame =
              use_pwl_gamma_ramp ? kGammaRampSize256EntryTable : 0;
          uint32_t gamma_ramp_upload_offset =
              kGammaRampSize * swap_frame_index + gamma_ramp_offset_in_frame;
          uint32_t gamma_ramp_size =
              use_pwl_gamma_ramp ? kGammaRampSizePWL : kGammaRampSize256EntryTable;
          void* gamma_ramp_frame_upload =
              reinterpret_cast<uint8_t*>(gamma_ramp_upload_mapping_) + gamma_ramp_upload_offset;
          if (std::endian::native != std::endian::little && use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload =
                reinterpret_cast<reg::DC_LUT_PWL_DATA*>(gamma_ramp_frame_upload);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_entry = gamma_ramp_pwl_upload[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(gamma_ramp_frame_upload,
                        use_pwl_gamma_ramp ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                                           : static_cast<const void*>(gamma_ramp_256_entry_table()),
                        gamma_ramp_size);
          }
          bool gamma_ramp_has_upload_buffer = gamma_ramp_upload_buffer_memory_ != VK_NULL_HANDLE;
          VkPipelineStageFlags gamma_ramp_read_stage_mask =
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
          ui::vulkan::util::FlushMappedMemoryRange(
              vulkan_device,
              gamma_ramp_has_upload_buffer ? gamma_ramp_upload_buffer_memory_
                                           : gamma_ramp_buffer_memory_,
              gamma_ramp_upload_memory_type_, gamma_ramp_upload_offset,
              gamma_ramp_upload_memory_size_, gamma_ramp_size);
          if (gamma_ramp_has_upload_buffer) {
            // Copy from the host-visible buffer to the device-local one.
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
          // The device-local, but not host-visible, gamma ramp buffer doesn't
          // have per-frame sets of gamma ramps.
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
            ui::vulkan::util::InitializeSubresourceRange();
        if (use_compute_gamma) {
          // Transition the destination image for compute writes. Contents are
          // fully overwritten, so old layout can always be UNDEFINED.
          PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                 vulkan_context.image_ever_written_previously()
                                     ? ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask
                                     : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 vulkan_context.image_ever_written_previously()
                                     ? ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask
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
              use_fxaa ? swap_fxaa_source_image_view_ : vulkan_context.image_view();
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
                                           uint32_t(use_pwl_gamma_ramp)];
          swap_apply_gamma_compute_descriptor_sets[kSwapApplyGammaComputeDescriptorSetSource] =
              swap_descriptor_source;
          swap_apply_gamma_compute_descriptor_sets[kSwapApplyGammaComputeDescriptorSetDestination] =
              swap_descriptor_destination_storage;
          deferred_command_buffer_.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_COMPUTE, swap_apply_gamma_compute_pipeline_layout_, 0,
              uint32_t(swap_apply_gamma_compute_descriptor_sets.size()),
              swap_apply_gamma_compute_descriptor_sets.data(), 0, nullptr);
          SwapApplyGammaConstants swap_apply_gamma_constants = {
              {guest_output_width, guest_output_height}};
          deferred_command_buffer_.CmdVkPushConstants(
              swap_apply_gamma_compute_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
              sizeof(swap_apply_gamma_constants), &swap_apply_gamma_constants);
          BindExternalComputePipeline(swap_apply_gamma_compute_pipeline);
          uint32_t group_count_x = (guest_output_width + 15) / 16;
          uint32_t group_count_y = (guest_output_height + 7) / 8;
          deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);

          if (use_fxaa) {
            // Make the FXAA source image readable and bind a separate
            // destination storage descriptor targeting the guest output image.
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
                vulkan_context.image_view();
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
                uint32_t(swap_fxaa_descriptor_sets.size()), swap_fxaa_descriptor_sets.data(), 0,
                nullptr);
            SwapFxaaConstants swap_fxaa_constants = {
                {guest_output_width, guest_output_height},
                {1.0f / float(guest_output_width), 1.0f / float(guest_output_height)}};
            deferred_command_buffer_.CmdVkPushConstants(
                swap_fxaa_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(swap_fxaa_constants), &swap_fxaa_constants);
            BindExternalComputePipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme
                                            ? swap_fxaa_extreme_pipeline_
                                            : swap_fxaa_pipeline_);
            deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);
          }

          // Insert the release barrier.
          PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                 VK_ACCESS_SHADER_WRITE_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                                 VK_IMAGE_LAYOUT_GENERAL,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);
        } else {
          // Make sure a framebuffer is available for the current guest output
          // image version.
          size_t swap_framebuffer_index = SIZE_MAX;
          size_t swap_framebuffer_new_index = SIZE_MAX;
          // Try to find the existing framebuffer for the current guest output
          // image version, or an unused (without an existing framebuffer, or
          // with one, but that has never actually been used dynamically) slot.
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
              // Replace the earliest used framebuffer.
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
            VkImageView guest_output_image_view = vulkan_context.image_view();
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
              REXGPU_ERROR("Failed to create the Vulkan framebuffer for presentation");
              return false;
            }
            new_swap_framebuffer.version = guest_output_image_version;
            // The actual submission index will be set if the framebuffer is
            // actually used, not dropped due to some error.
            new_swap_framebuffer.last_submission = 0;
          }

          if (vulkan_context.image_ever_written_previously()) {
            // Insert a barrier after the last presenter's usage of the guest
            // output image. Will be overwriting all the contents, so oldLayout
            // layout is UNDEFINED. The render pass will do the layout
            // transition, but newLayout must not be UNDEFINED.
            PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                   ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
          }

          // End the current render pass before inserting barriers and starting
          // a new one, and insert the barrier.
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
          viewport.width = float(guest_output_width);
          viewport.height = float(guest_output_height);
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
          if (!vulkan_device->properties().imageViewFormatSwizzle && swap_source_needs_rb_swap) {
            VkPipeline swap_apply_gamma_rb_swap_pipeline =
                use_pwl_gamma_ramp ? swap_apply_gamma_pwl_rb_swap_pipeline_
                                   : swap_apply_gamma_256_entry_table_rb_swap_pipeline_;
            if (swap_apply_gamma_rb_swap_pipeline != VK_NULL_HANDLE) {
              swap_apply_gamma_pipeline = swap_apply_gamma_rb_swap_pipeline;
            } else {
              static bool swap_rb_swap_fallback_missing_logged = false;
              if (!swap_rb_swap_fallback_missing_logged) {
                swap_rb_swap_fallback_missing_logged = true;
                REXGPU_WARN(
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
                                           uint32_t(use_pwl_gamma_ramp)];
          swap_descriptor_sets[kSwapApplyGammaDescriptorSetSource] = swap_descriptor_source;
          deferred_command_buffer_.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, swap_apply_gamma_pipeline_layout_, 0,
              uint32_t(swap_descriptor_sets.size()), swap_descriptor_sets.data(), 0, nullptr);

          deferred_command_buffer_.CmdVkDraw(3, 1, 0, 0);
          deferred_command_buffer_.CmdVkEndRenderPass();

          // Insert the release barrier.
          PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);
        }

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue, and also need to submit the release barrier.
        EndSubmission(true);
        return true;
      });

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);
}

bool VulkanCommandProcessor::PushBufferMemoryBarrier(
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkPipelineStageFlags src_stage_mask,
    VkPipelineStageFlags dst_stage_mask, VkAccessFlags src_access_mask,
    VkAccessFlags dst_access_mask, uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask && src_access_mask == dst_access_mask &&
      src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping buffer ranges into different
  // pipeline barrier commands.
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
      // The barrier is already pending.
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
    VkImageLayout new_layout, uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask && src_access_mask == dst_access_mask &&
      old_layout == new_layout && src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping image subresource ranges into
  // different pipeline barrier commands.
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
      // The barrier is already pending.
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
  assert_true(submission_open_);
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
    // .data() + offset, not &[offset], for buffer and image barriers, because
    // if there are no buffer or image memory barriers in the last pipeline
    // barriers, the offsets may be equal to the sizes of the vectors.
    deferred_command_buffer_.CmdVkPipelineBarrier(
        it->src_stage_mask ? it->src_stage_mask : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        it->dst_stage_mask ? it->dst_stage_mask : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
        nullptr,
        uint32_t((is_last ? pending_barriers_buffer_memory_barriers_.size()
                          : it_next->buffer_memory_barriers_offset) -
                 it->buffer_memory_barriers_offset),
        pending_barriers_buffer_memory_barriers_.data() + it->buffer_memory_barriers_offset,
        uint32_t((is_last ? pending_barriers_image_memory_barriers_.size()
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
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && vulkan_device->properties().dynamicRendering;

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
    uint32_t color_attachment_count = 0;
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
    // TODO(Triang3l): Actual dirty width / height in the deferred command
    // buffer.
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
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && vulkan_device->properties().dynamicRendering;

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
  assert_true(submission_open_);
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
  assert_true(frame_open_);
  VkDescriptorSet descriptor_set;
  std::vector<VkDescriptorSet>& transient_descriptors_free =
      single_transient_descriptors_free_[size_t(transient_descriptor_layout)];
  if (!transient_descriptors_free.empty()) {
    descriptor_set = transient_descriptors_free.back();
    transient_descriptors_free.pop_back();
  } else {
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    [[maybe_unused]] const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    [[maybe_unused]] const VkDevice device = vulkan_device->device();
    bool is_storage_buffer =
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageBufferCompute ||
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageBufferPairCompute;
    ui::vulkan::LinkedTypeDescriptorSetAllocator& transient_descriptor_allocator =
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
  texture_descriptor_set_layout_key.texture_count = uint32_t(texture_count);
  texture_descriptor_set_layout_key.sampler_count = uint32_t(sampler_count);
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  auto it_existing = descriptor_set_layouts_textures_.find(texture_descriptor_set_layout_key);
  if (it_existing != descriptor_set_layouts_textures_.end()) {
    return it_existing->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  descriptor_set_layout_bindings_.clear();
  descriptor_set_layout_bindings_.reserve(binding_count);
  VkShaderStageFlags stage_flags =
      is_vertex ? guest_shader_vertex_stages_ : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (size_t i = 0; i < texture_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(i);
    descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  for (size_t i = 0; i < sampler_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(texture_count + i);
    descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = uint32_t(binding_count);
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
  pipeline_layout_key.texture_count_pixel = uint16_t(texture_count_pixel);
  pipeline_layout_key.sampler_count_pixel = uint16_t(sampler_count_pixel);
  pipeline_layout_key.texture_count_vertex = uint16_t(texture_count_vertex);
  pipeline_layout_key.sampler_count_vertex = uint16_t(sampler_count_vertex);
  {
    auto it = pipeline_layouts_.find(pipeline_layout_key);
    if (it != pipeline_layouts_.end()) {
      return &it->second;
    }
  }

  VkDescriptorSetLayout descriptor_set_layout_textures_vertex =
      GetTextureDescriptorSetLayout(true, texture_count_vertex, sampler_count_vertex);
  if (descriptor_set_layout_textures_vertex == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest vertex shaders",
        texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  VkDescriptorSetLayout descriptor_set_layout_textures_pixel =
      GetTextureDescriptorSetLayout(false, texture_count_pixel, sampler_count_pixel);
  if (descriptor_set_layout_textures_pixel == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest pixel shaders",
        texture_count_pixel, sampler_count_pixel);
    return nullptr;
  }

  VkDescriptorSetLayout descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetCount];
  // Immutable layouts.
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
      descriptor_set_layout_shared_memory_and_edram_;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetConstants] =
      descriptor_set_layout_constants_;
  // Mutable layouts.
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
      descriptor_set_layout_textures_vertex;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
      descriptor_set_layout_textures_pixel;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = nullptr;
  pipeline_layout_create_info.flags = 0;
  pipeline_layout_create_info.setLayoutCount = uint32_t(rex::countof(descriptor_set_layouts));
  pipeline_layout_create_info.pSetLayouts = descriptor_set_layouts;
  pipeline_layout_create_info.pushConstantRangeCount = 0;
  pipeline_layout_create_info.pPushConstantRanges = nullptr;
  VkPipelineLayout pipeline_layout;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr, &pipeline_layout) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan pipeline layout for guest drawing with {} "
        "pixel shader and {} vertex shader textures",
        texture_count_pixel, texture_count_vertex);
    return nullptr;
  }
  auto emplaced_pair = pipeline_layouts_.emplace(
      std::piecewise_construct, std::forward_as_tuple(pipeline_layout_key),
      std::forward_as_tuple(pipeline_layout, descriptor_set_layout_textures_vertex,
                            descriptor_set_layout_textures_pixel));
  // unordered_map insertion doesn't invalidate element references.
  return &emplaced_pair.first->second;
}

VulkanCommandProcessor::ScratchBufferAcquisition VulkanCommandProcessor::AcquireScratchGpuBuffer(
    VkDeviceSize size, VkPipelineStageFlags initial_stage_mask, VkAccessFlags initial_access_mask) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || !size) {
    return ScratchBufferAcquisition();
  }

  uint64_t submission_current = GetCurrentSubmission();

  if (scratch_buffer_ != VK_NULL_HANDLE && size <= scratch_buffer_size_) {
    // Already used previously - transition.
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

  size = rex::align(size, kScratchBufferSizeIncrement);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();

  VkDeviceMemory new_scratch_buffer_memory;
  VkBuffer new_scratch_buffer;
  // VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT for
  // texture loading.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, size,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, new_scratch_buffer,
          new_scratch_buffer_memory)) {
    REXGPU_ERROR("VulkanCommandProcessor: Failed to create a {} MB scratch GPU buffer", size >> 20);
    return ScratchBufferAcquisition();
  }

  if (submission_completed_ >= scratch_buffer_last_usage_submission_) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
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
  if (REXCVAR_GET(vulkan_submit_on_primary_buffer_end) && submission_open_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

Shader* VulkanCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool VulkanCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  (void)index_buffer_info;
  auto draw_fail = [&](const char* stage) {
    auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
    REXGPU_ERROR(
        "Vulkan IssueDraw failed at {} "
        "(prim_type={}, index_count={}, source_select={}, major_mode={}, explicit_major={}, "
        "path_select={}, tess_mode={}, edram_mode={})",
        stage, uint32_t(prim_type), index_count, uint32_t(vgt_draw_initiator.source_select),
        uint32_t(vgt_draw_initiator.major_mode), uint32_t(major_mode_explicit),
        uint32_t(regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select),
        uint32_t(regs.Get<reg::VGT_HOS_CNTL>().tess_mode),
        uint32_t(regs.Get<reg::RB_MODECONTROL>().edram_mode));
    return false;
  };

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    return IssueCopy();
  }

  bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;

  const ui::vulkan::VulkanDevice::Properties& device_properties = GetVulkanDevice()->properties();

  memexport_ranges_.clear();

  // Vertex shader analysis.
  auto vertex_shader = static_cast<VulkanShader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return draw_fail("missing_vertex_shader");
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;
  if (memexport_used_vertex) {
    if (!device_properties.vertexPipelineStoresAndAtomics) {
      REXGPU_ERROR(
          "Vertex shader memexport draw encountered without "
          "vertexPipelineStoresAndAtomics support");
      return false;
    }
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    // Doesn't actually draw.
    // Unlikely that zero would even really be legal though.
    return true;
  }
  VulkanShader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<VulkanShader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return true;
    }
  }
  bool memexport_used_pixel = pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  if (memexport_used_pixel) {
    if (!device_properties.fragmentStoresAndAtomics) {
      REXGPU_ERROR(
          "Pixel shader memexport draw encountered without "
          "fragmentStoresAndAtomics support");
      return false;
    }
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;

  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  SpirvShaderTranslator::Modification vertex_shader_modification;
  SpirvShaderTranslator::Modification pixel_shader_modification;
  VulkanShader::VulkanTranslation* vertex_shader_translation;
  VulkanShader::VulkanTranslation* pixel_shader_translation;
  bool memexport_writes_possible = memexport_used_vertex || memexport_used_pixel;

  // Two iterations because a submission (even the current one - in which case
  // it needs to be ended, and a new one must be started) may need to be awaited
  // in case of a sampler count overflow, and if that happens, all subsystem
  // updates done previously must be performed again because the updates done
  // before the awaiting may be referencing objects destroyed by
  // CompletedSubmissionUpdated.
  for (uint32_t i = 0; i < 2; ++i) {
    if (!BeginSubmission(true)) {
      return draw_fail("begin_submission");
    }

    // Process primitives.
    if (!primitive_processor_->Process(primitive_processing_result)) {
      return draw_fail("primitive_processing");
    }
    if (!primitive_processing_result.host_draw_vertex_count) {
      // Nothing to draw.
      return true;
    }
    if (primitive_processing_result.host_primitive_type == xenos::PrimitiveType::kTriangleFan) {
      // Vulkan uses the same fan-to-list conversion policy as D3D12.
      REXGPU_ERROR(
          "PrimitiveProcessor returned triangle fan for Vulkan draw; expected "
          "triangle list conversion for D3D12 parity");
      assert_always();
      return false;
    }
    // Tessellation and rectangle expansion variants for rasterization are
    // produced by the primitive processor and are handled by the Vulkan
    // pipeline cache.
    Shader::HostVertexShaderType host_vertex_shader_type =
        primitive_processing_result.host_vertex_shader_type;
    if (host_vertex_shader_type != Shader::HostVertexShaderType::kVertex &&
        host_vertex_shader_type != Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        host_vertex_shader_type != Shader::HostVertexShaderType::kRectangleListAsTriangleStrip &&
        !Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
      REXGPU_ERROR("Unsupported Vulkan host vertex shader type {}",
                   uint32_t(host_vertex_shader_type));
      return false;
    }

    // Shader modifications.
    vertex_shader_modification = pipeline_cache_->GetCurrentVertexShaderModification(
        *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask,
        ps_param_gen_pos != UINT32_MAX);
    pixel_shader_modification = pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                                                   *pixel_shader, interpolator_mask,
                                                   ps_param_gen_pos, normalized_depth_control)
                                             : SpirvShaderTranslator::Modification(0);

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

    // Obtain the samplers. Note that the bindings don't depend on the shader
    // modification, so if on the second iteration of this loop it becomes
    // different for some reason (like a race condition with the guest in index
    // buffer processing in the primitive processor resulting in different host
    // vertex shader types), the bindings will stay the same.
    // TODO(Triang3l): Sampler caching and reuse for adjacent draws within one
    // submission.
    uint32_t samplers_overflowed_count = 0;
    for (uint32_t j = 0; j < 2; ++j) {
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
        // UseSampler calls are needed even on the second iteration in case the
        // submission was broken (and thus the last usage submission indices for
        // the used samplers need to be updated) due to an overflow within one
        // submission. Though sampler overflow is a very rare situation overall.
        bool sampler_overflowed;
        VkSampler shader_sampler =
            texture_cache_->UseSampler(shader_sampler_pair.first, sampler_overflowed);
        shader_sampler_pair.second = shader_sampler;
        if (shader_sampler == VK_NULL_HANDLE) {
          if (!sampler_overflowed || i) {
            // If !sampler_overflowed, just failed to create a sampler for some
            // reason.
            // If i == 1, an overflow has happened twice, can't recover from it
            // anymore (would enter an infinite loop otherwise if the number of
            // attempts was not limited to 2). Possibly too many unique samplers
            // in one draw, or failed to await submission completion.
            return draw_fail("sampler_acquisition");
          }
          ++samplers_overflowed_count;
        }
      }
    }
    if (!samplers_overflowed_count) {
      break;
    }
    assert_zero(i);
    // Free space for as many samplers as how many haven't been allocated
    // successfully - obtain the submission index that needs to be awaited to
    // reuse `samplers_overflowed_count` slots. This must be done after all the
    // UseSampler calls, not inside the loop calling UseSampler, because earlier
    // UseSampler calls may "mark for deletion" some samplers that later
    // UseSampler calls in the loop may actually demand.
    uint64_t sampler_overflow_await_submission =
        texture_cache_->GetSubmissionToAwaitOnSamplerOverflow(samplers_overflowed_count);
    assert_true(sampler_overflow_await_submission <= GetCurrentSubmission());
    CheckSubmissionFenceAndDeviceLoss(sampler_overflow_await_submission);
  }

  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;

  // Update the textures before most other work in the submission because
  // samplers depend on this (and in case of sampler overflow in a submission,
  // submissions must be split) - may perform dispatches and copying.
  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  texture_cache_->RequestTextures(used_texture_mask);

  const VulkanPipelineCache::PipelineLayoutProvider* pipeline_layout_provider;
  // Set up the render targets - this may perform dispatches and draws.
  if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                    normalized_color_mask, *vertex_shader)) {
    return draw_fail("render_target_update");
  }

  // Create the pipeline (for this, need the render pass from the render target
  // cache), translating the shaders - doing this now to obtain the used
  // textures.
  VkPipeline pipeline;
  void* pipeline_handle = nullptr;
  if (!pipeline_cache_->ConfigurePipeline(vertex_shader_translation, pixel_shader_translation,
                                          primitive_processing_result, normalized_depth_control,
                                          normalized_color_mask,
                                          render_target_cache_->last_update_render_pass_key(),
                                          pipeline, pipeline_layout_provider, &pipeline_handle)) {
    return draw_fail("configure_pipeline");
  }
  bool pipeline_is_placeholder = false;
  // Reload the current handle state to observe async hot-swap completions that
  // may happen between pipeline configuration and binding.
  pipeline_cache_->GetPipelineAndLayoutByHandle(pipeline_handle, pipeline, pipeline_layout_provider,
                                                &pipeline_is_placeholder);
  if (REXCVAR_GET(async_shader_compilation) && pipeline_is_placeholder) {
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

  // Update the graphics pipeline, and if the new graphics pipeline has a
  // different layout, invalidate incompatible descriptor sets before updating
  // current_guest_graphics_pipeline_layout_.
  auto pipeline_layout = static_cast<const PipelineLayout*>(pipeline_layout_provider);
  if (current_guest_graphics_pipeline_layout_ != pipeline_layout) {
    if (current_guest_graphics_pipeline_layout_) {
      // Keep descriptor set layouts for which the new pipeline layout is
      // compatible with the previous one (pipeline layouts are compatible for
      // set N if set layouts 0 through N are compatible).
      uint32_t descriptor_sets_kept = uint32_t(SpirvShaderTranslator::kDescriptorSetCount);
      if (current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_vertex_ref() !=
          pipeline_layout->descriptor_set_layout_textures_vertex_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept, uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesVertex));
      }
      if (current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_pixel_ref() !=
          pipeline_layout->descriptor_set_layout_textures_pixel_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept, uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesPixel));
      }
    } else {
      // No or unknown pipeline layout previously bound - all bindings are in an
      // indeterminate state.
      current_graphics_descriptor_sets_bound_up_to_date_ = 0;
    }
    current_guest_graphics_pipeline_layout_ = pipeline_layout;
  }

  bool host_render_targets_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  // Get dynamic rasterizer state.
  draw_util::ViewportInfo viewport_info;
  // Just handling maxViewportDimensions is enough - viewportBoundsRange[1] must
  // be at least 2 * max(maxViewportDimensions[0...1]) - 1, and
  // maxViewportDimensions must be greater than or equal to the size of the
  // largest possible framebuffer attachment (if the viewport has positive
  // offset and is between maxViewportDimensions and viewportBoundsRange[1],
  // GetHostViewportInfo will adjust ndc_scale/ndc_offset to clamp it, and the
  // clamped range will be outside the largest possible framebuffer anyway.
  // FIXME(Triang3l): Possibly handle maxViewportDimensions and
  // viewportBoundsRange separately because when using fragment shader
  // interlocks, framebuffers are not used, while the range may be wider than
  // dimensions? Though viewport bigger than 4096 - the smallest possible
  // maximum dimension (which is below the 8192 texture size limit on the Xbox
  // 360) - and with offset, is probably a situation that never happens in real
  // life. Or even disregard the viewport bounds range in the fragment shader
  // interlocks case completely - apply the viewport and the scissor offset
  // directly to pixel address and to things like ps_param_gen.
  draw_util::GetHostViewportInfo(
      regs, draw_resolution_scale_x, draw_resolution_scale_y, false,
      device_properties.maxViewportDimensions[0], device_properties.maxViewportDimensions[1], true,
      normalized_depth_control,
      host_render_targets_used && render_target_cache_->depth_float24_convert_in_pixel_shader(),
      host_render_targets_used, pixel_shader && pixel_shader->writes_depth(), viewport_info);

  // Update dynamic graphics pipeline state.
  UpdateDynamicState(viewport_info, primitive_polygonal, normalized_depth_control);

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  // Whether to load the guest 32-bit (usually big-endian) vertex index
  // indirectly in the vertex shader if full 32-bit indices are not supported by
  // the host.
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
    // PrimitiveProcessor is expected to pre-convert this case to
    // ProcessedIndexBufferType::kHostConverted with host-endian 24-bit
    // indices. Keep drawing, but report if that pre-conversion didn't happen.
    static bool non_vertex_32bit_guest_dma_no_full_uint32_logged = false;
    if (!non_vertex_32bit_guest_dma_no_full_uint32_logged) {
      REXGPU_WARN(
          "Vulkan draw has host vertex shader type {} with guest DMA 32-bit "
          "indices on a device without fullDrawIndexUint32; expected "
          "PrimitiveProcessor to return host-converted indices",
          uint32_t(primitive_processing_result.host_vertex_shader_type));
      non_vertex_32bit_guest_dma_no_full_uint32_logged = true;
    }
  }

  // Update system constants before uploading them.
  UpdateSystemConstantValues(primitive_polygonal, primitive_processing_result,
                             shader_32bit_index_dma, 0, viewport_info, used_texture_mask,
                             normalized_depth_control, normalized_color_mask);

  // Update uniform buffers and descriptor sets after binding the pipeline with
  // the new layout.
  if (!UpdateBindings(vertex_shader, pixel_shader)) {
    return draw_fail("update_bindings");
  }

  // Ensure vertex buffers are resident.
  const Shader::ConstantRegisterMap& constant_map_vertex = vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      uint64_t vfetch_bit = uint64_t(1) << (vfetch_index & 63);
      if (vertex_buffers_in_sync_[vfetch_index >> 6] & vfetch_bit) {
        continue;
      }
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          REXGPU_WARN(
              "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
              "This "
              "is incorrect behavior, but you can try bypassing this by "
              "launching Xenia with --gpu_allow_invalid_fetch_constants=true.",
              vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
        default:
          REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
      }
      VertexBufferState& state = vertex_buffer_states_[vfetch_index];
      if (state.address == vfetch_constant.address && state.size == vfetch_constant.size) {
        vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
        continue;
      }
      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {
        REXGPU_ERROR(
            "Failed to request vertex buffer at 0x{:08X} (size {}) in the shared "
            "memory",
            vfetch_constant.address << 2, vfetch_constant.size << 2);
        return false;
      }
      state.address = vfetch_constant.address;
      state.size = vfetch_constant.size;
      vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
    }
  }

  // Synchronize the memory pages backing memory scatter export streams, and
  // calculate the range that includes the streams for the buffer barrier.
  uint32_t memexport_extent_start = UINT32_MAX, memexport_extent_end = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    uint32_t memexport_range_base_bytes = memexport_range.base_address_dwords << 2;
    if (!shared_memory_->RequestRange(memexport_range_base_bytes, memexport_range.size_bytes)) {
      REXGPU_ERROR(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range_base_bytes, memexport_range.size_bytes);
      return false;
    }
    memexport_extent_start = std::min(memexport_extent_start, memexport_range_base_bytes);
    memexport_extent_end =
        std::max(memexport_extent_end, memexport_range_base_bytes + memexport_range.size_bytes);
  }
  if (memexport_writes_possible && memexport_ranges_.empty()) {
    if (!shared_memory_->RequestRange(0, SharedMemory::kBufferSize)) {
      REXGPU_ERROR(
          "Failed to request full shared memory residency for unresolved "
          "memexport destinations");
      return false;
    }
  }

  ScratchBufferAcquisition guest_dma_index_scratch_buffer;
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      !shader_32bit_index_dma && memexport_writes_possible) {
    // Align with D3D12 behavior where memexporting draws using guest DMA indices
    // read a snapshot of indices to avoid read/write aliasing through shared
    // memory.
    VkDeviceSize guest_dma_index_size =
        VkDeviceSize(primitive_processing_result.host_draw_vertex_count) *
        (primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
             ? sizeof(uint16_t)
             : sizeof(uint32_t));
    if (guest_dma_index_size) {
      guest_dma_index_scratch_buffer = AcquireScratchGpuBuffer(
          guest_dma_index_size, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      if (guest_dma_index_scratch_buffer.buffer() == VK_NULL_HANDLE) {
        return draw_fail("guest_dma_index_scratch_buffer");
      }

      shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
      SubmitBarriers(true);

      VkBufferCopy guest_dma_index_copy_region = {};
      guest_dma_index_copy_region.srcOffset = primitive_processing_result.guest_index_base;
      guest_dma_index_copy_region.dstOffset = 0;
      guest_dma_index_copy_region.size = guest_dma_index_size;
      deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),
                                               guest_dma_index_scratch_buffer.buffer(), 1,
                                               &guest_dma_index_copy_region);
      PushBufferMemoryBarrier(
          guest_dma_index_scratch_buffer.buffer(), 0, guest_dma_index_size,
          guest_dma_index_scratch_buffer.GetStageMask(), VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
          guest_dma_index_scratch_buffer.GetAccessMask(), VK_ACCESS_INDEX_READ_BIT);
      guest_dma_index_scratch_buffer.SetStageMask(VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
      guest_dma_index_scratch_buffer.SetAccessMask(VK_ACCESS_INDEX_READ_BIT);
    }
  }

  // Insert the shared memory barrier if needed.
  // TODO(Triang3l): Find some PM4 command that can be used for indication of
  // when memexports should be awaited instead of inserting the barrier in Use
  // every time if memory export was done in the previous draw?
  if (memexport_extent_start < memexport_extent_end) {
    shared_memory_->Use(
        VulkanSharedMemory::Usage::kGuestDrawReadWrite,
        std::make_pair(memexport_extent_start, memexport_extent_end - memexport_extent_start));
  } else if (memexport_writes_possible) {
    // Stream constants can be invalid or dynamic, making the exact destination
    // unknown. Keep synchronization conservative similarly to D3D12 UAV
    // handling for memexport-capable draws.
    shared_memory_->Use(VulkanSharedMemory::Usage::kGuestDrawReadWrite,
                        std::make_pair(0u, SharedMemory::kBufferSize));
  } else {
    shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  }

  // After all commands that may dispatch, copy or insert barriers, submit
  // the barriers (may end the render pass), and (re)enter the render pass
  // before drawing.
  SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      render_target_cache_->last_update_render_pass(),
      render_target_cache_->last_update_framebuffer());

  // Draw.
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kNone ||
      shader_32bit_index_dma) {
    deferred_command_buffer_.CmdVkDraw(primitive_processing_result.host_draw_vertex_count, 1, 0, 0);
  } else {
    std::pair<VkBuffer, VkDeviceSize> index_buffer;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        if (guest_dma_index_scratch_buffer.buffer() != VK_NULL_HANDLE) {
          index_buffer.first = guest_dma_index_scratch_buffer.buffer();
          index_buffer.second = 0;
        } else {
          index_buffer.first = shared_memory_->buffer();
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
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
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

  // Invalidate textures in memexported memory and watch for changes.
  if (!memexport_ranges_.empty()) {
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                        memexport_range.size_bytes);
    }
  } else if (memexport_writes_possible) {
    // Stream constants can be invalid or dynamic, so exact destinations may be
    // unknown. Keep invalidation conservative in this case.
    shared_memory_->RangeWrittenByGpu(0, SharedMemory::kBufferSize);
  }

  if (IsReadbackMemexportEnabled(REXCVAR_GET(vulkan_readback_memexport)) &&
      !memexport_ranges_.empty()) {
    uint32_t memexport_total_size = 0;
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      memexport_total_size += memexport_range.size_bytes;
    }
    if (memexport_total_size) {
      if (REXCVAR_GET(readback_memexport_fast)) {
        IssueDraw_MemexportReadbackFastPath(memexport_total_size);
      } else {
        IssueDraw_MemexportReadbackFullPath(memexport_total_size);
      }
    }
  }

  return true;
}

bool VulkanCommandProcessor::IssueDraw_MemexportReadbackFullPath(uint32_t total_size) {
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkBuffer readback_buffer = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory = VK_NULL_HANDLE;
  uint32_t readback_memory_type = UINT32_MAX;
  VkDeviceSize readback_memory_size = 0;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, total_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, readback_buffer, readback_memory,
          &readback_memory_type, &readback_memory_size)) {
    REXGPU_ERROR("Failed to create a Vulkan memexport readback buffer");
    return true;
  }

  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  SubmitBarriers(true);

  uint32_t readback_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    VkBufferCopy readback_region = {};
    readback_region.srcOffset = memexport_range.base_address_dwords << 2;
    readback_region.dstOffset = readback_offset;
    readback_region.size = memexport_range.size_bytes;
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(), readback_buffer, 1,
                                             &readback_region);
    readback_offset += memexport_range.size_bytes;
  }
  PushBufferMemoryBarrier(readback_buffer, 0, total_size, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_HOST_READ_BIT);

  if (!AwaitAllQueueOperationsCompletion()) {
    REXGPU_ERROR("Failed to await completion of Vulkan memexport readback");
    dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
    dfn.vkFreeMemory(device, readback_memory, nullptr);
    return true;
  }

  void* readback_mapping = nullptr;
  if (dfn.vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, &readback_mapping) !=
      VK_SUCCESS) {
    REXGPU_ERROR("Failed to map a Vulkan memexport readback buffer");
    dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
    dfn.vkFreeMemory(device, readback_memory, nullptr);
    return true;
  }

  if (!(vulkan_device->memory_types().host_coherent & (uint32_t(1) << readback_memory_type))) {
    VkMappedMemoryRange readback_memory_range = {};
    readback_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    readback_memory_range.memory = readback_memory;
    readback_memory_range.offset = 0;
    readback_memory_range.size = std::min(
        rex::round_up(VkDeviceSize(total_size), vulkan_device->properties().nonCoherentAtomSize),
        readback_memory_size);
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &readback_memory_range);
  }

  const uint8_t* readback_bytes = reinterpret_cast<const uint8_t*>(readback_mapping);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }
  dfn.vkUnmapMemory(device, readback_memory);
  dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
  dfn.vkFreeMemory(device, readback_memory, nullptr);
  return true;
}

bool VulkanCommandProcessor::IssueDraw_MemexportReadbackFastPath(uint32_t total_size) {
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  const uint64_t readback_key =
      MakeMemexportReadbackKey(memexport_ranges_.front().base_address_dwords, total_size);
  ReadbackBuffer& readback = memexport_readback_buffers_[readback_key];
  readback.last_used_frame = frame_current_;

  auto ensure_readback_slot = [&](uint32_t index, uint32_t size) -> bool {
    if (readback.buffers[index] != VK_NULL_HANDLE && readback.memories[index] != VK_NULL_HANDLE &&
        readback.mapped_data[index] != nullptr && size <= readback.sizes[index]) {
      return true;
    }

    VkBuffer new_buffer = VK_NULL_HANDLE;
    VkDeviceMemory new_memory = VK_NULL_HANDLE;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback, new_buffer, new_memory)) {
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

  const uint32_t write_index = readback.current_index;
  const uint32_t read_index = 1 - write_index;
  const uint32_t readback_size = AlignReadbackBufferSize(total_size);
  if (!ensure_readback_slot(write_index, readback_size)) {
    return IssueDraw_MemexportReadbackFullPath(total_size);
  }

  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  SubmitBarriers(true);

  uint32_t readback_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    VkBufferCopy readback_region = {};
    readback_region.srcOffset = memexport_range.base_address_dwords << 2;
    readback_region.dstOffset = readback_offset;
    readback_region.size = memexport_range.size_bytes;
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),
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
    IssueDraw_MemexportReadbackFullPath(total_size);
    readback.current_index = read_index;
    return true;
  }

  VkMappedMemoryRange readback_memory_range = {};
  readback_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  readback_memory_range.memory = readback.memories[read_index];
  readback_memory_range.offset = 0;
  readback_memory_range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(device, 1, &readback_memory_range);

  const uint8_t* readback_bytes =
      reinterpret_cast<const uint8_t*>(readback.mapped_data[read_index]);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }

  readback.current_index = read_index;
  return true;
}

bool VulkanCommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (!BeginSubmission(true)) {
    return false;
  }

  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(vulkan_readback_resolve));
  if (readback_mode == ReadbackResolveMode::kDisabled) {
    uint32_t written_address, written_length;
    return render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                         written_address, written_length);
  }

  return IssueCopy_ReadbackResolvePath();
}

bool VulkanCommandProcessor::IssueCopy_ReadbackResolvePath() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  uint32_t written_address, written_length;
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

  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(vulkan_readback_resolve));
  if (readback_mode == ReadbackResolveMode::kDisabled) {
    return true;
  }

  auto ensure_readback_slot = [&](ReadbackBuffer& readback, uint32_t index, uint32_t size) -> bool {
    if (readback.buffers[index] != VK_NULL_HANDLE && size <= readback.sizes[index] &&
        readback.mapped_data[index] != nullptr) {
      return true;
    }

    VkBuffer new_buffer = VK_NULL_HANDLE;
    VkDeviceMemory new_memory = VK_NULL_HANDLE;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback, new_buffer, new_memory)) {
      REXGPU_ERROR("Failed to create a {} MB Vulkan resolve readback buffer", size >> 20);
      return false;
    }

    void* new_mapping = nullptr;
    if (dfn.vkMapMemory(device, new_memory, 0, VK_WHOLE_SIZE, 0, &new_mapping) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to map a Vulkan resolve readback buffer");
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
  uint64_t resolve_key = MakeReadbackResolveKey(written_address, written_length);
  ReadbackBuffer& readback = readback_buffers_[resolve_key];
  readback.last_used_frame = frame_current_;
  uint32_t write_index = readback.current_index;
  uint32_t readback_size = AlignReadbackBufferSize(written_length);
  if (!ensure_readback_slot(readback, write_index, readback_size)) {
    return true;
  }

  if (is_scaled) {
    if (!resolve_downscale_pipeline_ || !resolve_downscale_pipeline_layout_) {
      return true;
    }

    reg::RB_COPY_DEST_INFO copy_dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
    const FormatInfo* format_info = FormatInfo::Get(uint32_t(copy_dest_info.copy_dest_format));
    uint32_t bits_per_pixel = format_info->bits_per_pixel;
    if (bits_per_pixel != 8 && bits_per_pixel != 16 && bits_per_pixel != 32 &&
        bits_per_pixel != 64) {
      return true;
    }

    uint32_t pixel_size_log2;
    if (!rex::bit_scan_forward(bits_per_pixel >> 3, &pixel_size_log2)) {
      return true;
    }
    uint32_t tile_size_1x = 32 * 32 * (uint32_t(1) << pixel_size_log2);
    uint32_t tile_count = written_length / tile_size_1x;
    if (!tile_count) {
      return true;
    }

    uint64_t scaled_start = 0, scaled_length = 0;
    if (!texture_cache_->GetScaledResolveRange(written_address, written_length, 0, scaled_start,
                                               scaled_length)) {
      return true;
    }
    if (!scaled_length) {
      return true;
    }

    VkBuffer scaled_resolve_buffer = texture_cache_->scaled_resolve_buffer();
    if (scaled_resolve_buffer == VK_NULL_HANDLE || scaled_start > uint64_t(UINT32_MAX)) {
      return true;
    }

    uint32_t downscale_buffer_size = AlignReadbackBufferSize(written_length);
    if (downscale_buffer_size > resolve_downscale_buffer_size_) {
      VkBuffer new_buffer = VK_NULL_HANDLE;
      VkDeviceMemory new_memory = VK_NULL_HANDLE;
      if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device, downscale_buffer_size,
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              ui::vulkan::util::MemoryPurpose::kDeviceLocal, new_buffer, new_memory)) {
        REXGPU_ERROR("Failed to create a {} MB Vulkan resolve downscale buffer",
                     downscale_buffer_size >> 20);
        return true;
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
    for (uint32_t i = 0; i < 2; ++i) {
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
    constants.scale_x = texture_cache_->draw_resolution_scale_x();
    constants.scale_y = texture_cache_->draw_resolution_scale_y();
    constants.pixel_size_log2 = pixel_size_log2;
    constants.tile_count = tile_count;
    constants.source_offset_bytes = uint32_t(scaled_start);
    constants.half_pixel_offset = (REXCVAR_GET(readback_resolve_half_pixel_offset) &&
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
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),
                                             readback.buffers[write_index], 1, &readback_region);
  }

  PushBufferMemoryBarrier(readback.buffers[write_index], 0, written_length,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);

  bool use_delayed_sync =
      readback_mode == ReadbackResolveMode::kFast || readback_mode == ReadbackResolveMode::kSome;
  uint32_t read_index = write_index;
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

    uint8_t* destination = memory_->TranslatePhysical(written_address);
    if (destination) {
      std::memcpy(destination, readback.mapped_data[read_index], written_length);
    }
  }

  readback.current_index = 1 - readback.current_index;
  return true;
}

void VulkanCommandProcessor::EvictOldReadbackBuffers(
    std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map) {
  if (buffer_map.empty()) {
    return;
  }

  const uint64_t eviction_frame_floor = (frame_current_ > kReadbackBufferEvictionAgeFrames)
                                            ? (frame_current_ - kReadbackBufferEvictionAgeFrames)
                                            : 0;
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (auto it = buffer_map.begin(); it != buffer_map.end();) {
    ReadbackBuffer& readback = it->second;
    bool evict =
        buffer_map.size() > kMaxReadbackBuffers || readback.last_used_frame < eviction_frame_floor;
    if (!evict) {
      ++it;
      continue;
    }
    for (uint32_t i = 0; i < 2; ++i) {
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

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  VkQueryPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  pool_info.queryType = VK_QUERY_TYPE_OCCLUSION;
  pool_info.queryCount = kMaxOcclusionQueries;
  if (dfn.vkCreateQueryPool(device, &pool_info, nullptr, &occlusion_query_pool_) != VK_SUCCESS) {
    REXGPU_WARN(
        "VulkanCommandProcessor: Failed to create occlusion query pool, using fake sample counts");
    return false;
  }

  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, sizeof(uint64_t) * kMaxOcclusionQueries, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, occlusion_query_readback_buffer_,
          occlusion_query_readback_memory_, &occlusion_query_readback_memory_type_,
          &occlusion_query_readback_memory_size_)) {
    REXGPU_WARN(
        "VulkanCommandProcessor: Failed to create occlusion query readback buffer, using fake "
        "sample counts");
    ShutdownOcclusionQueryResources();
    return false;
  }

  if (dfn.vkMapMemory(device, occlusion_query_readback_memory_, 0, VK_WHOLE_SIZE, 0,
                      reinterpret_cast<void**>(&occlusion_query_readback_mapping_)) != VK_SUCCESS) {
    REXGPU_WARN(
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

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

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

bool VulkanCommandProcessor::AcquireOcclusionQueryIndex(uint32_t& host_index_out) {
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

bool VulkanCommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||
      occlusion_query_pool_ == VK_NULL_HANDLE || occlusion_query_readback_mapping_ == nullptr) {
    return false;
  }
  if (active_occlusion_query_.valid) {
    REXGPU_WARN(
        "VulkanCommandProcessor: Occlusion query begin issued while another query is active");
    DisableHostOcclusionQueries();
    return false;
  }

  uint32_t host_index = 0;
  if (!AcquireOcclusionQueryIndex(host_index)) {
    return false;
  }
  if (!BeginSubmission(true)) {
    return false;
  }

  EndRenderPass();

  DeferredCommandBuffer& command_buffer = deferred_command_buffer();
  command_buffer.CmdVkResetQueryPool(occlusion_query_pool_, host_index, 1);
  command_buffer.CmdVkBeginQuery(occlusion_query_pool_, host_index, 0);
  active_occlusion_query_.sample_count_address = sample_count_address;
  active_occlusion_query_.host_index = host_index;
  active_occlusion_query_.valid = true;
  return true;
}

bool VulkanCommandProcessor::EndGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||
      !active_occlusion_query_.valid || occlusion_query_pool_ == VK_NULL_HANDLE ||
      occlusion_query_readback_mapping_ == nullptr) {
    return false;
  }

  uint32_t host_index = active_occlusion_query_.host_index;
  active_occlusion_query_ = {};

  if (!BeginSubmission(true)) {
    return false;
  }

  EndRenderPass();

  DeferredCommandBuffer& command_buffer = deferred_command_buffer();
  command_buffer.CmdVkEndQuery(occlusion_query_pool_, host_index);
  command_buffer.CmdVkCopyQueryPoolResults(occlusion_query_pool_, host_index, 1,
                                           occlusion_query_readback_buffer_,
                                           sizeof(uint64_t) * host_index, sizeof(uint64_t),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

  if (!EndSubmission(false)) {
    return false;
  }
  if (!AwaitAllQueueOperationsCompletion()) {
    return false;
  }

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (!(vulkan_device->memory_types().host_coherent &
        (uint32_t(1) << occlusion_query_readback_memory_type_))) {
    VkMappedMemoryRange memory_range = {};
    memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    memory_range.memory = occlusion_query_readback_memory_;
    memory_range.offset = 0;
    memory_range.size =
        std::min(rex::round_up(VkDeviceSize(sizeof(uint64_t) * kMaxOcclusionQueries),
                               vulkan_device->properties().nonCoherentAtomSize),
                 occlusion_query_readback_memory_size_);
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &memory_range);
  }

  const uint64_t* results = reinterpret_cast<const uint64_t*>(occlusion_query_readback_mapping_);
  uint64_t samples = NormalizeOcclusionSamples(results[host_index]);
  WriteGuestOcclusionResult(sample_count_address, samples);
  return true;
}

uint64_t VulkanCommandProcessor::NormalizeOcclusionSamples(uint64_t samples) const {
  if (samples == 0 || !texture_cache_) {
    return samples;
  }
  uint64_t scale_x = texture_cache_->draw_resolution_scale_x();
  uint64_t scale_y = texture_cache_->draw_resolution_scale_y();
  uint64_t scale = scale_x * scale_y;
  if (scale <= 1) {
    return samples;
  }
  return (samples + (scale >> 1)) / scale;
}

void VulkanCommandProcessor::WriteGuestOcclusionResult(uint32_t sample_count_address,
                                                       uint64_t samples) {
  auto* sample_counts =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_address);
  if (!sample_counts) {
    return;
  }
  uint32_t clamped = samples > uint64_t(UINT32_MAX) ? UINT32_MAX : uint32_t(samples);
  sample_counts->Total_A = clamped;
  sample_counts->Total_B = 0;
  sample_counts->ZPass_A = clamped;
  sample_counts->ZPass_B = 0;
  sample_counts->ZFail_A = 0;
  sample_counts->ZFail_B = 0;
  sample_counts->StencilFail_A = 0;
  sample_counts->StencilFail_B = 0;
}

void VulkanCommandProcessor::InitializeTrace() {
  CommandProcessor::InitializeTrace();

  if (!BeginSubmission(true)) {
    return;
  }
  bool render_target_submitted = render_target_cache_->InitializeTraceSubmitDownloads();
  bool shared_memory_submitted = shared_memory_->InitializeTraceSubmitDownloads();
  if (!render_target_submitted && !shared_memory_submitted) {
    return;
  }
  AwaitAllQueueOperationsCompletion();
  if (render_target_submitted) {
    render_target_cache_->InitializeTraceCompleteDownloads();
  }
  if (shared_memory_submitted) {
    shared_memory_->InitializeTraceCompleteDownloads();
  }
}

void VulkanCommandProcessor::CheckSubmissionFenceAndDeviceLoss(uint64_t await_submission) {
  // Only report once, no need to retry a wait that won't succeed anyway.
  if (device_lost_) {
    return;
  }

  if (await_submission >= GetCurrentSubmission()) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = GetCurrentSubmission() - 1;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  size_t fences_total = submissions_in_flight_fences_.size();
  size_t fences_awaited = 0;
  if (await_submission > submission_completed_) {
    // Await in a blocking way if requested.
    // TODO(Triang3l): Await only one fence. "Fence signal operations that are
    // defined by vkQueueSubmit additionally include in the first
    // synchronization scope all commands that occur earlier in submission
    // order."
    VkResult wait_result =
        dfn.vkWaitForFences(device, uint32_t(await_submission - submission_completed_),
                            submissions_in_flight_fences_.data(), VK_TRUE, UINT64_MAX);
    if (wait_result == VK_SUCCESS) {
      fences_awaited += await_submission - submission_completed_;
    } else {
      REXGPU_ERROR("Failed to await submission completion Vulkan fences");
      if (wait_result == VK_ERROR_DEVICE_LOST) {
        device_lost_ = true;
      }
    }
  }
  // Check how far into the submissions the GPU currently is, in order because
  // submission themselves can be executed out of order, but Xenia serializes
  // that for simplicity.
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
      graphics_system_->OnHostGpuLossFromAnyThread(true);
    }
    return;
  }
  if (!fences_awaited) {
    // Not updated - no need to reclaim or download things.
    return;
  }
  // Reclaim fences.
  fences_free_.reserve(fences_free_.size() + fences_awaited);
  auto submissions_in_flight_fences_awaited_end = submissions_in_flight_fences_.cbegin();
  std::advance(submissions_in_flight_fences_awaited_end, fences_awaited);
  fences_free_.insert(fences_free_.cend(), submissions_in_flight_fences_.cbegin(),
                      submissions_in_flight_fences_awaited_end);
  submissions_in_flight_fences_.erase(submissions_in_flight_fences_.cbegin(),
                                      submissions_in_flight_fences_awaited_end);
  submission_completed_ += fences_awaited;

  // Reclaim semaphores.
  while (!submissions_in_flight_semaphores_.empty()) {
    const auto& semaphore_submission = submissions_in_flight_semaphores_.front();
    if (semaphore_submission.first > submission_completed_) {
      break;
    }
    semaphores_free_.push_back(semaphore_submission.second);
    submissions_in_flight_semaphores_.pop_front();
  }

  // Reclaim command pools.
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

  // Destroy objects scheduled for destruction.
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
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_lost_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame. Also check whether the device
  // is still available, and whether the await was successful.
  uint64_t await_submission =
      is_opening_frame ? closed_frame_submissions_[frame_current_ % kMaxFramesInFlight] : 0;
  CheckSubmissionFenceAndDeviceLoss(await_submission);
  if (device_lost_ || submission_completed_ < await_submission) {
    return false;
  }

  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kMaxFramesInFlight)) - kMaxFramesInFlight;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_; ++frame) {
      if (closed_frame_submissions_[frame % kMaxFramesInFlight] > submission_completed_) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;

    // Start a new deferred command buffer - will submit it to the real one in
    // the end of the submission (when async pipeline object creation requests
    // are fulfilled).
    deferred_command_buffer_.Reset();

    // Reset cached state of the command buffer.
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

    // Reset bindings that depend on transient data.
    std::memset(current_float_constant_map_vertex_, 0, sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
    std::memset(current_graphics_descriptor_sets_, 0, sizeof(current_graphics_descriptor_sets_));
    current_constant_buffers_up_to_date_ = 0;
    current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
        shared_memory_and_edram_descriptor_set_;
    current_graphics_descriptor_set_values_up_to_date_ =
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram;

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    // FIXME(Triang3l): This will result in a memory leak if the guest is not
    // presenting.
    uniform_buffer_pool_->Reclaim(frame_completed_);
    while (!single_transient_descriptors_used_.empty()) {
      const UsedSingleTransientDescriptor& used_transient_descriptor =
          single_transient_descriptors_used_.front();
      if (used_transient_descriptor.frame > frame_completed_) {
        break;
      }
      single_transient_descriptors_free_[size_t(used_transient_descriptor.layout)].push_back(
          used_transient_descriptor.set);
      single_transient_descriptors_used_.pop_front();
    }
    while (!constants_transient_descriptors_used_.empty()) {
      const std::pair<uint64_t, VkDescriptorSet>& used_transient_descriptor =
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
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Make sure everything needed for submitting exist.
  if (submission_open_) {
    if (fences_free_.empty()) {
      VkFenceCreateInfo fence_create_info;
      fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      fence_create_info.pNext = nullptr;
      fence_create_info.flags = 0;
      VkFence fence;
      if (dfn.vkCreateFence(device, &fence_create_info, nullptr, &fence) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to create a Vulkan fence");
        // Try to submit later. Completely dropping the submission is not
        // permitted because resources would be left in an undefined state.
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
        REXGPU_ERROR("Failed to create a Vulkan semaphore");
        return false;
      }
      semaphores_free_.push_back(semaphore);
    }
    if (command_buffers_writable_.empty()) {
      CommandBuffer command_buffer;
      VkCommandPoolCreateInfo command_pool_create_info;
      command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      command_pool_create_info.pNext = nullptr;
      command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      command_pool_create_info.queueFamilyIndex = vulkan_device->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr,
                                  &command_buffer.pool) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to create a Vulkan command pool");
        return false;
      }
      VkCommandBufferAllocateInfo command_buffer_allocate_info;
      command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      command_buffer_allocate_info.pNext = nullptr;
      command_buffer_allocate_info.commandPool = command_buffer.pool;
      command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_buffer_allocate_info.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info,
                                       &command_buffer.buffer) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to allocate a Vulkan command buffer");
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
    assert_false(scratch_buffer_used_);

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

    // Submit sparse binds earlier, before executing the deferred command
    // buffer, to reduce latency.
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
      assert_false(semaphores_free_.empty());
      VkSemaphore bind_sparse_semaphore = semaphores_free_.back();
      VkBindSparseInfo bind_sparse_info;
      bind_sparse_info.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
      bind_sparse_info.pNext = nullptr;
      bind_sparse_info.waitSemaphoreCount = 0;
      bind_sparse_info.pWaitSemaphores = nullptr;
      bind_sparse_info.bufferBindCount = uint32_t(sparse_buffer_bind_infos_temp_.size());
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
        ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
            vulkan_device->AcquireQueue(vulkan_device->queue_family_sparse_binding(), 0);
        bind_sparse_result =
            dfn.vkQueueBindSparse(queue_acquisition.queue(), 1, &bind_sparse_info, VK_NULL_HANDLE);
      }
      if (bind_sparse_result != VK_SUCCESS) {
        REXGPU_ERROR("Failed to submit Vulkan sparse binds");
        return false;
      }
      current_submission_wait_semaphores_.push_back(bind_sparse_semaphore);
      semaphores_free_.pop_back();
      current_submission_wait_stage_masks_.push_back(sparse_bind_wait_stage_mask_);
      sparse_bind_wait_stage_mask_ = 0;
      sparse_buffer_binds_.clear();
      sparse_memory_binds_.clear();
    }

    SubmitBarriers(true);

    assert_false(command_buffers_writable_.empty());
    CommandBuffer command_buffer = command_buffers_writable_.back();
    if (dfn.vkResetCommandPool(device, command_buffer.pool, 0) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to reset a Vulkan command pool");
      return false;
    }
    VkCommandBufferBeginInfo command_buffer_begin_info;
    command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_buffer_begin_info.pNext = nullptr;
    command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    command_buffer_begin_info.pInheritanceInfo = nullptr;
    if (dfn.vkBeginCommandBuffer(command_buffer.buffer, &command_buffer_begin_info) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to begin a Vulkan command buffer");
      return false;
    }
    deferred_command_buffer_.Execute(command_buffer.buffer);
    if (dfn.vkEndCommandBuffer(command_buffer.buffer) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to end a Vulkan command buffer");
      return false;
    }

    VkSubmitInfo submit_info;
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = nullptr;
    if (!current_submission_wait_semaphores_.empty()) {
      submit_info.waitSemaphoreCount = uint32_t(current_submission_wait_semaphores_.size());
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
    assert_false(fences_free_.empty());
    VkFence fence = fences_free_.back();
    if (dfn.vkResetFences(device, 1, &fence) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to reset a Vulkan submission fence");
      return false;
    }
    VkResult submit_result;
    {
      ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
          vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
      submit_result = dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit_info, fence);
    }
    if (submit_result != VK_SUCCESS) {
      REXGPU_ERROR("Failed to submit a Vulkan command buffer");
      if (submit_result == VK_ERROR_DEVICE_LOST && !device_lost_) {
        device_lost_ = true;
        if (graphics_system_) {
          graphics_system_->OnHostGpuLossFromAnyThread(true);
        }
      }
      return false;
    }
    uint64_t submission_current = GetCurrentSubmission();
    current_submission_wait_stage_masks_.clear();
    for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
      submissions_in_flight_semaphores_.emplace_back(submission_current, semaphore);
    }
    current_submission_wait_semaphores_.clear();
    command_buffers_submitted_.emplace_back(submission_current, command_buffer);
    command_buffers_writable_.pop_back();
    // Increments the current submission number, going to the next submission.
    submissions_in_flight_fences_.push_back(fence);
    fences_free_.pop_back();

    submission_open_ = false;
  }

  if (is_closing_frame) {
    if (REXCVAR_GET(clear_memory_page_state) && shared_memory_) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }
    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kMaxFramesInFlight] = GetCurrentSubmission() - 1;

    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      DestroyScratchBuffer();

      for (SwapFramebuffer& swap_framebuffer : swap_framebuffers_) {
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                               swap_framebuffer.framebuffer);
      }

      assert_true(command_buffers_submitted_.empty());
      for (const CommandBuffer& command_buffer : command_buffers_writable_) {
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
      }
      command_buffers_writable_.clear();

      ClearTransientDescriptorPools();

      uniform_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      render_target_cache_->ClearCache();

      // Not clearing the pipeline layouts and the descriptor set layouts as
      // they're referenced by pipelines, which are not destroyed.

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
  assert_false(scratch_buffer_used_);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  scratch_buffer_last_usage_submission_ = 0;
  scratch_buffer_last_access_mask_ = 0;
  scratch_buffer_last_stage_mask_ = 0;
  scratch_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, scratch_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, scratch_buffer_memory_);
}

bool VulkanCommandProcessor::EnsureSwapFxaaSourceImage(uint32_t width, uint32_t height) {
  assert_true(submission_open_);
  if (!width || !height) {
    return false;
  }
  if (swap_fxaa_source_image_ != VK_NULL_HANDLE && swap_fxaa_source_image_width_ == width &&
      swap_fxaa_source_image_height_ == height) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  if (swap_fxaa_source_image_ != VK_NULL_HANDLE) {
    const uint64_t destroy_submission = swap_fxaa_source_image_submission_;
    const uint64_t deferred_destroy_submission = GetCurrentSubmission();
    if (submission_completed_ >= destroy_submission) {
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device,
                                             swap_fxaa_source_image_view_);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, swap_fxaa_source_image_);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
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
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_create_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          swap_fxaa_source_image_, swap_fxaa_source_image_memory_)) {
    REXGPU_ERROR("Failed to create the FXAA source image");
    return false;
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
  image_view_create_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &image_view_create_info, nullptr,
                            &swap_fxaa_source_image_view_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the FXAA source image view");
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, swap_fxaa_source_image_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
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
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device,
                                         swap_fxaa_source_image_view_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, swap_fxaa_source_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, swap_fxaa_source_image_memory_);
  swap_fxaa_source_image_width_ = 0;
  swap_fxaa_source_image_height_ = 0;
  swap_fxaa_source_image_submission_ = 0;
  swap_fxaa_source_stage_mask_ = 0;
  swap_fxaa_source_access_mask_ = 0;
  swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanCommandProcessor::UpdateDynamicState(const draw_util::ViewportInfo& viewport_info,
                                                bool primitive_polygonal,
                                                reg::RB_DEPTHCONTROL normalized_depth_control) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  uint32_t draw_resolution_scale_x = texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y = texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;

  // Window parameters.
  // http://ftp.tku.edu.tw/NetBSD/NetBSD-current/xsrc/external/mit/xf86-video-ati/dist/src/r600_reg_auto_r6xx.h
  // See r200UpdateWindow:
  // https://github.com/freedreno/mesa/blob/master/src/mesa/drivers/dri/r200/r200_state.c
  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();

  // Viewport.
  VkViewport viewport;
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    viewport.x = float(viewport_info.xy_offset[0]);
    viewport.y = float(viewport_info.xy_offset[1]);
    viewport.width = float(viewport_info.xy_extent[0]);
    viewport.height = float(viewport_info.xy_extent[1]);
  } else {
    // Vulkan viewport width must be greater than 0.0f, but the Xenia  viewport
    // may be empty for various reasons - set the viewport to outside the
    // framebuffer.
    viewport.x = -1.0f;
    viewport.y = -1.0f;
    viewport.width = 1.0f;
    viewport.height = 1.0f;
  }
  viewport.minDepth = viewport_info.z_min;
  viewport.maxDepth = viewport_info.z_max;
  SetViewport(viewport);

  // Scissor.
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;
  VkRect2D scissor_rect;
  scissor_rect.offset.x = int32_t(scissor.offset[0]);
  scissor_rect.offset.y = int32_t(scissor.offset[1]);
  scissor_rect.extent.width = scissor.extent[0];
  scissor_rect.extent.height = scissor.extent[1];
  SetScissor(scissor_rect);

  if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    // Depth bias.
    float depth_bias_constant_factor, depth_bias_slope_factor;
    draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, depth_bias_slope_factor,
                                             depth_bias_constant_factor);
    depth_bias_constant_factor *=
        regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8
            ? draw_util::kD3D10PolygonOffsetFactorUnorm24
            : draw_util::kD3D10PolygonOffsetFactorFloat24;
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    depth_bias_slope_factor *= xenos::kPolygonOffsetScaleSubpixelUnit *
                               float(std::max(draw_resolution_scale_x, draw_resolution_scale_y));
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    dynamic_depth_bias_update_needed_ |=
        std::memcmp(&dynamic_depth_bias_constant_factor_, &depth_bias_constant_factor,
                    sizeof(float)) != 0;
    dynamic_depth_bias_update_needed_ |= std::memcmp(&dynamic_depth_bias_slope_factor_,
                                                     &depth_bias_slope_factor, sizeof(float)) != 0;
    if (dynamic_depth_bias_update_needed_) {
      dynamic_depth_bias_constant_factor_ = depth_bias_constant_factor;
      dynamic_depth_bias_slope_factor_ = depth_bias_slope_factor;
      deferred_command_buffer_.CmdVkSetDepthBias(dynamic_depth_bias_constant_factor_, 0.0f,
                                                 dynamic_depth_bias_slope_factor_);
      dynamic_depth_bias_update_needed_ = false;
    }

    // Blend constants.
    float blend_constants[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    dynamic_blend_constants_update_needed_ |=
        std::memcmp(dynamic_blend_constants_, blend_constants, sizeof(float) * 4) != 0;
    if (dynamic_blend_constants_update_needed_) {
      std::memcpy(dynamic_blend_constants_, blend_constants, sizeof(float) * 4);
      deferred_command_buffer_.CmdVkSetBlendConstants(dynamic_blend_constants_);
      dynamic_blend_constants_update_needed_ = false;
    }

    // Stencil masks and references.
    // Due to pretty complex conditions involving registers not directly related
    // to stencil (primitive type, culling), changing the values only when
    // stencil is actually needed. However, due to the way dynamic state needs
    // to be set in Vulkan, which doesn't take into account whether the state
    // actually has effect on drawing, and because the masks and the references
    // are always dynamic in Xenia guest pipelines, they must be set in the
    // command buffer before any draw.
    if (normalized_depth_control.stencil_enable) {
      Register stencil_ref_mask_front_reg, stencil_ref_mask_back_reg;
      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        if (GetVulkanDevice()->properties().separateStencilMaskRef) {
          stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
        } else {
          // Choose the back face values only if drawing only back faces.
          stencil_ref_mask_front_reg = regs.Get<reg::PA_SU_SC_MODE_CNTL>().cull_front
                                           ? XE_GPU_REG_RB_STENCILREFMASK_BF
                                           : XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = stencil_ref_mask_front_reg;
        }
      } else {
        stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
        stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK;
      }
      auto stencil_ref_mask_front = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_front_reg);
      auto stencil_ref_mask_back = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_back_reg);
      // Compare mask.
      dynamic_stencil_compare_mask_front_update_needed_ |=
          dynamic_stencil_compare_mask_front_ != stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_front_ = stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_back_update_needed_ |=
          dynamic_stencil_compare_mask_back_ != stencil_ref_mask_back.stencilmask;
      dynamic_stencil_compare_mask_back_ = stencil_ref_mask_back.stencilmask;
      // Write mask.
      dynamic_stencil_write_mask_front_update_needed_ |=
          dynamic_stencil_write_mask_front_ != stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_front_ = stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_back_update_needed_ |=
          dynamic_stencil_write_mask_back_ != stencil_ref_mask_back.stencilwritemask;
      dynamic_stencil_write_mask_back_ = stencil_ref_mask_back.stencilwritemask;
      // Reference.
      dynamic_stencil_reference_front_update_needed_ |=
          dynamic_stencil_reference_front_ != stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_front_ = stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_back_update_needed_ |=
          dynamic_stencil_reference_back_ != stencil_ref_mask_back.stencilref;
      dynamic_stencil_reference_back_ = stencil_ref_mask_back.stencilref;
    }
    // Using VK_STENCIL_FACE_FRONT_AND_BACK for higher safety when running on
    // the Vulkan portability subset without separateStencilMaskRef.
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

  // TODO(Triang3l): VK_EXT_extended_dynamic_state and
  // VK_EXT_extended_dynamic_state2.
}

void VulkanCommandProcessor::UpdateSystemConstantValues(
    bool primitive_polygonal,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool shader_32bit_index_dma, uint32_t compute_memexport_vertex_count,
    const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  auto vgt_indx_offset = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  auto vgt_max_vtx_indx = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
  auto vgt_min_vtx_indx = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);

  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  // Get the color info register values for each render target. Also, for FSI,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  reg::RB_COLOR_INFO color_infos[xenos::kMaxColorRenderTargets];
  float rt_clamp[4][4];
  // Two UINT32_MAX if no components actually existing in the RT are written.
  uint32_t rt_keep_masks[4][2];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    color_infos[i] = color_info;
    if (edram_fragment_shader_interlock) {
      RenderTargetCache::GetPSIColorFormatInfo(
          color_info.color_format, (normalized_color_mask >> (i * 4)) & 0b1111, rt_clamp[i][0],
          rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3], rt_keep_masks[i][0], rt_keep_masks[i][1]);
    }
  }

  // Disable depth and stencil if it aliases a color render target (for
  // instance, during the XBLA logo in 58410954, though depth writing is already
  // disabled there).
  bool depth_stencil_enabled =
      normalized_depth_control.stencil_enable || normalized_depth_control.z_enable;
  if (edram_fragment_shader_interlock && depth_stencil_enabled) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Vertex index shader loading.
  if (shader_32bit_index_dma) {
    flags |= SpirvShaderTranslator::kSysFlag_VertexIndexLoad;
  }
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA ||
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA ||
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator ::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
    // Point and rectangle expansion uses primitive restart only in the host
    // built-in strip index buffer; guest indices are not reset-based there.
    bool compute_or_primitive_vertex_index_reset =
        primitive_processing_result.host_primitive_reset_enabled &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
    if (compute_or_primitive_vertex_index_reset) {
      flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexReset;
    }
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kTriangleDomainPatchIndexed) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeMemExportPatchIndexInRegister1;
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kTriangleDomainCPIndexed) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeMemExportTriangleCPIndexed;
  } else if (primitive_processing_result.host_vertex_shader_type ==
             Shader::HostVertexShaderType::kQuadDomainCPIndexed) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeMemExportQuadCPIndexed;
  }
  if (primitive_processing_result.tessellation_mode == xenos::TessellationMode::kAdaptive &&
      (primitive_processing_result.host_vertex_shader_type ==
           Shader::HostVertexShaderType::kTriangleDomainPatchIndexed ||
       primitive_processing_result.host_vertex_shader_type ==
           Shader::HostVertexShaderType::kQuadDomainPatchIndexed)) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeMemExportPatchIndexFromInvocation;
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal, and gl_FrontFacing matters.
  if (primitive_polygonal) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // MSAA sample count.
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing.
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    // Keep parity with D3D12: gamma targets in this path are converted via
    // explicit Xenos PWL gamma logic in shaders rather than via host sRGB
    // attachment conversion semantics.
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  if (edram_fragment_shader_interlock && depth_stencil_enabled) {
    flags |= SpirvShaderTranslator::kSysFlag_FSIDepthStencil;
    if (normalized_depth_control.z_enable) {
      flags |= uint32_t(normalized_depth_control.zfunc)
               << SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess_Shift;
      if (normalized_depth_control.z_write_enable) {
        flags |= SpirvShaderTranslator::kSysFlag_FSIDepthWrite;
      }
    } else {
      // In case stencil is used without depth testing - always pass, and
      // don't modify the stored depth.
      flags |= SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess |
               SpirvShaderTranslator::kSysFlag_FSIDepthPassIfEqual |
               SpirvShaderTranslator::kSysFlag_FSIDepthPassIfGreater;
    }
    if (normalized_depth_control.stencil_enable) {
      flags |= SpirvShaderTranslator::kSysFlag_FSIStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= SpirvShaderTranslator::kSysFlag_FSIDepthStencilEarlyWrite;
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Index buffer address for loading in the shaders.
  if (flags & (SpirvShaderTranslator::kSysFlag_VertexIndexLoad |
               SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)) {
    dirty |=
        system_constants_.vertex_index_load_address != primitive_processing_result.guest_index_base;
    system_constants_.vertex_index_load_address = primitive_processing_result.guest_index_base;
  }

  // Primitive reset index for shader-side index loading.
  uint32_t vertex_index_reset =
      (flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexReset)
          ? regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx
          : 0;
  dirty |= system_constants_.vertex_index_reset != vertex_index_reset;
  system_constants_.vertex_index_reset = vertex_index_reset;
  dirty |= system_constants_.compute_memexport_vertex_count != compute_memexport_vertex_count;
  system_constants_.compute_memexport_vertex_count = compute_memexport_vertex_count;

  // Index or tessellation edge factor buffer endianness.
  xenos::Endian guest_index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16 &&
      guest_index_endian != xenos::Endian::kNone && guest_index_endian != xenos::Endian::k8in16) {
    guest_index_endian =
        guest_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
  }
  xenos::Endian index_endian =
      (flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)
          ? guest_index_endian
          : primitive_processing_result.host_shader_index_endian;
  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  dirty |= system_constants_.line_loop_closing_index !=
           primitive_processing_result.line_loop_closing_index;
  system_constants_.line_loop_closing_index = primitive_processing_result.line_loop_closing_index;

  // Vertex index offset.
  dirty |= system_constants_.vertex_base_index != vgt_indx_offset;
  system_constants_.vertex_base_index = vgt_indx_offset;

  // Vertex index range.
  dirty |= system_constants_.vertex_index_min != vgt_min_vtx_indx;
  dirty |= system_constants_.vertex_index_max != vgt_max_vtx_indx;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // User clip planes (UCP_ENA_#), when not CLIP_DISABLE.
  // The shader knows only the total count - tightly packing the user clip
  // planes that are actually used.
  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  // Tessellation factor range, plus 1.0 according to
  // https://www.slideshare.net/blackdevilvikas/next-generation-graphics-programming-on-xbox-360.
  float tessellation_factor_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min != tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max != tessellation_factor_max;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Conversion to host normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x = float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y = float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min != point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max != point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] != point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] != point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  uint32_t textures_resolution_scaled = 0;
  {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
      uint32_t texture_signs_shift = 8 * (texture_index & 3);
      uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      uint32_t texture_signs_shifted = uint32_t(texture_signs) << texture_signs_shift;
      uint32_t texture_signs_mask = ((UINT32_C(1) << 8) - 1) << texture_signs_shift;
      dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
      texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
      textures_resolution_scaled |=
          uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
    }
  }
  dirty |= system_constants_.textures_resolution_scaled != textures_resolution_scaled;
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;

  // Texture host swizzle in the shader.
  if (!GetVulkanDevice()->properties().imageViewFormatSwizzle) {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_swizzles_uint = system_constants_.texture_swizzles[texture_index >> 1];
      uint32_t texture_swizzle_shift = 12 * (texture_index & 1);
      uint32_t texture_swizzle = texture_cache_->GetActiveTextureHostSwizzle(texture_index);
      uint32_t texture_swizzle_shifted = uint32_t(texture_swizzle) << texture_swizzle_shift;
      uint32_t texture_swizzle_mask = ((UINT32_C(1) << 12) - 1) << texture_swizzle_shift;
      dirty |= (texture_swizzles_uint & texture_swizzle_mask) != texture_swizzle_shifted;
      texture_swizzles_uint =
          (texture_swizzles_uint & ~texture_swizzle_mask) | texture_swizzle_shifted;
    }
  }

  // Alpha test.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  uint32_t alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8) : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  uint32_t edram_tile_dwords_scaled = xenos::kEdramTileWidthSamples *
                                      xenos::kEdramTileHeightSamples *
                                      (draw_resolution_scale_x * draw_resolution_scale_y);

  // EDRAM pitch for FSI render target writing.
  if (edram_fragment_shader_interlock) {
    // Align, then multiply by 32bpp tile size in dwords.
    uint32_t edram_32bpp_tile_pitch_dwords_scaled =
        ((rb_surface_info.surface_pitch *
          (rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1)) +
         (xenos::kEdramTileWidthSamples - 1)) /
        xenos::kEdramTileWidthSamples * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_32bpp_tile_pitch_dwords_scaled !=
             edram_32bpp_tile_pitch_dwords_scaled;
    system_constants_.edram_32bpp_tile_pitch_dwords_scaled = edram_32bpp_tile_pitch_dwords_scaled;
  }

  // Color exponent bias and FSI render target writing.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets &&
        (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 &&
             !render_target_cache_->IsFixedRG16TruncatedToMinus1To1() ||
         color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
             !render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1())) {
      // Remap from -32...32 to -1...1 by dividing the output values by 32,
      // losing blending correctness, but getting the full range.
      color_exp_bias -= 5;
    }
    float color_exp_bias_scale;
    *reinterpret_cast<int32_t*>(&color_exp_bias_scale) =
        UINT32_C(0x3F800000) + (color_exp_bias << 23);
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
    if (edram_fragment_shader_interlock) {
      dirty |= system_constants_.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
      system_constants_.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      dirty |= system_constants_.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
      system_constants_.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX) {
        uint32_t rt_base_dwords_scaled = color_info.color_base * edram_tile_dwords_scaled;
        dirty |= system_constants_.edram_rt_base_dwords_scaled[i] != rt_base_dwords_scaled;
        system_constants_.edram_rt_base_dwords_scaled[i] = rt_base_dwords_scaled;
        uint32_t format_flags = RenderTargetCache::AddPSIColorFormatFlags(color_info.color_format);
        dirty |= system_constants_.edram_rt_format_flags[i] != format_flags;
        system_constants_.edram_rt_format_flags[i] = format_flags;
        uint32_t blend_factors_ops =
            regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
        dirty |= system_constants_.edram_rt_blend_factors_ops[i] != blend_factors_ops;
        system_constants_.edram_rt_blend_factors_ops[i] = blend_factors_ops;
        // Can't do float comparisons here because NaNs would result in always
        // setting the dirty flag.
        dirty |=
            std::memcmp(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float)) != 0;
        std::memcpy(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float));
      }
    }
  }

  if (edram_fragment_shader_interlock) {
    uint32_t depth_base_dwords_scaled = rb_depth_info.depth_base * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_depth_base_dwords_scaled != depth_base_dwords_scaled;
    system_constants_.edram_depth_base_dwords_scaled = depth_base_dwords_scaled;

    // For non-polygons, front polygon offset is used, and it's enabled if
    // POLY_OFFSET_PARA_ENABLED is set, for polygons, separate front and back
    // are used.
    float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
        poly_offset_back_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
        poly_offset_back_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else {
      if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
        poly_offset_back_scale = poly_offset_front_scale;
        poly_offset_back_offset = poly_offset_front_offset;
      }
    }
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    float poly_offset_scale_factor = xenos::kPolygonOffsetScaleSubpixelUnit *
                                     std::max(draw_resolution_scale_x, draw_resolution_scale_y);
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
      uint32_t stencil_front_reference_masks = rb_stencilrefmask.value & 0xFFFFFF;
      dirty |=
          system_constants_.edram_stencil_front_reference_masks != stencil_front_reference_masks;
      system_constants_.edram_stencil_front_reference_masks = stencil_front_reference_masks;
      uint32_t stencil_func_ops = (normalized_depth_control.value >> 8) & ((1 << 12) - 1);
      dirty |= system_constants_.edram_stencil_front_func_ops != stencil_func_ops;
      system_constants_.edram_stencil_front_func_ops = stencil_func_ops;

      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        uint32_t stencil_back_reference_masks = rb_stencilrefmask_bf.value & 0xFFFFFF;
        dirty |=
            system_constants_.edram_stencil_back_reference_masks != stencil_back_reference_masks;
        system_constants_.edram_stencil_back_reference_masks = stencil_back_reference_masks;
        uint32_t stencil_func_ops_bf = (normalized_depth_control.value >> 20) & ((1 << 12) - 1);
        dirty |= system_constants_.edram_stencil_back_func_ops != stencil_func_ops_bf;
        system_constants_.edram_stencil_back_func_ops = stencil_func_ops_bf;
      } else {
        dirty |= std::memcmp(system_constants_.edram_stencil_back,
                             system_constants_.edram_stencil_front, 2 * sizeof(uint32_t)) != 0;
        std::memcpy(system_constants_.edram_stencil_back, system_constants_.edram_stencil_front,
                    2 * sizeof(uint32_t));
      }
    }

    dirty |= system_constants_.edram_blend_constant[0] != regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    system_constants_.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    dirty |=
        system_constants_.edram_blend_constant[1] != regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    system_constants_.edram_blend_constant[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    dirty |= system_constants_.edram_blend_constant[2] != regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    system_constants_.edram_blend_constant[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    dirty |=
        system_constants_.edram_blend_constant[3] != regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
    system_constants_.edram_blend_constant[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }

  if (dirty) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
  }
}

bool VulkanCommandProcessor::UpdateBindings(const VulkanShader* vertex_shader,
                                            const VulkanShader* pixel_shader) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Invalidate constant buffers and descriptors for changed data.

  // Float constants.
  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST] == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST] == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST] == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST] == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, any buffer can be reused for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        current_constant_buffers_up_to_date_ &=
            ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
      }
    }
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }

  // Write the new constant buffers.
  constexpr uint32_t kAllConstantBuffersMask =
      (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferCount) - 1;
  assert_zero(current_constant_buffers_up_to_date_ & ~kAllConstantBuffersMask);
  if ((current_constant_buffers_up_to_date_ & kAllConstantBuffersMask) != kAllConstantBuffersMask) {
    current_graphics_descriptor_set_values_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants);
    size_t uniform_buffer_alignment =
        size_t(vulkan_device->properties().minUniformBufferOffsetAlignment);
    // System constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferSystem];
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, sizeof(SpirvShaderTranslator::SystemConstants), uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = sizeof(SpirvShaderTranslator::SystemConstants);
      std::memcpy(mapping, &system_constants_, sizeof(SpirvShaderTranslator::SystemConstants));
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferSystem;
    }
    // Vertex shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferFloatVertex];
      // Even if the shader doesn't need any float constants, a valid binding
      // must still be provided (the pipeline layout always has float constants,
      // for both the vertex shader and the pixel shader), so if the first draw
      // in the frame doesn't have float constants at all, still allocate a
      // dummy buffer.
      size_t float_constants_size =
          sizeof(float) * 4 * std::max(float_constant_count_vertex, UINT32_C(1));
      uint8_t* mapping = uniform_buffer_pool_->Request(frame_current_, float_constants_size,
                                                       uniform_buffer_alignment, buffer_info.buffer,
                                                       buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_size);
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry = current_float_constant_map_vertex_[i];
        uint32_t float_constant_index;
        while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(
              mapping,
              &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (float_constant_index << 2)],
              sizeof(float) * 4);
          mapping += sizeof(float) * 4;
        }
      }
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferFloatVertex;
    }
    // Pixel shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferFloatPixel];
      size_t float_constants_size =
          sizeof(float) * 4 * std::max(float_constant_count_pixel, UINT32_C(1));
      uint8_t* mapping = uniform_buffer_pool_->Request(frame_current_, float_constants_size,
                                                       uniform_buffer_alignment, buffer_info.buffer,
                                                       buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_size);
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry = current_float_constant_map_pixel_[i];
        uint32_t float_constant_index;
        while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(
              mapping,
              &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) + (float_constant_index << 2)],
              sizeof(float) * 4);
          mapping += sizeof(float) * 4;
        }
      }
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferFloatPixel;
    }
    // Bool and loop constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferBoolLoop];
      constexpr size_t kBoolLoopConstantsSize = sizeof(uint32_t) * (8 + 32);
      uint8_t* mapping = uniform_buffer_pool_->Request(frame_current_, kBoolLoopConstantsSize,
                                                       uniform_buffer_alignment, buffer_info.buffer,
                                                       buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kBoolLoopConstantsSize);
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], kBoolLoopConstantsSize);
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferBoolLoop;
    }
    // Fetch constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferFetch];
      constexpr size_t kFetchConstantsSize = sizeof(uint32_t) * 6 * 32;
      uint8_t* mapping = uniform_buffer_pool_->Request(frame_current_, kFetchConstantsSize,
                                                       uniform_buffer_alignment, buffer_info.buffer,
                                                       buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kFetchConstantsSize);
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferFetch;
    }
  }

  // Textures and samplers.
  const std::vector<VulkanShader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  const std::vector<VulkanShader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  uint32_t sampler_count_vertex = uint32_t(samplers_vertex.size());
  uint32_t texture_count_vertex = uint32_t(textures_vertex.size());
  const std::vector<VulkanShader::SamplerBinding>* samplers_pixel;
  const std::vector<VulkanShader::TextureBinding>* textures_pixel;
  uint32_t sampler_count_pixel, texture_count_pixel;
  if (pixel_shader) {
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    sampler_count_pixel = uint32_t(samplers_pixel->size());
    texture_count_pixel = uint32_t(textures_pixel->size());
  } else {
    samplers_pixel = nullptr;
    textures_pixel = nullptr;
    sampler_count_pixel = 0;
    texture_count_pixel = 0;
  }
  // TODO(Triang3l): Reuse texture and sampler bindings if not changed.
  current_graphics_descriptor_set_values_up_to_date_ &=
      ~((UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex) |
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel));

  // Make sure new descriptor sets are bound to the command buffer.

  current_graphics_descriptor_sets_bound_up_to_date_ &=
      current_graphics_descriptor_set_values_up_to_date_;

  // Fill the texture and sampler write image infos.

  bool write_vertex_textures =
      (texture_count_vertex || sampler_count_vertex) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex));
  bool write_pixel_textures =
      (texture_count_pixel || sampler_count_pixel) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel));
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

  // Write the new descriptor sets.

  // Consecutive bindings updated via a single VkWriteDescriptorSet must have
  // identical stage flags, but for the constants they vary. Plus vertex and
  // pixel texture images and samplers.
  std::array<VkWriteDescriptorSet, SpirvShaderTranslator::kConstantBufferCount + 2 * 2>
      write_descriptor_sets;
  uint32_t write_descriptor_set_count = 0;
  uint32_t write_descriptor_set_bits = 0;
  assert_not_zero(current_graphics_descriptor_set_values_up_to_date_ &
                  (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram));
  // Constant buffers.
  if (!(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants))) {
    VkDescriptorSet constants_descriptor_set;
    if (!constants_transient_descriptors_free_.empty()) {
      constants_descriptor_set = constants_transient_descriptors_free_.back();
      constants_transient_descriptors_free_.pop_back();
    } else {
      VkDescriptorPoolSize constants_descriptor_count;
      constants_descriptor_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      constants_descriptor_count.descriptorCount = SpirvShaderTranslator::kConstantBufferCount;
      constants_descriptor_set = transient_descriptor_allocator_uniform_buffer_.Allocate(
          descriptor_set_layout_constants_, &constants_descriptor_count, 1);
      if (constants_descriptor_set == VK_NULL_HANDLE) {
        return false;
      }
    }
    constants_transient_descriptors_used_.emplace_back(frame_current_, constants_descriptor_set);
    // Consecutive bindings updated via a single VkWriteDescriptorSet must have
    // identical stage flags, but for the constants they vary.
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
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
    write_descriptor_set_bits |= UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants;
    current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetConstants] =
        constants_descriptor_set;
  }
  // Vertex shader textures and samplers.
  if (write_vertex_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = WriteTransientTextureBindings(
        true, texture_count_vertex, sampler_count_vertex,
        current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_vertex_ref(),
        descriptor_write_image_info_.data() + vertex_texture_image_info_offset,
        descriptor_write_image_info_.data() + vertex_sampler_image_info_offset, write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |= UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex;
    current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
        write_textures[0].dstSet;
  }
  // Pixel shader textures and samplers.
  if (write_pixel_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = WriteTransientTextureBindings(
        false, texture_count_pixel, sampler_count_pixel,
        current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_pixel_ref(),
        descriptor_write_image_info_.data() + pixel_texture_image_info_offset,
        descriptor_write_image_info_.data() + pixel_sampler_image_info_offset, write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |= UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel;
    current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
        write_textures[0].dstSet;
  }
  // Write.
  if (write_descriptor_set_count) {
    dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count, write_descriptor_sets.data(), 0,
                               nullptr);
  }
  // Only make valid if all descriptor sets have been allocated and written
  // successfully.
  current_graphics_descriptor_set_values_up_to_date_ |= write_descriptor_set_bits;

  // Bind the new descriptor sets.
  uint32_t descriptor_sets_needed = (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetCount) - 1;
  if (!texture_count_vertex && !sampler_count_vertex) {
    descriptor_sets_needed &= ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
  }
  if (!texture_count_pixel && !sampler_count_pixel) {
    descriptor_sets_needed &= ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
  }
  uint32_t descriptor_sets_remaining =
      descriptor_sets_needed & ~current_graphics_descriptor_sets_bound_up_to_date_;
  uint32_t descriptor_set_index;
  while (rex::bit_scan_forward(descriptor_sets_remaining, &descriptor_set_index)) {
    uint32_t descriptor_set_mask_tzcnt =
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

uint32_t VulkanCommandProcessor::WriteTransientTextureBindings(
    bool is_vertex, uint32_t texture_count, uint32_t sampler_count,
    VkDescriptorSetLayout descriptor_set_layout, const VkDescriptorImageInfo* texture_image_info,
    const VkDescriptorImageInfo* sampler_image_info,
    VkWriteDescriptorSet* descriptor_set_writes_out) {
  assert_true(frame_open_);
  if (!texture_count && !sampler_count) {
    return 0;
  }
  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = texture_count;
  texture_descriptor_set_layout_key.sampler_count = sampler_count;
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  VkDescriptorSet texture_descriptor_set;
  auto textures_free_it =
      texture_transient_descriptor_sets_free_.find(texture_descriptor_set_layout_key);
  if (textures_free_it != texture_transient_descriptor_sets_free_.end() &&
      !textures_free_it->second.empty()) {
    texture_descriptor_set = textures_free_it->second.back();
    textures_free_it->second.pop_back();
  } else {
    std::array<VkDescriptorPoolSize, 2> texture_descriptor_counts;
    uint32_t texture_descriptor_counts_count = 0;
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
    assert_not_zero(texture_descriptor_counts_count);
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
  uint32_t descriptor_set_write_count = 0;
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
  assert_not_zero(descriptor_set_write_count);
  return descriptor_set_write_count;
}

}  // namespace rex::graphics::vulkan
