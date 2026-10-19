/**
 * @file        tests/unit/runtime/guest_path_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include <rex/string/utf8.h>

using rex::NormalizeGuestPath;

REX_TEST_CASE("Guest path normalization strips the device prefix", "[runtime][system][path]") {
  CHECK(NormalizeGuestPath("game:\\bin\\somelib.dll") == "bin/somelib.dll");
  CHECK(NormalizeGuestPath("GAME:\\BIN\\SomeLib.DLL") == "bin/somelib.dll");
  CHECK(NormalizeGuestPath("d:\\content\\modules\\test.dll") == "content/modules/test.dll");
}

REX_TEST_CASE("Guest path normalization keeps a normalized path", "[runtime][system][path]") {
  CHECK(NormalizeGuestPath("bin/somelib.dll") == "bin/somelib.dll");
}

REX_TEST_CASE("Guest path normalization turns a backslash into a forward slash",
              "[runtime][system][path]") {
  CHECK(NormalizeGuestPath("data\\libs\\foo.dll") == "data/libs/foo.dll");
}

REX_TEST_CASE("Guest path normalization folds case", "[runtime][system][path]") {
  CHECK(NormalizeGuestPath("BIN/SomeLib.DLL") == "bin/somelib.dll");
}
