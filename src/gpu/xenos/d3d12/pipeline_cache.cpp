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

#include "thirdparty/dxbc/DXBCChecksum.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include <format>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include "gpu/xenos/shared_cvars.h"
#include "gpu/xenos/d3d12/command_processor.h"
#include "gpu/xenos/d3d12/pipeline_cache.h"
#include "gpu/xenos/d3d12/render_target_cache.h"
#include "gpu/xenos/format/dxbc.h"
#include "gpu/xenos/pipeline_priority.h"
#include "gpu/xenos/pipeline/shader/dxbc_translator.h"
#include <rex/xenos/registers.h>
#include "gpu/xenos/draw_state.h"
#include <rex/xenos/xenos.h>
#include <rex/hash.h>
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/platform/file.h>
#include <rex/no_throw.h>
#include <rex/math.h>
#include <rex/string.h>
#include <rex/string/buffer.h>
#include <rex/platform/thread.h>
#include <rex/types.h>
#include "gpu/xenos/host/d3d12/d3d12_objects.h"

#include "gpu/xenos/cvar_table_pair.h"
#include "gpu/xenos/d3d12/cvar_table.h"

namespace rex::gpu::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/adaptive_quad_hs.h"
#include "../shaders/bytecode/d3d12_5_1/adaptive_triangle_hs.h"
#include "../shaders/bytecode/d3d12_5_1/continuous_quad_1cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/continuous_quad_4cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/continuous_triangle_1cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/continuous_triangle_3cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/discrete_quad_1cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/discrete_quad_4cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/discrete_triangle_1cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/discrete_triangle_3cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/float24_round_ps.h"
#include "../shaders/bytecode/d3d12_5_1/float24_truncate_ps.h"
#include "../shaders/bytecode/d3d12_5_1/tessellation_adaptive_vs.h"
#include "../shaders/bytecode/d3d12_5_1/tessellation_indexed_vs.h"
}  // namespace shaders

PipelineCache::PipelineCache(D3D12CommandProcessor& command_processor,
                             const xenos::RegisterFile& register_file,
                             const D3D12RenderTargetCache& render_target_cache,
                             bool bindless_resources_used)
    : command_processor_(command_processor),
      register_file_(register_file),
      render_target_cache_(render_target_cache),
      bindless_resources_used_(bindless_resources_used) {
  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();

  bool edram_rov_used =
      render_target_cache.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  shader_translator_ = std::make_unique<DXBCShaderTranslator>(
      provider.GetAdapterVendorID(), bindless_resources_used_, edram_rov_used,
      !render_target_cache_.IsGammaRenderTargetAsUnorm16(),
      render_target_cache_.IsMSAA2xSupported(), render_target_cache_.GetDrawResolutionScaleX(),
      render_target_cache_.GetDrawResolutionScaleY(), provider.GetGraphicsAnalysis() != nullptr);

  if (edram_rov_used) {
    depth_only_pixel_shader_ = shader_translator_->CreateDepthOnlyPixelShader();
  }
}

PipelineCache::~PipelineCache() {
  log::NoThrow(log::gpu, [this] { Shutdown(); });
}

bool PipelineCache::Initialize() {
  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();

  dxbc_converter_ = nullptr;
  dxc_utils_ = nullptr;
  dxc_compiler_ = nullptr;
  if (REXCVAR_GET(::rex::gpu::cvars::d3d12_dxbc_disasm_dxilconv)) {
    if (FAILED(provider.DXBCConverterCreateInstance(CLSID_DxbcConverter,
                                                    IID_PPV_ARGS(&dxbc_converter_)))) {
      REXLOG_ERROR(gpu,
                   "Failed to create DxbcConverter, converted DXIL disassembly for "
                   "debugging will be unavailable");
    }
    if (FAILED(provider.DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxc_utils_)))) {
      REXLOG_ERROR(gpu,
                   "Failed to create DxcUtils, converted DXIL disassembly for debugging "
                   "will be unavailable");
    }
    if (FAILED(provider.DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc_compiler_)))) {
      REXLOG_ERROR(gpu,
                   "Failed to create DxcCompiler, converted DXIL disassembly for "
                   "debugging will be unavailable");
    }
  }

  u32 logical_processor_count = rex::platform::GetLogicalProcessorCount();
  if (!logical_processor_count) {
    logical_processor_count = 6;
  }
  // Needed even without creation threads, as loading from the storage may start them.
  creation_threads_busy_ = 0;
  creation_completion_event_ = rex::platform::Event::CreateManualResetEvent(true);
  REX_ASSERT(creation_completion_event_ != nullptr);
  creation_completion_set_event_ = false;
  creation_threads_shutdown_from_ = SIZE_MAX;
  if (REXCVAR_GET(::rex::gpu::cvars::d3d12_pipeline_creation_threads) != 0) {
    size_t creation_thread_count;
    if (REXCVAR_GET(::rex::gpu::cvars::d3d12_pipeline_creation_threads) < 0) {
      creation_thread_count = std::max(logical_processor_count * 3 / 4, static_cast<u32>(1));
    } else {
      creation_thread_count = std::min(
          static_cast<u32>(REXCVAR_GET(::rex::gpu::cvars::d3d12_pipeline_creation_threads)),
          logical_processor_count);
    }
    for (size_t i = 0; i < creation_thread_count; ++i) {
      std::unique_ptr<rex::platform::Thread> creation_thread =
          rex::platform::Thread::Create({}, [this, i]() { CreationThread(i); });
      REX_ASSERT(creation_thread != nullptr);
      creation_thread->SetName("D3D12 Pipelines");
      creation_threads_.push_back(std::move(creation_thread));
    }
  }
  return true;
}

void PipelineCache::Shutdown() {
  // Threads may be creating pipelines, so shut them down before destroying the pipelines.
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
  creation_completion_event_.reset();

  ShutdownShaderStorage();

  current_pipeline_ = nullptr;
  for (const auto& it : pipelines_) {
    ID3D12PipelineState* state = it.second->state.load(std::memory_order_acquire);
    if (state) {
      state->Release();
    }
  }
  pipelines_.clear();
  REX_PROFILE_PLOT("gpu/pipeline_cache/pipelines", 0);

  if (bindless_resources_used_) {
    bindless_sampler_layout_map_.clear();
    bindless_sampler_layouts_.clear();
  }
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();
  shaders_.clear();
  storage_.shader_index = 0;

  gpu::d3d12::objects::ReleaseAndNull(dxc_compiler_);
  gpu::d3d12::objects::ReleaseAndNull(dxc_utils_);
  gpu::d3d12::objects::ReleaseAndNull(dxbc_converter_);
}

void PipelineCache::InitializeShaderStorage(const std::filesystem::path& cache_root, u32 title_id,
                                            bool blocking) {
  ShutdownShaderStorage();

  auto shader_storage_root = cache_root / "shaders";
  // Shareable files can move between hosts, host PSO blobs belong in shaders/local/.
  auto shader_storage_shareable_root = shader_storage_root / "shareable";
  if (!std::filesystem::exists(shader_storage_shareable_root)) {
    if (!std::filesystem::create_directories(shader_storage_shareable_root)) {
      REXLOG_ERROR(gpu,
                   "Failed to create the shareable shader storage directory, persistent "
                   "shader storage will be disabled, path {}",
                   rex::PathToUTF8(shader_storage_shareable_root));
      return;
    }
  }

  bool edram_rov_used =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  std::vector<PipelineStoredDescription> pipeline_stored_descriptions;
  std::set<std::pair<u64, u64>> shader_translations_needed;
  auto pipeline_storage_file_path =
      shader_storage_shareable_root /
      std::format("{:08X}.{}.d3d12.xpso", title_id, edram_rov_used ? "rov" : "rtv");
  storage_.pipeline_file_path = pipeline_storage_file_path;
  if (!platform::OpenStorageFile(storage_.pipeline_file, pipeline_storage_file_path)) {
    REXLOG_ERROR(gpu,
                 "Failed to open the Direct3D 12 pipeline description storage file for "
                 "writing, persistent shader storage will be disabled, path {}",
                 rex::PathToUTF8(pipeline_storage_file_path));
    return;
  }
  storage_.pipeline_file_flush_needed = false;
  // 'XEPS'.
  const u32 pipeline_storage_magic = 0x53504558;
  // 'DXRO' or 'DXRT'.
  const u32 pipeline_storage_magic_api = edram_rov_used ? 0x4F525844 : 0x54525844;
  const u32 pipeline_storage_version =
      std::max(PipelineDescription::kVersion, DXBCShaderTranslator::Modification::kVersion);
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
    const u64 pipeline_storage_file_size = platform::GetStorageFileSize(pipeline_storage_file_path);
    size_t pipeline_storage_description_count = static_cast<size_t>(
        pipeline_storage_file_size >= sizeof(pipeline_storage_file_header)
            ? (pipeline_storage_file_size - sizeof(pipeline_storage_file_header)) /
                  sizeof(PipelineStoredDescription)
            : 0);
    if (pipeline_storage_description_count) {
      pipeline_stored_descriptions.resize(pipeline_storage_description_count);
      pipeline_stored_descriptions.resize(
          platform::ReadStorageFile(
              storage_.pipeline_file, pipeline_stored_descriptions.data(),
              pipeline_storage_description_count * sizeof(PipelineStoredDescription)) /
          sizeof(PipelineStoredDescription));
      size_t pipeline_storage_read_count = pipeline_stored_descriptions.size();
      for (size_t i = 0; i < pipeline_storage_read_count; ++i) {
        const PipelineStoredDescription& pipeline_stored_description =
            pipeline_stored_descriptions[i];
        if (XXH3_64bits(&pipeline_stored_description.description,
                        sizeof(pipeline_stored_description.description)) !=
            pipeline_stored_description.description_hash) {
          pipeline_stored_descriptions.resize(i);
          break;
        }
        shader_translations_needed.emplace(
            pipeline_stored_description.description.vertex_shader_hash,
            pipeline_stored_description.description.vertex_shader_modification);
        if (pipeline_stored_description.description.pixel_shader_hash) {
          shader_translations_needed.emplace(
              pipeline_stored_description.description.pixel_shader_hash,
              pipeline_stored_description.description.pixel_shader_modification);
        }
      }
    }
  }

  size_t logical_processor_count = rex::platform::GetLogicalProcessorCount();
  if (!logical_processor_count) {
    logical_processor_count = 6;
  }

  u64 shader_storage_initialization_start = rex::Clock::GetHostTickCount();
  auto shader_storage_file_path =
      shader_storage_shareable_root / std::format("{:08X}.xsh", title_id);
  storage_.shader_file_path = shader_storage_file_path;
  if (!platform::OpenStorageFile(storage_.shader_file, shader_storage_file_path)) {
    REXLOG_ERROR(gpu,
                 "Failed to open the title shader storage file for writing, persistent "
                 "shader storage will be disabled, path {}",
                 rex::PathToUTF8(shader_storage_file_path));
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
    size_t shaders_translated = 0;

    std::mutex shaders_translation_thread_mutex;
    std::condition_variable shaders_translation_thread_cond;
    std::deque<D3D12Shader*> shaders_to_translate;
    size_t shader_translation_threads_busy = 0;
    bool shader_translation_threads_shutdown = false;
    std::mutex shaders_failed_to_translate_mutex;
    std::vector<D3D12Shader::D3D12Translation*> shaders_failed_to_translate;
    auto shader_translation_thread_function = [&]() {
      const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
      StringBuffer ucode_disasm_buffer;
      DXBCShaderTranslator translator(
          provider.GetAdapterVendorID(), bindless_resources_used_, edram_rov_used,
          !render_target_cache_.IsGammaRenderTargetAsUnorm16(),
          render_target_cache_.IsMSAA2xSupported(), render_target_cache_.GetDrawResolutionScaleX(),
          render_target_cache_.GetDrawResolutionScaleY(),
          provider.GetGraphicsAnalysis() != nullptr);
      IDxbcConverter* dxbc_converter = nullptr;
      IDxcUtils* dxc_utils = nullptr;
      IDxcCompiler* dxc_compiler = nullptr;
      if (REXCVAR_GET(::rex::gpu::cvars::d3d12_dxbc_disasm_dxilconv) && dxbc_converter_ &&
          dxc_utils_ && dxc_compiler_) {
        provider.DXBCConverterCreateInstance(CLSID_DxbcConverter, IID_PPV_ARGS(&dxbc_converter));
        provider.DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxc_utils));
        provider.DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc_compiler));
      }
      for (;;) {
        D3D12Shader* shader_to_translate;
        for (;;) {
          std::unique_lock<std::mutex> lock(shaders_translation_thread_mutex);
          if (shaders_to_translate.empty()) {
            if (shader_translation_threads_shutdown) {
              return;
            }
            shaders_translation_thread_cond.wait(lock);
            continue;
          }
          shader_to_translate = shaders_to_translate.front();
          shaders_to_translate.pop_front();
          ++shader_translation_threads_busy;
          break;
        }
        shader_to_translate->AnalyzeUcode(ucode_disasm_buffer);
        u64 ucode_data_hash = shader_to_translate->GetUcodeDataHash();
        for (auto modification_it = shader_translations_needed.lower_bound(
                 std::make_pair(ucode_data_hash, static_cast<u64>(0)));
             modification_it != shader_translations_needed.end() &&
             modification_it->first == ucode_data_hash;
             ++modification_it) {
          D3D12Shader::D3D12Translation* translation = static_cast<D3D12Shader::D3D12Translation*>(
              shader_to_translate->GetOrCreateTranslation(modification_it->second));
          // Only new translations are dropped on failure, so earlier failures are not retried.
          if (!translation->IsTranslated() &&
              !TranslateAnalyzedShader(translator, *translation, dxbc_converter, dxc_utils,
                                       dxc_compiler)) {
            std::lock_guard<std::mutex> lock(shaders_failed_to_translate_mutex);
            shaders_failed_to_translate.push_back(translation);
          }
        }
        {
          std::lock_guard<std::mutex> lock(shaders_translation_thread_mutex);
          --shader_translation_threads_busy;
        }
      }
      if (dxc_compiler) {
        dxc_compiler->Release();
      }
      if (dxc_utils) {
        dxc_utils->Release();
      }
      if (dxbc_converter) {
        dxbc_converter->Release();
      }
    };
    std::vector<std::unique_ptr<rex::platform::Thread>> shader_translation_threads;

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
      D3D12Shader* shader = LoadShader(shader_header.type, ucode_dwords.data(),
                                       shader_header.ucode_dword_count, ucode_data_hash);
      if (shader->GetUcodeStorageIndex() == storage_.shader_index) {
        // A duplicate in the file, translating it twice in parallel would race.
        continue;
      }
      shader->SetUcodeStorageIndex(storage_.shader_index);
      size_t shader_translation_threads_needed;
      {
        std::lock_guard<std::mutex> lock(shaders_translation_thread_mutex);
        shader_translation_threads_needed = std::min(
            shader_translation_threads_busy + shaders_to_translate.size() + static_cast<size_t>(1),
            logical_processor_count - static_cast<size_t>(1));
      }
      while (shader_translation_threads.size() < shader_translation_threads_needed) {
        auto thread = rex::platform::Thread::Create({}, shader_translation_thread_function);
        REX_ASSERT(thread != nullptr);
        thread->SetName("Shader Translation");
        shader_translation_threads.push_back(std::move(thread));
      }
      {
        std::lock_guard<std::mutex> lock(shaders_translation_thread_mutex);
        shaders_to_translate.push_back(shader);
      }
      shaders_translation_thread_cond.notify_one();
      ++shaders_translated;
    }
    if (!shader_translation_threads.empty()) {
      {
        std::lock_guard<std::mutex> lock(shaders_translation_thread_mutex);
        shader_translation_threads_shutdown = true;
      }
      shaders_translation_thread_cond.notify_all();
      for (auto& shader_translation_thread : shader_translation_threads) {
        rex::platform::Wait(shader_translation_thread.get(), false);
      }
      shader_translation_threads.clear();
      for (D3D12Shader::D3D12Translation* translation : shaders_failed_to_translate) {
        D3D12Shader* shader = static_cast<D3D12Shader*>(&translation->GetShader());
        shader->DestroyTranslation(translation->GetModification());
        if (shader->GetTranslations().empty()) {
          shaders_.erase(shader->GetUcodeDataHash());
        }
      }
    }
    REXLOG_INFO(gpu, "Translated {} shaders from the storage in {} milliseconds",
                shaders_translated,
                (rex::Clock::GetHostTickCount() - shader_storage_initialization_start) * 1000 /
                    rex::Clock::GetHostTickFrequency());
    platform::ResetStorageFile(storage_.shader_file, storage_.shader_file_path,
                               shader_storage_valid_bytes);
  } else {
    platform::ResetStorageFile(storage_.shader_file, storage_.shader_file_path, 0);
    shader_storage_file_header.magic = shader_storage_magic;
    shader_storage_file_header.version = ShaderStoredHeader::kVersion;
    platform::AppendStorageFile(storage_.shader_file, &shader_storage_file_header,
                                sizeof(shader_storage_file_header));
  }

  if (!pipeline_stored_descriptions.empty()) {
    u64 pipeline_creation_start = rex::Clock::GetHostTickCount();

    size_t creation_thread_original_count = creation_threads_.size();
    size_t creation_thread_needed_count =
        std::max(std::min(pipeline_stored_descriptions.size(), logical_processor_count) -
                     static_cast<size_t>(1),
                 creation_thread_original_count);
    while (creation_threads_.size() < creation_thread_needed_count) {
      size_t creation_thread_index = creation_threads_.size();
      std::unique_ptr<rex::platform::Thread> creation_thread = rex::platform::Thread::Create(
          {}, [this, creation_thread_index]() { CreationThread(creation_thread_index); });
      REX_ASSERT(creation_thread != nullptr);
      creation_thread->SetName("D3D12 Pipelines");
      creation_threads_.push_back(std::move(creation_thread));
    }

    size_t pipelines_created = 0;
    for (const PipelineStoredDescription& pipeline_stored_description :
         pipeline_stored_descriptions) {
      const PipelineDescription& pipeline_description = pipeline_stored_description.description;
      auto found_range = pipelines_.equal_range(pipeline_stored_description.description_hash);
      bool pipeline_found = false;
      for (auto it = found_range.first; it != found_range.second; ++it) {
        Pipeline* found_pipeline = it->second.get();
        if (found_pipeline->description.description == pipeline_description) {
          pipeline_found = true;
          break;
        }
      }
      if (pipeline_found) {
        continue;
      }

      PipelineRuntimeDescription pipeline_runtime_description;
      auto vertex_shader_it = shaders_.find(pipeline_description.vertex_shader_hash);
      if (vertex_shader_it == shaders_.end()) {
        continue;
      }
      D3D12Shader* vertex_shader = vertex_shader_it->second.get();
      pipeline_runtime_description.vertex_shader = static_cast<D3D12Shader::D3D12Translation*>(
          vertex_shader->GetTranslation(pipeline_description.vertex_shader_modification));
      if (!pipeline_runtime_description.vertex_shader ||
          !pipeline_runtime_description.vertex_shader->IsTranslated() ||
          !pipeline_runtime_description.vertex_shader->IsValid()) {
        continue;
      }
      D3D12Shader* pixel_shader;
      if (pipeline_description.pixel_shader_hash) {
        auto pixel_shader_it = shaders_.find(pipeline_description.pixel_shader_hash);
        if (pixel_shader_it == shaders_.end()) {
          continue;
        }
        pixel_shader = pixel_shader_it->second.get();
        pipeline_runtime_description.pixel_shader = static_cast<D3D12Shader::D3D12Translation*>(
            pixel_shader->GetTranslation(pipeline_description.pixel_shader_modification));
        if (!pipeline_runtime_description.pixel_shader ||
            !pipeline_runtime_description.pixel_shader->IsTranslated() ||
            !pipeline_runtime_description.pixel_shader->IsValid()) {
          continue;
        }
      } else {
        pixel_shader = nullptr;
        pipeline_runtime_description.pixel_shader = nullptr;
      }
      GeometryShaderKey pipeline_geometry_shader_key;
      pipeline_runtime_description.geometry_shader =
          GetGeometryShaderKey(
              pipeline_description.geometry_shader,
              DXBCShaderTranslator::Modification(pipeline_description.vertex_shader_modification),
              DXBCShaderTranslator::Modification(pipeline_description.pixel_shader_modification),
              pipeline_geometry_shader_key)
              ? &GetGeometryShader(pipeline_geometry_shader_key)
              : nullptr;
      pipeline_runtime_description.root_signature = command_processor_.GetRootSignature(
          vertex_shader, pixel_shader,
          Shader::IsHostVertexShaderTypeDomain(
              DXBCShaderTranslator::Modification(pipeline_description.vertex_shader_modification)
                  .vertex.host_vertex_shader_type));
      if (!pipeline_runtime_description.root_signature) {
        continue;
      }
      std::memcpy(&pipeline_runtime_description.description, &pipeline_description,
                  sizeof(pipeline_description));

      auto new_pipeline_owner = std::make_unique<Pipeline>();
      Pipeline* new_pipeline = new_pipeline_owner.get();
      std::memcpy(&new_pipeline->description, &pipeline_runtime_description,
                  sizeof(pipeline_runtime_description));
      new_pipeline->root_signature.store(pipeline_runtime_description.root_signature,
                                         std::memory_order_release);
      u32 bound_rts = 0;
      for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        if (pipeline_runtime_description.description.render_targets[i].used) {
          bound_rts |= static_cast<u32>(1) << i;
        }
      }
      u32 shader_writes_color_targets =
          pipeline_runtime_description.pixel_shader
              ? pipeline_runtime_description.pixel_shader->GetShader().GetWritesColorTargets()
              : 0;
      bool shader_writes_depth =
          pipeline_runtime_description.pixel_shader
              ? pipeline_runtime_description.pixel_shader->GetShader().IsDepthWritten()
              : pipeline_runtime_description.description.depth_write != 0;
      new_pipeline->priority = pipeline_priority::CalculatePipelinePriority(
          bound_rts, shader_writes_color_targets, shader_writes_depth);
      pipelines_.emplace(pipeline_stored_description.description_hash,
                         std::move(new_pipeline_owner));
      REX_PROFILE_PLOT("gpu/pipeline_cache/pipelines", pipelines_.size());
      if (!creation_threads_.empty()) {
        {
          std::lock_guard<std::mutex> lock(creation_request_lock_);
          creation_queue_.push(new_pipeline);
        }
        creation_request_cond_.notify_one();
      } else {
        new_pipeline->state.store(CreateD3D12Pipeline(pipeline_runtime_description),
                                  std::memory_order_release);
      }
      ++pipelines_created;
    }

    if (!creation_threads_.empty()) {
      CreateQueuedPipelinesOnProcessorThread();
      if (creation_threads_.size() > creation_thread_original_count) {
        {
          std::lock_guard<std::mutex> lock(creation_request_lock_);
          creation_threads_shutdown_from_ = creation_thread_original_count;
          // The queue is empty after CreateQueuedPipelinesOnProcessorThread.
        }
        creation_request_cond_.notify_all();
        while (creation_threads_.size() > creation_thread_original_count) {
          rex::platform::Wait(creation_threads_.back().get(), false);
          creation_threads_.pop_back();
        }
        bool await_creation_completion_event;
        {
          std::lock_guard<std::mutex> lock(creation_request_lock_);
          creation_threads_shutdown_from_ = SIZE_MAX;
          await_creation_completion_event = blocking && creation_threads_busy_ != 0;
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

    REXLOG_INFO(gpu,
                "Created {} graphics pipelines (not including reading the "
                "descriptions) from the storage in {} milliseconds",
                pipelines_created,
                (rex::Clock::GetHostTickCount() - pipeline_creation_start) * 1000 /
                    rex::Clock::GetHostTickFrequency());
    platform::ResetStorageFile(
        storage_.pipeline_file, storage_.pipeline_file_path,
        static_cast<u64>(sizeof(pipeline_storage_file_header) +
                         sizeof(PipelineStoredDescription) * pipeline_stored_descriptions.size()));
  } else {
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
  storage_.write_thread->SetName("D3D12 Storage writer");
}

void PipelineCache::ShutdownShaderStorage() {
  storage_.Shutdown();
}

void PipelineCache::EndSubmission() {
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
    CreateQueuedPipelinesOnProcessorThread();
    bool await_creation_completion_event;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      // The queue is empty as the processor thread created the leftovers.
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

bool PipelineCache::IsCreatingPipelines() {
  if (creation_threads_.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(creation_request_lock_);
  return !creation_queue_.empty() || creation_threads_busy_ != 0;
}

D3D12Shader* PipelineCache::LoadShader(xenos::ShaderType shader_type, const u32* host_address,
                                       u32 dword_count) {
  return LoadShader(shader_type, host_address, dword_count,
                    XXH3_64bits(host_address, dword_count * sizeof(u32)));
}

D3D12Shader* PipelineCache::LoadShader(xenos::ShaderType shader_type, const u32* host_address,
                                       u32 dword_count, u64 data_hash) {
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  // Failed translations stay tracked so they are not retried.
  auto shader = std::make_unique<D3D12Shader>(shader_type, data_hash, host_address, dword_count);
  D3D12Shader* raw = shader.get();
  shaders_.emplace(data_hash, std::move(shader));
  return raw;
}

DXBCShaderTranslator::Modification PipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    u32 interpolator_mask) const {
  REX_ASSERT(shader.GetType() == xenos::ShaderType::kVertex);
  REX_ASSERT(shader.IsUcodeAnalyzed());
  const auto& regs = register_file_;

  DXBCShaderTranslator::Modification modification(
      shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<xenos::reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;

  auto pa_cl_clip_cntl = regs.Get<xenos::reg::PA_CL_CLIP_CNTL>();
  u32 user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = rex::BitCount(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      static_cast<u32>(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
  modification.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;
  modification.vertex.vertex_kill_and = static_cast<u32>(
      (shader.GetWritesPointSizeEdgeFlagKillVertex() & 0b100) && !pa_cl_clip_cntl.vtx_kill_or);

  modification.vertex.output_point_size = static_cast<u32>(
      (shader.GetWritesPointSizeEdgeFlagKillVertex() & 0b001) &&
      regs.Get<xenos::reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);

  return modification;
}

DXBCShaderTranslator::Modification PipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, u32 interpolator_mask, u32 param_gen_pos,
    xenos::reg::RB_DEPTHCONTROL normalized_depth_control) const {
  REX_ASSERT(shader.GetType() == xenos::ShaderType::kPixel);
  REX_ASSERT(shader.IsUcodeAnalyzed());
  const auto& regs = register_file_;

  DXBCShaderTranslator::Modification modification(
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
    using DepthStencilMode = DXBCShaderTranslator::Modification::DepthStencilMode;
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

bool PipelineCache::ConfigurePipeline(
    D3D12Shader::D3D12Translation* vertex_shader, D3D12Shader::D3D12Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    xenos::reg::RB_DEPTHCONTROL normalized_depth_control, u32 normalized_color_mask,
    u32 bound_depth_and_color_render_target_bits,
    const u32* bound_depth_and_color_render_target_formats, void** pipeline_handle_out,
    ID3D12RootSignature** root_signature_out) {
#if REXGPU_FINE_GRAINED_DRAW_SCOPES
  REX_PROFILE_SCOPE("gpu");
#endif  // REXGPU_FINE_GRAINED_DRAW_SCOPES

  REX_ASSERT(pipeline_handle_out != nullptr);
  REX_ASSERT(root_signature_out != nullptr);

  bool use_async = REXCVAR_GET(::rex::gpu::cvars::async_shader_compilation) &&
                   !creation_threads_.empty() && pixel_shader != nullptr;

  // Edge flags are not supported as polygon primitives are not.
  REX_ASSERT(register_file_.Get<xenos::reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                 xenos::VertexShaderExportMode::kPosition2VectorsEdge &&
             register_file_.Get<xenos::reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                 xenos::VertexShaderExportMode::kPosition2VectorsEdgeKill);
  REX_ASSERT(!(register_file_.Get<xenos::reg::SQ_PROGRAM_CNTL>().gen_index_vtx));
  // Ucode analysis runs on the main thread, translation may be deferred to background threads.
  if (!vertex_shader->GetShader().IsUcodeAnalyzed()) {
    vertex_shader->GetShader().AnalyzeUcode(ucode_disasm_buffer_);
  }
  if (!vertex_shader->IsTranslated() && !use_async) {
    std::lock_guard<std::mutex> lock(translation_request_lock_);
    if (!vertex_shader->IsTranslated()) {
      if (!TranslateAnalyzedShader(*shader_translator_, *vertex_shader, dxbc_converter_, dxc_utils_,
                                   dxc_compiler_)) {
        REX_FAIL(gpu, false, "Failed to translate the vertex shader!");
      }
      if (storage_.shader_file.is_open() &&
          vertex_shader->GetShader().GetUcodeStorageIndex() != storage_.shader_index) {
        vertex_shader->GetShader().SetUcodeStorageIndex(storage_.shader_index);
        REX_ASSERT(storage_.write_thread != nullptr);
        storage_.shader_file_flush_needed = true;
        {
          std::lock_guard<std::mutex> storage_lock(storage_.write_request_lock);
          storage_.write_shader_queue.push_back(&vertex_shader->GetShader());
        }
        storage_.write_request_cond.notify_all();
      }
    }
  }
  if (!use_async && !vertex_shader->IsValid()) {
    return false;
  }
  if (pixel_shader != nullptr) {
    if (!pixel_shader->IsTranslated() && !use_async) {
      std::lock_guard<std::mutex> lock(translation_request_lock_);
      if (!pixel_shader->IsTranslated()) {
        pixel_shader->GetShader().AnalyzeUcode(ucode_disasm_buffer_);
        if (!TranslateAnalyzedShader(*shader_translator_, *pixel_shader, dxbc_converter_,
                                     dxc_utils_, dxc_compiler_)) {
          REX_FAIL(gpu, false, "Failed to translate the pixel shader!");
        }
        if (storage_.shader_file.is_open() &&
            pixel_shader->GetShader().GetUcodeStorageIndex() != storage_.shader_index) {
          pixel_shader->GetShader().SetUcodeStorageIndex(storage_.shader_index);
          REX_ASSERT(storage_.write_thread != nullptr);
          storage_.shader_file_flush_needed = true;
          {
            std::lock_guard<std::mutex> storage_lock(storage_.write_request_lock);
            storage_.write_shader_queue.push_back(&pixel_shader->GetShader());
          }
          storage_.write_request_cond.notify_all();
        }
      }
    }
    if (pixel_shader->IsTranslated() && !pixel_shader->IsValid()) {
      return false;
    }
  }

  PipelineRuntimeDescription runtime_description;
  if (!GetCurrentStateDescription(
          vertex_shader, pixel_shader, primitive_processing_result, normalized_depth_control,
          normalized_color_mask, bound_depth_and_color_render_target_bits,
          bound_depth_and_color_render_target_formats, runtime_description, use_async)) {
    return false;
  }
  PipelineDescription& description = runtime_description.description;

  if (current_pipeline_ != nullptr && current_pipeline_->description.description == description) {
    *pipeline_handle_out = current_pipeline_;
    *root_signature_out = current_pipeline_->root_signature.load(std::memory_order_acquire);
    return true;
  }

  u64 hash = XXH3_64bits(&description, sizeof(description));
  auto found_range = pipelines_.equal_range(hash);
  for (auto it = found_range.first; it != found_range.second; ++it) {
    Pipeline* found_pipeline = it->second.get();
    if (found_pipeline->description.description == description) {
      REX_PERF_INC(kPipelineCacheHits);
      current_pipeline_ = found_pipeline;
      *pipeline_handle_out = found_pipeline;
      *root_signature_out = found_pipeline->root_signature.load(std::memory_order_acquire);
      return true;
    }
  }
  REX_PERF_INC(kPipelineCacheMisses);

  auto new_pipeline_owner = std::make_unique<Pipeline>();
  Pipeline* new_pipeline = new_pipeline_owner.get();
  std::memcpy(&new_pipeline->description, &runtime_description, sizeof(runtime_description));
  new_pipeline->root_signature.store(runtime_description.root_signature, std::memory_order_release);
  pipelines_.emplace(hash, std::move(new_pipeline_owner));
  REX_PROFILE_PLOT("gpu/pipeline_cache/pipelines", pipelines_.size());

  if (use_async) {
    u32 bound_rts = pipeline_priority::GetBoundRTMaskFromNormalizedColorMask(normalized_color_mask);
    u32 shader_writes_color_targets =
        pixel_shader ? pixel_shader->GetShader().GetWritesColorTargets() : 0;
    bool shader_writes_depth = pixel_shader ? pixel_shader->GetShader().IsDepthWritten()
                                            : normalized_depth_control.z_write_enable != 0;
    new_pipeline->priority = pipeline_priority::CalculatePipelinePriority(
        bound_rts, shader_writes_color_targets, shader_writes_depth);
    new_pipeline->pending_vertex_shader = vertex_shader;
    new_pipeline->pending_pixel_shader = pixel_shader;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_queue_.push(new_pipeline);
    }
    creation_request_cond_.notify_one();
  } else {
    new_pipeline->state.store(CreateD3D12Pipeline(runtime_description), std::memory_order_release);
  }

  if (storage_.pipeline_file.is_open()) {
    REX_ASSERT(storage_.write_thread != nullptr);
    storage_.pipeline_file_flush_needed = true;
    {
      std::lock_guard<std::mutex> lock(storage_.write_request_lock);
      storage_.write_pipeline_queue.emplace_back();
      PipelineStoredDescription& stored_description = storage_.write_pipeline_queue.back();
      stored_description.description_hash = hash;
      std::memcpy(&stored_description.description, &description, sizeof(description));
    }
    storage_.write_request_cond.notify_all();
  }

  current_pipeline_ = new_pipeline;
  *pipeline_handle_out = new_pipeline;
  *root_signature_out = new_pipeline->root_signature.load(std::memory_order_acquire);
  return true;
}

bool PipelineCache::TranslateAnalyzedShader(DXBCShaderTranslator& translator,
                                            D3D12Shader::D3D12Translation& translation,
                                            IDxbcConverter* dxbc_converter, IDxcUtils* dxc_utils,
                                            IDxcCompiler* dxc_compiler) {
  D3D12Shader& shader = static_cast<D3D12Shader&>(translation.GetShader());

  if (!translator.TranslateAnalyzedShader(translation)) {
    REX_FAIL(gpu, false, "Shader {:016X} translation failed and the shader is marked as ignored",
             shader.GetUcodeDataHash());
  }

  const char* host_shader_type;
  if (shader.GetType() == xenos::ShaderType::kVertex) {
    DXBCShaderTranslator::Modification modification(translation.GetModification());
    switch (modification.vertex.host_vertex_shader_type) {
      case Shader::HostVertexShaderType::kLineDomainCPIndexed:
        host_shader_type = "control point indexed line domain";
        break;
      case Shader::HostVertexShaderType::kLineDomainPatchIndexed:
        host_shader_type = "patch indexed line domain";
        break;
      case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
        host_shader_type = "control point indexed triangle domain";
        break;
      case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
        host_shader_type = "patch indexed triangle domain";
        break;
      case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
        host_shader_type = "control point indexed quad domain";
        break;
      case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
        host_shader_type = "patch indexed quad domain";
        break;
      default:
        assert(modification.vertex.host_vertex_shader_type ==
               Shader::HostVertexShaderType::kVertex);
        host_shader_type = "vertex";
    }
  } else {
    host_shader_type = "pixel";
  }
  REXLOG_DEV(gpu, "Generated {} shader ({}b) with hash {:016X}\n{}\n", host_shader_type,
             shader.GetUcodeDwordCount() * sizeof(u32), shader.GetUcodeDataHash(),
             shader.GetUcodeDisassembly().c_str());

  if (shader.EnterBindingLayoutUserUIDSetup()) {
    const std::vector<D3D12Shader::TextureBinding>& texture_bindings =
        shader.GetTextureBindingsAfterTranslation();
    size_t texture_binding_count = texture_bindings.size();
    const std::vector<D3D12Shader::SamplerBinding>& sampler_bindings =
        shader.GetSamplerBindingsAfterTranslation();
    size_t sampler_binding_count = sampler_bindings.size();
    REX_ASSERT(!(bindless_resources_used_ && texture_binding_count + sampler_binding_count >
                                                 D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 4));
    size_t texture_binding_layout_bytes = texture_binding_count * sizeof(*texture_bindings.data());
    u64 texture_binding_layout_hash = 0;
    if (texture_binding_count) {
      texture_binding_layout_hash =
          XXH3_64bits(texture_bindings.data(), texture_binding_layout_bytes);
    }
    size_t bindless_sampler_count = bindless_resources_used_ ? sampler_binding_count : 0;
    u64 bindless_sampler_layout_hash = 0;
    if (bindless_sampler_count) {
      XXH3_state_t hash_state;
      XXH3_64bits_reset(&hash_state);
      for (size_t i = 0; i < bindless_sampler_count; ++i) {
        XXH3_64bits_update(&hash_state, &sampler_bindings[i].bindless_descriptor_index,
                           sizeof(sampler_bindings[i].bindless_descriptor_index));
      }
      bindless_sampler_layout_hash = XXH3_64bits_digest(&hash_state);
    }
    size_t texture_binding_layout_uid = kLayoutUIDEmpty;
    // For bindful, only the sampler count must match for layouts to be compatible.
    static_assert(kLayoutUIDEmpty == 0,
                  "Empty layout UID is assumed to be 0 because for bindful samplers, the "
                  "UID is their count");
    size_t sampler_binding_layout_uid =
        bindless_resources_used_ ? kLayoutUIDEmpty : sampler_binding_count;
    if (texture_binding_count || bindless_sampler_count) {
      std::lock_guard<std::mutex> layouts_lock(layouts_mutex_);
      if (texture_binding_count) {
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
                        "Layout UID is size + 1 because it's assumed that 0 is the UID "
                        "for an empty layout");
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
      if (bindless_sampler_count) {
        auto found_range = bindless_sampler_layout_map_.equal_range(bindless_sampler_layout_hash);
        for (auto it = found_range.first; it != found_range.second; ++it) {
          if (it->second.vector_span_length != bindless_sampler_count) {
            continue;
          }
          sampler_binding_layout_uid = it->second.uid;
          const u32* vector_bindless_sampler_layout =
              bindless_sampler_layouts_.data() + it->second.vector_span_offset;
          for (size_t i = 0; i < bindless_sampler_count; ++i) {
            if (vector_bindless_sampler_layout[i] !=
                sampler_bindings[i].bindless_descriptor_index) {
              sampler_binding_layout_uid = kLayoutUIDEmpty;
              break;
            }
          }
          if (sampler_binding_layout_uid != kLayoutUIDEmpty) {
            break;
          }
        }
        if (sampler_binding_layout_uid == kLayoutUIDEmpty) {
          sampler_binding_layout_uid = bindless_sampler_layout_map_.size() + 1;
          LayoutUID new_uid;
          static_assert(kLayoutUIDEmpty == 0,
                        "Layout UID is size + 1 because it's assumed that 0 is the UID "
                        "for an empty layout");
          new_uid.uid = sampler_binding_layout_uid;
          new_uid.vector_span_offset = bindless_sampler_layouts_.size();
          new_uid.vector_span_length = sampler_binding_count;
          bindless_sampler_layouts_.resize(new_uid.vector_span_offset + sampler_binding_count);
          u32* vector_bindless_sampler_layout =
              bindless_sampler_layouts_.data() + new_uid.vector_span_offset;
          for (size_t i = 0; i < bindless_sampler_count; ++i) {
            vector_bindless_sampler_layout[i] = sampler_bindings[i].bindless_descriptor_index;
          }
          bindless_sampler_layout_map_.emplace(bindless_sampler_layout_hash, new_uid);
        }
      }
    }
    shader.SetTextureBindingLayoutUserUID(texture_binding_layout_uid);
    shader.SetSamplerBindingLayoutUserUID(sampler_binding_layout_uid);
  }

  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  if (REXCVAR_GET(::rex::gpu::cvars::d3d12_dxbc_disasm_dxilconv)) {
    translation.DisassembleDXBCAndDXIL(provider, REXCVAR_GET(::rex::gpu::cvars::d3d12_dxbc_disasm),
                                       dxbc_converter, dxc_utils, dxc_compiler);
  } else {
    translation.DisassembleDXBCAndDXIL(provider, REXCVAR_GET(::rex::gpu::cvars::d3d12_dxbc_disasm));
  }

  if (const std::string dump_path = GetDumpShaders(); !dump_path.empty()) {
    bool edram_rov_used =
        render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
    translation.Dump(dump_path, (shader.GetType() == xenos::ShaderType::kPixel)
                                    ? (edram_rov_used ? "d3d12_rov" : "d3d12_rtv")
                                    : "d3d12");
  }

  return translation.IsValid();
}

bool PipelineCache::GetCurrentStateDescription(
    D3D12Shader::D3D12Translation* vertex_shader, D3D12Shader::D3D12Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    xenos::reg::RB_DEPTHCONTROL normalized_depth_control, u32 normalized_color_mask,
    u32 bound_depth_and_color_render_target_bits,
    const u32* bound_depth_and_color_render_target_formats,
    PipelineRuntimeDescription& runtime_description_out, bool for_placeholder) {
  // Translated shaders are required for the root signature unless for_placeholder defers them.
  REX_ASSERT(for_placeholder || (vertex_shader->IsTranslated() && vertex_shader->IsValid()));
  REX_ASSERT(for_placeholder || !pixel_shader ||
             (pixel_shader->IsTranslated() && pixel_shader->IsValid()));

  PipelineDescription& description_out = runtime_description_out.description;

  const auto& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<xenos::reg::PA_SU_SC_MODE_CNTL>();

  // Unused fields must be zero for comparison and hashing.
  std::memset(&runtime_description_out, 0, sizeof(runtime_description_out));

  REX_ASSERT(DXBCShaderTranslator::Modification(vertex_shader->GetModification())
                 .vertex.host_vertex_shader_type ==
             primitive_processing_result.host_vertex_shader_type);
  bool tessellated = primitive_processing_result.IsTessellated();
  bool primitive_polygonal = draw_state::IsPrimitivePolygonal(regs);
  bool rasterization_enabled = draw_state::IsRasterizationDone(
      regs, primitive_polygonal, pixel_shader && pixel_shader->GetShader().GetMemExportEMWritten());
  // With rasterization off the caller omits the pixel shader so texture binding layouts match.
  if (!rasterization_enabled) {
    REX_ASSERT(pixel_shader == nullptr);
    if (pixel_shader) {
      return false;
    }
  }

  bool edram_rov_used =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  runtime_description_out.root_signature = command_processor_.GetRootSignature(
      static_cast<const DXBCShader*>(&vertex_shader->GetShader()),
      (pixel_shader && !for_placeholder)
          ? static_cast<const DXBCShader*>(&pixel_shader->GetShader())
          : nullptr,
      tessellated);
  if (runtime_description_out.root_signature == nullptr) {
    return false;
  }

  runtime_description_out.vertex_shader = vertex_shader;
  description_out.vertex_shader_hash = vertex_shader->GetShader().GetUcodeDataHash();
  description_out.vertex_shader_modification = vertex_shader->GetModification();

  if (primitive_processing_result.host_primitive_reset_enabled) {
    description_out.strip_cut_index =
        primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
            ? PipelineStripCutIndex::kFFFF
            : PipelineStripCutIndex::kFFFFFFFF;
  } else {
    description_out.strip_cut_index = PipelineStripCutIndex::kNone;
  }

  if (tessellated) {
    description_out.primitive_topology_type_or_tessellation_mode =
        static_cast<u32>(primitive_processing_result.tessellation_mode);
  } else {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        description_out.primitive_topology_type_or_tessellation_mode =
            std::to_underlying(PipelinePrimitiveTopologyType::kPoint);
        break;
      case xenos::PrimitiveType::kLineList:
      case xenos::PrimitiveType::kLineStrip:
      // Quads are emulated as line lists with adjacency.
      case xenos::PrimitiveType::kQuadList:
      case xenos::PrimitiveType::k2DLineStrip:
        description_out.primitive_topology_type_or_tessellation_mode =
            std::to_underlying(PipelinePrimitiveTopologyType::kLine);
        break;
      default:
        description_out.primitive_topology_type_or_tessellation_mode =
            std::to_underlying(PipelinePrimitiveTopologyType::kTriangle);
        break;
    }
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        description_out.geometry_shader = PipelineGeometryShader::kPointList;
        break;
      case xenos::PrimitiveType::kRectangleList:
        description_out.geometry_shader = PipelineGeometryShader::kRectangleList;
        break;
      case xenos::PrimitiveType::kQuadList:
        description_out.geometry_shader = PipelineGeometryShader::kQuadList;
        break;
      default:
        description_out.geometry_shader = PipelineGeometryShader::kNone;
        break;
    }
  }
  GeometryShaderKey geometry_shader_key;
  runtime_description_out.geometry_shader =
      GetGeometryShaderKey(
          description_out.geometry_shader,
          DXBCShaderTranslator::Modification(vertex_shader->GetModification()),
          DXBCShaderTranslator::Modification(pixel_shader ? pixel_shader->GetModification() : 0),
          geometry_shader_key)
          ? &GetGeometryShader(geometry_shader_key)
          : nullptr;

  // The rest is irrelevant with rasterization disabled, nothing is written or counted.
  if (!rasterization_enabled) {
    description_out.cull_mode = PipelineCullMode::kDisableRasterization;
    return true;
  }

  if (pixel_shader) {
    runtime_description_out.pixel_shader = pixel_shader;
    description_out.pixel_shader_hash = pixel_shader->GetShader().GetUcodeDataHash();
    description_out.pixel_shader_modification = pixel_shader->GetModification();
  }

  bool cull_front, cull_back;
  if (primitive_polygonal) {
    description_out.front_counter_clockwise = pa_su_sc_mode_cntl.face == 0;
    cull_front = pa_su_sc_mode_cntl.cull_front != 0;
    cull_back = pa_su_sc_mode_cntl.cull_back != 0;
    if (cull_front) {
      // Culling both faces is handled by disabling rasterization.
      REX_ASSERT(!cull_back);
      description_out.cull_mode = PipelineCullMode::kFront;
    } else if (cull_back) {
      description_out.cull_mode = PipelineCullMode::kBack;
    } else {
      description_out.cull_mode = PipelineCullMode::kNone;
    }
    if (!cull_front) {
      if (pa_su_sc_mode_cntl.polymode_front_ptype != xenos::PolygonType::kTriangles) {
        description_out.fill_mode_wireframe = 1;
      }
    }
    if (!cull_back) {
      if (pa_su_sc_mode_cntl.polymode_back_ptype != xenos::PolygonType::kTriangles) {
        description_out.fill_mode_wireframe = 1;
      }
    }
    if (pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDualMode) {
      description_out.fill_mode_wireframe = 0;
    }
  } else {
    cull_front = false;
    cull_back = false;
  }
  // With ROV the depth bias is applied in the pixel shader, as MSAA needs per sample depth.
  if (!edram_rov_used) {
    f32 polygon_offset, polygon_offset_scale;
    draw_state::GetPreferredFacePolygonOffset(regs, primitive_polygonal, polygon_offset_scale,
                                              polygon_offset);
    description_out.depth_bias = draw_state::GetD3D10IntegerPolygonOffset(
        regs.Get<xenos::reg::RB_DEPTH_INFO>().depth_format, polygon_offset);
    description_out.depth_bias_slope_scaled =
        polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit;
  }
  if (tessellated && REXCVAR_GET(::rex::gpu::cvars::d3d12_tessellation_wireframe)) {
    description_out.fill_mode_wireframe = 1;
  }
  description_out.depth_clip = !regs.Get<xenos::reg::PA_CL_CLIP_CNTL>().clip_disable;
  bool depth_stencil_bound_and_used = false;
  if (!edram_rov_used) {
    // No stencil, an always passing depth test and no depth writes mean depth is disabled.
    if (bound_depth_and_color_render_target_bits & 1) {
      if (normalized_depth_control.z_enable) {
        description_out.depth_func = normalized_depth_control.zfunc;
        description_out.depth_write = normalized_depth_control.z_write_enable;
      } else {
        description_out.depth_func = xenos::CompareFunction::kAlways;
      }
      if (normalized_depth_control.stencil_enable) {
        description_out.stencil_enable = 1;
        bool stencil_backface_enable =
            primitive_polygonal && normalized_depth_control.backface_enable;
        // Direct3D 12 has no per face masks, so back face ones apply only to back faces.
        xenos::Register stencil_ref_mask_reg;
        if (stencil_backface_enable && cull_front) {
          stencil_ref_mask_reg = xenos::Register::RB_STENCILREFMASK_BF;
        } else {
          stencil_ref_mask_reg = xenos::Register::RB_STENCILREFMASK;
        }
        auto stencil_ref_mask = regs.Get<xenos::reg::RB_STENCILREFMASK>(stencil_ref_mask_reg);
        description_out.stencil_read_mask = stencil_ref_mask.stencilmask;
        description_out.stencil_write_mask = stencil_ref_mask.stencilwritemask;
        description_out.stencil_front_fail_op = normalized_depth_control.stencilfail;
        description_out.stencil_front_depth_fail_op = normalized_depth_control.stencilzfail;
        description_out.stencil_front_pass_op = normalized_depth_control.stencilzpass;
        description_out.stencil_front_func = normalized_depth_control.stencilfunc;
        if (stencil_backface_enable) {
          description_out.stencil_back_fail_op = normalized_depth_control.stencilfail_bf;
          description_out.stencil_back_depth_fail_op = normalized_depth_control.stencilzfail_bf;
          description_out.stencil_back_pass_op = normalized_depth_control.stencilzpass_bf;
          description_out.stencil_back_func = normalized_depth_control.stencilfunc_bf;
        } else {
          description_out.stencil_back_fail_op = description_out.stencil_front_fail_op;
          description_out.stencil_back_depth_fail_op = description_out.stencil_front_depth_fail_op;
          description_out.stencil_back_pass_op = description_out.stencil_front_pass_op;
          description_out.stencil_back_func = description_out.stencil_front_func;
        }
      }
      // The format is set only when depth or stencil is used.
      if (description_out.depth_func != xenos::CompareFunction::kAlways ||
          description_out.depth_write || description_out.stencil_enable) {
        description_out.depth_format = static_cast<xenos::DepthRenderTargetFormat>(
            bound_depth_and_color_render_target_formats[0]);
        depth_stencil_bound_and_used = true;
      }
    } else {
      description_out.depth_func = xenos::CompareFunction::kAlways;
    }

    // 32 entries because of the 0x1F mask, unknown values map to 0.
    static const PipelineBlendFactor kBlendFactorMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,
        /*  3 */ PipelineBlendFactor::kZero,
        /*  4 */ PipelineBlendFactor::kSrcColor,
        /*  5 */ PipelineBlendFactor::kInvSrcColor,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDestColor,
        /*  9 */ PipelineBlendFactor::kInvDestColor,
        /* 10 */ PipelineBlendFactor::kDestAlpha,
        /* 11 */ PipelineBlendFactor::kInvDestAlpha,
        /* 12 */ PipelineBlendFactor::kBlendFactor,
        /* 13 */ PipelineBlendFactor::kInvBlendFactor,
        /* 14 */ PipelineBlendFactor::kBlendFactor,
        /* 15 */ PipelineBlendFactor::kInvBlendFactor,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSat,
    };
    // Like kBlendFactorMap, with color modes changed to alpha modes.
    static const PipelineBlendFactor kBlendFactorAlphaMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,
        /*  3 */ PipelineBlendFactor::kZero,
        /*  4 */ PipelineBlendFactor::kSrcAlpha,
        /*  5 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDestAlpha,
        /*  9 */ PipelineBlendFactor::kInvDestAlpha,
        /* 10 */ PipelineBlendFactor::kDestAlpha,
        /* 11 */ PipelineBlendFactor::kInvDestAlpha,
        /* 12 */ PipelineBlendFactor::kBlendFactor,
        /* 13 */ PipelineBlendFactor::kInvBlendFactor,
        /* 14 */ PipelineBlendFactor::kBlendFactor,
        /* 15 */ PipelineBlendFactor::kInvBlendFactor,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSat,
    };
    for (u32 i = 0; i < 4; ++i) {
      if (!(bound_depth_and_color_render_target_bits & (static_cast<u32>(1) << (1 + i)))) {
        continue;
      }
      PipelineRenderTarget& rt = description_out.render_targets[i];
      rt.used = 1;
      rt.format = static_cast<xenos::ColorRenderTargetFormat>(
          bound_depth_and_color_render_target_formats[1 + i]);
      rt.write_mask = (normalized_color_mask >> (i * 4)) & 0xF;
      if (rt.write_mask) {
        auto blendcontrol = regs.Get<xenos::reg::RB_BLENDCONTROL>(
            xenos::reg::RB_BLENDCONTROL::kRTRegisterIndices[i]);
        rt.src_blend = kBlendFactorMap[static_cast<u32>(blendcontrol.color_srcblend)];
        rt.dest_blend = kBlendFactorMap[static_cast<u32>(blendcontrol.color_destblend)];
        rt.blend_op = blendcontrol.color_comb_fcn;
        rt.src_blend_alpha = kBlendFactorAlphaMap[static_cast<u32>(blendcontrol.alpha_srcblend)];
        rt.dest_blend_alpha = kBlendFactorAlphaMap[static_cast<u32>(blendcontrol.alpha_destblend)];
        rt.blend_op_alpha = blendcontrol.alpha_comb_fcn;
      } else {
        rt.src_blend = PipelineBlendFactor::kOne;
        rt.dest_blend = PipelineBlendFactor::kZero;
        rt.blend_op = xenos::BlendOp::kAdd;
        rt.src_blend_alpha = PipelineBlendFactor::kOne;
        rt.dest_blend_alpha = PipelineBlendFactor::kZero;
        rt.blend_op_alpha = xenos::BlendOp::kAdd;
      }
    }
  }
  xenos::MSAASamples host_msaa_samples = regs.Get<xenos::reg::RB_SURFACE_INFO>().msaa_samples;
  if (edram_rov_used) {
    if (host_msaa_samples == xenos::MSAASamples::k2X) {
      // 2 is not supported in ForcedSampleCount on Nvidia.
      host_msaa_samples = xenos::MSAASamples::k4X;
    }
  } else {
    if (!(bound_depth_and_color_render_target_bits & ~static_cast<u32>(1)) &&
        !depth_stencil_bound_and_used) {
      // Direct3D 12 requires a sample count of 1 when no render targets are bound.
      host_msaa_samples = xenos::MSAASamples::k1X;
    }
  }
  description_out.host_msaa_samples = host_msaa_samples;

  return true;
}

bool PipelineCache::GetGeometryShaderKey(
    PipelineGeometryShader geometry_shader_type,
    DXBCShaderTranslator::Modification vertex_shader_modification,
    DXBCShaderTranslator::Modification pixel_shader_modification, GeometryShaderKey& key_out) {
  if (geometry_shader_type == PipelineGeometryShader::kNone) {
    return false;
  }
  REX_ASSERT(vertex_shader_modification.vertex.interpolator_mask ==
             pixel_shader_modification.pixel.interpolator_mask);
  GeometryShaderKey key;
  key.type = geometry_shader_type;
  key.interpolator_count = rex::BitCount(vertex_shader_modification.vertex.interpolator_mask);
  key.user_clip_plane_count = vertex_shader_modification.vertex.user_clip_plane_count;
  key.user_clip_plane_cull = vertex_shader_modification.vertex.user_clip_plane_cull;
  key.has_vertex_kill_and = vertex_shader_modification.vertex.vertex_kill_and;
  key.has_point_size = vertex_shader_modification.vertex.output_point_size;
  key.has_point_coordinates = pixel_shader_modification.pixel.param_gen_point;
  key.point_ps_ucp_mode = vertex_shader_modification.vertex.point_ps_ucp_mode;
  key_out = key;
  return true;
}

void PipelineCache::CreateDXBCGeometryShader(GeometryShaderKey key, std::vector<u32>& shader_out) {
  shader_out.clear();
  u32 point_clip_distance_count = key.user_clip_plane_cull ? 0 : key.user_clip_plane_count;
  u32 point_user_cull_distance_count =
      key.type == PipelineGeometryShader::kPointList && key.user_clip_plane_cull
          ? key.user_clip_plane_count
          : 0;
  bool point_recalculate_clip_distances = key.type == PipelineGeometryShader::kPointList &&
                                          point_clip_distance_count && key.point_ps_ucp_mode >= 2;
  bool point_recalculate_cull_distances = key.type == PipelineGeometryShader::kPointList &&
                                          point_user_cull_distance_count &&
                                          key.point_ps_ucp_mode >= 3;

  shader_out.resize(sizeof(dxbc::ContainerHeader) / sizeof(u32) + dxbc::kBlobCount);
  u32 blob_offset_position_dwords = sizeof(dxbc::ContainerHeader) / sizeof(u32);
  u32 blob_position_dwords = static_cast<u32>(shader_out.size());

  u32 name_ptr;

  shader_out[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 rdef_position_dwords = blob_position_dwords + dxbc::kBlobHeaderSizeDwords;

  shader_out.resize(rdef_position_dwords + sizeof(dxbc::RDEFHeader) / sizeof(u32));
  dxbc::AppendAlignedString(shader_out, "ReXGlue");
  {
    auto& rdef_header =
        *reinterpret_cast<dxbc::RDEFHeader*>(shader_out.data() + rdef_position_dwords);
    rdef_header.shader_model = dxbc::RDEFShaderModel::kGeometryShader51;
    rdef_header.compile_flags =
        dxbc::kCompileFlagNoPreshader | dxbc::kCompileFlagPreferFlowControl |
        dxbc::kCompileFlagIEEEStrictness | dxbc::kCompileFlagAllResourcesBound;
    rdef_header.generator_name_ptr = sizeof(dxbc::RDEFHeader);
    rdef_header.fourcc = dxbc::RDEFHeader::FourCC::k51;
    rdef_header.InitializeSizes();
  }

  u32 system_cbuffer_size_vector_aligned_bytes = 0;

  if (key.type == PipelineGeometryShader::kPointList) {
    name_ptr = static_cast<u32>((shader_out.size() - rdef_position_dwords) * sizeof(u32));
    u32 rdef_name_ptr_float2 = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "float2");
    u32 rdef_type_float2_position_dwords = static_cast<u32>(shader_out.size());
    u32 rdef_type_float2_ptr =
        static_cast<u32>((rdef_type_float2_position_dwords - rdef_position_dwords) * sizeof(u32));
    shader_out.resize(rdef_type_float2_position_dwords + sizeof(dxbc::RDEFType) / sizeof(u32));
    {
      auto& rdef_type_float2 =
          *reinterpret_cast<dxbc::RDEFType*>(shader_out.data() + rdef_type_float2_position_dwords);
      rdef_type_float2.variable_class = dxbc::RDEFVariableClass::kVector;
      rdef_type_float2.variable_type = dxbc::RDEFVariableType::kFloat;
      rdef_type_float2.row_count = 1;
      rdef_type_float2.column_count = 2;
      rdef_type_float2.name_ptr = rdef_name_ptr_float2;
    }

    enum PointConstant : u8 {
      kPointConstantConstantDiameter,
      kPointConstantScreenDiameterToNDCRadius,
      kPointConstantCount,
    };
    name_ptr = static_cast<u32>((shader_out.size() - rdef_position_dwords) * sizeof(u32));
    u32 rdef_name_ptr_xe_point_constant_diameter = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "xe_point_constant_diameter");
    u32 rdef_name_ptr_xe_point_screen_diameter_to_ndc_radius = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "xe_point_screen_diameter_to_ndc_radius");
    u32 rdef_constants_position_dwords = static_cast<u32>(shader_out.size());
    u32 rdef_constants_ptr =
        static_cast<u32>((rdef_constants_position_dwords - rdef_position_dwords) * sizeof(u32));
    shader_out.resize(rdef_constants_position_dwords +
                      sizeof(dxbc::RDEFVariable) / sizeof(u32) * kPointConstantCount);
    {
      auto rdef_constants =
          reinterpret_cast<dxbc::RDEFVariable*>(shader_out.data() + rdef_constants_position_dwords);
      static_assert(sizeof(DXBCShaderTranslator::SystemConstants ::point_constant_diameter) ==
                        sizeof(f32) * 2,
                    "DXBCShaderTranslator point_constant_diameter system constant size "
                    "differs between the shader translator and geometry shader "
                    "generation");
      REX_STATIC_ASSERT_SIZE(DXBCShaderTranslator::SystemConstants::point_constant_diameter,
                             sizeof(f32) * 2);
      dxbc::RDEFVariable& rdef_constant_point_constant_diameter =
          rdef_constants[kPointConstantConstantDiameter];
      rdef_constant_point_constant_diameter.name_ptr = rdef_name_ptr_xe_point_constant_diameter;
      rdef_constant_point_constant_diameter.start_offset_bytes =
          offsetof(DXBCShaderTranslator::SystemConstants, point_constant_diameter);
      rdef_constant_point_constant_diameter.size_bytes = sizeof(f32) * 2;
      rdef_constant_point_constant_diameter.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_point_constant_diameter.type_ptr = rdef_type_float2_ptr;
      rdef_constant_point_constant_diameter.start_texture = UINT32_MAX;
      rdef_constant_point_constant_diameter.start_sampler = UINT32_MAX;
      static_assert(
          sizeof(DXBCShaderTranslator::SystemConstants ::point_screen_diameter_to_ndc_radius) ==
              sizeof(f32) * 2,
          "DXBCShaderTranslator point_screen_diameter_to_ndc_radius system "
          "constant size differs between the shader translator and geometry "
          "shader generation");
      dxbc::RDEFVariable& rdef_constant_point_screen_diameter_to_ndc_radius =
          rdef_constants[kPointConstantScreenDiameterToNDCRadius];
      rdef_constant_point_screen_diameter_to_ndc_radius.name_ptr =
          rdef_name_ptr_xe_point_screen_diameter_to_ndc_radius;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_offset_bytes =
          offsetof(DXBCShaderTranslator::SystemConstants, point_screen_diameter_to_ndc_radius);
      rdef_constant_point_screen_diameter_to_ndc_radius.size_bytes = sizeof(f32) * 2;
      rdef_constant_point_screen_diameter_to_ndc_radius.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_point_screen_diameter_to_ndc_radius.type_ptr = rdef_type_float2_ptr;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_texture = UINT32_MAX;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_sampler = UINT32_MAX;
    }

    name_ptr = static_cast<u32>((shader_out.size() - rdef_position_dwords) * sizeof(u32));
    u32 rdef_name_ptr_xe_system_cbuffer = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "xe_system_cbuffer");
    u32 rdef_cbuffer_position_dwords = static_cast<u32>(shader_out.size());
    shader_out.resize(rdef_cbuffer_position_dwords + sizeof(dxbc::RDEFCbuffer) / sizeof(u32));
    {
      auto& rdef_cbuffer_system =
          *reinterpret_cast<dxbc::RDEFCbuffer*>(shader_out.data() + rdef_cbuffer_position_dwords);
      rdef_cbuffer_system.name_ptr = rdef_name_ptr_xe_system_cbuffer;
      rdef_cbuffer_system.variable_count = kPointConstantCount;
      rdef_cbuffer_system.variables_ptr = rdef_constants_ptr;
      auto rdef_constants = reinterpret_cast<const dxbc::RDEFVariable*>(
          shader_out.data() + rdef_constants_position_dwords);
      for (u32 i = 0; i < kPointConstantCount; ++i) {
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     rdef_constants[i].start_offset_bytes + rdef_constants[i].size_bytes);
      }
      if (point_recalculate_clip_distances || point_recalculate_cull_distances) {
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     static_cast<u32>(offsetof(DXBCShaderTranslator::SystemConstants, ndc_scale) +
                                      sizeof(f32) * 3));
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     static_cast<u32>(offsetof(DXBCShaderTranslator::SystemConstants, ndc_offset) +
                                      sizeof(f32) * 3));
        system_cbuffer_size_vector_aligned_bytes = std::max(
            system_cbuffer_size_vector_aligned_bytes,
            static_cast<u32>(offsetof(DXBCShaderTranslator::SystemConstants, user_clip_planes) +
                             sizeof(f32) * 4 * 6));
      }
      system_cbuffer_size_vector_aligned_bytes = rex::RoundUpPow2(
          system_cbuffer_size_vector_aligned_bytes, static_cast<u32>(sizeof(u32) * 4));
      rdef_cbuffer_system.size_vector_aligned_bytes = system_cbuffer_size_vector_aligned_bytes;
    }

    u32 rdef_binding_position_dwords = static_cast<u32>(shader_out.size());
    shader_out.resize(rdef_binding_position_dwords + sizeof(dxbc::RDEFInputBind) / sizeof(u32));
    {
      auto& rdef_binding_cbuffer_system =
          *reinterpret_cast<dxbc::RDEFInputBind*>(shader_out.data() + rdef_binding_position_dwords);
      rdef_binding_cbuffer_system.name_ptr = rdef_name_ptr_xe_system_cbuffer;
      rdef_binding_cbuffer_system.type = dxbc::RDEFInputType::kCbuffer;
      rdef_binding_cbuffer_system.bind_point =
          std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants);
      rdef_binding_cbuffer_system.bind_count = 1;
      rdef_binding_cbuffer_system.flags = dxbc::kRdefInputFlagUserPacked;
    }

    {
      auto& rdef_header =
          *reinterpret_cast<dxbc::RDEFHeader*>(shader_out.data() + rdef_position_dwords);
      rdef_header.cbuffer_count = 1;
      rdef_header.cbuffers_ptr =
          static_cast<u32>((rdef_cbuffer_position_dwords - rdef_position_dwords) * sizeof(u32));
      rdef_header.input_bind_count = 1;
      rdef_header.input_binds_ptr =
          static_cast<u32>((rdef_binding_position_dwords - rdef_position_dwords) * sizeof(u32));
    }
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = static_cast<u32>(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - dxbc::kBlobHeaderSizeDwords) * sizeof(u32) -
                             shader_out[blob_offset_position_dwords++];
  }

  u32 input_clip_distance_count = key.user_clip_plane_cull ? 0 : key.user_clip_plane_count;
  u32 input_cull_distance_count =
      (key.user_clip_plane_cull ? key.user_clip_plane_count : 0) + key.has_vertex_kill_and;
  u32 input_clip_and_cull_distance_count = input_clip_distance_count + input_cull_distance_count;

  u32 isgn_parameter_count =
      key.interpolator_count + 1 + ((input_clip_and_cull_distance_count + 3) / 4) +
      static_cast<u32>(input_cull_distance_count && (input_clip_distance_count & 3) != 0) +
      key.has_point_size;

  shader_out[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 isgn_position_dwords = blob_position_dwords + dxbc::kBlobHeaderSizeDwords;
  shader_out.resize(isgn_position_dwords + sizeof(dxbc::Signature) / sizeof(u32) +
                    sizeof(dxbc::SignatureParameter) / sizeof(u32) * isgn_parameter_count);

  name_ptr = static_cast<u32>((shader_out.size() - isgn_position_dwords) * sizeof(u32));
  u32 isgn_name_ptr_texcoord = name_ptr;
  if (key.interpolator_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "TEXCOORD");
  }
  u32 isgn_name_ptr_sv_position = name_ptr;
  name_ptr += dxbc::AppendAlignedString(shader_out, "SV_Position");
  u32 isgn_name_ptr_sv_clip_distance = name_ptr;
  if (input_clip_distance_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "SV_ClipDistance");
  }
  u32 isgn_name_ptr_sv_cull_distance = name_ptr;
  if (input_cull_distance_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "SV_CullDistance");
  }
  u32 isgn_name_ptr_xepsize = name_ptr;
  if (key.has_point_size) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "XEPSIZE");
  }

  u32 input_register_interpolators = UINT32_MAX;
  u32 input_register_position;
  u32 input_register_clip_and_cull_distances = UINT32_MAX;
  u32 input_register_point_size = UINT32_MAX;
  {
    auto& isgn_header =
        *reinterpret_cast<dxbc::Signature*>(shader_out.data() + isgn_position_dwords);
    isgn_header.parameter_count = isgn_parameter_count;
    isgn_header.parameter_info_ptr = sizeof(dxbc::Signature);

    auto isgn_parameters = reinterpret_cast<dxbc::SignatureParameter*>(
        shader_out.data() + isgn_position_dwords + sizeof(dxbc::Signature) / sizeof(u32));
    u32 isgn_parameter_index = 0;
    u32 input_register_index = 0;

    if (key.interpolator_count) {
      input_register_interpolators = input_register_index;
      for (u32 i = 0; i < key.interpolator_count; ++i) {
        REX_ASSERT(isgn_parameter_index < isgn_parameter_count);
        dxbc::SignatureParameter& isgn_interpolator = isgn_parameters[isgn_parameter_index++];
        isgn_interpolator.semantic_name_ptr = isgn_name_ptr_texcoord;
        isgn_interpolator.semantic_index = i;
        isgn_interpolator.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        isgn_interpolator.register_index = input_register_index++;
        isgn_interpolator.mask = 0b1111;
        isgn_interpolator.always_reads_mask = 0b1111;
      }
    }

    input_register_position = input_register_index;
    REX_ASSERT(isgn_parameter_index < isgn_parameter_count);
    dxbc::SignatureParameter& isgn_sv_position = isgn_parameters[isgn_parameter_index++];
    isgn_sv_position.semantic_name_ptr = isgn_name_ptr_sv_position;
    isgn_sv_position.system_value = dxbc::SystemValueName::kPosition;
    isgn_sv_position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
    isgn_sv_position.register_index = input_register_index++;
    isgn_sv_position.mask = 0b1111;
    isgn_sv_position.always_reads_mask = 0b1111;

    if (input_clip_and_cull_distance_count) {
      input_register_clip_and_cull_distances = input_register_index;
      u32 isgn_cull_distance_semantic_index = 0;
      for (u32 i = 0; i < input_clip_and_cull_distance_count; i += 4) {
        if (i < input_clip_distance_count) {
          dxbc::SignatureParameter& isgn_sv_clip_distance = isgn_parameters[isgn_parameter_index++];
          isgn_sv_clip_distance.semantic_name_ptr = isgn_name_ptr_sv_clip_distance;
          isgn_sv_clip_distance.semantic_index = i / 4;
          isgn_sv_clip_distance.system_value = dxbc::SystemValueName::kClipDistance;
          isgn_sv_clip_distance.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
          isgn_sv_clip_distance.register_index = input_register_index;
          u8 isgn_sv_clip_distance_mask =
              (UINT8_C(1) << std::min(input_clip_distance_count - i, UINT32_C(4))) - 1;
          isgn_sv_clip_distance.mask = isgn_sv_clip_distance_mask;
          isgn_sv_clip_distance.always_reads_mask = isgn_sv_clip_distance_mask;
        }
        if (input_cull_distance_count && i + 4 > input_clip_distance_count) {
          dxbc::SignatureParameter& isgn_sv_cull_distance = isgn_parameters[isgn_parameter_index++];
          isgn_sv_cull_distance.semantic_name_ptr = isgn_name_ptr_sv_cull_distance;
          isgn_sv_cull_distance.semantic_index = isgn_cull_distance_semantic_index++;
          isgn_sv_cull_distance.system_value = dxbc::SystemValueName::kCullDistance;
          isgn_sv_cull_distance.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
          isgn_sv_cull_distance.register_index = input_register_index;
          u8 isgn_sv_cull_distance_mask =
              (UINT8_C(1) << std::min(input_clip_and_cull_distance_count - i, UINT32_C(4))) - 1;
          if (i < input_clip_distance_count) {
            isgn_sv_cull_distance_mask &= ~((UINT8_C(1) << (input_clip_distance_count - i)) - 1);
          }
          isgn_sv_cull_distance.mask = isgn_sv_cull_distance_mask;
          isgn_sv_cull_distance.always_reads_mask = isgn_sv_cull_distance_mask;
        }
        ++input_register_index;
      }
    }

    if (key.has_point_size) {
      input_register_point_size = input_register_index;
      REX_ASSERT(isgn_parameter_index < isgn_parameter_count);
      dxbc::SignatureParameter& isgn_point_size = isgn_parameters[isgn_parameter_index++];
      isgn_point_size.semantic_name_ptr = isgn_name_ptr_xepsize;
      isgn_point_size.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      isgn_point_size.register_index = input_register_index++;
      isgn_point_size.mask = 0b0001;
      isgn_point_size.always_reads_mask =
          key.type == PipelineGeometryShader::kPointList ? 0b0001 : 0;
    }

    REX_ASSERT(isgn_parameter_index == isgn_parameter_count);
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kInputSignature;
    blob_position_dwords = static_cast<u32>(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - dxbc::kBlobHeaderSizeDwords) * sizeof(u32) -
                             shader_out[blob_offset_position_dwords++];
  }

  u32 osgn_parameter_count = key.interpolator_count + key.has_point_coordinates + 1 +
                             ((input_clip_distance_count + 3) / 4);

  shader_out[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 osgn_position_dwords = blob_position_dwords + dxbc::kBlobHeaderSizeDwords;
  shader_out.resize(osgn_position_dwords + sizeof(dxbc::Signature) / sizeof(u32) +
                    sizeof(dxbc::SignatureParameterForGS) / sizeof(u32) * osgn_parameter_count);

  name_ptr = static_cast<u32>((shader_out.size() - osgn_position_dwords) * sizeof(u32));
  u32 osgn_name_ptr_texcoord = name_ptr;
  if (key.interpolator_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "TEXCOORD");
  }
  u32 osgn_name_ptr_xespritetexcoord = name_ptr;
  if (key.has_point_coordinates) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "XESPRITETEXCOORD");
  }
  u32 osgn_name_ptr_sv_position = name_ptr;
  name_ptr += dxbc::AppendAlignedString(shader_out, "SV_Position");
  u32 osgn_name_ptr_sv_clip_distance = name_ptr;
  if (input_clip_distance_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "SV_ClipDistance");
  }

  u32 output_register_interpolators = UINT32_MAX;
  u32 output_register_point_coordinates = UINT32_MAX;
  u32 output_register_position;
  u32 output_register_clip_distances = UINT32_MAX;
  {
    auto& osgn_header =
        *reinterpret_cast<dxbc::Signature*>(shader_out.data() + osgn_position_dwords);
    osgn_header.parameter_count = osgn_parameter_count;
    osgn_header.parameter_info_ptr = sizeof(dxbc::Signature);

    auto osgn_parameters = reinterpret_cast<dxbc::SignatureParameterForGS*>(
        shader_out.data() + osgn_position_dwords + sizeof(dxbc::Signature) / sizeof(u32));
    u32 osgn_parameter_index = 0;
    u32 output_register_index = 0;

    if (key.interpolator_count) {
      output_register_interpolators = output_register_index;
      for (u32 i = 0; i < key.interpolator_count; ++i) {
        REX_ASSERT(osgn_parameter_index < osgn_parameter_count);
        dxbc::SignatureParameterForGS& osgn_interpolator = osgn_parameters[osgn_parameter_index++];
        osgn_interpolator.semantic_name_ptr = osgn_name_ptr_texcoord;
        osgn_interpolator.semantic_index = i;
        osgn_interpolator.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        osgn_interpolator.register_index = output_register_index++;
        osgn_interpolator.mask = 0b1111;
      }
    }

    if (key.has_point_coordinates) {
      output_register_point_coordinates = output_register_index;
      REX_ASSERT(osgn_parameter_index < osgn_parameter_count);
      dxbc::SignatureParameterForGS& osgn_point_coordinates =
          osgn_parameters[osgn_parameter_index++];
      osgn_point_coordinates.semantic_name_ptr = osgn_name_ptr_xespritetexcoord;
      osgn_point_coordinates.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      osgn_point_coordinates.register_index = output_register_index++;
      osgn_point_coordinates.mask = 0b0011;
      osgn_point_coordinates.never_writes_mask = 0b1100;
    }

    output_register_position = output_register_index;
    REX_ASSERT(osgn_parameter_index < osgn_parameter_count);
    dxbc::SignatureParameterForGS& osgn_sv_position = osgn_parameters[osgn_parameter_index++];
    osgn_sv_position.semantic_name_ptr = osgn_name_ptr_sv_position;
    osgn_sv_position.system_value = dxbc::SystemValueName::kPosition;
    osgn_sv_position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
    osgn_sv_position.register_index = output_register_index++;
    osgn_sv_position.mask = 0b1111;

    if (input_clip_distance_count) {
      output_register_clip_distances = output_register_index;
      for (u32 i = 0; i < input_clip_distance_count; i += 4) {
        dxbc::SignatureParameterForGS& osgn_sv_clip_distance =
            osgn_parameters[osgn_parameter_index++];
        osgn_sv_clip_distance.semantic_name_ptr = osgn_name_ptr_sv_clip_distance;
        osgn_sv_clip_distance.semantic_index = i / 4;
        osgn_sv_clip_distance.system_value = dxbc::SystemValueName::kClipDistance;
        osgn_sv_clip_distance.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        osgn_sv_clip_distance.register_index = output_register_index++;
        u8 osgn_sv_clip_distance_mask =
            (UINT8_C(1) << std::min(input_clip_distance_count - i, UINT32_C(4))) - 1;
        osgn_sv_clip_distance.mask = osgn_sv_clip_distance_mask;
        osgn_sv_clip_distance.never_writes_mask = osgn_sv_clip_distance_mask ^ 0b1111;
      }
    }

    REX_ASSERT(osgn_parameter_index == osgn_parameter_count);
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kOutputSignatureForGS;
    blob_position_dwords = static_cast<u32>(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - dxbc::kBlobHeaderSizeDwords) * sizeof(u32) -
                             shader_out[blob_offset_position_dwords++];
  }

  shader_out[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 shex_position_dwords = blob_position_dwords + dxbc::kBlobHeaderSizeDwords;
  shader_out.resize(shex_position_dwords);

  shader_out.push_back(dxbc::VersionToken(dxbc::ProgramType::kGeometryShader, 5, 1));
  shader_out.push_back(0);

  dxbc::Statistics stat{};
  dxbc::Assembler a(shader_out, stat);

  a.OpDclGlobalFlags(dxbc::kGlobalFlagAllResourcesBound);

  if (system_cbuffer_size_vector_aligned_bytes) {
    a.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::kDcl, 0,
                      std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
                      std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants)),
        system_cbuffer_size_vector_aligned_bytes / (sizeof(u32) * 4));
  }

  dxbc::Primitive input_primitive = dxbc::Primitive::kUndefined;
  u32 input_primitive_vertex_count = 0;
  dxbc::PrimitiveTopology output_primitive_topology = dxbc::PrimitiveTopology::kUndefined;
  u32 max_output_vertex_count = 0;
  switch (key.type) {
    case PipelineGeometryShader::kPointList:
      input_primitive = dxbc::Primitive::kPoint;
      input_primitive_vertex_count = 1;
      output_primitive_topology = dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    case PipelineGeometryShader::kRectangleList:
      input_primitive = dxbc::Primitive::kTriangle;
      input_primitive_vertex_count = 3;
      output_primitive_topology = dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    case PipelineGeometryShader::kQuadList:
      input_primitive = dxbc::Primitive::kLineWithAdjacency;
      input_primitive_vertex_count = 4;
      output_primitive_topology = dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    default:
      REX_UNHANDLED_CASE(key.type);
  }

  REX_ASSERT(!(key.interpolator_count && input_register_interpolators == UINT32_MAX));
  for (u32 i = 0; i < key.interpolator_count; ++i) {
    a.OpDclInput(dxbc::Dest::V2D(input_primitive_vertex_count, input_register_interpolators + i));
  }
  a.OpDclInputSIV(dxbc::Dest::V2D(input_primitive_vertex_count, input_register_position),
                  dxbc::SystemValueName::kPosition);
  REX_ASSERT(!(input_clip_and_cull_distance_count &&
               input_register_clip_and_cull_distances == UINT32_MAX));
  for (u32 i = 0; i < input_clip_and_cull_distance_count; i += 4) {
    if (i < input_clip_distance_count) {
      a.OpDclInput(dxbc::Dest::V2D(
          input_primitive_vertex_count, input_register_clip_and_cull_distances + (i >> 2),
          (UINT32_C(1) << std::min(input_clip_distance_count - i, UINT32_C(4))) - 1));
    }
    if (input_cull_distance_count && i + 4 > input_clip_distance_count) {
      u32 cull_distance_mask =
          (UINT32_C(1) << std::min(input_clip_and_cull_distance_count - i, UINT32_C(4))) - 1;
      if (i < input_clip_distance_count) {
        cull_distance_mask &= ~((UINT32_C(1) << (input_clip_distance_count - i)) - 1);
      }
      a.OpDclInput(dxbc::Dest::V2D(input_primitive_vertex_count,
                                   input_register_clip_and_cull_distances + (i >> 2),
                                   cull_distance_mask));
    }
  }
  if (key.has_point_size && key.type == PipelineGeometryShader::kPointList) {
    REX_ASSERT(input_register_point_size != UINT32_MAX);
    a.OpDclInput(dxbc::Dest::V2D(input_primitive_vertex_count, input_register_point_size, 0b0001));
  }

  // At least 1 temporary register is needed to discard primitives with a NaN position.
  size_t dcl_temps_count_position_dwords = a.OpDclTemps(1);

  a.OpDclInputPrimitive(input_primitive);
  dxbc::Dest stream(dxbc::Dest::M(0));
  a.OpDclStream(stream);
  a.OpDclOutputTopology(output_primitive_topology);

  REX_ASSERT(!(key.interpolator_count && output_register_interpolators == UINT32_MAX));
  for (u32 i = 0; i < key.interpolator_count; ++i) {
    a.OpDclOutput(dxbc::Dest::O(output_register_interpolators + i));
  }
  if (key.has_point_coordinates) {
    REX_ASSERT(output_register_point_coordinates != UINT32_MAX);
    a.OpDclOutput(dxbc::Dest::O(output_register_point_coordinates, 0b0011));
  }
  a.OpDclOutputSIV(dxbc::Dest::O(output_register_position), dxbc::SystemValueName::kPosition);
  REX_ASSERT(!(input_clip_distance_count && output_register_clip_distances == UINT32_MAX));
  for (u32 i = 0; i < input_clip_distance_count; i += 4) {
    a.OpDclOutputSIV(
        dxbc::Dest::O(output_register_clip_distances + (i >> 2),
                      (UINT32_C(1) << std::min(input_clip_distance_count - i, UINT32_C(4))) - 1),
        dxbc::SystemValueName::kClipDistance);
  }

  a.OpDclMaxOutputVertexCount(max_output_vertex_count);

  // Discard the primitive if any vertex position is NaN, which emulates vertex kill with OR.
  for (u32 i = 0; i < input_primitive_vertex_count; ++i) {
    a.OpNE(dxbc::Dest::R(0), dxbc::Src::V2D(i, input_register_position),
           dxbc::Src::V2D(i, input_register_position));
    a.OpOr(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0, 0b0100), dxbc::Src::R(0, 0b1110));
    a.OpOr(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
           dxbc::Src::R(0, dxbc::Src::kYYYY));
    a.OpRetC(true, dxbc::Src::R(0, dxbc::Src::kXXXX));
  }

  // Cull if a cull distance is < 0 for all vertices, ps_ucp_mode 3 points cull per vertex later.
  if (input_cull_distance_count) {
    u32 cull_distance_start = point_recalculate_cull_distances ? point_user_cull_distance_count : 0;
    for (u32 i = cull_distance_start; i < input_cull_distance_count; ++i) {
      u32 cull_distance_register =
          input_register_clip_and_cull_distances + ((input_clip_distance_count + i) >> 2);
      u32 cull_distance_component = (input_clip_distance_count + i) & 3;
      a.OpLT(dxbc::Dest::R(0, 0b0001),
             dxbc::Src::V2D(0, cull_distance_register).Select(cull_distance_component),
             dxbc::Src::LF(0.0f));
      for (u32 j = 1; j < input_primitive_vertex_count; ++j) {
        a.OpLT(dxbc::Dest::R(0, 0b0010),
               dxbc::Src::V2D(j, cull_distance_register).Select(cull_distance_component),
               dxbc::Src::LF(0.0f));
        a.OpAnd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
                dxbc::Src::R(0, dxbc::Src::kYYYY));
      }
      a.OpRetC(true, dxbc::Src::R(0, dxbc::Src::kXXXX));
    }
  }

  switch (key.type) {
    case PipelineGeometryShader::kPointList: {
      dxbc::Src point_size_src(dxbc::Src::CB(
          0, std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
          offsetof(DXBCShaderTranslator::SystemConstants, point_constant_diameter) >> 4,
          ((offsetof(DXBCShaderTranslator::SystemConstants, point_constant_diameter[0]) >> 2) & 3) |
              (((offsetof(DXBCShaderTranslator::SystemConstants, point_constant_diameter[1]) >> 2) &
                3)
               << 2)));
      if (key.has_point_size) {
        // point_size defaults to -1.0, so a non negative value overrides the constant size.
        a.OpGE(dxbc::Dest::R(0, 0b0001),
               dxbc::Src::V2D(0, input_register_point_size, dxbc::Src::kXXXX), dxbc::Src::LF(0.0f));
        a.OpMovC(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0, dxbc::Src::kXXXX),
                 dxbc::Src::V2D(0, input_register_point_size, dxbc::Src::kXXXX), point_size_src);
        point_size_src = dxbc::Src::R(0, 0b0100);
      }
      // Drop zero size points early, as memexport may use point lists as compute.
      for (u32 i = 0; i < 2; ++i) {
        a.OpLT(dxbc::Dest::R(0, 0b0100), dxbc::Src::LF(0.0f), point_size_src.SelectFromSwizzled(i));
        a.OpRetC(false, dxbc::Src::R(0, dxbc::Src::kZZZZ));
      }
      // Convert the screen diameter to an NDC radius, then to clip space by multiplying by W.
      a.OpMul(dxbc::Dest::R(0, 0b0011), point_size_src,
              dxbc::Src::CB(
                  0, std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
                  offsetof(DXBCShaderTranslator::SystemConstants,
                           point_screen_diameter_to_ndc_radius) >>
                      4,
                  ((offsetof(DXBCShaderTranslator::SystemConstants,
                             point_screen_diameter_to_ndc_radius[0]) >>
                    2) &
                   3) |
                      (((offsetof(DXBCShaderTranslator::SystemConstants,
                                  point_screen_diameter_to_ndc_radius[1]) >>
                         2) &
                        3)
                       << 2)));
      point_size_src = dxbc::Src::R(0, 0b0100);
      a.OpMul(dxbc::Dest::R(0, 0b0011), point_size_src,
              dxbc::Src::V2D(0, input_register_position, dxbc::Src::kWWWW));
      dxbc::Src point_radius_x_src(point_size_src.SelectFromSwizzled(0));
      dxbc::Src point_radius_y_src(point_size_src.SelectFromSwizzled(1));

      if (point_recalculate_cull_distances) {
        stat.temp_register_count = std::max(UINT32_C(4), stat.temp_register_count);
        dxbc::Src ndc_scale_xy(dxbc::Src::CB(
            0, std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
            offsetof(DXBCShaderTranslator::SystemConstants, ndc_scale) >> 4,
            ((offsetof(DXBCShaderTranslator::SystemConstants, ndc_scale[0]) >> 2) & 3) |
                (((offsetof(DXBCShaderTranslator::SystemConstants, ndc_scale[1]) >> 2) & 3) << 2)));
        dxbc::Src ndc_scale_z(dxbc::Src::CB(
            0, std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
            offsetof(DXBCShaderTranslator::SystemConstants, ndc_scale) >> 4, dxbc::Src::kZZZZ));
        dxbc::Src ndc_offset_xy(dxbc::Src::CB(
            0, std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
            offsetof(DXBCShaderTranslator::SystemConstants, ndc_offset) >> 4,
            ((offsetof(DXBCShaderTranslator::SystemConstants, ndc_offset[0]) >> 2) & 3) |
                (((offsetof(DXBCShaderTranslator::SystemConstants, ndc_offset[1]) >> 2) & 3)
                 << 2)));
        dxbc::Src ndc_offset_z(dxbc::Src::CB(
            0, std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
            offsetof(DXBCShaderTranslator::SystemConstants, ndc_offset) >> 4, dxbc::Src::kZZZZ));
        for (u32 j = 0; j < point_user_cull_distance_count; ++j) {
          for (u32 i = 0; i < 4; ++i) {
            a.OpAdd(dxbc::Dest::R(2, 0b0001),
                    dxbc::Src::V2D(0, input_register_position, dxbc::Src::kXXXX),
                    (i & 1) ? point_radius_x_src : -point_radius_x_src);
            a.OpAdd(dxbc::Dest::R(2, 0b0010),
                    dxbc::Src::V2D(0, input_register_position, dxbc::Src::kYYYY),
                    (i >> 1) ? -point_radius_y_src : point_radius_y_src);
            a.OpMov(dxbc::Dest::R(2, 0b0100),
                    dxbc::Src::V2D(0, input_register_position, dxbc::Src::kZZZZ));
            a.OpMov(dxbc::Dest::R(2, 0b1000),
                    dxbc::Src::V2D(0, input_register_position, dxbc::Src::kWWWW));
            a.OpMAd(dxbc::Dest::R(2, 0b0011), -ndc_offset_xy, dxbc::Src::R(2, dxbc::Src::kWWWW),
                    dxbc::Src::R(2, 0b0011));
            a.OpDiv(dxbc::Dest::R(2, 0b0011), dxbc::Src::R(2, 0b0011), ndc_scale_xy);
            a.OpMAd(dxbc::Dest::R(2, 0b0100), -ndc_offset_z, dxbc::Src::R(2, dxbc::Src::kWWWW),
                    dxbc::Src::R(2, dxbc::Src::kZZZZ));
            a.OpDiv(dxbc::Dest::R(2, 0b0100), dxbc::Src::R(2, dxbc::Src::kZZZZ), ndc_scale_z);
            a.OpDP4(
                dxbc::Dest::R(2, 0b0001), dxbc::Src::R(2),
                dxbc::Src::CB(
                    0, std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
                    (offsetof(DXBCShaderTranslator::SystemConstants, user_clip_planes) +
                     sizeof(f32) * 4 * j) >>
                        4,
                    dxbc::Src::kXYZW));
            a.OpLT(dxbc::Dest::R(2, 0b0001), dxbc::Src::R(2, dxbc::Src::kXXXX),
                   dxbc::Src::LF(0.0f));
            if (i == 0) {
              a.OpMov(dxbc::Dest::R(3, 0b0001), dxbc::Src::R(2, dxbc::Src::kXXXX));
            } else {
              a.OpAnd(dxbc::Dest::R(3, 0b0001), dxbc::Src::R(3, dxbc::Src::kXXXX),
                      dxbc::Src::R(2, dxbc::Src::kXXXX));
            }
          }
          a.OpRetC(true, dxbc::Src::R(3, dxbc::Src::kXXXX));
        }
      }

      for (u32 i = 0; i < 4; ++i) {
        for (u32 j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                  dxbc::Src::V2D(0, input_register_interpolators + j));
        }
        // Order is TL, TR, BL, BR, bottom is -Y in NDC and +V in point coordinates.
        if (key.has_point_coordinates) {
          a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011),
                  dxbc::Src::LF(static_cast<f32>(i & 1), static_cast<f32>(i >> 1), 0.0f, 0.0f));
        }
        // o# only takes mov, so r0.zw is temporary as r0.xy holds the point size.
        a.OpAdd(dxbc::Dest::R(0, 0b0100),
                dxbc::Src::V2D(0, input_register_position, dxbc::Src::kXXXX),
                (i & 1) ? point_radius_x_src : -point_radius_x_src);
        a.OpAdd(dxbc::Dest::R(0, 0b1000),
                dxbc::Src::V2D(0, input_register_position, dxbc::Src::kYYYY),
                (i >> 1) ? -point_radius_y_src : point_radius_y_src);
        a.OpMov(dxbc::Dest::O(output_register_position, 0b0011), dxbc::Src::R(0, 0b1110));
        a.OpMov(dxbc::Dest::O(output_register_position, 0b1100),
                dxbc::Src::V2D(0, input_register_position));
        if (point_recalculate_clip_distances) {
          // Convert host clip space back to guest clip space before applying user clip planes.
          dxbc::Src ndc_scale_xy(dxbc::Src::CB(
              0, std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DXBCShaderTranslator::SystemConstants, ndc_scale) >> 4,
              ((offsetof(DXBCShaderTranslator::SystemConstants, ndc_scale[0]) >> 2) & 3) |
                  (((offsetof(DXBCShaderTranslator::SystemConstants, ndc_scale[1]) >> 2) & 3)
                   << 2)));
          dxbc::Src ndc_scale_z(dxbc::Src::CB(
              0, std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DXBCShaderTranslator::SystemConstants, ndc_scale) >> 4, dxbc::Src::kZZZZ));
          dxbc::Src ndc_offset_xy(dxbc::Src::CB(
              0, std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DXBCShaderTranslator::SystemConstants, ndc_offset) >> 4,
              ((offsetof(DXBCShaderTranslator::SystemConstants, ndc_offset[0]) >> 2) & 3) |
                  (((offsetof(DXBCShaderTranslator::SystemConstants, ndc_offset[1]) >> 2) & 3)
                   << 2)));
          dxbc::Src ndc_offset_z(dxbc::Src::CB(
              0, std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DXBCShaderTranslator::SystemConstants, ndc_offset) >> 4, dxbc::Src::kZZZZ));
          a.OpMov(dxbc::Dest::R(1, 0b0011), dxbc::Src::R(0, 0b1110));
          a.OpMov(dxbc::Dest::R(1, 0b0100),
                  dxbc::Src::V2D(0, input_register_position, dxbc::Src::kZZZZ));
          a.OpMov(dxbc::Dest::R(1, 0b1000),
                  dxbc::Src::V2D(0, input_register_position, dxbc::Src::kWWWW));
          a.OpMAd(dxbc::Dest::R(1, 0b0011), -ndc_offset_xy, dxbc::Src::R(1, dxbc::Src::kWWWW),
                  dxbc::Src::R(1, 0b0100));
          a.OpDiv(dxbc::Dest::R(1, 0b0011), dxbc::Src::R(1, 0b0100), ndc_scale_xy);
          a.OpMAd(dxbc::Dest::R(1, 0b0100), -ndc_offset_z, dxbc::Src::R(1, dxbc::Src::kWWWW),
                  dxbc::Src::R(1, dxbc::Src::kZZZZ));
          a.OpDiv(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(1, dxbc::Src::kZZZZ), ndc_scale_z);
          for (u32 j = 0; j < input_clip_distance_count; ++j) {
            a.OpDP4(
                dxbc::Dest::O(output_register_clip_distances + (j >> 2), UINT32_C(1) << (j & 3)),
                dxbc::Src::R(1),
                dxbc::Src::CB(
                    0, std::to_underlying(DXBCShaderTranslator::CbufferRegister::kSystemConstants),
                    (offsetof(DXBCShaderTranslator::SystemConstants, user_clip_planes) +
                     sizeof(f32) * 4 * j) >>
                        4,
                    dxbc::Src::kXYZW));
          }
        } else {
          for (u32 j = 0; j < input_clip_distance_count; j += 4) {
            a.OpMov(dxbc::Dest::O(
                        output_register_clip_distances + (j >> 2),
                        (UINT32_C(1) << std::min(input_clip_distance_count - j, UINT32_C(4))) - 1),
                    dxbc::Src::V2D(0, input_register_clip_and_cull_distances + (j >> 2)));
          }
        }
        a.OpEmitStream(stream);
      }
      a.OpCutStream(stream);
    } break;

    case PipelineGeometryShader::kRectangleList: {
      a.OpAdd(dxbc::Dest::R(0, 0b0011), dxbc::Src::V2D(2, input_register_position, 0b0100),
              -dxbc::Src::V2D(1, input_register_position, 0b0100));
      a.OpDP2(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, 0b0100), dxbc::Src::R(0, 0b0100));
      a.OpAdd(dxbc::Dest::R(0, 0b0110), dxbc::Src::V2D(0, input_register_position, 0b0100 << 2),
              -dxbc::Src::V2D(2, input_register_position, 0b0100 << 2));
      a.OpDP2(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, 0b1001), dxbc::Src::R(0, 0b1001));
      a.OpAdd(dxbc::Dest::R(0, 0b1100), dxbc::Src::V2D(1, input_register_position, 0b0100 << 4),
              -dxbc::Src::V2D(0, input_register_position, 0b0100 << 4));
      a.OpDP2(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, 0b1110), dxbc::Src::R(0, 0b1110));

      a.OpLT(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kYYYY),
             dxbc::Src::R(0, dxbc::Src::kXXXX));
      a.OpLT(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kZZZZ),
             dxbc::Src::R(0, dxbc::Src::kXXXX));
      a.OpAnd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kWWWW),
              dxbc::Src::R(0, dxbc::Src::kXXXX));
      a.OpIf(true, dxbc::Src::R(0, dxbc::Src::kXXXX));
      { a.OpMov(dxbc::Dest::R(0, 0b0111), dxbc::Src::LU(0, 1, 2, 0)); }
      a.OpElse();
      {
        a.OpLT(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kZZZZ),
               dxbc::Src::R(0, dxbc::Src::kYYYY));
        a.OpMovC(dxbc::Dest::R(0, 0b0111), dxbc::Src::R(0, dxbc::Src::kXXXX),
                 dxbc::Src::LU(1, 2, 0, 0), dxbc::Src::LU(2, 0, 1, 0));
      }
      a.OpEndIf();

      for (u32 i = 0; i < 3; ++i) {
        dxbc::Index input_vertex_index(0, i);
        for (u32 j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                  dxbc::Src::V2D(input_vertex_index, input_register_interpolators + j));
        }
        if (key.has_point_coordinates) {
          a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011), dxbc::Src::LF(0.0f));
        }
        a.OpMov(dxbc::Dest::O(output_register_position),
                dxbc::Src::V2D(input_vertex_index, input_register_position));
        for (u32 j = 0; j < input_clip_distance_count; j += 4) {
          a.OpMov(dxbc::Dest::O(
                      output_register_clip_distances + (j >> 2),
                      (UINT32_C(1) << std::min(input_clip_distance_count - j, UINT32_C(4))) - 1),
                  dxbc::Src::V2D(input_vertex_index,
                                 input_register_clip_and_cull_distances + (j >> 2)));
        }
        a.OpEmitStream(stream);
      }

      // r1 is temporary for the fourth vertex as o# only takes mov.
      stat.temp_register_count = std::max(UINT32_C(2), stat.temp_register_count);
      for (u32 j = 0; j < key.interpolator_count; ++j) {
        u32 input_register_interpolator = input_register_interpolators + j;
        a.OpAdd(dxbc::Dest::R(1), -dxbc::Src::V2D(dxbc::Index(0, 0), input_register_interpolator),
                dxbc::Src::V2D(dxbc::Index(0, 1), input_register_interpolator));
        a.OpAdd(dxbc::Dest::R(1), dxbc::Src::R(1),
                dxbc::Src::V2D(dxbc::Index(0, 2), input_register_interpolator));
        a.OpMov(dxbc::Dest::O(output_register_interpolators + j), dxbc::Src::R(1));
      }
      if (key.has_point_coordinates) {
        a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011), dxbc::Src::LF(0.0f));
      }
      a.OpAdd(dxbc::Dest::R(1), -dxbc::Src::V2D(dxbc::Index(0, 0), input_register_position),
              dxbc::Src::V2D(dxbc::Index(0, 1), input_register_position));
      a.OpAdd(dxbc::Dest::R(1), dxbc::Src::R(1),
              dxbc::Src::V2D(dxbc::Index(0, 2), input_register_position));
      a.OpMov(dxbc::Dest::O(output_register_position), dxbc::Src::R(1));
      for (u32 j = 0; j < input_clip_distance_count; j += 4) {
        u32 clip_distance_mask =
            (UINT32_C(1) << std::min(input_clip_distance_count - j, UINT32_C(4))) - 1;
        u32 input_register_clip_distance = input_register_clip_and_cull_distances + (j >> 2);
        a.OpAdd(dxbc::Dest::R(1, clip_distance_mask),
                -dxbc::Src::V2D(dxbc::Index(0, 0), input_register_clip_distance),
                dxbc::Src::V2D(dxbc::Index(0, 1), input_register_clip_distance));
        a.OpAdd(dxbc::Dest::R(1, clip_distance_mask), dxbc::Src::R(1),
                dxbc::Src::V2D(dxbc::Index(0, 2), input_register_clip_distance));
        a.OpMov(dxbc::Dest::O(output_register_clip_distances + (j >> 2), clip_distance_mask),
                dxbc::Src::R(1));
      }
      a.OpEmitStream(stream);
      a.OpCutStream(stream);
    } break;

    case PipelineGeometryShader::kQuadList: {
      // Triangle strip order is 0, 1, 3, 2 like GL_QUAD_STRIP.
      for (u32 i = 0; i < 4; ++i) {
        u32 input_vertex_index = i ^ (i >> 1);
        for (u32 j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                  dxbc::Src::V2D(input_vertex_index, input_register_interpolators + j));
        }
        if (key.has_point_coordinates) {
          a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011), dxbc::Src::LF(0.0f));
        }
        a.OpMov(dxbc::Dest::O(output_register_position),
                dxbc::Src::V2D(input_vertex_index, input_register_position));
        for (u32 j = 0; j < input_clip_distance_count; j += 4) {
          a.OpMov(dxbc::Dest::O(
                      output_register_clip_distances + (j >> 2),
                      (UINT32_C(1) << std::min(input_clip_distance_count - j, UINT32_C(4))) - 1),
                  dxbc::Src::V2D(input_vertex_index,
                                 input_register_clip_and_cull_distances + (j >> 2)));
        }
        a.OpEmitStream(stream);
      }
      a.OpCutStream(stream);
    } break;

    default:
      REX_UNHANDLED_CASE(key.type);
  }

  a.OpRet();

  shader_out[dcl_temps_count_position_dwords] = stat.temp_register_count;

  shader_out[shex_position_dwords + 1] = static_cast<u32>(shader_out.size()) - shex_position_dwords;

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = static_cast<u32>(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - dxbc::kBlobHeaderSizeDwords) * sizeof(u32) -
                             shader_out[blob_offset_position_dwords++];
  }

  shader_out[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 stat_position_dwords = blob_position_dwords + dxbc::kBlobHeaderSizeDwords;
  shader_out.resize(stat_position_dwords + sizeof(dxbc::Statistics) / sizeof(u32));
  std::memcpy(shader_out.data() + stat_position_dwords, &stat, sizeof(dxbc::Statistics));

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = static_cast<u32>(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - dxbc::kBlobHeaderSizeDwords) * sizeof(u32) -
                             shader_out[blob_offset_position_dwords++];
  }

  u32 shader_size_bytes = static_cast<u32>(shader_out.size() * sizeof(u32));
  {
    auto& container_header = *reinterpret_cast<dxbc::ContainerHeader*>(shader_out.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = shader_size_bytes;
    container_header.blob_count = dxbc::kBlobCount;
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(shader_out.data()),
                          static_cast<unsigned int>(shader_size_bytes), container_header.hash);
  }
}

const std::vector<u32>& PipelineCache::GetGeometryShader(GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key);
  if (it != geometry_shaders_.end()) {
    return it->second;
  }
  std::vector<u32> shader;
  CreateDXBCGeometryShader(key, shader);
  return geometry_shaders_.emplace(key, std::move(shader)).first->second;
}

ID3D12PipelineState* PipelineCache::CreateD3D12Pipeline(
    const PipelineRuntimeDescription& runtime_description) {
  const PipelineDescription& description = runtime_description.description;

  if (runtime_description.pixel_shader != nullptr) {
    REXLOG_DEBUG(gpu, "Creating graphics pipeline with VS {:016X}, PS {:016X}",
                 runtime_description.vertex_shader->GetShader().GetUcodeDataHash(),
                 runtime_description.pixel_shader->GetShader().GetUcodeDataHash());
  } else {
    REXLOG_DEBUG(gpu, "Creating graphics pipeline with VS {:016X}",
                 runtime_description.vertex_shader->GetShader().GetUcodeDataHash());
  }

  auto state_desc = objects::BuildGraphicsPipelineStateDesc();

  bool edram_rov_used =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  state_desc.pRootSignature = runtime_description.root_signature;

  switch (description.strip_cut_index) {
    case PipelineStripCutIndex::kFFFF:
      state_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
      break;
    case PipelineStripCutIndex::kFFFFFFFF:
      state_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
      break;
    default:
      state_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
      break;
  }

  if (!runtime_description.vertex_shader->IsTranslated()) {
    REXLOG_ERROR(gpu, "Vertex shader {:016X} not translated",
                 runtime_description.vertex_shader->GetShader().GetUcodeDataHash());
    REX_ASSERT_ALWAYS();
    return nullptr;
  }
  Shader::HostVertexShaderType host_vertex_shader_type =
      DXBCShaderTranslator::Modification(runtime_description.vertex_shader->GetModification())
          .vertex.host_vertex_shader_type;
  if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
    state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    xenos::TessellationMode tessellation_mode = static_cast<xenos::TessellationMode>(
        description.primitive_topology_type_or_tessellation_mode);
    if (tessellation_mode == xenos::TessellationMode::kAdaptive) {
      state_desc.VS.pShaderBytecode = shaders::kTessellationAdaptiveVS;
      state_desc.VS.BytecodeLength = sizeof(shaders::kTessellationAdaptiveVS);
    } else {
      state_desc.VS.pShaderBytecode = shaders::kTessellationIndexedVS;
      state_desc.VS.BytecodeLength = sizeof(shaders::kTessellationIndexedVS);
    }
    switch (tessellation_mode) {
      case xenos::TessellationMode::kDiscrete:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::kDiscreteTriangle3CPHS;
            state_desc.HS.BytecodeLength = sizeof(shaders::kDiscreteTriangle3CPHS);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::kDiscreteTriangle1CPHS;
            state_desc.HS.BytecodeLength = sizeof(shaders::kDiscreteTriangle1CPHS);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::kDiscreteQuad4CPHS;
            state_desc.HS.BytecodeLength = sizeof(shaders::kDiscreteQuad4CPHS);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::kDiscreteQuad1CPHS;
            state_desc.HS.BytecodeLength = sizeof(shaders::kDiscreteQuad1CPHS);
            break;
          default:
            REX_UNHANDLED_CASE(host_vertex_shader_type);
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kContinuous:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::kContinuousTriangle3CPHS;
            state_desc.HS.BytecodeLength = sizeof(shaders::kContinuousTriangle3CPHS);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::kContinuousTriangle1CPHS;
            state_desc.HS.BytecodeLength = sizeof(shaders::kContinuousTriangle1CPHS);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::kContinuousQuad4CPHS;
            state_desc.HS.BytecodeLength = sizeof(shaders::kContinuousQuad4CPHS);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::kContinuousQuad1CPHS;
            state_desc.HS.BytecodeLength = sizeof(shaders::kContinuousQuad1CPHS);
            break;
          default:
            REX_UNHANDLED_CASE(host_vertex_shader_type);
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kAdaptive:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::kAdaptiveTriangleHS;
            state_desc.HS.BytecodeLength = sizeof(shaders::kAdaptiveTriangleHS);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::kAdaptiveQuadHS;
            state_desc.HS.BytecodeLength = sizeof(shaders::kAdaptiveQuadHS);
            break;
          default:
            REX_UNHANDLED_CASE(host_vertex_shader_type);
            return nullptr;
        }
        break;
      default:
        REX_UNHANDLED_CASE(tessellation_mode);
        return nullptr;
    }
    state_desc.DS.pShaderBytecode = runtime_description.vertex_shader->GetTranslatedBinary().data();
    state_desc.DS.BytecodeLength = runtime_description.vertex_shader->GetTranslatedBinary().size();
  } else {
    REX_ASSERT(host_vertex_shader_type == Shader::HostVertexShaderType::kVertex);
    if (host_vertex_shader_type != Shader::HostVertexShaderType::kVertex) {
      return nullptr;
    }
    state_desc.VS.pShaderBytecode = runtime_description.vertex_shader->GetTranslatedBinary().data();
    state_desc.VS.BytecodeLength = runtime_description.vertex_shader->GetTranslatedBinary().size();
    PipelinePrimitiveTopologyType primitive_topology_type =
        static_cast<PipelinePrimitiveTopologyType>(
            description.primitive_topology_type_or_tessellation_mode);
    switch (primitive_topology_type) {
      case PipelinePrimitiveTopologyType::kPoint:
        state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        break;
      case PipelinePrimitiveTopologyType::kLine:
        state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
      case PipelinePrimitiveTopologyType::kTriangle:
        state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
      default:
        REX_UNHANDLED_CASE(primitive_topology_type);
        return nullptr;
    }
  }

  if (runtime_description.pixel_shader != nullptr) {
    if (!runtime_description.pixel_shader->IsTranslated()) {
      REXLOG_ERROR(gpu, "Pixel shader {:016X} not translated",
                   runtime_description.pixel_shader->GetShader().GetUcodeDataHash());
      REX_ASSERT_ALWAYS();
      return nullptr;
    }
    state_desc.PS.pShaderBytecode = runtime_description.pixel_shader->GetTranslatedBinary().data();
    state_desc.PS.BytecodeLength = runtime_description.pixel_shader->GetTranslatedBinary().size();
  } else if (edram_rov_used) {
    state_desc.PS.pShaderBytecode = depth_only_pixel_shader_.data();
    state_desc.PS.BytecodeLength = depth_only_pixel_shader_.size();
  } else {
    if (render_target_cache_.IsDepthFloat24ConvertedInPixelShader() &&
        (description.depth_func != xenos::CompareFunction::kAlways || description.depth_write) &&
        description.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      if (render_target_cache_.IsDepthFloat24Rounded()) {
        state_desc.PS.pShaderBytecode = shaders::kFloat24RoundPS;
        state_desc.PS.BytecodeLength = sizeof(shaders::kFloat24RoundPS);
      } else {
        state_desc.PS.pShaderBytecode = shaders::kFloat24TruncatePS;
        state_desc.PS.BytecodeLength = sizeof(shaders::kFloat24TruncatePS);
      }
    }
  }

  if (runtime_description.geometry_shader != nullptr) {
    state_desc.GS.pShaderBytecode = runtime_description.geometry_shader->data();
    state_desc.GS.BytecodeLength = sizeof(*runtime_description.geometry_shader->data()) *
                                   runtime_description.geometry_shader->size();
  }

  state_desc.RasterizerState.FillMode =
      description.fill_mode_wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
  switch (description.cull_mode) {
    case PipelineCullMode::kFront:
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
      break;
    case PipelineCullMode::kBack:
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
      break;
    default:
      REX_ASSERT(description.cull_mode == PipelineCullMode::kNone ||
                 description.cull_mode == PipelineCullMode::kDisableRasterization);
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      break;
  }
  state_desc.RasterizerState.FrontCounterClockwise =
      description.front_counter_clockwise ? TRUE : FALSE;
  state_desc.RasterizerState.DepthBias = description.depth_bias;
  state_desc.RasterizerState.DepthBiasClamp = 0.0f;
  // Non square resolution scaling takes the larger scale, as too much bias beats Z fighting.
  state_desc.RasterizerState.SlopeScaledDepthBias =
      description.depth_bias_slope_scaled *
      static_cast<f32>(std::max(render_target_cache_.GetDrawResolutionScaleX(),
                                render_target_cache_.GetDrawResolutionScaleY()));
  state_desc.RasterizerState.DepthClipEnable = description.depth_clip ? TRUE : FALSE;
  u32 msaa_sample_count = static_cast<u32>(1) << static_cast<u32>(description.host_msaa_samples);
  if (edram_rov_used) {
    REX_ASSERT(msaa_sample_count == 1 || msaa_sample_count == 4);
    if (msaa_sample_count != 1 && msaa_sample_count != 4) {
      return nullptr;
    }
    state_desc.RasterizerState.ForcedSampleCount =
        static_cast<u32>(1) << static_cast<u32>(description.host_msaa_samples);
  }

  state_desc.SampleMask = UINT_MAX;
  if (edram_rov_used) {
    state_desc.SampleDesc.Count = 1;
  } else {
    REX_ASSERT(msaa_sample_count <= 4);
    if (msaa_sample_count > 4) {
      return nullptr;
    }
    if (msaa_sample_count == 2 && !render_target_cache_.IsMSAA2xSupported()) {
      state_desc.SampleMask = 0b1001;
      state_desc.SampleDesc.Count = 4;
    } else {
      state_desc.SampleDesc.Count = msaa_sample_count;
    }
  }

  if (!edram_rov_used) {
    if (description.depth_func != xenos::CompareFunction::kAlways || description.depth_write) {
      state_desc.DepthStencilState.DepthEnable = TRUE;
      state_desc.DepthStencilState.DepthWriteMask =
          description.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
      // Comparison functions map to the Direct3D 12 ones plus 1.
      state_desc.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(
          static_cast<u32>(D3D12_COMPARISON_FUNC_NEVER) + static_cast<u32>(description.depth_func));
    }
    if (description.stencil_enable) {
      state_desc.DepthStencilState.StencilEnable = TRUE;
      state_desc.DepthStencilState.StencilReadMask = description.stencil_read_mask;
      state_desc.DepthStencilState.StencilWriteMask = description.stencil_write_mask;
      // Stencil operations map to the Direct3D 12 ones plus 1.
      state_desc.DepthStencilState.FrontFace.StencilFailOp =
          static_cast<D3D12_STENCIL_OP>(static_cast<u32>(D3D12_STENCIL_OP_KEEP) +
                                        static_cast<u32>(description.stencil_front_fail_op));
      state_desc.DepthStencilState.FrontFace.StencilDepthFailOp =
          static_cast<D3D12_STENCIL_OP>(static_cast<u32>(D3D12_STENCIL_OP_KEEP) +
                                        static_cast<u32>(description.stencil_front_depth_fail_op));
      state_desc.DepthStencilState.FrontFace.StencilPassOp =
          static_cast<D3D12_STENCIL_OP>(static_cast<u32>(D3D12_STENCIL_OP_KEEP) +
                                        static_cast<u32>(description.stencil_front_pass_op));
      state_desc.DepthStencilState.FrontFace.StencilFunc =
          static_cast<D3D12_COMPARISON_FUNC>(static_cast<u32>(D3D12_COMPARISON_FUNC_NEVER) +
                                             static_cast<u32>(description.stencil_front_func));
      state_desc.DepthStencilState.BackFace.StencilFailOp =
          static_cast<D3D12_STENCIL_OP>(static_cast<u32>(D3D12_STENCIL_OP_KEEP) +
                                        static_cast<u32>(description.stencil_back_fail_op));
      state_desc.DepthStencilState.BackFace.StencilDepthFailOp =
          static_cast<D3D12_STENCIL_OP>(static_cast<u32>(D3D12_STENCIL_OP_KEEP) +
                                        static_cast<u32>(description.stencil_back_depth_fail_op));
      state_desc.DepthStencilState.BackFace.StencilPassOp =
          static_cast<D3D12_STENCIL_OP>(static_cast<u32>(D3D12_STENCIL_OP_KEEP) +
                                        static_cast<u32>(description.stencil_back_pass_op));
      state_desc.DepthStencilState.BackFace.StencilFunc =
          static_cast<D3D12_COMPARISON_FUNC>(static_cast<u32>(D3D12_COMPARISON_FUNC_NEVER) +
                                             static_cast<u32>(description.stencil_back_func));
    }
    if (state_desc.DepthStencilState.DepthEnable || state_desc.DepthStencilState.StencilEnable) {
      state_desc.DSVFormat =
          D3D12RenderTargetCache::GetDepthDSVDXGIFormat(description.depth_format);
    }

    state_desc.BlendState.IndependentBlendEnable = TRUE;
    static const D3D12_BLEND kBlendFactorMap[] = {
        D3D12_BLEND_ZERO,          D3D12_BLEND_ONE,
        D3D12_BLEND_SRC_COLOR,     D3D12_BLEND_INV_SRC_COLOR,
        D3D12_BLEND_SRC_ALPHA,     D3D12_BLEND_INV_SRC_ALPHA,
        D3D12_BLEND_DEST_COLOR,    D3D12_BLEND_INV_DEST_COLOR,
        D3D12_BLEND_DEST_ALPHA,    D3D12_BLEND_INV_DEST_ALPHA,
        D3D12_BLEND_BLEND_FACTOR,  D3D12_BLEND_INV_BLEND_FACTOR,
        D3D12_BLEND_SRC_ALPHA_SAT,
    };
    // 8 entries because 3 bits from the guest index the map directly.
    static const D3D12_BLEND_OP kBlendOpMap[] = {
        D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT,     D3D12_BLEND_OP_MIN,
        D3D12_BLEND_OP_MAX, D3D12_BLEND_OP_REV_SUBTRACT, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_ADD};
    for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      const PipelineRenderTarget& rt = description.render_targets[i];
      if (!rt.used) {
        // Null RTV descriptors are valid for DXGI_FORMAT_UNKNOWN slots.
        state_desc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
        continue;
      }
      state_desc.NumRenderTargets = i + 1;
      state_desc.RTVFormats[i] = render_target_cache_.GetColorDrawDXGIFormat(rt.format);
      if (state_desc.RTVFormats[i] == DXGI_FORMAT_UNKNOWN) {
        REX_ASSERT_ALWAYS();
        return nullptr;
      }
      D3D12_RENDER_TARGET_BLEND_DESC& blend_desc = state_desc.BlendState.RenderTarget[i];
      if (rt.src_blend != PipelineBlendFactor::kOne ||
          rt.dest_blend != PipelineBlendFactor::kZero || rt.blend_op != xenos::BlendOp::kAdd ||
          rt.src_blend_alpha != PipelineBlendFactor::kOne ||
          rt.dest_blend_alpha != PipelineBlendFactor::kZero ||
          rt.blend_op_alpha != xenos::BlendOp::kAdd) {
        blend_desc.BlendEnable = TRUE;
        blend_desc.SrcBlend = kBlendFactorMap[static_cast<u32>(rt.src_blend)];
        blend_desc.DestBlend = kBlendFactorMap[static_cast<u32>(rt.dest_blend)];
        blend_desc.BlendOp = kBlendOpMap[static_cast<u32>(rt.blend_op)];
        blend_desc.SrcBlendAlpha = kBlendFactorMap[static_cast<u32>(rt.src_blend_alpha)];
        blend_desc.DestBlendAlpha = kBlendFactorMap[static_cast<u32>(rt.dest_blend_alpha)];
        blend_desc.BlendOpAlpha = kBlendOpMap[static_cast<u32>(rt.blend_op_alpha)];
      }
      blend_desc.RenderTargetWriteMask = rt.write_mask;
    }
  }

  // Rasterization is disabled the Direct3D way, with no pixel shader and no depth or stencil.
  if (description.cull_mode == PipelineCullMode::kDisableRasterization) {
    state_desc.PS.pShaderBytecode = nullptr;
    state_desc.PS.BytecodeLength = 0;
    state_desc.DepthStencilState.DepthEnable = FALSE;
    state_desc.DepthStencilState.StencilEnable = FALSE;
  }

  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  ID3D12PipelineState* state;
  if (FAILED(device->CreateGraphicsPipelineState(&state_desc, IID_PPV_ARGS(&state)))) {
    if (runtime_description.pixel_shader != nullptr) {
      REXLOG_ERROR(gpu, "Failed to create graphics pipeline with VS {:016X}, PS {:016X}",
                   runtime_description.vertex_shader->GetShader().GetUcodeDataHash(),
                   runtime_description.pixel_shader->GetShader().GetUcodeDataHash());
    } else {
      REXLOG_ERROR(gpu, "Failed to create graphics pipeline with VS {:016X}",
                   runtime_description.vertex_shader->GetShader().GetUcodeDataHash());
    }
    return nullptr;
  }
  std::u16string name;
  if (runtime_description.pixel_shader != nullptr) {
    name = rex::ToUTF16(std::format(
        "VS {:016X}, PS {:016X}", runtime_description.vertex_shader->GetShader().GetUcodeDataHash(),
        runtime_description.pixel_shader->GetShader().GetUcodeDataHash()));
  } else {
    name = rex::ToUTF16(std::format(
        "VS {:016X}", runtime_description.vertex_shader->GetShader().GetUcodeDataHash()));
  }
  state->SetName(reinterpret_cast<LPCWSTR>(name.c_str()));
  return state;
}

void PipelineCache::StorageWriteThread() {
  ShaderStoredHeader shader_header;
  // Zero the unused bits so nothing leaks into the file.
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
        std::memcpy(&pipeline_description, &storage_.write_pipeline_queue.front(),
                    sizeof(pipeline_description));
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
        // Swap back, as the hash is calculated over the guest endian ucode.
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

bool PipelineCache::PrepareRuntimeDescriptionForQueuedCreation(
    Pipeline* pipeline, PipelineRuntimeDescription& runtime_description) {
  REX_ASSERT(pipeline != nullptr);
  std::memcpy(&runtime_description, &pipeline->description, sizeof(runtime_description));
  runtime_description.root_signature = pipeline->root_signature.load(std::memory_order_acquire);

  auto translate_pending_shader = [this](D3D12Shader::D3D12Translation* translation,
                                         const char* shader_type) -> bool {
    if (!translation) {
      return true;
    }
    if (!translation->IsTranslated()) {
      std::lock_guard<std::mutex> lock(translation_request_lock_);
      if (!translation->IsTranslated()) {
        translation->GetShader().AnalyzeUcode(ucode_disasm_buffer_);
        if (!TranslateAnalyzedShader(*shader_translator_, *translation, dxbc_converter_, dxc_utils_,
                                     dxc_compiler_)) {
          REX_FAIL(gpu, false, "Failed to translate queued {} shader", shader_type);
        }
        if (storage_.shader_file.is_open() &&
            translation->GetShader().GetUcodeStorageIndex() != storage_.shader_index) {
          translation->GetShader().SetUcodeStorageIndex(storage_.shader_index);
          REX_ASSERT(storage_.write_thread != nullptr);
          storage_.shader_file_flush_needed = true;
          {
            std::lock_guard<std::mutex> storage_lock(storage_.write_request_lock);
            storage_.write_shader_queue.push_back(&translation->GetShader());
          }
          storage_.write_request_cond.notify_all();
        }
      }
    }
    return translation->IsValid();
  };

  if (pipeline->pending_vertex_shader) {
    D3D12Shader::D3D12Translation* pending_vertex = pipeline->pending_vertex_shader;
    pipeline->pending_vertex_shader = nullptr;
    if (!translate_pending_shader(pending_vertex, "vertex")) {
      return false;
    }
  }

  if (pipeline->pending_pixel_shader) {
    D3D12Shader::D3D12Translation* pending_pixel = pipeline->pending_pixel_shader;
    pipeline->pending_pixel_shader = nullptr;
    if (!translate_pending_shader(pending_pixel, "pixel")) {
      return false;
    }
    bool tessellated = Shader::IsHostVertexShaderTypeDomain(
        DXBCShaderTranslator::Modification(runtime_description.vertex_shader->GetModification())
            .vertex.host_vertex_shader_type);
    ID3D12RootSignature* root_signature = command_processor_.GetRootSignature(
        static_cast<const DXBCShader*>(&runtime_description.vertex_shader->GetShader()),
        static_cast<const DXBCShader*>(&pending_pixel->GetShader()), tessellated);
    if (!root_signature) {
      return false;
    }
    runtime_description.root_signature = root_signature;
    pipeline->root_signature.store(root_signature, std::memory_order_release);
  }

  return true;
}

void PipelineCache::CreationThread(size_t thread_index) {
  while (true) {
    Pipeline* pipeline_to_create = nullptr;

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
      // The busy count stays raised until creation completes so the event is not set early.
      pipeline_to_create = creation_queue_.top();
      creation_queue_.pop();
      ++creation_threads_busy_;
    }

    PipelineRuntimeDescription runtime_description;
    if (!PrepareRuntimeDescriptionForQueuedCreation(pipeline_to_create, runtime_description)) {
      pipeline_to_create->state.store(nullptr, std::memory_order_release);
    } else {
      pipeline_to_create->state.store(CreateD3D12Pipeline(runtime_description),
                                      std::memory_order_release);
    }

    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      --creation_threads_busy_;
    }
  }
}

void PipelineCache::CreateQueuedPipelinesOnProcessorThread() {
  REX_ASSERT(!creation_threads_.empty());
  while (true) {
    Pipeline* pipeline_to_create;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      if (creation_queue_.empty()) {
        break;
      }
      pipeline_to_create = creation_queue_.top();
      creation_queue_.pop();
    }
    PipelineRuntimeDescription runtime_description;
    if (!PrepareRuntimeDescriptionForQueuedCreation(pipeline_to_create, runtime_description)) {
      pipeline_to_create->state.store(nullptr, std::memory_order_release);
      continue;
    }
    pipeline_to_create->state.store(CreateD3D12Pipeline(runtime_description),
                                    std::memory_order_release);
  }
}

}  // namespace rex::gpu::d3d12
