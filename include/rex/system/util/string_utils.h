/**
 * @file        kernel/util/string_utils.h
 * @brief       String utility functions for kernel operations
 *
 * @copyright   Copyright 2022 Ben Vanik. All rights reserved. (Xenia Project)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <string>
#include <string_view>

#include <rex/memory.h>
#include <rex/system/xio.h>  // X_ANSI_STRING, X_UNICODE_STRING
#include <rex/system/xtypes.h>

namespace rex {
namespace system {
namespace util {

inline std::string_view TranslateAnsiString(const memory::Memory* memory,
                                            const X_ANSI_STRING* ansi_string) {
  if (!ansi_string || !ansi_string->length) {
    return "";
  }
  return std::string_view(memory->TranslateVirtual<const char*>(ansi_string->pointer),
                          ansi_string->length);
}

inline std::string_view TranslateAnsiStringAddress(const memory::Memory* memory,
                                                   uint32_t guest_address) {
  if (!guest_address) {
    return "";
  }
  return TranslateAnsiString(memory, memory->TranslateVirtual<const X_ANSI_STRING*>(guest_address));
}

inline std::u16string TranslateUnicodeString(const memory::Memory* memory,
                                             const X_UNICODE_STRING* unicode_string) {
  if (!unicode_string) {
    return u"";
  }
  uint16_t length = unicode_string->length;
  if (!length) {
    return u"";
  }
  auto src = memory->TranslateVirtual<const uint16_t*>(unicode_string->pointer);
  std::u16string result;
  result.reserve(length);
  for (uint16_t i = 0; i < length / 2; ++i) {
    uint16_t c = rex::byte_swap(src[i]);
    result.push_back(c);
  }
  return result;
}

}  // namespace util
}  // namespace system
}  // namespace rex
