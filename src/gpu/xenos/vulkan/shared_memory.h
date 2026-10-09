/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "gpu/xenos/shared_memory.h"
#include <rex/memory.h>
#include "gpu/xenos/host/vulkan/upload_buffer_pool.h"

namespace rex::gpu::vulkan {

class VulkanCommandProcessor;

class VulkanSharedMemory : public SharedMemory {
 public:
  VulkanSharedMemory(VulkanCommandProcessor& command_processor, runtime::Memory& memory,
                     VkPipelineStageFlags guest_shader_pipeline_stages);
  ~VulkanSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void CompletedSubmissionUpdated();
  void EndSubmission();

  enum class Usage {
    /** Index buffer, vfetch, compute read, transfer source. */
    kRead,
    /** Index buffer, vfetch, memexport. */
    kGuestDrawReadWrite,
    kComputeWrite,
    kTransferDestination,
  };
  /**
   * Inserts a pipeline barrier for the target usage and orders consecutive read write accesses.
   *
   * @param usage Usage to transition to.
   * @param written_range Byte offset and length the usage writes.
   */
  void Use(Usage usage, std::pair<u32, u32> written_range = {});

  VkBuffer buffer() const { return buffer_; }

 protected:
  bool AllocateSparseHostGpuMemoryRange(u32 offset_allocations, u32 length_allocations) override;

  bool UploadRanges(const std::vector<std::pair<u32, u32>>& upload_page_ranges) override;

 private:
  void GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                     VkAccessFlags& access_mask) const;

  VulkanCommandProcessor& command_processor_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  VkBuffer buffer_ = VK_NULL_HANDLE;
  u32 buffer_memory_type_;
  /** Single allocation when non sparse, every allocation so far when sparse. */
  std::vector<VkDeviceMemory> buffer_memory_;

  Usage last_usage_;
  std::pair<u32, u32> last_written_range_;

  std::unique_ptr<gpu::vulkan::VulkanUploadBufferPool> upload_buffer_pool_;
  std::vector<VkBufferCopy> upload_regions_;
};

}  // namespace rex::gpu::vulkan
