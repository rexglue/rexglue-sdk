#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <memory>

#include <rex/platform/thread.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/** The host state of a mutant object. */
struct MutantObject {
  MutantObject() = default;

  /**
   * @param initial_owner True when the creating thread owns the mutant at once.
   */
  explicit MutantObject(bool initial_owner);

  std::unique_ptr<rex::platform::Mutant> mutant;
  std::atomic<u32> owner_thread_id{0};
};

/**
 * Releases a mutant held by the current thread.
 *
 * @param mutant The mutant.
 * @param abandon True to abandon the mutant, which is not implemented.
 * @return X_STATUS_SUCCESS, or X_STATUS_MUTANT_NOT_OWNED when the caller does not own it.
 */
X_STATUS MutantRelease(MutantObject& mutant, bool abandon);

/**
 * Records the current thread as the owner after a successful wait.
 *
 * @param mutant The mutant.
 */
void MutantAcquired(MutantObject& mutant);

}  // namespace rex::runtime
