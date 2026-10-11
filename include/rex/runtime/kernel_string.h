/**
 * @file        runtime/kernel_string.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <string_view>

#include <rex/memory.h>
#include <rex/string.h>
#include <rex/runtime/xio.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace runtime {

/**
 * Returns the characters of a guest ANSI string without copying.
 *
 * @param memory The guest memory.
 * @param ansi_string The guest string, may be null.
 * @return The text, empty when the string is null or has no length.
 */
inline std::string_view TranslateAnsiString(const runtime::Memory* memory,
                                            const X_ANSI_STRING* ansi_string) {
  if (!ansi_string || !ansi_string->length) {
    return "";
  }
  return std::string_view(memory->TranslateVirtual<const char*>(ansi_string->pointer),
                          ansi_string->length);
}

/**
 * Returns the text of a guest ANSI string with surrounding whitespace removed.
 *
 * @param memory The guest memory.
 * @param ansi_string The guest string, may be null.
 * @return The trimmed path.
 */
inline std::string TranslateAnsiPath(const runtime::Memory* memory,
                                     const X_ANSI_STRING* ansi_string) {
  return rex::trim_string(std::string(TranslateAnsiString(memory, ansi_string)));
}

/**
 * Returns the characters of the guest ANSI string at an address without copying.
 *
 * @param memory The guest memory.
 * @param guest_address The guest address of the string structure, 0 for none.
 * @return The text, empty when the address is 0.
 */
inline std::string_view TranslateAnsiStringAddress(const runtime::Memory* memory,
                                                   u32 guest_address) {
  if (!guest_address) {
    return "";
  }
  return TranslateAnsiString(memory, memory->TranslateVirtual<const X_ANSI_STRING*>(guest_address));
}

/**
 * Converts a guest UTF-16 string to host byte order.
 *
 * @param memory The guest memory.
 * @param unicode_string The guest string, may be null.
 * @return The text, empty when the string is null or has no length.
 */
inline std::u16string TranslateUnicodeString(const runtime::Memory* memory,
                                             const X_UNICODE_STRING* unicode_string) {
  if (!unicode_string) {
    return u"";
  }
  u16 length = unicode_string->length;
  if (!length) {
    return u"";
  }
  auto src = memory->TranslateVirtual<const be_u16*>(unicode_string->pointer);
  std::u16string result;
  result.reserve(length);
  for (u16 i = 0; i < length / 2; ++i) {
    result.push_back(src[i]);
  }
  return result;
}

}  // namespace runtime
}  // namespace rex
