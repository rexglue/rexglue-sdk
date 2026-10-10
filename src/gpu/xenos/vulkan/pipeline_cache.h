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

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gpu/xenos/pipeline/shader/spirv_translator.h"
#include <rex/hash.h>
#include <rex/platform.h>
#include <rex/platform/thread.h>
#include "gpu/xenos/primitive_processor.h"
#include <rex/xenos/register_file.h>
#include <rex/xenos/registers.h>
#include "gpu/xenos/vulkan/render_target_cache.h"
#include "gpu/xenos/vulkan/shader.h"
#include <rex/xenos/xenos.h>
#include "gpu/xenos/host/vulkan/api.h"

namespace rex::gpu::vulkan {

class VulkanCommandProcessor;

class VulkanPipelineCache {
 public:
  static constexpr size_t kLayoutUIDEmpty = 0;

  class PipelineLayoutProvider {
   public:
    virtual ~PipelineLayoutProvider() {}
    virtual VkPipelineLayout GetPipelineLayout() const = 0;

   protected:
    PipelineLayoutProvider() = default;
  };

  VulkanPipelineCache(VulkanCommandProcessor& command_processor,
                      const xenos::RegisterFile& register_file,
                      VulkanRenderTargetCache& render_target_cache,
                      VkShaderStageFlags guest_shader_vertex_stages);
  ~VulkanPipelineCache();

  bool Initialize();
  void Shutdown();
  void InitializeShaderStorage(const std::filesystem::path& cache_root, u32 title_id,
                               bool blocking);
  void ShutdownShaderStorage();
  void EndSubmission();

  VulkanShader* LoadShader(xenos::ShaderType shader_type, const u32* host_address, u32 dword_count);
  // Analyze shader microcode on the translator thread.
  void AnalyzeShaderUcode(Shader& shader) { shader.AnalyzeUcode(ucode_disasm_buffer_); }

  // The shader must have microcode analyzed.
  SpirvShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
      u32 interpolator_mask, bool ps_param_gen_used) const;
  SpirvShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, u32 interpolator_mask, u32 param_gen_pos,
      xenos::reg::RB_DEPTHCONTROL normalized_depth_control) const;

  bool EnsureShadersTranslated(VulkanShader::VulkanTranslation* vertex_shader,
                               VulkanShader::VulkanTranslation* pixel_shader);
  bool ConfigurePipeline(
      VulkanShader::VulkanTranslation* vertex_shader, VulkanShader::VulkanTranslation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      xenos::reg::RB_DEPTHCONTROL normalized_depth_control, u32 normalized_color_mask,
      VulkanRenderTargetCache::RenderPassKey render_pass_key, VkPipeline& pipeline_out,
      const PipelineLayoutProvider*& pipeline_layout_out, void** pipeline_handle_out = nullptr);
  bool IsCreatingPipelines() const;
  void GetPipelineAndLayoutByHandle(void* handle, VkPipeline& pipeline_out,
                                    const PipelineLayoutProvider*& pipeline_layout_out,
                                    bool* is_placeholder_out = nullptr) const;

 private:
  REX_PACKED_STRUCT(ShaderStoredHeader, {
    u64 ucode_data_hash;

    u32 ucode_dword_count : 31;
    xenos::ShaderType type : 1;

    static constexpr u32 kVersion = 0x20261017;
  });

  enum class PipelineGeometryShader : u32 {
    kNone,
    kPointList,
    kRectangleList,
    kQuadList,
  };

  enum class PipelinePrimitiveTopology : u32 {
    kPointList,
    kLineList,
    kLineStrip,
    kTriangleList,
    kTriangleStrip,
    kTriangleFan,
    kLineListWithAdjacency,
    kPatchList,
  };

  enum class PipelinePolygonMode : u32 {
    kFill,
    kLine,
    kPoint,
  };

  enum class PipelineBlendFactor : u32 {
    kZero,
    kOne,
    kSrcColor,
    kOneMinusSrcColor,
    kDstColor,
    kOneMinusDstColor,
    kSrcAlpha,
    kOneMinusSrcAlpha,
    kDstAlpha,
    kOneMinusDstAlpha,
    kConstantColor,
    kOneMinusConstantColor,
    kConstantAlpha,
    kOneMinusConstantAlpha,
    kSrcAlphaSaturate,
  };

  // Update PipelineDescription::kVersion if anything is changed!
  REX_PACKED_STRUCT(PipelineRenderTarget, {
    PipelineBlendFactor src_color_blend_factor : 4;
    PipelineBlendFactor dst_color_blend_factor : 4;
    xenos::BlendOp color_blend_op : 3;
    PipelineBlendFactor src_alpha_blend_factor : 4;
    PipelineBlendFactor dst_alpha_blend_factor : 4;
    xenos::BlendOp alpha_blend_op : 3;
    u32 color_write_mask : 4;
    u32 padding : 6;
  });
  static_assert(std::is_trivially_copyable_v<PipelineRenderTarget> &&
                std::has_unique_object_representations_v<PipelineRenderTarget>);

  REX_PACKED_STRUCT(PipelineDescription, {
    u64 vertex_shader_hash;
    u64 vertex_shader_modification;
    /** Zero when there is no pixel shader. */
    u64 pixel_shader_hash;
    u64 pixel_shader_modification;
    VulkanRenderTargetCache::RenderPassKey render_pass_key;

    PipelineGeometryShader geometry_shader : 2;
    PipelinePrimitiveTopology primitive_topology : 3;
    u32 primitive_restart : 1;
    xenos::TessellationMode tessellation_mode : 2;
    u32 depth_clamp_enable : 1;
    PipelinePolygonMode polygon_mode : 2;
    u32 cull_front : 1;
    u32 cull_back : 1;
    u32 front_face_clockwise : 1;
    u32 rasterizer_discard : 1;
    u32 depth_write_enable : 1;
    xenos::CompareFunction depth_compare_op : 3;
    u32 stencil_test_enable : 1;
    xenos::StencilOp stencil_front_fail_op : 3;
    xenos::StencilOp stencil_front_pass_op : 3;
    xenos::StencilOp stencil_front_depth_fail_op : 3;
    xenos::CompareFunction stencil_front_compare_op : 3;
    xenos::StencilOp stencil_back_fail_op : 3;

    xenos::StencilOp stencil_back_pass_op : 3;
    xenos::StencilOp stencil_back_depth_fail_op : 3;
    xenos::CompareFunction stencil_back_compare_op : 3;
    u32 sample_rate_shading : 1;
    u32 padding : 19;

    /** Filled only for the attachments present in the render pass object. */
    PipelineRenderTarget render_targets[xenos::kMaxColorRenderTargets];

    static constexpr u32 kVersion = 0x20260228;
    PipelineDescription() {
      Reset();
    }
    bool operator==(const PipelineDescription& description) const {
      return std::memcmp(this, &description, sizeof(*this)) == 0;
    }
    void Reset() {
      std::memset(this, 0, sizeof(*this));
    }
    u64 GetHash() const {
      return XXH3_64bits(this, sizeof(*this));
    }
    struct Hasher {
      size_t operator()(const PipelineDescription& description) const {
        return static_cast<size_t>(description.GetHash());
      }
    };
  });
  static_assert(std::is_trivially_copyable_v<PipelineDescription> &&
                std::has_unique_object_representations_v<PipelineDescription>);
  REX_PACKED_STRUCT(PipelineStoredDescription, {
    u64 description_hash;
    PipelineDescription description;
  });
  static_assert(std::is_trivially_copyable_v<PipelineStoredDescription> &&
                std::has_unique_object_representations_v<PipelineStoredDescription>);

  struct Pipeline {
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    /** Owned by VulkanCommandProcessor, which must keep it alive while the cache is active. */
    std::atomic<const PipelineLayoutProvider*> pipeline_layout{nullptr};
    std::atomic<bool> is_placeholder{false};
    Pipeline() = default;
    Pipeline(const PipelineLayoutProvider* pipeline_layout_provider)
        : pipeline_layout(pipeline_layout_provider) {}
    Pipeline(const Pipeline& other)
        : pipeline(other.pipeline.load(std::memory_order_acquire)),
          pipeline_layout(other.pipeline_layout.load(std::memory_order_acquire)),
          is_placeholder(other.is_placeholder.load(std::memory_order_acquire)) {}
    Pipeline& operator=(const Pipeline& other) {
      if (this == &other) {
        return *this;
      }
      pipeline.store(other.pipeline.load(std::memory_order_acquire), std::memory_order_release);
      pipeline_layout.store(other.pipeline_layout.load(std::memory_order_acquire),
                            std::memory_order_release);
      is_placeholder.store(other.is_placeholder.load(std::memory_order_acquire),
                           std::memory_order_release);
      return *this;
    }
  };

  /** Creation request passed from the command processor thread to the creation threads. */
  struct PipelineCreationArguments {
    u8 priority = 0;
    std::pair<const PipelineDescription, Pipeline>* pipeline = nullptr;
    const PipelineLayoutProvider* pipeline_layout = nullptr;
    const VulkanShader::VulkanTranslation* vertex_shader = nullptr;
    const VulkanShader::VulkanTranslation* pixel_shader = nullptr;
    VkShaderModule tessellation_vertex_shader = VK_NULL_HANDLE;
    VkShaderModule tessellation_control_shader = VK_NULL_HANDLE;
    u32 tessellation_patch_control_points = 0;
    VkShaderModule geometry_shader = VK_NULL_HANDLE;
    /** VK_NULL_HANDLE when dynamic rendering is used. */
    VkRenderPass render_pass = VK_NULL_HANDLE;
  };
  struct PipelineCreationArgumentsPriorityComparator {
    bool operator()(const PipelineCreationArguments& a, const PipelineCreationArguments& b) const {
      return a.priority < b.priority;
    }
  };

  union TessellationControlShaderKey {
    u32 key;
    struct {
      Shader::HostVertexShaderType host_vertex_shader_type : Shader::kHostVertexShaderTypeBitCount;
      xenos::TessellationMode tessellation_mode : 2;
    };

    TessellationControlShaderKey() : key(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TessellationControlShaderKey& key) const {
        return std::hash<u32>{}(key.key);
      }
    };
    bool operator==(const TessellationControlShaderKey& other_key) const {
      return key == other_key.key;
    }
  };

  union GeometryShaderKey {
    u32 key;
    struct {
      PipelineGeometryShader type : 2;
      u32 interpolator_count : 5;
      u32 user_clip_plane_count : 3;
      u32 user_clip_plane_cull : 1;
      u32 has_vertex_kill_and : 1;
      u32 has_point_size : 1;
      u32 has_point_coordinates : 1;
      /** PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives. */
      u32 point_ps_ucp_mode : 2;
    };

    GeometryShaderKey() : key(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const GeometryShaderKey& key) const { return std::hash<u32>{}(key.key); }
    };
    bool operator==(const GeometryShaderKey& other_key) const { return key == other_key.key; }
    bool operator!=(const GeometryShaderKey& other_key) const { return !(*this == other_key); }
  };

  VulkanShader* LoadShader(xenos::ShaderType shader_type, const u32* host_address, u32 dword_count,
                           u64 data_hash);

  // Can be called from multiple threads.
  bool TranslateAnalyzedShader(SpirvShaderTranslator& translator,
                               VulkanShader::VulkanTranslation& translation);

  void WritePipelineRenderTargetDescription(xenos::reg::RB_BLENDCONTROL blend_control,
                                            u32 write_mask,
                                            PipelineRenderTarget& render_target_out) const;
  bool GetCurrentStateDescription(
      const VulkanShader::VulkanTranslation* vertex_shader,
      const VulkanShader::VulkanTranslation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      xenos::reg::RB_DEPTHCONTROL normalized_depth_control, u32 normalized_color_mask,
      VulkanRenderTargetCache::RenderPassKey render_pass_key,
      PipelineDescription& description_out) const;

  /**
   * Reports whether the device supports the pipeline.
   *
   * @param description Pipeline description to check.
   * @return True when the device supports the pipeline.
   */
  bool ArePipelineRequirementsMet(const PipelineDescription& description) const;

  static bool GetGeometryShaderKey(PipelineGeometryShader geometry_shader_type,
                                   SpirvShaderTranslator::Modification vertex_shader_modification,
                                   SpirvShaderTranslator::Modification pixel_shader_modification,
                                   GeometryShaderKey& key_out);
  static u32 GetTessellationPatchControlPointCount(
      Shader::HostVertexShaderType host_vertex_shader_type,
      xenos::TessellationMode tessellation_mode);
  VkShaderModule GetTessellationVertexShader(bool adaptive);
  VkShaderModule GetTessellationControlShader(Shader::HostVertexShaderType host_vertex_shader_type,
                                              xenos::TessellationMode tessellation_mode);
  VkShaderModule GetGeometryShader(GeometryShaderKey key);
  bool TryGetPipelineCreationArgumentsForDescription(
      const PipelineDescription& description,
      std::pair<const PipelineDescription, Pipeline>* pipeline,
      PipelineCreationArguments& creation_arguments, bool for_placeholder = false);

  /**
   * Creates the pipeline. Callable from creation threads once the shaders are translated and the
   * pipeline layout and render pass object (unless dynamic rendering is used) are available.
   *
   * @param creation_arguments Pre-looked-up creation request.
   * @param fragment_shader_override Replacement fragment shader, or VK_NULL_HANDLE for none.
   * @return True when the pipeline was created.
   */
  bool EnsurePipelineCreated(const PipelineCreationArguments& creation_arguments,
                             VkShaderModule fragment_shader_override = VK_NULL_HANDLE);
  void CreationThread(size_t thread_index);
  void CreateQueuedPipelinesOnProcessorThread();
  void ProcessDeferredPipelineDestructions(bool force_all);

  VulkanCommandProcessor& command_processor_;
  const xenos::RegisterFile& register_file_;
  VulkanRenderTargetCache& render_target_cache_;
  VkShaderStageFlags guest_shader_vertex_stages_;

  StringBuffer ucode_disasm_buffer_;
  std::unique_ptr<SpirvShaderTranslator> shader_translator_;

  struct LayoutUID {
    size_t uid;
    size_t vector_span_offset;
    size_t vector_span_length;
  };
  std::mutex layouts_mutex_;
  std::vector<VulkanShader::TextureBinding> texture_binding_layouts_;
  /** Keys are XXH3 hashes of layouts, collisions resolved through texture_binding_layouts_. */
  std::unordered_multimap<u64, LayoutUID, rex::IdentityHasher<u64>> texture_binding_layout_map_;

  std::unordered_map<u64, std::unique_ptr<VulkanShader>, rex::IdentityHasher<u64>> shaders_;

  /** Holds VK_NULL_HANDLE when creation failed. */
  std::unordered_map<GeometryShaderKey, VkShaderModule, GeometryShaderKey::Hasher>
      geometry_shaders_;

  bool tessellation_indexed_vertex_shader_attempted_ = false;
  VkShaderModule tessellation_indexed_vertex_shader_ = VK_NULL_HANDLE;
  bool tessellation_adaptive_vertex_shader_attempted_ = false;
  VkShaderModule tessellation_adaptive_vertex_shader_ = VK_NULL_HANDLE;
  std::unordered_map<TessellationControlShaderKey, VkShaderModule,
                     TessellationControlShaderKey::Hasher>
      tessellation_control_shaders_;

  VkShaderModule depth_only_fragment_shader_ = VK_NULL_HANDLE;
  VkShaderModule placeholder_pixel_shader_ = VK_NULL_HANDLE;
  VkShaderModule depth_float24_truncate_fragment_shader_ = VK_NULL_HANDLE;
  VkShaderModule depth_float24_round_fragment_shader_ = VK_NULL_HANDLE;

  std::unordered_map<PipelineDescription, Pipeline, PipelineDescription::Hasher> pipelines_;

  const std::pair<const PipelineDescription, Pipeline>* last_pipeline_ = nullptr;
  /** Pairs of submission index and pipeline. */
  std::deque<std::pair<u64, VkPipeline>> deferred_destroy_pipelines_;
  std::mutex deferred_destroy_lock_;

  std::filesystem::path shader_storage_cache_root_;
  u32 shader_storage_title_id_ = 0;

  std::fstream shader_storage_file_;
  std::filesystem::path shader_storage_file_path_;
  u32 shader_storage_index_ = 0;
  bool shader_storage_file_flush_needed_ = false;

  std::fstream pipeline_storage_file_;
  std::filesystem::path pipeline_storage_file_path_;
  bool pipeline_storage_file_flush_needed_ = false;

  void StorageWriteThread();
  std::mutex storage_write_request_lock_;
  std::condition_variable storage_write_request_cond_;
  /** Guarded by storage_write_request_lock_, signaled through storage_write_request_cond_. */
  std::deque<const Shader*> storage_write_shader_queue_;
  std::deque<PipelineStoredDescription> storage_write_pipeline_queue_;
  bool storage_write_flush_shaders_ = false;
  bool storage_write_flush_pipelines_ = false;
  bool storage_write_thread_shutdown_ = false;
  std::unique_ptr<rex::platform::Thread> storage_write_thread_;

  mutable std::mutex creation_request_lock_;
  std::condition_variable creation_request_cond_;
  std::priority_queue<PipelineCreationArguments, std::vector<PipelineCreationArguments>,
                      PipelineCreationArgumentsPriorityComparator>
      creation_queue_;
  size_t creation_threads_busy_ = 0;
  bool startup_loading_ = false;
  std::unique_ptr<rex::platform::Event> creation_completion_event_;
  bool creation_completion_set_event_ = false;
  size_t creation_threads_shutdown_from_ = SIZE_MAX;
  std::vector<std::unique_ptr<rex::platform::Thread>> creation_threads_;
};

}  // namespace rex::gpu::vulkan
