#pragma once
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

#include <memory>

#include <rex/platform/thread.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

struct X_KSEMAPHORE {
  X_DISPATCH_HEADER header;
  rex::be<uint32_t> limit;
};
static_assert_size(X_KSEMAPHORE, 0x14);

/** The host state of a semaphore object. */
struct SemaphoreObject {
  /**
   * @param initial_count The starting count.
   * @param maximum_count_in The count the semaphore cannot exceed.
   */
  SemaphoreObject(i32 initial_count, i32 maximum_count_in);

  /**
   * @param header The guest semaphore whose signal state and limit seed the object.
   */
  explicit SemaphoreObject(const X_KSEMAPHORE& header);

  std::unique_ptr<rex::platform::Semaphore> semaphore;
  u32 maximum_count = 0;
};

/**
 * Releases a semaphore.
 *
 * @param semaphore The semaphore.
 * @param release_count The count to add.
 * @param out_previous_count Receives the count before the release, may be null.
 * @return True when the release stayed within the maximum count.
 */
bool SemaphoreRelease(SemaphoreObject& semaphore, i32 release_count, i32* out_previous_count);

}  // namespace rex::runtime
