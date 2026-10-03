/**
 * @file        tests/filesystem/open_node_test.cpp
 * @brief       Tests that an open file keeps its node through delete, host removal and unmount
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

#include <catch2/catch_test_macros.hpp>

#include "filesystem/fixture.h"

namespace {

using rex::u8;
using rex::filesystem::Access;
using rex::filesystem::CodeOf;
using rex::filesystem::Disposition;
using rex::filesystem::Error;
using rex::filesystem::MountFlags;
using rex::filesystem::ProviderKind;
using rex::filesystem::test::Fixture;
using rex::filesystem::test::ReadText;
using rex::filesystem::test::WriteHost;

constexpr std::string_view kMountPath = "\\OpenRoot";
constexpr std::string_view kPath = "\\OpenRoot\\held.dat";

class OpenRoot : public Fixture {
 public:
  OpenRoot() {
    WriteHost(HostPath(), "held");
    REQUIRE(fs().Mount({std::string(kMountPath), ProviderKind::kHostDirectory, temp(),
                        MountFlags::kWritable | MountFlags::kShareDelete}));
  }

  std::filesystem::path HostPath() const { return temp() / "held.dat"; }
};

}  // namespace

TEST_CASE_METHOD(OpenRoot, "A file with an open handle cannot be deleted",
                 "[filesystem][open_node]") {
  auto reader = Open(kPath, {.disposition = Disposition::kOpen, .access = Access::kGenericRead});

  auto removed = fs().Remove(kPath);
  REQUIRE_FALSE(removed);
  CHECK(CodeOf(removed.error()) == Error::kAccessDenied);
  CHECK(std::filesystem::exists(HostPath()));
  CHECK(ReadText(*reader.file) == "held");

  reader.file.reset();
  CHECK(fs().Remove(kPath));
  CHECK_FALSE(std::filesystem::exists(HostPath()));
}

TEST_CASE_METHOD(OpenRoot, "Delete on close removes the file at its last close",
                 "[filesystem][open_node]") {
  auto first = Open(kPath, {.disposition = Disposition::kOpen, .access = Access::kGenericRead});
  auto second = Open(kPath, {.disposition = Disposition::kOpen, .access = Access::kGenericRead});
  first.file->SetDeleteOnClose(true);

  first.file.reset();
  CHECK(std::filesystem::exists(HostPath()));
  second.file.reset();
  CHECK_FALSE(std::filesystem::exists(HostPath()));
  CHECK_FALSE(fs().Stat(kPath));
}

TEST_CASE_METHOD(OpenRoot, "A file removed on the host stays readable through its open handle",
                 "[filesystem][open_node]") {
  auto reader = Open(kPath, {.disposition = Disposition::kOpen, .access = Access::kGenericRead});
  std::filesystem::remove(HostPath());

  auto created =
      Open(kPath, {.disposition = Disposition::kOpenIf, .access = Access::kGenericWrite});
  const std::array<u8, 3> payload{'n', 'e', 'w'};
  REQUIRE(created.file->Write(payload, 0));

  CHECK(reader.file->Info().name == "held.dat");
  reader.file.reset();
  created.file.reset();
  auto reopened = Open(kPath);
  reopened.file->Refresh();
  CHECK(ReadText(*reopened.file) == "new");
}

TEST_CASE_METHOD(OpenRoot, "An open file outlives the unmount of its mount",
                 "[filesystem][open_node]") {
  auto reader = Open(kPath, {.disposition = Disposition::kOpen, .access = Access::kGenericRead});
  auto list = fs().List(kMountPath, "*");
  REQUIRE(list);

  REQUIRE(fs().Unmount(kMountPath));
  CHECK_FALSE(fs().Stat(kPath));

  CHECK(reader.file->Info().name == "held.dat");
  CHECK(ReadText(*reader.file) == "held");
  auto entry = (*list)->Next();
  REQUIRE(entry);
  CHECK(entry->name == "held.dat");

  fs().Clear();
  CHECK(reader.file->Info().absolute_path == "\\OpenRoot\\held.dat");
}
