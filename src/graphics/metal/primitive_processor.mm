/**
 ******************************************************************************
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 */

#import <Metal/Metal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <rex/graphics/metal/primitive_processor.h>
#include <rex/graphics/metal/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/xenia_logging_compat.h>

namespace rex::graphics::metal {

MetalPrimitiveProcessor::MetalPrimitiveProcessor(
    const RegisterFile& register_file,
    memory::Memory& memory,
    TraceWriter& trace_writer,
    SharedMemory& shared_memory,
    MetalCommandProcessor& command_processor)
    : PrimitiveProcessor(register_file, memory, trace_writer, shared_memory),
      command_processor_(command_processor) {}

MetalPrimitiveProcessor::~MetalPrimitiveProcessor() {
  Shutdown(true);
}

bool MetalPrimitiveProcessor::Initialize() {
  bool rectlist_vs_expand = REXCVAR_GET(metal_rectlist_vs_expand);
  if (!InitializeCommon(
          true,  // full_32bit_vertex_indices_supported
          false, // triangle_fans_supported
          false, // line_loops_supported
          false, // quad_lists_supported
          true,  // point_sprites_supported_without_vs_expansion
          !rectlist_vs_expand)) {
    Shutdown();
    return false;
  }

  current_frame_ = 0;
  converted_index_buffers_.clear();
  frame_index_buffers_.clear();

  XELOGI("MetalPrimitiveProcessor: Initialized (rectlist_vs_expand={})",
         rectlist_vs_expand ? "true" : "false");
  return true;
}

void MetalPrimitiveProcessor::Shutdown(bool from_destructor) {
  converted_index_buffers_.clear();
  frame_index_buffers_.clear();
  builtin_index_buffer_ = nil;
  builtin_index_buffer_size_ = 0;

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void MetalPrimitiveProcessor::CompletedSubmissionUpdated() {}

void MetalPrimitiveProcessor::BeginSubmission() {}

void MetalPrimitiveProcessor::BeginFrame() {
  ++current_frame_;
  converted_index_buffers_.clear();

  uint64_t current_frame = current_frame_;
  frame_index_buffers_.erase(
      std::remove_if(frame_index_buffers_.begin(), frame_index_buffers_.end(),
                     [current_frame](const FrameIndexBuffer& buffer) {
                       return current_frame - buffer.last_frame_used > 2;
                     }),
      frame_index_buffers_.end());
}

#ifdef __OBJC__
id<MTLBuffer> MetalPrimitiveProcessor::GetBuiltinIndexBuffer(
    size_t handle, uint64_t& offset_bytes_out) const {
  offset_bytes_out = uint64_t(GetBuiltinIndexBufferOffsetBytes(handle));
  return builtin_index_buffer_;
}

id<MTLBuffer> MetalPrimitiveProcessor::GetConvertedIndexBuffer(
    size_t handle, uint64_t& offset_bytes_out,
    uint64_t* size_bytes_out) const {
  if (handle >= converted_index_buffers_.size()) {
    offset_bytes_out = 0;
    if (size_bytes_out) {
      *size_bytes_out = 0;
    }
    return nil;
  }
  const ConvertedIndexBufferBinding& binding = converted_index_buffers_[handle];
  offset_bytes_out = binding.offset_bytes;
  if (size_bytes_out) {
    *size_bytes_out = binding.size_bytes;
  }
  return binding.buffer;
}
#else
void* MetalPrimitiveProcessor::GetBuiltinIndexBuffer(
    size_t handle, uint64_t& offset_bytes_out) const {
  offset_bytes_out = uint64_t(GetBuiltinIndexBufferOffsetBytes(handle));
  return builtin_index_buffer_;
}

void* MetalPrimitiveProcessor::GetConvertedIndexBuffer(
    size_t handle, uint64_t& offset_bytes_out,
    uint64_t* size_bytes_out) const {
  if (handle >= converted_index_buffers_.size()) {
    offset_bytes_out = 0;
    if (size_bytes_out) {
      *size_bytes_out = 0;
    }
    return nullptr;
  }
  const ConvertedIndexBufferBinding& binding = converted_index_buffers_[handle];
  offset_bytes_out = binding.offset_bytes;
  if (size_bytes_out) {
    *size_bytes_out = binding.size_bytes;
  }
  return binding.buffer;
}
#endif

bool MetalPrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  if (!size_bytes || builtin_index_buffer_) {
    return false;
  }

  auto& provider = command_processor_.GetMetalProvider();
  id<MTLDevice> device = provider.device();
  if (!device) {
    XELOGE("MetalPrimitiveProcessor: No Metal device for builtin IB");
    return false;
  }

  MTLResourceOptions options = provider.has_unified_memory()
                                   ? (MTLResourceStorageModeShared |
                                      MTLResourceCPUCacheModeWriteCombined)
                                   : MTLResourceStorageModeManaged;
  builtin_index_buffer_ = [device newBufferWithLength:size_bytes options:options];
  if (!builtin_index_buffer_) {
    XELOGE("MetalPrimitiveProcessor: Failed to allocate builtin IB ({} bytes)",
           size_bytes);
    return false;
  }

  builtin_index_buffer_size_ = size_bytes;
  builtin_index_buffer_.label = @"PrimitiveProcessorBuiltinIndexBuffer";

  void* mapping = [builtin_index_buffer_ contents];
  if (!mapping) {
    XELOGE("MetalPrimitiveProcessor: Failed to map builtin IB");
    builtin_index_buffer_ = nil;
    builtin_index_buffer_size_ = 0;
    return false;
  }
  fill_callback(mapping);
  if (builtin_index_buffer_.storageMode == MTLStorageModeManaged) {
    [builtin_index_buffer_ didModifyRange:NSMakeRange(0, size_bytes)];
  }

  return true;
}

void* MetalPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t)
                                                            : sizeof(uint32_t);
  size_t required_size = index_size * index_count;
  if (coalign_for_simd) {
    required_size += XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE;
  }

  FrameIndexBuffer* chosen_buffer = nullptr;
  for (auto& frame_buffer : frame_index_buffers_) {
    if (frame_buffer.size >= required_size &&
        frame_buffer.last_frame_used != current_frame_) {
      chosen_buffer = &frame_buffer;
      break;
    }
  }

  if (!chosen_buffer) {
    auto& provider = command_processor_.GetMetalProvider();
    id<MTLDevice> device = provider.device();
    if (!device) {
      backend_handle_out = SIZE_MAX;
      return nullptr;
    }

    size_t allocation_size = std::max(required_size, size_t(4096));
    allocation_size = (allocation_size + 4095) & ~size_t(4095);
    MTLResourceOptions options = provider.has_unified_memory()
                                     ? (MTLResourceStorageModeShared |
                                        MTLResourceCPUCacheModeWriteCombined)
                                     : MTLResourceStorageModeManaged;
    id<MTLBuffer> new_buffer =
        [device newBufferWithLength:allocation_size options:options];
    if (!new_buffer) {
      backend_handle_out = SIZE_MAX;
      XELOGE("MetalPrimitiveProcessor: Failed to allocate converted IB ({} bytes)",
             allocation_size);
      return nullptr;
    }

    new_buffer.label = @"PrimitiveProcessorConvertedIndexBuffer";
    frame_index_buffers_.push_back({new_buffer, allocation_size, 0});
    chosen_buffer = &frame_index_buffers_.back();
  }

  chosen_buffer->last_frame_used = current_frame_;

  uint8_t* mapping = static_cast<uint8_t*>([chosen_buffer->buffer contents]);
  if (!mapping) {
    backend_handle_out = SIZE_MAX;
    return nullptr;
  }

  uint64_t offset = 0;
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset =
        GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    offset = uint64_t(coalignment_offset);
  }

  backend_handle_out = converted_index_buffers_.size();
  converted_index_buffers_.push_back(
      {chosen_buffer->buffer, offset, index_size * uint64_t(index_count)});
  return mapping;
}

void MetalPrimitiveProcessor::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  PrimitiveProcessor::MemoryInvalidationCallback(physical_address_start, length,
                                                 exact_range);
}

MetalPrimitiveProcessor::ConvertedIndices
MetalPrimitiveProcessor::ConvertPrimitives(
    xenos::PrimitiveType xenos_type,
    const void* index_data, uint32_t index_count,
    xenos::IndexFormat index_format, xenos::Endian endian) {
  ConvertedIndices result;
  result.index_count = 0;
  result.needs_conversion = false;

  // Helper: read an index with endian swap.
  auto read_index = [&](uint32_t i) -> uint32_t {
    if (index_format == xenos::IndexFormat::kInt16) {
      uint16_t idx;
      std::memcpy(&idx, static_cast<const uint8_t*>(index_data) + i * 2, 2);
      if (endian == xenos::Endian::k8in16 ||
          endian == xenos::Endian::k8in32) {
        idx = __builtin_bswap16(idx);
      }
      return uint32_t(idx);
    } else {
      uint32_t idx;
      std::memcpy(&idx, static_cast<const uint8_t*>(index_data) + i * 4, 4);
      if (endian == xenos::Endian::k8in32) {
        idx = __builtin_bswap32(idx);
      } else if (endian == xenos::Endian::k8in16) {
        idx = ((idx & 0xFF00FF00) >> 8) | ((idx & 0x00FF00FF) << 8);
      } else if (endian == xenos::Endian::k16in32) {
        idx = ((idx & 0xFFFF0000) >> 16) | ((idx & 0x0000FFFF) << 16);
      }
      return idx;
    }
  };

  switch (xenos_type) {
    case xenos::PrimitiveType::kTriangleList:
      // Metal supports triangle lists natively.
      result.metal_primitive_type = MTLPrimitiveTypeTriangle;
      result.needs_conversion = false;
      result.index_count = index_count;
      break;

    case xenos::PrimitiveType::kTriangleStrip:
      // Metal supports triangle strips natively.
      result.metal_primitive_type = MTLPrimitiveTypeTriangleStrip;
      result.needs_conversion = false;
      result.index_count = index_count;
      break;

    case xenos::PrimitiveType::kTriangleFan:
      // Metal does NOT support triangle fans.
      // Convert: fan with N vertices → (N-2) triangles.
      if (index_count < 3) {
        result.index_count = 0;
        return result;
      }
      result.needs_conversion = true;
      result.metal_primitive_type = MTLPrimitiveTypeTriangle;
      result.index_count = (index_count - 2) * 3;
      result.indices.reserve(result.index_count);
      if (index_data) {
        uint32_t center = read_index(0);
        for (uint32_t i = 1; i < index_count - 1; ++i) {
          result.indices.push_back(center);
          result.indices.push_back(read_index(i));
          result.indices.push_back(read_index(i + 1));
        }
      } else {
        // Auto-indexed fan.
        for (uint32_t i = 1; i < index_count - 1; ++i) {
          result.indices.push_back(0);
          result.indices.push_back(i);
          result.indices.push_back(i + 1);
        }
      }
      break;

    case xenos::PrimitiveType::kLineList:
      result.metal_primitive_type = MTLPrimitiveTypeLine;
      result.needs_conversion = false;
      result.index_count = index_count;
      break;

    case xenos::PrimitiveType::kLineStrip:
      result.metal_primitive_type = MTLPrimitiveTypeLineStrip;
      result.needs_conversion = false;
      result.index_count = index_count;
      break;

    case xenos::PrimitiveType::kPointList:
      result.metal_primitive_type = MTLPrimitiveTypePoint;
      result.needs_conversion = false;
      result.index_count = index_count;
      break;

    case xenos::PrimitiveType::kQuadList:
      // Metal does NOT support quads.
      // Convert: each quad (4 vertices) → 2 triangles (6 indices).
      if (index_count < 4) {
        result.index_count = 0;
        return result;
      }
      result.needs_conversion = true;
      result.metal_primitive_type = MTLPrimitiveTypeTriangle;
      result.index_count = (index_count / 4) * 6;
      result.indices.reserve(result.index_count);
      for (uint32_t i = 0; i + 3 < index_count; i += 4) {
        uint32_t v0, v1, v2, v3;
        if (index_data) {
          v0 = read_index(i);
          v1 = read_index(i + 1);
          v2 = read_index(i + 2);
          v3 = read_index(i + 3);
        } else {
          v0 = i; v1 = i + 1; v2 = i + 2; v3 = i + 3;
        }
        // Quad: v0-v1-v2-v3 → triangles: v0-v1-v2, v0-v2-v3
        result.indices.push_back(v0);
        result.indices.push_back(v1);
        result.indices.push_back(v2);
        result.indices.push_back(v0);
        result.indices.push_back(v2);
        result.indices.push_back(v3);
      }
      break;

    case xenos::PrimitiveType::kRectangleList:
      if (REXCVAR_GET(metal_rectlist_vs_expand)) {
        static bool rect_expand_legacy_warned = false;
        if (!rect_expand_legacy_warned) {
          rect_expand_legacy_warned = true;
          XELOGW(
              "MetalPrimitiveProcessor: metal_rectlist_vs_expand is enabled, "
              "but translated-shader rectangle VS expansion is not wired yet; "
              "using temporary half-quad fallback");
        }
      }
      // Metal does NOT support rectangle lists.
      // Xenos kRectangleList provides 3 vertices per rect (two corners +
      // a third defining the opposite edge). The fourth vertex must be
      // synthesized as v3 = v1 + v2 - v0. Proper implementation requires
      // a vertex shader expansion pass or geometry shader emulation.
      //
      // TODO(metal): Implement vertex shader–based rect expansion.
      // For now, emit only the single triangle (v0, v1, v2) per rect.
      // This renders half the quad — incorrect but avoids the misleading
      // reversed-winding duplicate that was here before.
      if (index_count < 3) {
        result.index_count = 0;
        return result;
      }
      result.needs_conversion = true;
      result.metal_primitive_type = MTLPrimitiveTypeTriangle;
      result.index_count = (index_count / 3) * 3;
      result.indices.reserve(result.index_count);
      for (uint32_t i = 0; i + 2 < index_count; i += 3) {
        uint32_t v0, v1, v2;
        if (index_data) {
          v0 = read_index(i);
          v1 = read_index(i + 1);
          v2 = read_index(i + 2);
        } else {
          v0 = i; v1 = i + 1; v2 = i + 2;
        }
        // First triangle only — second triangle needs synthesized v3.
        result.indices.push_back(v0);
        result.indices.push_back(v1);
        result.indices.push_back(v2);
      }
      static bool rect_half_quad_warned = false;
      if (!rect_half_quad_warned) {
        rect_half_quad_warned = true;
        XELOGW("MetalPrimitiveProcessor: kRectangleList rendered as half-quad "
               "(vertex shader rect expansion not yet implemented)");
      }
      break;

    default:
      XELOGW("MetalPrimitiveProcessor: Unsupported primitive type {}",
             uint32_t(xenos_type));
      result.metal_primitive_type = MTLPrimitiveTypeTriangle;
      result.needs_conversion = false;
      result.index_count = index_count;
      break;
  }

  // If we don't need conversion but have index data that requires endian
  // swapping, copy the indices into the output buffer with the swap applied.
  // For native endian (kNone), skip the copy entirely — the caller can use
  // the original index data directly from shared memory.
  if (!result.needs_conversion && index_data && endian != xenos::Endian::kNone) {
    result.indices.reserve(index_count);
    for (uint32_t i = 0; i < index_count; ++i) {
      result.indices.push_back(read_index(i));
    }
    result.needs_conversion = true;  // We have converted indices to use
  }

  return result;
}

}  // namespace rex::graphics::metal
