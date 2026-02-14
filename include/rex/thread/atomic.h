/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

namespace rex::thread {

int32_t atomic_inc(volatile int32_t* value);
int32_t atomic_dec(volatile int32_t* value);

int32_t atomic_exchange(int32_t new_value, volatile int32_t* value);
int64_t atomic_exchange(int64_t new_value, volatile int64_t* value);

int32_t atomic_exchange_add(int32_t amount, volatile int32_t* value);
int64_t atomic_exchange_add(int64_t amount, volatile int64_t* value);

bool atomic_cas(int32_t old_value, int32_t new_value, volatile int32_t* value);
bool atomic_cas(int64_t old_value, int64_t new_value, volatile int64_t* value);

inline uint32_t atomic_inc(volatile uint32_t* value) {
  return static_cast<uint32_t>(atomic_inc(reinterpret_cast<volatile int32_t*>(value)));
}
inline uint32_t atomic_dec(volatile uint32_t* value) {
  return static_cast<uint32_t>(atomic_dec(reinterpret_cast<volatile int32_t*>(value)));
}

inline uint32_t atomic_exchange(uint32_t new_value, volatile uint32_t* value) {
  return static_cast<uint32_t>(
      atomic_exchange(static_cast<int32_t>(new_value), reinterpret_cast<volatile int32_t*>(value)));
}
inline uint64_t atomic_exchange(uint64_t new_value, volatile uint64_t* value) {
  return static_cast<uint64_t>(
      atomic_exchange(static_cast<int64_t>(new_value), reinterpret_cast<volatile int64_t*>(value)));
}

inline uint32_t atomic_exchange_add(uint32_t amount, volatile uint32_t* value) {
  return static_cast<uint32_t>(atomic_exchange_add(static_cast<int32_t>(amount),
                                                   reinterpret_cast<volatile int32_t*>(value)));
}
inline uint64_t atomic_exchange_add(uint64_t amount, volatile uint64_t* value) {
  return static_cast<uint64_t>(atomic_exchange_add(static_cast<int64_t>(amount),
                                                   reinterpret_cast<volatile int64_t*>(value)));
}

inline bool atomic_cas(uint32_t old_value, uint32_t new_value, volatile uint32_t* value) {
  return atomic_cas(static_cast<int32_t>(old_value), static_cast<int32_t>(new_value),
                    reinterpret_cast<volatile int32_t*>(value));
}
inline bool atomic_cas(uint64_t old_value, uint64_t new_value, volatile uint64_t* value) {
  return atomic_cas(static_cast<int64_t>(old_value), static_cast<int64_t>(new_value),
                    reinterpret_cast<volatile int64_t*>(value));
}

}  // namespace rex::thread
