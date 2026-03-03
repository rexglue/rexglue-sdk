/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 5: Full pipeline state caching + shader compilation
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/string/buffer.h>
#include <rex/graphics/xenos.h>
#include <rex/graphics/metal/dxbc_to_dxil_converter.h>
#include <rex/graphics/metal/metal_shader_converter.h>
#include <rex/graphics/metal/shader_cache.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>

#ifdef __OBJC__
@protocol MTLDevice;
@protocol MTLLibrary;
@protocol MTLFunction;
@protocol MTLRenderPipelineState;
@protocol MTLComputePipelineState;
@protocol MTLDepthStencilState;
#endif

namespace rex::graphics::metal {

class MetalCommandProcessor;
class MetalShader;

class MetalPipelineCache {
 public:
  MetalPipelineCache(MetalCommandProcessor& command_processor);
  ~MetalPipelineCache();

  bool Initialize();
  void Shutdown();
  void ClearCache();

  void InitializeShaderStorage(const std::filesystem::path& cache_root,
                               uint32_t title_id, bool blocking);

  // =========================================================================
  // Shader compilation
  // =========================================================================

  // Translate and compile a MetalShader to produce MTLFunction objects.
  // Returns true if the shader is now valid and ready for pipeline creation.
  bool TranslateShader(MetalShader* shader, uint64_t modification = 0);

  // =========================================================================
  // Pipeline state lookup/creation
  // =========================================================================

  // All render state needed to create a pipeline state object.
  struct RenderPipelineKey {
    // Shader hashes.
    uint64_t vertex_shader_hash = 0;
    uint64_t pixel_shader_hash = 0;
    uint64_t vertex_shader_modification = 0;
    uint64_t pixel_shader_modification = 0;
    uint64_t vertex_layout_hash = 0;

    // Render target formats (up to 4 MRT + depth).
    uint32_t color_formats[4] = {};      // MTLPixelFormat cast to uint32_t
    uint32_t depth_format = 0;           // MTLPixelFormat cast to uint32_t
    uint32_t color_target_count = 0;

    // Blend state per color attachment.
    struct BlendState {
      bool blend_enable = false;
      uint32_t src_blend = 0;
      uint32_t dst_blend = 0;
      uint32_t blend_op = 0;
      uint32_t src_blend_alpha = 0;
      uint32_t dst_blend_alpha = 0;
      uint32_t blend_op_alpha = 0;
      uint32_t write_mask = 0xF;  // RGBA
    };
    BlendState blend_states[4];

    // Vertex state.
    bool has_vertex_index = true;

    // MSAA.
    uint32_t sample_count = 1;

    bool operator==(const RenderPipelineKey& o) const;
  };

  struct RenderPipelineKeyHash {
    size_t operator()(const RenderPipelineKey& key) const;
  };

  // Get or create a render pipeline state for the given key.
  // Returns nullptr if creation fails.
#ifdef __OBJC__
  id<MTLRenderPipelineState> GetOrCreateRenderPipelineState(
      const RenderPipelineKey& key,
      MetalShader* vertex_shader,
      MetalShader* pixel_shader);
#endif

  // =========================================================================
  // Depth/stencil state
  // =========================================================================

  struct DepthStencilKey {
    bool depth_test_enable = false;
    bool depth_write_enable = false;
    uint32_t depth_compare_func = 0;  // MTLCompareFunction

    bool stencil_enable = false;
    uint32_t stencil_read_mask = 0xFF;
    uint32_t stencil_write_mask = 0xFF;
    uint32_t stencil_front_compare = 0;
    uint32_t stencil_front_pass = 0;
    uint32_t stencil_front_fail = 0;
    uint32_t stencil_front_depth_fail = 0;
    uint32_t stencil_back_compare = 0;
    uint32_t stencil_back_pass = 0;
    uint32_t stencil_back_fail = 0;
    uint32_t stencil_back_depth_fail = 0;

    bool operator==(const DepthStencilKey& o) const;
  };

  struct DepthStencilKeyHash {
    size_t operator()(const DepthStencilKey& key) const;
  };

#ifdef __OBJC__
  id<MTLDepthStencilState> GetOrCreateDepthStencilState(
      const DepthStencilKey& key);
#endif

  // =========================================================================
  // Fallback shaders
  // =========================================================================

  // Get the fallback vertex/fragment functions for shaders that haven't
  // been translated yet. These produce a basic colored output so something
  // is visible even before full shader translation.
#ifdef __OBJC__
  id<MTLFunction> fallback_vertex_function() const {
    return fallback_vertex_func_;
  }
  id<MTLFunction> fallback_fragment_function() const {
    return fallback_fragment_func_;
  }
  id<MTLFunction> resolve_compute_function() const {
    return resolve_compute_func_;
  }
  id<MTLFunction> store_compute_function() const {
    return store_compute_func_;
  }
#endif

 private:
  MetalCommandProcessor& command_processor_;

  // Fallback shader library.
#ifdef __OBJC__
  id<MTLLibrary> fallback_library_ = nullptr;
  id<MTLFunction> fallback_vertex_func_ = nullptr;
  id<MTLFunction> fallback_fragment_func_ = nullptr;
  id<MTLFunction> resolve_compute_func_ = nullptr;
  id<MTLFunction> store_compute_func_ = nullptr;
#else
  void* fallback_library_ = nullptr;
  void* fallback_vertex_func_ = nullptr;
  void* fallback_fragment_func_ = nullptr;
  void* resolve_compute_func_ = nullptr;
  void* store_compute_func_ = nullptr;
#endif

  // Cached render pipeline states.
  std::unordered_map<RenderPipelineKey,
#ifdef __OBJC__
                     id<MTLRenderPipelineState>,
#else
                     void*,
#endif
                     RenderPipelineKeyHash>
      render_pipeline_cache_;

  // Cached depth/stencil states.
  std::unordered_map<DepthStencilKey,
#ifdef __OBJC__
                     id<MTLDepthStencilState>,
#else
                     void*,
#endif
                     DepthStencilKeyHash>
      depth_stencil_cache_;

  // Last successfully materialized shader modification per shader hash.
  std::unordered_map<uint64_t, uint64_t> active_shader_modifications_;

  // Shader storage path.
  std::filesystem::path shader_storage_root_;
  uint32_t current_title_id_ = 0;

  // Shader translation pipeline components.
  std::unique_ptr<DxbcShaderTranslator> shader_translator_;
  string::StringBuffer ucode_disasm_buffer_;
  DxbcToDxilConverter dxbc_to_dxil_converter_;
  MetalShaderConverter metal_shader_converter_;
  MetalShaderCache shader_cache_;
  bool shader_pipeline_available_ = false;
};

}  // namespace rex::graphics::metal
