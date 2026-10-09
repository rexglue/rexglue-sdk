#include <rex/platform.h>
#include <rex/platform/atomic.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include <intrin.h>

namespace rex::platform {

i32 atomic_inc(volatile i32* value) {
  return _InterlockedIncrement(reinterpret_cast<volatile long*>(value));
}
i32 atomic_dec(volatile i32* value) {
  return _InterlockedDecrement(reinterpret_cast<volatile long*>(value));
}

i32 atomic_exchange(i32 new_value, volatile i32* value) {
  return _InterlockedExchange(reinterpret_cast<volatile long*>(value), new_value);
}
i64 atomic_exchange(i64 new_value, volatile i64* value) {
  return _InterlockedExchange64(reinterpret_cast<volatile long long*>(value), new_value);
}

i32 atomic_exchange_add(i32 amount, volatile i32* value) {
  return _InterlockedExchangeAdd(reinterpret_cast<volatile long*>(value), amount);
}
i64 atomic_exchange_add(i64 amount, volatile i64* value) {
  return _InterlockedExchangeAdd64(reinterpret_cast<volatile long long*>(value), amount);
}

bool atomic_cas(i32 old_value, i32 new_value, volatile i32* value) {
  return _InterlockedCompareExchange(reinterpret_cast<volatile long*>(value), new_value,
                                     old_value) == old_value;
}
bool atomic_cas(i64 old_value, i64 new_value, volatile i64* value) {
  return _InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(value), new_value,
                                       old_value) == old_value;
}

void atomic_store_release(i32 new_value, volatile i32* value) {
  _InterlockedExchange(reinterpret_cast<volatile long*>(value), new_value);
}

}  // namespace rex::platform
