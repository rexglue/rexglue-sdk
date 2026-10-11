/**
 * @file        tests/unit/memory/file_mapping_test.cpp
 * @brief       Named file mappings are exclusive to their creator
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <filesystem>
#include <format>
#include <random>

#include <rex/memory/virtual_memory.h>

using rex::memory::CloseFileMappingHandle;
using rex::memory::CreateFileMappingHandle;
using rex::memory::kFileMappingHandleInvalid;
using rex::memory::PageAccess;

REX_TEST_CASE("Creating a file mapping under a name that exists fails", "[memory][file_mapping]") {
  const std::filesystem::path name =
      std::format("rex_file_mapping_test_{:08x}", static_cast<rex::u32>(std::random_device{}()));
  const auto first = CreateFileMappingHandle(name, 0x10000, PageAccess::kReadWrite, false);
  REQUIRE(first != kFileMappingHandleInvalid);

  const auto second = CreateFileMappingHandle(name, 0x10000, PageAccess::kReadWrite, false);
  CHECK(second == kFileMappingHandleInvalid);
  if (second != kFileMappingHandleInvalid) {
    CloseFileMappingHandle(second, name);
  }
  CloseFileMappingHandle(first, name);
}
