/**
 * @file        tests/filesystem/wildcard_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include "filesystem/wildcard.h"

namespace {

using rex::filesystem::WildcardMatcher;

bool Matches(std::string_view pattern, std::string_view name) {
  return WildcardMatcher(pattern).Match(name);
}

}  // namespace

TEST_CASE("An empty pattern and a star match every name", "[filesystem][wildcard]") {
  CHECK(Matches("", "anything"));
  CHECK(Matches("*", "anything"));
  CHECK(Matches("*", ""));
}

TEST_CASE("Literals match whole names ignoring case", "[filesystem][wildcard]") {
  CHECK(Matches("Data.BIN", "data.bin"));
  CHECK_FALSE(Matches("data.bin", "data.bin2"));
  CHECK_FALSE(Matches("data.bin", "xdata.bin"));
}

TEST_CASE("A literal after a star is retried at every later offset", "[filesystem][wildcard]") {
  CHECK(Matches("*a", "data"));
  CHECK(Matches("*.bin", "x.bin.bin"));
  CHECK(Matches("a*b*c", "abxbc"));
  CHECK_FALSE(Matches("*a", "dat"));
}

TEST_CASE("A question mark matches exactly 1 character", "[filesystem][wildcard]") {
  CHECK(Matches("file?.dat", "file1.dat"));
  CHECK_FALSE(Matches("file?.dat", "file12.dat"));
  CHECK_FALSE(Matches("file?.dat", "file.dat"));
  CHECK(Matches("file?", "file1"));
  CHECK_FALSE(Matches("file?", "file12"));
  CHECK_FALSE(Matches("file?", "file"));
}

TEST_CASE("Star dot star matches every name", "[filesystem][wildcard]") {
  CHECK(Matches("*.*", "readme"));
  CHECK(Matches("*.*", "a.b"));
  CHECK(Matches("*.*", ""));
}

TEST_CASE("The DOS star stops at the final dot", "[filesystem][wildcard]") {
  CHECK(Matches("<.txt", "a.b.txt"));
  CHECK(Matches("<", "abc"));
  CHECK_FALSE(Matches("<", "a.b.txt"));
  CHECK_FALSE(Matches("<", "a."));
  CHECK(Matches("<.", "a."));
  CHECK_FALSE(Matches("<.bin", "a.txt"));
}

TEST_CASE("The DOS question mark matches 1 character or nothing before a dot or the end",
          "[filesystem][wildcard]") {
  CHECK(Matches(">>>.txt", "ab.txt"));
  CHECK(Matches(">>>.txt", "abc.txt"));
  CHECK_FALSE(Matches(">>>.txt", "abcd.txt"));
  CHECK(Matches("a>>", "a"));
  CHECK(Matches("a>>", "abc"));
  CHECK_FALSE(Matches("a>.txt", "a..txt"));
}

TEST_CASE("The DOS dot matches a dot or the end of the name", "[filesystem][wildcard]") {
  CHECK(Matches("readme\"", "readme"));
  CHECK(Matches("readme\"", "readme."));
  CHECK_FALSE(Matches("readme\"", "readmex"));
  CHECK(Matches("a\"txt", "a.txt"));
}
