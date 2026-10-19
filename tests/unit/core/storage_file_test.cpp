/**
 * @file        tests/unit/core/storage_file_test.cpp
 * @brief       The storage file helpers open, append, read, size and reset a host file
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"

#include <array>
#include <fstream>

#include <rex/platform/file.h>

REX_TEST_CASE("A storage file is created, appended to, read back and sized", "[core][file]") {
  rex::test::TempFolder folder;
  const auto path = folder.path / "storage.bin";
  std::fstream file;
  REQUIRE(rex::platform::OpenStorageFile(file, path));
  constexpr std::array<u8, 4> kBytes = {1, 2, 3, 4};
  rex::platform::AppendStorageFile(file, kBytes.data(), kBytes.size());
  file.flush();
  CHECK(rex::platform::GetStorageFileSize(path) == 4);
  file.seekg(0);
  std::array<u8, 8> read{};
  CHECK(rex::platform::ReadStorageFile(file, read.data(), read.size()) == 4);
  CHECK(read[0] == 1);
  CHECK(read[3] == 4);
}

REX_TEST_CASE("Resetting a storage file truncates it and appends after the new end",
              "[core][file]") {
  rex::test::TempFolder folder;
  const auto path = folder.path / "storage.bin";
  std::fstream file;
  REQUIRE(rex::platform::OpenStorageFile(file, path));
  constexpr std::array<u8, 6> kBytes = {1, 2, 3, 4, 5, 6};
  rex::platform::AppendStorageFile(file, kBytes.data(), kBytes.size());
  rex::platform::ResetStorageFile(file, path, 2);
  CHECK(rex::platform::GetStorageFileSize(path) == 2);
  rex::platform::AppendStorageFile(file, kBytes.data(), 1);
  file.flush();
  CHECK(rex::platform::GetStorageFileSize(path) == 3);
}

REX_TEST_CASE("The size of a missing storage file is 0", "[core][file]") {
  rex::test::TempFolder folder;
  CHECK(rex::platform::GetStorageFileSize(folder.path / "missing.bin") == 0);
}
