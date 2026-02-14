#include <rex/platform.h>
#include <rex/thread/atomic.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

namespace rex::thread {

int32_t atomic_inc(volatile int32_t* value) {
  return __sync_add_and_fetch(value, 1);
}
int32_t atomic_dec(volatile int32_t* value) {
  return __sync_sub_and_fetch(value, 1);
}

int32_t atomic_exchange(int32_t new_value, volatile int32_t* value) {
  return __atomic_exchange_n(value, new_value, __ATOMIC_SEQ_CST);
}
int64_t atomic_exchange(int64_t new_value, volatile int64_t* value) {
  return __atomic_exchange_n(value, new_value, __ATOMIC_SEQ_CST);
}

int32_t atomic_exchange_add(int32_t amount, volatile int32_t* value) {
  return __sync_fetch_and_add(value, amount);
}
int64_t atomic_exchange_add(int64_t amount, volatile int64_t* value) {
  return __sync_fetch_and_add(value, amount);
}

bool atomic_cas(int32_t old_value, int32_t new_value, volatile int32_t* value) {
  return __sync_bool_compare_and_swap(reinterpret_cast<volatile int32_t*>(value), old_value,
                                      new_value);
}
bool atomic_cas(int64_t old_value, int64_t new_value, volatile int64_t* value) {
  return __sync_bool_compare_and_swap(reinterpret_cast<volatile int64_t*>(value), old_value,
                                      new_value);
}

}  // namespace rex::thread
