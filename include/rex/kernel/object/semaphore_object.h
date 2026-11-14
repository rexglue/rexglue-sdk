/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/object/semaphore_object.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2015).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 */

#pragma once

#include <memory>

#include <rex/assert.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/types/list.h>
#include <rex/kernel/types/memory.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/types/thread.h>
#include <rex/module.h>
#include <rex/platform.h>
#include <rex/platform/sync.h>
#include <rex/platform/thread.h>
#include <rex/platform/timer.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::kernel {

struct SemaphoreObject {
  /**
   * Constructs a semaphore from a starting count and a maximum count.
   *
   * @param maximum_count_in The count the semaphore cannot exceed.
   */
  REX_KERNEL_API SemaphoreObject(i32 initial_count, i32 maximum_count_in);

  /**
   * Constructs a semaphore from a guest semaphore.
   *
   * @param header The guest semaphore whose signal state and limit initialize the object.
   */
  REX_KERNEL_API explicit SemaphoreObject(const X_KSEMAPHORE& header);

  /** Host semaphore that carries the count and wakes waiters. */
  PublishedPtr<rex::platform::Semaphore> semaphore;
  u32 maximum_count = 0;

  /**
   * Releases a semaphore.
   *
   * @param out_previous_count Receives the count before the release, may be null.
   * @return True when the release stayed within the maximum count.
   */
  REX_KERNEL_API bool Release(i32 release_count, i32* out_previous_count);
};

}  // namespace rex::kernel
