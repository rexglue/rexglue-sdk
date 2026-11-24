/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_RESOLVE_SHADER_PIPELINES_H_
#define XENIA_GPU_METAL_METAL_RESOLVE_SHADER_PIPELINES_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gpu/xenos/metal/metal_draw_util.h>

namespace rex {
namespace graphics {
namespace metal {

// Pipelines for the SHARED cross-backend resolve-copy compute shaders
// (src/rex/graphics/shaders/resolve_{fast,full}_*.cs.xesl, MSL branch), which
// resolve EDRAM directly into the shared-memory buffer at GUEST TILED
// destination addresses.
//
// WHY THESE AND NOT A METAL-SPECIFIC KERNEL (task 4.1 assessment,
// docs/dev/archive/evidence/metal_shared_resolve_port_assessment.md): D3D12 and Vulkan already
// resolve with these exact shaders, so their semantics -- destination
// addressing, the sample-select matrix including averaging, the half-pixel
// offset source clamp -- are validated cross-backend against the same guest
// content. A bespoke MSL scatter is a third implementation of the same
// contract, and the SF4 divergence recorded on 2026-08-15 was a disagreement
// between hand-maintained copies.
//
// Scope, deliberately narrow for the first step:
//   * COPY shaders only. `resolve_clear_*` has its own constants and Metal has
//     its own validated canonical clear.
//   * UNSCALED variants only. Metal is 1x; the `_scaled` sources compile but
//     must never be selected.
class MetalResolveShaderPipelines {
 public:
  // Buffer binding indices of the generated entry_xe ABI (resolve.xesli):
  //   buffer(0) constant push_const_struct_xe&
  //   buffer(1) device char* xe_resolve_dest   (shared memory buffer)
  //   buffer(2) const device char* xe_resolve_edram (canonical EDRAM buffer)
  static constexpr uint32_t kBufferIndexPushConstants = 0;
  static constexpr uint32_t kBufferIndexDest = 1;
  static constexpr uint32_t kBufferIndexEdram = 2;

  // Host mirror of the MSL push_const_struct_xe for the resolve-copy family.
  // These are the FIVE RAW GUEST WORDS the shared
  // draw_util::ResolveCopyShaderConstants already produces -- this struct
  // exists to pin the ABI, never to re-derive the values.
  struct ResolveConstants {
    uint32_t edram_info;             // 0
    uint32_t coordinate_info;        // 4
    uint32_t dest_info;              // 8
    uint32_t dest_coordinate_info;   // 12
    uint32_t dest_base;              // 16
    // Read only by the kDXT3AAs1111 shader (resolve.xesli
    // XE_RESOLVE_COPY_DXT3A_AS_1111): RB_COPY_FUNC, RB_COPY_REF, RB_COPY_MASK.
    uint32_t copy_func;              // 20
    uint32_t copy_ref;               // 24
    uint32_t copy_mask;              // 28
  };
  static_assert(sizeof(ResolveConstants) == 32, "MSL push-const ABI");

  MetalResolveShaderPipelines() = default;
  ~MetalResolveShaderPipelines();
  MetalResolveShaderPipelines(const MetalResolveShaderPipelines&) = delete;
  MetalResolveShaderPipelines& operator=(const MetalResolveShaderPipelines&) =
      delete;

  // Builds a library + entry_xe function + compute pipeline for every
  // supported copy shader. `device` is an id<MTLDevice>. Returns false and
  // leaves the object empty if any pipeline cannot be created -- partial
  // initialization would make an unsupported index silently dispatch nothing.
  bool Initialize(void* device);
  void Shutdown();

  bool initialized() const { return !pipelines_.empty(); }

  // RETAINED id<MTLComputePipelineState> as void* for `index`, or nullptr if
  // the index is unsupported (Metal is 1x, so only the unscaled copy family
  // is present) or the object is not initialized. Ownership stays here.
  void* PipelineFor(draw_util::ResolveCopyShaderIndex index) const;

 private:
  std::vector<void*> libraries_;   // retained id<MTLLibrary>
  std::vector<void*> pipelines_;   // retained id<MTLComputePipelineState>,
                                   // indexed by ResolveCopyShaderIndex
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_RESOLVE_SHADER_PIPELINES_H_
