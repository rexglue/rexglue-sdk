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
#include <rex/logging.h>
#include <rex/no_throw.h>
#include <rex/types.h>
#include "gpu/xenos/host/vulkan/device.h"

namespace rex {
namespace gpu {
namespace vulkan {

/**
 * Fence wrapper that handles an uninitialized or shut down fence and failed or dropped
 * submissions.
 *
 * The current submission index tags object usage so objects are released once the GPU no longer
 * references them. 0 is the never referenced submission index. The timeline survives Shutdown,
 * so indices can go to clients unaware of the tracker lifetime. To move to another queue, call
 * AwaitAllSubmissionsCompletion before the first submission on the new one, so its first signal
 * follows the last signal on the old one.
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

    /**
     * Returns the fence, in the unsignaled state.
     *
     * @return The fence, or null when creating or resetting it failed.
     */
    VkFence fence() { return fence_; }

    /**
     * Marks the submission as failed or dropped, so releasing it does not increment the
     * submission index.
     *
     * Applies when vkQueueSubmit failed or the submission was not committed, for example to retry
     * commands with long term effects such as copies or image layout changes under the same index.
     */
    void SubmissionFailedOrDropped();
    /**
     * Marks that the fence only submission failed after the command buffer submission succeeded.
     *
     * Applies when the fence is signaled in a separate submission, for example to signal multiple
     * fences. Commands with long term effects still execute, but the fence stays unsignaled so the
     * tracker does not wait for it.
     */
    void SubmissionSucceededSignalFailed() { signal_failed_ = true; }

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
   * Needed where stronger completeness is required, such as downloading rather than destroying.
   * When false, GetCompletedSubmission may still return less than submission_index. The index
   * must not exceed the current submission. Awaiting the current submission is valid after a
   * failed or dropped submission that did not increment the index, to release the objects it
   * referenced, for example on shutdown. A failed signal after a successful submission can still
   * race, so the closest successful signal is the best approximation. Waiting starts from the
   * most recent fence, as a vkQueueSubmit fence signal covers all earlier commands in submission
   * order.
   *
   * @param submission_index The submission to await.
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
  /**
   * A fence goes from reclaimed (or created when none) to acquired, pending, then reclaimed.
   *
   * A submission dropped while acquired goes straight from acquired to reclaimed.
   */
  VkFence fence_acquired_ = VK_NULL_HANDLE;
  /** Ordered by the submission index, the first pair member. */
  std::deque<std::pair<u64, VkFence>> fences_pending_;
  /** Fences are reclaimed when awaiting or refreshing the completed value. */
  std::vector<VkFence> fences_reclaimed_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
