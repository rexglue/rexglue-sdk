/**
 * @file        tests/unit/filesystem/filesystem_module_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstring>
#include <span>

#include "rex_test.h"

#include <rex/filesystem/file_system.h>
#include <rex/filesystem/file_system_factory.h>
#include <rex/filesystem/stfs.h>
#include <rex/memory/host.h>

#include "base/module_group.h"
#include "fixture_dir.h"
#include <rex/kernel/types/xex.h>

REX_TEST_CASE("The file system module names its tag when it connects", "[filesystem][module]") {
  rex::ModuleGroup group;
  REQUIRE(group.Load("rexfilesystem").has_value());
  REQUIRE(group.Connect().has_value());
  CHECK(std::strcmp(rex::memory::GetTagName(rex::filesystem::kFilesystem), "Filesystem") == 0);
}

REX_TEST_CASE("The file system factory creates the container parsers", "[filesystem][module]") {
  rex::ModuleGroup group;
  REQUIRE(group.Load("rexfilesystem").has_value());
  REQUIRE(group.Connect().has_value());
  auto* files = group.Get<rex::filesystem::IFileSystemFactory>();
  REQUIRE(files != nullptr);

  auto container = files->OpenContainer(rex::test::GetFixtureFolder() / "stfs_con.bin");
  REQUIRE(container);
  CHECK((*container)->GetType() == rex::filesystem::ContainerType::kSTFS);
  CHECK((*container)->GetHeader() != nullptr);
  CHECK_FALSE(files->OpenContainer(rex::test::GetFixtureFolder() / "missing.bin"));

  auto header = files->ReadPackageHeader(rex::test::GetFixtureFolder() / "stfs_con.bin");
  CHECK(header);
  CHECK_FALSE(files->ReadPackageHeader(std::span<const rex::u8>{}));

  CHECK_FALSE(files->ParseXEX(std::span<const rex::u8>{}));
  CHECK_FALSE(files->ParsePE(std::span<const rex::u8>{}));
  CHECK_FALSE(
      files->FindXEXOptHeader(std::span<const rex::u8>{}, rex::kernel::kXEXHeaderEntryPoint));
  CHECK_FALSE(files->OpenXDBF(nullptr, 0)->IsValid());
}
