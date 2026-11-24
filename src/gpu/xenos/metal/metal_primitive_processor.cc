/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_primitive_processor.h"

#include <algorithm>
#include <cstring>

#include "rex/math.h"

#include "rex/assert.h"
#include <rex/logging/log_category.h>

namespace rex {
namespace graphics {
namespace metal {

bool MetalPrimitiveProcessor::Initialize(bool convert_quad_lists) {
  // PP1 configuration (see the class comment):
  //   full_32bit_vertex_indices_supported = true
  //     Metal supports full 32-bit indices; keeps 32-bit guest DMA indices as
  //     kGuestDMA with host_shader_index_endian == the guest endian (no
  //     indirection / pre-masking), matching the command processor's existing
  //     CPU byte-swap-into-arena mechanism exactly.
  //   line_loops_supported = false
  //     PP3-auto activates the shared auto line-loop -> line-strip result. Only
  //     auto-indexed reset-disabled loops are routed by the command processor,
  //     so the DMA kHostConverted path remains unreachable.
  //   quad_lists supported = !convert_quad_lists (true on Apple GPUs, where
  //     MSC geometry emulation draws them; false elsewhere provisions the
  //     shared quad -> triangle-list table for IsAutoQuadRoutable draws),
  //   point_sprites/rectangle_lists native (no VS expansion) = true
  //     None of the routed classes depend on these flags, and the routing
  //     predicates keep expanded classes on the MSC path. PP2-auto's fan table
  //     is the only built-in allocation; line-loop conversion adds none.
  //   host_primitive_reset_always_enabled = true (2026-10-26)
  //     Metal cannot disable primitive restart: a real 0xFFFF index in a
  //     reset-disabled 16-bit buffer drops its primitive. The shared processor
  //     widens such buffers to 32-bit (kHostConverted, 0x0000FFFF), which the
  //     command processor now consumes. Buffers without 0xFFFF are untouched
  //     (kGuestDMA, byte-identical to before).
  //   host_primitive_reset_strips_only = true (2026-11-01)
  //     Metal restarts strips only: 0xFFFF in a triangle or line list draws
  //     as a vertex (probed on M1 Max, RX 6900 XT, Radeon Pro 580X), so those
  //     draws skip the scan.
  // PP2-auto: triangle_fans_supported = false enables shared fan-to-list
  // conversion. Only auto-indexed fans are routed into Process by the command
  // processor (kHostBuiltinForAuto); DMA/reset fans stay on the inline path.
  // convert_triangle_fans_first_provoking = true makes both the built-in table
  // and TriangleFanToList emit the center-first (v0,v1,v2),(v0,v2,v3) order,
  // preserving Metal's historical inline fan order and provoking behavior.
  return InitializeCommon(
      /*full_32bit_vertex_indices_supported=*/true,
      /*triangle_fans_supported=*/false,
      /*line_loops_supported=*/false,
      /*quad_lists_supported=*/!convert_quad_lists,
      /*point_sprites_supported_without_vs_expansion=*/true,
      /*rectangle_lists_supported_without_vs_expansion=*/true);
}

bool MetalPrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  // PP2-auto: own the built-in auto-index fan table on the CPU. The command
  // processor materializes each draw's slice into its transient upload arena,
  // so no persistent MTL resource is created here.
  if (!size_bytes) {
    return false;
  }
  builtin_index_data_.resize(size_bytes);
  fill_callback(builtin_index_data_.data());
  return true;
}

bool MetalPrimitiveProcessor::CopyBuiltinIndexData(size_t handle,
                                                   size_t byte_count,
                                                   void* dst) const {
  if (handle > builtin_index_data_.size() ||
      byte_count > builtin_index_data_.size() - handle) {
    return false;
  }
  std::memcpy(dst, builtin_index_data_.data() + handle, byte_count);
  return true;
}

void* MetalPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  const size_t index_size = format == xenos::IndexFormat::kInt16
                                ? sizeof(uint16_t)
                                : sizeof(uint32_t);
  // A zero-count conversion (degenerate fan/loop) still needs a non-null
  // mapping, exactly like a device upload pool returns one; reserve a slot.
  const size_t bytes = std::max(
      size_t(1),
      index_size * size_t(index_count) +
          (coalign_for_simd ? size_t(REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE)
                            : 0));
  // Every allocation starts 16-byte aligned within the pool so the shared
  // SIMD conversions see the alignment they would from a device mapping.
  const size_t base = rex::align(converted_index_data_.size(), size_t(16));
  if (bytes > SIZE_MAX - base) {
    return nullptr;
  }
  converted_index_data_.resize(base + bytes);
  uint8_t* mapping = converted_index_data_.data() + base;
  size_t handle = base;
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset =
        GetSIMDCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    handle += size_t(coalignment_offset);
  }
  backend_handle_out = handle;
  return mapping;
}

bool MetalPrimitiveProcessor::CopyConvertedIndexData(size_t handle,
                                                     size_t byte_count,
                                                     void* dst) const {
  if (handle > converted_index_data_.size() ||
      byte_count > converted_index_data_.size() - handle) {
    return false;
  }
  std::memcpy(dst, converted_index_data_.data() + handle, byte_count);
  return true;
}

void MetalPrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  converted_index_data_.clear();
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex
