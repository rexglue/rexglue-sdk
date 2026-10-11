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

#pragma once

#include <atomic>
#include <memory>

#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/** A guest KMUTANT. */
struct X_KMUTANT {
  /** Dispatcher header with type 2. */
  X_DISPATCH_HEADER header;
  /** Links the mutant in the owner's mutant list. */
  X_LIST_ENTRY mutant_list_entry;
  /** Guest KTHREAD of the owner, 0 when free. */
  be_u32 owner_thread;
  /** Nonzero when the owner exited without a release. */
  u8 abandoned;
  /** Padding to the KMUTANT size. */
  u8 reserved[3];
};
REX_STATIC_ASSERT_SIZE(X_KMUTANT, 0x20);

/** The host state of a mutant object. */
struct MutantObject {
  MutantObject() = default;

  /**
   * @param initial_owner True when the creating thread owns the mutant at once.
   * @param guest_mutant_in The guest KMUTANT the object backs, 0 for none.
   */
  REX_RUNTIME_API explicit MutantObject(bool initial_owner, u32 guest_mutant_in = 0);

  std::unique_ptr<rex::platform::Mutant> mutant; /**< Host mutant that waiters block on. */
  /** Id of the owning thread, 0 when no thread owns the mutant. */
  std::atomic<u32> owner_thread_id{0};
  std::atomic<i32> hold_count{0}; /**< Number of recursive acquires by the owner, 0 when unowned. */
  /** Set by an abandon, cleared by the next acquire. */
  std::atomic<bool> abandoned{false};
  /** The guest KMUTANT the object backs, 0 for none. */
  u32 guest_mutant = 0;

  /**
   * Releases a mutant held by the current thread.
   *
   * @param abandon True to release every hold and mark the mutant abandoned.
   * @param previous_state Receives the signal state before the release, may be null.
   * @return X_STATUS_SUCCESS, or X_STATUS_MUTANT_NOT_OWNED when the caller does not own it.
   */
  REX_RUNTIME_API X_STATUS Release(bool abandon, i32* previous_state = nullptr);

  /**
   * Sets the current thread as the owner after a successful wait.
   *
   * @return True when the mutant was abandoned since its last acquire.
   */
  REX_RUNTIME_API bool OnAcquired();
};

}  // namespace rex::runtime
