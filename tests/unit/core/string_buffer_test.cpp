/**
 * @file        tests/unit/core/string_buffer_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstdarg>
#include <string>

#include <rex/string/buffer.h>

namespace {

std::string Format(const char* format, ...) {
  rex::StringBuffer buffer;
  va_list args;
  va_start(args, format);
  buffer.AppendVarargs(format, args);
  va_end(args);
  return buffer.ToString();
}

}  // namespace

REX_TEST_CASE("AppendVarargs formats every argument", "[core][string]") {
  REQUIRE(Format("%s-%d-%s", "alpha", 42, "beta") == "alpha-42-beta");
}
