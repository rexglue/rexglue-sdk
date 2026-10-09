#include <rex/platform.h>
#include <rex/platform/atomic.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

namespace rex::platform {

i32 atomic_inc(volatile i32* value) {
  return __sync_add_and_fetch(value, 1);
}
i32 atomic_dec(volatile i32* value) {
  return __sync_sub_and_fetch(value, 1);
}

i32 atomic_exchange(i32 new_value, volatile i32* value) {
  return __atomic_exchange_n(value, new_value, __ATOMIC_SEQ_CST);
}
i64 atomic_exchange(i64 new_value, volatile i64* value) {
  return __atomic_exchange_n(value, new_value, __ATOMIC_SEQ_CST);
}

i32 atomic_exchange_add(i32 amount, volatile i32* value) {
  return __sync_fetch_and_add(value, amount);
}
i64 atomic_exchange_add(i64 amount, volatile i64* value) {
  return __sync_fetch_and_add(value, amount);
}

bool atomic_cas(i32 old_value, i32 new_value, volatile i32* value) {
  return __sync_bool_compare_and_swap(reinterpret_cast<volatile i32*>(value), old_value, new_value);
}
bool atomic_cas(i64 old_value, i64 new_value, volatile i64* value) {
  return __sync_bool_compare_and_swap(reinterpret_cast<volatile i64*>(value), old_value, new_value);
}

void atomic_store_release(i32 new_value, volatile i32* value) {
  __atomic_store_n(value, new_value, __ATOMIC_RELEASE);
}

}  // namespace rex::platform
