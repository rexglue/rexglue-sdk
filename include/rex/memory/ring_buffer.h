/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include <rex/assert.h>
#include <rex/types.h>

namespace rex::memory {

// Use uint32_t instead of size_t to eliminate REX prefix on x86-64
// instructions in this hot class.
using ring_size_t = uint32_t;

class RingBuffer {
 public:
  RingBuffer(uint8_t* buffer, size_t capacity);

  uint8_t* buffer() const { return buffer_; }
  ring_size_t capacity() const { return capacity_; }
  bool empty() const { return read_offset_ == write_offset_; }

  ring_size_t read_offset() const { return read_offset_; }
  uintptr_t read_ptr() const { return uintptr_t(buffer_) + static_cast<uintptr_t>(read_offset_); }
  void set_read_offset(size_t offset) {
    read_offset_ = static_cast<ring_size_t>(offset) % capacity_;
  }
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

  ring_size_t write_offset() const { return write_offset_; }
  uintptr_t write_ptr() const { return uintptr_t(buffer_) + static_cast<uintptr_t>(write_offset_); }
  void set_write_offset(size_t offset) {
    write_offset_ = static_cast<ring_size_t>(offset) % capacity_;
  }
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

  void AdvanceRead(size_t count);
  void AdvanceWrite(size_t count);

  struct ReadRange {
    const uint8_t* __restrict first;
    const uint8_t* __restrict second;
    ring_size_t first_length;
    ring_size_t second_length;
  };
  ReadRange BeginRead(size_t count);
  void EndRead(ReadRange read_range);

  size_t Read(uint8_t* buffer, size_t count);
  template <typename T>
  size_t Read(T* buffer, size_t count) {
    return Read(reinterpret_cast<uint8_t*>(buffer), count);
  }

  template <typename T>
  T Read() {
    static_assert(std::is_fundamental<T>::value, "Immediate read only supports basic types!");

    T imm;
    size_t read = Read(reinterpret_cast<uint8_t*>(&imm), sizeof(T));
    assert_true(read == sizeof(T));
    return imm;
  }

  template <typename T>
  T ReadAndSwap() {
    static_assert(std::is_fundamental<T>::value, "Immediate read only supports basic types!");

    T imm;
    size_t read = Read(reinterpret_cast<uint8_t*>(&imm), sizeof(T));
    assert_true(read == sizeof(T));
    imm = rex::byte_swap(imm);
    return imm;
  }

  size_t Write(const uint8_t* buffer, size_t count);
  template <typename T>
  size_t Write(const T* buffer, size_t count) {
    return Write(reinterpret_cast<const uint8_t*>(buffer), count);
  }

  template <typename T>
  size_t Write(T& data) {
    return Write(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
  }

 private:
  uint8_t* __restrict buffer_ = nullptr;
  ring_size_t capacity_ = 0;
  ring_size_t read_offset_ = 0;
  ring_size_t write_offset_ = 0;
};

// Fast path for GPU command processor - the hottest path.
// Direct load bypasses generic Read() + memcpy.
// Uses == (not >=) for wrap check: capacity must be a multiple of 4,
// so next_read_offset can only land exactly at capacity, never past it.
template <>
inline uint32_t RingBuffer::ReadAndSwap<uint32_t>() {
  assert_true(capacity_ >= 4);
  ring_size_t read_offs = read_offset_;
  ring_size_t next_read_offs = read_offs + 4;
  if (next_read_offs == capacity_) [[unlikely]] {
    next_read_offs = 0;
  }
  read_offset_ = next_read_offs;
  uint32_t ring_value = *(uint32_t*)(buffer_ + read_offs);
  return rex::byte_swap(ring_value);
}

}  // namespace rex::memory
