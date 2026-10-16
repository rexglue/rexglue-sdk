/**
 * @file        tests/unit/core/host_file_io_test.cpp
 * @brief       ReadHostFile reads a range of a host file and WriteHostFile replaces it atomically
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "support/temp_folder.h"

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <rex/platform/file.h>

namespace {

const std::vector<u8>& GetBytes() {
  static const std::vector<u8> kBytes = {10, 11, 12, 13, 14, 15, 16, 17};
  return kBytes;
}

struct HostFileFixture {
  rex::test::TempFolder folder;
  std::filesystem::path path = folder.path / "data.bin";

  HostFileFixture() {
    REQUIRE(rex::platform::WriteHostFile(path, std::span<const u8>(GetBytes())));
  }
};

}  // namespace

REX_TEST_CASE_METHOD(HostFileFixture, "ReadHostFile reads the whole file by default",
                     "[core][file]") {
  auto read = rex::platform::ReadHostFile(path);
  REQUIRE(read);
  CHECK(*read == GetBytes());
}

REX_TEST_CASE_METHOD(HostFileFixture, "ReadHostFile reads a range from an offset and a length",
                     "[core][file]") {
  auto read = rex::platform::ReadHostFile(path, 2, 3);
  REQUIRE(read);
  CHECK(*read == std::vector<u8>({12, 13, 14}));
}

REX_TEST_CASE_METHOD(HostFileFixture, "ReadHostFile with a length of 0 reads to the end",
                     "[core][file]") {
  auto read = rex::platform::ReadHostFile(path, 5);
  REQUIRE(read);
  CHECK(*read == std::vector<u8>({15, 16, 17}));
}

REX_TEST_CASE_METHOD(HostFileFixture, "ReadHostFile at the end gives an empty vector",
                     "[core][file]") {
  auto read = rex::platform::ReadHostFile(path, GetBytes().size());
  REQUIRE(read);
  CHECK(read->empty());
}

REX_TEST_CASE_METHOD(HostFileFixture, "ReadHostFile past the end is an error", "[core][file]") {
  CHECK_FALSE(rex::platform::ReadHostFile(path, GetBytes().size() + 1));
  CHECK_FALSE(rex::platform::ReadHostFile(folder.path / "missing"));
}

REX_TEST_CASE_METHOD(HostFileFixture, "ReadHostFile with a length past the end is a short read",
                     "[core][file]") {
  auto read = rex::platform::ReadHostFile(path, 6, 100);
  REQUIRE(read);
  CHECK(*read == std::vector<u8>({16, 17}));
}

REX_TEST_CASE_METHOD(HostFileFixture, "WriteHostFile writes text and leaves no temp file",
                     "[core][file]") {
  const auto target = folder.path / "a" / "b" / "text.txt";
  REQUIRE(rex::platform::WriteHostFile(target, std::string_view("hello")));
  auto read = rex::platform::ReadHostFile(target);
  REQUIRE(read);
  CHECK(*read == std::vector<u8>({'h', 'e', 'l', 'l', 'o'}));
  CHECK_FALSE(std::filesystem::exists(target.string() + ".tmp"));
  REQUIRE(rex::platform::WriteHostFile(target, std::span<const u8>()));
  read = rex::platform::ReadHostFile(target);
  REQUIRE(read);
  CHECK(read->empty());
}
