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

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/logging/log_category.h>
#include <rex/types.h>
#include <rex/utility.h>
#include "gpu/xenos/host/vulkan/vulkan_device.h"

namespace rex {
namespace gpu {
namespace vulkan {

/**
 * Tracks GPU submissions with a fence.
 *
 * The submission index tags object usage so objects are released once the GPU no longer uses
 * them. Index 0 is never referenced. The timeline survives Shutdown. To move to another queue,
 * call AwaitAllSubmissionsCompletion before the first submission on the new one.
 */
class VulkanSubmissionTracker : public rex::NonCopyable {
 public:
  class FenceAcquisition : public rex::MoveOnly {
   public:
    FenceAcquisition() = default;
    FenceAcquisition(VulkanSubmissionTracker& submission_tracker, VkFence fence)
        : submission_tracker_(&submission_tracker), fence_(fence) {}
    FenceAcquisition(FenceAcquisition&& fence_acquisition) noexcept
        : submission_tracker_(std::exchange(fence_acquisition.submission_tracker_, nullptr)),
          fence_(std::exchange(fence_acquisition.fence_, VK_NULL_HANDLE)),
          signal_failed_(std::exchange(fence_acquisition.signal_failed_, false)) {}
    FenceAcquisition& operator=(FenceAcquisition&& fence_acquisition) noexcept {
      if (this == &fence_acquisition) {
        return *this;
      }
      Release();
      submission_tracker_ = std::exchange(fence_acquisition.submission_tracker_, nullptr);
      fence_ = std::exchange(fence_acquisition.fence_, VK_NULL_HANDLE);
      signal_failed_ = std::exchange(fence_acquisition.signal_failed_, false);
      return *this;
    }
    ~FenceAcquisition() { Release(); }

    /** Returns the unsignaled fence, or null when creating or resetting it failed. */
    VkFence GetFence() { return fence_; }

    /**
     * Marks the submission as failed or discarded, so releasing it does not increment the index.
     *
     * Applies when vkQueueSubmit failed or the submission was not committed, so commands with long
     * term effects can be retried under the same index.
     */
    void SubmissionFailedOrDiscarded();
    /**
     * Marks that the separate fence only submission failed after the command buffer submission
     * succeeded. The commands still execute, but the tracker does not wait for the fence.
     */
    void MarkFenceSignalFailed() { signal_failed_ = true; }

   private:
    void Release() noexcept;

    /** Null once moved to another FenceAcquisition, which then holds no fence. */
    VulkanSubmissionTracker* submission_tracker_ = nullptr;
    VkFence fence_ = VK_NULL_HANDLE;
    bool signal_failed_ = false;
  };

  VulkanSubmissionTracker(const VulkanDevice* vulkan_device) : vulkan_device_(vulkan_device) {
    REX_ASSERT(vulkan_device != nullptr);
  }

  ~VulkanSubmissionTracker() {
    log::NoThrow(log::gpu, [this] { Shutdown(); });
  }

  void Shutdown();

  u64 GetCurrentSubmission() const { return submission_current_; }
  u64 UpdateAndGetCompletedSubmission();

  /**
   * Reports whether the expected GPU signal was reached rather than a fallback condition.
   *
   * Needed where completeness matters, such as downloading rather than destroying. When false,
   * UpdateAndGetCompletedSubmission may still return less than submission_index. The index must
   * not exceed the current submission.
   *
   * @return True when the expected GPU signal was reached.
   */
  bool AwaitSubmissionCompletion(u64 submission_index);
  bool AwaitAllSubmissionsCompletion() {
    return AwaitSubmissionCompletion(submission_current_ - 1);
  }

  [[nodiscard]] FenceAcquisition AcquireFenceToAdvanceSubmission();

 private:
  const VulkanDevice* vulkan_device_;
  u64 submission_current_ = 1;
  /** Last submission with a successful fence signal and a successful fence wait or query. */
  u64 submission_completed_on_gpu_ = 0;
  /** Cycles reclaimed, acquired, pending, reclaimed. A discarded submission skips pending. */
  VkFence fence_acquired_ = VK_NULL_HANDLE;
  /** Ordered by the submission index, the first pair member. */
  std::deque<std::pair<u64, VkFence>> fences_pending_;
  /** Fences are reclaimed when awaiting or refreshing the completed value. */
  std::vector<VkFence> fences_reclaimed_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
