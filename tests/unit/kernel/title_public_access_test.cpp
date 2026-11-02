/**
 * @file        tests/unit/kernel/title_public_access_test.cpp
 * @brief       FindFunction and GetFileSystem on TitleRuntime
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <filesystem>
#include <span>

#include <rex/filesystem/file_open.h>
#include <rex/filesystem/mount.h>
#include <rex/filesystem/file_system.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/kernel/title_runtime.h>

#include "kernel/title_fixture.h"
#include "test_memory.h"

namespace {

using rex::filesystem::Access;
using rex::filesystem::Disposition;
using rex::filesystem::MountEntry;
using rex::filesystem::MountFlags;
using rex::filesystem::OpenOptions;
using rex::filesystem::ProviderType;

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
};

void Registered(PPCContext&, rex::u8*) {}

const PPCFuncMapping kMappings[] = {
    {0x82000100, Registered},
    {0, nullptr},
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "FindFunction returns the function registered at an address",
                     "[kernel][title]") {
  rex::test::TitleBoot boot(true);
  rex::ppc::PPCImage image = rex::test::MakeTitleImage(kMappings);
  image.code_base = 0x82000000;
  rex::kernel::TitleRuntime runtime(image);
  REQUIRE(runtime.Load());

  const auto found = runtime.FindFunction(0x82000100);
  REQUIRE(found);
  CHECK(*found == Registered);
  runtime.Unload();
}

REX_TEST_CASE_METHOD(Fixture, "FindFunction fails for an address with no function",
                     "[kernel][title]") {
  rex::test::TitleBoot boot(true);
  rex::ppc::PPCImage image = rex::test::MakeTitleImage(kMappings);
  image.code_base = 0x82000000;
  rex::kernel::TitleRuntime runtime(image);
  REQUIRE(runtime.Load());

  const auto missing = runtime.FindFunction(0x82000200);
  REQUIRE_FALSE(missing);
  CHECK(missing.error().category == rex::ErrorCategory::kNotFound);
  runtime.Unload();
}

REX_TEST_CASE_METHOD(Fixture, "GetFileSystem mounts a writable host folder and links a path",
                     "[kernel][title]") {
  rex::test::TitleBoot boot(true);
  rex::kernel::TitleRuntime runtime(rex::test::MakeTitleImage());
  REQUIRE(runtime.Load());
  rex::filesystem::IFileSystem& files = runtime.GetFileSystem();

  const std::filesystem::path save = boot.folder.path / "save";
  std::filesystem::create_directories(save);
  REQUIRE(files.Mount(MountEntry{.path = "\\Device\\Save",
                                 .type = ProviderType::kHostDirectory,
                                 .source = save,
                                 .flags = MountFlags::kWritable}));
  REQUIRE(files.Link("save:", "\\Device\\Save"));
  CHECK(files.FindLink("save:\\slot0.bin").value_or("") == "\\Device\\Save");

  {
    auto opened = files.Open("save:\\slot0.bin", OpenOptions{.disposition = Disposition::kCreate,
                                                             .access = Access::kWriteData});
    REQUIRE(opened);
    const std::array<rex::u8, 4> bytes = {1, 2, 3, 4};
    REQUIRE(opened->file->Write(std::span<const rex::u8>(bytes), 0));
  }
  CHECK(std::filesystem::file_size(save / "slot0.bin") == 4);

  REQUIRE(files.Unlink("save:"));
  CHECK_FALSE(files.FindLink("save:\\slot0.bin").has_value());
  CHECK_FALSE(files.Unlink("save:"));
  runtime.Unload();
}
