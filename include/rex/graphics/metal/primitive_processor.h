/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/xenos.h>

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

namespace rex::graphics::metal {

class MetalCommandProcessor;

class MetalPrimitiveProcessor : public PrimitiveProcessor {
 public:
  MetalPrimitiveProcessor(const RegisterFile& register_file,
                          memory::Memory& memory,
                          TraceWriter& trace_writer,
                          SharedMemory& shared_memory,
                          MetalCommandProcessor& command_processor);
  ~MetalPrimitiveProcessor() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

#ifdef __OBJC__
  id<MTLBuffer> GetBuiltinIndexBuffer(size_t handle,
                                      uint64_t& offset_bytes_out) const;
  id<MTLBuffer> GetConvertedIndexBuffer(size_t handle,
                                        uint64_t& offset_bytes_out,
                                        uint64_t* size_bytes_out = nullptr) const;
#else
  void* GetBuiltinIndexBuffer(size_t handle, uint64_t& offset_bytes_out) const;
  void* GetConvertedIndexBuffer(size_t handle,
                                uint64_t& offset_bytes_out,
                                uint64_t* size_bytes_out = nullptr) const;
#endif

  void CompletedSubmissionUpdated();
  void BeginSubmission();
  void BeginFrame();

  // Called when memory is invalidated.
  void MemoryInvalidationCallback(uint32_t physical_address_start,
                                  uint32_t length, bool exact_range);

  // Primitive type conversion result.
  struct ConvertedIndices {
    std::vector<uint32_t> indices;
#ifdef __OBJC__
    MTLPrimitiveType metal_primitive_type;
#endif
    uint32_t index_count;
    bool needs_conversion;
  };

  // Convert Xenos primitives to Metal-compatible primitives.
  // Handles triangle fans, quads, rects -> triangle lists.
  ConvertedIndices ConvertPrimitives(
      xenos::PrimitiveType xenos_type,
      const void* index_data, uint32_t index_count,
      xenos::IndexFormat index_format, xenos::Endian endian);

 protected:
  bool InitializeBuiltinIndexBuffer(
      size_t size_bytes, std::function<void(void*)> fill_callback) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(
      xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
      uint32_t coalignment_original_address,
      size_t& backend_handle_out) override;

 private:
  MetalCommandProcessor& command_processor_;

  struct ConvertedIndexBufferBinding {
#ifdef __OBJC__
    id<MTLBuffer> buffer = nil;
#else
    void* buffer = nullptr;
#endif
    uint64_t offset_bytes = 0;
    uint64_t size_bytes = 0;
  };
  std::vector<ConvertedIndexBufferBinding> converted_index_buffers_;
  uint64_t current_frame_ = 0;

#ifdef __OBJC__
  id<MTLBuffer> builtin_index_buffer_ = nil;
#else
  void* builtin_index_buffer_ = nullptr;
#endif
  size_t builtin_index_buffer_size_ = 0;

  struct FrameIndexBuffer {
#ifdef __OBJC__
    id<MTLBuffer> buffer = nil;
#else
    void* buffer = nullptr;
#endif
    size_t size = 0;
    uint64_t last_frame_used = 0;
  };
  std::vector<FrameIndexBuffer> frame_index_buffers_;
};

}  // namespace rex::graphics::metal
