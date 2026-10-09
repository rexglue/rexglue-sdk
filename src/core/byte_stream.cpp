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

namespace rex {

ByteStream::ByteStream(u8* data, size_t data_length, size_t offset)
    : data_(data), data_length_(data_length), offset_(offset) {}

ByteStream::~ByteStream() = default;

void ByteStream::Advance(size_t num_bytes) {
  REX_ASSERT(offset_ + num_bytes <= data_length_);
  offset_ += num_bytes;
}

void ByteStream::Read(std::span<u8> buf) {
  REX_ASSERT(offset_ + buf.size() <= data_length_);
  std::memcpy(buf.data(), data_ + offset_, buf.size());
  Advance(buf.size());
}

void ByteStream::Write(std::span<const u8> buf) {
  REX_ASSERT(offset_ + buf.size() <= data_length_);
  std::memcpy(data_ + offset_, buf.data(), buf.size());
  Advance(buf.size());
}

template <>
std::string ByteStream::Read() {
  std::string str;
  u32 len = Read<u32>();
  str.resize(len);
  Read(std::span<u8>(reinterpret_cast<u8*>(str.data()), len));
  return str;
}

template <>
std::u16string ByteStream::Read() {
  std::u16string str;
  size_t len = Read<u32>();
  str.resize(len);
  Read(std::span<u8>(reinterpret_cast<u8*>(str.data()), len * sizeof(char16_t)));
  return str;
}

}  // namespace rex
