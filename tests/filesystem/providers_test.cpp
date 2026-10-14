/**
 * @file        tests/filesystem/providers_test.cpp
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

#include "rex_test.h"

#include "filesystem/fixture.h"

namespace {

using rex::u8;
using rex::filesystem::Access;
using rex::filesystem::CodeOf;
using rex::filesystem::Disposition;
using rex::filesystem::FileAttributes;
using rex::filesystem::FileSystemError;
using rex::filesystem::MountFlags;
using rex::filesystem::OpenAction;
using rex::filesystem::ProviderKind;
using rex::filesystem::test::Fixture;
using rex::filesystem::test::ReadText;
using rex::filesystem::test::WriteHost;

constexpr std::string_view kRoot = "\\Device\\Test";
const std::filesystem::path& Fixtures() {
  static const std::filesystem::path kDir = REXGLUE_FIXTURE_DIR;
  return kDir;
}

std::string BigBin() {
  std::string bytes(0x1800, '\0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>(i % 251);
  }
  return bytes;
}

std::vector<std::string> Names(Fixture& fixture, std::string_view path, std::string_view pattern) {
  auto list = fixture.GetFs().List(path, pattern);
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
  REQUIRE(
      fixture.GetFs().Mount({std::string(kRoot), kind, Fixtures() / image, MountFlags::kWritable}));
  CHECK(Names(fixture, kRoot, "") == std::vector<std::string>{"a.txt", "big.bin", "folder"});
  CHECK(Names(fixture, "\\Device\\Test\\folder", "*.txt") == std::vector<std::string>{"c.txt"});
  CHECK(ReadText(*fixture.Open("\\Device\\Test\\a.txt").file) == "alpha\n");
  CHECK(ReadText(*fixture.Open("\\Device\\Test\\big.bin").file) == BigBin());
  CHECK(ReadText(*fixture.Open("\\Device\\Test\\folder\\c.txt").file) == "charlie in folder\n");

  auto opened = fixture.Open("\\Device\\Test\\a.txt", {.access = Access::kGenericAll});
  CHECK_FALSE(rex::HasFlags(opened.file->GetAccess(), Access::kWriteData));
  const std::array<u8, 1> patch{'x'};
  CHECK_FALSE(opened.file->Write(patch, 0));
  CHECK(opened.file->GetVolume().name == volume);

  auto created =
      fixture.GetFs().Open("\\Device\\Test\\new.txt",
                           {.disposition = Disposition::kCreate, .access = Access::kGenericWrite});
  REQUIRE_FALSE(created);
  CHECK(CodeOf(created.error()) == FileSystemError::kAccessDenied);
  auto removed = fixture.GetFs().Remove("\\Device\\Test\\a.txt");
  REQUIRE_FALSE(removed);
  CHECK(CodeOf(removed.error()) == FileSystemError::kAccessDenied);
}

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "Host provider reads, writes and changes files",
                     "[filesystem][providers]") {
  WriteHost(GetTemp() / "a.txt", "alpha");
  WriteHost(GetTemp() / "b.bin", "bravo");
  MountTemp(kRoot, MountFlags::kWritable);
  REQUIRE(GetFs().Link("test:", kRoot));

  auto opened = Open("\\Device\\Test\\a.txt", {.access = Access::kGenericAll});
  CHECK(ReadText(*opened.file) == "alpha");
  const std::array<u8, 3> patch{'A', 'L', 'P'};
  auto written = opened.file->Write(patch, 0);
  REQUIRE(written);
  CHECK(*written == patch.size());
  REQUIRE(opened.file->SetLength(8));
  opened.file->Refresh();
  CHECK(opened.file->GetInfo().size == 8);
  auto host = rex::platform::ReadWholeFile(GetTemp() / "a.txt");
  REQUIRE(host);
  CHECK(std::string(host->begin(), host->begin() + 5) == "ALPha");

  REQUIRE(opened.file->SetAttributes(FileAttributes::kHidden));
  CHECK(opened.file->GetInfo().attributes == FileAttributes::kHidden);
  REQUIRE(opened.file->SetTimes({.write = 0x01D0000000000000}));
  CHECK(opened.file->GetInfo().write_time == 0x01D0000000000000);
  CHECK(opened.file->GetVolume().name == "STFS");
  opened.file.reset();

  CHECK(Names(*this, kRoot, "*.txt") == std::vector<std::string>{"a.txt"});
  CHECK(Names(*this, kRoot, "") == std::vector<std::string>{"a.txt", "b.bin"});

  auto renamed = Open("\\Device\\Test\\b.bin", {.access = Access::kGenericAll});
  REQUIRE(GetFs().Rename(*renamed.file, "test:\\c.bin", false));
  CHECK(renamed.file->GetInfo().name == "c.bin");
  renamed.file.reset();
  CHECK(std::filesystem::exists(GetTemp() / "c.bin"));
  CHECK_FALSE(std::filesystem::exists(GetTemp() / "b.bin"));

  REQUIRE(GetFs().Remove("\\Device\\Test\\a.txt"));
  CHECK_FALSE(std::filesystem::exists(GetTemp() / "a.txt"));
  auto volume = GetFs().Volume(kRoot);
  REQUIRE(volume);
  CHECK(volume->name == "STFS");
}

REX_TEST_CASE_METHOD(Fixture, "Host provider finds files created on the host after the mount",
                     "[filesystem][providers]") {
  MountTemp(kRoot);
  WriteHost(GetTemp() / "late.txt", "late");
  WriteHost(GetTemp() / "Mixed.txt", "mixed");
  REQUIRE(GetFs().Stat("\\Device\\Test\\late.txt"));
  REQUIRE(GetFs().Stat("\\Device\\Test\\mixed.TXT"));
  CHECK(ReadText(*Open("\\Device\\Test\\late.txt").file) == "late");
  CHECK(ReadText(*Open("\\Device\\Test\\MIXED.txt").file) == "mixed");
}

REX_TEST_CASE_METHOD(Fixture, "Open if keeps a host file added after the mount",
                     "[filesystem][providers]") {
  WriteHost(GetTemp() / "sub" / "seen.txt", "seen");
  MountTemp(kRoot);
  WriteHost(GetTemp() / "sub" / "late.txt", "late bytes");
  WriteHost(GetTemp() / "root_late.txt", "root bytes");

  auto opened = Open("\\Device\\Test\\sub\\late.txt",
                     {.disposition = Disposition::kOpenIf, .access = Access::kGenericAll});
  CHECK(opened.action == OpenAction::kOpened);
  CHECK(ReadText(*opened.file) == "late bytes");
  opened.file.reset();
  auto at_root = Open("\\Device\\Test\\root_late.txt",
                      {.disposition = Disposition::kOpenIf, .access = Access::kGenericAll});
  CHECK(at_root.action == OpenAction::kOpened);
  at_root.file.reset();

  auto host = rex::platform::ReadWholeFile(GetTemp() / "sub" / "late.txt");
  REQUIRE(host);
  CHECK(std::string(host->begin(), host->end()) == "late bytes");
  auto root_host = rex::platform::ReadWholeFile(GetTemp() / "root_late.txt");
  REQUIRE(root_host);
  CHECK(std::string(root_host->begin(), root_host->end()) == "root bytes");
}

REX_TEST_CASE_METHOD(Fixture, "Create of a name that exists only on the host is kExists",
                     "[filesystem][providers]") {
  MountTemp(kRoot);
  WriteHost(GetTemp() / "late.txt", "late");
  auto created = GetFs().Open("\\Device\\Test\\late.txt", {.disposition = Disposition::kCreate,
                                                           .access = Access::kGenericWrite});
  REQUIRE_FALSE(created);
  CHECK(CodeOf(created.error()) == FileSystemError::kExists);
  auto host = rex::platform::ReadWholeFile(GetTemp() / "late.txt");
  REQUIRE(host);
  CHECK(std::string(host->begin(), host->end()) == "late");
}

REX_TEST_CASE_METHOD(Fixture, "Host provider refuses names that leave the mounted folder",
                     "[filesystem][providers]") {
  WriteHost(GetTemp() / "outside.txt", "outside");
  WriteHost(GetTemp() / "mount" / "a.txt", "alpha");
  REQUIRE(GetFs().Mount({std::string(kRoot), ProviderKind::kHostDirectory, GetTemp() / "mount",
                         MountFlags::kWritable}));

  std::string escape(kRoot);
  escape += '\\';
  escape += GetTemp().root_name().string();
  for (const auto& part : GetTemp().relative_path()) {
    escape += '\\';
    escape += part.string();
  }
  escape += "\\outside.txt";
  CHECK_FALSE(GetFs().Stat(escape));

  auto stream = GetFs().Open("\\Device\\Test\\a.txt:hidden", {.disposition = Disposition::kCreate,
                                                              .access = Access::kGenericWrite});
  CHECK_FALSE(stream);
  CHECK_FALSE(std::filesystem::exists(GetTemp() / "mount" / "a.txt:hidden"));
}

REX_TEST_CASE_METHOD(Fixture, "Null provider answers only its paths", "[filesystem][providers]") {
  REQUIRE(GetFs().Mount({"\\Device\\Null",
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
  CHECK(opened.file->GetVolume().name == "NullDevice");

  auto missing = GetFs().Open("\\Device\\Null\\Cache2", {});
  REQUIRE_FALSE(missing);
  CHECK(CodeOf(missing.error()) == FileSystemError::kNotFound);
}

REX_TEST_CASE_METHOD(Fixture, "STFS provider mounts the fixture read only",
                     "[filesystem][providers]") {
  CheckContainer(*this, ProviderKind::kSTFS, "stfs_con.bin", "STFS");
}

REX_TEST_CASE_METHOD(Fixture, "Disc provider mounts the fixture read only",
                     "[filesystem][providers]") {
  CheckContainer(*this, ProviderKind::kDiscImage, "gdfx.iso", "GDFX");
}

REX_TEST_CASE_METHOD(Fixture, "A container of the wrong kind is kCorrupt",
                     "[filesystem][providers]") {
  auto mounted =
      GetFs().Mount({std::string(kRoot), ProviderKind::kDiscImage, Fixtures() / "stfs_con.bin"});
  REQUIRE_FALSE(mounted);
  CHECK(CodeOf(mounted.error()) == FileSystemError::kCorrupt);
  auto missing = GetFs().Mount({std::string(kRoot), ProviderKind::kSTFS, GetTemp() / "none.bin"});
  REQUIRE_FALSE(missing);
  CHECK(CodeOf(missing.error()) == FileSystemError::kNotFound);
}
