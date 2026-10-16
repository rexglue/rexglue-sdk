/**
 * @file        string/buffer.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/string_buffer.h (Ben Vanik, 2020).
 */

#pragma once

#include <cstdarg>
#include <format>
#include <string>
#include <vector>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/types.h>

namespace rex {

/** Growable character buffer that stays NUL terminated after every append. */
class REX_API StringBuffer {
 public:
  /**
   * Allocates the buffer.
   *
   * @param initial_capacity Initial capacity in bytes. The buffer holds at least 16 KiB.
   */
  explicit StringBuffer(size_t initial_capacity = 0);
  /** Frees the buffer. */
  ~StringBuffer();

  /**
   * Returns the characters.
   *
   * @return Pointer to the NUL terminated contents.
   */
  char* GetBuffer() const { return buffer_; }

  /**
   * Returns the content length.
   *
   * @return Byte count, not counting the terminator.
   */
  size_t GetLength() const { return buffer_offset_; }

  /** Empties the buffer and keeps its capacity. */
  void Reset();

  /**
   * Appends a character.
   *
   * @param c Character to append.
   */
  void Append(char c);

  /**
   * Appends a character repeated.
   *
   * @param c Character to append.
   * @param count Number of copies.
   */
  void Append(char c, size_t count);

  /**
   * Appends a NUL terminated string.
   *
   * @param value String to append.
   */
  void Append(const char* value);

  /**
   * Appends a string.
   *
   * @param value String to append.
   */
  void Append(const std::string_view value);

  /**
   * Appends text formatted with std::format.
   *
   * @tparam Args Argument types.
   * @param format Format string, checked at compile time.
   * @param args Format arguments.
   */
  template <typename... Args>
  void AppendFormat(std::format_string<const Args&...> format, const Args&... args) {
    Append(std::format(format, args...));
  }

  /**
   * Appends text formatted with vsnprintf.
   *
   * @param format printf format string.
   * @param args Format arguments.
   */
  void AppendVarargs(const char* format, va_list args);

  /**
   * Appends raw bytes.
   *
   * @param buffer Bytes to append.
   * @param length Byte count.
   */
  void AppendBytes(const u8* buffer, size_t length);

  /**
   * Copies the contents into a string.
   *
   * @return The contents.
   */
  std::string ToString();

  /**
   * Returns a view of the contents.
   *
   * @return View valid until the next append or destruction.
   */
  std::string_view ToStringView() const;

  /**
   * Copies the contents into a byte vector.
   *
   * @return The contents without the terminator.
   */
  std::vector<u8> ToBytes() const;

 private:
  void Grow(size_t additional_length);

  char* buffer_ = nullptr;
  size_t buffer_offset_ = 0;
  size_t buffer_capacity_ = 0;
};

}  // namespace rex
