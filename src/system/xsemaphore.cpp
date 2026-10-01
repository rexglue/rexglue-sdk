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

SemaphoreObject::SemaphoreObject(i32 initial_count, i32 maximum_count_in)
    : maximum_count(static_cast<u32>(maximum_count_in)) {
  semaphore = rex::platform::Semaphore::Create(initial_count, maximum_count_in);
}

SemaphoreObject::SemaphoreObject(const X_KSEMAPHORE& header) : maximum_count(header.limit) {
  semaphore = rex::platform::Semaphore::Create(header.header.signal_state, header.limit);
}

bool SemaphoreRelease(SemaphoreObject& semaphore, i32 release_count, i32* out_previous_count) {
  i32 previous_count = 0;
  const bool success = semaphore.semaphore->Release(release_count, &previous_count);
  if (out_previous_count) {
    *out_previous_count = previous_count;
  }
  return success;
}

}  // namespace rex::system
