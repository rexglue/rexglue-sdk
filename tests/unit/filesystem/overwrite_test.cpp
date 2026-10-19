/**
 * @file        tests/unit/filesystem/overwrite_test.cpp
 * @brief       Tests that overwrite and supersede truncate host files in place
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <filesystem>
#include <string_view>

#include "rex_test.h"

#include "filesystem/fixture.h"

namespace {

using rex::u8;
using rex::filesystem::Access;
using rex::filesystem::Disposition;
using rex::filesystem::MountFlags;
using rex::filesystem::OpenAction;
using rex::filesystem::ProviderKind;
using rex::filesystem::test::Fixture;
using rex::filesystem::test::WriteHost;

constexpr std::string_view kMountPath = "\\TestRoot";
constexpr std::string_view kPath = "test:\\savegame.dat";
constexpr std::string_view kOriginal = "AAAA";

class ScopedRoot : public Fixture {
 public:
  ScopedRoot() {
    WriteHost(GetHostPath(), kOriginal);
    REQUIRE(GetFileSystem().Mount(
        {std::string(kMountPath), ProviderKind::kHostDirectory, GetTemp(), MountFlags::kWritable}));
    REQUIRE(GetFileSystem().Link("test:", kMountPath));
  }

  std::filesystem::path GetHostPath() const { return GetTemp() / "savegame.dat"; }
};

}  // namespace

REX_TEST_CASE_METHOD(ScopedRoot, "Overwrite truncates in place instead of deleting",
                     "[filesystem][overwrite]") {
  auto reader = Open(kPath, {.disposition = Disposition::kOpen, .access = Access::kReadAccess});
  CHECK(reader.action == OpenAction::kOpened);

  auto writer =
      Open(kPath, {.disposition = Disposition::kOverwriteIf, .access = Access::kWriteAccess});
  CHECK(writer.action == OpenAction::kOverwritten);
  CHECK(std::filesystem::file_size(GetHostPath()) == 0);

  const std::array<u8, 1> payload{'B'};
  auto written = writer.file->Write(payload, 0);
  REQUIRE(written);
  CHECK(*written == payload.size());

  std::array<u8, 8> buffer{};
  auto read = reader.file->Read(buffer, 0);
  REQUIRE(read);
  CHECK(*read == payload.size());
  CHECK(buffer[0] == 'B');

  writer.file.reset();
  reader.file.reset();

  CHECK(std::filesystem::exists(GetHostPath()));
  CHECK(std::filesystem::file_size(GetHostPath()) == payload.size());
}

REX_TEST_CASE_METHOD(ScopedRoot, "Supersede of a file held open by a reader keeps the file",
                     "[filesystem][overwrite]") {
  auto reader = Open(kPath, {.disposition = Disposition::kOpen, .access = Access::kReadAccess});

  auto writer =
      Open(kPath, {.disposition = Disposition::kSupersede, .access = Access::kWriteAccess});
  CHECK(writer.action == OpenAction::kSuperseded);

  writer.file.reset();
  reader.file.reset();

  CHECK(std::filesystem::exists(GetHostPath()));
}
