/**
 * @file        platform/interrupt_mask.h
 * @brief       Interrupt masking for recompiled code
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <mutex>

#include <rex/macros.h>

namespace rex::platform {

/**
 * Process wide recursive lock behind the interrupt mask of recompiled code. A recompiled
 * `mtmsrd` that clears interrupts takes it, and the matching `mtmsrd` that restores them
 * releases it. Interrupt callbacks hold it for their whole run, so they never start inside a
 * masked section. The kernel never takes it for its own structures.
 */
class REX_API InterruptMask {
 public:
  /**
   * Masks interrupts on behalf of the calling thread. Calls nest.
   */
  static void Disable();

  /**
   * Undoes 1 call to Disable made by the calling thread.
   */
  static void Enable();

  /**
   * Tests whether interrupts are enabled. Waits while another thread holds the lock.
   *
   * @return True when no thread has interrupts masked.
   */
  static bool Enabled();

  /**
   * Scope that holds the lock for an interrupt callback.
   */
  class REX_API Guard {
   public:
    /**
     * Takes the lock.
     */
    Guard();

    /**
     * Releases the lock.
     */
    ~Guard();

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
  };
};

}  // namespace rex::platform
