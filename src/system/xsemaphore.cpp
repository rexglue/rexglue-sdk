/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/logging.h>
#include <rex/system/xsemaphore.h>

namespace rex::system {

XSemaphore::XSemaphore(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSemaphore::~XSemaphore() = default;

bool XSemaphore::Initialize(int32_t initial_count, int32_t maximum_count) {
  assert_false(semaphore_);

  CreateNative(sizeof(X_KSEMAPHORE));

  maximum_count_ = maximum_count;
  semaphore_ = rex::platform::Semaphore::Create(initial_count, maximum_count);
  return !!semaphore_;
}

bool XSemaphore::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  assert_false(semaphore_);

  auto semaphore = reinterpret_cast<X_KSEMAPHORE*>(native_ptr);
  maximum_count_ = semaphore->limit;
  semaphore_ = rex::platform::Semaphore::Create(semaphore->header.signal_state, semaphore->limit);
  return !!semaphore_;
}

bool XSemaphore::ReleaseSemaphore(int32_t release_count, int32_t* out_previous_count) {
  int32_t previous_count = 0;
  bool success = semaphore_->Release(release_count, &previous_count);
  if (out_previous_count) {
    *out_previous_count = previous_count;
  }
  return success;
}

}  // namespace rex::system
