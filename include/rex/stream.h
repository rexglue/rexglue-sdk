/**
 * @file        stream.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/bit_stream.h and xenia/base/byte_stream.h
 *              (Ben Vanik, 2015-2020).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <rex/macros.h>
#include <rex/types.h>

namespace rex {

/** Bit cursor over a buffer the caller owns. Bits are read most significant bit first. */
class REX_API BitStream {
 public:
  /**
   * Wraps a buffer.
   *
   * @param buffer Backing memory.
   * @param size_in_bits Size of buffer in bits.
   */
  BitStream(u8* buffer, size_t size_in_bits);
  /** Destroys the stream. The buffer stays with the caller. */
  ~BitStream() = default;

  /**
   * Returns the backing memory.
   *
   * @return Pointer to the first byte.
   */
  const u8* buffer() const { return buffer_; }

  /**
   * Returns the backing memory.
   *
   * @return Pointer to the first byte.
   */
  u8* buffer() { return buffer_; }

  /**
   * Returns the cursor position.
   *
   * @return Offset in bits.
   */
  size_t offset_bits() const { return offset_bits_; }

  /**
   * Returns the buffer size.
   *
   * @return Size in bits.
   */
  size_t size_bits() const { return size_bits_; }

  /**
   * Moves the cursor forward.
   *
   * @param num_bits Bit count.
   */
  void Advance(size_t num_bits);

  /**
   * Moves the cursor, clamped to the buffer size.
   *
   * @param offset_bits New offset in bits.
   */
  void SetOffset(size_t offset_bits);

  /**
   * Returns the bits after the cursor.
   *
   * @return Bit count.
   */
  size_t BitsRemaining() const;

  /**
   * Reads bits at the cursor without moving it.
   *
   * @param num_bits Bit count, 0 to 57.
   * @return The bits, right aligned.
   */
  u64 Peek(size_t num_bits);

  /**
   * Reads bits at the cursor and moves past them.
   *
   * @param num_bits Bit count, 0 to 57.
   * @return The bits, right aligned.
   */
  u64 Read(size_t num_bits);

  /**
   * Writes bits at the cursor and moves past them. Untested.
   *
   * @param val Bits to write, right aligned.
   * @param num_bits Bit count, 0 to 57.
   * @return True.
   */
  bool Write(u64 val, size_t num_bits);

  /**
   * Copies bits from the cursor into a byte buffer and moves past them.
   *
   * @param dest_buffer Destination.
   * @param num_bits Bit count.
   * @return Bit offset of the cursor within its byte before the copy.
   */
  size_t Copy(u8* dest_buffer, size_t num_bits);

 private:
  u8* buffer_ = nullptr;
  size_t offset_bits_ = 0;
  size_t size_bits_ = 0;
};

/** Byte cursor over a buffer the caller owns. Values are copied in host byte order. */
class REX_API ByteStream {
 public:
  /**
   * Wraps a buffer.
   *
   * @param data Backing memory.
   * @param data_length Byte size of data.
   * @param offset Initial cursor position in bytes.
   */
  ByteStream(u8* data, size_t data_length, size_t offset = 0);
  /** Destroys the stream. The buffer stays with the caller. */
  ~ByteStream() = default;

  /**
   * Moves the cursor forward.
   *
   * @param num_bytes Byte count.
   */
  void Advance(size_t num_bytes);

  /**
   * Copies bytes at the cursor out and moves past them.
   *
   * @param buf Destination. Its size is the byte count.
   */
  void Read(std::span<u8> buf);

  /**
   * Copies bytes in at the cursor and moves past them.
   *
   * @param buf Source. Its size is the byte count.
   */
  void Write(std::span<const u8> buf);

  /**
   * Copies bytes at the cursor out and moves past them.
   *
   * @param buf Destination.
   * @param len Byte count.
   */
  void Read(void* buf, size_t len) { return Read(std::span<u8>(reinterpret_cast<u8*>(buf), len)); }

  /**
   * Copies bytes in at the cursor and moves past them.
   *
   * @param buf Source.
   * @param len Byte count.
   */
  void Write(const void* buf, size_t len) {
    return Write(std::span<const u8>(reinterpret_cast<const u8*>(buf), len));
  }

  /**
   * Returns the backing memory.
   *
   * @return Pointer to the first byte.
   */
  const u8* data() const { return data_; }

  /**
   * Returns the backing memory.
   *
   * @return Pointer to the first byte.
   */
  u8* data() { return data_; }

  /**
   * Returns the buffer size.
   *
   * @return Byte count.
   */
  size_t data_length() const { return data_length_; }

  /**
   * Returns the cursor position.
   *
   * @return Offset in bytes.
   */
  size_t offset() const { return offset_; }

  /**
   * Moves the cursor.
   *
   * @param offset New offset in bytes.
   */
  void set_offset(size_t offset) { offset_ = offset; }

  /**
   * Reads 1 value at the cursor and moves past it.
   *
   * @tparam T Trivially copyable value type, or std::string or std::u16string.
   * @return The value.
   */
  template <typename T>
  T Read() {
    T data;
    Read(std::span<u8>(reinterpret_cast<u8*>(&data), sizeof(T)));
    return data;
  }

  /**
   * Writes 1 value at the cursor and moves past it.
   *
   * @tparam T Trivially copyable value type.
   * @param data Value to write.
   */
  template <typename T>
  void Write(T data) {
    Write(std::span<const u8>(reinterpret_cast<const u8*>(&data), sizeof(T)));
  }

  /**
   * Writes a u32 character count followed by the characters.
   *
   * @param str String to write.
   */
  void Write(const std::string_view str) {
    Write(static_cast<u32>(str.length()));
    Write(str.data(), str.length() * sizeof(char));
  }

  /**
   * Writes a u32 code unit count followed by the code units.
   *
   * @param str String to write.
   */
  void Write(const std::u16string_view str) {
    Write(static_cast<u32>(str.length()));
    Write(str.data(), str.length() * sizeof(char16_t));
  }

 private:
  u8* data_ = nullptr;
  size_t data_length_ = 0;
  size_t offset_ = 0;
};

/**
 * Reads a u32 character count followed by the characters.
 *
 * @return The string.
 */
template <>
REX_API std::string ByteStream::Read();

/**
 * Reads a u32 code unit count followed by the code units.
 *
 * @return The string.
 */
template <>
REX_API std::u16string ByteStream::Read();

}  // namespace rex
