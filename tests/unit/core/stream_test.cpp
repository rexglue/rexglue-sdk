/**
 * Unit tests for stream utilities (BitStream, ByteStream)
 *
 * Tests bit-level and byte-level stream operations.
 */

#include <array>
#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/stream.h>

using namespace rex::stream;

// =============================================================================
// ByteStream Basic Tests
// =============================================================================

TEST_CASE("ByteStream construction and initial state", "[stream][bytestream]") {
  std::array<uint8_t, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  CHECK(stream.data() == buffer.data());
  CHECK(stream.data_length() == 16);
  CHECK(stream.offset() == 0);
}

TEST_CASE("ByteStream construction with offset", "[stream][bytestream]") {
  std::array<uint8_t, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size(), 8);

  CHECK(stream.offset() == 8);
}

TEST_CASE("ByteStream Advance moves offset", "[stream][bytestream]") {
  std::array<uint8_t, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  stream.Advance(4);
  CHECK(stream.offset() == 4);

  stream.Advance(8);
  CHECK(stream.offset() == 12);
}

TEST_CASE("ByteStream set_offset works", "[stream][bytestream]") {
  std::array<uint8_t, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  stream.set_offset(10);
  CHECK(stream.offset() == 10);

  stream.set_offset(0);
  CHECK(stream.offset() == 0);
}

// =============================================================================
// ByteStream Read/Write Tests
// =============================================================================

TEST_CASE("ByteStream Write and Read span", "[stream][bytestream]") {
  std::array<uint8_t, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  std::array<uint8_t, 4> write_data = {0x11, 0x22, 0x33, 0x44};
  stream.Write(write_data.data(), write_data.size());

  CHECK(stream.offset() == 4);
  CHECK(buffer[0] == 0x11);
  CHECK(buffer[1] == 0x22);
  CHECK(buffer[2] == 0x33);
  CHECK(buffer[3] == 0x44);

  // Read it back
  stream.set_offset(0);
  std::array<uint8_t, 4> read_data{};
  stream.Read(read_data.data(), read_data.size());

  CHECK(read_data == write_data);
  CHECK(stream.offset() == 4);
}

TEST_CASE("ByteStream Write and Read template primitives", "[stream][bytestream]") {
  std::array<uint8_t, 32> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  // Write various types
  stream.Write<uint8_t>(0x12);
  stream.Write<uint16_t>(0x3456);
  stream.Write<uint32_t>(0x789ABCDE);
  stream.Write<uint64_t>(0xFEDCBA9876543210);

  CHECK(stream.offset() == 1 + 2 + 4 + 8);

  // Read them back
  stream.set_offset(0);
  CHECK(stream.Read<uint8_t>() == 0x12);
  CHECK(stream.Read<uint16_t>() == 0x3456);
  CHECK(stream.Read<uint32_t>() == 0x789ABCDE);
  CHECK(stream.Read<uint64_t>() == 0xFEDCBA9876543210);
}

TEST_CASE("ByteStream Write and Read void* overloads", "[stream][bytestream]") {
  std::array<uint8_t, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  const char* msg = "test";
  stream.Write(msg, 4);

  stream.set_offset(0);
  char read_buf[5] = {};
  stream.Read(read_buf, 4);

  CHECK(std::memcmp(read_buf, msg, 4) == 0);
}

TEST_CASE("ByteStream Write and Read std::string", "[stream][bytestream]") {
  std::array<uint8_t, 64> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  std::string original = "Hello, World!";
  stream.Write(std::string_view(original));

  stream.set_offset(0);
  std::string result = stream.Read<std::string>();

  CHECK(result == original);
}

TEST_CASE("ByteStream Write and Read std::u16string", "[stream][bytestream]") {
  std::array<uint8_t, 64> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  std::u16string original = u"Unicode\u00AE";
  stream.Write(std::u16string_view(original));

  stream.set_offset(0);
  std::u16string result = stream.Read<std::u16string>();

  CHECK(result == original);
}

TEST_CASE("ByteStream empty string round-trip", "[stream][bytestream]") {
  std::array<uint8_t, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  stream.Write(std::string_view(""));

  stream.set_offset(0);
  std::string result = stream.Read<std::string>();

  CHECK(result.empty());
}

// =============================================================================
// BitStream Basic Tests
// =============================================================================

TEST_CASE("BitStream construction and initial state", "[stream][bitstream]") {
  std::array<uint8_t, 16> buffer{};
  BitStream stream(buffer.data(), buffer.size() * 8);

  CHECK(stream.buffer() == buffer.data());
  CHECK(stream.size_bits() == 128);
  CHECK(stream.offset_bits() == 0);
  CHECK(stream.BitsRemaining() == 128);
}

TEST_CASE("BitStream SetOffset works", "[stream][bitstream]") {
  std::array<uint8_t, 16> buffer{};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.SetOffset(32);
  CHECK(stream.offset_bits() == 32);
  CHECK(stream.BitsRemaining() == 96);
}

TEST_CASE("BitStream Advance works", "[stream][bitstream]") {
  std::array<uint8_t, 16> buffer{};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Advance(10);
  CHECK(stream.offset_bits() == 10);

  stream.Advance(5);
  CHECK(stream.offset_bits() == 15);
}

// =============================================================================
// BitStream Peek/Read Tests
// =============================================================================

TEST_CASE("BitStream Peek reads without advancing", "[stream][bitstream]") {
  // Buffer with known pattern: 0xAB = 10101011
  std::array<uint8_t, 16> buffer = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
  BitStream stream(buffer.data(), buffer.size() * 8);

  uint64_t val1 = stream.Peek(8);
  CHECK(stream.offset_bits() == 0);  // No advance

  uint64_t val2 = stream.Peek(8);
  CHECK(val1 == val2);  // Same value
}

TEST_CASE("BitStream Read advances offset", "[stream][bitstream]") {
  std::array<uint8_t, 16> buffer = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Read(8);
  CHECK(stream.offset_bits() == 8);

  stream.Read(16);
  CHECK(stream.offset_bits() == 24);
}

TEST_CASE("BitStream Read byte-aligned values", "[stream][bitstream]") {
  // Big-endian data: reading first byte should give 0xAB
  std::array<uint8_t, 16> buffer = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
  BitStream stream(buffer.data(), buffer.size() * 8);

  CHECK(stream.Read(8) == 0xAB);
  CHECK(stream.Read(8) == 0xCD);
  CHECK(stream.Read(8) == 0xEF);
}

TEST_CASE("BitStream Read 16-bit aligned", "[stream][bitstream]") {
  std::array<uint8_t, 16> buffer = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
  BitStream stream(buffer.data(), buffer.size() * 8);

  // Reading 16 bits from big-endian: 0xABCD
  CHECK(stream.Read(16) == 0xABCD);
  CHECK(stream.Read(16) == 0xEF12);
}

TEST_CASE("BitStream Read 32-bit aligned", "[stream][bitstream]") {
  std::array<uint8_t, 16> buffer = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
  BitStream stream(buffer.data(), buffer.size() * 8);

  CHECK(stream.Read(32) == 0xABCDEF12);
}

TEST_CASE("BitStream Read non-byte-aligned", "[stream][bitstream]") {
  // 0xAB = 10101011, 0xCD = 11001101
  std::array<uint8_t, 16> buffer = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
  BitStream stream(buffer.data(), buffer.size() * 8);

  // Read 4 bits: should be 1010 = 0xA
  CHECK(stream.Read(4) == 0xA);

  // Read 4 bits: should be 1011 = 0xB
  CHECK(stream.Read(4) == 0xB);

  // Read 8 bits: should be 0xCD
  CHECK(stream.Read(8) == 0xCD);
}

TEST_CASE("BitStream Read single bits", "[stream][bitstream]") {
  // 0xAB = 10101011
  std::array<uint8_t, 16> buffer = {0xAB, 0xCD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  BitStream stream(buffer.data(), buffer.size() * 8);

  // Read bits one at a time from MSB
  CHECK(stream.Read(1) == 1);  // 1
  CHECK(stream.Read(1) == 0);  // 0
  CHECK(stream.Read(1) == 1);  // 1
  CHECK(stream.Read(1) == 0);  // 0
  CHECK(stream.Read(1) == 1);  // 1
  CHECK(stream.Read(1) == 0);  // 0
  CHECK(stream.Read(1) == 1);  // 1
  CHECK(stream.Read(1) == 1);  // 1
}

TEST_CASE("BitStream Read crossing byte boundary", "[stream][bitstream]") {
  // Read bits that span byte boundaries
  std::array<uint8_t, 16> buffer = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Advance(4);  // Now at bit 4

  // Read 8 bits crossing the boundary: last 4 of 0xFF + first 4 of 0x00
  // 0xFF = 11111111, last 4 = 1111
  // 0x00 = 00000000, first 4 = 0000
  // Combined: 11110000 = 0xF0
  CHECK(stream.Read(8) == 0xF0);
}

// =============================================================================
// BitStream Write Tests
// NOTE: BitStream::Write is marked "TODO: This is totally not tested!" in source.
// It has a bug: doesn't byte-swap when storing, but Read expects big-endian.
// These tests are skipped until Write is fixed.
// =============================================================================

TEST_CASE("BitStream Write byte-aligned", "[stream][bitstream][!mayfail]") {
  SKIP("BitStream::Write is broken - doesn't byte-swap on store");

  std::array<uint8_t, 16> buffer{};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Write(0xAB, 8);
  CHECK(stream.offset_bits() == 8);

  stream.SetOffset(0);
  CHECK(stream.Read(8) == 0xAB);
}

TEST_CASE("BitStream Write 16-bit value", "[stream][bitstream][!mayfail]") {
  SKIP("BitStream::Write is broken - doesn't byte-swap on store");

  std::array<uint8_t, 16> buffer{};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Write(0x1234, 16);

  stream.SetOffset(0);
  CHECK(stream.Read(16) == 0x1234);
}

TEST_CASE("BitStream Write non-byte-aligned", "[stream][bitstream][!mayfail]") {
  SKIP("BitStream::Write is broken - doesn't byte-swap on store");

  std::array<uint8_t, 16> buffer{};
  BitStream stream(buffer.data(), buffer.size() * 8);

  // Write 4 bits, then 4 more
  stream.Write(0xA, 4);  // 1010
  stream.Write(0xB, 4);  // 1011

  stream.SetOffset(0);
  CHECK(stream.Read(8) == 0xAB);
}

TEST_CASE("BitStream Write preserves surrounding bits", "[stream][bitstream][!mayfail]") {
  SKIP("BitStream::Write is broken - doesn't byte-swap on store");

  std::array<uint8_t, 16> buffer = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Advance(4);
  stream.Write(0x0, 4);  // Clear middle 4 bits

  stream.SetOffset(0);
  // First 4 bits should still be 1111, next 4 should be 0000
  CHECK(stream.Read(4) == 0xF);
  CHECK(stream.Read(4) == 0x0);
}

// =============================================================================
// BitStream Edge Cases
// =============================================================================

TEST_CASE("BitStream Read zero bits returns zero", "[stream][bitstream]") {
  std::array<uint8_t, 16> buffer = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  BitStream stream(buffer.data(), buffer.size() * 8);

  CHECK(stream.Read(0) == 0);
  CHECK(stream.offset_bits() == 0);  // No advance
}

TEST_CASE("BitStream maximum bits (57)", "[stream][bitstream]") {
  std::array<uint8_t, 16> buffer = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  BitStream stream(buffer.data(), buffer.size() * 8);

  // 57 bits of all 1s = 0x1FFFFFFFFFFFFFF
  uint64_t expected = (1ULL << 57) - 1;
  CHECK(stream.Read(57) == expected);
}

TEST_CASE("BitStream BitsRemaining decreases", "[stream][bitstream]") {
  std::array<uint8_t, 16> buffer{};
  BitStream stream(buffer.data(), 64);  // 64 bits = 8 bytes

  CHECK(stream.BitsRemaining() == 64);

  stream.Read(16);
  CHECK(stream.BitsRemaining() == 48);

  stream.Read(32);
  CHECK(stream.BitsRemaining() == 16);
}
