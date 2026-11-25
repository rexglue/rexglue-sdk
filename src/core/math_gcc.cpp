#include <rex/math.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
static_assert(false, "This file is non-MSVC only");
#endif

namespace rex {

uint8_t lzcnt(uint8_t v) {
  return v == 0 ? 8 : static_cast<uint8_t>(__builtin_clz(v) - 24);
}
uint8_t lzcnt(uint16_t v) {
  return v == 0 ? 16 : static_cast<uint8_t>(__builtin_clz(v) - 16);
}
uint8_t lzcnt(uint32_t v) {
  return v == 0 ? 32 : static_cast<uint8_t>(__builtin_clz(v));
}
uint8_t lzcnt(uint64_t v) {
  return v == 0 ? 64 : static_cast<uint8_t>(__builtin_clzll(v));
}

uint8_t tzcnt(uint8_t v) {
  return v == 0 ? 8 : static_cast<uint8_t>(__builtin_ctz(v));
}
uint8_t tzcnt(uint16_t v) {
  return v == 0 ? 16 : static_cast<uint8_t>(__builtin_ctz(v));
}
uint8_t tzcnt(uint32_t v) {
  return v == 0 ? 32 : static_cast<uint8_t>(__builtin_ctz(v));
}
uint8_t tzcnt(uint64_t v) {
  return v == 0 ? 64 : static_cast<uint8_t>(__builtin_ctzll(v));
}

// bit_scan_forward is inline in rex/math.h for GCC and Clang.

}  // namespace rex
