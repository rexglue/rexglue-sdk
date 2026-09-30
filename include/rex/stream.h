/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015-2020 Ben Vanik. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 Tom Clay <tomc@tctechstuff.com>
 *              Modified for rexglue - Xbox 360 recompilation framework
 *
 * @changes     - Consolidated bit.h and byte.h into single header
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace rex {

//=============================================================================
// BitStream - Bit-level stream operations
//=============================================================================

class BitStream {
 public:
  BitStream(uint8_t* buffer, size_t size_in_bits);
  ~BitStream();

  const uint8_t* buffer() const { return buffer_; }
  uint8_t* buffer() { return buffer_; }
  size_t offset_bits() const { return offset_bits_; }
  size_t size_bits() const { return size_bits_; }

  void Advance(size_t num_bits);
  void SetOffset(size_t offset_bits);
  size_t BitsRemaining();

  // Note: num_bits MUST be in the range 0-57 (inclusive)
  uint64_t Peek(size_t num_bits);
  uint64_t Read(size_t num_bits);
  bool Write(uint64_t val, size_t num_bits);  // TODO(DrChat): Not tested!

  size_t Copy(uint8_t* dest_buffer, size_t num_bits);

 private:
  uint8_t* buffer_ = nullptr;
  size_t offset_bits_ = 0;
  size_t size_bits_ = 0;
};

//=============================================================================
// ByteStream - Byte-level stream operations
//=============================================================================

class ByteStream {
 public:
  ByteStream(uint8_t* data, size_t data_length, size_t offset = 0);
  ~ByteStream();

  void Advance(size_t num_bytes);
  void Read(std::span<uint8_t> buf);
  void Write(std::span<const uint8_t> buf);

  // Convenience overloads for void* (legacy compatibility)
  void Read(void* buf, size_t len) {
    return Read(std::span<uint8_t>(reinterpret_cast<uint8_t*>(buf), len));
  }
  void Write(const void* buf, size_t len) {
    return Write(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(buf), len));
  }

  const uint8_t* data() const { return data_; }
  uint8_t* data() { return data_; }
  size_t data_length() const { return data_length_; }

  size_t offset() const { return offset_; }
  void set_offset(size_t offset) { offset_ = offset; }

  template <typename T>
  T Read() {
    T data;
    Read(std::span<uint8_t>(reinterpret_cast<uint8_t*>(&data), sizeof(T)));
    return data;
  }

  template <typename T>
  void Write(T data) {
    Write(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&data), sizeof(T)));
  }

  void Write(const std::string_view str) {
    Write(uint32_t(str.length()));
    Write(str.data(), str.length() * sizeof(char));
  }

  void Write(const std::u16string_view str) {
    Write(uint32_t(str.length()));
    Write(str.data(), str.length() * sizeof(char16_t));
  }

 private:
  uint8_t* data_ = nullptr;
  size_t data_length_ = 0;
  size_t offset_ = 0;
};

template <>
std::string ByteStream::Read();

template <>
std::u16string ByteStream::Read();

}  // namespace rex
