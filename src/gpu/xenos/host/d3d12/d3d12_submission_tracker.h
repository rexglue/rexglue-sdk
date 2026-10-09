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

#include "gpu/xenos/host/d3d12/d3d12_api.h"
#include <rex/types.h>

namespace rex::gpu::d3d12 {

/**
 * GPU to CPU fence wrapper.
 *
 * Handles an uninitialized or shut down fence, dropped submissions and queue transfers so signals
 * stay ordered. The current submission index is associated with object usage to release objects
 * once the GPU no longer references them. 0 is the never referenced submission index. The index
 * timeline survives Shutdown and Initialize, so indices can be given to clients unaware of the
 * tracker lifetime.
 */
class D3D12SubmissionTracker : public rex::NonCopyable {
 public:
  D3D12SubmissionTracker() = default;
  ~D3D12SubmissionTracker() { Shutdown(); }

  /**
   * Creates the fence and takes a reference to the queue.
   *
   * @param device Device creating the fence.
   * @param queue Queue signaling the fence, null when set dynamically.
   * @return True on success.
   */
  bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
  void Shutdown();

  UINT64 GetCurrentSubmission() const { return submission_current_; }
  /**
   * Returns the last completed submission index.
   *
   * May be lower than a value awaited by AwaitSubmissionCompletion if it returned false.
   *
   * @return The completed submission index.
   */
  UINT64 GetCompletedSubmission() const {
    return fence_ ? fence_->GetCompletedValue() : (GetCurrentSubmission() - 1);
  }

  /**
   * Reports whether the expected GPU signal was reached rather than a fallback condition.
   *
   * Waiting on a submission index beyond the last queued signal retries the signal and waits for
   * the closest successful one. When false, GetCompletedSubmission may be below
   * submission_index.
   *
   * @param submission_index Submission to wait for, at most the current submission.
   * @return True when the expected signal was reached.
   */
  bool AwaitSubmissionCompletion(UINT64 submission_index);
  bool AwaitAllSubmissionsCompletion() {
    return AwaitSubmissionCompletion(GetCurrentSubmission() - 1);
  }

  /**
   * Increments the current submission index and tries to enqueue the fence signal.
   *
   * Waits on submissions without an enqueued signal are bounded, so clients need not revert
   * submission indices associated with object usage.
   *
   * @return True when the signal was enqueued.
   */
  bool NextSubmission();

  /**
   * Retries enqueueing the signal after a failed NextSubmission.
   *
   * @return True when the signal is enqueued.
   */
  bool TrySignalEnqueueing();

 private:
  UINT64 submission_current_ = 1;
  UINT64 submission_signal_queued_ = 0;
  HANDLE fence_completion_event_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
};

}  // namespace rex::gpu::d3d12
