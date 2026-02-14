/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <span>

#include <rex/assert.h>
#include <rex/stream.h>

namespace rex::stream {

ByteStream::ByteStream(uint8_t* data, size_t data_length, size_t offset)
    : data_(data), data_length_(data_length), offset_(offset) {}

ByteStream::~ByteStream() = default;

void ByteStream::Advance(size_t num_bytes) {
  assert_true(offset_ + num_bytes <= data_length_);
  offset_ += num_bytes;
}

void ByteStream::Read(std::span<uint8_t> buf) {
  assert_true(offset_ + buf.size() <= data_length_);
  std::memcpy(buf.data(), data_ + offset_, buf.size());
  Advance(buf.size());
}

void ByteStream::Write(std::span<const uint8_t> buf) {
  assert_true(offset_ + buf.size() <= data_length_);
  std::memcpy(data_ + offset_, buf.data(), buf.size());
  Advance(buf.size());
}

template <>
std::string ByteStream::Read() {
  std::string str;
  uint32_t len = Read<uint32_t>();
  str.resize(len);
  Read(std::span<uint8_t>(reinterpret_cast<uint8_t*>(str.data()), len));
  return str;
}

template <>
std::u16string ByteStream::Read() {
  std::u16string str;
  size_t len = Read<uint32_t>();
  str.resize(len);
  Read(std::span<uint8_t>(reinterpret_cast<uint8_t*>(str.data()), len * sizeof(char16_t)));
  return str;
}

}  // namespace rex::stream
