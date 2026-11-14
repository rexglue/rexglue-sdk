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

#include <rex/assert.h>
#include <rex/logging/log_category.h>
#include "gpu/xenos/host/d3d12/d3d12_submission_tracker.h"

namespace rex::gpu::d3d12 {

bool D3D12SubmissionTracker::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
  Shutdown();
  fence_completion_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (!fence_completion_event_) {
    REXUI_ERROR("D3D12SubmissionTracker: Failed to create the fence completion event");
    Shutdown();
    return false;
  }
  if (FAILED(device->CreateFence(submission_current_ - 1, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&fence_)))) {
    REXUI_ERROR("D3D12SubmissionTracker: Failed to create the fence");
    Shutdown();
    return false;
  }
  queue_ = queue;
  submission_signal_queued_ = submission_current_ - 1;
  return true;
}

void D3D12SubmissionTracker::Shutdown() {
  AwaitAllSubmissionsCompletion();
  queue_.Reset();
  fence_.Reset();
  if (fence_completion_event_) {
    CloseHandle(fence_completion_event_);
    fence_completion_event_ = nullptr;
  }
}

bool D3D12SubmissionTracker::AwaitSubmissionCompletion(UINT64 submission_index) {
  if (!fence_ || !fence_completion_event_) {
    return false;
  }
  REX_ASSERT(submission_index <= submission_current_);
  UINT64 fence_value = submission_index;
  if (submission_index > submission_signal_queued_) {
    TrySignalEnqueueing();
    fence_value = submission_signal_queued_;
  }
  if (fence_->GetCompletedValue() < fence_value) {
    if (FAILED(fence_->SetEventOnCompletion(fence_value, fence_completion_event_))) {
      return false;
    }
    if (WaitForSingleObject(fence_completion_event_, INFINITE) != WAIT_OBJECT_0) {
      return false;
    }
  }
  return fence_value == submission_index;
}

bool D3D12SubmissionTracker::NextSubmission() {
  ++submission_current_;
  REX_ASSERT(queue_ != nullptr);
  REX_ASSERT(fence_ != nullptr);
  return TrySignalEnqueueing();
}

bool D3D12SubmissionTracker::TrySignalEnqueueing() {
  if (submission_signal_queued_ + 1 >= submission_current_) {
    return true;
  }
  if (!queue_ || !fence_) {
    return false;
  }
  if (FAILED(queue_->Signal(fence_.Get(), submission_current_ - 1))) {
    return false;
  }
  submission_signal_queued_ = submission_current_ - 1;
  return true;
}

}  // namespace rex::gpu::d3d12
