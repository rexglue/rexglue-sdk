/**
 * @file        tests/unit/base/hash_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <rex/hash.h>

namespace fs = std::filesystem;

REX_TEST_CASE("HashBytes is stable and 128 bits wide", "[base][hash]") {
  auto digest = rex::HashBytes("hello world");
  CHECK(digest.size() == 32);
  CHECK(digest == rex::HashBytes("hello world"));
  CHECK(digest != rex::HashBytes("hello worlds"));
  CHECK(rex::HashBytes("") == "99aa06d3014798d86001c324468d497f");
}

REX_TEST_CASE("HashFile matches HashBytes over the same content", "[base][hash]") {
  const rex::test::TempFolder folder("rexglue_hash_test_");
  const fs::path& root = folder.path;

  const std::string small = "binary bytes";
  const std::string large = std::string(size_t{1024} * 1024, 'x') + "tail";

  auto write = [&](std::string_view name, const std::string& content) {
    auto path = root / fs::path(name);
    std::ofstream(path, std::ios::binary) << content;
    return path;
  };

  CHECK(rex::HashFile(write("small.bin", small)) == rex::HashBytes(small));
  CHECK(rex::HashFile(write("large.bin", large)) == rex::HashBytes(large));
}

REX_TEST_CASE("HashFile returns empty for a missing file", "[base][hash]") {
  CHECK(rex::HashFile(fs::temp_directory_path() / "rexglue_hash_test_absent.bin").empty());
}

REX_TEST_CASE("HashFile returns empty for a directory", "[base][hash]") {
  CHECK(rex::HashFile(fs::temp_directory_path()).empty());
}

REX_TEST_CASE("KeyHasher hashes the key member with std::hash", "[base][hash]") {
  struct Wide {
    rex::u64 key;
  };
  struct Narrow {
    union {
      rex::u32 key;
      rex::u8 bytes[4];
    };
  };
  CHECK(rex::KeyHasher<Wide>{}(Wide{0x123456789ull}) == std::hash<rex::u64>{}(0x123456789ull));
  Narrow narrow;
  narrow.key = 77;
  CHECK(rex::KeyHasher<Narrow>{}(narrow) == std::hash<rex::u32>{}(77));
}
