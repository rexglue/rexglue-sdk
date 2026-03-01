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
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/thread.h>
#include <rex/memory.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline_util.h>
#include <rex/graphics/pipeline/shader/spirv_builder.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/pipeline_cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/types.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_INT32(
    vulkan_pipeline_creation_threads, -1, "GPU/Vulkan",
    "Number of pipeline creation threads for Vulkan async pipeline creation (-1 for auto)")
    .range(-1, 32)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vulkan_tessellation_wireframe, false, "GPU/Vulkan",
                    "Render tessellation as wireframe")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::vulkan {

namespace {

constexpr std::string_view kPlaceholderPixelShaderSource = R"(#version 460
layout(location = 0) out vec4 oC0;
void main() {
  // Avoid full-screen black flashes while async pipelines are warming up.
  discard;
}
)";

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

std::string GetTessellationSystemConstantsBlockGlsl() {
  std::string source;
  source += "layout(set = 0, binding = 0, std140) uniform XeSystemConstants {\n";
  source += fmt::format("  layout(offset = {}) uint xe_vertex_index_endian;\n",
                        offsetof(SpirvShaderTranslator::SystemConstants, vertex_index_endian));
  source += fmt::format("  layout(offset = {}) int xe_vertex_base_index;\n",
                        offsetof(SpirvShaderTranslator::SystemConstants, vertex_base_index));
  source += fmt::format("  layout(offset = {}) uint xe_vertex_index_min;\n",
                        offsetof(SpirvShaderTranslator::SystemConstants, vertex_index_min));
  source += fmt::format("  layout(offset = {}) uint xe_vertex_index_max;\n",
                        offsetof(SpirvShaderTranslator::SystemConstants, vertex_index_max));
  source +=
      fmt::format("  layout(offset = {}) float xe_tessellation_factor_range_min;\n",
                  offsetof(SpirvShaderTranslator::SystemConstants, tessellation_factor_range_min));
  source +=
      fmt::format("  layout(offset = {}) float xe_tessellation_factor_range_max;\n",
                  offsetof(SpirvShaderTranslator::SystemConstants, tessellation_factor_range_max));
  source += "} xe_system_cbuffer;\n";
  return source;
}

std::string GetTessellationIndexedVertexShaderGlsl() {
  std::string source;
  source += "#version 450\n";
  source += GetTessellationSystemConstantsBlockGlsl();
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

std::string GetTessellationAdaptiveVertexShaderGlsl() {
  std::string source;
  source += "#version 450\n";
  source += GetTessellationSystemConstantsBlockGlsl();
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

std::string GetTessellationControlShaderGlsl(Shader::HostVertexShaderType host_vertex_shader_type,
                                             xenos::TessellationMode tessellation_mode,
                                             uint32_t input_control_points,
                                             uint32_t output_control_points) {
  std::string source;
  source += "#version 450\n";
  source += GetTessellationSystemConstantsBlockGlsl();
  source += "layout(location = 0) in float xe_input_value[];\n";
  source += "layout(location = 0) patch out vec4 xe_out_patch_control_point_indices;\n";
  source += fmt::format(
      "layout({}, {}, cw, vertices = {}) out;\n",
      IsTriangleDomainHostVertexShaderType(host_vertex_shader_type) ? "triangles" : "quads",
      tessellation_mode == xenos::TessellationMode::kDiscrete ? "equal_spacing"
                                                              : "fractional_even_spacing",
      output_control_points);

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

}  // namespace

VulkanPipelineCache::VulkanPipelineCache(VulkanCommandProcessor& command_processor,
                                         const RegisterFile& register_file,
                                         VulkanRenderTargetCache& render_target_cache,
                                         VkShaderStageFlags guest_shader_vertex_stages)
    : command_processor_(command_processor),
      register_file_(register_file),
      render_target_cache_(render_target_cache),
      guest_shader_vertex_stages_(guest_shader_vertex_stages) {}

VulkanPipelineCache::~VulkanPipelineCache() {
  Shutdown();
}

bool VulkanPipelineCache::Initialize() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  shader_translator_ = std::make_unique<SpirvShaderTranslator>(
      SpirvShaderTranslator::Features(vulkan_device),
      render_target_cache_.msaa_2x_attachments_supported(),
      render_target_cache_.msaa_2x_no_attachments_supported(), edram_fragment_shader_interlock,
      render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y());

  if (edram_fragment_shader_interlock) {
    std::vector<uint8_t> depth_only_fragment_shader_code =
        shader_translator_->CreateDepthOnlyFragmentShader();
    depth_only_fragment_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, reinterpret_cast<const uint32_t*>(depth_only_fragment_shader_code.data()),
        depth_only_fragment_shader_code.size());
    if (depth_only_fragment_shader_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanPipelineCache: Failed to create the depth/stencil-only "
          "fragment shader for the fragment shader interlock render backend "
          "implementation");
      return false;
    }
  } else {
    std::vector<uint8_t> depth_float24_truncate_fragment_shader_code =
        shader_translator_->CreateDepthOnlyFragmentShader(
            SpirvShaderTranslator::Modification::DepthStencilMode::kFloat24Truncating);
    depth_float24_truncate_fragment_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device,
        reinterpret_cast<const uint32_t*>(depth_float24_truncate_fragment_shader_code.data()),
        depth_float24_truncate_fragment_shader_code.size());
    if (depth_float24_truncate_fragment_shader_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanPipelineCache: Failed to create the float24 truncating "
          "depth-only fragment shader");
      return false;
    }
    std::vector<uint8_t> depth_float24_round_fragment_shader_code =
        shader_translator_->CreateDepthOnlyFragmentShader(
            SpirvShaderTranslator::Modification::DepthStencilMode::kFloat24Rounding);
    depth_float24_round_fragment_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device,
        reinterpret_cast<const uint32_t*>(depth_float24_round_fragment_shader_code.data()),
        depth_float24_round_fragment_shader_code.size());
    if (depth_float24_round_fragment_shader_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanPipelineCache: Failed to create the float24 rounding "
          "depth-only fragment shader");
      return false;
    }
  }

  std::vector<uint32_t> placeholder_pixel_shader_spirv;
  std::string placeholder_pixel_shader_compile_error;
  if (command_processor_.CompileGlslToSpirv(
          VK_SHADER_STAGE_FRAGMENT_BIT, kPlaceholderPixelShaderSource,
          placeholder_pixel_shader_spirv, placeholder_pixel_shader_compile_error)) {
    placeholder_pixel_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, placeholder_pixel_shader_spirv.data(),
        placeholder_pixel_shader_spirv.size() * sizeof(uint32_t));
  } else {
    REXGPU_WARN("VulkanPipelineCache: Failed to compile placeholder pixel shader: {}",
                placeholder_pixel_shader_compile_error);
  }
  if (placeholder_pixel_shader_ == VK_NULL_HANDLE) {
    REXGPU_WARN(
        "VulkanPipelineCache: Failed to create placeholder pixel shader - "
        "async placeholder hot-swap will be unavailable");
  }

  uint32_t logical_processor_count = rex::thread::logical_processor_count();
  if (!logical_processor_count) {
    logical_processor_count = 6;
  }
  creation_threads_busy_ = 0;
  startup_loading_ = false;
  creation_completion_event_ = rex::thread::Event::CreateManualResetEvent(true);
  assert_not_null(creation_completion_event_);
  creation_completion_set_event_ = false;
  creation_threads_shutdown_from_ = SIZE_MAX;
  if (REXCVAR_GET(vulkan_pipeline_creation_threads) != 0) {
    size_t creation_thread_count;
    if (REXCVAR_GET(vulkan_pipeline_creation_threads) < 0) {
      creation_thread_count = std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      creation_thread_count = std::min(uint32_t(REXCVAR_GET(vulkan_pipeline_creation_threads)),
                                       logical_processor_count);
    }
    for (size_t i = 0; i < creation_thread_count; ++i) {
      std::unique_ptr<rex::thread::Thread> creation_thread =
          rex::thread::Thread::Create({}, [this, i]() { CreationThread(i); });
      assert_not_null(creation_thread);
      creation_thread->set_name("Vulkan Pipelines");
      creation_threads_.push_back(std::move(creation_thread));
    }
  }

  return true;
}

void VulkanPipelineCache::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                  uint32_t title_id, bool blocking) {
  ShutdownShaderStorage();
  {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    startup_loading_ = false;
  }

  auto shader_storage_root = cache_root / "shaders";
  auto shader_storage_shareable_root = shader_storage_root / "shareable";
  if (!std::filesystem::exists(shader_storage_shareable_root)) {
    if (!std::filesystem::create_directories(shader_storage_shareable_root)) {
      REXGPU_ERROR(
          "Failed to create the shareable shader storage directory, persistent "
          "shader storage will be disabled: {}",
          rex::path_to_utf8(shader_storage_shareable_root));
      return;
    }
  }

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  // Initialize the pipeline storage stream - read pipeline descriptions and
  // collect used shader modifications to translate.
  std::vector<PipelineStoredDescription> pipeline_stored_descriptions;
  // <Shader hash, modification bits>.
  std::set<std::pair<uint64_t, uint64_t>> shader_translations_needed;
  auto pipeline_storage_file_path =
      shader_storage_shareable_root /
      fmt::format("{:08X}.{}.vk.xpso", title_id, edram_fragment_shader_interlock ? "fsi" : "fbo");
  pipeline_storage_file_ = rex::filesystem::OpenFile(pipeline_storage_file_path, "a+b");
  if (!pipeline_storage_file_) {
    REXGPU_ERROR(
        "Failed to open the Vulkan pipeline description storage file for "
        "writing, persistent shader storage will be disabled: {}",
        rex::path_to_utf8(pipeline_storage_file_path));
    return;
  }
  pipeline_storage_file_flush_needed_ = false;
  // 'XEPS'.
  const uint32_t pipeline_storage_magic = 0x53504558;
  const uint32_t pipeline_storage_magic_api = edram_fragment_shader_interlock ? 1u : 0u;
  const uint32_t pipeline_storage_version_swapped = rex::byte_swap(
      std::max(PipelineDescription::kVersion, SpirvShaderTranslator::Modification::kVersion));
  struct {
    uint32_t magic;
    uint32_t magic_api;
    uint32_t version_swapped;
  } pipeline_storage_file_header;
  if (fread(&pipeline_storage_file_header, sizeof(pipeline_storage_file_header), 1,
            pipeline_storage_file_) &&
      pipeline_storage_file_header.magic == pipeline_storage_magic &&
      pipeline_storage_file_header.magic_api == pipeline_storage_magic_api &&
      pipeline_storage_file_header.version_swapped == pipeline_storage_version_swapped) {
    rex::filesystem::Seek(pipeline_storage_file_, 0, SEEK_END);
    int64_t pipeline_storage_told_end = rex::filesystem::Tell(pipeline_storage_file_);
    size_t pipeline_storage_told_count =
        size_t(pipeline_storage_told_end >= int64_t(sizeof(pipeline_storage_file_header))
                   ? (uint64_t(pipeline_storage_told_end) - sizeof(pipeline_storage_file_header)) /
                         sizeof(PipelineStoredDescription)
                   : 0);
    if (pipeline_storage_told_count &&
        rex::filesystem::Seek(pipeline_storage_file_, int64_t(sizeof(pipeline_storage_file_header)),
                              SEEK_SET)) {
      pipeline_stored_descriptions.resize(pipeline_storage_told_count);
      pipeline_stored_descriptions.resize(
          fread(pipeline_stored_descriptions.data(), sizeof(PipelineStoredDescription),
                pipeline_storage_told_count, pipeline_storage_file_));
      size_t pipeline_storage_read_count = pipeline_stored_descriptions.size();
      size_t pipeline_storage_kept_count = 0;
      size_t pipeline_storage_skipped_unsupported_count = 0;
      size_t pipeline_storage_skipped_corrupted_count = 0;
      for (size_t i = 0; i < pipeline_storage_read_count; ++i) {
        const PipelineStoredDescription pipeline_stored_description =
            pipeline_stored_descriptions[i];
        // Validate file integrity, stop and truncate the stream if data is
        // corrupted.
        if (XXH3_64bits(&pipeline_stored_description.description,
                        sizeof(pipeline_stored_description.description)) !=
            pipeline_stored_description.description_hash) {
          pipeline_storage_skipped_corrupted_count = pipeline_storage_read_count - i;
          pipeline_storage_read_count = i;
          break;
        }
        if (!ArePipelineRequirementsMet(pipeline_stored_description.description)) {
          ++pipeline_storage_skipped_unsupported_count;
          continue;
        }
        pipeline_stored_descriptions[pipeline_storage_kept_count++] = pipeline_stored_description;
        // Mark the shader modifications as needed for translation.
        shader_translations_needed.emplace(
            pipeline_stored_description.description.vertex_shader_hash,
            pipeline_stored_description.description.vertex_shader_modification);
        if (pipeline_stored_description.description.pixel_shader_hash) {
          shader_translations_needed.emplace(
              pipeline_stored_description.description.pixel_shader_hash,
              pipeline_stored_description.description.pixel_shader_modification);
        }
      }
      pipeline_stored_descriptions.resize(pipeline_storage_kept_count);
      if (pipeline_storage_skipped_unsupported_count || pipeline_storage_skipped_corrupted_count) {
        REXGPU_INFO(
            "VulkanPipelineCache: Pipeline storage filtering kept {} descriptions (skipped {} "
            "unsupported, {} corrupted)",
            pipeline_storage_kept_count, pipeline_storage_skipped_unsupported_count,
            pipeline_storage_skipped_corrupted_count);
      }
    }
  }

  // Initialize the Xenos shader storage stream.
  auto shader_storage_file_path =
      shader_storage_shareable_root / fmt::format("{:08X}.xsh", title_id);
  shader_storage_file_ = rex::filesystem::OpenFile(shader_storage_file_path, "a+b");
  if (!shader_storage_file_) {
    REXGPU_ERROR(
        "Failed to open the guest shader storage file for writing, persistent "
        "shader storage will be disabled: {}",
        rex::path_to_utf8(shader_storage_file_path));
    fclose(pipeline_storage_file_);
    pipeline_storage_file_ = nullptr;
    return;
  }
  ++shader_storage_index_;
  shader_storage_file_flush_needed_ = false;
  struct {
    uint32_t magic;
    uint32_t version_swapped;
  } shader_storage_file_header;
  // 'XESH'.
  const uint32_t shader_storage_magic = 0x48534558;
  if (fread(&shader_storage_file_header, sizeof(shader_storage_file_header), 1,
            shader_storage_file_) &&
      shader_storage_file_header.magic == shader_storage_magic &&
      rex::byte_swap(shader_storage_file_header.version_swapped) == ShaderStoredHeader::kVersion) {
    uint64_t shader_storage_valid_bytes = sizeof(shader_storage_file_header);
    // Load shaders written by previous runs until the end of the file or until
    // a corrupted one is detected.
    ShaderStoredHeader shader_header;
    std::vector<uint32_t> ucode_dwords;
    ucode_dwords.reserve(0xFFFF);
    while (true) {
      if (!fread(&shader_header, sizeof(shader_header), 1, shader_storage_file_)) {
        break;
      }
      size_t ucode_byte_count = shader_header.ucode_dword_count * sizeof(uint32_t);
      ucode_dwords.resize(shader_header.ucode_dword_count);
      if (shader_header.ucode_dword_count &&
          !fread(ucode_dwords.data(), ucode_byte_count, 1, shader_storage_file_)) {
        break;
      }
      uint64_t ucode_data_hash = XXH3_64bits(ucode_dwords.data(), ucode_byte_count);
      if (shader_header.ucode_data_hash != ucode_data_hash) {
        // Validation failed.
        break;
      }
      shader_storage_valid_bytes += sizeof(shader_header) + ucode_byte_count;
      VulkanShader* shader = LoadShader(shader_header.type, ucode_dwords.data(),
                                        shader_header.ucode_dword_count, ucode_data_hash);
      if (shader->ucode_storage_index() == shader_storage_index_) {
        // Appeared twice in this file for some reason - skip.
        continue;
      }
      // Loaded from the current storage - don't write again.
      shader->set_ucode_storage_index(shader_storage_index_);
    }
    rex::filesystem::TruncateStdioFile(shader_storage_file_, shader_storage_valid_bytes);
  } else {
    rex::filesystem::TruncateStdioFile(shader_storage_file_, 0);
    shader_storage_file_header.magic = shader_storage_magic;
    shader_storage_file_header.version_swapped = rex::byte_swap(ShaderStoredHeader::kVersion);
    fwrite(&shader_storage_file_header, sizeof(shader_storage_file_header), 1,
           shader_storage_file_);
  }

  // Translate shader modifications needed for stored pipelines.
  for (const std::pair<uint64_t, uint64_t>& translation_needed : shader_translations_needed) {
    auto shader_it = shaders_.find(translation_needed.first);
    if (shader_it == shaders_.end()) {
      continue;
    }
    auto* shader = shader_it->second;
    if (!shader->is_ucode_analyzed()) {
      shader->AnalyzeUcode(ucode_disasm_buffer_);
    }
    bool translation_is_new = false;
    auto* translation = static_cast<VulkanShader::VulkanTranslation*>(
        shader->GetOrCreateTranslation(translation_needed.second, &translation_is_new));
    if (!translation->is_translated() &&
        !TranslateAnalyzedShader(*shader_translator_, *translation)) {
      if (translation_is_new) {
        shader->DestroyTranslation(translation_needed.second);
      }
    }
  }

  // Create the pipelines.
  std::vector<PipelineCreationArguments> pipeline_creations;
  pipeline_creations.reserve(pipeline_stored_descriptions.size());
  for (const PipelineStoredDescription& pipeline_stored_description :
       pipeline_stored_descriptions) {
    const PipelineDescription& pipeline_description = pipeline_stored_description.description;

    auto find_it = pipelines_.find(pipeline_description);
    if (find_it != pipelines_.end()) {
      continue;
    }
    auto& pipeline = *pipelines_.emplace(pipeline_description, Pipeline()).first;
    PipelineCreationArguments creation_arguments;
    if (!TryGetPipelineCreationArgumentsForDescription(pipeline_description, &pipeline,
                                                       creation_arguments)) {
      pipelines_.erase(pipeline_description);
      continue;
    }
    uint32_t bound_rts = 0;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (pipeline_description.render_targets[i].color_write_mask) {
        bound_rts |= uint32_t(1) << i;
      }
    }
    uint32_t shader_writes_color_targets =
        creation_arguments.pixel_shader
            ? creation_arguments.pixel_shader->shader().writes_color_targets()
            : 0;
    bool shader_writes_depth = creation_arguments.pixel_shader
                                   ? creation_arguments.pixel_shader->shader().writes_depth()
                                   : pipeline_description.depth_write_enable != 0;
    creation_arguments.priority = pipeline_util::CalculatePipelinePriority(
        bound_rts, shader_writes_color_targets, shader_writes_depth);
    pipeline_creations.push_back(creation_arguments);
  }

  if (!pipeline_creations.empty()) {
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      startup_loading_ = true;
    }
    uint32_t logical_processor_count = rex::thread::logical_processor_count();
    if (!logical_processor_count) {
      // Pick some reasonable amount if couldn't determine the number of cores.
      logical_processor_count = 6;
    }
    size_t creation_thread_count;
    if (REXCVAR_GET(vulkan_pipeline_creation_threads) == 0) {
      creation_thread_count = 1;
    } else if (REXCVAR_GET(vulkan_pipeline_creation_threads) < 0) {
      creation_thread_count = std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      creation_thread_count = std::min(uint32_t(REXCVAR_GET(vulkan_pipeline_creation_threads)),
                                       logical_processor_count);
    }
    if (!blocking) {
      // Vulkan currently doesn't support deferred pipeline handles at draw time,
      // so complete preload creation regardless of blocking mode.
      creation_thread_count = std::max(creation_thread_count, size_t(1));
    }
    creation_thread_count = std::min(creation_thread_count, pipeline_creations.size());

    std::atomic<size_t> next_creation_index(0);
    std::atomic<size_t> created_pipeline_count(0);
    auto creation_worker = [this, &pipeline_creations, &next_creation_index,
                            &created_pipeline_count]() {
      while (true) {
        size_t creation_index = next_creation_index.fetch_add(1);
        if (creation_index >= pipeline_creations.size()) {
          break;
        }
        if (EnsurePipelineCreated(pipeline_creations[creation_index])) {
          created_pipeline_count.fetch_add(1);
        }
      }
    };

    if (creation_thread_count <= 1) {
      creation_worker();
    } else {
      std::vector<std::unique_ptr<rex::thread::Thread>> creation_threads;
      creation_threads.reserve(creation_thread_count);
      for (size_t i = 0; i < creation_thread_count; ++i) {
        std::unique_ptr<rex::thread::Thread> creation_thread =
            rex::thread::Thread::Create({}, creation_worker);
        assert_not_null(creation_thread);
        creation_thread->set_name("Vulkan Pipelines");
        creation_threads.push_back(std::move(creation_thread));
      }
      for (const auto& creation_thread : creation_threads) {
        rex::thread::Wait(creation_thread.get(), false);
      }
    }

    REXGPU_INFO("Created {} graphics pipelines from Vulkan storage ({} requested)",
                created_pipeline_count.load(), pipeline_creations.size());
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      startup_loading_ = false;
    }
  }

  // If any pipeline descriptions were corrupted (or the whole file has excess
  // bytes in the end), truncate to the last valid pipeline description.
  rex::filesystem::TruncateStdioFile(
      pipeline_storage_file_,
      uint64_t(sizeof(pipeline_storage_file_header) +
               sizeof(PipelineStoredDescription) * pipeline_stored_descriptions.size()));
  if (pipeline_stored_descriptions.empty()) {
    rex::filesystem::TruncateStdioFile(pipeline_storage_file_, 0);
    pipeline_storage_file_header.magic = pipeline_storage_magic;
    pipeline_storage_file_header.magic_api = pipeline_storage_magic_api;
    pipeline_storage_file_header.version_swapped = pipeline_storage_version_swapped;
    fwrite(&pipeline_storage_file_header, sizeof(pipeline_storage_file_header), 1,
           pipeline_storage_file_);
  }

  shader_storage_cache_root_ = cache_root;
  shader_storage_title_id_ = title_id;

  // Start the storage writing thread.
  storage_write_flush_shaders_ = false;
  storage_write_flush_pipelines_ = false;
  storage_write_thread_shutdown_ = false;
  storage_write_thread_ = rex::thread::Thread::Create({}, [this]() { StorageWriteThread(); });
  assert_not_null(storage_write_thread_);
  storage_write_thread_->set_name("Vulkan Storage writer");
}

void VulkanPipelineCache::ShutdownShaderStorage() {
  if (storage_write_thread_) {
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_thread_shutdown_ = true;
    }
    storage_write_request_cond_.notify_all();
    rex::thread::Wait(storage_write_thread_.get(), false);
    storage_write_thread_.reset();
  }
  storage_write_shader_queue_.clear();
  storage_write_pipeline_queue_.clear();

  if (pipeline_storage_file_) {
    fclose(pipeline_storage_file_);
    pipeline_storage_file_ = nullptr;
    pipeline_storage_file_flush_needed_ = false;
  }

  if (shader_storage_file_) {
    fclose(shader_storage_file_);
    shader_storage_file_ = nullptr;
    shader_storage_file_flush_needed_ = false;
  }

  shader_storage_cache_root_.clear();
  shader_storage_title_id_ = 0;
}

void VulkanPipelineCache::EndSubmission() {
  if (shader_storage_file_flush_needed_ || pipeline_storage_file_flush_needed_) {
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      if (shader_storage_file_flush_needed_) {
        storage_write_flush_shaders_ = true;
      }
      if (pipeline_storage_file_flush_needed_) {
        storage_write_flush_pipelines_ = true;
      }
    }
    storage_write_request_cond_.notify_one();
    shader_storage_file_flush_needed_ = false;
    pipeline_storage_file_flush_needed_ = false;
  }

  if (!creation_threads_.empty()) {
    bool startup_loading = false;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      startup_loading = startup_loading_;
    }
    if (startup_loading) {
      creation_request_cond_.notify_one();
    } else {
      // Help worker threads on the processor thread to reduce warm-up latency.
      CreateQueuedPipelinesOnProcessorThread();
      bool await_creation_completion_event;
      {
        std::lock_guard<std::mutex> lock(creation_request_lock_);
        // The queue is empty because of CreateQueuedPipelinesOnProcessorThread,
        // only check in-flight creation by worker threads.
        await_creation_completion_event = creation_threads_busy_ != 0;
        if (await_creation_completion_event) {
          creation_completion_event_->Reset();
          creation_completion_set_event_ = true;
        }
      }
      if (await_creation_completion_event) {
        creation_request_cond_.notify_one();
        rex::thread::Wait(creation_completion_event_.get(), false);
      }
    }
  }

  ProcessDeferredPipelineDestructions(false);
}

void VulkanPipelineCache::Shutdown() {
  // Shut down creation threads before destroying any pipelines they may touch.
  if (!creation_threads_.empty()) {
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_threads_shutdown_from_ = 0;
    }
    creation_request_cond_.notify_all();
    for (size_t i = 0; i < creation_threads_.size(); ++i) {
      rex::thread::Wait(creation_threads_[i].get(), false);
    }
    creation_threads_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    while (!creation_queue_.empty()) {
      creation_queue_.pop();
    }
    creation_threads_busy_ = 0;
    startup_loading_ = false;
    creation_completion_set_event_ = false;
    creation_threads_shutdown_from_ = SIZE_MAX;
  }
  creation_completion_event_.reset();

  ShutdownShaderStorage();

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  ProcessDeferredPipelineDestructions(true);

  // Destroy all pipelines.
  last_pipeline_ = nullptr;
  for (const auto& pipeline_pair : pipelines_) {
    VkPipeline pipeline = pipeline_pair.second.pipeline.load(std::memory_order_acquire);
    if (pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
  pipelines_.clear();

  // Destroy all internal shaders.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         depth_only_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         depth_float24_truncate_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         depth_float24_round_fragment_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         placeholder_pixel_shader_);
  if (tessellation_indexed_vertex_shader_ != VK_NULL_HANDLE) {
    dfn.vkDestroyShaderModule(device, tessellation_indexed_vertex_shader_, nullptr);
    tessellation_indexed_vertex_shader_ = VK_NULL_HANDLE;
  }
  if (tessellation_adaptive_vertex_shader_ != VK_NULL_HANDLE) {
    dfn.vkDestroyShaderModule(device, tessellation_adaptive_vertex_shader_, nullptr);
    tessellation_adaptive_vertex_shader_ = VK_NULL_HANDLE;
  }
  tessellation_indexed_vertex_shader_attempted_ = false;
  tessellation_adaptive_vertex_shader_attempted_ = false;
  for (const auto& tessellation_control_shader_pair : tessellation_control_shaders_) {
    if (tessellation_control_shader_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, tessellation_control_shader_pair.second, nullptr);
    }
  }
  tessellation_control_shaders_.clear();
  for (const auto& geometry_shader_pair : geometry_shaders_) {
    if (geometry_shader_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, geometry_shader_pair.second, nullptr);
    }
  }
  geometry_shaders_.clear();

  // Destroy all translated shaders.
  for (auto it : shaders_) {
    delete it.second;
  }
  shaders_.clear();
  shader_storage_index_ = 0;
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();

  // Shut down shader translation.
  shader_translator_.reset();
}

VulkanShader* VulkanPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                              const uint32_t* host_address, uint32_t dword_count) {
  // Hash the input memory and lookup the shader.
  return LoadShader(shader_type, host_address, dword_count,
                    XXH3_64bits(host_address, dword_count * sizeof(uint32_t)));
}

VulkanShader* VulkanPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                              const uint32_t* host_address, uint32_t dword_count,
                                              uint64_t data_hash) {
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    // Shader has been previously loaded.
    return it->second;
  }
  // Always create the shader and stash it away.
  // We need to track it even if it fails translation so we know not to try
  // again.
  VulkanShader* shader = new VulkanShader(command_processor_.GetVulkanDevice(), shader_type,
                                          data_hash, host_address, dword_count);
  shaders_.emplace(data_hash, shader);
  return shader;
}

SpirvShaderTranslator::Modification VulkanPipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, bool ps_param_gen_used) const {
  assert_true(shader.type() == xenos::ShaderType::kVertex);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;
  const auto& device_properties = command_processor_.GetVulkanDevice()->properties();

  SpirvShaderTranslator::Modification modification(
      shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;
  if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
    modification.vertex.tessellation_mode = uint32_t(regs.Get<reg::VGT_HOS_CNTL>().tess_mode);
  } else {
    modification.vertex.tessellation_mode = 0;
  }

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  if (user_clip_planes) {
    if (pa_cl_clip_cntl.ucp_cull_only_ena && device_properties.shaderCullDistance) {
      modification.vertex.user_clip_plane_cull = 1;
    } else if (!device_properties.shaderClipDistance && device_properties.shaderCullDistance) {
      // Fallback if clip distances are unavailable.
      modification.vertex.user_clip_plane_cull = 1;
    } else if (!device_properties.shaderClipDistance) {
      // No supported clip/cull built-in for user clip planes.
      user_clip_planes = 0;
      modification.vertex.user_clip_plane_cull = 0;
    } else {
      modification.vertex.user_clip_plane_cull = 0;
    }
  } else {
    modification.vertex.user_clip_plane_cull = 0;
  }
  modification.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  modification.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;

  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = uint32_t(ps_param_gen_used);
  } else {
    modification.vertex.output_point_parameters =
        uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  }
  modification.vertex.vertex_kill_and = uint32_t(
      device_properties.shaderCullDistance &&
      (shader.writes_point_size_edge_flag_kill_vertex() & 0b100) && !pa_cl_clip_cntl.vtx_kill_or);

  return modification;
}

SpirvShaderTranslator::Modification VulkanPipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control) const {
  assert_true(shader.type() == xenos::ShaderType::kPixel);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  SpirvShaderTranslator::Modification modification(
      shader_translator_->GetDefaultPixelShaderModification(
          shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));

  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                              regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                              regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                              regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  if (render_target_cache_.GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
    if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
        normalized_depth_control.z_enable &&
        regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      // For host D32 depth, emulate guest D24FS8 float24 depth behavior.
      modification.pixel.depth_stencil_mode = render_target_cache_.depth_float24_round()
                                                  ? DepthStencilMode::kFloat24Rounding
                                                  : DepthStencilMode::kFloat24Truncating;
    } else {
      if (shader.implicit_early_z_write_allowed() &&
          (!shader.writes_color_target(0) ||
           !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))) {
        modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
      } else {
        modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
      }
    }
  }

  return modification;
}

bool VulkanPipelineCache::EnsureShadersTranslated(VulkanShader::VulkanTranslation* vertex_shader,
                                                  VulkanShader::VulkanTranslation* pixel_shader) {
  // Edge flags are not supported yet (because polygon primitives are not).
  assert_true(register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdge &&
              register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdgeKill);
  assert_false(register_file_.Get<reg::SQ_PROGRAM_CNTL>().gen_index_vtx);
  if (!vertex_shader->is_translated()) {
    vertex_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
    if (!TranslateAnalyzedShader(*shader_translator_, *vertex_shader)) {
      REXGPU_ERROR("Failed to translate the vertex shader!");
      return false;
    }
  }
  if (!vertex_shader->is_valid()) {
    // Translation attempted previously, but not valid.
    return false;
  }
  if (pixel_shader != nullptr) {
    if (!pixel_shader->is_translated()) {
      pixel_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
      if (!TranslateAnalyzedShader(*shader_translator_, *pixel_shader)) {
        REXGPU_ERROR("Failed to translate the pixel shader!");
        return false;
      }
    }
    if (!pixel_shader->is_valid()) {
      // Translation attempted previously, but not valid.
      return false;
    }
  }
  return true;
}

bool VulkanPipelineCache::ConfigurePipeline(
    VulkanShader::VulkanTranslation* vertex_shader, VulkanShader::VulkanTranslation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    VulkanRenderTargetCache::RenderPassKey render_pass_key, VkPipeline& pipeline_out,
    const PipelineLayoutProvider*& pipeline_layout_out, void** pipeline_handle_out) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Ensure shaders are translated - needed now for GetCurrentStateDescription.
  if (!EnsureShadersTranslated(vertex_shader, pixel_shader)) {
    return false;
  }
  if (shader_storage_file_ &&
      vertex_shader->shader().ucode_storage_index() != shader_storage_index_) {
    vertex_shader->shader().set_ucode_storage_index(shader_storage_index_);
    assert_not_null(storage_write_thread_);
    shader_storage_file_flush_needed_ = true;
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_shader_queue_.push_back(&vertex_shader->shader());
    }
    storage_write_request_cond_.notify_all();
  }
  if (pixel_shader && shader_storage_file_ &&
      pixel_shader->shader().ucode_storage_index() != shader_storage_index_) {
    pixel_shader->shader().set_ucode_storage_index(shader_storage_index_);
    assert_not_null(storage_write_thread_);
    shader_storage_file_flush_needed_ = true;
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_shader_queue_.push_back(&pixel_shader->shader());
    }
    storage_write_request_cond_.notify_all();
  }

  PipelineDescription description;
  if (!GetCurrentStateDescription(vertex_shader, pixel_shader, primitive_processing_result,
                                  normalized_depth_control, normalized_color_mask, render_pass_key,
                                  description)) {
    return false;
  }
  if (last_pipeline_ && last_pipeline_->first == description) {
    VkPipeline last_pipeline = last_pipeline_->second.pipeline.load(std::memory_order_acquire);
    const PipelineLayoutProvider* last_pipeline_layout =
        last_pipeline_->second.pipeline_layout.load(std::memory_order_acquire);
    if (last_pipeline != VK_NULL_HANDLE && last_pipeline_layout != nullptr) {
      pipeline_out = last_pipeline;
      pipeline_layout_out = last_pipeline_layout;
      if (pipeline_handle_out) {
        *pipeline_handle_out = const_cast<Pipeline*>(&last_pipeline_->second);
      }
      return true;
    }
  }

  bool use_async = REXCVAR_GET(async_shader_compilation) && !creation_threads_.empty() &&
                   pixel_shader && placeholder_pixel_shader_ != VK_NULL_HANDLE;
  uint8_t async_priority = pipeline_util::kPriorityLowest;
  if (use_async) {
    uint32_t bound_rts =
        pipeline_util::GetBoundRTMaskFromNormalizedColorMask(normalized_color_mask);
    uint32_t shader_writes_color_targets =
        pixel_shader ? pixel_shader->shader().writes_color_targets() : 0;
    bool shader_writes_depth = pixel_shader ? pixel_shader->shader().writes_depth()
                                            : normalized_depth_control.z_write_enable != 0;
    async_priority = pipeline_util::CalculatePipelinePriority(
        bound_rts, shader_writes_color_targets, shader_writes_depth);
  }

  auto it = pipelines_.find(description);
  if (it != pipelines_.end()) {
    VkPipeline found_pipeline = it->second.pipeline.load(std::memory_order_acquire);
    if (found_pipeline == VK_NULL_HANDLE) {
      PipelineCreationArguments creation_arguments;
      if (!TryGetPipelineCreationArgumentsForDescription(description, &*it, creation_arguments) ||
          !EnsurePipelineCreated(creation_arguments)) {
        return false;
      }
      found_pipeline = it->second.pipeline.load(std::memory_order_acquire);
    }
    const PipelineLayoutProvider* found_pipeline_layout =
        it->second.pipeline_layout.load(std::memory_order_acquire);
    last_pipeline_ = &*it;
    pipeline_out = found_pipeline;
    pipeline_layout_out = found_pipeline_layout;
    if (pipeline_handle_out) {
      *pipeline_handle_out = &it->second;
    }
    return pipeline_out != VK_NULL_HANDLE && pipeline_layout_out != nullptr;
  }

  // Create the pipeline if not already existing.
  auto& pipeline = *pipelines_.emplace(description, Pipeline()).first;
  PipelineCreationArguments creation_arguments_real;
  if (!TryGetPipelineCreationArgumentsForDescription(description, &pipeline,
                                                     creation_arguments_real)) {
    pipelines_.erase(description);
    return false;
  }

  bool queued_async_creation = false;
  if (use_async) {
    creation_arguments_real.priority = async_priority;
    PipelineCreationArguments creation_arguments_placeholder;
    if (TryGetPipelineCreationArgumentsForDescription(description, &pipeline,
                                                      creation_arguments_placeholder, true)) {
      pipeline.second.is_placeholder.store(true, std::memory_order_release);
      if (EnsurePipelineCreated(creation_arguments_placeholder, placeholder_pixel_shader_)) {
        {
          std::lock_guard<std::mutex> lock(creation_request_lock_);
          creation_queue_.push(creation_arguments_real);
        }
        creation_request_cond_.notify_one();
        queued_async_creation = true;
      } else {
        pipeline.second.is_placeholder.store(false, std::memory_order_release);
      }
    }
  }

  if (!queued_async_creation && !EnsurePipelineCreated(creation_arguments_real)) {
    pipelines_.erase(description);
    return false;
  }
  if (!queued_async_creation) {
    pipeline.second.is_placeholder.store(false, std::memory_order_release);
  }

  if (pipeline_storage_file_) {
    assert_not_null(storage_write_thread_);
    pipeline_storage_file_flush_needed_ = true;
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_pipeline_queue_.emplace_back();
      PipelineStoredDescription& stored_description = storage_write_pipeline_queue_.back();
      stored_description.description_hash = description.GetHash();
      std::memcpy(&stored_description.description, &description, sizeof(description));
    }
    storage_write_request_cond_.notify_all();
  }
  last_pipeline_ = &pipeline;
  pipeline_out = pipeline.second.pipeline.load(std::memory_order_acquire);
  pipeline_layout_out = pipeline.second.pipeline_layout.load(std::memory_order_acquire);
  if (pipeline_handle_out) {
    *pipeline_handle_out = &pipeline.second;
  }
  return pipeline_out != VK_NULL_HANDLE && pipeline_layout_out != nullptr;
}

bool VulkanPipelineCache::IsCreatingPipelines() const {
  std::lock_guard<std::mutex> lock(creation_request_lock_);
  if (creation_threads_.empty()) {
    return startup_loading_;
  }
  return startup_loading_ || !creation_queue_.empty() || creation_threads_busy_ != 0;
}

void VulkanPipelineCache::GetPipelineAndLayoutByHandle(
    void* handle, VkPipeline& pipeline_out, const PipelineLayoutProvider*& pipeline_layout_out,
    bool* is_placeholder_out) const {
  if (!handle) {
    pipeline_out = VK_NULL_HANDLE;
    pipeline_layout_out = nullptr;
    if (is_placeholder_out) {
      *is_placeholder_out = false;
    }
    return;
  }
  const auto* pipeline = reinterpret_cast<const Pipeline*>(handle);
  pipeline_out = pipeline->pipeline.load(std::memory_order_acquire);
  pipeline_layout_out = pipeline->pipeline_layout.load(std::memory_order_acquire);
  if (is_placeholder_out) {
    *is_placeholder_out = pipeline->is_placeholder.load(std::memory_order_acquire);
  }
}

bool VulkanPipelineCache::TranslateAnalyzedShader(SpirvShaderTranslator& translator,
                                                  VulkanShader::VulkanTranslation& translation) {
  VulkanShader& shader = static_cast<VulkanShader&>(translation.shader());

  // Perform translation.
  // If this fails the shader will be marked as invalid and ignored later.
  if (!translator.TranslateAnalyzedShader(translation)) {
    REXGPU_ERROR("Shader {:016X} translation failed; marking as ignored", shader.ucode_data_hash());
    return false;
  }
  if (translation.GetOrCreateShaderModule() == VK_NULL_HANDLE) {
    return false;
  }

  // TODO(Triang3l): Log that the shader has been successfully translated in
  // common code.

  // Set up the texture binding layout.
  if (shader.EnterBindingLayoutUserUIDSetup()) {
    // Obtain the unique IDs of the binding layout if there are any texture
    // bindings, for invalidation in the command processor.
    size_t texture_binding_layout_uid = kLayoutUIDEmpty;
    const std::vector<VulkanShader::TextureBinding>& texture_bindings =
        shader.GetTextureBindingsAfterTranslation();
    size_t texture_binding_count = texture_bindings.size();
    if (texture_binding_count) {
      size_t texture_binding_layout_bytes =
          texture_binding_count * sizeof(*texture_bindings.data());
      uint64_t texture_binding_layout_hash =
          XXH3_64bits(texture_bindings.data(), texture_binding_layout_bytes);
      auto found_range = texture_binding_layout_map_.equal_range(texture_binding_layout_hash);
      for (auto it = found_range.first; it != found_range.second; ++it) {
        if (it->second.vector_span_length == texture_binding_count &&
            !std::memcmp(texture_binding_layouts_.data() + it->second.vector_span_offset,
                         texture_bindings.data(), texture_binding_layout_bytes)) {
          texture_binding_layout_uid = it->second.uid;
          break;
        }
      }
      if (texture_binding_layout_uid == kLayoutUIDEmpty) {
        static_assert(kLayoutUIDEmpty == 0,
                      "Layout UID is size + 1 because it's assumed that 0 is the UID for "
                      "an empty layout");
        texture_binding_layout_uid = texture_binding_layout_map_.size() + 1;
        LayoutUID new_uid;
        new_uid.uid = texture_binding_layout_uid;
        new_uid.vector_span_offset = texture_binding_layouts_.size();
        new_uid.vector_span_length = texture_binding_count;
        texture_binding_layouts_.resize(new_uid.vector_span_offset + texture_binding_count);
        std::memcpy(texture_binding_layouts_.data() + new_uid.vector_span_offset,
                    texture_bindings.data(), texture_binding_layout_bytes);
        texture_binding_layout_map_.emplace(texture_binding_layout_hash, new_uid);
      }
    }
    shader.SetTextureBindingLayoutUserUID(texture_binding_layout_uid);

    // Use the sampler count for samplers because it's the only thing that must
    // be the same for layouts to be compatible in this case
    // (instruction-specified parameters are used as overrides for creating
    // actual samplers).
    static_assert(kLayoutUIDEmpty == 0,
                  "Empty layout UID is assumed to be 0 because for bindful samplers, the "
                  "UID is their count");
    shader.SetSamplerBindingLayoutUserUID(shader.GetSamplerBindingsAfterTranslation().size());
  }

  return true;
}

void VulkanPipelineCache::WritePipelineRenderTargetDescription(
    reg::RB_BLENDCONTROL blend_control, uint32_t write_mask,
    PipelineRenderTarget& render_target_out) const {
  if (write_mask) {
    assert_zero(write_mask & ~uint32_t(0b1111));
    // 32 because of 0x1F mask, for safety (all unknown to zero).
    static const PipelineBlendFactor kBlendFactorMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcColor,
        /*  5 */ PipelineBlendFactor::kOneMinusSrcColor,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kOneMinusSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDstColor,
        /*  9 */ PipelineBlendFactor::kOneMinusDstColor,
        /* 10 */ PipelineBlendFactor::kDstAlpha,
        /* 11 */ PipelineBlendFactor::kOneMinusDstAlpha,
        /* 12 */ PipelineBlendFactor::kConstantColor,
        /* 13 */ PipelineBlendFactor::kOneMinusConstantColor,
        /* 14 */ PipelineBlendFactor::kConstantAlpha,
        /* 15 */ PipelineBlendFactor::kOneMinusConstantAlpha,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSaturate,
    };
    render_target_out.src_color_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.color_srcblend)];
    render_target_out.dst_color_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.color_destblend)];
    render_target_out.color_blend_op = blend_control.color_comb_fcn;
    render_target_out.src_alpha_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.alpha_srcblend)];
    render_target_out.dst_alpha_blend_factor =
        kBlendFactorMap[uint32_t(blend_control.alpha_destblend)];
    render_target_out.alpha_blend_op = blend_control.alpha_comb_fcn;
    if (!command_processor_.GetVulkanDevice()->properties().constantAlphaColorBlendFactors) {
      if (blend_control.color_srcblend == xenos::BlendFactor::kConstantAlpha) {
        render_target_out.src_color_blend_factor = PipelineBlendFactor::kConstantColor;
      } else if (blend_control.color_srcblend == xenos::BlendFactor::kOneMinusConstantAlpha) {
        render_target_out.src_color_blend_factor = PipelineBlendFactor::kOneMinusConstantColor;
      }
      if (blend_control.color_destblend == xenos::BlendFactor::kConstantAlpha) {
        render_target_out.dst_color_blend_factor = PipelineBlendFactor::kConstantColor;
      } else if (blend_control.color_destblend == xenos::BlendFactor::kOneMinusConstantAlpha) {
        render_target_out.dst_color_blend_factor = PipelineBlendFactor::kOneMinusConstantColor;
      }
    }
  } else {
    render_target_out.src_color_blend_factor = PipelineBlendFactor::kOne;
    render_target_out.dst_color_blend_factor = PipelineBlendFactor::kZero;
    render_target_out.color_blend_op = xenos::BlendOp::kAdd;
    render_target_out.src_alpha_blend_factor = PipelineBlendFactor::kOne;
    render_target_out.dst_alpha_blend_factor = PipelineBlendFactor::kZero;
    render_target_out.alpha_blend_op = xenos::BlendOp::kAdd;
  }
  render_target_out.color_write_mask = write_mask;
}

bool VulkanPipelineCache::GetCurrentStateDescription(
    const VulkanShader::VulkanTranslation* vertex_shader,
    const VulkanShader::VulkanTranslation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    VulkanRenderTargetCache::RenderPassKey render_pass_key,
    PipelineDescription& description_out) const {
  description_out.Reset();

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();

  const RegisterFile& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();

  description_out.vertex_shader_hash = vertex_shader->shader().ucode_data_hash();
  description_out.vertex_shader_modification = vertex_shader->modification();
  SpirvShaderTranslator::Modification vertex_shader_modification(
      description_out.vertex_shader_modification);
  Shader::HostVertexShaderType host_vertex_shader_type =
      vertex_shader_modification.vertex.host_vertex_shader_type;
  if (pixel_shader) {
    description_out.pixel_shader_hash = pixel_shader->shader().ucode_data_hash();
    description_out.pixel_shader_modification = pixel_shader->modification();
    SpirvShaderTranslator::Modification pixel_shader_modification(
        description_out.pixel_shader_modification);
    using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
    description_out.sample_rate_shading =
        !pixel_shader->shader().writes_depth() &&
        (pixel_shader_modification.pixel.depth_stencil_mode ==
             DepthStencilMode::kFloat24Truncating ||
         pixel_shader_modification.pixel.depth_stencil_mode == DepthStencilMode::kFloat24Rounding);
  }
  description_out.render_pass_key = render_pass_key;

  bool tessellated = primitive_processing_result.IsTessellated();
  PipelineGeometryShader geometry_shader = PipelineGeometryShader::kNone;
  PipelinePrimitiveTopology primitive_topology;
  if (tessellated) {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kQuadList:
      case xenos::PrimitiveType::kTrianglePatch:
      case xenos::PrimitiveType::kQuadPatch:
        primitive_topology = PipelinePrimitiveTopology::kPatchList;
        break;
      default:
        return false;
    }
    description_out.tessellation_mode = primitive_processing_result.tessellation_mode;
  } else {
    // Fallback expansion in the vertex shader for primitive types that may be
    // emulated through geometry shaders on GS-capable hosts.
    if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
        host_vertex_shader_type == Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
      if (primitive_processing_result.host_primitive_type != xenos::PrimitiveType::kTriangleStrip ||
          !primitive_processing_result.host_primitive_reset_enabled) {
        assert_always();
        return false;
      }
      primitive_topology = PipelinePrimitiveTopology::kTriangleStrip;
    } else {
      switch (primitive_processing_result.host_primitive_type) {
        case xenos::PrimitiveType::kPointList:
          geometry_shader = PipelineGeometryShader::kPointList;
          primitive_topology = PipelinePrimitiveTopology::kPointList;
          break;
        case xenos::PrimitiveType::kLineList:
          primitive_topology = PipelinePrimitiveTopology::kLineList;
          break;
        case xenos::PrimitiveType::kLineStrip:
          primitive_topology = PipelinePrimitiveTopology::kLineStrip;
          break;
        case xenos::PrimitiveType::kTriangleList:
          primitive_topology = PipelinePrimitiveTopology::kTriangleList;
          break;
        case xenos::PrimitiveType::kTriangleFan:
          // Triangle fans on Vulkan are expected to be converted to triangle
          // lists in primitive processing for parity with D3D12 behavior.
          assert_always();
          return false;
        case xenos::PrimitiveType::kTriangleStrip:
          primitive_topology = PipelinePrimitiveTopology::kTriangleStrip;
          break;
        case xenos::PrimitiveType::kRectangleList:
          geometry_shader = PipelineGeometryShader::kRectangleList;
          primitive_topology = PipelinePrimitiveTopology::kTriangleList;
          break;
        case xenos::PrimitiveType::kQuadList:
          geometry_shader = PipelineGeometryShader::kQuadList;
          primitive_topology = PipelinePrimitiveTopology::kLineListWithAdjacency;
          break;
        default:
          return false;
      }
    }
  }
  description_out.geometry_shader = geometry_shader;
  description_out.primitive_topology = primitive_topology;
  description_out.primitive_restart = primitive_processing_result.host_primitive_reset_enabled;

  description_out.depth_clamp_enable =
      device_properties.depthClamp && regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;

  // Tessellated draws are patch-domain polygonal primitives regardless of guest
  // register ambiguity in non-explicit major mode configurations.
  bool primitive_polygonal = tessellated ? true : draw_util::IsPrimitivePolygonal(regs);
  bool rasterization_enabled = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (!rasterization_enabled) {
    // Keep parity with D3D12 by fully disabling rasterization for draws where
    // only non-raster stages (for instance, vertex memexport) can have side
    // effects.
    description_out.rasterizer_discard = 1;
    return true;
  }
  if (primitive_polygonal) {
    // Vulkan only allows the polygon mode to be set for both faces - pick the
    // most special one (more likely to represent the developer's deliberate
    // intentions - fill is very generic, wireframe is common in debug, points
    // are for pretty unusual things, but closer to debug purposes too - on the
    // Xenos, points have the lowest register value and triangles have the
    // highest) based on which faces are not culled.
    bool cull_front = pa_su_sc_mode_cntl.cull_front;
    bool cull_back = pa_su_sc_mode_cntl.cull_back;
    description_out.cull_front = cull_front;
    description_out.cull_back = cull_back;
    if (device_properties.fillModeNonSolid) {
      xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
      if (!cull_front) {
        polygon_type = std::min(polygon_type, pa_su_sc_mode_cntl.polymode_front_ptype);
      }
      if (!cull_back) {
        polygon_type = std::min(polygon_type, pa_su_sc_mode_cntl.polymode_back_ptype);
      }
      if (pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDualMode) {
        polygon_type = xenos::PolygonType::kTriangles;
      }
      switch (polygon_type) {
        case xenos::PolygonType::kPoints:
        case xenos::PolygonType::kLines:
          // Keep non-triangle polygon fill behavior aligned with D3D12 (which
          // maps both point and line polygon modes to wireframe).
          description_out.polygon_mode = PipelinePolygonMode::kLine;
          break;
        case xenos::PolygonType::kTriangles:
          description_out.polygon_mode = PipelinePolygonMode::kFill;
          break;
        default:
          assert_unhandled_case(polygon_type);
          return false;
      }
    } else {
      description_out.polygon_mode = PipelinePolygonMode::kFill;
    }
    description_out.front_face_clockwise = pa_su_sc_mode_cntl.face != 0;
  } else {
    description_out.polygon_mode = PipelinePolygonMode::kFill;
  }
  if (tessellated && REXCVAR_GET(vulkan_tessellation_wireframe) &&
      device_properties.fillModeNonSolid) {
    description_out.polygon_mode = PipelinePolygonMode::kLine;
  }

  if (render_target_cache_.GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    if (render_pass_key.depth_and_color_used & 1) {
      if (normalized_depth_control.z_enable) {
        description_out.depth_write_enable = normalized_depth_control.z_write_enable;
        description_out.depth_compare_op = normalized_depth_control.zfunc;
      } else {
        description_out.depth_compare_op = xenos::CompareFunction::kAlways;
      }
      if (normalized_depth_control.stencil_enable) {
        description_out.stencil_test_enable = 1;
        description_out.stencil_front_fail_op = normalized_depth_control.stencilfail;
        description_out.stencil_front_pass_op = normalized_depth_control.stencilzpass;
        description_out.stencil_front_depth_fail_op = normalized_depth_control.stencilzfail;
        description_out.stencil_front_compare_op = normalized_depth_control.stencilfunc;
        if (primitive_polygonal && normalized_depth_control.backface_enable) {
          description_out.stencil_back_fail_op = normalized_depth_control.stencilfail_bf;
          description_out.stencil_back_pass_op = normalized_depth_control.stencilzpass_bf;
          description_out.stencil_back_depth_fail_op = normalized_depth_control.stencilzfail_bf;
          description_out.stencil_back_compare_op = normalized_depth_control.stencilfunc_bf;
        } else {
          description_out.stencil_back_fail_op = description_out.stencil_front_fail_op;
          description_out.stencil_back_pass_op = description_out.stencil_front_pass_op;
          description_out.stencil_back_depth_fail_op = description_out.stencil_front_depth_fail_op;
          description_out.stencil_back_compare_op = description_out.stencil_front_compare_op;
        }
      }
    }

    // Color blending and write masks (filled only for the attachments present
    // in the render pass object).
    uint32_t render_pass_color_rts = render_pass_key.depth_and_color_used >> 1;
    assert_true(device_properties.independentBlend);
    uint32_t render_pass_color_rts_remaining = render_pass_color_rts;
    uint32_t color_rt_index;
    while (rex::bit_scan_forward(render_pass_color_rts_remaining, &color_rt_index)) {
      render_pass_color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
      WritePipelineRenderTargetDescription(
          regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[color_rt_index]),
          (normalized_color_mask >> (color_rt_index * 4)) & 0b1111,
          description_out.render_targets[color_rt_index]);
    }
  }

  return true;
}

bool VulkanPipelineCache::ArePipelineRequirementsMet(const PipelineDescription& description) const {
  SpirvShaderTranslator::Modification vertex_shader_modification(
      description.vertex_shader_modification);
  Shader::HostVertexShaderType host_vertex_shader_type =
      vertex_shader_modification.vertex.host_vertex_shader_type;
  bool tessellated = Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type);

  VkShaderStageFlags vertex_shader_stage =
      tessellated ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : VK_SHADER_STAGE_VERTEX_BIT;
  if (!(guest_shader_vertex_stages_ & vertex_shader_stage)) {
    return false;
  }

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();

  if (tessellated) {
    if (!device_properties.tessellationShader) {
      return false;
    }
    if (description.primitive_topology != PipelinePrimitiveTopology::kPatchList) {
      return false;
    }
    if (description.geometry_shader != PipelineGeometryShader::kNone) {
      return false;
    }
    switch (description.tessellation_mode) {
      case xenos::TessellationMode::kDiscrete:
      case xenos::TessellationMode::kContinuous:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            break;
          default:
            return false;
        }
        break;
      case xenos::TessellationMode::kAdaptive:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            break;
          default:
            return false;
        }
        break;
      default:
        return false;
    }
  } else {
    if (description.primitive_topology == PipelinePrimitiveTopology::kPatchList) {
      return false;
    }
  }

  if (!device_properties.geometryShader &&
      description.geometry_shader != PipelineGeometryShader::kNone) {
    return false;
  }
  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      host_vertex_shader_type == Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
    if (description.geometry_shader != PipelineGeometryShader::kNone ||
        description.primitive_topology != PipelinePrimitiveTopology::kTriangleStrip ||
        !description.primitive_restart) {
      return false;
    }
  }
  // Keep fan handling consistent with D3D12 by always using primitive
  // processor conversion to triangle lists instead of native fan topologies.
  if (description.primitive_topology == PipelinePrimitiveTopology::kTriangleFan) {
    return false;
  }

  if (!device_properties.depthClamp && description.depth_clamp_enable) {
    return false;
  }

  if (!device_properties.pointPolygons && description.polygon_mode == PipelinePolygonMode::kPoint) {
    return false;
  }

  if (!device_properties.fillModeNonSolid &&
      description.polygon_mode != PipelinePolygonMode::kFill) {
    return false;
  }

  if (description.sample_rate_shading && !device_properties.sampleRateShading) {
    return false;
  }

  assert_true(device_properties.independentBlend);

  if (!device_properties.constantAlphaColorBlendFactors) {
    uint32_t color_rts_remaining = description.render_pass_key.depth_and_color_used >> 1;
    uint32_t color_rt_index;
    while (rex::bit_scan_forward(color_rts_remaining, &color_rt_index)) {
      color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
      const PipelineRenderTarget& color_rt = description.render_targets[color_rt_index];
      if (color_rt.src_color_blend_factor == PipelineBlendFactor::kConstantAlpha ||
          color_rt.src_color_blend_factor == PipelineBlendFactor::kOneMinusConstantAlpha ||
          color_rt.dst_color_blend_factor == PipelineBlendFactor::kConstantAlpha ||
          color_rt.dst_color_blend_factor == PipelineBlendFactor::kOneMinusConstantAlpha) {
        return false;
      }
    }
  }

  return true;
}

uint32_t VulkanPipelineCache::GetTessellationPatchControlPointCount(
    Shader::HostVertexShaderType host_vertex_shader_type,
    xenos::TessellationMode tessellation_mode) {
  switch (tessellation_mode) {
    case xenos::TessellationMode::kDiscrete:
    case xenos::TessellationMode::kContinuous:
      switch (host_vertex_shader_type) {
        case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
          return 3;
        case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
          return 1;
        case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
          return 4;
        case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
          return 1;
        default:
          return 0;
      }
    case xenos::TessellationMode::kAdaptive:
      switch (host_vertex_shader_type) {
        case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
          return 3;
        case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
          return 4;
        default:
          return 0;
      }
    default:
      return 0;
  }
}

VkShaderModule VulkanPipelineCache::GetTessellationVertexShader(bool adaptive) {
  bool& attempted = adaptive ? tessellation_adaptive_vertex_shader_attempted_
                             : tessellation_indexed_vertex_shader_attempted_;
  VkShaderModule& shader_module =
      adaptive ? tessellation_adaptive_vertex_shader_ : tessellation_indexed_vertex_shader_;
  if (attempted) {
    return shader_module;
  }
  attempted = true;

  std::vector<uint32_t> shader_spirv;
  std::string compile_error;
  std::string source = adaptive ? GetTessellationAdaptiveVertexShaderGlsl()
                                : GetTessellationIndexedVertexShaderGlsl();
  if (!command_processor_.CompileGlslToSpirv(VK_SHADER_STAGE_VERTEX_BIT, source, shader_spirv,
                                             compile_error)) {
    REXGPU_ERROR(
        "VulkanPipelineCache: Failed to compile tessellation {} vertex "
        "shader: {}",
        adaptive ? "adaptive" : "indexed", compile_error);
    shader_module = VK_NULL_HANDLE;
    return shader_module;
  }

  shader_module = ui::vulkan::util::CreateShaderModule(command_processor_.GetVulkanDevice(),
                                                       shader_spirv.data(),
                                                       sizeof(uint32_t) * shader_spirv.size());
  if (shader_module == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "VulkanPipelineCache: Failed to create tessellation {} vertex "
        "shader module",
        adaptive ? "adaptive" : "indexed");
  }
  return shader_module;
}

VkShaderModule VulkanPipelineCache::GetTessellationControlShader(
    Shader::HostVertexShaderType host_vertex_shader_type,
    xenos::TessellationMode tessellation_mode) {
  TessellationControlShaderKey key;
  key.host_vertex_shader_type = host_vertex_shader_type;
  key.tessellation_mode = tessellation_mode;
  auto it = tessellation_control_shaders_.find(key);
  if (it != tessellation_control_shaders_.end()) {
    return it->second;
  }

  uint32_t input_control_points =
      GetTessellationPatchControlPointCount(host_vertex_shader_type, tessellation_mode);
  uint32_t output_control_points = 0;
  if (tessellation_mode == xenos::TessellationMode::kAdaptive) {
    output_control_points = 1;
  } else if (IsPatchIndexedHostVertexShaderType(host_vertex_shader_type)) {
    output_control_points = 1;
  } else if (IsTriangleDomainHostVertexShaderType(host_vertex_shader_type)) {
    output_control_points = 3;
  } else if (IsQuadDomainHostVertexShaderType(host_vertex_shader_type)) {
    output_control_points = 4;
  }
  if (!input_control_points || !output_control_points) {
    tessellation_control_shaders_.emplace(key, VK_NULL_HANDLE);
    return VK_NULL_HANDLE;
  }

  std::vector<uint32_t> shader_spirv;
  std::string compile_error;
  std::string source = GetTessellationControlShaderGlsl(
      host_vertex_shader_type, tessellation_mode, input_control_points, output_control_points);
  if (!command_processor_.CompileGlslToSpirv(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, source,
                                             shader_spirv, compile_error)) {
    REXGPU_ERROR(
        "VulkanPipelineCache: Failed to compile tessellation control shader "
        "0x{:08X}: {}",
        key.key, compile_error);
    tessellation_control_shaders_.emplace(key, VK_NULL_HANDLE);
    return VK_NULL_HANDLE;
  }

  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      command_processor_.GetVulkanDevice(), shader_spirv.data(),
      sizeof(uint32_t) * shader_spirv.size());
  if (shader_module == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "VulkanPipelineCache: Failed to create tessellation control shader "
        "module 0x{:08X}",
        key.key);
  }
  tessellation_control_shaders_.emplace(key, shader_module);
  return shader_module;
}

bool VulkanPipelineCache::GetGeometryShaderKey(
    PipelineGeometryShader geometry_shader_type,
    SpirvShaderTranslator::Modification vertex_shader_modification,
    SpirvShaderTranslator::Modification pixel_shader_modification, GeometryShaderKey& key_out) {
  if (geometry_shader_type == PipelineGeometryShader::kNone) {
    return false;
  }
  // For kPointListAsTriangleStrip, output_point_parameters has a different
  // meaning (the coordinates, not the size). However, the AsTriangleStrip host
  // vertex shader types are needed specifically when geometry shaders are not
  // supported as fallbacks.
  if (vertex_shader_modification.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      vertex_shader_modification.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
    assert_always();
    return false;
  }
  GeometryShaderKey key;
  key.type = geometry_shader_type;
  key.interpolator_count = rex::bit_count(vertex_shader_modification.vertex.interpolator_mask);
  key.user_clip_plane_count = vertex_shader_modification.vertex.user_clip_plane_count;
  key.user_clip_plane_cull = vertex_shader_modification.vertex.user_clip_plane_cull;
  key.has_vertex_kill_and = vertex_shader_modification.vertex.vertex_kill_and;
  key.has_point_size = vertex_shader_modification.vertex.output_point_parameters;
  key.has_point_coordinates = pixel_shader_modification.pixel.param_gen_point;
  key.point_ps_ucp_mode = vertex_shader_modification.vertex.point_ps_ucp_mode;
  key_out = key;
  return true;
}

VkShaderModule VulkanPipelineCache::GetGeometryShader(GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key);
  if (it != geometry_shaders_.end()) {
    return it->second;
  }

  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;

  spv::ExecutionMode input_primitive_execution_mode = spv::ExecutionMode(0);
  uint32_t input_primitive_vertex_count = 0;
  spv::ExecutionMode output_primitive_execution_mode = spv::ExecutionMode(0);
  uint32_t output_max_vertices = 0;
  switch (key.type) {
    case PipelineGeometryShader::kPointList:
      // Point to a strip of 2 triangles.
      input_primitive_execution_mode = spv::ExecutionModeInputPoints;
      input_primitive_vertex_count = 1;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case PipelineGeometryShader::kRectangleList:
      // Triangle to a strip of 2 triangles.
      input_primitive_execution_mode = spv::ExecutionModeTriangles;
      input_primitive_vertex_count = 3;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case PipelineGeometryShader::kQuadList:
      // 4 vertices passed via a line list with adjacency to a strip of 2
      // triangles.
      input_primitive_execution_mode = spv::ExecutionModeInputLinesAdjacency;
      input_primitive_vertex_count = 4;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    default:
      assert_unhandled_case(key.type);
  }

  uint32_t clip_distance_count = key.user_clip_plane_cull ? 0 : key.user_clip_plane_count;
  uint32_t point_user_cull_distance_count =
      key.type == PipelineGeometryShader::kPointList && key.user_clip_plane_cull
          ? key.user_clip_plane_count
          : 0;
  uint32_t cull_distance_count = point_user_cull_distance_count + key.has_vertex_kill_and;
  bool point_recalculate_clip_distances = key.type == PipelineGeometryShader::kPointList &&
                                          clip_distance_count && key.point_ps_ucp_mode >= 2;
  bool point_recalculate_cull_distances = key.type == PipelineGeometryShader::kPointList &&
                                          point_user_cull_distance_count &&
                                          key.point_ps_ucp_mode >= 3;

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();
  const ui::vulkan::VulkanDevice::Extensions& device_extensions = vulkan_device->extensions();
  spv::SpvBuildLogger builder_logger;
  spv::SpvBuildLogger* builder_logger_ptr = nullptr;
  spv::SpvVersion spirv_version;
  if (device_properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    spirv_version = spv::Spv_1_5;
  } else if (device_extensions.ext_1_2_KHR_spirv_1_4) {
    spirv_version = spv::Spv_1_4;
  } else if (device_properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    spirv_version = spv::Spv_1_3;
  } else {
    spirv_version = spv::Spv_1_0;
    // Keep the build log around for compatibility diagnostics on older paths.
    builder_logger_ptr = &builder_logger;
  }
  SpirvBuilder builder(spirv_version, (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1,
                       builder_logger_ptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityGeometry);
  if (clip_distance_count) {
    builder.addCapability(spv::CapabilityClipDistance);
  }
  if (cull_distance_count) {
    builder.addCapability(spv::CapabilityCullDistance);
  }
  bool denorm_flush_to_zero_float32 = device_properties.shaderDenormFlushToZeroFloat32;
  bool signed_zero_inf_nan_preserve_float32 =
      device_properties.shaderSignedZeroInfNanPreserveFloat32;
  bool rounding_mode_rte_float32 = device_properties.shaderRoundingModeRTEFloat32;
  if (spirv_version < spv::Spv_1_4) {
    if (denorm_flush_to_zero_float32 || signed_zero_inf_nan_preserve_float32 ||
        rounding_mode_rte_float32) {
      builder.addExtension("SPV_KHR_float_controls");
    }
  }
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  std::vector<spv::Id> main_interface;

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_bool4 = builder.makeVectorType(type_bool, 4);
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float2 = builder.makeVectorType(type_float, 2);
  spv::Id type_float3 = builder.makeVectorType(type_float, 3);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);
  spv::Id type_float4_array_6 =
      builder.makeArrayType(type_float4, builder.makeUintConstant(6), sizeof(float) * 4);
  builder.addDecoration(type_float4_array_6, spv::DecorationArrayStride, sizeof(float) * 4);
  spv::Id type_clip_distances =
      clip_distance_count
          ? builder.makeArrayType(type_float, builder.makeUintConstant(clip_distance_count), 0)
          : spv::NoType;
  spv::Id type_cull_distances =
      cull_distance_count
          ? builder.makeArrayType(type_float, builder.makeUintConstant(cull_distance_count), 0)
          : spv::NoType;

  // System constants.
  // For points:
  // - float2 point_constant_diameter
  // - float2 point_screen_diameter_to_ndc_radius
  // - float3 ndc_scale
  // - float3 ndc_offset
  // - float4[6] user_clip_planes
  enum PointConstant : uint32_t {
    kPointConstantConstantDiameter,
    kPointConstantScreenDiameterToNdcRadius,
    kPointConstantNdcScale,
    kPointConstantNdcOffset,
    kPointConstantUserClipPlanes,
    kPointConstantCount,
  };
  spv::Id type_system_constants = spv::NoType;
  if (key.type == PipelineGeometryShader::kPointList) {
    id_vector_temp.clear();
    id_vector_temp.resize(kPointConstantCount);
    id_vector_temp[kPointConstantConstantDiameter] = type_float2;
    id_vector_temp[kPointConstantScreenDiameterToNdcRadius] = type_float2;
    id_vector_temp[kPointConstantNdcScale] = type_float3;
    id_vector_temp[kPointConstantNdcOffset] = type_float3;
    id_vector_temp[kPointConstantUserClipPlanes] = type_float4_array_6;
    type_system_constants = builder.makeStructType(id_vector_temp, "XeSystemConstants");
    builder.addMemberName(type_system_constants, kPointConstantConstantDiameter,
                          "point_constant_diameter");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantConstantDiameter, spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants, point_constant_diameter)));
    builder.addMemberName(type_system_constants, kPointConstantScreenDiameterToNdcRadius,
                          "point_screen_diameter_to_ndc_radius");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantScreenDiameterToNdcRadius, spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants, point_screen_diameter_to_ndc_radius)));
    builder.addMemberName(type_system_constants, kPointConstantNdcScale, "ndc_scale");
    builder.addMemberDecoration(type_system_constants, kPointConstantNdcScale,
                                spv::DecorationOffset,
                                int(offsetof(SpirvShaderTranslator::SystemConstants, ndc_scale)));
    builder.addMemberName(type_system_constants, kPointConstantNdcOffset, "ndc_offset");
    builder.addMemberDecoration(type_system_constants, kPointConstantNdcOffset,
                                spv::DecorationOffset,
                                int(offsetof(SpirvShaderTranslator::SystemConstants, ndc_offset)));
    builder.addMemberName(type_system_constants, kPointConstantUserClipPlanes, "user_clip_planes");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantUserClipPlanes, spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants, user_clip_planes)));
  }
  spv::Id uniform_system_constants = spv::NoResult;
  if (type_system_constants != spv::NoType) {
    builder.addDecoration(type_system_constants, spv::DecorationBlock);
    uniform_system_constants =
        builder.createVariable(spv::NoPrecision, spv::StorageClassUniform, type_system_constants,
                               "xe_uniform_system_constants");
    builder.addDecoration(uniform_system_constants, spv::DecorationDescriptorSet,
                          int(SpirvShaderTranslator::kDescriptorSetConstants));
    builder.addDecoration(uniform_system_constants, spv::DecorationBinding,
                          int(SpirvShaderTranslator::kConstantBufferSystem));
    // Generating SPIR-V 1.0, no need to add bindings to the entry point's
    // interface until SPIR-V 1.4.
  }

  // Inputs and outputs - matching glslang order, in gl_PerVertex gl_in[],
  // user-defined outputs, user-defined inputs, out gl_PerVertex.

  spv::Id const_input_primitive_vertex_count =
      builder.makeUintConstant(input_primitive_vertex_count);

  // in gl_PerVertex gl_in[].
  // gl_Position.
  id_vector_temp.clear();
  uint32_t member_in_gl_per_vertex_position = uint32_t(id_vector_temp.size());
  id_vector_temp.push_back(type_float4);
  spv::Id const_member_in_gl_per_vertex_position =
      builder.makeIntConstant(int32_t(member_in_gl_per_vertex_position));
  // gl_ClipDistance.
  uint32_t member_in_gl_per_vertex_clip_distance = UINT32_MAX;
  spv::Id const_member_in_gl_per_vertex_clip_distance = spv::NoResult;
  if (clip_distance_count) {
    member_in_gl_per_vertex_clip_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_clip_distances);
    const_member_in_gl_per_vertex_clip_distance =
        builder.makeIntConstant(int32_t(member_in_gl_per_vertex_clip_distance));
  }
  // gl_CullDistance.
  uint32_t member_in_gl_per_vertex_cull_distance = UINT32_MAX;
  if (cull_distance_count) {
    member_in_gl_per_vertex_cull_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_cull_distances);
  }
  // Structure and array.
  spv::Id type_struct_in_gl_per_vertex = builder.makeStructType(id_vector_temp, "gl_PerVertex");
  builder.addMemberName(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_position,
                        "gl_Position");
  builder.addMemberDecoration(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_position,
                              spv::DecorationBuiltIn, spv::BuiltInPosition);
  if (clip_distance_count) {
    builder.addMemberName(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_clip_distance,
                          "gl_ClipDistance");
    builder.addMemberDecoration(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_clip_distance,
                                spv::DecorationBuiltIn, spv::BuiltInClipDistance);
  }
  if (cull_distance_count) {
    builder.addMemberName(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_cull_distance,
                          "gl_CullDistance");
    builder.addMemberDecoration(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_cull_distance,
                                spv::DecorationBuiltIn, spv::BuiltInCullDistance);
  }
  builder.addDecoration(type_struct_in_gl_per_vertex, spv::DecorationBlock);
  spv::Id type_array_in_gl_per_vertex =
      builder.makeArrayType(type_struct_in_gl_per_vertex, const_input_primitive_vertex_count, 0);
  spv::Id in_gl_per_vertex = builder.createVariable(spv::NoPrecision, spv::StorageClassInput,
                                                    type_array_in_gl_per_vertex, "gl_in");
  main_interface.push_back(in_gl_per_vertex);

  uint32_t output_location = 0;

  // Interpolators outputs.
  std::array<spv::Id, xenos::kMaxInterpolators> out_interpolators;
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    spv::Id out_interpolator =
        builder.createVariable(spv::NoPrecision, spv::StorageClassOutput, type_float4,
                               fmt::format("xe_out_interpolator_{}", i).c_str());
    out_interpolators[i] = out_interpolator;
    builder.addDecoration(out_interpolator, spv::DecorationLocation, int(output_location));
    builder.addDecoration(out_interpolator, spv::DecorationInvariant);
    main_interface.push_back(out_interpolator);
    ++output_location;
  }

  // Point coordinate output.
  spv::Id out_point_coordinates = spv::NoResult;
  if (key.has_point_coordinates) {
    out_point_coordinates = builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                   type_float2, "xe_out_point_coordinates");
    builder.addDecoration(out_point_coordinates, spv::DecorationLocation, int(output_location));
    builder.addDecoration(out_point_coordinates, spv::DecorationInvariant);
    main_interface.push_back(out_point_coordinates);
    ++output_location;
  }

  uint32_t input_location = 0;

  // Interpolator inputs.
  std::array<spv::Id, xenos::kMaxInterpolators> in_interpolators;
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    spv::Id in_interpolator = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(type_float4, const_input_primitive_vertex_count, 0),
        fmt::format("xe_in_interpolator_{}", i).c_str());
    in_interpolators[i] = in_interpolator;
    builder.addDecoration(in_interpolator, spv::DecorationLocation, int(input_location));
    main_interface.push_back(in_interpolator);
    ++input_location;
  }

  // Point size input.
  spv::Id in_point_size = spv::NoResult;
  if (key.has_point_size) {
    in_point_size = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(type_float, const_input_primitive_vertex_count, 0),
        "xe_in_point_size");
    builder.addDecoration(in_point_size, spv::DecorationLocation, int(input_location));
    main_interface.push_back(in_point_size);
    ++input_location;
  }

  // out gl_PerVertex.
  // gl_Position.
  id_vector_temp.clear();
  uint32_t member_out_gl_per_vertex_position = uint32_t(id_vector_temp.size());
  id_vector_temp.push_back(type_float4);
  spv::Id const_member_out_gl_per_vertex_position =
      builder.makeIntConstant(int32_t(member_out_gl_per_vertex_position));
  // gl_ClipDistance.
  uint32_t member_out_gl_per_vertex_clip_distance = UINT32_MAX;
  spv::Id const_member_out_gl_per_vertex_clip_distance = spv::NoResult;
  if (clip_distance_count) {
    member_out_gl_per_vertex_clip_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_clip_distances);
    const_member_out_gl_per_vertex_clip_distance =
        builder.makeIntConstant(int32_t(member_out_gl_per_vertex_clip_distance));
  }
  // Structure.
  spv::Id type_struct_out_gl_per_vertex = builder.makeStructType(id_vector_temp, "gl_PerVertex");
  builder.addMemberName(type_struct_out_gl_per_vertex, member_out_gl_per_vertex_position,
                        "gl_Position");
  builder.addMemberDecoration(type_struct_out_gl_per_vertex, member_out_gl_per_vertex_position,
                              spv::DecorationBuiltIn, spv::BuiltInPosition);
  if (clip_distance_count) {
    builder.addMemberName(type_struct_out_gl_per_vertex, member_out_gl_per_vertex_clip_distance,
                          "gl_ClipDistance");
    builder.addMemberDecoration(type_struct_out_gl_per_vertex,
                                member_out_gl_per_vertex_clip_distance, spv::DecorationBuiltIn,
                                spv::BuiltInClipDistance);
  }
  builder.addDecoration(type_struct_out_gl_per_vertex, spv::DecorationBlock);
  spv::Id out_gl_per_vertex = builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                     type_struct_out_gl_per_vertex, "");
  builder.addDecoration(out_gl_per_vertex, spv::DecorationInvariant);
  main_interface.push_back(out_gl_per_vertex);

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function = builder.makeFunctionEntry(
      spv::NoPrecision, type_void, "main", main_param_types, main_precisions, &main_entry);
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelGeometry, main_function, "main");
  for (spv::Id interface_id : main_interface) {
    entry_point->addIdOperand(interface_id);
  }
  builder.addExecutionMode(main_function, input_primitive_execution_mode);
  builder.addExecutionMode(main_function, spv::ExecutionModeInvocations, 1);
  builder.addExecutionMode(main_function, output_primitive_execution_mode);
  builder.addExecutionMode(main_function, spv::ExecutionModeOutputVertices,
                           int(output_max_vertices));
  if (denorm_flush_to_zero_float32) {
    builder.addCapability(spv::CapabilityDenormFlushToZero);
    builder.addExecutionMode(main_function, spv::ExecutionModeDenormFlushToZero, 32);
  }
  if (signed_zero_inf_nan_preserve_float32) {
    builder.addCapability(spv::CapabilitySignedZeroInfNanPreserve);
    builder.addExecutionMode(main_function, spv::ExecutionModeSignedZeroInfNanPreserve, 32);
  }
  if (rounding_mode_rte_float32) {
    builder.addCapability(spv::CapabilityRoundingModeRTE);
    builder.addExecutionMode(main_function, spv::ExecutionModeRoundingModeRTE, 32);
  }

  // Note that after every OpEmitVertex, all output variables are undefined.

  // Discard the whole primitive if any vertex has a NaN position (may also be
  // set to NaN for emulation of vertex killing with the OR operator).
  for (uint32_t i = 0; i < input_primitive_vertex_count; ++i) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
    id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
    spv::Id position_is_nan = builder.createUnaryOp(
        spv::OpAny, type_bool,
        builder.createUnaryOp(
            spv::OpIsNan, type_bool4,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision)));
    spv::Block& discard_predecessor = *builder.getBuildPoint();
    spv::Block& discard_then_block = builder.makeNewBlock();
    spv::Block& discard_merge_block = builder.makeNewBlock();
    builder.createSelectionMerge(&discard_merge_block, spv::SelectionControlDontFlattenMask);
    {
      std::unique_ptr<spv::Instruction> branch_conditional_op(
          std::make_unique<spv::Instruction>(spv::OpBranchConditional));
      branch_conditional_op->addIdOperand(position_is_nan);
      branch_conditional_op->addIdOperand(discard_then_block.getId());
      branch_conditional_op->addIdOperand(discard_merge_block.getId());
      branch_conditional_op->addImmediateOperand(1);
      branch_conditional_op->addImmediateOperand(2);
      discard_predecessor.addInstruction(std::move(branch_conditional_op));
    }
    discard_then_block.addPredecessor(&discard_predecessor);
    discard_merge_block.addPredecessor(&discard_predecessor);
    builder.setBuildPoint(&discard_then_block);
    builder.createNoResultOp(spv::OpReturn);
    builder.setBuildPoint(&discard_merge_block);
  }

  // Cull the whole primitive if any cull distance for all vertices in the
  // primitive is < 0.
  // For point lists with ps_ucp_mode 3, user cull plane distances are
  // calculated per expanded vertex later.
  if (cull_distance_count) {
    spv::Id const_member_in_gl_per_vertex_cull_distance =
        builder.makeIntConstant(int32_t(member_in_gl_per_vertex_cull_distance));
    spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
    spv::Id cull_condition = spv::NoResult;
    uint32_t cull_distance_start =
        point_recalculate_cull_distances ? point_user_cull_distance_count : 0;
    if (cull_distance_start < cull_distance_count) {
      for (uint32_t i = cull_distance_start; i < cull_distance_count; ++i) {
        for (uint32_t j = 0; j < input_primitive_vertex_count; ++j) {
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeIntConstant(int32_t(j)));
          id_vector_temp.push_back(const_member_in_gl_per_vertex_cull_distance);
          id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
          spv::Id cull_distance_is_negative = builder.createBinOp(
              spv::OpFOrdLessThan, type_bool,
              builder.createLoad(builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                                           id_vector_temp),
                                 spv::NoPrecision),
              const_float_0);
          if (cull_condition != spv::NoResult) {
            cull_condition = builder.createBinOp(spv::OpLogicalAnd, type_bool, cull_condition,
                                                 cull_distance_is_negative);
          } else {
            cull_condition = cull_distance_is_negative;
          }
        }
      }
      assert_true(cull_condition != spv::NoResult);
      spv::Block& discard_predecessor = *builder.getBuildPoint();
      spv::Block& discard_then_block = builder.makeNewBlock();
      spv::Block& discard_merge_block = builder.makeNewBlock();
      builder.createSelectionMerge(&discard_merge_block, spv::SelectionControlDontFlattenMask);
      {
        std::unique_ptr<spv::Instruction> branch_conditional_op(
            std::make_unique<spv::Instruction>(spv::OpBranchConditional));
        branch_conditional_op->addIdOperand(cull_condition);
        branch_conditional_op->addIdOperand(discard_then_block.getId());
        branch_conditional_op->addIdOperand(discard_merge_block.getId());
        branch_conditional_op->addImmediateOperand(1);
        branch_conditional_op->addImmediateOperand(2);
        discard_predecessor.addInstruction(std::move(branch_conditional_op));
      }
      discard_then_block.addPredecessor(&discard_predecessor);
      discard_merge_block.addPredecessor(&discard_predecessor);
      builder.setBuildPoint(&discard_then_block);
      builder.createNoResultOp(spv::OpReturn);
      builder.setBuildPoint(&discard_merge_block);
    }
  }

  switch (key.type) {
    case PipelineGeometryShader::kPointList: {
      // Expand the point sprite, with left-to-right, top-to-bottom UVs.

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_float_0 = builder.makeFloatConstant(0.0f);

      // Load the point diameter in guest pixels.
      id_vector_temp.clear();
      id_vector_temp.push_back(builder.makeIntConstant(int32_t(kPointConstantConstantDiameter)));
      id_vector_temp.push_back(const_int_0);
      spv::Id point_guest_diameter_x =
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision);
      id_vector_temp.back() = const_int_1;
      spv::Id point_guest_diameter_y =
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision);
      if (key.has_point_size) {
        // The vertex shader's header writes -1.0 to point_size by default, so
        // any non-negative value means that it was overwritten by the
        // translated vertex shader, and needs to be used instead of the
        // constant size. The per-vertex diameter is already clamped in the
        // vertex shader (combined with making it non-negative).
        id_vector_temp.clear();
        // 0 is the input primitive vertex index.
        id_vector_temp.push_back(const_int_0);
        spv::Id point_vertex_diameter = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_point_size, id_vector_temp),
            spv::NoPrecision);
        spv::Id point_vertex_diameter_written = builder.createBinOp(
            spv::OpFOrdGreaterThanEqual, type_bool, point_vertex_diameter, const_float_0);
        point_guest_diameter_x =
            builder.createTriOp(spv::OpSelect, type_float, point_vertex_diameter_written,
                                point_vertex_diameter, point_guest_diameter_x);
        point_guest_diameter_y =
            builder.createTriOp(spv::OpSelect, type_float, point_vertex_diameter_written,
                                point_vertex_diameter, point_guest_diameter_y);
      }

      // 4D5307F1 has zero-size snowflakes, drop them quicker, and also drop
      // points with a constant size of zero since point lists may also be used
      // as just "compute" with memexport.
      spv::Id point_size_not_zero =
          builder.createBinOp(spv::OpLogicalAnd, type_bool,
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  point_guest_diameter_x, const_float_0),
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  point_guest_diameter_y, const_float_0));
      spv::Block& point_size_zero_predecessor = *builder.getBuildPoint();
      spv::Block& point_size_zero_then_block = builder.makeNewBlock();
      spv::Block& point_size_zero_merge_block = builder.makeNewBlock();
      builder.createSelectionMerge(&point_size_zero_merge_block,
                                   spv::SelectionControlDontFlattenMask);
      {
        std::unique_ptr<spv::Instruction> branch_conditional_op(
            std::make_unique<spv::Instruction>(spv::OpBranchConditional));
        branch_conditional_op->addIdOperand(point_size_not_zero);
        branch_conditional_op->addIdOperand(point_size_zero_merge_block.getId());
        branch_conditional_op->addIdOperand(point_size_zero_then_block.getId());
        branch_conditional_op->addImmediateOperand(2);
        branch_conditional_op->addImmediateOperand(1);
        point_size_zero_predecessor.addInstruction(std::move(branch_conditional_op));
      }
      point_size_zero_then_block.addPredecessor(&point_size_zero_predecessor);
      point_size_zero_merge_block.addPredecessor(&point_size_zero_predecessor);
      builder.setBuildPoint(&point_size_zero_then_block);
      builder.createNoResultOp(spv::OpReturn);
      builder.setBuildPoint(&point_size_zero_merge_block);

      // Transform the diameter in the guest screen coordinates to radius in the
      // normalized device coordinates, and then to the clip space by
      // multiplying by W.
      id_vector_temp.clear();
      id_vector_temp.push_back(
          builder.makeIntConstant(int32_t(kPointConstantScreenDiameterToNdcRadius)));
      id_vector_temp.push_back(const_int_0);
      spv::Id point_radius_x = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_guest_diameter_x,
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision));
      id_vector_temp.back() = const_int_1;
      spv::Id point_radius_y = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_guest_diameter_y,
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision));
      id_vector_temp.clear();
      // 0 is the input primitive vertex index.
      id_vector_temp.push_back(const_int_0);
      id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
      spv::Id point_position = builder.createLoad(
          builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
          spv::NoPrecision);
      spv::Id point_w = builder.createCompositeExtract(point_position, type_float, 3);
      point_radius_x =
          builder.createNoContractionBinOp(spv::OpFMul, type_float, point_radius_x, point_w);
      point_radius_y =
          builder.createNoContractionBinOp(spv::OpFMul, type_float, point_radius_y, point_w);

      // Load the inputs for the guest point.
      // Interpolators.
      std::array<spv::Id, xenos::kMaxInterpolators> point_interpolators;
      id_vector_temp.clear();
      // 0 is the input primitive vertex index.
      id_vector_temp.push_back(const_int_0);
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        point_interpolators[i] = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_interpolators[i], id_vector_temp),
            spv::NoPrecision);
      }
      // Positions.
      spv::Id point_x = builder.createCompositeExtract(point_position, type_float, 0);
      spv::Id point_y = builder.createCompositeExtract(point_position, type_float, 1);
      std::array<spv::Id, 2> point_edge_x, point_edge_y;
      for (uint32_t i = 0; i < 2; ++i) {
        spv::Op point_radius_add_op = i ? spv::OpFAdd : spv::OpFSub;
        point_edge_x[i] = builder.createNoContractionBinOp(point_radius_add_op, type_float, point_x,
                                                           point_radius_x);
        point_edge_y[i] = builder.createNoContractionBinOp(point_radius_add_op, type_float, point_y,
                                                           point_radius_y);
      };
      spv::Id point_z = builder.createCompositeExtract(point_position, type_float, 2);
      // Clip distances.
      spv::Id point_clip_distances = spv::NoResult;
      std::vector<spv::Id> point_user_clip_planes;
      point_user_clip_planes.reserve(clip_distance_count);
      spv::Id point_ndc_scale = spv::NoResult;
      spv::Id point_ndc_offset = spv::NoResult;
      if (point_recalculate_clip_distances || point_recalculate_cull_distances) {
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.makeIntConstant(int32_t(kPointConstantNdcScale)));
        point_ndc_scale =
            builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                         uniform_system_constants, id_vector_temp),
                               spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.makeIntConstant(int32_t(kPointConstantNdcOffset)));
        point_ndc_offset =
            builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                         uniform_system_constants, id_vector_temp),
                               spv::NoPrecision);
        uint32_t point_user_clip_plane_count =
            point_recalculate_cull_distances ? point_user_cull_distance_count : clip_distance_count;
        for (uint32_t i = 0; i < point_user_clip_plane_count; ++i) {
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeIntConstant(int32_t(kPointConstantUserClipPlanes)));
          id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
          point_user_clip_planes.push_back(builder.createLoad(
              builder.createAccessChain(spv::StorageClassUniform, uniform_system_constants,
                                        id_vector_temp),
              spv::NoPrecision));
        }
      } else if (clip_distance_count) {
        id_vector_temp.clear();
        // 0 is the input primitive vertex index.
        id_vector_temp.push_back(const_int_0);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        point_clip_distances = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
      }

      if (point_recalculate_cull_distances) {
        // Cull if any user clip plane has all expanded vertices outside.
        std::array<spv::Id, 4> point_guest_positions;
        for (uint32_t i = 0; i < 4; ++i) {
          uint32_t point_vertex_x = i >> 1;
          uint32_t point_vertex_y = i & 1;
          id_vector_temp.clear();
          id_vector_temp.push_back(point_edge_x[point_vertex_x]);
          id_vector_temp.push_back(point_edge_y[point_vertex_y]);
          id_vector_temp.push_back(point_z);
          id_vector_temp.push_back(point_w);
          spv::Id point_vertex_position =
              builder.createCompositeConstruct(type_float4, id_vector_temp);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 2));
          spv::Id point_vertex_position_xyz =
              builder.createCompositeConstruct(type_float3, id_vector_temp);
          spv::Id point_guest_position_xyz = builder.createNoContractionBinOp(
              spv::OpFSub, type_float3, point_vertex_position_xyz,
              builder.createNoContractionBinOp(spv::OpVectorTimesScalar, type_float3,
                                               point_ndc_offset, point_w));
          point_guest_position_xyz = builder.createNoContractionBinOp(
              spv::OpFDiv, type_float3, point_guest_position_xyz, point_ndc_scale);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 2));
          id_vector_temp.push_back(point_w);
          point_guest_positions[i] = builder.createCompositeConstruct(type_float4, id_vector_temp);
        }
        spv::Id point_cull_condition = spv::NoResult;
        for (uint32_t i = 0; i < point_user_cull_distance_count; ++i) {
          spv::Id point_plane_cull_condition = spv::NoResult;
          for (uint32_t j = 0; j < 4; ++j) {
            spv::Id point_distance_is_negative = builder.createBinOp(
                spv::OpFOrdLessThan, type_bool,
                builder.createBinOp(spv::OpDot, type_float, point_guest_positions[j],
                                    point_user_clip_planes[i]),
                const_float_0);
            if (point_plane_cull_condition != spv::NoResult) {
              point_plane_cull_condition =
                  builder.createBinOp(spv::OpLogicalAnd, type_bool, point_plane_cull_condition,
                                      point_distance_is_negative);
            } else {
              point_plane_cull_condition = point_distance_is_negative;
            }
          }
          if (point_cull_condition != spv::NoResult) {
            point_cull_condition = builder.createBinOp(
                spv::OpLogicalOr, type_bool, point_cull_condition, point_plane_cull_condition);
          } else {
            point_cull_condition = point_plane_cull_condition;
          }
        }
        assert_true(point_cull_condition != spv::NoResult);
        spv::Block& point_cull_predecessor = *builder.getBuildPoint();
        spv::Block& point_cull_then_block = builder.makeNewBlock();
        spv::Block& point_cull_merge_block = builder.makeNewBlock();
        builder.createSelectionMerge(&point_cull_merge_block, spv::SelectionControlDontFlattenMask);
        {
          std::unique_ptr<spv::Instruction> branch_conditional_op(
              std::make_unique<spv::Instruction>(spv::OpBranchConditional));
          branch_conditional_op->addIdOperand(point_cull_condition);
          branch_conditional_op->addIdOperand(point_cull_then_block.getId());
          branch_conditional_op->addIdOperand(point_cull_merge_block.getId());
          branch_conditional_op->addImmediateOperand(1);
          branch_conditional_op->addImmediateOperand(2);
          point_cull_predecessor.addInstruction(std::move(branch_conditional_op));
        }
        point_cull_then_block.addPredecessor(&point_cull_predecessor);
        point_cull_merge_block.addPredecessor(&point_cull_predecessor);
        builder.setBuildPoint(&point_cull_then_block);
        builder.createNoResultOp(spv::OpReturn);
        builder.setBuildPoint(&point_cull_merge_block);
      }

      for (uint32_t i = 0; i < 4; ++i) {
        // Same interpolators for the entire sprite.
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          builder.createStore(point_interpolators[j], out_interpolators[j]);
        }
        // Top-left, bottom-left, top-right, bottom-right order (chosen
        // arbitrarily, simply based on counterclockwise meaning front with
        // frontFace = VkFrontFace(0), but faceness is ignored for non-polygon
        // primitive types).
        uint32_t point_vertex_x = i >> 1;
        uint32_t point_vertex_y = i & 1;
        // Point coordinates.
        if (key.has_point_coordinates) {
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeFloatConstant(float(point_vertex_x)));
          id_vector_temp.push_back(builder.makeFloatConstant(float(point_vertex_y)));
          builder.createStore(builder.makeCompositeConstant(type_float2, id_vector_temp),
                              out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(point_edge_x[point_vertex_x]);
        id_vector_temp.push_back(point_edge_y[point_vertex_y]);
        id_vector_temp.push_back(point_z);
        id_vector_temp.push_back(point_w);
        spv::Id point_vertex_position =
            builder.createCompositeConstruct(type_float4, id_vector_temp);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            point_vertex_position,
            builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (point_recalculate_clip_distances) {
          // Convert host clip space back to guest clip space before applying
          // user clip planes.
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 2));
          spv::Id point_vertex_position_xyz =
              builder.createCompositeConstruct(type_float3, id_vector_temp);
          spv::Id point_guest_position_xyz = builder.createNoContractionBinOp(
              spv::OpFSub, type_float3, point_vertex_position_xyz,
              builder.createNoContractionBinOp(spv::OpVectorTimesScalar, type_float3,
                                               point_ndc_offset, point_w));
          point_guest_position_xyz = builder.createNoContractionBinOp(
              spv::OpFDiv, type_float3, point_guest_position_xyz, point_ndc_scale);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 2));
          id_vector_temp.push_back(point_w);
          spv::Id point_guest_position =
              builder.createCompositeConstruct(type_float4, id_vector_temp);
          for (uint32_t j = 0; j < clip_distance_count; ++j) {
            id_vector_temp.clear();
            id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
            id_vector_temp.push_back(builder.makeIntConstant(int32_t(j)));
            builder.createStore(builder.createBinOp(spv::OpDot, type_float, point_guest_position,
                                                    point_user_clip_planes[j]),
                                builder.createAccessChain(spv::StorageClassOutput,
                                                          out_gl_per_vertex, id_vector_temp));
          }
        } else if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(point_clip_distances,
                              builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex,
                                                        id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case PipelineGeometryShader::kRectangleList: {
      // Construct a strip with the fourth vertex generated by mirroring a
      // vertex across the longest edge (the diagonal).
      //
      // Possible options:
      //
      // 0---1
      // |  /|
      // | / |  - 12 is the longest edge, strip 0123 (most commonly used)
      // |/  |    v3 = v0 + (v1 - v0) + (v2 - v0), or v3 = -v0 + v1 + v2
      // 2--[3]
      //
      // 1---2
      // |  /|
      // | / |  - 20 is the longest edge, strip 1203
      // |/  |
      // 0--[3]
      //
      // 2---0
      // |  /|
      // | / |  - 01 is the longest edge, strip 2013
      // |/  |
      // 1--[3]

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_int_2 = builder.makeIntConstant(2);
      spv::Id const_int_3 = builder.makeIntConstant(3);

      // Get squares of edge lengths to choose the longest edge.
      // [0] - 12, [1] - 20, [2] - 01.
      spv::Id edge_lengths[3];
      id_vector_temp.resize(3);
      id_vector_temp[1] = const_member_in_gl_per_vertex_position;
      for (uint32_t i = 0; i < 3; ++i) {
        id_vector_temp[0] = builder.makeIntConstant(int32_t((1 + i) % 3));
        id_vector_temp[2] = const_int_0;
        spv::Id edge_0_x = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[2] = const_int_1;
        spv::Id edge_0_y = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = builder.makeIntConstant(int32_t((2 + i) % 3));
        id_vector_temp[2] = const_int_0;
        spv::Id edge_1_x = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[2] = const_int_1;
        spv::Id edge_1_y = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        spv::Id edge_x = builder.createBinOp(spv::OpFSub, type_float, edge_1_x, edge_0_x);
        spv::Id edge_y = builder.createBinOp(spv::OpFSub, type_float, edge_1_y, edge_0_y);
        edge_lengths[i] = builder.createBinOp(
            spv::OpFAdd, type_float, builder.createBinOp(spv::OpFMul, type_float, edge_x, edge_x),
            builder.createBinOp(spv::OpFMul, type_float, edge_y, edge_y));
      }

      // Choose the index of the first vertex in the strip based on which edge
      // is the longest, and calculate the indices of the other vertices.
      spv::Id vertex_indices[3];
      // If 12 > 20 && 12 > 01, then 12 is the longest edge, and the strip is
      // 0123. Otherwise, if 20 > 01, then 20 is the longest, and the strip is
      // 1203, but if not, 01 is the longest, and the strip is 2013.
      vertex_indices[0] = builder.createTriOp(
          spv::OpSelect, type_int,
          builder.createBinOp(spv::OpLogicalAnd, type_bool,
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  edge_lengths[0], edge_lengths[1]),
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  edge_lengths[0], edge_lengths[2])),
          const_int_0,
          builder.createTriOp(spv::OpSelect, type_int,
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  edge_lengths[1], edge_lengths[2]),
                              const_int_1, const_int_2));
      for (uint32_t i = 1; i < 3; ++i) {
        // vertex_indices[i] = (vertex_indices[0] + i) % 3
        spv::Id vertex_index_without_wrapping = builder.createBinOp(
            spv::OpIAdd, type_int, vertex_indices[0], builder.makeIntConstant(int32_t(i)));
        vertex_indices[i] = builder.createTriOp(
            spv::OpSelect, type_int,
            builder.createBinOp(spv::OpSLessThan, type_bool, vertex_index_without_wrapping,
                                const_int_3),
            vertex_index_without_wrapping,
            builder.createBinOp(spv::OpISub, type_int, vertex_index_without_wrapping, const_int_3));
      }

      // Initialize the point coordinates output for safety if this shader type
      // is used with has_point_coordinates for some reason.
      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (key.has_point_coordinates) {
        spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero = builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      // Emit the triangle in the strip that consists of the original vertices.
      for (uint32_t i = 0; i < 3; ++i) {
        spv::Id vertex_index = vertex_indices[i];
        // Interpolators.
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_index);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(spv::StorageClassInput,
                                                           in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        // Point coordinates.
        if (key.has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero, out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_index);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
        spv::Id vertex_position = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            vertex_position,
            builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(vertex_index);
          id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
          spv::Id vertex_clip_distances = builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision);
          id_vector_temp.clear();
          id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(vertex_clip_distances,
                              builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex,
                                                        id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }

      // Construct the fourth vertex.
      // Interpolators.
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        spv::Id in_interpolator = in_interpolators[i];
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_indices[0]);
        spv::Id vertex_interpolator_v0 = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_interpolator, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = vertex_indices[1];
        spv::Id vertex_interpolator_v01 = builder.createNoContractionBinOp(
            spv::OpFSub, type_float4,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_interpolator, id_vector_temp),
                spv::NoPrecision),
            vertex_interpolator_v0);
        id_vector_temp[0] = vertex_indices[2];
        spv::Id vertex_interpolator_v3 = builder.createNoContractionBinOp(
            spv::OpFAdd, type_float4, vertex_interpolator_v01,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_interpolator, id_vector_temp),
                spv::NoPrecision));
        builder.createStore(vertex_interpolator_v3, out_interpolators[i]);
      }
      // Point coordinates.
      if (key.has_point_coordinates) {
        builder.createStore(const_point_coordinates_zero, out_point_coordinates);
      }
      // Position.
      id_vector_temp.clear();
      id_vector_temp.push_back(vertex_indices[0]);
      id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
      spv::Id vertex_position_v0 = builder.createLoad(
          builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
          spv::NoPrecision);
      id_vector_temp[0] = vertex_indices[1];
      spv::Id vertex_position_v01 = builder.createNoContractionBinOp(
          spv::OpFSub, type_float4,
          builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision),
          vertex_position_v0);
      id_vector_temp[0] = vertex_indices[2];
      spv::Id vertex_position_v3 = builder.createNoContractionBinOp(
          spv::OpFAdd, type_float4, vertex_position_v01,
          builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision));
      id_vector_temp.clear();
      id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
      builder.createStore(
          vertex_position_v3,
          builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
      // Clip distances.
      for (uint32_t i = 0; i < clip_distance_count; ++i) {
        spv::Id const_int_i = builder.makeIntConstant(int32_t(i));
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_indices[0]);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        id_vector_temp.push_back(const_int_i);
        spv::Id vertex_clip_distance_v0 = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = vertex_indices[1];
        spv::Id vertex_clip_distance_v01 = builder.createNoContractionBinOp(
            spv::OpFSub, type_float,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision),
            vertex_clip_distance_v0);
        id_vector_temp[0] = vertex_indices[2];
        spv::Id vertex_clip_distance_v3 = builder.createNoContractionBinOp(
            spv::OpFAdd, type_float, vertex_clip_distance_v01,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision));
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
        id_vector_temp.push_back(const_int_i);
        builder.createStore(
            vertex_clip_distance_v3,
            builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
      }
      // Emit the vertex.
      builder.createNoResultOp(spv::OpEmitVertex);
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case PipelineGeometryShader::kQuadList: {
      // Initialize the point coordinates output for safety if this shader type
      // is used with has_point_coordinates for some reason.
      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (key.has_point_coordinates) {
        spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero = builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      // Build the triangle strip from the original quad vertices in the
      // 0, 1, 3, 2 order (like specified for GL_QUAD_STRIP).
      // TODO(Triang3l): Find the correct decomposition of quads into triangles
      // on the real hardware.
      for (uint32_t i = 0; i < 4; ++i) {
        spv::Id const_vertex_index = builder.makeIntConstant(int32_t(i ^ (i >> 1)));
        // Interpolators.
        id_vector_temp.clear();
        id_vector_temp.push_back(const_vertex_index);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(spv::StorageClassInput,
                                                           in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        // Point coordinates.
        if (key.has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero, out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(const_vertex_index);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
        spv::Id vertex_position = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            vertex_position,
            builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(const_vertex_index);
          id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
          spv::Id vertex_clip_distances = builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision);
          id_vector_temp.clear();
          id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(vertex_clip_distances,
                              builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex,
                                                        id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    default:
      assert_unhandled_case(key.type);
  }

  // End the main function.
  builder.leaveFunction();

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the shader module, and store the handle even if creation fails not
  // to try to create it again later.
  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      command_processor_.GetVulkanDevice(), reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (shader_module == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "VulkanPipelineCache: Failed to create the primitive type geometry "
        "shader 0x{:08X}",
        key.key);
  }
  geometry_shaders_.emplace(key, shader_module);
  return shader_module;
}

bool VulkanPipelineCache::TryGetPipelineCreationArgumentsForDescription(
    const PipelineDescription& description,
    std::pair<const PipelineDescription, Pipeline>* pipeline,
    PipelineCreationArguments& creation_arguments, bool for_placeholder) {
  if (!pipeline) {
    return false;
  }
  if (!ArePipelineRequirementsMet(description)) {
    return false;
  }

  auto vertex_shader_it = shaders_.find(description.vertex_shader_hash);
  if (vertex_shader_it == shaders_.end()) {
    return false;
  }
  auto* vertex_shader = static_cast<VulkanShader::VulkanTranslation*>(
      vertex_shader_it->second->GetTranslation(description.vertex_shader_modification));
  if (!vertex_shader || !vertex_shader->is_translated() || !vertex_shader->is_valid()) {
    return false;
  }
  SpirvShaderTranslator::Modification vertex_shader_modification(
      description.vertex_shader_modification);
  Shader::HostVertexShaderType host_vertex_shader_type =
      vertex_shader_modification.vertex.host_vertex_shader_type;
  bool tessellated = Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type);

  VulkanShader::VulkanTranslation* pixel_shader = nullptr;
  if (description.pixel_shader_hash && !for_placeholder) {
    auto pixel_shader_it = shaders_.find(description.pixel_shader_hash);
    if (pixel_shader_it == shaders_.end()) {
      return false;
    }
    pixel_shader = static_cast<VulkanShader::VulkanTranslation*>(
        pixel_shader_it->second->GetTranslation(description.pixel_shader_modification));
    if (!pixel_shader || !pixel_shader->is_translated() || !pixel_shader->is_valid()) {
      return false;
    }
  }
  SpirvShaderTranslator::Modification pixel_shader_modification(
      description.pixel_shader_modification);

  VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
  VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
  uint32_t tessellation_patch_control_points = 0;
  if (tessellated) {
    xenos::TessellationMode tessellation_mode = description.tessellation_mode;
    tessellation_patch_control_points =
        GetTessellationPatchControlPointCount(host_vertex_shader_type, tessellation_mode);
    if (!tessellation_patch_control_points) {
      return false;
    }
    tessellation_vertex_shader =
        GetTessellationVertexShader(tessellation_mode == xenos::TessellationMode::kAdaptive);
    if (tessellation_vertex_shader == VK_NULL_HANDLE) {
      return false;
    }
    tessellation_control_shader =
        GetTessellationControlShader(host_vertex_shader_type, tessellation_mode);
    if (tessellation_control_shader == VK_NULL_HANDLE) {
      return false;
    }
  }

  VkShaderModule geometry_shader = VK_NULL_HANDLE;
  GeometryShaderKey geometry_shader_key;
  if (GetGeometryShaderKey(description.geometry_shader, vertex_shader_modification,
                           pixel_shader_modification, geometry_shader_key)) {
    geometry_shader = GetGeometryShader(geometry_shader_key);
    if (geometry_shader == VK_NULL_HANDLE) {
      return false;
    }
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && vulkan_device->properties().dynamicRendering;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (!use_dynamic_rendering) {
    render_pass =
        render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock
            ? render_target_cache_.GetFragmentShaderInterlockRenderPass()
            : render_target_cache_.GetHostRenderTargetsRenderPass(description.render_pass_key);
    if (render_pass == VK_NULL_HANDLE) {
      return false;
    }
  }

  const PipelineLayoutProvider* pipeline_layout = command_processor_.GetPipelineLayout(
      (!for_placeholder && pixel_shader) ? static_cast<const VulkanShader&>(pixel_shader->shader())
                                               .GetTextureBindingsAfterTranslation()
                                               .size()
                                         : 0,
      (!for_placeholder && pixel_shader) ? static_cast<const VulkanShader&>(pixel_shader->shader())
                                               .GetSamplerBindingsAfterTranslation()
                                               .size()
                                         : 0,
      static_cast<const VulkanShader&>(vertex_shader->shader())
          .GetTextureBindingsAfterTranslation()
          .size(),
      static_cast<const VulkanShader&>(vertex_shader->shader())
          .GetSamplerBindingsAfterTranslation()
          .size());
  if (!pipeline_layout) {
    return false;
  }

  creation_arguments.pipeline = pipeline;
  creation_arguments.pipeline_layout = pipeline_layout;
  creation_arguments.vertex_shader = vertex_shader;
  creation_arguments.pixel_shader = for_placeholder ? nullptr : pixel_shader;
  creation_arguments.tessellation_vertex_shader = tessellation_vertex_shader;
  creation_arguments.tessellation_control_shader = tessellation_control_shader;
  creation_arguments.tessellation_patch_control_points = tessellation_patch_control_points;
  creation_arguments.geometry_shader = geometry_shader;
  creation_arguments.render_pass = render_pass;
  return true;
}

bool VulkanPipelineCache::EnsurePipelineCreated(const PipelineCreationArguments& creation_arguments,
                                                VkShaderModule fragment_shader_override) {
  VkPipeline existing_pipeline =
      creation_arguments.pipeline->second.pipeline.load(std::memory_order_acquire);
  bool is_placeholder =
      creation_arguments.pipeline->second.is_placeholder.load(std::memory_order_acquire);
  bool creating_placeholder = fragment_shader_override != VK_NULL_HANDLE;
  if (existing_pipeline != VK_NULL_HANDLE) {
    if (!is_placeholder || creating_placeholder) {
      return true;
    }
  }

  // This function preferably should validate the description to prevent
  // unsupported behavior that may be dangerous/crashing because pipelines can
  // be created from the disk storage.

  if (creation_arguments.pixel_shader) {
    REXGPU_INFO("Creating graphics pipeline state with VS {:016X}, PS {:016X}",
                creation_arguments.vertex_shader->shader().ucode_data_hash(),
                creation_arguments.pixel_shader->shader().ucode_data_hash());
  } else {
    REXGPU_INFO("Creating graphics pipeline state with VS {:016X}",
                creation_arguments.vertex_shader->shader().ucode_data_hash());
  }

  const PipelineDescription& description = creation_arguments.pipeline->first;
  if (!ArePipelineRequirementsMet(description)) {
    assert_always(
        "When creating a new pipeline, the description must not require "
        "unsupported features, and when loading the pipeline storage, "
        "pipelines with unsupported features must be filtered out");
    return false;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  bool tessellated = description.primitive_topology == PipelinePrimitiveTopology::kPatchList;

  std::array<VkPipelineShaderStageCreateInfo, 5> shader_stages;
  uint32_t shader_stage_count = 0;

  // Vertex or tessellation evaluation shader (plus helper stages for tessellation).
  assert_true(creation_arguments.vertex_shader->is_translated());
  if (!creation_arguments.vertex_shader->is_valid()) {
    return false;
  }
  if (tessellated) {
    if (creation_arguments.tessellation_vertex_shader == VK_NULL_HANDLE ||
        creation_arguments.tessellation_control_shader == VK_NULL_HANDLE ||
        !creation_arguments.tessellation_patch_control_points) {
      return false;
    }

    VkPipelineShaderStageCreateInfo& shader_stage_vertex = shader_stages[shader_stage_count++];
    shader_stage_vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_vertex.pNext = nullptr;
    shader_stage_vertex.flags = 0;
    shader_stage_vertex.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stage_vertex.module = creation_arguments.tessellation_vertex_shader;
    shader_stage_vertex.pName = "main";
    shader_stage_vertex.pSpecializationInfo = nullptr;

    VkPipelineShaderStageCreateInfo& shader_stage_tess_control =
        shader_stages[shader_stage_count++];
    shader_stage_tess_control.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tess_control.pNext = nullptr;
    shader_stage_tess_control.flags = 0;
    shader_stage_tess_control.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    shader_stage_tess_control.module = creation_arguments.tessellation_control_shader;
    shader_stage_tess_control.pName = "main";
    shader_stage_tess_control.pSpecializationInfo = nullptr;

    VkPipelineShaderStageCreateInfo& shader_stage_tess_eval = shader_stages[shader_stage_count++];
    shader_stage_tess_eval.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_tess_eval.pNext = nullptr;
    shader_stage_tess_eval.flags = 0;
    shader_stage_tess_eval.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    shader_stage_tess_eval.module = creation_arguments.vertex_shader->shader_module();
    assert_true(shader_stage_tess_eval.module != VK_NULL_HANDLE);
    shader_stage_tess_eval.pName = "main";
    shader_stage_tess_eval.pSpecializationInfo = nullptr;
  } else {
    VkPipelineShaderStageCreateInfo& shader_stage_vertex = shader_stages[shader_stage_count++];
    shader_stage_vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_vertex.pNext = nullptr;
    shader_stage_vertex.flags = 0;
    shader_stage_vertex.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stage_vertex.module = creation_arguments.vertex_shader->shader_module();
    assert_true(shader_stage_vertex.module != VK_NULL_HANDLE);
    shader_stage_vertex.pName = "main";
    shader_stage_vertex.pSpecializationInfo = nullptr;
  }
  // Geometry shader.
  if (creation_arguments.geometry_shader != VK_NULL_HANDLE) {
    if (tessellated) {
      return false;
    }
    VkPipelineShaderStageCreateInfo& shader_stage_geometry = shader_stages[shader_stage_count++];
    shader_stage_geometry.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_geometry.pNext = nullptr;
    shader_stage_geometry.flags = 0;
    shader_stage_geometry.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    shader_stage_geometry.module = creation_arguments.geometry_shader;
    shader_stage_geometry.pName = "main";
    shader_stage_geometry.pSpecializationInfo = nullptr;
  }
  // Fragment shader.
  VkPipelineShaderStageCreateInfo& shader_stage_fragment = shader_stages[shader_stage_count++];
  shader_stage_fragment.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stage_fragment.pNext = nullptr;
  shader_stage_fragment.flags = 0;
  shader_stage_fragment.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shader_stage_fragment.module = VK_NULL_HANDLE;
  shader_stage_fragment.pName = "main";
  shader_stage_fragment.pSpecializationInfo = nullptr;
  if (fragment_shader_override != VK_NULL_HANDLE) {
    shader_stage_fragment.module = fragment_shader_override;
  } else if (creation_arguments.pixel_shader) {
    assert_true(creation_arguments.pixel_shader->is_translated());
    if (!creation_arguments.pixel_shader->is_valid()) {
      return false;
    }
    shader_stage_fragment.module = creation_arguments.pixel_shader->shader_module();
    assert_true(shader_stage_fragment.module != VK_NULL_HANDLE);
  } else {
    if (edram_fragment_shader_interlock) {
      shader_stage_fragment.module = depth_only_fragment_shader_;
    } else if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
               (description.render_pass_key.depth_and_color_used & 1) &&
               (description.depth_compare_op != xenos::CompareFunction::kAlways ||
                description.depth_write_enable) &&
               description.render_pass_key.depth_format ==
                   xenos::DepthRenderTargetFormat::kD24FS8) {
      shader_stage_fragment.module = render_target_cache_.depth_float24_round()
                                         ? depth_float24_round_fragment_shader_
                                         : depth_float24_truncate_fragment_shader_;
      if (shader_stage_fragment.module == VK_NULL_HANDLE) {
        return false;
      }
    }
  }
  if (shader_stage_fragment.module == VK_NULL_HANDLE) {
    --shader_stage_count;
  }

  VkPipelineVertexInputStateCreateInfo vertex_input_state = {};
  vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
  input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.pNext = nullptr;
  input_assembly_state.flags = 0;
  switch (description.primitive_topology) {
    case PipelinePrimitiveTopology::kPointList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kLineList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kLineStrip:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kTriangleStrip:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleFan:
      // Keep parity with D3D12 by requiring triangle fan to list conversion in
      // PrimitiveProcessor rather than emitting native fan pipelines.
      assert_always();
      return false;
    case PipelinePrimitiveTopology::kLineListWithAdjacency:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kPatchList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
      assert_false(description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    default:
      assert_unhandled_case(description.primitive_topology);
      return false;
  }
  input_assembly_state.primitiveRestartEnable = description.primitive_restart ? VK_TRUE : VK_FALSE;

  VkPipelineViewportStateCreateInfo viewport_state;
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.flags = 0;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = nullptr;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.rasterizerDiscardEnable = description.rasterizer_discard ? VK_TRUE : VK_FALSE;
  rasterization_state.depthClampEnable = description.depth_clamp_enable ? VK_TRUE : VK_FALSE;
  switch (description.polygon_mode) {
    case PipelinePolygonMode::kFill:
      rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
      break;
    case PipelinePolygonMode::kLine:
      rasterization_state.polygonMode = VK_POLYGON_MODE_LINE;
      break;
    case PipelinePolygonMode::kPoint:
      rasterization_state.polygonMode = VK_POLYGON_MODE_POINT;
      break;
    default:
      assert_unhandled_case(description.polygon_mode);
      return false;
  }
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  if (description.cull_front) {
    rasterization_state.cullMode |= VK_CULL_MODE_FRONT_BIT;
  }
  if (description.cull_back) {
    rasterization_state.cullMode |= VK_CULL_MODE_BACK_BIT;
  }
  rasterization_state.frontFace =
      description.front_face_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // Depth bias is dynamic (even toggling - pipeline creation is expensive).
  // "If no depth attachment is present, r is undefined" in the depth bias
  // formula, though Z has no effect on anything if a depth attachment is not
  // used (the guest shader can't access Z), enabling only when there's a
  // depth / stencil attachment for correctness.
  rasterization_state.depthBiasEnable =
      (!edram_fragment_shader_interlock && (description.render_pass_key.depth_and_color_used & 0b1))
          ? VK_TRUE
          : VK_FALSE;
  // TODO(Triang3l): Wide lines.
  rasterization_state.lineWidth = 1.0f;

  bool subpass_has_attachments =
      !edram_fragment_shader_interlock && description.render_pass_key.depth_and_color_used != 0;
  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  if (description.rasterizer_discard) {
    // Keep rasterizer-discard pipelines independent from guest MSAA state, as
    // done by D3D12 when rasterization is disabled.
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  } else if (!edram_fragment_shader_interlock && !subpass_has_attachments) {
    // Keep parity with D3D12 host-render-target path, where draws without
    // color/depth attachments must run at 1x sample count.
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  } else if (description.render_pass_key.msaa_samples == xenos::MsaaSamples::k2X &&
             !render_target_cache_.IsMsaa2xSupported(subpass_has_attachments)) {
    // Using sample 0 as 0 and 3 as 1 for 2x instead (not exactly the same
    // sample locations, but still top-left and bottom-right - however, this can
    // be adjusted with custom sample locations).
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    // Keep parity with D3D12 ROV: sample masks are ignored without attachments.
    if (subpass_has_attachments) {
      sample_mask = 0b1001;
      multisample_state.pSampleMask = &sample_mask;
    }
  } else {
    multisample_state.rasterizationSamples =
        VkSampleCountFlagBits(uint32_t(1) << uint32_t(description.render_pass_key.msaa_samples));
  }
  if (description.sample_rate_shading &&
      multisample_state.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT) {
    multisample_state.sampleShadingEnable = VK_TRUE;
    multisample_state.minSampleShading = 1.0f;
  }

  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil_state.pNext = nullptr;
  if (!edram_fragment_shader_interlock) {
    if (description.depth_write_enable ||
        description.depth_compare_op != xenos::CompareFunction::kAlways) {
      depth_stencil_state.depthTestEnable = VK_TRUE;
      depth_stencil_state.depthWriteEnable = description.depth_write_enable ? VK_TRUE : VK_FALSE;
      depth_stencil_state.depthCompareOp =
          VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(description.depth_compare_op));
    }
    if (description.stencil_test_enable) {
      depth_stencil_state.stencilTestEnable = VK_TRUE;
      depth_stencil_state.front.failOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_front_fail_op));
      depth_stencil_state.front.passOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_front_pass_op));
      depth_stencil_state.front.depthFailOp = VkStencilOp(
          uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_front_depth_fail_op));
      depth_stencil_state.front.compareOp = VkCompareOp(
          uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(description.stencil_front_compare_op));
      depth_stencil_state.back.failOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_back_fail_op));
      depth_stencil_state.back.passOp =
          VkStencilOp(uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_back_pass_op));
      depth_stencil_state.back.depthFailOp = VkStencilOp(
          uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(description.stencil_back_depth_fail_op));
      depth_stencil_state.back.compareOp = VkCompareOp(
          uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(description.stencil_back_compare_op));
    }
  }

  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  VkPipelineColorBlendAttachmentState color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  if (!edram_fragment_shader_interlock) {
    uint32_t color_rts_used = description.render_pass_key.depth_and_color_used >> 1;
    {
      static const VkBlendFactor kBlendFactorMap[] = {
          VK_BLEND_FACTOR_ZERO,
          VK_BLEND_FACTOR_ONE,
          VK_BLEND_FACTOR_SRC_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
          VK_BLEND_FACTOR_DST_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
          VK_BLEND_FACTOR_SRC_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          VK_BLEND_FACTOR_DST_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
          VK_BLEND_FACTOR_CONSTANT_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
          VK_BLEND_FACTOR_CONSTANT_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
          VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
      };
      // 8 entries for safety since 3 bits from the guest are passed directly.
      static const VkBlendOp kBlendOpMap[] = {VK_BLEND_OP_ADD,
                                              VK_BLEND_OP_SUBTRACT,
                                              VK_BLEND_OP_MIN,
                                              VK_BLEND_OP_MAX,
                                              VK_BLEND_OP_REVERSE_SUBTRACT,
                                              VK_BLEND_OP_ADD,
                                              VK_BLEND_OP_ADD,
                                              VK_BLEND_OP_ADD};
      assert_true(vulkan_device->properties().independentBlend);
      uint32_t color_rts_remaining = color_rts_used;
      uint32_t color_rt_index;
      while (rex::bit_scan_forward(color_rts_remaining, &color_rt_index)) {
        color_rts_remaining &= ~(uint32_t(1) << color_rt_index);
        VkPipelineColorBlendAttachmentState& color_blend_attachment =
            color_blend_attachments[color_rt_index];
        const PipelineRenderTarget& color_rt = description.render_targets[color_rt_index];
        if (color_rt.src_color_blend_factor != PipelineBlendFactor::kOne ||
            color_rt.dst_color_blend_factor != PipelineBlendFactor::kZero ||
            color_rt.color_blend_op != xenos::BlendOp::kAdd ||
            color_rt.src_alpha_blend_factor != PipelineBlendFactor::kOne ||
            color_rt.dst_alpha_blend_factor != PipelineBlendFactor::kZero ||
            color_rt.alpha_blend_op != xenos::BlendOp::kAdd) {
          color_blend_attachment.blendEnable = VK_TRUE;
          color_blend_attachment.srcColorBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.src_color_blend_factor)];
          color_blend_attachment.dstColorBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.dst_color_blend_factor)];
          color_blend_attachment.colorBlendOp = kBlendOpMap[uint32_t(color_rt.color_blend_op)];
          color_blend_attachment.srcAlphaBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.src_alpha_blend_factor)];
          color_blend_attachment.dstAlphaBlendFactor =
              kBlendFactorMap[uint32_t(color_rt.dst_alpha_blend_factor)];
          color_blend_attachment.alphaBlendOp = kBlendOpMap[uint32_t(color_rt.alpha_blend_op)];
        }
        color_blend_attachment.colorWriteMask = VkColorComponentFlags(color_rt.color_write_mask);
      }
    }
    color_blend_state.attachmentCount = 32 - rex::lzcnt(color_rts_used);
    color_blend_state.pAttachments = color_blend_attachments;
  }

  std::array<VkDynamicState, 7> dynamic_states;
  VkPipelineDynamicStateCreateInfo dynamic_state;
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.pNext = nullptr;
  dynamic_state.flags = 0;
  dynamic_state.dynamicStateCount = 0;
  dynamic_state.pDynamicStates = dynamic_states.data();
  // Regardless of whether some of this state actually has any effect on the
  // pipeline, marking all as dynamic because otherwise, binding any pipeline
  // with such state not marked as dynamic will cause the dynamic state to be
  // invalidated (again, even if it has no effect).
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
  if (!edram_fragment_shader_interlock) {
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_DEPTH_BIAS;
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
  }

  VkPipelineTessellationStateCreateInfo tessellation_state = {};
  if (tessellated) {
    tessellation_state.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tessellation_state.patchControlPoints = creation_arguments.tessellation_patch_control_points;
  }

  VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {};
  VkFormat color_attachment_formats[xenos::kMaxColorRenderTargets] = {};
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && vulkan_device->properties().dynamicRendering;
  if (use_dynamic_rendering) {
    pipeline_rendering_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipeline_rendering_create_info.pNext = nullptr;
    pipeline_rendering_create_info.viewMask = 0;

    uint32_t color_attachment_count = 0;
    const auto& key = description.render_pass_key;
    xenos::ColorRenderTargetFormat color_formats[] = {
        key.color_0_view_format, key.color_1_view_format, key.color_2_view_format,
        key.color_3_view_format};
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (key.depth_and_color_used & (1 << (1 + i))) {
        color_attachment_formats[i] = render_target_cache_.GetColorVulkanFormat(color_formats[i]);
        color_attachment_count = i + 1;
      }
    }
    pipeline_rendering_create_info.colorAttachmentCount = color_attachment_count;
    pipeline_rendering_create_info.pColorAttachmentFormats =
        color_attachment_count ? color_attachment_formats : nullptr;

    if (key.depth_and_color_used & 0b1) {
      VkFormat depth_format = render_target_cache_.GetDepthVulkanFormat(key.depth_format);
      pipeline_rendering_create_info.depthAttachmentFormat = depth_format;
      pipeline_rendering_create_info.stencilAttachmentFormat = depth_format;
    }
  }

  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = use_dynamic_rendering ? &pipeline_rendering_create_info : nullptr;
  pipeline_create_info.flags = 0;
  pipeline_create_info.stageCount = shader_stage_count;
  pipeline_create_info.pStages = shader_stages.data();
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState = tessellated ? &tessellation_state : nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = &depth_stencil_state;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  if (creation_arguments.pipeline_layout == nullptr) {
    return false;
  }
  pipeline_create_info.layout = creation_arguments.pipeline_layout->GetPipelineLayout();
  pipeline_create_info.renderPass =
      use_dynamic_rendering ? VK_NULL_HANDLE : creation_arguments.render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkPipeline pipeline;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                    &pipeline) != VK_SUCCESS) {
    // TODO(Triang3l): Move these error messages outside.
    /* if (creation_arguments.pixel_shader) {
      REXGPU_ERROR(
          "Failed to create graphics pipeline with VS {:016X}, PS {:016X}",
          creation_arguments.vertex_shader->shader().ucode_data_hash(),
          creation_arguments.pixel_shader->shader().ucode_data_hash());
    } else {
      REXGPU_ERROR("Failed to create graphics pipeline with VS {:016X}",
             creation_arguments.vertex_shader->shader().ucode_data_hash());
    } */
    return false;
  }
  bool was_placeholder =
      creation_arguments.pipeline->second.is_placeholder.load(std::memory_order_acquire);
  VkPipeline old_pipeline =
      creation_arguments.pipeline->second.pipeline.exchange(pipeline, std::memory_order_acq_rel);
  creation_arguments.pipeline->second.pipeline_layout.store(creation_arguments.pipeline_layout,
                                                            std::memory_order_release);
  if (creating_placeholder) {
    creation_arguments.pipeline->second.is_placeholder.store(true, std::memory_order_release);
  } else {
    creation_arguments.pipeline->second.is_placeholder.store(false, std::memory_order_release);
    if (was_placeholder && old_pipeline != VK_NULL_HANDLE) {
      std::lock_guard<std::mutex> lock(deferred_destroy_lock_);
      deferred_destroy_pipelines_.emplace_back(command_processor_.GetCurrentSubmission(),
                                               old_pipeline);
    }
  }

  return true;
}

void VulkanPipelineCache::CreationThread(size_t thread_index) {
  while (true) {
    PipelineCreationArguments creation_arguments;
    {
      std::unique_lock<std::mutex> lock(creation_request_lock_);
      if (thread_index >= creation_threads_shutdown_from_ || creation_queue_.empty()) {
        if (creation_completion_set_event_ && creation_threads_busy_ == 0) {
          creation_completion_set_event_ = false;
          creation_completion_event_->Set();
        }
        if (thread_index >= creation_threads_shutdown_from_) {
          return;
        }
        creation_request_cond_.wait(lock);
        continue;
      }
      creation_arguments = creation_queue_.top();
      creation_queue_.pop();
      ++creation_threads_busy_;
    }

    bool created = EnsurePipelineCreated(creation_arguments);
    if (!created) {
      bool has_placeholder =
          creation_arguments.pipeline->second.is_placeholder.load(std::memory_order_acquire);
      VkPipeline pipeline =
          creation_arguments.pipeline->second.pipeline.load(std::memory_order_acquire);
      if (has_placeholder && pipeline != VK_NULL_HANDLE) {
        // Keep the placeholder resident and stop waiting for a real pipeline.
        creation_arguments.pipeline->second.is_placeholder.store(false, std::memory_order_release);
      } else {
        creation_arguments.pipeline->second.pipeline.store(VK_NULL_HANDLE,
                                                           std::memory_order_release);
      }
    }

    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      --creation_threads_busy_;
    }
  }
}

void VulkanPipelineCache::CreateQueuedPipelinesOnProcessorThread() {
  assert_false(creation_threads_.empty());
  while (true) {
    PipelineCreationArguments creation_arguments;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      if (creation_queue_.empty()) {
        break;
      }
      creation_arguments = creation_queue_.top();
      creation_queue_.pop();
    }
    bool created = EnsurePipelineCreated(creation_arguments);
    if (!created) {
      bool has_placeholder =
          creation_arguments.pipeline->second.is_placeholder.load(std::memory_order_acquire);
      VkPipeline pipeline =
          creation_arguments.pipeline->second.pipeline.load(std::memory_order_acquire);
      if (has_placeholder && pipeline != VK_NULL_HANDLE) {
        creation_arguments.pipeline->second.is_placeholder.store(false, std::memory_order_release);
      } else {
        creation_arguments.pipeline->second.pipeline.store(VK_NULL_HANDLE,
                                                           std::memory_order_release);
      }
    }
  }
}

void VulkanPipelineCache::ProcessDeferredPipelineDestructions(bool force_all) {
  std::vector<VkPipeline> pipelines_to_destroy;
  uint64_t completed_submission = command_processor_.GetCompletedSubmission();
  {
    std::lock_guard<std::mutex> lock(deferred_destroy_lock_);
    if (deferred_destroy_pipelines_.empty()) {
      return;
    }
    auto it = deferred_destroy_pipelines_.begin();
    while (it != deferred_destroy_pipelines_.end()) {
      if (force_all || it->first <= completed_submission) {
        pipelines_to_destroy.push_back(it->second);
        it = deferred_destroy_pipelines_.erase(it);
      } else if (!force_all) {
        // The queue is sorted by submission index.
        break;
      } else {
        ++it;
      }
    }
  }
  if (pipelines_to_destroy.empty()) {
    return;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (VkPipeline pipeline : pipelines_to_destroy) {
    if (pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
}

void VulkanPipelineCache::StorageWriteThread() {
  ShaderStoredHeader shader_header;
  // Don't leak anything in unused bits.
  std::memset(&shader_header, 0, sizeof(shader_header));

  std::vector<uint32_t> ucode_guest_endian;
  ucode_guest_endian.reserve(0xFFFF);

  bool flush_shaders = false;
  bool flush_pipelines = false;

  while (true) {
    if (flush_shaders) {
      flush_shaders = false;
      assert_not_null(shader_storage_file_);
      fflush(shader_storage_file_);
    }
    if (flush_pipelines) {
      flush_pipelines = false;
      assert_not_null(pipeline_storage_file_);
      fflush(pipeline_storage_file_);
    }

    const Shader* shader = nullptr;
    PipelineStoredDescription pipeline_description;
    bool write_pipeline = false;
    {
      std::unique_lock<std::mutex> lock(storage_write_request_lock_);
      if (storage_write_thread_shutdown_) {
        return;
      }
      if (!storage_write_shader_queue_.empty()) {
        shader = storage_write_shader_queue_.front();
        storage_write_shader_queue_.pop_front();
      } else if (storage_write_flush_shaders_) {
        storage_write_flush_shaders_ = false;
        flush_shaders = true;
      }
      if (!storage_write_pipeline_queue_.empty()) {
        std::memcpy(&pipeline_description, &storage_write_pipeline_queue_.front(),
                    sizeof(pipeline_description));
        storage_write_pipeline_queue_.pop_front();
        write_pipeline = true;
      } else if (storage_write_flush_pipelines_) {
        storage_write_flush_pipelines_ = false;
        flush_pipelines = true;
      }
      if (!shader && !write_pipeline) {
        storage_write_request_cond_.wait(lock);
        continue;
      }
    }

    if (shader) {
      shader_header.ucode_data_hash = shader->ucode_data_hash();
      shader_header.ucode_dword_count = shader->ucode_dword_count();
      shader_header.type = shader->type();
      assert_not_null(shader_storage_file_);
      fwrite(&shader_header, sizeof(shader_header), 1, shader_storage_file_);
      if (shader_header.ucode_dword_count) {
        ucode_guest_endian.resize(shader_header.ucode_dword_count);
        // Need to swap because the hash is calculated for the shader with guest
        // endianness.
        memory::copy_and_swap(ucode_guest_endian.data(), shader->ucode_dwords(),
                              shader_header.ucode_dword_count);
        fwrite(ucode_guest_endian.data(), shader_header.ucode_dword_count * sizeof(uint32_t), 1,
               shader_storage_file_);
      }
    }

    if (write_pipeline) {
      assert_not_null(pipeline_storage_file_);
      fwrite(&pipeline_description, sizeof(pipeline_description), 1, pipeline_storage_file_);
    }
  }
}

}  // namespace rex::graphics::vulkan
