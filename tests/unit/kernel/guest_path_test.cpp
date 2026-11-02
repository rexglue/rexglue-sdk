/**
 * @file        tests/unit/kernel/guest_path_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include <rex/string/text.h>
#include <rex/filesystem/path.h>

REX_TEST_CASE("Guest path normalization strips the device prefix", "[kernel][system][path]") {
  CHECK(rex::filesystem::NormalizeGuestPath("game:\\bin\\somelib.dll") == "bin/somelib.dll");
  CHECK(rex::filesystem::NormalizeGuestPath("GAME:\\BIN\\SomeLib.DLL") == "bin/somelib.dll");
  CHECK(rex::filesystem::NormalizeGuestPath("d:\\content\\modules\\test.dll") ==
        "content/modules/test.dll");
}

REX_TEST_CASE("Guest path normalization keeps a normalized path", "[kernel][system][path]") {
  CHECK(rex::filesystem::NormalizeGuestPath("bin/somelib.dll") == "bin/somelib.dll");
}

REX_TEST_CASE("Guest path normalization turns a backslash into a forward slash",
              "[kernel][system][path]") {
  CHECK(rex::filesystem::NormalizeGuestPath("data\\libs\\foo.dll") == "data/libs/foo.dll");
}

REX_TEST_CASE("Guest path normalization folds case", "[kernel][system][path]") {
  CHECK(rex::filesystem::NormalizeGuestPath("BIN/SomeLib.DLL") == "bin/somelib.dll");
}
