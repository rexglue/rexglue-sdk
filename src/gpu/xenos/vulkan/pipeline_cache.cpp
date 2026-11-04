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

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>
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

#include <format>

#include <rex/assert.h>
#include <rex/platform/perf.h>
#include <rex/logging/log.h>
#include <rex/platform/file.h>
#include <rex/bit.h>
#include <rex/platform/thread.h>
#include <rex/platform/sync.h>
#include <rex/memory.h>
#include "gpu/xenos/draw_state.h"
#include "gpu/xenos/pipeline_priority.h"
#include "gpu/xenos/pipeline/shader/spirv_builder.h"
#include "gpu/xenos/pipeline/shader/spirv_translator.h"
#include <rex/string/text.h>
#include <rex/xenos/register.h>
#include "gpu/xenos/vulkan/command_processor.h"
#include "gpu/xenos/vulkan/pipeline_cache.h"
#include "gpu/xenos/vulkan/shader.h"
#include "gpu/xenos/vulkan/tessellation_glsl.h"
#include <rex/xenos/format.h>
#include <rex/xenos/packet.h>
#include <rex/xenos/state.h>
#include <rex/hash.h>
#include <rex/math.h>
#include <rex/types.h>
#include "gpu/xenos/host/vulkan/objects.h"

namespace rex::gpu::vulkan {

namespace {

constexpr std::string_view kPlaceholderPixelShaderSource = R"(#version 460
layout(location = 0) out vec4 oC0;
void main() {
  // Avoid full-screen black flashes while async pipelines are warming up.
  discard;
}
)";

}  // namespace

VulkanPipelineCache::VulkanPipelineCache(VulkanCommandProcessor& command_processor,
                                         const xenos::RegisterFile& register_file,
                                         VulkanRenderTargetCache& render_target_cache,
                                         VkShaderStageFlags guest_shader_vertex_stages)
    : command_processor_(command_processor),
      register_file_(register_file),
      render_target_cache_(render_target_cache),
      guest_shader_vertex_stages_(guest_shader_vertex_stages) {}

void VulkanPipelineCache::AnalyzeShaderUCode(Shader& shader) {
  shader.AnalyzeUcode(ucode_disasm_buffer_,
                      command_processor_.GetXenosSettings().dump_shaders.Get());
}

VulkanPipelineCache::~VulkanPipelineCache() {
  log::NoThrow(log::gpu, [this] { Shutdown(); });
}

bool VulkanPipelineCache::Initialize() {
  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  shader_translator_ = std::make_unique<SPIRVShaderTranslator>(
      SPIRVShaderTranslator::Features(vulkan_device),
      render_target_cache_.IsMSAA2xAttachmentsSupported(),
      render_target_cache_.IsMSAA2xNoAttachmentsSupported(), edram_fragment_shader_interlock,
      render_target_cache_.GetDrawResolutionScaleX(),
      render_target_cache_.GetDrawResolutionScaleY());

  if (edram_fragment_shader_interlock) {
    std::vector<u8> depth_only_fragment_shader_code =
        shader_translator_->CreateDepthOnlyFragmentShader();
    depth_only_fragment_shader_ = gpu::vulkan::objects::CreateShaderModule(
        vulkan_device, reinterpret_cast<const u32*>(depth_only_fragment_shader_code.data()),
        depth_only_fragment_shader_code.size());
    if (depth_only_fragment_shader_ == VK_NULL_HANDLE) {
      REXGPU_FAIL(false,
                  "VulkanPipelineCache: Failed to create the depth/stencil-only "
                  "fragment shader for the fragment shader interlock render backend "
                  "implementation");
    }
  } else {
    std::vector<u8> depth_float24_truncate_fragment_shader_code =
        shader_translator_->CreateDepthOnlyFragmentShader(
            SPIRVShaderTranslator::Modification::DepthStencilMode::kFloat24Truncating);
    depth_float24_truncate_fragment_shader_ = gpu::vulkan::objects::CreateShaderModule(
        vulkan_device,
        reinterpret_cast<const u32*>(depth_float24_truncate_fragment_shader_code.data()),
        depth_float24_truncate_fragment_shader_code.size());
    if (depth_float24_truncate_fragment_shader_ == VK_NULL_HANDLE) {
      REXGPU_FAIL(false,
                  "VulkanPipelineCache: Failed to create the float24 truncating "
                  "depth-only fragment shader");
    }
    std::vector<u8> depth_float24_round_fragment_shader_code =
        shader_translator_->CreateDepthOnlyFragmentShader(
            SPIRVShaderTranslator::Modification::DepthStencilMode::kFloat24Rounding);
    depth_float24_round_fragment_shader_ = gpu::vulkan::objects::CreateShaderModule(
        vulkan_device,
        reinterpret_cast<const u32*>(depth_float24_round_fragment_shader_code.data()),
        depth_float24_round_fragment_shader_code.size());
    if (depth_float24_round_fragment_shader_ == VK_NULL_HANDLE) {
      REXGPU_FAIL(false,
                  "VulkanPipelineCache: Failed to create the float24 rounding "
                  "depth-only fragment shader");
    }
  }

  std::vector<u32> placeholder_pixel_shader_spirv;
  std::string placeholder_pixel_shader_compile_error;
  if (command_processor_.CompileGLSLToSPIRV(
          VK_SHADER_STAGE_FRAGMENT_BIT, kPlaceholderPixelShaderSource,
          placeholder_pixel_shader_spirv, placeholder_pixel_shader_compile_error)) {
    placeholder_pixel_shader_ = gpu::vulkan::objects::CreateShaderModule(
        vulkan_device, placeholder_pixel_shader_spirv.data(),
        placeholder_pixel_shader_spirv.size() * sizeof(u32));
  } else {
    REXGPU_WARN("VulkanPipelineCache: Failed to compile placeholder pixel shader: {}",
                placeholder_pixel_shader_compile_error);
  }
  if (placeholder_pixel_shader_ == VK_NULL_HANDLE) {
    REXGPU_WARN(
        "VulkanPipelineCache: Failed to create placeholder pixel shader. "
        "Async placeholder hot swap will be unavailable");
  }

  u32 logical_processor_count = rex::platform::GetLogicalProcessorCount();
  if (!logical_processor_count) {
    logical_processor_count = 6;
  }
  creation_threads_busy_ = 0;
  startup_loading_ = false;
  creation_completion_event_ = rex::platform::Event::CreateManualResetEvent(true);
  REX_ASSERT(creation_completion_event_ != nullptr);
  creation_completion_set_event_ = false;
  creation_threads_shutdown_from_ = SIZE_MAX;
  if (command_processor_.GetVulkanSettings().pipeline_creation_threads.Get() != 0) {
    size_t creation_thread_count;
    if (command_processor_.GetVulkanSettings().pipeline_creation_threads.Get() < 0) {
      creation_thread_count = std::max(logical_processor_count * 3 / 4, static_cast<u32>(1));
    } else {
      creation_thread_count = std::min(
          static_cast<u32>(command_processor_.GetVulkanSettings().pipeline_creation_threads.Get()),
          logical_processor_count);
    }
    for (size_t i = 0; i < creation_thread_count; ++i) {
      std::unique_ptr<rex::platform::Thread> creation_thread =
          rex::platform::Thread::Create({}, [this, i]() { CreationThread(i); });
      REX_ASSERT(creation_thread != nullptr);
      creation_thread->SetName("Vulkan Pipelines");
      creation_threads_.push_back(std::move(creation_thread));
    }
  }

  return true;
}

void VulkanPipelineCache::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                  u32 title_id, bool blocking) {
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
          rex::string::PathToUTF8(shader_storage_shareable_root));
      return;
    }
  }

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  std::vector<PipelineStoredDescription> pipeline_stored_descriptions;
  std::set<std::pair<u64, u64>> shader_translations_needed;
  auto pipeline_storage_file_path =
      shader_storage_shareable_root /
      std::format("{:08X}.{}.vk.xpso", title_id, edram_fragment_shader_interlock ? "fsi" : "fbo");
  storage_.pipeline_file_path = pipeline_storage_file_path;
  if (!platform::OpenStorageFile(storage_.pipeline_file, pipeline_storage_file_path)) {
    REXGPU_ERROR(
        "Failed to open the Vulkan pipeline description storage file for "
        "writing, persistent shader storage will be disabled: {}",
        rex::string::PathToUTF8(pipeline_storage_file_path));
    return;
  }
  storage_.pipeline_file_flush_needed = false;
  // 'XEPS'.
  const u32 pipeline_storage_magic = 0x53504558;
  const u32 pipeline_storage_magic_api = edram_fragment_shader_interlock ? 1u : 0u;
  const u32 pipeline_storage_version =
      std::max(PipelineDescription::kVersion, SPIRVShaderTranslator::Modification::kVersion);
  struct {
    u32 magic;
    u32 magic_api;
    be_u32 version;
  } pipeline_storage_file_header;
  if (platform::ReadStorageFile(storage_.pipeline_file, &pipeline_storage_file_header,
                                sizeof(pipeline_storage_file_header)) ==
          sizeof(pipeline_storage_file_header) &&
      pipeline_storage_file_header.magic == pipeline_storage_magic &&
      pipeline_storage_file_header.magic_api == pipeline_storage_magic_api &&
      pipeline_storage_file_header.version == pipeline_storage_version) {
    const u64 pipeline_storage_file_end = platform::GetStorageFileSize(pipeline_storage_file_path);
    size_t pipeline_storage_expected_count = static_cast<size_t>(
        pipeline_storage_file_end >= sizeof(pipeline_storage_file_header)
            ? (pipeline_storage_file_end - sizeof(pipeline_storage_file_header)) /
                  sizeof(PipelineStoredDescription)
            : 0);
    if (pipeline_storage_expected_count) {
      pipeline_stored_descriptions.resize(pipeline_storage_expected_count);
      pipeline_stored_descriptions.resize(
          platform::ReadStorageFile(
              storage_.pipeline_file, pipeline_stored_descriptions.data(),
              pipeline_storage_expected_count * sizeof(PipelineStoredDescription)) /
          sizeof(PipelineStoredDescription));
      size_t pipeline_storage_read_count = pipeline_stored_descriptions.size();
      size_t pipeline_storage_kept_count = 0;
      size_t pipeline_storage_skipped_unsupported_count = 0;
      size_t pipeline_storage_skipped_corrupted_count = 0;
      for (size_t i = 0; i < pipeline_storage_read_count; ++i) {
        const PipelineStoredDescription pipeline_stored_description =
            pipeline_stored_descriptions[i];
        if (XXH3_64bits(&pipeline_stored_description.description,
                        sizeof(pipeline_stored_description.description)) !=
            pipeline_stored_description.description_hash) {
          pipeline_storage_skipped_corrupted_count = pipeline_storage_read_count - i;
          pipeline_storage_read_count = i;
          break;
        }
        if (!IsPipelineSupported(pipeline_stored_description.description)) {
          ++pipeline_storage_skipped_unsupported_count;
          continue;
        }
        pipeline_stored_descriptions[pipeline_storage_kept_count++] = pipeline_stored_description;
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

  auto shader_storage_file_path =
      shader_storage_shareable_root / std::format("{:08X}.xsh", title_id);
  storage_.shader_file_path = shader_storage_file_path;
  if (!platform::OpenStorageFile(storage_.shader_file, shader_storage_file_path)) {
    REXGPU_ERROR(
        "Failed to open the title shader storage file for writing, persistent "
        "shader storage will be disabled: {}",
        rex::string::PathToUTF8(shader_storage_file_path));
    storage_.pipeline_file.close();
    return;
  }
  ++storage_.shader_index;
  storage_.shader_file_flush_needed = false;
  struct {
    u32 magic;
    be_u32 version;
  } shader_storage_file_header;
  // 'XESH'.
  const u32 shader_storage_magic = 0x48534558;
  if (platform::ReadStorageFile(storage_.shader_file, &shader_storage_file_header,
                                sizeof(shader_storage_file_header)) ==
          sizeof(shader_storage_file_header) &&
      shader_storage_file_header.magic == shader_storage_magic &&
      shader_storage_file_header.version == ShaderStoredHeader::kVersion) {
    u64 shader_storage_valid_bytes = sizeof(shader_storage_file_header);
    ShaderStoredHeader shader_header;
    std::vector<u32> ucode_dwords;
    ucode_dwords.reserve(0xFFFF);
    while (true) {
      if (platform::ReadStorageFile(storage_.shader_file, &shader_header, sizeof(shader_header)) !=
          sizeof(shader_header)) {
        break;
      }
      size_t ucode_byte_count = shader_header.ucode_dword_count * sizeof(u32);
      ucode_dwords.resize(shader_header.ucode_dword_count);
      if (shader_header.ucode_dword_count &&
          platform::ReadStorageFile(storage_.shader_file, ucode_dwords.data(), ucode_byte_count) !=
              ucode_byte_count) {
        break;
      }
      u64 ucode_data_hash = XXH3_64bits(ucode_dwords.data(), ucode_byte_count);
      if (shader_header.ucode_data_hash != ucode_data_hash) {
        break;
      }
      shader_storage_valid_bytes += sizeof(shader_header) + ucode_byte_count;
      VulkanShader* shader = LoadShader(shader_header.type, ucode_dwords.data(),
                                        shader_header.ucode_dword_count, ucode_data_hash);
      if (shader->GetUcodeStorageIndex() == storage_.shader_index) {
        continue;
      }
      shader->SetUcodeStorageIndex(storage_.shader_index);
    }
    platform::ResetStorageFile(storage_.shader_file, storage_.shader_file_path,
                               shader_storage_valid_bytes);
  } else {
    platform::ResetStorageFile(storage_.shader_file, storage_.shader_file_path, 0);
    shader_storage_file_header.magic = shader_storage_magic;
    shader_storage_file_header.version = ShaderStoredHeader::kVersion;
    platform::AppendStorageFile(storage_.shader_file, &shader_storage_file_header,
                                sizeof(shader_storage_file_header));
  }

  for (const std::pair<u64, u64>& translation_needed : shader_translations_needed) {
    auto shader_it = shaders_.find(translation_needed.first);
    if (shader_it == shaders_.end()) {
      continue;
    }
    auto* shader = shader_it->second.get();
    if (!shader->IsUcodeAnalyzed()) {
      shader->AnalyzeUcode(ucode_disasm_buffer_,
                           command_processor_.GetXenosSettings().dump_shaders.Get());
    }
    bool translation_is_new = false;
    auto* translation = static_cast<VulkanShader::VulkanTranslation*>(
        shader->GetOrCreateTranslation(translation_needed.second, &translation_is_new));
    if (!translation->IsTranslated() &&
        !TranslateAnalyzedShader(*shader_translator_, *translation)) {
      if (translation_is_new) {
        shader->DestroyTranslation(translation_needed.second);
      }
    }
  }

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
    u32 bound_rts = 0;
    for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (pipeline_description.render_targets[i].color_write_mask) {
        bound_rts |= static_cast<u32>(1) << i;
      }
    }
    u32 shader_writes_color_targets =
        creation_arguments.pixel_shader
            ? creation_arguments.pixel_shader->GetShader().GetWritesColorTargets()
            : 0;
    bool shader_writes_depth = creation_arguments.pixel_shader
                                   ? creation_arguments.pixel_shader->GetShader().IsDepthWritten()
                                   : pipeline_description.depth_write_enable != 0;
    creation_arguments.priority = pipeline_priority::CalculatePipelinePriority(
        bound_rts, shader_writes_color_targets, shader_writes_depth);
    pipeline_creations.push_back(creation_arguments);
  }

  if (!pipeline_creations.empty()) {
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      startup_loading_ = true;
    }
    u32 logical_processor_count = rex::platform::GetLogicalProcessorCount();
    if (!logical_processor_count) {
      logical_processor_count = 6;
    }
    size_t creation_thread_count;
    if (command_processor_.GetVulkanSettings().pipeline_creation_threads.Get() == 0) {
      creation_thread_count = 1;
    } else if (command_processor_.GetVulkanSettings().pipeline_creation_threads.Get() < 0) {
      creation_thread_count = std::max(logical_processor_count * 3 / 4, static_cast<u32>(1));
    } else {
      creation_thread_count = std::min(
          static_cast<u32>(command_processor_.GetVulkanSettings().pipeline_creation_threads.Get()),
          logical_processor_count);
    }
    if (!blocking) {
      // No deferred pipeline handles at draw time, so preload completes even when not blocking.
      creation_thread_count = std::max(creation_thread_count, static_cast<size_t>(1));
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
        if (CreatePipeline(pipeline_creations[creation_index])) {
          created_pipeline_count.fetch_add(1);
        }
      }
    };

    if (creation_thread_count <= 1) {
      creation_worker();
    } else {
      std::vector<std::unique_ptr<rex::platform::Thread>> creation_threads;
      creation_threads.reserve(creation_thread_count);
      for (size_t i = 0; i < creation_thread_count; ++i) {
        std::unique_ptr<rex::platform::Thread> creation_thread =
            rex::platform::Thread::Create({}, creation_worker);
        REX_ASSERT(creation_thread != nullptr);
        creation_thread->SetName("Vulkan Pipelines");
        creation_threads.push_back(std::move(creation_thread));
      }
      for (const auto& creation_thread : creation_threads) {
        rex::platform::Wait(creation_thread.get(), false);
      }
    }

    REXGPU_INFO("Created {} graphics pipelines from Vulkan storage ({} requested)",
                created_pipeline_count.load(), pipeline_creations.size());
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      startup_loading_ = false;
    }
  }

  platform::ResetStorageFile(
      storage_.pipeline_file, storage_.pipeline_file_path,
      static_cast<u64>(sizeof(pipeline_storage_file_header) +
                       sizeof(PipelineStoredDescription) * pipeline_stored_descriptions.size()));
  if (pipeline_stored_descriptions.empty()) {
    platform::ResetStorageFile(storage_.pipeline_file, storage_.pipeline_file_path, 0);
    pipeline_storage_file_header.magic = pipeline_storage_magic;
    pipeline_storage_file_header.magic_api = pipeline_storage_magic_api;
    pipeline_storage_file_header.version = pipeline_storage_version;
    platform::AppendStorageFile(storage_.pipeline_file, &pipeline_storage_file_header,
                                sizeof(pipeline_storage_file_header));
  }

  storage_.cache_root = cache_root;
  storage_.title_id = title_id;

  storage_.write_flush_shaders = false;
  storage_.write_flush_pipelines = false;
  storage_.write_thread_shutdown = false;
  storage_.write_thread = rex::platform::Thread::Create({}, [this]() { StorageWriteThread(); });
  REX_ASSERT(storage_.write_thread != nullptr);
  storage_.write_thread->SetName("Vulkan Storage writer");
}

void VulkanPipelineCache::ShutdownShaderStorage() {
  storage_.Shutdown();
}

void VulkanPipelineCache::EndSubmission() {
  if (storage_.shader_file_flush_needed || storage_.pipeline_file_flush_needed) {
    {
      std::lock_guard<std::mutex> lock(storage_.write_request_lock);
      if (storage_.shader_file_flush_needed) {
        storage_.write_flush_shaders = true;
      }
      if (storage_.pipeline_file_flush_needed) {
        storage_.write_flush_pipelines = true;
      }
    }
    storage_.write_request_cond.notify_one();
    storage_.shader_file_flush_needed = false;
    storage_.pipeline_file_flush_needed = false;
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
      CreateQueuedPipelinesOnProcessorThread();
      bool await_creation_completion_event;
      {
        std::lock_guard<std::mutex> lock(creation_request_lock_);
        // The queue is empty here, so only in flight worker creation is checked.
        await_creation_completion_event = creation_threads_busy_ != 0;
        if (await_creation_completion_event) {
          creation_completion_event_->Reset();
          creation_completion_set_event_ = true;
        }
      }
      if (await_creation_completion_event) {
        creation_request_cond_.notify_one();
        rex::platform::Wait(creation_completion_event_.get(), false);
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
    for (auto& thread : creation_threads_) {
      rex::platform::Wait(thread.get(), false);
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

  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  ProcessDeferredPipelineDestructions(true);

  last_pipeline_ = nullptr;
  for (const auto& pipeline_pair : pipelines_) {
    VkPipeline pipeline = pipeline_pair.second.pipeline.load(std::memory_order_acquire);
    if (pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
  pipelines_.clear();

  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                             depth_only_fragment_shader_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                             depth_float24_truncate_fragment_shader_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                             depth_float24_round_fragment_shader_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
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

  shaders_.clear();
  storage_.shader_index = 0;
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();

  shader_translator_.reset();
}

VulkanShader* VulkanPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                              const u32* host_address, u32 dword_count) {
  return LoadShader(shader_type, host_address, dword_count,
                    XXH3_64bits(host_address, dword_count * sizeof(u32)));
}

VulkanShader* VulkanPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                              const u32* host_address, u32 dword_count,
                                              u64 data_hash) {
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<VulkanShader>(command_processor_.GetVulkanDevice(), shader_type,
                                               data_hash, host_address, dword_count);
  VulkanShader* raw = shader.get();
  shaders_.emplace(data_hash, std::move(shader));
  return raw;
}

SPIRVShaderTranslator::Modification VulkanPipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    u32 interpolator_mask, bool ps_param_gen_used) const {
  REX_ASSERT(shader.GetType() == xenos::ShaderType::kVertex);
  REX_ASSERT(shader.IsUcodeAnalyzed());
  const auto& regs = register_file_;
  const auto& device_properties = command_processor_.GetVulkanDevice()->GetProperties();

  SPIRVShaderTranslator::Modification modification(
      shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<xenos::reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;
  if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
    modification.vertex.tessellation_mode =
        static_cast<u32>(regs.Get<xenos::reg::VGT_HOS_CNTL>().tess_mode);
  } else {
    modification.vertex.tessellation_mode = 0;
  }

  auto pa_cl_clip_cntl = regs.Get<xenos::reg::PA_CL_CLIP_CNTL>();
  u32 user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  if (user_clip_planes) {
    if (device_properties.shaderCullDistance &&
        (pa_cl_clip_cntl.ucp_cull_only_ena || !device_properties.shaderClipDistance)) {
      modification.vertex.user_clip_plane_cull = 1;
    } else if (!device_properties.shaderClipDistance) {
      user_clip_planes = 0;
      modification.vertex.user_clip_plane_cull = 0;
    } else {
      modification.vertex.user_clip_plane_cull = 0;
    }
  } else {
    modification.vertex.user_clip_plane_cull = 0;
  }
  modification.vertex.user_clip_plane_count = rex::BitCount(user_clip_planes);
  modification.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;

  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = static_cast<u32>(ps_param_gen_used);
  } else {
    modification.vertex.output_point_parameters = static_cast<u32>(
        (shader.GetWritesPointSizeEdgeFlagKillVertex() & 0b001) &&
        regs.Get<xenos::reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  }
  modification.vertex.vertex_kill_and = static_cast<u32>(
      device_properties.shaderCullDistance &&
      (shader.GetWritesPointSizeEdgeFlagKillVertex() & 0b100) && !pa_cl_clip_cntl.vtx_kill_or);

  return modification;
}

SPIRVShaderTranslator::Modification VulkanPipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, u32 interpolator_mask, u32 param_gen_pos,
    xenos::reg::RB_DEPTHCONTROL normalized_depth_control) const {
  REX_ASSERT(shader.GetType() == xenos::ShaderType::kPixel);
  REX_ASSERT(shader.IsUcodeAnalyzed());
  const auto& regs = register_file_;

  SPIRVShaderTranslator::Modification modification(
      shader_translator_->GetDefaultPixelShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<xenos::reg::SQ_PROGRAM_CNTL>().ps_num_reg)));

  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                              regs.Get<xenos::reg::RB_SURFACE_INFO>().msaa_samples,
                              regs.Get<xenos::reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                              regs.Get<xenos::reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point = static_cast<u32>(
        regs.Get<xenos::reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  if (render_target_cache_.GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    using DepthStencilMode = SPIRVShaderTranslator::Modification::DepthStencilMode;
    if (render_target_cache_.IsDepthFloat24ConvertedInPixelShader() &&
        normalized_depth_control.z_enable &&
        regs.Get<xenos::reg::RB_DEPTH_INFO>().depth_format ==
            xenos::DepthRenderTargetFormat::kD24FS8) {
      modification.pixel.depth_stencil_mode = render_target_cache_.IsDepthFloat24Rounded()
                                                  ? DepthStencilMode::kFloat24Rounding
                                                  : DepthStencilMode::kFloat24Truncating;
    } else {
      if (shader.IsImplicitEarlyZWriteAllowed() &&
          (!shader.IsColorTargetWritten(0) ||
           !draw_state::IsCoverageAlphaDependent(regs.Get<xenos::reg::RB_COLORCONTROL>()))) {
        modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
      } else {
        modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
      }
    }
  }

  return modification;
}

bool VulkanPipelineCache::TranslateShaders(VulkanShader::VulkanTranslation* vertex_shader,
                                           VulkanShader::VulkanTranslation* pixel_shader) {
  // Edge flags are unsupported for polygon primitives.
  REX_ASSERT(register_file_.Get<xenos::reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                 xenos::VertexShaderExportMode::kPosition2VectorsEdge &&
             register_file_.Get<xenos::reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                 xenos::VertexShaderExportMode::kPosition2VectorsEdgeKill);
  REX_ASSERT(!(register_file_.Get<xenos::reg::SQ_PROGRAM_CNTL>().gen_index_vtx));
  if (!vertex_shader->IsTranslated()) {
    vertex_shader->GetShader().AnalyzeUcode(
        ucode_disasm_buffer_, command_processor_.GetXenosSettings().dump_shaders.Get());
    if (!TranslateAnalyzedShader(*shader_translator_, *vertex_shader)) {
      REXGPU_FAIL(false, "Failed to translate the vertex shader!");
    }
  }
  if (!vertex_shader->IsValid()) {
    return false;
  }
  if (pixel_shader != nullptr) {
    if (!pixel_shader->IsTranslated()) {
      pixel_shader->GetShader().AnalyzeUcode(
          ucode_disasm_buffer_, command_processor_.GetXenosSettings().dump_shaders.Get());
      if (!TranslateAnalyzedShader(*shader_translator_, *pixel_shader)) {
        REXGPU_FAIL(false, "Failed to translate the pixel shader!");
      }
    }
    if (!pixel_shader->IsValid()) {
      return false;
    }
  }
  return true;
}

bool VulkanPipelineCache::ConfigurePipeline(
    VulkanShader::VulkanTranslation* vertex_shader, VulkanShader::VulkanTranslation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    xenos::reg::RB_DEPTHCONTROL normalized_depth_control, u32 normalized_color_mask,
    VulkanRenderTargetCache::RenderPassKey render_pass_key, VkPipeline& pipeline_out,
    const IPipelineLayoutProvider*& pipeline_layout_out, void** pipeline_handle_out) {
#if REXGPU_FINE_GRAINED_DRAW_SCOPES
  REX_PROFILE_SCOPE("gpu");
#endif  // REXGPU_FINE_GRAINED_DRAW_SCOPES

  if (!TranslateShaders(vertex_shader, pixel_shader)) {
    return false;
  }
  if (storage_.shader_file.is_open() &&
      vertex_shader->GetShader().GetUcodeStorageIndex() != storage_.shader_index) {
    vertex_shader->GetShader().SetUcodeStorageIndex(storage_.shader_index);
    REX_ASSERT(storage_.write_thread != nullptr);
    storage_.shader_file_flush_needed = true;
    {
      std::lock_guard<std::mutex> lock(storage_.write_request_lock);
      storage_.write_shader_queue.push_back(&vertex_shader->GetShader());
    }
    storage_.write_request_cond.notify_all();
  }
  if (pixel_shader && storage_.shader_file.is_open() &&
      pixel_shader->GetShader().GetUcodeStorageIndex() != storage_.shader_index) {
    pixel_shader->GetShader().SetUcodeStorageIndex(storage_.shader_index);
    REX_ASSERT(storage_.write_thread != nullptr);
    storage_.shader_file_flush_needed = true;
    {
      std::lock_guard<std::mutex> lock(storage_.write_request_lock);
      storage_.write_shader_queue.push_back(&pixel_shader->GetShader());
    }
    storage_.write_request_cond.notify_all();
  }

  PipelineDescription description;
  if (!GetCurrentStateDescription(vertex_shader, pixel_shader, primitive_processing_result,
                                  normalized_depth_control, normalized_color_mask, render_pass_key,
                                  description)) {
    REXGPU_FAIL(false,
                "VulkanPipelineCache: GetCurrentStateDescription failed "
                "(guest_prim={}, host_prim={}, host_vs_type={}, tess_mode={}, host_reset={}, "
                "render_pass_key=0x{:08X})",
                static_cast<u32>(primitive_processing_result.guest_primitive_type),
                static_cast<u32>(primitive_processing_result.host_primitive_type),
                static_cast<u32>(primitive_processing_result.host_vertex_shader_type),
                static_cast<u32>(primitive_processing_result.tessellation_mode),
                static_cast<u32>(primitive_processing_result.host_primitive_reset_enabled),
                render_pass_key.key);
  }
  if (last_pipeline_ && last_pipeline_->first == description) {
    VkPipeline last_pipeline = last_pipeline_->second.pipeline.load(std::memory_order_acquire);
    const IPipelineLayoutProvider* last_pipeline_layout =
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

  bool use_async = command_processor_.GetXenosSettings().async_shader_compilation.Get() &&
                   !creation_threads_.empty() && pixel_shader &&
                   placeholder_pixel_shader_ != VK_NULL_HANDLE;
  u8 async_priority = pipeline_priority::kPriorityLowest;
  if (use_async) {
    u32 bound_rts = pipeline_priority::GetBoundRTMaskFromNormalizedColorMask(normalized_color_mask);
    u32 shader_writes_color_targets =
        pixel_shader ? pixel_shader->GetShader().GetWritesColorTargets() : 0;
    bool shader_writes_depth = pixel_shader ? pixel_shader->GetShader().IsDepthWritten()
                                            : normalized_depth_control.z_write_enable != 0;
    async_priority = pipeline_priority::CalculatePipelinePriority(
        bound_rts, shader_writes_color_targets, shader_writes_depth);
  }

  auto it = pipelines_.find(description);
  if (it != pipelines_.end()) {
    VkPipeline found_pipeline = it->second.pipeline.load(std::memory_order_acquire);
    if (found_pipeline == VK_NULL_HANDLE) {
      PipelineCreationArguments creation_arguments;
      if (!TryGetPipelineCreationArgumentsForDescription(description, &*it, creation_arguments) ||
          !CreatePipeline(creation_arguments)) {
        return false;
      }
      found_pipeline = it->second.pipeline.load(std::memory_order_acquire);
    }
    const IPipelineLayoutProvider* found_pipeline_layout =
        it->second.pipeline_layout.load(std::memory_order_acquire);
    last_pipeline_ = &*it;
    pipeline_out = found_pipeline;
    pipeline_layout_out = found_pipeline_layout;
    if (pipeline_handle_out) {
      *pipeline_handle_out = &it->second;
    }
    return pipeline_out != VK_NULL_HANDLE && pipeline_layout_out != nullptr;
  }

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
      if (CreatePipeline(creation_arguments_placeholder, placeholder_pixel_shader_)) {
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

  if (!queued_async_creation && !CreatePipeline(creation_arguments_real)) {
    pipelines_.erase(description);
    return false;
  }
  if (!queued_async_creation) {
    pipeline.second.is_placeholder.store(false, std::memory_order_release);
  }

  if (storage_.pipeline_file.is_open()) {
    REX_ASSERT(storage_.write_thread != nullptr);
    storage_.pipeline_file_flush_needed = true;
    {
      std::lock_guard<std::mutex> lock(storage_.write_request_lock);
      storage_.write_pipeline_queue.emplace_back();
      PipelineStoredDescription& stored_description = storage_.write_pipeline_queue.back();
      stored_description.description_hash = description.GetHash();
      stored_description.description = description;
    }
    storage_.write_request_cond.notify_all();
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
    void* handle, VkPipeline& pipeline_out, const IPipelineLayoutProvider*& pipeline_layout_out,
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

bool VulkanPipelineCache::TranslateAnalyzedShader(SPIRVShaderTranslator& translator,
                                                  VulkanShader::VulkanTranslation& translation) {
  VulkanShader& shader = static_cast<VulkanShader&>(translation.GetShader());

  if (!translator.TranslateAnalyzedShader(translation)) {
    REXGPU_FAIL(false, "Shader {:016X} translation failed, marking as ignored",
                shader.GetUcodeDataHash());
  }
  if (const std::string dump_path = command_processor_.GetXenosSettings().dump_shaders.Get();
      !dump_path.empty()) {
    translation.DumpTranslation(dump_path, "vulkan");
  }
  if (translation.GetOrCreateShaderModule() == VK_NULL_HANDLE) {
    return false;
  }

  if (shader.EnterBindingLayoutUserUIDSetup()) {
    size_t texture_binding_layout_uid = kLayoutUIDEmpty;
    const std::vector<VulkanShader::TranslatedTextureBinding>& texture_bindings =
        shader.GetTextureBindingsAfterTranslation();
    size_t texture_binding_count = texture_bindings.size();
    if (texture_binding_count) {
      size_t texture_binding_layout_bytes =
          texture_binding_count * sizeof(*texture_bindings.data());
      u64 texture_binding_layout_hash =
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

    // Only the sampler count decides layout compatibility, fetch parameters are overrides.
    static_assert(kLayoutUIDEmpty == 0,
                  "Empty layout UID is assumed to be 0 because for bindful samplers, the "
                  "UID is their count");
    shader.SetSamplerBindingLayoutUserUID(shader.GetSamplerBindingsAfterTranslation().size());
  }

  return true;
}

void VulkanPipelineCache::WritePipelineRenderTargetDescription(
    xenos::reg::RB_BLENDCONTROL blend_control, u32 write_mask,
    PipelineRenderTarget& render_target_out) const {
  if (write_mask) {
    REX_ASSERT((write_mask & ~static_cast<u32>(0b1111)) == 0);
    // 32 entries cover the 0x1F mask, unknown values map to zero.
    static const PipelineBlendFactor kBlendFactorMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,
        /*  3 */ PipelineBlendFactor::kZero,
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
        kBlendFactorMap[static_cast<u32>(blend_control.color_srcblend)];
    render_target_out.dst_color_blend_factor =
        kBlendFactorMap[static_cast<u32>(blend_control.color_destblend)];
    render_target_out.color_blend_op = blend_control.color_comb_fcn;
    render_target_out.src_alpha_blend_factor =
        kBlendFactorMap[static_cast<u32>(blend_control.alpha_srcblend)];
    render_target_out.dst_alpha_blend_factor =
        kBlendFactorMap[static_cast<u32>(blend_control.alpha_destblend)];
    render_target_out.alpha_blend_op = blend_control.alpha_comb_fcn;
    if (!command_processor_.GetVulkanDevice()->GetProperties().constantAlphaColorBlendFactors) {
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
    xenos::reg::RB_DEPTHCONTROL normalized_depth_control, u32 normalized_color_mask,
    VulkanRenderTargetCache::RenderPassKey render_pass_key,
    PipelineDescription& description_out) const {
  description_out.Reset();

  const gpu::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->GetProperties();

  const xenos::RegisterFile& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<xenos::reg::PA_SU_SC_MODE_CNTL>();

  description_out.vertex_shader_hash = vertex_shader->GetShader().GetUcodeDataHash();
  description_out.vertex_shader_modification = vertex_shader->GetModification();
  SPIRVShaderTranslator::Modification vertex_shader_modification(
      description_out.vertex_shader_modification);
  Shader::HostVertexShaderType host_vertex_shader_type =
      vertex_shader_modification.vertex.host_vertex_shader_type;
  if (pixel_shader) {
    description_out.pixel_shader_hash = pixel_shader->GetShader().GetUcodeDataHash();
    description_out.pixel_shader_modification = pixel_shader->GetModification();
    SPIRVShaderTranslator::Modification pixel_shader_modification(
        description_out.pixel_shader_modification);
    using DepthStencilMode = SPIRVShaderTranslator::Modification::DepthStencilMode;
    description_out.sample_rate_shading =
        !pixel_shader->GetShader().IsDepthWritten() &&
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
    if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
        host_vertex_shader_type == Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
      if (primitive_processing_result.host_primitive_type != xenos::PrimitiveType::kTriangleStrip ||
          !primitive_processing_result.host_primitive_reset_enabled) {
        REX_ASSERT_ALWAYS();
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
          // Primitive processing converts triangle fans to triangle lists.
          REX_ASSERT_ALWAYS();
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
      device_properties.depthClamp && regs.Get<xenos::reg::PA_CL_CLIP_CNTL>().clip_disable;

  // Tessellated patch draws are always polygonal, even when the major mode is not explicit.
  bool primitive_polygonal = tessellated ? true : draw_state::IsPrimitivePolygonal(regs);
  bool rasterization_enabled = draw_state::IsRasterizationDone(
      regs, primitive_polygonal, pixel_shader && pixel_shader->GetShader().GetMemExportEMWritten());
  if (!rasterization_enabled) {
    description_out.rasterizer_discard = 1;
    return true;
  }
  if (primitive_polygonal) {
    // A polygon mode serves both faces, the lowest type (points lowest, triangles highest) wins.
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
          description_out.polygon_mode = PipelinePolygonMode::kLine;
          break;
        case xenos::PolygonType::kTriangles:
          description_out.polygon_mode = PipelinePolygonMode::kFill;
          break;
        default:
          REX_UNHANDLED_CASE(polygon_type);
          return false;
      }
    } else {
      description_out.polygon_mode = PipelinePolygonMode::kFill;
    }
    description_out.front_face_clockwise = pa_su_sc_mode_cntl.face != 0;
  } else {
    description_out.polygon_mode = PipelinePolygonMode::kFill;
  }
  if (tessellated && command_processor_.GetVulkanSettings().tessellation_wireframe.Get() &&
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

    u32 render_pass_color_rts = render_pass_key.depth_and_color_used >> 1;
    REX_ASSERT(device_properties.independentBlend);
    u32 render_pass_color_rts_remaining = render_pass_color_rts;
    u32 color_rt_index;
    while (rex::bit_scan_forward(render_pass_color_rts_remaining, &color_rt_index)) {
      render_pass_color_rts_remaining &= ~(static_cast<u32>(1) << color_rt_index);
      WritePipelineRenderTargetDescription(
          regs.Get<xenos::reg::RB_BLENDCONTROL>(
              xenos::reg::RB_BLENDCONTROL::kRTRegisterIndices[color_rt_index]),
          (normalized_color_mask >> (color_rt_index * 4)) & 0b1111,
          description_out.render_targets[color_rt_index]);
    }
  }

  return true;
}

bool VulkanPipelineCache::IsPipelineSupported(const PipelineDescription& description) const {
  VkShaderStageFlags vertex_shader_stage =
      Shader::IsHostVertexShaderTypeDomain(
          SPIRVShaderTranslator::Modification(description.vertex_shader_modification)
              .vertex.host_vertex_shader_type)
          ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
          : VK_SHADER_STAGE_VERTEX_BIT;
  if (!(guest_shader_vertex_stages_ & vertex_shader_stage)) {
    return false;
  }

  const gpu::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->GetProperties();

  if (!device_properties.geometryShader &&
      description.geometry_shader != PipelineGeometryShader::kNone) {
    return false;
  }

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

  REX_ASSERT(device_properties.independentBlend);

  if (!device_properties.constantAlphaColorBlendFactors) {
    u32 color_rts_remaining = description.render_pass_key.depth_and_color_used >> 1;
    u32 color_rt_index;
    while (rex::bit_scan_forward(color_rts_remaining, &color_rt_index)) {
      color_rts_remaining &= ~(static_cast<u32>(1) << color_rt_index);
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

u32 VulkanPipelineCache::GetTessellationPatchControlPointCount(
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

  std::vector<u32> shader_spirv;
  std::string compile_error;
  std::string source = adaptive ? GetTessellationAdaptiveVertexShaderGLSL()
                                : GetTessellationIndexedVertexShaderGLSL();
  if (!command_processor_.CompileGLSLToSPIRV(VK_SHADER_STAGE_VERTEX_BIT, source, shader_spirv,
                                             compile_error)) {
    REXGPU_ERROR(
        "VulkanPipelineCache: Failed to compile tessellation {} vertex "
        "shader: {}",
        adaptive ? "adaptive" : "indexed", compile_error);
    shader_module = VK_NULL_HANDLE;
    return shader_module;
  }

  shader_module = gpu::vulkan::objects::CreateShaderModule(
      command_processor_.GetVulkanDevice(), shader_spirv.data(), sizeof(u32) * shader_spirv.size());
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

  u32 input_control_points =
      GetTessellationPatchControlPointCount(host_vertex_shader_type, tessellation_mode);
  u32 output_control_points = 0;
  if (tessellation_mode == xenos::TessellationMode::kAdaptive ||
      IsPatchIndexedHostVertexShaderType(host_vertex_shader_type)) {
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

  std::vector<u32> shader_spirv;
  std::string compile_error;
  std::string source = GetTessellationControlShaderGLSL(
      host_vertex_shader_type, tessellation_mode, input_control_points, output_control_points);
  if (!command_processor_.CompileGLSLToSPIRV(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, source,
                                             shader_spirv, compile_error)) {
    REXGPU_ERROR(
        "VulkanPipelineCache: Failed to compile tessellation control shader "
        "0x{:08X}: {}",
        key.key, compile_error);
    tessellation_control_shaders_.emplace(key, VK_NULL_HANDLE);
    return VK_NULL_HANDLE;
  }

  VkShaderModule shader_module = gpu::vulkan::objects::CreateShaderModule(
      command_processor_.GetVulkanDevice(), shader_spirv.data(), sizeof(u32) * shader_spirv.size());
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
    SPIRVShaderTranslator::Modification vertex_shader_modification,
    SPIRVShaderTranslator::Modification pixel_shader_modification, GeometryShaderKey& key_out) {
  if (geometry_shader_type == PipelineGeometryShader::kNone) {
    return false;
  }
  // AsTriangleStrip host vertex shaders are the fallback for hosts without geometry shaders.
  if (vertex_shader_modification.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      vertex_shader_modification.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
    REX_ASSERT_ALWAYS();
    return false;
  }
  GeometryShaderKey key;
  key.type = geometry_shader_type;
  key.interpolator_count = rex::BitCount(vertex_shader_modification.vertex.interpolator_mask);
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

  spv::ExecutionMode input_primitive_execution_mode = static_cast<spv::ExecutionMode>(0);
  u32 input_primitive_vertex_count = 0;
  spv::ExecutionMode output_primitive_execution_mode = static_cast<spv::ExecutionMode>(0);
  u32 output_max_vertices = 0;
  switch (key.type) {
    case PipelineGeometryShader::kPointList:
      input_primitive_execution_mode = spv::ExecutionModeInputPoints;
      input_primitive_vertex_count = 1;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case PipelineGeometryShader::kRectangleList:
      input_primitive_execution_mode = spv::ExecutionModeTriangles;
      input_primitive_vertex_count = 3;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case PipelineGeometryShader::kQuadList:
      input_primitive_execution_mode = spv::ExecutionModeInputLinesAdjacency;
      input_primitive_vertex_count = 4;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    default:
      REX_UNHANDLED_CASE(key.type);
  }

  u32 clip_distance_count = key.user_clip_plane_cull ? 0 : key.user_clip_plane_count;
  u32 point_user_cull_distance_count =
      key.type == PipelineGeometryShader::kPointList && key.user_clip_plane_cull
          ? key.user_clip_plane_count
          : 0;
  u32 cull_distance_count = point_user_cull_distance_count + key.has_vertex_kill_and;
  bool point_recalculate_clip_distances = key.type == PipelineGeometryShader::kPointList &&
                                          clip_distance_count && key.point_ps_ucp_mode >= 2;
  bool point_recalculate_cull_distances = key.type == PipelineGeometryShader::kPointList &&
                                          point_user_cull_distance_count &&
                                          key.point_ps_ucp_mode >= 3;

  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->GetProperties();
  const gpu::vulkan::VulkanDevice::Extensions& device_extensions = vulkan_device->GetExtensions();
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
    builder_logger_ptr = &builder_logger;
  }
  SPIRVBuilder builder(spirv_version, (SPIRVShaderTranslator::kSPIRVMagicToolID << 16) | 1,
                       builder_logger_ptr);
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
      builder.makeArrayType(type_float4, builder.makeUintConstant(6), sizeof(f32) * 4);
  builder.addDecoration(type_float4_array_6, spv::DecorationArrayStride, sizeof(f32) * 4);
  spv::Id type_clip_distances =
      clip_distance_count
          ? builder.makeArrayType(type_float, builder.makeUintConstant(clip_distance_count), 0)
          : spv::NoType;
  spv::Id type_cull_distances =
      cull_distance_count
          ? builder.makeArrayType(type_float, builder.makeUintConstant(cull_distance_count), 0)
          : spv::NoType;

  enum PointConstant : u8 {
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
    builder.addMemberDecoration(type_system_constants, kPointConstantConstantDiameter,
                                spv::DecorationOffset,
                                static_cast<int>(offsetof(SPIRVShaderTranslator::SystemConstants,
                                                          point_constant_diameter)));
    builder.addMemberName(type_system_constants, kPointConstantScreenDiameterToNdcRadius,
                          "point_screen_diameter_to_ndc_radius");
    builder.addMemberDecoration(type_system_constants, kPointConstantScreenDiameterToNdcRadius,
                                spv::DecorationOffset,
                                static_cast<int>(offsetof(SPIRVShaderTranslator::SystemConstants,
                                                          point_screen_diameter_to_ndc_radius)));
    builder.addMemberName(type_system_constants, kPointConstantNdcScale, "ndc_scale");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantNdcScale, spv::DecorationOffset,
        static_cast<int>(offsetof(SPIRVShaderTranslator::SystemConstants, ndc_scale)));
    builder.addMemberName(type_system_constants, kPointConstantNdcOffset, "ndc_offset");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantNdcOffset, spv::DecorationOffset,
        static_cast<int>(offsetof(SPIRVShaderTranslator::SystemConstants, ndc_offset)));
    builder.addMemberName(type_system_constants, kPointConstantUserClipPlanes, "user_clip_planes");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantUserClipPlanes, spv::DecorationOffset,
        static_cast<int>(offsetof(SPIRVShaderTranslator::SystemConstants, user_clip_planes)));
  }
  spv::Id uniform_system_constants = spv::NoResult;
  if (type_system_constants != spv::NoType) {
    builder.addDecoration(type_system_constants, spv::DecorationBlock);
    uniform_system_constants =
        builder.createVariable(spv::NoPrecision, spv::StorageClassUniform, type_system_constants,
                               "xe_uniform_system_constants");
    builder.addDecoration(uniform_system_constants, spv::DecorationDescriptorSet,
                          static_cast<int>(SPIRVShaderTranslator::kDescriptorSetConstants));
    builder.addDecoration(uniform_system_constants, spv::DecorationBinding,
                          static_cast<int>(SPIRVShaderTranslator::kConstantBufferSystem));
    main_interface.push_back(uniform_system_constants);
  }

  spv::Id const_input_primitive_vertex_count =
      builder.makeUintConstant(input_primitive_vertex_count);

  id_vector_temp.clear();
  i32 member_in_gl_per_vertex_position = static_cast<i32>(id_vector_temp.size());
  id_vector_temp.push_back(type_float4);
  spv::Id const_member_in_gl_per_vertex_position =
      builder.makeIntConstant(member_in_gl_per_vertex_position);
  i32 member_in_gl_per_vertex_clip_distance = -1;
  spv::Id const_member_in_gl_per_vertex_clip_distance = spv::NoResult;
  if (clip_distance_count) {
    member_in_gl_per_vertex_clip_distance = static_cast<i32>(id_vector_temp.size());
    id_vector_temp.push_back(type_clip_distances);
    const_member_in_gl_per_vertex_clip_distance =
        builder.makeIntConstant(member_in_gl_per_vertex_clip_distance);
  }
  i32 member_in_gl_per_vertex_cull_distance = -1;
  if (cull_distance_count) {
    member_in_gl_per_vertex_cull_distance = static_cast<i32>(id_vector_temp.size());
    id_vector_temp.push_back(type_cull_distances);
  }
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

  u32 output_location = 0;

  std::array<spv::Id, xenos::kMaxInterpolators> out_interpolators;
  for (u32 i = 0; i < key.interpolator_count; ++i) {
    spv::Id out_interpolator =
        builder.createVariable(spv::NoPrecision, spv::StorageClassOutput, type_float4,
                               std::format("xe_out_interpolator_{}", i).c_str());
    out_interpolators[i] = out_interpolator;
    builder.addDecoration(out_interpolator, spv::DecorationLocation,
                          static_cast<int>(output_location));
    builder.addDecoration(out_interpolator, spv::DecorationInvariant);
    main_interface.push_back(out_interpolator);
    ++output_location;
  }

  spv::Id out_point_coordinates = spv::NoResult;
  if (key.has_point_coordinates) {
    out_point_coordinates = builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                   type_float2, "xe_out_point_coordinates");
    builder.addDecoration(out_point_coordinates, spv::DecorationLocation,
                          static_cast<int>(output_location));
    builder.addDecoration(out_point_coordinates, spv::DecorationInvariant);
    main_interface.push_back(out_point_coordinates);
    ++output_location;
  }

  u32 input_location = 0;

  std::array<spv::Id, xenos::kMaxInterpolators> in_interpolators;
  for (u32 i = 0; i < key.interpolator_count; ++i) {
    spv::Id in_interpolator = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(type_float4, const_input_primitive_vertex_count, 0),
        std::format("xe_in_interpolator_{}", i).c_str());
    in_interpolators[i] = in_interpolator;
    builder.addDecoration(in_interpolator, spv::DecorationLocation,
                          static_cast<int>(input_location));
    main_interface.push_back(in_interpolator);
    ++input_location;
  }

  spv::Id in_point_size = spv::NoResult;
  if (key.has_point_size) {
    in_point_size = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(type_float, const_input_primitive_vertex_count, 0),
        "xe_in_point_size");
    builder.addDecoration(in_point_size, spv::DecorationLocation, static_cast<int>(input_location));
    main_interface.push_back(in_point_size);
    ++input_location;
  }

  id_vector_temp.clear();
  i32 member_out_gl_per_vertex_position = static_cast<i32>(id_vector_temp.size());
  id_vector_temp.push_back(type_float4);
  spv::Id const_member_out_gl_per_vertex_position =
      builder.makeIntConstant(member_out_gl_per_vertex_position);
  i32 member_out_gl_per_vertex_clip_distance = -1;
  spv::Id const_member_out_gl_per_vertex_clip_distance = spv::NoResult;
  if (clip_distance_count) {
    member_out_gl_per_vertex_clip_distance = static_cast<i32>(id_vector_temp.size());
    id_vector_temp.push_back(type_clip_distances);
    const_member_out_gl_per_vertex_clip_distance =
        builder.makeIntConstant(member_out_gl_per_vertex_clip_distance);
  }
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
                           static_cast<int>(output_max_vertices));
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

  // After every OpEmitVertex all output variables are undefined.

  // Discard the primitive when any vertex position is NaN, which emulates vertex kill with OR.
  for (u32 i = 0; i < input_primitive_vertex_count; ++i) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeIntConstant(static_cast<i32>(i)));
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

  // Point lists with ps_ucp_mode 3 compute user cull plane distances per expanded vertex later.
  if (cull_distance_count) {
    spv::Id const_member_in_gl_per_vertex_cull_distance =
        builder.makeIntConstant(member_in_gl_per_vertex_cull_distance);
    spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
    spv::Id cull_condition = spv::NoResult;
    u32 cull_distance_start = point_recalculate_cull_distances ? point_user_cull_distance_count : 0;
    if (cull_distance_start < cull_distance_count) {
      for (u32 i = cull_distance_start; i < cull_distance_count; ++i) {
        for (u32 j = 0; j < input_primitive_vertex_count; ++j) {
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeIntConstant(static_cast<i32>(j)));
          id_vector_temp.push_back(const_member_in_gl_per_vertex_cull_distance);
          id_vector_temp.push_back(builder.makeIntConstant(static_cast<i32>(i)));
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
      REX_ASSERT(cull_condition != spv::NoResult);
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
      // Expand the point sprite with left to right, top to bottom UVs.

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_float_0 = builder.makeFloatConstant(0.0f);

      id_vector_temp.clear();
      id_vector_temp.push_back(
          builder.makeIntConstant(static_cast<i32>(kPointConstantConstantDiameter)));
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
        // Nonnegative means the translated vertex shader wrote it, the header default is -1.0.
        id_vector_temp.clear();
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

      // Zero size points are discarded early, point lists may run as compute with memexport.
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

      // Convert the diameter in guest screen coordinates to NDC radius, then to clip space by W.
      id_vector_temp.clear();
      id_vector_temp.push_back(
          builder.makeIntConstant(static_cast<i32>(kPointConstantScreenDiameterToNdcRadius)));
      id_vector_temp.push_back(const_int_0);
      spv::Id point_radius_x = builder.CreateNoContractionBinOp(
          spv::OpFMul, type_float, point_guest_diameter_x,
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision));
      id_vector_temp.back() = const_int_1;
      spv::Id point_radius_y = builder.CreateNoContractionBinOp(
          spv::OpFMul, type_float, point_guest_diameter_y,
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision));
      id_vector_temp.clear();
      id_vector_temp.push_back(const_int_0);
      id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
      spv::Id point_position = builder.createLoad(
          builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
          spv::NoPrecision);
      spv::Id point_w = builder.createCompositeExtract(point_position, type_float, 3);
      point_radius_x =
          builder.CreateNoContractionBinOp(spv::OpFMul, type_float, point_radius_x, point_w);
      point_radius_y =
          builder.CreateNoContractionBinOp(spv::OpFMul, type_float, point_radius_y, point_w);

      std::array<spv::Id, xenos::kMaxInterpolators> point_interpolators;
      id_vector_temp.clear();
      id_vector_temp.push_back(const_int_0);
      for (u32 i = 0; i < key.interpolator_count; ++i) {
        point_interpolators[i] = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_interpolators[i], id_vector_temp),
            spv::NoPrecision);
      }
      spv::Id point_x = builder.createCompositeExtract(point_position, type_float, 0);
      spv::Id point_y = builder.createCompositeExtract(point_position, type_float, 1);
      std::array<spv::Id, 2> point_edge_x, point_edge_y;
      for (u32 i = 0; i < 2; ++i) {
        spv::Op point_radius_add_op = i ? spv::OpFAdd : spv::OpFSub;
        point_edge_x[i] = builder.CreateNoContractionBinOp(point_radius_add_op, type_float, point_x,
                                                           point_radius_x);
        point_edge_y[i] = builder.CreateNoContractionBinOp(point_radius_add_op, type_float, point_y,
                                                           point_radius_y);
      };
      spv::Id point_z = builder.createCompositeExtract(point_position, type_float, 2);
      spv::Id point_clip_distances = spv::NoResult;
      std::vector<spv::Id> point_user_clip_planes;
      point_user_clip_planes.reserve(clip_distance_count);
      spv::Id point_ndc_scale = spv::NoResult;
      spv::Id point_ndc_offset = spv::NoResult;
      if (point_recalculate_clip_distances || point_recalculate_cull_distances) {
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.makeIntConstant(static_cast<i32>(kPointConstantNdcScale)));
        point_ndc_scale =
            builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                         uniform_system_constants, id_vector_temp),
                               spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(
            builder.makeIntConstant(static_cast<i32>(kPointConstantNdcOffset)));
        point_ndc_offset =
            builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                         uniform_system_constants, id_vector_temp),
                               spv::NoPrecision);
        u32 point_user_clip_plane_count =
            point_recalculate_cull_distances ? point_user_cull_distance_count : clip_distance_count;
        for (u32 i = 0; i < point_user_clip_plane_count; ++i) {
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.makeIntConstant(static_cast<i32>(kPointConstantUserClipPlanes)));
          id_vector_temp.push_back(builder.makeIntConstant(static_cast<i32>(i)));
          point_user_clip_planes.push_back(builder.createLoad(
              builder.createAccessChain(spv::StorageClassUniform, uniform_system_constants,
                                        id_vector_temp),
              spv::NoPrecision));
        }
      } else if (clip_distance_count) {
        id_vector_temp.clear();
        id_vector_temp.push_back(const_int_0);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        point_clip_distances = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
      }

      if (point_recalculate_cull_distances) {
        std::array<spv::Id, 4> point_guest_positions;
        for (u32 i = 0; i < 4; ++i) {
          u32 point_vertex_x = i >> 1;
          u32 point_vertex_y = i & 1;
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
          spv::Id point_guest_position_xyz = builder.CreateNoContractionBinOp(
              spv::OpFSub, type_float3, point_vertex_position_xyz,
              builder.CreateNoContractionBinOp(spv::OpVectorTimesScalar, type_float3,
                                               point_ndc_offset, point_w));
          point_guest_position_xyz = builder.CreateNoContractionBinOp(
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
        for (u32 i = 0; i < point_user_cull_distance_count; ++i) {
          spv::Id point_plane_cull_condition = spv::NoResult;
          for (u32 j = 0; j < 4; ++j) {
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
        REX_ASSERT(point_cull_condition != spv::NoResult);
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

      for (u32 i = 0; i < 4; ++i) {
        for (u32 j = 0; j < key.interpolator_count; ++j) {
          builder.createStore(point_interpolators[j], out_interpolators[j]);
        }
        // Top left, bottom left, top right, bottom right order, face winding is ignored for points.
        u32 point_vertex_x = i >> 1;
        u32 point_vertex_y = i & 1;
        if (key.has_point_coordinates) {
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeFloatConstant(static_cast<f32>(point_vertex_x)));
          id_vector_temp.push_back(builder.makeFloatConstant(static_cast<f32>(point_vertex_y)));
          builder.createStore(builder.makeCompositeConstant(type_float2, id_vector_temp),
                              out_point_coordinates);
        }
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
        if (point_recalculate_clip_distances) {
          // Convert host clip space back to guest clip space before applying user clip planes.
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 2));
          spv::Id point_vertex_position_xyz =
              builder.createCompositeConstruct(type_float3, id_vector_temp);
          spv::Id point_guest_position_xyz = builder.CreateNoContractionBinOp(
              spv::OpFSub, type_float3, point_vertex_position_xyz,
              builder.CreateNoContractionBinOp(spv::OpVectorTimesScalar, type_float3,
                                               point_ndc_offset, point_w));
          point_guest_position_xyz = builder.CreateNoContractionBinOp(
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
          for (u32 j = 0; j < clip_distance_count; ++j) {
            id_vector_temp.clear();
            id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
            id_vector_temp.push_back(builder.makeIntConstant(static_cast<i32>(j)));
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
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case PipelineGeometryShader::kRectangleList: {
      // The longest edge picks the strip order 0123, 1203 or 2013 with v3 = -v0 + v1 + v2.

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_int_2 = builder.makeIntConstant(2);
      spv::Id const_int_3 = builder.makeIntConstant(3);

      // Squared edge lengths, [0] is 12, [1] is 20, [2] is 01.
      spv::Id edge_lengths[3];
      id_vector_temp.resize(3);
      id_vector_temp[1] = const_member_in_gl_per_vertex_position;
      for (u32 i = 0; i < 3; ++i) {
        id_vector_temp[0] = builder.makeIntConstant(static_cast<i32>((1 + i) % 3));
        id_vector_temp[2] = const_int_0;
        spv::Id edge_0_x = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[2] = const_int_1;
        spv::Id edge_0_y = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = builder.makeIntConstant(static_cast<i32>((2 + i) % 3));
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

      spv::Id vertex_indices[3];
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
      for (u32 i = 1; i < 3; ++i) {
        spv::Id vertex_index_without_wrapping = builder.createBinOp(
            spv::OpIAdd, type_int, vertex_indices[0], builder.makeIntConstant(static_cast<i32>(i)));
        vertex_indices[i] = builder.createTriOp(
            spv::OpSelect, type_int,
            builder.createBinOp(spv::OpSLessThan, type_bool, vertex_index_without_wrapping,
                                const_int_3),
            vertex_index_without_wrapping,
            builder.createBinOp(spv::OpISub, type_int, vertex_index_without_wrapping, const_int_3));
      }

      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (key.has_point_coordinates) {
        spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero = builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      for (u32 i = 0; i < 3; ++i) {
        spv::Id vertex_index = vertex_indices[i];
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_index);
        for (u32 j = 0; j < key.interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(spv::StorageClassInput,
                                                           in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        if (key.has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero, out_point_coordinates);
        }
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
        builder.createNoResultOp(spv::OpEmitVertex);
      }

      for (u32 i = 0; i < key.interpolator_count; ++i) {
        spv::Id in_interpolator = in_interpolators[i];
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_indices[0]);
        spv::Id vertex_interpolator_v0 = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_interpolator, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = vertex_indices[1];
        spv::Id vertex_interpolator_v01 = builder.CreateNoContractionBinOp(
            spv::OpFSub, type_float4,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_interpolator, id_vector_temp),
                spv::NoPrecision),
            vertex_interpolator_v0);
        id_vector_temp[0] = vertex_indices[2];
        spv::Id vertex_interpolator_v3 = builder.CreateNoContractionBinOp(
            spv::OpFAdd, type_float4, vertex_interpolator_v01,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_interpolator, id_vector_temp),
                spv::NoPrecision));
        builder.createStore(vertex_interpolator_v3, out_interpolators[i]);
      }
      if (key.has_point_coordinates) {
        builder.createStore(const_point_coordinates_zero, out_point_coordinates);
      }
      id_vector_temp.clear();
      id_vector_temp.push_back(vertex_indices[0]);
      id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
      spv::Id vertex_position_v0 = builder.createLoad(
          builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
          spv::NoPrecision);
      id_vector_temp[0] = vertex_indices[1];
      spv::Id vertex_position_v01 = builder.CreateNoContractionBinOp(
          spv::OpFSub, type_float4,
          builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision),
          vertex_position_v0);
      id_vector_temp[0] = vertex_indices[2];
      spv::Id vertex_position_v3 = builder.CreateNoContractionBinOp(
          spv::OpFAdd, type_float4, vertex_position_v01,
          builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision));
      id_vector_temp.clear();
      id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
      builder.createStore(
          vertex_position_v3,
          builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
      for (u32 i = 0; i < clip_distance_count; ++i) {
        spv::Id const_int_i = builder.makeIntConstant(static_cast<i32>(i));
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_indices[0]);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        id_vector_temp.push_back(const_int_i);
        spv::Id vertex_clip_distance_v0 = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = vertex_indices[1];
        spv::Id vertex_clip_distance_v01 = builder.CreateNoContractionBinOp(
            spv::OpFSub, type_float,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision),
            vertex_clip_distance_v0);
        id_vector_temp[0] = vertex_indices[2];
        spv::Id vertex_clip_distance_v3 = builder.CreateNoContractionBinOp(
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
      builder.createNoResultOp(spv::OpEmitVertex);
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case PipelineGeometryShader::kQuadList: {
      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (key.has_point_coordinates) {
        spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero = builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      // Strip order 0, 1, 3, 2 as for GL_QUAD_STRIP.
      for (u32 i = 0; i < 4; ++i) {
        spv::Id const_vertex_index = builder.makeIntConstant(static_cast<i32>(i ^ (i >> 1)));
        id_vector_temp.clear();
        id_vector_temp.push_back(const_vertex_index);
        for (u32 j = 0; j < key.interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(spv::StorageClassInput,
                                                           in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        if (key.has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero, out_point_coordinates);
        }
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
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    default:
      REX_UNHANDLED_CASE(key.type);
  }

  builder.leaveFunction();

  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);
  if (const std::string dump_path = command_processor_.GetXenosSettings().dump_shaders.Get();
      !dump_path.empty()) {
    gpu::vulkan::objects::DumpShader(dump_path, std::format("gs_{:08X}", key.key), shader_code);
  }

  // The handle is stored even when creation fails so it is not retried.
  VkShaderModule shader_module = gpu::vulkan::objects::CreateShaderModule(
      command_processor_.GetVulkanDevice(), reinterpret_cast<const u32*>(shader_code.data()),
      sizeof(u32) * shader_code.size());
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
  auto fail = [&](const char* reason) {
    REXGPU_FAIL(false,
                "VulkanPipelineCache: TryGetPipelineCreationArgumentsForDescription "
                "failed: {} (vs={:016X}, ps={:016X}, topo={}, geom={}, tess_mode={}, "
                "for_placeholder={}, render_pass_key=0x{:08X})",
                reason, description.vertex_shader_hash, description.pixel_shader_hash,
                static_cast<u32>(description.primitive_topology),
                static_cast<u32>(description.geometry_shader),
                static_cast<u32>(description.tessellation_mode), static_cast<u32>(for_placeholder),
                description.render_pass_key.key);
  };

  if (!pipeline) {
    return fail("null_pipeline_storage");
  }
  if (!IsPipelineSupported(description)) {
    return fail("pipeline_requirements_not_met");
  }

  auto vertex_shader_it = shaders_.find(description.vertex_shader_hash);
  if (vertex_shader_it == shaders_.end()) {
    return fail("vertex_shader_not_found");
  }
  auto* vertex_shader = static_cast<VulkanShader::VulkanTranslation*>(
      vertex_shader_it->second->GetTranslation(description.vertex_shader_modification));
  if (!vertex_shader || !vertex_shader->IsTranslated() || !vertex_shader->IsValid()) {
    return fail("vertex_shader_translation_missing_or_invalid");
  }
  SPIRVShaderTranslator::Modification vertex_shader_modification(
      description.vertex_shader_modification);
  Shader::HostVertexShaderType host_vertex_shader_type =
      vertex_shader_modification.vertex.host_vertex_shader_type;
  bool tessellated = Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type);

  VulkanShader::VulkanTranslation* pixel_shader = nullptr;
  if (description.pixel_shader_hash && !for_placeholder) {
    auto pixel_shader_it = shaders_.find(description.pixel_shader_hash);
    if (pixel_shader_it == shaders_.end()) {
      return fail("pixel_shader_not_found");
    }
    pixel_shader = static_cast<VulkanShader::VulkanTranslation*>(
        pixel_shader_it->second->GetTranslation(description.pixel_shader_modification));
    if (!pixel_shader || !pixel_shader->IsTranslated() || !pixel_shader->IsValid()) {
      return fail("pixel_shader_translation_missing_or_invalid");
    }
  }
  SPIRVShaderTranslator::Modification pixel_shader_modification(
      description.pixel_shader_modification);

  VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
  VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
  u32 tessellation_patch_control_points = 0;
  if (tessellated) {
    xenos::TessellationMode tessellation_mode = description.tessellation_mode;
    tessellation_patch_control_points =
        GetTessellationPatchControlPointCount(host_vertex_shader_type, tessellation_mode);
    if (!tessellation_patch_control_points) {
      return fail("tessellation_patch_control_points_zero");
    }
    tessellation_vertex_shader =
        GetTessellationVertexShader(tessellation_mode == xenos::TessellationMode::kAdaptive);
    if (tessellation_vertex_shader == VK_NULL_HANDLE) {
      return fail("tessellation_vertex_shader_unavailable");
    }
    tessellation_control_shader =
        GetTessellationControlShader(host_vertex_shader_type, tessellation_mode);
    if (tessellation_control_shader == VK_NULL_HANDLE) {
      return fail("tessellation_control_shader_unavailable");
    }
  }

  VkShaderModule geometry_shader = VK_NULL_HANDLE;
  GeometryShaderKey geometry_shader_key;
  if (GetGeometryShaderKey(description.geometry_shader, vertex_shader_modification,
                           pixel_shader_modification, geometry_shader_key)) {
    geometry_shader = GetGeometryShader(geometry_shader_key);
    if (geometry_shader == VK_NULL_HANDLE) {
      return fail("geometry_shader_unavailable");
    }
  }

  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  bool use_dynamic_rendering = command_processor_.GetVulkanSettings().dynamic_rendering.Get() &&
                               vulkan_device->GetProperties().dynamicRendering;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (!use_dynamic_rendering) {
    render_pass =
        render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock
            ? render_target_cache_.GetPixelShaderInterlockRenderPass()
            : render_target_cache_.GetHostRenderTargetsRenderPass(description.render_pass_key);
    if (render_pass == VK_NULL_HANDLE) {
      return fail("render_pass_unavailable");
    }
  }

  const IPipelineLayoutProvider* pipeline_layout = command_processor_.GetPipelineLayout(
      (!for_placeholder && pixel_shader)
          ? static_cast<const VulkanShader&>(pixel_shader->GetShader())
                .GetTextureBindingsAfterTranslation()
                .size()
          : 0,
      (!for_placeholder && pixel_shader)
          ? static_cast<const VulkanShader&>(pixel_shader->GetShader())
                .GetSamplerBindingsAfterTranslation()
                .size()
          : 0,
      static_cast<const VulkanShader&>(vertex_shader->GetShader())
          .GetTextureBindingsAfterTranslation()
          .size(),
      static_cast<const VulkanShader&>(vertex_shader->GetShader())
          .GetSamplerBindingsAfterTranslation()
          .size());
  if (!pipeline_layout) {
    return fail("pipeline_layout_unavailable");
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

bool VulkanPipelineCache::CreatePipeline(const PipelineCreationArguments& creation_arguments,
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

  if (creation_arguments.pixel_shader) {
    REXGPU_DEBUG("Creating graphics pipeline state with VS {:016X}, PS {:016X}",
                 creation_arguments.vertex_shader->GetShader().GetUcodeDataHash(),
                 creation_arguments.pixel_shader->GetShader().GetUcodeDataHash());
  } else {
    REXGPU_DEBUG("Creating graphics pipeline state with VS {:016X}",
                 creation_arguments.vertex_shader->GetShader().GetUcodeDataHash());
  }

  const PipelineDescription& description = creation_arguments.pipeline->first;
  if (!IsPipelineSupported(description)) {
    REX_ASSERT_ALWAYS(
        "When creating a new pipeline, the description must not require "
        "unsupported features, and when loading the pipeline storage, "
        "pipelines with unsupported features must be filtered out");
    return false;
  }

  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();

  bool edram_fragment_shader_interlock =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  bool tessellated = description.primitive_topology == PipelinePrimitiveTopology::kPatchList;

  std::array<VkPipelineShaderStageCreateInfo, 5> shader_stages;
  u32 shader_stage_count = 0;

  REX_ASSERT(creation_arguments.vertex_shader->IsTranslated());
  if (!creation_arguments.vertex_shader->IsValid()) {
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
    shader_stage_tess_eval.module = creation_arguments.vertex_shader->GetShaderModule();
    REX_ASSERT(shader_stage_tess_eval.module != VK_NULL_HANDLE);
    shader_stage_tess_eval.pName = "main";
    shader_stage_tess_eval.pSpecializationInfo = nullptr;
  } else {
    VkPipelineShaderStageCreateInfo& shader_stage_vertex = shader_stages[shader_stage_count++];
    shader_stage_vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stage_vertex.pNext = nullptr;
    shader_stage_vertex.flags = 0;
    shader_stage_vertex.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stage_vertex.module = creation_arguments.vertex_shader->GetShaderModule();
    REX_ASSERT(shader_stage_vertex.module != VK_NULL_HANDLE);
    shader_stage_vertex.pName = "main";
    shader_stage_vertex.pSpecializationInfo = nullptr;
  }
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
    REX_ASSERT(creation_arguments.pixel_shader->IsTranslated());
    if (!creation_arguments.pixel_shader->IsValid()) {
      return false;
    }
    shader_stage_fragment.module = creation_arguments.pixel_shader->GetShaderModule();
    REX_ASSERT(shader_stage_fragment.module != VK_NULL_HANDLE);
  } else {
    if (edram_fragment_shader_interlock) {
      shader_stage_fragment.module = depth_only_fragment_shader_;
    } else if (render_target_cache_.IsDepthFloat24ConvertedInPixelShader() &&
               (description.render_pass_key.depth_and_color_used & 1) &&
               (description.depth_compare_op != xenos::CompareFunction::kAlways ||
                description.depth_write_enable) &&
               description.render_pass_key.depth_format ==
                   xenos::DepthRenderTargetFormat::kD24FS8) {
      shader_stage_fragment.module = render_target_cache_.IsDepthFloat24Rounded()
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
      REX_ASSERT(!description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kLineList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      REX_ASSERT(!description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kLineStrip:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      REX_ASSERT(!description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kTriangleStrip:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case PipelinePrimitiveTopology::kTriangleFan:
      REX_ASSERT_ALWAYS();
      return false;
    case PipelinePrimitiveTopology::kLineListWithAdjacency:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
      REX_ASSERT(!description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    case PipelinePrimitiveTopology::kPatchList:
      input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
      REX_ASSERT(!description.primitive_restart);
      if (description.primitive_restart) {
        return false;
      }
      break;
    default:
      REX_UNHANDLED_CASE(description.primitive_topology);
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
      REX_UNHANDLED_CASE(description.polygon_mode);
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
  // Depth bias is enabled only with a depth attachment, r is undefined in the formula without one.
  rasterization_state.depthBiasEnable =
      (!edram_fragment_shader_interlock && (description.render_pass_key.depth_and_color_used & 0b1))
          ? VK_TRUE
          : VK_FALSE;
  rasterization_state.lineWidth = 1.0f;

  bool subpass_has_attachments =
      !edram_fragment_shader_interlock && description.render_pass_key.depth_and_color_used != 0;
  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state =
      objects::BuildMultisampleState(VK_SAMPLE_COUNT_1_BIT);
  if (description.rasterizer_discard ||
      (!edram_fragment_shader_interlock && !subpass_has_attachments)) {
    // Rasterizer discard and attachmentless draws run at 1 sample.
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  } else if (description.render_pass_key.msaa_samples == xenos::MSAASamples::k2X &&
             !render_target_cache_.IsMSAA2xSupported(subpass_has_attachments)) {
    // 4 samples with samples 0 and 3 stand in for 2x, top left and bottom right.
    multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    // Sample masks are ignored without attachments.
    if (subpass_has_attachments) {
      sample_mask = 0b1001;
      multisample_state.pSampleMask = &sample_mask;
    }
  } else {
    multisample_state.rasterizationSamples = static_cast<VkSampleCountFlagBits>(
        static_cast<u32>(1) << static_cast<u32>(description.render_pass_key.msaa_samples));
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
      depth_stencil_state.depthCompareOp = static_cast<VkCompareOp>(
          static_cast<u32>(VK_COMPARE_OP_NEVER) + static_cast<u32>(description.depth_compare_op));
    }
    if (description.stencil_test_enable) {
      depth_stencil_state.stencilTestEnable = VK_TRUE;
      depth_stencil_state.front.failOp =
          static_cast<VkStencilOp>(static_cast<u32>(VK_STENCIL_OP_KEEP) +
                                   static_cast<u32>(description.stencil_front_fail_op));
      depth_stencil_state.front.passOp =
          static_cast<VkStencilOp>(static_cast<u32>(VK_STENCIL_OP_KEEP) +
                                   static_cast<u32>(description.stencil_front_pass_op));
      depth_stencil_state.front.depthFailOp =
          static_cast<VkStencilOp>(static_cast<u32>(VK_STENCIL_OP_KEEP) +
                                   static_cast<u32>(description.stencil_front_depth_fail_op));
      depth_stencil_state.front.compareOp =
          static_cast<VkCompareOp>(static_cast<u32>(VK_COMPARE_OP_NEVER) +
                                   static_cast<u32>(description.stencil_front_compare_op));
      depth_stencil_state.back.failOp =
          static_cast<VkStencilOp>(static_cast<u32>(VK_STENCIL_OP_KEEP) +
                                   static_cast<u32>(description.stencil_back_fail_op));
      depth_stencil_state.back.passOp =
          static_cast<VkStencilOp>(static_cast<u32>(VK_STENCIL_OP_KEEP) +
                                   static_cast<u32>(description.stencil_back_pass_op));
      depth_stencil_state.back.depthFailOp =
          static_cast<VkStencilOp>(static_cast<u32>(VK_STENCIL_OP_KEEP) +
                                   static_cast<u32>(description.stencil_back_depth_fail_op));
      depth_stencil_state.back.compareOp =
          static_cast<VkCompareOp>(static_cast<u32>(VK_COMPARE_OP_NEVER) +
                                   static_cast<u32>(description.stencil_back_compare_op));
    }
  }

  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  VkPipelineColorBlendAttachmentState color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  if (!edram_fragment_shader_interlock) {
    u32 color_rts_used = description.render_pass_key.depth_and_color_used >> 1;
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
      // 8 entries because 3 bits from the guest index it directly.
      static const VkBlendOp kBlendOpMap[] = {VK_BLEND_OP_ADD,
                                              VK_BLEND_OP_SUBTRACT,
                                              VK_BLEND_OP_MIN,
                                              VK_BLEND_OP_MAX,
                                              VK_BLEND_OP_REVERSE_SUBTRACT,
                                              VK_BLEND_OP_ADD,
                                              VK_BLEND_OP_ADD,
                                              VK_BLEND_OP_ADD};
      REX_ASSERT(vulkan_device->GetProperties().independentBlend);
      u32 color_rts_remaining = color_rts_used;
      u32 color_rt_index;
      while (rex::bit_scan_forward(color_rts_remaining, &color_rt_index)) {
        color_rts_remaining &= ~(static_cast<u32>(1) << color_rt_index);
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
              kBlendFactorMap[static_cast<u32>(color_rt.src_color_blend_factor)];
          color_blend_attachment.dstColorBlendFactor =
              kBlendFactorMap[static_cast<u32>(color_rt.dst_color_blend_factor)];
          color_blend_attachment.colorBlendOp =
              kBlendOpMap[static_cast<u32>(color_rt.color_blend_op)];
          color_blend_attachment.srcAlphaBlendFactor =
              kBlendFactorMap[static_cast<u32>(color_rt.src_alpha_blend_factor)];
          color_blend_attachment.dstAlphaBlendFactor =
              kBlendFactorMap[static_cast<u32>(color_rt.dst_alpha_blend_factor)];
          color_blend_attachment.alphaBlendOp =
              kBlendOpMap[static_cast<u32>(color_rt.alpha_blend_op)];
        }
        color_blend_attachment.colorWriteMask =
            static_cast<VkColorComponentFlags>(color_rt.color_write_mask);
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
  // Viewport, scissor and, without FSI, depth bias, blend constants and stencil state are dynamic.
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
  bool use_dynamic_rendering = command_processor_.GetVulkanSettings().dynamic_rendering.Get() &&
                               vulkan_device->GetProperties().dynamicRendering;
  if (use_dynamic_rendering) {
    pipeline_rendering_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipeline_rendering_create_info.pNext = nullptr;
    pipeline_rendering_create_info.viewMask = 0;

    u32 color_attachment_count = 0;
    const auto& key = description.render_pass_key;
    xenos::ColorRenderTargetFormat color_formats[] = {
        key.color_0_view_format, key.color_1_view_format, key.color_2_view_format,
        key.color_3_view_format};
    for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
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

  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  VkPipeline pipeline;
  VkResult create_result = dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                                         &pipeline_create_info, nullptr, &pipeline);
  if (create_result != VK_SUCCESS) {
    u64 ps_hash = creation_arguments.pixel_shader
                      ? creation_arguments.pixel_shader->GetShader().GetUcodeDataHash()
                      : 0;
    REXGPU_FAIL(
        false,
        "VulkanPipelineCache: vkCreateGraphicsPipelines failed (result={}, vs={:016X}, "
        "ps={:016X}, topo={}, geom={}, tess_mode={}, patch_cp={}, render_pass_key=0x{:08X}, "
        "dynamic_rendering={})",
        static_cast<i32>(create_result),
        creation_arguments.vertex_shader->GetShader().GetUcodeDataHash(), ps_hash,
        static_cast<u32>(description.primitive_topology),
        static_cast<u32>(description.geometry_shader),
        static_cast<u32>(description.tessellation_mode),
        creation_arguments.tessellation_patch_control_points, description.render_pass_key.key,
        static_cast<u32>(use_dynamic_rendering));
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

    bool created = CreatePipeline(creation_arguments);
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

    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      --creation_threads_busy_;
    }
  }
}

void VulkanPipelineCache::CreateQueuedPipelinesOnProcessorThread() {
  REX_ASSERT(!creation_threads_.empty());
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
    bool created = CreatePipeline(creation_arguments);
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
  u64 completed_submission = command_processor_.GetCompletedSubmission();
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
  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  for (VkPipeline pipeline : pipelines_to_destroy) {
    if (pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
}

void VulkanPipelineCache::StorageWriteThread() {
  ShaderStoredHeader shader_header;
  // Zeroed so padding bits never reach the file.
  std::memset(&shader_header, 0, sizeof(shader_header));

  std::vector<u32> ucode_guest_endian;
  ucode_guest_endian.reserve(0xFFFF);

  bool flush_shaders = false;
  bool flush_pipelines = false;

  while (true) {
    if (flush_shaders) {
      flush_shaders = false;
      REX_ASSERT(storage_.shader_file.is_open());
      storage_.shader_file.flush();
    }
    if (flush_pipelines) {
      flush_pipelines = false;
      REX_ASSERT(storage_.pipeline_file.is_open());
      storage_.pipeline_file.flush();
    }

    const Shader* shader = nullptr;
    PipelineStoredDescription pipeline_description;
    bool write_pipeline = false;
    {
      std::unique_lock<std::mutex> lock(storage_.write_request_lock);
      if (storage_.write_thread_shutdown) {
        return;
      }
      if (!storage_.write_shader_queue.empty()) {
        shader = storage_.write_shader_queue.front();
        storage_.write_shader_queue.pop_front();
      } else if (storage_.write_flush_shaders) {
        storage_.write_flush_shaders = false;
        flush_shaders = true;
      }
      if (!storage_.write_pipeline_queue.empty()) {
        pipeline_description = storage_.write_pipeline_queue.front();
        storage_.write_pipeline_queue.pop_front();
        write_pipeline = true;
      } else if (storage_.write_flush_pipelines) {
        storage_.write_flush_pipelines = false;
        flush_pipelines = true;
      }
      if (!shader && !write_pipeline) {
        storage_.write_request_cond.wait(lock);
        continue;
      }
    }

    if (shader) {
      shader_header.ucode_data_hash = shader->GetUcodeDataHash();
      shader_header.ucode_dword_count = shader->GetUcodeDwordCount();
      shader_header.type = shader->GetType();
      REX_ASSERT(storage_.shader_file.is_open());
      platform::AppendStorageFile(storage_.shader_file, &shader_header, sizeof(shader_header));
      if (shader_header.ucode_dword_count) {
        ucode_guest_endian.resize(shader_header.ucode_dword_count);
        // The hash is calculated over guest endianness, so swap back.
        memory::copy_and_swap(ucode_guest_endian.data(), shader->GetUcodeDwords(),
                              shader_header.ucode_dword_count);
        platform::AppendStorageFile(storage_.shader_file, ucode_guest_endian.data(),
                                    shader_header.ucode_dword_count * sizeof(u32));
      }
    }

    if (write_pipeline) {
      REX_ASSERT(storage_.pipeline_file.is_open());
      platform::AppendStorageFile(storage_.pipeline_file, &pipeline_description,
                                  sizeof(pipeline_description));
    }
  }
}

}  // namespace rex::gpu::vulkan
