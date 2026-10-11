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

#pragma once

#include <memory>

#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/** Guest layout of a kernel semaphore. */
struct X_KSEMAPHORE {
  X_DISPATCH_HEADER header; /**< Dispatch header. */
  be_u32 limit;             /**< Largest count the semaphore reaches. */
};
REX_STATIC_ASSERT_SIZE(X_KSEMAPHORE, 0x14);

/** The host state of a semaphore object. */
struct SemaphoreObject {
  /**
   * @param initial_count The starting count.
   * @param maximum_count_in The count the semaphore cannot exceed.
   */
  REX_RUNTIME_API SemaphoreObject(i32 initial_count, i32 maximum_count_in);

  /**
   * @param header The guest semaphore whose signal state and limit initialize the object.
   */
  REX_RUNTIME_API explicit SemaphoreObject(const X_KSEMAPHORE& header);

  /** Host semaphore that carries the count and wakes waiters. */
  PropagateConst<std::unique_ptr<rex::platform::Semaphore>> semaphore;
  u32 maximum_count = 0; /**< Maximum count the semaphore was created with. */

  /**
   * Releases a semaphore.
   *
   * @param release_count The count to add.
   * @param out_previous_count Receives the count before the release, may be null.
   * @return True when the release stayed within the maximum count.
   */
  REX_RUNTIME_API bool Release(i32 release_count, i32* out_previous_count);
};

}  // namespace rex::runtime
