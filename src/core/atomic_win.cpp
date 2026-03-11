#include <rex/platform.h>
#include <rex/thread/atomic.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include <intrin.h>

namespace rex::thread {

int32_t atomic_inc(volatile int32_t* value) {
  return _InterlockedIncrement(reinterpret_cast<volatile long*>(value));
}
int32_t atomic_dec(volatile int32_t* value) {
  return _InterlockedDecrement(reinterpret_cast<volatile long*>(value));
}

int32_t atomic_exchange(int32_t new_value, volatile int32_t* value) {
  return _InterlockedExchange(reinterpret_cast<volatile long*>(value), new_value);
}
int64_t atomic_exchange(int64_t new_value, volatile int64_t* value) {
  return _InterlockedExchange64(reinterpret_cast<volatile long long*>(value), new_value);
}

int32_t atomic_exchange_add(int32_t amount, volatile int32_t* value) {
  return _InterlockedExchangeAdd(reinterpret_cast<volatile long*>(value), amount);
}
int64_t atomic_exchange_add(int64_t amount, volatile int64_t* value) {
  return _InterlockedExchangeAdd64(reinterpret_cast<volatile long long*>(value), amount);
}

bool atomic_cas(int32_t old_value, int32_t new_value, volatile int32_t* value) {
  return _InterlockedCompareExchange(reinterpret_cast<volatile long*>(value), new_value,
                                     old_value) == old_value;
}
bool atomic_cas(int64_t old_value, int64_t new_value, volatile int64_t* value) {
  return _InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(value), new_value,
                                       old_value) == old_value;
}

void atomic_store_release(int32_t new_value, volatile int32_t* value) {
  _InterlockedExchange(reinterpret_cast<volatile long*>(value), new_value);
}

}  // namespace rex::thread
