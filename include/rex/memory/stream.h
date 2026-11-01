/**
 * @file        memory/stream.h
 * @brief       Bit, byte and ring buffer streams over memory
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/bit_stream.h, xenia/base/byte_stream.h and
 *              xenia/base/ring_buffer.h (Ben Vanik, 2015-2020).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <rex/assert.h>
#include <rex/memory/endian.h>
#include <rex/platform.h>
#include <rex/types.h>

namespace rex::memory {

/** Bit cursor over a buffer the caller owns. Bits are read most significant bit first. */
class REX_API BitStream {
 public:
  /**
   * Wraps a buffer.
   *
   * @param size_in_bits Size of buffer in bits.
   */
  BitStream(u8* buffer, size_t size_in_bits);
  ~BitStream() = default;

  const u8* GetBuffer() const { return buffer_; }

  u8* GetBuffer() { return buffer_; }

  size_t GetOffsetBits() const { return offset_bits_; }

  size_t GetSizeBits() const { return size_bits_; }

  void Advance(size_t num_bits);

  /** Moves the cursor, clamped to the buffer size. */
  void SetOffset(size_t offset_bits);

  size_t GetBitsRemaining() const;

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
   * Writes bits at the cursor and moves past them.
   *
   * @param val Bits to write, right aligned.
   * @param num_bits Bit count, 0 to 57.
   */
  bool Write(u64 val, size_t num_bits);

  /**
   * Copies bits from the cursor into a byte buffer and moves past them.
   *
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
   * @param offset Initial cursor position in bytes.
   */
  ByteStream(u8* data, size_t data_length, size_t offset = 0);
  ~ByteStream() = default;

  void Advance(size_t num_bytes);

  /**
   * Copies bytes from the cursor into a buffer and moves past them.
   *
   * @param buf Destination. Its size is the byte count.
   */
  void Read(std::span<u8> buf);

  /**
   * Copies bytes from a buffer to the cursor and moves past them.
   *
   * @param buf Source. Its size is the byte count.
   */
  void Write(std::span<const u8> buf);

  /** Copies bytes from the cursor into a buffer and moves past them. */
  void Read(void* buf, size_t len) { return Read(std::span<u8>(reinterpret_cast<u8*>(buf), len)); }

  /** Copies bytes from a buffer to the cursor and moves past them. */
  void Write(const void* buf, size_t len) {
    return Write(std::span<const u8>(reinterpret_cast<const u8*>(buf), len));
  }

  const u8* GetData() const { return data_; }

  u8* GetData() { return data_; }

  size_t GetSize() const { return data_length_; }

  size_t GetOffset() const { return offset_; }

  void SetOffset(size_t offset) { offset_ = offset; }

  /**
   * Reads a value at the cursor and moves past it.
   *
   * @tparam T Trivially copyable value type, or std::string or std::u16string.
   */
  template <typename T>
  T Read() {
    T data;
    Read(std::span<u8>(reinterpret_cast<u8*>(&data), sizeof(T)));
    return data;
  }

  /** Writes a value at the cursor and moves past it. */
  template <typename T>
  void Write(T data) {
    Write(std::span<const u8>(reinterpret_cast<const u8*>(&data), sizeof(T)));
  }

  /** Writes a u32 character count followed by the characters. */
  void Write(const std::string_view str) {
    Write(static_cast<u32>(str.length()));
    Write(str.data(), str.length() * sizeof(char));
  }

  /** Writes a u32 code unit count followed by the code units. */
  void Write(const std::u16string_view str) {
    Write(static_cast<u32>(str.length()));
    Write(str.data(), str.length() * sizeof(char16_t));
  }

 private:
  u8* data_ = nullptr;
  size_t data_length_ = 0;
  size_t offset_ = 0;
};

/** Reads a u32 character count followed by the characters. */
template <>
REX_API std::string ByteStream::Read();

/** Reads a u32 code unit count followed by the code units. */
template <>
REX_API std::u16string ByteStream::Read();

using RingSizeT = u32;

/**
 * Ring buffer of bytes over memory the caller owns. The buffer is empty when the read and write
 * offsets are equal.
 */
class REX_API RingBuffer {
 public:
  RingBuffer(u8* buffer, size_t capacity);

  u8* GetBuffer() const { return buffer_; }

  RingSizeT GetCapacity() const { return capacity_; }

  /**
   * Reports whether the read and write offsets are equal.
   *
   * @return True when no bytes are waiting to be read.
   */
  bool IsEmpty() const { return read_offset_ == write_offset_; }

  RingSizeT GetReadOffset() const { return read_offset_; }

  /** Returns the address of the next read. It is GetBuffer() plus GetReadOffset(). */
  uintptr_t GetReadPtr() const {
    return reinterpret_cast<uintptr_t>(buffer_) + static_cast<uintptr_t>(read_offset_);
  }

  /**
   * Sets the read offset.
   *
   * @param offset New offset, wrapped to the capacity.
   */
  void SetReadOffset(size_t offset) { read_offset_ = static_cast<RingSizeT>(offset) % capacity_; }

  /** Returns the number of bytes between the read and write offsets. */
  RingSizeT GetReadCount() const {
    RingSizeT read_offs = read_offset_;
    RingSizeT write_offs = write_offset_;
    RingSizeT cap = capacity_;
    if (read_offs <= write_offs) {
      return write_offs - read_offs;
    } else {
      return (cap - read_offs) + write_offs;
    }
  }

  RingSizeT GetWriteOffset() const { return write_offset_; }

  /** Returns the address of the next write. It is GetBuffer() plus GetWriteOffset(). */
  uintptr_t GetWritePtr() const {
    return reinterpret_cast<uintptr_t>(buffer_) + static_cast<uintptr_t>(write_offset_);
  }

  /**
   * Sets the write offset.
   *
   * @param offset New offset, wrapped to the capacity.
   */
  void SetWriteOffset(size_t offset) { write_offset_ = static_cast<RingSizeT>(offset) % capacity_; }

  /**
   * Returns the number of bytes that can be written. It is the full capacity when the buffer is
   * empty.
   */
  RingSizeT GetWriteCount() const {
    RingSizeT read_offs = read_offset_;
    RingSizeT write_offs = write_offset_;
    RingSizeT cap = capacity_;
    if (read_offs == write_offs) {
      return cap;
    } else if (write_offs < read_offs) {
      return read_offs - write_offs;
    } else {
      return (cap - write_offs) + read_offs;
    }
  }

  /** Moves the read offset forward, wrapping at the capacity. */
  void AdvanceRead(size_t count);

  /** Moves the write offset forward, wrapping at the capacity. */
  void AdvanceWrite(size_t count);

  /** Bytes at the read offset, split in two parts when they wrap. */
  struct ReadRange {
    const u8* __restrict first;  /**< Bytes from the read offset, or null when empty. */
    const u8* __restrict second; /**< Bytes from the buffer start, or null without a wrap. */
    RingSizeT first_length;
    RingSizeT second_length;
  };

  /**
   * Returns the bytes at the read offset without moving it. The bytes come in one or two parts.
   *
   * @param count Byte count, clamped to the capacity.
   */
  ReadRange BeginRead(size_t count);

  /**
   * Moves the read offset past a range BeginRead returned.
   *
   * @param read_range Range BeginRead returned.
   */
  void EndRead(ReadRange read_range);

  /**
   * Copies bytes from the read offset into a buffer and moves the read offset past them.
   *
   * @param count Byte count, clamped to the capacity.
   * @return Bytes copied.
   */
  size_t Read(u8* buffer, size_t count);

  /**
   * Copies bytes from the read offset into a typed buffer and moves the read offset past them.
   *
   * @param count Byte count, clamped to the capacity.
   * @return Bytes copied.
   */
  template <typename T>
  size_t Read(T* buffer, size_t count) {
    return Read(reinterpret_cast<u8*>(buffer), count);
  }

  /** Reads a value in its stored byte order. */
  template <typename T>
  T Read() {
    static_assert(std::is_fundamental<T>::value, "Immediate read only supports basic types!");

    T imm;
    size_t read = Read(reinterpret_cast<u8*>(&imm), sizeof(T));
    REX_ASSERT(read == sizeof(T));
    return imm;
  }

  /** Reads a value and swaps its byte order. */
  template <typename T>
  T ReadAndSwap() {
    static_assert(std::is_fundamental<T>::value, "Immediate read only supports basic types!");

    T imm;
    size_t read = Read(reinterpret_cast<u8*>(&imm), sizeof(T));
    REX_ASSERT(read == sizeof(T));
    imm = rex::byte_swap(imm);
    return imm;
  }

  /**
   * Copies bytes from a buffer to the write offset and moves the write offset past them.
   *
   * @param count Byte count, clamped to the capacity.
   * @return Bytes copied.
   */
  size_t Write(const u8* buffer, size_t count);

  /**
   * Copies bytes from a typed buffer to the write offset and moves the write offset past them.
   *
   * @param count Byte count, clamped to the capacity.
   * @return Bytes copied.
   */
  template <typename T>
  size_t Write(const T* buffer, size_t count) {
    return Write(reinterpret_cast<const u8*>(buffer), count);
  }

  /**
   * Writes the bytes of one value in its host byte order.
   *
   * @return Bytes copied.
   */
  template <typename T>
  size_t Write(T& data) {
    return Write(reinterpret_cast<const u8*>(&data), sizeof(T));
  }

 private:
  u8* __restrict buffer_ = nullptr;
  RingSizeT capacity_ = 0;
  RingSizeT read_offset_ = 0;
  RingSizeT write_offset_ = 0;
};

/** Reads a 32 bit value and swaps its byte order. The capacity must be a multiple of 4. */
template <>
inline u32 RingBuffer::ReadAndSwap<u32>() {
  REX_ASSERT(capacity_ >= 4);
  RingSizeT read_offs = read_offset_;
  RingSizeT next_read_offs = read_offs + 4;
  if (next_read_offs == capacity_) [[unlikely]] {
    next_read_offs = 0;
  }
  read_offset_ = next_read_offs;
  return memory::load<be_u32>(buffer_ + read_offs);
}

}  // namespace rex::memory
