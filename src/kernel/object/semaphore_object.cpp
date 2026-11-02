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

#include <algorithm>
#include <limits>

#include <rex/logging/log.h>
#include <rex/kernel/object/semaphore_object.h>
#include <rex/platform/sync.h>

namespace rex::kernel {

SemaphoreObject::SemaphoreObject(i32 initial_count, i32 maximum_count_in)
    : maximum_count(static_cast<u32>(maximum_count_in)) {
  semaphore = rex::platform::Semaphore::Create(initial_count, maximum_count_in);
}

SemaphoreObject::SemaphoreObject(const X_KSEMAPHORE& header) {
  constexpr u32 kMaxCount = static_cast<u32>(std::numeric_limits<i32>::max());
  const u32 limit = std::clamp(u32{header.limit}, 1u, kMaxCount);
  const u32 count = std::min(u32{header.header.signal_state}, limit);
  maximum_count = limit;
  semaphore = rex::platform::Semaphore::Create(static_cast<i32>(count), static_cast<i32>(limit));
}

bool SemaphoreObject::Release(i32 release_count, i32* out_previous_count) {
  i32 previous_count = 0;
  const bool success = semaphore->Release(release_count, &previous_count);
  if (out_previous_count) {
    *out_previous_count = previous_count;
  }
  return success;
}

}  // namespace rex::kernel
