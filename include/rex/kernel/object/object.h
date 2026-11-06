/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/object/object.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <vector>

#include <rex/kernel/types/thread.h>
#include <rex/platform.h>
#include <rex/types.h>
#include <rex/assert.h>
#include <rex/memory/endian.h>
#include <rex/platform/thread.h>
#include <rex/platform/sync.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/types/status.h>
#include <rex/memory/host.h>

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel {

/** Value StashHandle writes to wait_list_flink to mark a dispatch header that holds a handle. */
constexpr memory::FourCCT kStashSignature = memory::make_fourcc('R', 'E', 'X', '\0');

enum class ObjectType : u8 {
  kEvent,
  kSemaphore,
  kMutant,
  kTimer,
  kFile,
  kNotifyListener,
  kIOCompletion,
  kSymbolicLink,
  kModule,
  kThread,
  kXAM,
};

/** The value added to a table slot index, shifted left by 2, to form a handle. */
inline constexpr u32 kHandleBase = 0xF8000000;

/**
 * Writes the guest header stash that Ke exports use to find the handle.
 *
 * @param header The dispatch header inside a guest object.
 */
inline void StashHandle(X_DISPATCH_HEADER* header, u32 handle) {
  header->wait_list_flink = kStashSignature;
  header->wait_list_blink = handle;
}

/**
 * Converts a wait timeout in 100 nanosecond ticks to milliseconds.
 *
 * @param timeout_ticks A negative relative timeout, a positive absolute system time, or 0.
 * @return The time until the timeout, 0 when it has passed.
 */
REX_KERNEL_API std::chrono::milliseconds TimeoutFromTicks(i64 timeout_ticks);

/**
 * Converts a delay interval in 100 nanosecond ticks to microseconds, scaled by the guest clock
 * and rounded up.
 *
 * @param delay_ticks A negative relative interval, a positive absolute system time, or 0.
 * @return The time until the interval ends, 0 when it has passed.
 */
REX_KERNEL_API std::chrono::microseconds DelayFromTicks(i64 delay_ticks);

/**
 * Reads the object name out of an X_OBJECT_ATTRIBUTES.
 *
 * @param memory Memory containing the attributes and the name.
 * @param obj_attributes_ptr The guest address of the attributes, or 0.
 * @return The name, empty when the attributes or the name pointer are null.
 */
REX_KERNEL_API std::string NameFromAttributes(kernel::GuestAddressSpace* memory,
                                              u32 obj_attributes_ptr);

template <typename T>
class Pinned;
class ObjectPin;

/**
 * Waits on a pinned object. A type without a wait handle succeeds at once.
 *
 * @param object A pin on the object, which is not empty.
 * @param alertable Nonzero when a user APC ends the wait.
 * @param opt_timeout The timeout in 100 nanosecond ticks, null to wait forever.
 * @return X_STATUS_SUCCESS, X_STATUS_USER_APC, X_STATUS_TIMEOUT, or X_STATUS_ABANDONED_WAIT_0.
 */
REX_KERNEL_API X_STATUS Wait(TitleRuntime& runtime, const ObjectPin& object, u32 wait_reason,
                             u32 processor_mode, u32 alertable, u64* opt_timeout);

/**
 * Waits on several pinned objects.
 *
 * @param objects Pins on the objects, none of them empty.
 * @param wait_type Nonzero to wait for any object, 0 to wait for all.
 * @param alertable Nonzero when a user APC ends the wait.
 * @param opt_timeout The timeout in 100 nanosecond ticks, null to wait forever.
 * @return The index of the signaled object for a wait on any, or the status Wait returns.
 */
REX_KERNEL_API X_STATUS WaitMultiple(TitleRuntime& runtime, std::span<const ObjectPin> objects,
                                     u32 wait_type, u32 wait_reason, u32 processor_mode,
                                     u32 alertable, u64* opt_timeout);

/**
 * Signals one pinned object and waits on another as a single operation.
 *
 * @param signal_object A pin on the object to signal, which is not empty.
 * @param wait_object A pin on the object to wait on, which is not empty.
 * @param alertable Nonzero when a user APC ends the wait.
 * @param opt_timeout The timeout in 100 nanosecond ticks, null to wait forever.
 * @return The status as Wait returns it.
 */
REX_KERNEL_API X_STATUS SignalAndWait(TitleRuntime& runtime, const ObjectPin& signal_object,
                                      const ObjectPin& wait_object, u32 wait_reason,
                                      u32 processor_mode, u32 alertable, u64* opt_timeout);

}  // namespace rex::kernel

/** Formats the enum as its underlying integer. */
template <>
struct std::formatter<rex::kernel::ObjectType> : std::formatter<rex::u32> {
  auto format(rex::kernel::ObjectType value, std::format_context& ctx) const {
    return std::formatter<rex::u32>::format(static_cast<rex::u32>(value), ctx);
  }
};

namespace rex::kernel {

/**
 * Counts transient pins on a `KernelObject` and decides who destroys it. The destroyer is
 * whichever of the final Retire and the last Unpin happens second.
 */
class PinCount {
 public:
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

  /** Returns the live pin count, without the retired bit. */
  [[nodiscard]] u32 GetPins() const noexcept {
    return count_.load(std::memory_order_acquire) & ~kRetired;
  }

 private:
  static constexpr u32 kRetired = 0x80000000u;
  std::atomic<u32> count_{0};
};

}  // namespace rex::kernel

namespace rex::kernel {

inline constexpr memory::Tag kRuntime{2};

}  // namespace rex::kernel
