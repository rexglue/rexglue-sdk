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

#include <algorithm>
#include <cstdarg>

#include <rex/assert.h>
#include <rex/literals.h>
#include <rex/math.h>
#include <rex/string/buffer.h>

namespace rex {

StringBuffer::StringBuffer(size_t initial_capacity) {
  buffer_capacity_ = std::max(initial_capacity, 16_KiB);
  buffer_ = reinterpret_cast<char*>(std::malloc(buffer_capacity_));
  REX_ASSERT(buffer_ != nullptr);
  buffer_[0] = 0;
}

StringBuffer::~StringBuffer() {
  free(buffer_);
  buffer_ = nullptr;
}

void StringBuffer::Reset() {
  length_ = 0;
  buffer_[0] = 0;
}

void StringBuffer::Grow(size_t additional_length) {
  if (length_ + additional_length <= buffer_capacity_) {
    return;
  }
  size_t old_capacity = buffer_capacity_;
  size_t new_capacity =
      std::max(rex::RoundUpNonZero(length_ + additional_length, 16_KiB), old_capacity * 2);
  auto new_buffer = std::realloc(buffer_, new_capacity);
  REX_ASSERT(new_buffer != nullptr);
  buffer_ = reinterpret_cast<char*>(new_buffer);
  buffer_capacity_ = new_capacity;
}

void StringBuffer::Append(char c) {
  AppendBytes(reinterpret_cast<const u8*>(&c), 1);
}

void StringBuffer::Append(char c, size_t count) {
  Grow(count + 1);
  std::memset(buffer_ + length_, c, count);
  length_ += count;
  buffer_[length_] = 0;
}

void StringBuffer::Append(const char* value) {
  AppendBytes(reinterpret_cast<const u8*>(value), std::strlen(value));
}

void StringBuffer::Append(const std::string_view value) {
  AppendBytes(reinterpret_cast<const u8*>(value.data()), value.size());
}

void StringBuffer::AppendVarargs(const char* format, va_list args) {
  va_list size_args;
  va_copy(size_args, args);
  int result = vsnprintf(nullptr, 0, format, size_args);
  va_end(size_args);
  if (result <= 0) {
    return;
  }
  auto length = static_cast<size_t>(result);
  Grow(length + 1);
  vsnprintf(buffer_ + length_, buffer_capacity_, format, args);
  length_ += length;
  buffer_[length_] = 0;
}

void StringBuffer::AppendBytes(const u8* buffer, size_t length) {
  Grow(length + 1);
  memcpy(buffer_ + length_, buffer, length);
  length_ += length;
  buffer_[length_] = 0;
}

std::string StringBuffer::ToString() {
  return std::string(buffer_, length_);
}

std::string_view StringBuffer::ToStringView() const {
  return std::string_view(buffer_, length_);
}

std::vector<u8> StringBuffer::ToBytes() const {
  std::vector<u8> bytes(length_);
  std::memcpy(bytes.data(), buffer_, length_);
  return bytes;
}

}  // namespace rex
