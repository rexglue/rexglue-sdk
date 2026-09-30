/**
 * @file        tests/filesystem/providers_test.cpp
 * @brief       Tests for the host, null, STFS and disc image providers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "filesystem/fixture.h"

namespace {

using rex::u8;
using rex::filesystem::Access;
using rex::filesystem::CodeOf;
using rex::filesystem::Disposition;
using rex::filesystem::Error;
using rex::filesystem::FileAttributes;
using rex::filesystem::MountFlags;
using rex::filesystem::ProviderKind;
using rex::filesystem::test::Fixture;
using rex::filesystem::test::ReadText;
using rex::filesystem::test::WriteHost;

constexpr std::string_view kRoot = "\\Device\\Test";
const std::filesystem::path kFixtures = REXGLUE_FIXTURE_DIR;

std::string BigBin() {
  std::string bytes(0x1800, '\0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>(i % 251);
  }
  return bytes;
}

std::vector<std::string> Names(Fixture& fixture, std::string_view path, std::string_view pattern) {
  auto list = fixture.fs().List(path, pattern);
  REQUIRE(list);
  std::vector<std::string> names;
  while (auto entry = (*list)->Next()) {
    names.push_back(entry->name);
  }
  std::ranges::sort(names);
  return names;
}

void CheckContainer(Fixture& fixture, ProviderKind kind, const char* image,
                    std::string_view volume) {
  REQUIRE(fixture.fs().Mount({std::string(kRoot), kind, kFixtures / image, MountFlags::kWritable}));
  CHECK(Names(fixture, kRoot, "") == std::vector<std::string>{"a.txt", "big.bin", "folder"});
  CHECK(Names(fixture, "\\Device\\Test\\folder", "*.txt") == std::vector<std::string>{"c.txt"});
  CHECK(ReadText(*fixture.Open("\\Device\\Test\\a.txt").file) == "alpha\n");
  CHECK(ReadText(*fixture.Open("\\Device\\Test\\big.bin").file) == BigBin());
  CHECK(ReadText(*fixture.Open("\\Device\\Test\\folder\\c.txt").file) == "charlie in folder\n");

  auto opened = fixture.Open("\\Device\\Test\\a.txt", {.access = Access::kGenericAll});
  CHECK_FALSE(rex::HasFlags(opened.file->access(), Access::kWriteData));
  const std::array<u8, 1> patch{'x'};
  CHECK_FALSE(opened.file->Write(patch, 0));
  CHECK(opened.file->Volume().name == volume);

  auto created = fixture.fs().Open("\\Device\\Test\\new.txt", {.disposition = Disposition::kCreate,
                                                               .access = Access::kGenericWrite});
  REQUIRE_FALSE(created);
  CHECK(CodeOf(created.error()) == Error::kAccessDenied);
  auto removed = fixture.fs().Remove("\\Device\\Test\\a.txt");
  REQUIRE_FALSE(removed);
  CHECK(CodeOf(removed.error()) == Error::kAccessDenied);
}

}  // namespace

TEST_CASE_METHOD(Fixture, "Host provider reads, writes and changes files",
                 "[filesystem][providers]") {
  WriteHost(temp() / "a.txt", "alpha");
  WriteHost(temp() / "b.bin", "bravo");
  MountTemp(kRoot, MountFlags::kWritable | MountFlags::kShareDelete);
  REQUIRE(fs().Link("test:", kRoot));

  auto opened = Open("\\Device\\Test\\a.txt", {.access = Access::kGenericAll});
  CHECK(ReadText(*opened.file) == "alpha");
  const std::array<u8, 3> patch{'A', 'L', 'P'};
  auto written = opened.file->Write(patch, 0);
  REQUIRE(written);
  CHECK(*written == patch.size());
  REQUIRE(opened.file->SetLength(8));
  opened.file->Refresh();
  CHECK(opened.file->Info().size == 8);
  auto host = rex::platform::ReadWholeFile(temp() / "a.txt");
  REQUIRE(host);
  CHECK(std::string(host->begin(), host->begin() + 5) == "ALPha");

  REQUIRE(opened.file->SetAttributes(FileAttributes::kHidden));
  CHECK(opened.file->Info().attributes == FileAttributes::kHidden);
  REQUIRE(opened.file->SetTimes({.write = 0x01D0000000000000}));
  CHECK(opened.file->Info().write_time == 0x01D0000000000000);
  CHECK(opened.file->Volume().name == "STFS");
  opened.file.reset();

  CHECK(Names(*this, kRoot, "*.txt") == std::vector<std::string>{"a.txt"});
  CHECK(Names(*this, kRoot, "") == std::vector<std::string>{"a.txt", "b.bin"});

  auto renamed = Open("\\Device\\Test\\b.bin", {.access = Access::kGenericAll});
  REQUIRE(renamed.file->Rename("test:\\c.bin"));
  CHECK(renamed.file->Info().name == "c.bin");
  renamed.file.reset();
  CHECK(std::filesystem::exists(temp() / "c.bin"));
  CHECK_FALSE(std::filesystem::exists(temp() / "b.bin"));

  REQUIRE(fs().Remove("\\Device\\Test\\a.txt"));
  CHECK_FALSE(std::filesystem::exists(temp() / "a.txt"));
  auto volume = fs().Volume(kRoot);
  REQUIRE(volume);
  CHECK(volume->name == "STFS");
}

TEST_CASE_METHOD(Fixture, "Host provider finds files created on the host after the mount",
                 "[filesystem][providers]") {
  MountTemp(kRoot);
  WriteHost(temp() / "late.txt", "late");
  WriteHost(temp() / "Mixed.txt", "mixed");
  REQUIRE(fs().Stat("\\Device\\Test\\late.txt"));
  REQUIRE(fs().Stat("\\Device\\Test\\mixed.TXT"));
  CHECK(ReadText(*Open("\\Device\\Test\\late.txt").file) == "late");
  CHECK(ReadText(*Open("\\Device\\Test\\MIXED.txt").file) == "mixed");
}

TEST_CASE_METHOD(Fixture, "Null provider answers only its paths", "[filesystem][providers]") {
  REQUIRE(fs().Mount({"\\Device\\Null",
                      ProviderKind::kNull,
                      {},
                      MountFlags::kWritable,
                      {"\\Cache0", "\\Cache1"}}));
  auto opened = Open("\\Device\\Null\\cache0", {.access = Access::kGenericAll});
  std::array<u8, 8> buffer{};
  auto read = opened.file->Read(buffer, 0);
  REQUIRE(read);
  CHECK(*read == 0);
  auto written = opened.file->Write(buffer, 0);
  REQUIRE(written);
  CHECK(*written == 0);
  CHECK(opened.file->Volume().name == "NullDevice");

  auto missing = fs().Open("\\Device\\Null\\Cache2", {});
  REQUIRE_FALSE(missing);
  CHECK(CodeOf(missing.error()) == Error::kNotFound);
}

TEST_CASE_METHOD(Fixture, "STFS provider mounts the fixture read only", "[filesystem][providers]") {
  CheckContainer(*this, ProviderKind::kSTFS, "stfs_con.bin", "STFS");
}

TEST_CASE_METHOD(Fixture, "Disc provider mounts the fixture read only", "[filesystem][providers]") {
  CheckContainer(*this, ProviderKind::kDiscImage, "gdfx.iso", "GDFX");
}

TEST_CASE_METHOD(Fixture, "A container of the wrong kind is kCorrupt", "[filesystem][providers]") {
  auto mounted =
      fs().Mount({std::string(kRoot), ProviderKind::kDiscImage, kFixtures / "stfs_con.bin"});
  REQUIRE_FALSE(mounted);
  CHECK(CodeOf(mounted.error()) == Error::kCorrupt);
  auto missing = fs().Mount({std::string(kRoot), ProviderKind::kSTFS, temp() / "none.bin"});
  REQUIRE_FALSE(missing);
  CHECK(CodeOf(missing.error()) == Error::kNotFound);
}
