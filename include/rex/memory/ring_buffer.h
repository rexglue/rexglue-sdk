/**
 * @file        memory/ring_buffer.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/ring_buffer.h (Ben Vanik, 2015).
 */

#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include <rex/macros.h>
#include <rex/assert.h>
#include <rex/memory/utils.h>
#include <rex/types.h>

namespace rex::memory {

/** Offset and size type of RingBuffer. 32 bits keeps the x86-64 REX.W prefix off the hot path. */
using ring_size_t = u32;

/**
 * Ring buffer of bytes over memory the caller owns. The buffer is empty when the read and write
 * offsets are equal.
 */
class REX_API RingBuffer {
 public:
  /**
   * Wraps a buffer.
   *
   * @param buffer Backing memory.
   * @param capacity Byte size of buffer.
   */
  RingBuffer(u8* buffer, size_t capacity);

  /**
   * Returns the backing memory.
   *
   * @return Pointer to the first byte.
   */
  u8* buffer() const { return buffer_; }

  /**
   * Returns the size of the backing memory.
   *
   * @return Byte count.
   */
  ring_size_t capacity() const { return capacity_; }

  /**
   * Reports whether the read and write offsets are equal.
   *
   * @return True when no bytes are waiting to be read.
   */
  bool empty() const { return read_offset_ == write_offset_; }

  /**
   * Returns the read offset.
   *
   * @return Byte offset of the next read.
   */
  ring_size_t read_offset() const { return read_offset_; }

  /**
   * Returns the address of the next read.
   *
   * @return buffer() plus read_offset().
   */
  uintptr_t read_ptr() const {
    return reinterpret_cast<uintptr_t>(buffer_) + static_cast<uintptr_t>(read_offset_);
  }

  /**
   * Sets the read offset.
   *
   * @param offset New offset, wrapped to the capacity.
   */
  void set_read_offset(size_t offset) {
    read_offset_ = static_cast<ring_size_t>(offset) % capacity_;
  }

  /**
   * Returns the number of bytes between the read and write offsets.
   *
   * @return Bytes waiting to be read.
   */
  ring_size_t read_count() const {
    ring_size_t read_offs = read_offset_;
    ring_size_t write_offs = write_offset_;
    ring_size_t cap = capacity_;
    if (read_offs <= write_offs) {
      return write_offs - read_offs;
    } else {
      return (cap - read_offs) + write_offs;
    }
  }

  /**
   * Returns the write offset.
   *
   * @return Byte offset of the next write.
   */
  ring_size_t write_offset() const { return write_offset_; }

  /**
   * Returns the address of the next write.
   *
   * @return buffer() plus write_offset().
   */
  uintptr_t write_ptr() const {
    return reinterpret_cast<uintptr_t>(buffer_) + static_cast<uintptr_t>(write_offset_);
  }

  /**
   * Sets the write offset.
   *
   * @param offset New offset, wrapped to the capacity.
   */
  void set_write_offset(size_t offset) {
    write_offset_ = static_cast<ring_size_t>(offset) % capacity_;
  }

  /**
   * Returns the number of bytes that can be written.
   *
   * @return Free bytes, the full capacity when the buffer is empty.
   */
  ring_size_t write_count() const {
    ring_size_t read_offs = read_offset_;
    ring_size_t write_offs = write_offset_;
    ring_size_t cap = capacity_;
    if (read_offs == write_offs) {
      return cap;
    } else if (write_offs < read_offs) {
      return read_offs - write_offs;
    } else {
      return (cap - write_offs) + read_offs;
    }
  }

  /**
   * Moves the read offset forward, wrapping at the capacity.
   *
   * @param count Byte count.
   */
  void AdvanceRead(size_t count);

  /**
   * Moves the write offset forward, wrapping at the capacity.
   *
   * @param count Byte count.
   */
  void AdvanceWrite(size_t count);

  /** Bytes at the read offset, split in 2 parts when they wrap. */
  struct ReadRange {
    const u8* __restrict first;  /**< Bytes from the read offset, or nullptr when empty. */
    const u8* __restrict second; /**< Bytes from the buffer start, or nullptr without a wrap. */
    ring_size_t first_length;    /**< Byte count of first. */
    ring_size_t second_length;   /**< Byte count of second. */
  };

  /**
   * Returns the bytes at the read offset without moving it. Pass the result to EndRead.
   *
   * @param count Byte count, clamped to the capacity.
   * @return The bytes in 1 or 2 parts.
   */
  ReadRange BeginRead(size_t count);

  /**
   * Moves the read offset past a range BeginRead returned.
   *
   * @param read_range Range BeginRead returned.
   */
  void EndRead(ReadRange read_range);

  /**
   * Copies bytes out and moves the read offset past them.
   *
   * @param buffer Destination.
   * @param count Byte count, clamped to the capacity.
   * @return Bytes copied.
   */
  size_t Read(u8* buffer, size_t count);

  /**
   * Copies bytes out into a typed buffer and moves the read offset past them.
   *
   * @tparam T Element type of the destination.
   * @param buffer Destination.
   * @param count Byte count, clamped to the capacity.
   * @return Bytes copied.
   */
  template <typename T>
  size_t Read(T* buffer, size_t count) {
    return Read(reinterpret_cast<u8*>(buffer), count);
  }

  /**
   * Reads 1 value in its stored byte order.
   *
   * @tparam T Fundamental type.
   * @return The value.
   */
  template <typename T>
  T Read() {
    static_assert(std::is_fundamental<T>::value, "Immediate read only supports basic types!");

    T imm;
    size_t read = Read(reinterpret_cast<u8*>(&imm), sizeof(T));
    REX_ASSERT(read == sizeof(T));
    return imm;
  }

  /**
   * Reads 1 value and swaps its byte order.
   *
   * @tparam T Fundamental type.
   * @return The byte swapped value.
   */
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
   * Copies bytes in and moves the write offset past them.
   *
   * @param buffer Source.
   * @param count Byte count, clamped to the capacity.
   * @return Bytes copied.
   */
  size_t Write(const u8* buffer, size_t count);

  /**
   * Copies bytes in from a typed buffer and moves the write offset past them.
   *
   * @tparam T Element type of the source.
   * @param buffer Source.
   * @param count Byte count, clamped to the capacity.
   * @return Bytes copied.
   */
  template <typename T>
  size_t Write(const T* buffer, size_t count) {
    return Write(reinterpret_cast<const u8*>(buffer), count);
  }

  /**
   * Writes the bytes of 1 value in its host byte order.
   *
   * @tparam T Value type.
   * @param data Value to write.
   * @return Bytes copied.
   */
  template <typename T>
  size_t Write(T& data) {
    return Write(reinterpret_cast<const u8*>(&data), sizeof(T));
  }

 private:
  u8* __restrict buffer_ = nullptr;
  ring_size_t capacity_ = 0;
  ring_size_t read_offset_ = 0;
  ring_size_t write_offset_ = 0;
};

/**
 * Reads 1 32 bit value and swaps its byte order with a direct load. The capacity is a multiple
 * of 4, so the wrap check compares for equality.
 *
 * @return The byte swapped value.
 */
template <>
inline u32 RingBuffer::ReadAndSwap<u32>() {
  REX_ASSERT(capacity_ >= 4);
  ring_size_t read_offs = read_offset_;
  ring_size_t next_read_offs = read_offs + 4;
  if (next_read_offs == capacity_) [[unlikely]] {
    next_read_offs = 0;
  }
  read_offset_ = next_read_offs;
  return memory::load<be_u32>(buffer_ + read_offs);
}

}  // namespace rex::memory
