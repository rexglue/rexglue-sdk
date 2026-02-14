/**
 * Unit tests for byte order utilities (byte_swap, endian_store)
 *
 * Tests endianness conversion and byte swapping operations.
 */

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/types.h>

using namespace rex;

// =============================================================================
// byte_swap Tests
// =============================================================================

TEST_CASE("byte_swap 1-byte is identity", "[byte_order]") {
  CHECK(byte_swap(uint8_t{0x12}) == 0x12);
  CHECK(byte_swap(uint8_t{0x00}) == 0x00);
  CHECK(byte_swap(uint8_t{0xFF}) == 0xFF);
}

TEST_CASE("byte_swap 2-byte swaps correctly", "[byte_order]") {
  CHECK(byte_swap(uint16_t{0x1234}) == 0x3412);
  CHECK(byte_swap(uint16_t{0x0000}) == 0x0000);
  CHECK(byte_swap(uint16_t{0xFFFF}) == 0xFFFF);
  CHECK(byte_swap(uint16_t{0xFF00}) == 0x00FF);
}

TEST_CASE("byte_swap 4-byte swaps correctly", "[byte_order]") {
  CHECK(byte_swap(uint32_t{0x12345678}) == 0x78563412);
  CHECK(byte_swap(uint32_t{0x00000000}) == 0x00000000);
  CHECK(byte_swap(uint32_t{0xFFFFFFFF}) == 0xFFFFFFFF);
  CHECK(byte_swap(uint32_t{0xFF000000}) == 0x000000FF);
}

TEST_CASE("byte_swap 8-byte swaps correctly", "[byte_order]") {
  CHECK(byte_swap(uint64_t{0x123456789ABCDEF0}) == 0xF0DEBC9A78563412);
  CHECK(byte_swap(uint64_t{0x0000000000000000}) == 0x0000000000000000);
  CHECK(byte_swap(uint64_t{0xFFFFFFFFFFFFFFFF}) == 0xFFFFFFFFFFFFFFFF);
}

TEST_CASE("byte_swap is self-inverse", "[byte_order]") {
  uint16_t val16 = 0x1234;
  CHECK(byte_swap(byte_swap(val16)) == val16);

  uint32_t val32 = 0x12345678;
  CHECK(byte_swap(byte_swap(val32)) == val32);

  uint64_t val64 = 0x123456789ABCDEF0;
  CHECK(byte_swap(byte_swap(val64)) == val64);
}

// =============================================================================
// endian_store Tests
// =============================================================================

TEST_CASE("endian_store be stores big-endian", "[byte_order]") {
  be<uint32_t> val;
  val.set(0x12345678);

  // On little-endian machine, stored bytes should be swapped
  if constexpr (std::endian::native == std::endian::little) {
    CHECK(val.value == 0x78563412);
  } else {
    CHECK(val.value == 0x12345678);
  }

  CHECK(val.get() == 0x12345678);
}

TEST_CASE("endian_store le stores little-endian", "[byte_order]") {
  le<uint32_t> val;
  val.set(0x12345678);

  // On little-endian machine, stored bytes should be native
  if constexpr (std::endian::native == std::endian::little) {
    CHECK(val.value == 0x12345678);
  } else {
    CHECK(val.value == 0x78563412);
  }

  CHECK(val.get() == 0x12345678);
}

TEST_CASE("endian_store implicit conversion works", "[byte_order]") {
  be<uint32_t> be_val(0x12345678);
  uint32_t native = be_val;  // Implicit conversion via operator T()
  CHECK(native == 0x12345678);
}

TEST_CASE("endian_store assignment from value", "[byte_order]") {
  be<uint16_t> val;
  val = be<uint16_t>(0xABCD);
  CHECK(val.get() == 0xABCD);
}

TEST_CASE("endian_store increment operators", "[byte_order]") {
  be<uint32_t> val(10);

  ++val;
  CHECK(val.get() == 11);

  val++;
  CHECK(val.get() == 12);

  --val;
  CHECK(val.get() == 11);

  val--;
  CHECK(val.get() == 10);
}

TEST_CASE("endian_store compound assignment", "[byte_order]") {
  be<uint32_t> val(100);

  val += 50;
  CHECK(val.get() == 150);

  val -= 30;
  CHECK(val.get() == 120);
}
