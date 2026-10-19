/**
 * @file        runtime/pin_count.h
 * @brief       Transient pin count with a retire handshake for `KernelObject` entries
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>

#include <rex/types.h>

namespace rex::runtime {

/**
 * Counts transient pins on a `KernelObject` and decides who destroys it. The destroyer is
 * whichever of the final Retire and the last Unpin happens second.
 */
class PinCount {
 public:
  /** Adds a pin. */
  void Pin() noexcept { count_.fetch_add(1, std::memory_order_acquire); }

  /**
   * Removes a pin.
   *
   * @return True when the caller must destroy the owner.
   */
  [[nodiscard]] bool Unpin() noexcept {
    return count_.fetch_sub(1, std::memory_order_acq_rel) - 1 == kRetired;
  }

  /**
   * Marks the owner retired.
   *
   * @return True when no pin is held and the caller must destroy the owner.
   */
  [[nodiscard]] bool Retire() noexcept {
    return count_.fetch_or(kRetired, std::memory_order_acq_rel) == 0;
  }

  /**
   * Returns the live pin count.
   *
   * @return The pins, without the retired bit.
   */
  [[nodiscard]] u32 GetPins() const noexcept {
    return count_.load(std::memory_order_acquire) & ~kRetired;
  }

 private:
  static constexpr u32 kRetired = 0x80000000u;
  std::atomic<u32> count_{0};
};

}  // namespace rex::runtime
