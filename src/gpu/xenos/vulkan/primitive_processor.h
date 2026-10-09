/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <memory>
#include <utility>

#include <rex/assert.h>
#include "gpu/xenos/primitive_processor.h"
#include "gpu/xenos/host/vulkan/upload_buffer_pool.h"

namespace rex::gpu::vulkan {

class VulkanCommandProcessor;

class VulkanPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  VulkanPrimitiveProcessor(const xenos::RegisterFile& register_file, runtime::Memory& memory,
                           SharedMemory& shared_memory, VulkanCommandProcessor& command_processor)
      : PrimitiveProcessor(register_file, memory, shared_memory),
        command_processor_(command_processor) {}
  ~VulkanPrimitiveProcessor();

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() { frame_index_buffer_pool_->ClearCache(); }

  void CompletedSubmissionUpdated();
  void BeginSubmission();
  void BeginFrame();
  void EndSubmission();
  void EndFrame();

  std::pair<VkBuffer, VkDeviceSize> GetBuiltinIndexBuffer(size_t handle) const {
    REX_ASSERT(builtin_index_buffer_ != nullptr);
    return std::make_pair(builtin_index_buffer_,
                          VkDeviceSize(GetBuiltinIndexBufferOffsetBytes(handle)));
  }
  std::pair<VkBuffer, VkDeviceSize> GetConvertedIndexBuffer(size_t handle) const {
    return frame_index_buffers_[handle];
  }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;

  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  VulkanCommandProcessor& command_processor_;

  VkDeviceSize builtin_index_buffer_size_ = 0;
  VkBuffer builtin_index_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory builtin_index_buffer_memory_ = VK_NULL_HANDLE;
  /** Staging buffer copied to builtin_index_buffer_ at the start of the first submission. */
  VkBuffer builtin_index_buffer_upload_ = VK_NULL_HANDLE;
  VkDeviceMemory builtin_index_buffer_upload_memory_ = VK_NULL_HANDLE;
  /** UINT64_MAX when the upload is pending for the first submission. */
  uint64_t builtin_index_buffer_upload_submission_ = UINT64_MAX;

  std::unique_ptr<gpu::vulkan::VulkanUploadBufferPool> frame_index_buffer_pool_;
  /** Indexed by the backend handles. */
  std::deque<std::pair<VkBuffer, VkDeviceSize>> frame_index_buffers_;
};

}  // namespace rex::gpu::vulkan
