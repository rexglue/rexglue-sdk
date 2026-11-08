/**
 * @file        tests/unit/filesystem/open_node_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include "rex_test.h"

#include "filesystem/fixture.h"

namespace {

using rex::u8;
using rex::filesystem::Access;
using rex::filesystem::CodeOf;
using rex::filesystem::Disposition;
using rex::filesystem::FileSystemError;
using rex::filesystem::MountFlags;
using rex::filesystem::ProviderType;
using rex::filesystem::test::Fixture;
using rex::filesystem::test::ReadText;
using rex::filesystem::test::WriteHost;

constexpr std::string_view kMountPath = "\\OpenRoot";
constexpr std::string_view kPath = "\\OpenRoot\\held.dat";

class OpenRoot : public Fixture {
 public:
  OpenRoot() {
    WriteHost(GetHostPath(), "held");
    REQUIRE(GetFileSystem().Mount({std::string(kMountPath),
                                   ProviderType::kHostDirectory,
                                   GetTemp(),
                                   MountFlags::kWritable,
                                   {}}));
  }

  std::filesystem::path GetHostPath() const { return GetTemp() / "held.dat"; }
};

}  // namespace

REX_TEST_CASE_METHOD(OpenRoot, "A file with an open handle cannot be deleted",
                     "[filesystem][open_node]") {
  auto reader = Open(kPath, {.disposition = Disposition::kOpen, .access = Access::kReadAccess});

  auto removed = GetFileSystem().Remove(kPath);
  REQUIRE_FALSE(removed);
  CHECK(CodeOf(removed.error()) == FileSystemError::kAccessDenied);
  CHECK(std::filesystem::exists(GetHostPath()));
  CHECK(ReadText(*reader.file) == "held");

  reader.file.reset();
  CHECK(GetFileSystem().Remove(kPath));
  CHECK_FALSE(std::filesystem::exists(GetHostPath()));
}

REX_TEST_CASE_METHOD(OpenRoot, "Delete on close removes the file at its last close",
                     "[filesystem][open_node]") {
  auto first = Open(kPath, {.disposition = Disposition::kOpen, .access = Access::kReadAccess});
  auto second = Open(kPath, {.disposition = Disposition::kOpen, .access = Access::kReadAccess});
  first.file->SetDeleteOnClose(true);

  first.file.reset();
  CHECK(std::filesystem::exists(GetHostPath()));
  second.file.reset();
  CHECK_FALSE(std::filesystem::exists(GetHostPath()));
  CHECK_FALSE(GetFileSystem().Stat(kPath));
}

REX_TEST_CASE_METHOD(OpenRoot, "A file removed on the host stays readable through its open handle",
                     "[filesystem][open_node]") {
  auto reader = Open(kPath, {.disposition = Disposition::kOpen, .access = Access::kReadAccess});
  std::filesystem::remove(GetHostPath());

  auto created = Open(kPath, {.disposition = Disposition::kOpenIf, .access = Access::kWriteAccess});
  const std::array<u8, 3> payload{'n', 'e', 'w'};
  REQUIRE(created.file->Write(payload, 0));

  CHECK(reader.file->GetInfo().name == "held.dat");
  reader.file.reset();
  created.file.reset();
  auto reopened = Open(kPath);
  reopened.file->Refresh();
  CHECK(ReadText(*reopened.file) == "new");
}

REX_TEST_CASE_METHOD(OpenRoot, "An open file outlives the unmount of its mount",
                     "[filesystem][open_node]") {
  auto reader = Open(kPath, {.disposition = Disposition::kOpen, .access = Access::kReadAccess});
  auto list = GetFileSystem().List(kMountPath, "*");
  REQUIRE(list);

  REQUIRE(GetFileSystem().Unmount(kMountPath));
  CHECK_FALSE(GetFileSystem().Stat(kPath));

  CHECK(reader.file->GetInfo().name == "held.dat");
  CHECK(ReadText(*reader.file) == "held");
  auto entry = (*list)->Next();
  CHECK((entry && entry->name == "held.dat"));

  GetFileSystem().Clear();
  CHECK(reader.file->GetInfo().absolute_path == "\\OpenRoot\\held.dat");
}
