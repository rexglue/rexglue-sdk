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
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include "gpu/xenos/d3d12/render_target_cache.h"
#include "gpu/xenos/d3d12/shader.h"
#include "gpu/xenos/pipeline/shader/dxbc_translator.h"
#include "gpu/xenos/primitive_processor.h"
#include <rex/xenos/register.h>
#include <rex/xenos/format.h>
#include <rex/xenos/packet.h>
#include <rex/xenos/state.h>
#include <rex/hash.h>
#include <rex/platform.h>
#include <rex/string/text.h>
#include <rex/platform/thread.h>
#include <rex/platform/sync.h>
#include "gpu/xenos/host/d3d12/d3d12_api.h"
#include "gpu/xenos/host/shader_storage.h"

namespace rex::gpu::d3d12 {

class D3D12CommandProcessor;

class PipelineCache {
 public:
  static constexpr size_t kLayoutUIDEmpty = 0;

  PipelineCache(D3D12CommandProcessor& command_processor, const xenos::RegisterFile& register_file,
                const D3D12RenderTargetCache& render_target_cache, bool bindless_resources_used);
  ~PipelineCache();

  bool Initialize();
  void Shutdown();

  void InitializeShaderStorage(const std::filesystem::path& cache_root, u32 title_id,
                               bool blocking);
  void ShutdownShaderStorage();

  void EndSubmission();
  bool IsCreatingPipelines();

  D3D12Shader* LoadShader(xenos::ShaderType shader_type, const u32* host_address, u32 dword_count);
  /** Analyzes shader microcode on the translator thread. */
  void AnalyzeShaderUCode(Shader& shader);

  /**
   * Returns the shader modification for the current state. The shader must have microcode
   * analyzed.
   *
   * @param interpolator_mask Mask of the interpolators the pixel shader reads.
   */
  DXBCShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
      u32 interpolator_mask) const;
  DXBCShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, u32 interpolator_mask, u32 param_gen_pos,
      xenos::reg::RB_DEPTHCONTROL normalized_depth_control) const;

  /**
   * Configures the pipeline for the current state. When draw_state::IsRasterizationDone is false,
   * the pixel shader must be null.
   *
   * @param pixel_shader Translated pixel shader, or null.
   * @param normalized_depth_control Normalized RB_DEPTHCONTROL.
   * @param pipeline_handle_out Receives the pipeline handle.
   * @param root_signature_out Receives the root signature.
   * @return True when the pipeline was configured.
   */
  bool ConfigurePipeline(D3D12Shader::D3D12Translation* vertex_shader,
                         D3D12Shader::D3D12Translation* pixel_shader,
                         const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
                         xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
                         u32 normalized_color_mask, u32 bound_depth_and_color_render_target_bits,
                         const u32* bound_depth_and_color_render_target_formats,
                         void** pipeline_handle_out, ID3D12RootSignature** root_signature_out);

  /**
   * Returns a pipeline by its handle, waiting for deferred creation, or null when creation failed.
   *
   * @param handle Handle from ConfigurePipeline.
   */
  ID3D12PipelineState* GetD3D12PipelineByHandle(void* handle) const {
    return reinterpret_cast<const Pipeline*>(handle)->state.load(std::memory_order_acquire);
  }

 private:
  REX_PACKED_STRUCT(ShaderStoredHeader, {
    u64 ucode_data_hash;

    u32 ucode_dword_count : 31;
    xenos::ShaderType type : 1;

    static constexpr u32 kVersion = 0x20201219;
  });

  // Changing any Pipeline* enum requires bumping PipelineDescription::kVersion.
  // NOLINTNEXTLINE(performance-enum-size): packed key bit field, MSVC packs per type
  enum class PipelineStripCutIndex : u32 {
    kNone,
    kFFFF,
    kFFFFFFFF,
  };

  enum class PipelineTessellationMode : u8 {
    kNone,
    kDiscrete,
    kContinuous,
    kAdaptive,
  };

  enum class PipelinePatchType : u8 {
    kNone,
    kLine,
    kTriangle,
    kQuad,
  };

  enum class PipelinePrimitiveTopologyType : u8 {
    kPoint,
    kLine,
    kTriangle,
  };

  // NOLINTNEXTLINE(performance-enum-size): packed key bit field, MSVC packs per type
  enum class PipelineGeometryShader : u32 {
    kNone,
    kPointList,
    kRectangleList,
    kQuadList,
  };

  // NOLINTNEXTLINE(performance-enum-size): packed key bit field, MSVC packs per type
  enum class PipelineCullMode : u32 {
    kNone,
    kFront,
    kBack,
    /** Disables the pixel shader and depth and stencil. */
    kDisableRasterization,
  };

  // NOLINTNEXTLINE(performance-enum-size): packed key bit field, MSVC packs per type
  enum class PipelineBlendFactor : u32 {
    kZero,
    kOne,
    kSrcColor,
    kInvSrcColor,
    kSrcAlpha,
    kInvSrcAlpha,
    kDestColor,
    kInvDestColor,
    kDestAlpha,
    kInvDestAlpha,
    kBlendFactor,
    kInvBlendFactor,
    kSrcAlphaSat,
  };

  // Changing PipelineRenderTarget requires bumping PipelineDescription::kVersion.
  REX_PACKED_STRUCT(PipelineRenderTarget, {
    u32 used : 1;
    xenos::ColorRenderTargetFormat format : 4;
    PipelineBlendFactor src_blend : 4;
    PipelineBlendFactor dest_blend : 4;
    xenos::BlendOp blend_op : 3;
    PipelineBlendFactor src_blend_alpha : 4;
    PipelineBlendFactor dest_blend_alpha : 4;
    xenos::BlendOp blend_op_alpha : 3;
    u32 write_mask : 4;

    bool operator==(const PipelineRenderTarget&) const = default;
  });

  REX_PACKED_STRUCT(PipelineDescription, {
    u64 vertex_shader_hash;
    u64 vertex_shader_modification;
    /** 0 if drawing without a pixel shader. */
    u64 pixel_shader_hash;
    u64 pixel_shader_modification;

    i32 depth_bias;
    f32 depth_bias_slope_scaled;

    PipelineStripCutIndex strip_cut_index : 2;
    /**
     * PipelinePrimitiveTopologyType for a vertex shader, xenos::TessellationMode for a domain
     * shader.
     */
    u32 primitive_topology_type_or_tessellation_mode : 2;
    /** 0 for non kVertex host_vertex_shader_type. */
    PipelineGeometryShader geometry_shader : 2;
    u32 fill_mode_wireframe : 1;
    PipelineCullMode cull_mode : 2;
    u32 front_counter_clockwise : 1;
    u32 depth_clip : 1;
    xenos::MSAASamples host_msaa_samples : 2;
    xenos::DepthRenderTargetFormat depth_format : 1;
    xenos::CompareFunction depth_func : 3;
    u32 depth_write : 1;
    u32 stencil_enable : 1;
    u32 stencil_read_mask : 8;

    u32 stencil_write_mask : 8;
    xenos::StencilOp stencil_front_fail_op : 3;
    xenos::StencilOp stencil_front_depth_fail_op : 3;
    xenos::StencilOp stencil_front_pass_op : 3;
    xenos::CompareFunction stencil_front_func : 3;
    xenos::StencilOp stencil_back_fail_op : 3;
    xenos::StencilOp stencil_back_depth_fail_op : 3;
    xenos::StencilOp stencil_back_pass_op : 3;
    xenos::CompareFunction stencil_back_func : 3;

    PipelineRenderTarget render_targets[xenos::kMaxColorRenderTargets];

    bool operator==(const PipelineDescription&) const = default;

    static constexpr u32 kVersion = 0x20210425;
  });

  REX_PACKED_STRUCT(PipelineStoredDescription, {
    u64 description_hash;
    PipelineDescription description;
  });

  struct PipelineRuntimeDescription {
    ID3D12RootSignature* root_signature;
    D3D12Shader::D3D12Translation* vertex_shader;
    D3D12Shader::D3D12Translation* pixel_shader;
    const std::vector<u32>* geometry_shader;
    PipelineDescription description;
  };

  struct Pipeline;

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

    using Hasher = rex::KeyHasher<GeometryShaderKey>;
    bool operator==(const GeometryShaderKey& other_key) const { return key == other_key.key; }
    bool operator!=(const GeometryShaderKey& other_key) const { return !(*this == other_key); }
  };

  DXBCOptions GetDXBCOptions() const;

  D3D12Shader* LoadShader(xenos::ShaderType shader_type, const u32* host_address, u32 dword_count,
                          u64 data_hash);

  /** Can be called from multiple threads. */
  bool TranslateAnalyzedShader(DXBCShaderTranslator& translator,
                               D3D12Shader::D3D12Translation& translation,
                               IDxbcConverter* dxbc_converter = nullptr,
                               IDxcUtils* dxc_utils = nullptr,
                               IDxcCompiler* dxc_compiler = nullptr);

  /**
   * Builds the pipeline description of the current state. When draw_state::IsRasterizationDone is
   * false, the pixel shader must be null. The shaders must be translated and valid unless
   * for_placeholder is true.
   *
   * Direct3D 12 has no per side fill mode and depth bias, so the culled side selects the values.
   * Without culling, wireframe on either side applies and the front side depth bias is used.
   * Point fill mode becomes wireframe. Only one side may be culled, as culling both disables
   * rasterization.
   *
   * @param pixel_shader The pixel shader translation, may be null.
   * @param normalized_depth_control The depth control with the guest quirks applied.
   * @param normalized_color_mask The color write mask with the guest quirks applied.
   * @param runtime_description_out Receives the description.
   * @param for_placeholder True to build the description of a placeholder pipeline.
   * @return True on success.
   */
  bool GetCurrentStateDescription(
      D3D12Shader::D3D12Translation* vertex_shader, D3D12Shader::D3D12Translation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      xenos::reg::RB_DEPTHCONTROL normalized_depth_control, u32 normalized_color_mask,
      u32 bound_depth_and_color_render_target_bits,
      const u32* bound_depth_and_color_render_target_formats,
      PipelineRuntimeDescription& runtime_description_out, bool for_placeholder = false);

  static bool GetGeometryShaderKey(PipelineGeometryShader geometry_shader_type,
                                   DXBCShaderTranslator::Modification vertex_shader_modification,
                                   DXBCShaderTranslator::Modification pixel_shader_modification,
                                   GeometryShaderKey& key_out);
  /**
   * Builds a DXBC geometry shader container for the key.
   *
   * @param shader_out Receives the container dwords.
   */
  static void CreateDXBCGeometryShader(GeometryShaderKey key, std::vector<u32>& shader_out);
  const std::vector<u32>& GetGeometryShader(GeometryShaderKey key);

  /** Creates the pipeline state object, or returns null on failure. */
  ID3D12PipelineState* CreateD3D12Pipeline(const PipelineRuntimeDescription& runtime_description);
  bool PrepareRuntimeDescriptionForQueuedCreation(Pipeline* pipeline,
                                                  PipelineRuntimeDescription& runtime_description);

  D3D12CommandProcessor& command_processor_;
  const xenos::RegisterFile& register_file_;
  const D3D12RenderTargetCache& render_target_cache_;
  bool bindless_resources_used_;

  string::StringBuffer ucode_disasm_buffer_;
  /** Used on the processor thread and, under translation_request_lock_, the creation threads. */
  std::unique_ptr<DXBCShaderTranslator> shader_translator_;
  std::mutex translation_request_lock_;

  /** Command processor thread DXIL conversion interfaces, null unless DXIL disassembly is on. */
  IDxbcConverter* dxbc_converter_ = nullptr;
  IDxcUtils* dxc_utils_ = nullptr;
  IDxcCompiler* dxc_compiler_ = nullptr;

  /** Keyed by ucode hash. */
  std::unordered_map<u64, std::unique_ptr<D3D12Shader>, rex::IdentityHasher<u64>> shaders_;

  struct LayoutUID {
    size_t uid;
    size_t vector_span_offset;
    size_t vector_span_length;
  };
  std::mutex layouts_mutex_;
  std::vector<D3D12Shader::TextureBinding> texture_binding_layouts_;
  /**
   * Keys are XXH3 hashes of layouts. Values need collision resolution using the span in
   * texture_binding_layouts_.
   */
  std::unordered_multimap<u64, LayoutUID, rex::IdentityHasher<u64>> texture_binding_layout_map_;
  /** For bindful, the sampler count is the UID instead. */
  std::vector<u32> bindless_sampler_layouts_;
  /** Keys are XXH3 hashes of used bindless sampler indices. */
  std::unordered_multimap<u64, LayoutUID, rex::IdentityHasher<u64>> bindless_sampler_layout_map_;

  std::unordered_map<GeometryShaderKey, std::vector<u32>, GeometryShaderKey::Hasher>
      geometry_shaders_;

  std::vector<u8> depth_only_pixel_shader_;

  struct Pipeline {
    /** Null if creation failed. */
    std::atomic<ID3D12PipelineState*> state{nullptr};
    std::atomic<ID3D12RootSignature*> root_signature{nullptr};
    PipelineRuntimeDescription description;
    D3D12Shader::D3D12Translation* pending_vertex_shader = nullptr;
    D3D12Shader::D3D12Translation* pending_pixel_shader = nullptr;
    u8 priority = 0;
  };
  struct PipelineCreationPriorityComparator {
    bool operator()(const Pipeline* a, const Pipeline* b) const {
      u8 priority_a = a ? a->priority : 0;
      u8 priority_b = b ? b->priority : 0;
      return priority_a < priority_b;
    }
  };
  std::unordered_multimap<u64, std::unique_ptr<Pipeline>, rex::IdentityHasher<u64>> pipelines_;

  Pipeline* current_pipeline_ = nullptr;

  void StorageWriteThread();
  gpu::ShaderStorage<Shader, PipelineStoredDescription> storage_;

  void CreationThread(size_t thread_index);
  void CreateQueuedPipelinesOnProcessorThread();
  std::mutex creation_request_lock_;
  std::condition_variable creation_request_cond_;
  /** Protected with creation_request_lock_, notify_one creation_request_cond_ when set. */
  std::priority_queue<Pipeline*, std::vector<Pipeline*>, PipelineCreationPriorityComparator>
      creation_queue_;
  /**
   * Number of threads creating a pipeline, incremented on dequeue as the completion event must
   * not trigger before this is 0. Protected with creation_request_lock_.
   */
  size_t creation_threads_busy_ = 0;
  /** Manual reset event set by the thread creating the last queued pipeline. */
  std::unique_ptr<rex::platform::Event> creation_completion_event_;
  /**
   * True when setting the event on completion is queued. Protected with creation_request_lock_,
   * notify_one creation_request_cond_ when set.
   */
  bool creation_completion_set_event_ = false;
  /**
   * Creation threads with this index or above must shut down. Protected with
   * creation_request_lock_, notify_all creation_request_cond_ when set.
   */
  size_t creation_threads_shutdown_from_ = SIZE_MAX;
  std::vector<std::unique_ptr<rex::platform::Thread>> creation_threads_;
};

}  // namespace rex::gpu::d3d12
