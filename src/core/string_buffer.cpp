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
  buffer_offset_ = 0;
  buffer_[0] = 0;
}

void StringBuffer::Grow(size_t additional_length) {
  if (buffer_offset_ + additional_length <= buffer_capacity_) {
    return;
  }
  size_t old_capacity = buffer_capacity_;
  size_t new_capacity =
      std::max(rex::round_up(buffer_offset_ + additional_length, 16_KiB), old_capacity * 2);
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
  std::memset(buffer_ + buffer_offset_, c, count);
  buffer_offset_ += count;
  buffer_[buffer_offset_] = 0;
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
  vsnprintf(buffer_ + buffer_offset_, buffer_capacity_, format, args);
  buffer_offset_ += length;
  buffer_[buffer_offset_] = 0;
}

void StringBuffer::AppendBytes(const u8* buffer, size_t length) {
  Grow(length + 1);
  memcpy(buffer_ + buffer_offset_, buffer, length);
  buffer_offset_ += length;
  buffer_[buffer_offset_] = 0;
}

std::string StringBuffer::ToString() {
  return std::string(buffer_, buffer_offset_);
}

std::string_view StringBuffer::ToStringView() const {
  return std::string_view(buffer_, buffer_offset_);
}

std::vector<u8> StringBuffer::ToBytes() const {
  std::vector<u8> bytes(buffer_offset_);
  std::memcpy(bytes.data(), buffer_, buffer_offset_);
  return bytes;
}

}  // namespace rex
