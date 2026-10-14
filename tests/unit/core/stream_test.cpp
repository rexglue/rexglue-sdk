/**
 * @file        tests/unit/core/stream_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <cstdint>
#include <cstring>

#include "rex_test.h"

#include <rex/stream.h>

using rex::BitStream;
using rex::ByteStream;

REX_TEST_CASE("ByteStream construction and initial state", "[stream][bytestream]") {
  std::array<u8, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  CHECK(stream.GetData() == buffer.data());
  CHECK(stream.GetDataLength() == 16);
  CHECK(stream.GetOffset() == 0);
}

REX_TEST_CASE("ByteStream construction with offset", "[stream][bytestream]") {
  std::array<u8, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size(), 8);

  CHECK(stream.GetOffset() == 8);
}

REX_TEST_CASE("ByteStream Advance moves offset", "[stream][bytestream]") {
  std::array<u8, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  stream.Advance(4);
  CHECK(stream.GetOffset() == 4);

  stream.Advance(8);
  CHECK(stream.GetOffset() == 12);
}

REX_TEST_CASE("ByteStream set_offset works", "[stream][bytestream]") {
  std::array<u8, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  stream.SetOffset(10);
  CHECK(stream.GetOffset() == 10);

  stream.SetOffset(0);
  CHECK(stream.GetOffset() == 0);
}

REX_TEST_CASE("ByteStream Write and Read span", "[stream][bytestream]") {
  std::array<u8, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  std::array<u8, 4> write_data = {0x11, 0x22, 0x33, 0x44};
  stream.Write(write_data.data(), write_data.size());

  CHECK(stream.GetOffset() == 4);
  CHECK(buffer[0] == 0x11);
  CHECK(buffer[1] == 0x22);
  CHECK(buffer[2] == 0x33);
  CHECK(buffer[3] == 0x44);

  stream.SetOffset(0);
  std::array<u8, 4> read_data{};
  stream.Read(read_data.data(), read_data.size());

  CHECK(read_data == write_data);
  CHECK(stream.GetOffset() == 4);
}

REX_TEST_CASE("ByteStream Write and Read template primitives", "[stream][bytestream]") {
  std::array<u8, 32> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  stream.Write<u8>(0x12);
  stream.Write<u16>(0x3456);
  stream.Write<u32>(0x789ABCDE);
  stream.Write<u64>(0xFEDCBA9876543210);

  CHECK(stream.GetOffset() == 1 + 2 + 4 + 8);

  stream.SetOffset(0);
  CHECK(stream.Read<u8>() == 0x12);
  CHECK(stream.Read<u16>() == 0x3456);
  CHECK(stream.Read<u32>() == 0x789ABCDE);
  CHECK(stream.Read<u64>() == 0xFEDCBA9876543210);
}

REX_TEST_CASE("ByteStream Write and Read void* overloads", "[stream][bytestream]") {
  std::array<u8, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  const char* msg = "test";
  stream.Write(msg, 4);

  stream.SetOffset(0);
  char read_buf[5] = {};
  stream.Read(read_buf, 4);

  CHECK(std::memcmp(read_buf, msg, 4) == 0);
}

REX_TEST_CASE("ByteStream Write and Read std::string", "[stream][bytestream]") {
  std::array<u8, 64> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  std::string original = "Hello, World!";
  stream.Write(std::string_view(original));

  stream.SetOffset(0);
  std::string result = stream.Read<std::string>();

  CHECK(result == original);
}

REX_TEST_CASE("ByteStream Write and Read std::u16string", "[stream][bytestream]") {
  std::array<u8, 64> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  std::u16string original = u"Unicode\u00AE";
  stream.Write(std::u16string_view(original));

  stream.SetOffset(0);
  std::u16string result = stream.Read<std::u16string>();

  CHECK(result == original);
}

REX_TEST_CASE("ByteStream empty string round-trip", "[stream][bytestream]") {
  std::array<u8, 16> buffer{};
  ByteStream stream(buffer.data(), buffer.size());

  stream.Write(std::string_view(""));

  stream.SetOffset(0);
  std::string result = stream.Read<std::string>();

  CHECK(result.empty());
}

REX_TEST_CASE("BitStream construction and initial state", "[stream][bitstream]") {
  std::array<u8, 16> buffer{};
  BitStream stream(buffer.data(), buffer.size() * 8);

  CHECK(stream.GetBuffer() == buffer.data());
  CHECK(stream.GetSizeBits() == 128);
  CHECK(stream.GetOffsetBits() == 0);
  CHECK(stream.GetBitsRemaining() == 128);
}

REX_TEST_CASE("BitStream SetOffset works", "[stream][bitstream]") {
  std::array<u8, 16> buffer{};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.SetOffset(32);
  CHECK(stream.GetOffsetBits() == 32);
  CHECK(stream.GetBitsRemaining() == 96);
}

REX_TEST_CASE("BitStream Advance works", "[stream][bitstream]") {
  std::array<u8, 16> buffer{};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Advance(10);
  CHECK(stream.GetOffsetBits() == 10);

  stream.Advance(5);
  CHECK(stream.GetOffsetBits() == 15);
}

REX_TEST_CASE("BitStream Peek reads without advancing", "[stream][bitstream]") {
  // 0xAB = 10101011
  std::array<u8, 16> buffer = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
  BitStream stream(buffer.data(), buffer.size() * 8);

  u64 val1 = stream.Peek(8);
  CHECK(stream.GetOffsetBits() == 0);

  u64 val2 = stream.Peek(8);
  CHECK(val1 == val2);
}

REX_TEST_CASE("BitStream Read advances offset", "[stream][bitstream]") {
  std::array<u8, 16> buffer = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Read(8);
  CHECK(stream.GetOffsetBits() == 8);

  stream.Read(16);
  CHECK(stream.GetOffsetBits() == 24);
}

REX_TEST_CASE("BitStream Read byte-aligned values", "[stream][bitstream]") {
  std::array<u8, 16> buffer = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
  BitStream stream(buffer.data(), buffer.size() * 8);

  CHECK(stream.Read(8) == 0xAB);
  CHECK(stream.Read(8) == 0xCD);
  CHECK(stream.Read(8) == 0xEF);
}

REX_TEST_CASE("BitStream Read 16-bit aligned", "[stream][bitstream]") {
  std::array<u8, 16> buffer = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
  BitStream stream(buffer.data(), buffer.size() * 8);

  CHECK(stream.Read(16) == 0xABCD);
  CHECK(stream.Read(16) == 0xEF12);
}

REX_TEST_CASE("BitStream Read 32-bit aligned", "[stream][bitstream]") {
  std::array<u8, 16> buffer = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
  BitStream stream(buffer.data(), buffer.size() * 8);

  CHECK(stream.Read(32) == 0xABCDEF12);
}

REX_TEST_CASE("BitStream Read non-byte-aligned", "[stream][bitstream]") {
  // 0xAB = 10101011, 0xCD = 11001101
  std::array<u8, 16> buffer = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
  BitStream stream(buffer.data(), buffer.size() * 8);

  CHECK(stream.Read(4) == 0xA);

  CHECK(stream.Read(4) == 0xB);

  CHECK(stream.Read(8) == 0xCD);
}

REX_TEST_CASE("BitStream Read single bits", "[stream][bitstream]") {
  // 0xAB = 10101011
  std::array<u8, 16> buffer = {0xAB, 0xCD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  BitStream stream(buffer.data(), buffer.size() * 8);

  CHECK(stream.Read(1) == 1);
  CHECK(stream.Read(1) == 0);
  CHECK(stream.Read(1) == 1);
  CHECK(stream.Read(1) == 0);
  CHECK(stream.Read(1) == 1);
  CHECK(stream.Read(1) == 0);
  CHECK(stream.Read(1) == 1);
  CHECK(stream.Read(1) == 1);
}

REX_TEST_CASE("BitStream Read crossing byte boundary", "[stream][bitstream]") {
  std::array<u8, 16> buffer = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Advance(4);

  // Last 4 bits of 0xFF and first 4 bits of 0x00 give 11110000 = 0xF0.
  CHECK(stream.Read(8) == 0xF0);
}

REX_TEST_CASE("BitStream Write byte-aligned", "[stream][bitstream]") {
  std::array<u8, 16> buffer{};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Write(0xAB, 8);
  CHECK(stream.GetOffsetBits() == 8);

  stream.SetOffset(0);
  CHECK(stream.Read(8) == 0xAB);
}

REX_TEST_CASE("BitStream Write 16-bit value", "[stream][bitstream]") {
  std::array<u8, 16> buffer{};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Write(0x1234, 16);

  stream.SetOffset(0);
  CHECK(stream.Read(16) == 0x1234);
}

REX_TEST_CASE("BitStream Write non-byte-aligned", "[stream][bitstream]") {
  std::array<u8, 16> buffer{};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Write(0xA, 4);
  stream.Write(0xB, 4);

  stream.SetOffset(0);
  CHECK(stream.Read(8) == 0xAB);
}

REX_TEST_CASE("BitStream Write preserves surrounding bits", "[stream][bitstream]") {
  std::array<u8, 16> buffer = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  BitStream stream(buffer.data(), buffer.size() * 8);

  stream.Advance(4);
  stream.Write(0x0, 4);

  stream.SetOffset(0);
  CHECK(stream.Read(4) == 0xF);
  CHECK(stream.Read(4) == 0x0);
}

REX_TEST_CASE("BitStream Read zero bits returns zero", "[stream][bitstream]") {
  std::array<u8, 16> buffer = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  BitStream stream(buffer.data(), buffer.size() * 8);

  CHECK(stream.Read(0) == 0);
  CHECK(stream.GetOffsetBits() == 0);
}

REX_TEST_CASE("BitStream maximum bits (57)", "[stream][bitstream]") {
  std::array<u8, 16> buffer = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  BitStream stream(buffer.data(), buffer.size() * 8);

  // 57 bits of all 1s = 0x1FFFFFFFFFFFFFF
  u64 expected = (1ULL << 57) - 1;
  CHECK(stream.Read(57) == expected);
}

REX_TEST_CASE("BitStream BitsRemaining decreases", "[stream][bitstream]") {
  std::array<u8, 16> buffer{};
  BitStream stream(buffer.data(), 64);

  CHECK(stream.GetBitsRemaining() == 64);

  stream.Read(16);
  CHECK(stream.GetBitsRemaining() == 48);

  stream.Read(32);
  CHECK(stream.GetBitsRemaining() == 16);
}
