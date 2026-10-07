/**
 * @file        tests/unit/system/guest_path_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>
#include <rex/runtime/guest_path.h>

using rex::runtime::NormalizeGuestPath;

TEST_CASE("Guest path normalization: strip device prefix", "[system][path]") {
  CHECK(NormalizeGuestPath("game:\\bin\\somelib.dll") == "bin/somelib.dll");
  CHECK(NormalizeGuestPath("GAME:\\BIN\\SomeLib.DLL") == "bin/somelib.dll");
  CHECK(NormalizeGuestPath("d:\\content\\modules\\test.dll") == "content/modules/test.dll");
}

TEST_CASE("Guest path normalization: already normalized", "[system][path]") {
  CHECK(NormalizeGuestPath("bin/somelib.dll") == "bin/somelib.dll");
}

TEST_CASE("Guest path normalization: backslash to forward slash", "[system][path]") {
  CHECK(NormalizeGuestPath("data\\libs\\foo.dll") == "data/libs/foo.dll");
}

TEST_CASE("Guest path normalization: case folding", "[system][path]") {
  CHECK(NormalizeGuestPath("BIN/SomeLib.DLL") == "bin/somelib.dll");
}
