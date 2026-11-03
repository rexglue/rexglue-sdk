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

#include <cstdint>

#include <rex/assert.h>
#include <rex/logging/log.h>
#include "gpu/xenos/host/vulkan/submission_tracker.h"
#include "gpu/xenos/host/vulkan/objects.h"

namespace rex {
namespace gpu {
namespace vulkan {

void VulkanSubmissionTracker::FenceAcquisition::Release() noexcept {
  if (!submission_tracker_) {
    return;
  }
  REX_ASSERT(submission_tracker_->fence_acquired_ == fence_);
  if (fence_ != VK_NULL_HANDLE) {
    log::NoThrow(log::gpu, [this] {
      if (signal_failed_) {
        submission_tracker_->fences_reclaimed_.push_back(fence_);
      } else {
        submission_tracker_->fences_pending_.emplace_back(submission_tracker_->submission_current_,
                                                          fence_);
      }
    });
    submission_tracker_->fence_acquired_ = VK_NULL_HANDLE;
  }
  ++submission_tracker_->submission_current_;
  submission_tracker_ = nullptr;
  fence_ = VK_NULL_HANDLE;
  signal_failed_ = false;
}

void VulkanSubmissionTracker::Shutdown() {
  AwaitAllSubmissionsCompletion();
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();
  for (VkFence fence : fences_reclaimed_) {
    dfn.vkDestroyFence(device, fence, nullptr);
  }
  fences_reclaimed_.clear();
  for (const std::pair<u64, VkFence>& fence_pair : fences_pending_) {
    dfn.vkDestroyFence(device, fence_pair.second, nullptr);
  }
  fences_pending_.clear();
  REX_ASSERT(fence_acquired_ == VK_NULL_HANDLE);
  objects::DestroyAndNullHandle(dfn.vkDestroyFence, device, fence_acquired_);
}

void VulkanSubmissionTracker::FenceAcquisition::SubmissionFailedOrDiscarded() {
  if (!submission_tracker_) {
    return;
  }
  REX_ASSERT(submission_tracker_->fence_acquired_ == fence_);
  if (fence_ != VK_NULL_HANDLE) {
    submission_tracker_->fences_reclaimed_.push_back(fence_);
  }
  submission_tracker_->fence_acquired_ = VK_NULL_HANDLE;
  fence_ = VK_NULL_HANDLE;
  submission_tracker_ = VK_NULL_HANDLE;
}

u64 VulkanSubmissionTracker::UpdateAndGetCompletedSubmission() {
  if (!fences_pending_.empty()) {
    const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
    const VkDevice device = vulkan_device_->GetDevice();
    while (!fences_pending_.empty()) {
      const std::pair<u64, VkFence>& pending_pair = fences_pending_.front();
      REX_ASSERT(pending_pair.first > submission_completed_on_gpu_);
      if (dfn.vkGetFenceStatus(device, pending_pair.second) != VK_SUCCESS) {
        break;
      }
      fences_reclaimed_.push_back(pending_pair.second);
      submission_completed_on_gpu_ = pending_pair.first;
      fences_pending_.pop_front();
    }
  }
  return submission_completed_on_gpu_;
}

bool VulkanSubmissionTracker::AwaitSubmissionCompletion(u64 submission_index) {
  REX_ASSERT(submission_index <= submission_current_);
  size_t reclaim_end = fences_pending_.size();
  if (reclaim_end) {
    const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
    const VkDevice device = vulkan_device_->GetDevice();
    while (reclaim_end) {
      const std::pair<u64, VkFence>& pending_pair = fences_pending_[reclaim_end - 1];
      REX_ASSERT(pending_pair.first > submission_completed_on_gpu_);
      if (pending_pair.first <= submission_index) {
        if (dfn.vkWaitForFences(device, 1, &pending_pair.second, VK_TRUE, UINT64_MAX) ==
            VK_SUCCESS) {
          break;
        }
      }
      if (dfn.vkGetFenceStatus(device, pending_pair.second) == VK_SUCCESS) {
        break;
      }
      --reclaim_end;
    }
    if (reclaim_end) {
      submission_completed_on_gpu_ = fences_pending_[reclaim_end - 1].first;
      for (; reclaim_end; --reclaim_end) {
        fences_reclaimed_.push_back(fences_pending_.front().second);
        fences_pending_.pop_front();
      }
    }
  }
  return submission_completed_on_gpu_ == submission_index;
}

VulkanSubmissionTracker::FenceAcquisition
VulkanSubmissionTracker::AcquireFenceToAdvanceSubmission() {
  REX_ASSERT(fence_acquired_ == VK_NULL_HANDLE);
  UpdateAndGetCompletedSubmission();
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();
  if (!fences_reclaimed_.empty()) {
    VkFence reclaimed_fence = fences_reclaimed_.back();
    if (dfn.vkResetFences(device, 1, &reclaimed_fence) == VK_SUCCESS) {
      fence_acquired_ = fences_reclaimed_.back();
      fences_reclaimed_.pop_back();
    }
  }
  if (fence_acquired_ == VK_NULL_HANDLE) {
    VkFenceCreateInfo fence_create_info;
    fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_create_info.pNext = nullptr;
    fence_create_info.flags = 0;
    // May fail, and FenceAcquisition handles a null fence.
    dfn.vkCreateFence(device, &fence_create_info, nullptr, &fence_acquired_);
  }
  return FenceAcquisition(*this, fence_acquired_);
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
