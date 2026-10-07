/**
 * @file        platform/atomic.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/atomic.h (Ben Vanik, 2014).
 */

#pragma once

#include <rex/macros.h>
#include <rex/types.h>

namespace rex::platform {

/**
 * Increments a value atomically.
 *
 * @param value Value to increment.
 * @return The incremented value.
 */
REX_API i32 atomic_inc(volatile i32* value);
/**
 * Decrements a value atomically.
 *
 * @param value Value to decrement.
 * @return The decremented value.
 */
REX_API i32 atomic_dec(volatile i32* value);

/**
 * Stores a new value atomically.
 *
 * @param new_value Value to store.
 * @param value Target.
 * @return The previous value.
 */
REX_API i32 atomic_exchange(i32 new_value, volatile i32* value);
/**
 * Stores a new value atomically.
 *
 * @param new_value Value to store.
 * @param value Target.
 * @return The previous value.
 */
REX_API i64 atomic_exchange(i64 new_value, volatile i64* value);

/**
 * Adds to a value atomically.
 *
 * @param amount Amount to add.
 * @param value Target.
 * @return The previous value.
 */
REX_API i32 atomic_exchange_add(i32 amount, volatile i32* value);
/**
 * Adds to a value atomically.
 *
 * @param amount Amount to add.
 * @param value Target.
 * @return The previous value.
 */
REX_API i64 atomic_exchange_add(i64 amount, volatile i64* value);

/**
 * Stores new_value when the target equals old_value, atomically.
 *
 * @param old_value Expected value.
 * @param new_value Value to store.
 * @param value Target.
 * @return True when the store happened.
 */
REX_API bool atomic_cas(i32 old_value, i32 new_value, volatile i32* value);
/**
 * Stores new_value when the target equals old_value, atomically.
 *
 * @param old_value Expected value.
 * @param new_value Value to store.
 * @param value Target.
 * @return True when the store happened.
 */
REX_API bool atomic_cas(i64 old_value, i64 new_value, volatile i64* value);
/**
 * Stores a value with release ordering.
 *
 * @param new_value Value to store.
 * @param value Target.
 */
REX_API void atomic_store_release(i32 new_value, volatile i32* value);

/**
 * Increments an unsigned value atomically.
 *
 * @param value Value to increment.
 * @return The incremented value.
 */
inline u32 atomic_inc(volatile u32* value) {
  return static_cast<u32>(atomic_inc(reinterpret_cast<volatile i32*>(value)));
}
/**
 * Decrements an unsigned value atomically.
 *
 * @param value Value to decrement.
 * @return The decremented value.
 */
inline u32 atomic_dec(volatile u32* value) {
  return static_cast<u32>(atomic_dec(reinterpret_cast<volatile i32*>(value)));
}

/**
 * Stores a new unsigned value atomically.
 *
 * @param new_value Value to store.
 * @param value Target.
 * @return The previous value.
 */
inline u32 atomic_exchange(u32 new_value, volatile u32* value) {
  return static_cast<u32>(
      atomic_exchange(static_cast<i32>(new_value), reinterpret_cast<volatile i32*>(value)));
}
/**
 * Stores a new unsigned value atomically.
 *
 * @param new_value Value to store.
 * @param value Target.
 * @return The previous value.
 */
inline u64 atomic_exchange(u64 new_value, volatile u64* value) {
  return static_cast<u64>(
      atomic_exchange(static_cast<i64>(new_value), reinterpret_cast<volatile i64*>(value)));
}

/**
 * Adds to an unsigned value atomically.
 *
 * @param amount Amount to add.
 * @param value Target.
 * @return The previous value.
 */
inline u32 atomic_exchange_add(u32 amount, volatile u32* value) {
  return static_cast<u32>(
      atomic_exchange_add(static_cast<i32>(amount), reinterpret_cast<volatile i32*>(value)));
}
/**
 * Adds to an unsigned value atomically.
 *
 * @param amount Amount to add.
 * @param value Target.
 * @return The previous value.
 */
inline u64 atomic_exchange_add(u64 amount, volatile u64* value) {
  return static_cast<u64>(
      atomic_exchange_add(static_cast<i64>(amount), reinterpret_cast<volatile i64*>(value)));
}

/**
 * Stores new_value when the unsigned target equals old_value, atomically.
 *
 * @param old_value Expected value.
 * @param new_value Value to store.
 * @param value Target.
 * @return True when the store happened.
 */
inline bool atomic_cas(u32 old_value, u32 new_value, volatile u32* value) {
  return atomic_cas(static_cast<i32>(old_value), static_cast<i32>(new_value),
                    reinterpret_cast<volatile i32*>(value));
}
/**
 * Stores new_value when the unsigned target equals old_value, atomically.
 *
 * @param old_value Expected value.
 * @param new_value Value to store.
 * @param value Target.
 * @return True when the store happened.
 */
inline bool atomic_cas(u64 old_value, u64 new_value, volatile u64* value) {
  return atomic_cas(static_cast<i64>(old_value), static_cast<i64>(new_value),
                    reinterpret_cast<volatile i64*>(value));
}

/**
 * Stores an unsigned value with release ordering.
 *
 * @param new_value Value to store.
 * @param value Target.
 */
inline void atomic_store_release(u32 new_value, volatile u32* value) {
  atomic_store_release(static_cast<i32>(new_value), reinterpret_cast<volatile i32*>(value));
}

}  // namespace rex::platform
