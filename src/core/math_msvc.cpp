#include <rex/math.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include <intrin.h>

namespace rex {

uint8_t lzcnt(uint8_t v) {
  unsigned long index;
  unsigned long mask = v;
  unsigned char is_nonzero = _BitScanReverse(&index, mask);
  return static_cast<uint8_t>(is_nonzero ? int8_t(index) ^ 0x7 : 8);
}
uint8_t lzcnt(uint16_t v) {
  unsigned long index;
  unsigned long mask = v;
  unsigned char is_nonzero = _BitScanReverse(&index, mask);
  return static_cast<uint8_t>(is_nonzero ? int8_t(index) ^ 0xF : 16);
}
uint8_t lzcnt(uint32_t v) {
  unsigned long index;
  unsigned long mask = v;
  unsigned char is_nonzero = _BitScanReverse(&index, mask);
  return static_cast<uint8_t>(is_nonzero ? int8_t(index) ^ 0x1F : 32);
}
uint8_t lzcnt(uint64_t v) {
  unsigned long index;
  unsigned long long mask = v;
  unsigned char is_nonzero = _BitScanReverse64(&index, mask);
  return static_cast<uint8_t>(is_nonzero ? int8_t(index) ^ 0x3F : 64);
}

uint8_t tzcnt(uint8_t v) {
  unsigned long index;
  unsigned long mask = v;
  unsigned char is_nonzero = _BitScanForward(&index, mask);
  return static_cast<uint8_t>(is_nonzero ? int8_t(index) : 8);
}
uint8_t tzcnt(uint16_t v) {
  unsigned long index;
  unsigned long mask = v;
  unsigned char is_nonzero = _BitScanForward(&index, mask);
  return static_cast<uint8_t>(is_nonzero ? int8_t(index) : 16);
}
uint8_t tzcnt(uint32_t v) {
  unsigned long index;
  unsigned long mask = v;
  unsigned char is_nonzero = _BitScanForward(&index, mask);
  return static_cast<uint8_t>(is_nonzero ? int8_t(index) : 32);
}
uint8_t tzcnt(uint64_t v) {
  unsigned long index;
  unsigned long long mask = v;
  unsigned char is_nonzero = _BitScanForward64(&index, mask);
  return static_cast<uint8_t>(is_nonzero ? int8_t(index) : 64);
}

bool bit_scan_forward(uint32_t v, uint32_t* out_first_set_index) {
  return _BitScanForward(reinterpret_cast<unsigned long*>(out_first_set_index), v) != 0;
}
bool bit_scan_forward(uint64_t v, uint32_t* out_first_set_index) {
  return _BitScanForward64(reinterpret_cast<unsigned long*>(out_first_set_index), v) != 0;
}

}  // namespace rex
