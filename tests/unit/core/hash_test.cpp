/**
 * @file        tests/unit/core/hash_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include <rex/hash.h>

namespace fs = std::filesystem;

TEST_CASE("hash_bytes is stable and 128 bits wide", "[hash]") {
  auto digest = rex::hash_bytes("hello world");
  CHECK(digest.size() == 32);
  CHECK(digest == rex::hash_bytes("hello world"));
  CHECK(digest != rex::hash_bytes("hello worlds"));
}

TEST_CASE("hash_bytes handles empty input", "[hash]") {
  CHECK(rex::hash_bytes("").size() == 32);
}

TEST_CASE("hash_file matches hash_bytes over the same content", "[hash]") {
  auto root = fs::temp_directory_path() / "rexglue_hash_test";
  fs::remove_all(root);
  fs::create_directories(root);

  const std::string small = "binary bytes";
  const std::string large = std::string(1024 * 1024, 'x') + "tail";

  auto write = [&](std::string_view name, const std::string& content) {
    auto path = root / fs::path(name);
    std::ofstream(path, std::ios::binary) << content;
    return path;
  };

  CHECK(rex::hash_file(write("small.bin", small)) == rex::hash_bytes(small));
  CHECK(rex::hash_file(write("large.bin", large)) == rex::hash_bytes(large));

  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("hash_file returns empty for a missing file", "[hash]") {
  CHECK(rex::hash_file(fs::temp_directory_path() / "rexglue_hash_test_absent.bin").empty());
}

TEST_CASE("hash_file returns empty for a directory", "[hash]") {
  CHECK(rex::hash_file(fs::temp_directory_path()).empty());
}
