/**
 * @file        tests/unit/filesystem/file_system_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <string>
#include <string_view>

#include "rex_test.h"

#include "filesystem/fixture.h"

namespace {

using rex::filesystem::Access;
using rex::filesystem::CodeOf;
using rex::filesystem::Disposition;
using rex::filesystem::FileSystemError;
using rex::filesystem::MountFlags;
using rex::filesystem::OpenAction;
using rex::filesystem::OpenOptions;
using rex::filesystem::ProviderKind;
using rex::filesystem::test::Fixture;
using rex::filesystem::test::ReadText;
using rex::filesystem::test::WriteHost;

constexpr std::string_view kRoot = "\\Device\\Test";

FileSystemError OpenError(Fixture& fixture, std::string_view path, const OpenOptions& options) {
  auto opened = fixture.GetFs().Open(path, options);
  REQUIRE_FALSE(opened);
  return CodeOf(opened.error());
}

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "Canonical paths name the same file", "[filesystem][paths]") {
  WriteHost(GetTemp() / "folder" / "c.txt", "charlie");
  MountTemp(kRoot);
  for (std::string_view path :
       {"\\Device\\Test\\folder\\c.txt", "\\Device\\Test\\.\\folder\\c.txt",
        "\\Device\\Test\\folder\\..\\folder\\c.txt", "\\Device\\Test\\\\folder\\\\c.txt",
        "\\Device\\Test/folder/c.txt", "\\DEVICE\\test\\FOLDER\\C.TXT"}) {
    INFO(path);
    auto opened = Open(path);
    CHECK(ReadText(*opened.file) == "charlie");
    CHECK(opened.file->GetInfo().absolute_path == "\\Device\\Test\\folder\\c.txt");
  }
}

REX_TEST_CASE_METHOD(Fixture, "Links substitute by prefix and repeat", "[filesystem][links]") {
  WriteHost(GetTemp() / "a.txt", "alpha");
  MountTemp(kRoot);
  REQUIRE(GetFs().Link("inner:", kRoot));
  REQUIRE(GetFs().Link("outer:", "inner:"));

  CHECK(ReadText(*Open("inner:\\a.txt").file) == "alpha");
  CHECK(ReadText(*Open("outer:\\a.txt").file) == "alpha");
  CHECK(ReadText(*Open("OUTER:\\A.TXT").file) == "alpha");

  CHECK(GetFs().FindLink("Inner:\\a.txt") == std::string(kRoot));
  CHECK(GetFs().FindLink("elsewhere:\\a.txt") == std::nullopt);

  auto again = GetFs().Link("inner:", "\\Device\\Other");
  REQUIRE_FALSE(again);
  CHECK(CodeOf(again.error()) == FileSystemError::kExists);
  CHECK(GetFs().FindLink("inner:") == std::string(kRoot));

  REQUIRE(GetFs().Unlink("INNER:"));
  CHECK(GetFs().FindLink("inner:") == std::nullopt);
  auto missing = GetFs().Unlink("inner:");
  REQUIRE_FALSE(missing);
  CHECK(CodeOf(missing.error()) == FileSystemError::kNotFound);
  CHECK(OpenError(*this, "outer:\\a.txt", {}) == FileSystemError::kNoMount);
}

REX_TEST_CASE_METHOD(Fixture, "Links and mounts match whole path components",
                     "[filesystem][links]") {
  WriteHost(GetTemp() / "a.txt", "alpha");
  MountTemp("\\Device\\Cdrom0");
  REQUIRE(GetFs().Link("cd:", "\\Device\\Cdrom0"));

  CHECK(ReadText(*Open("\\Device\\Cdrom0\\a.txt").file) == "alpha");
  CHECK(ReadText(*Open("cd:\\a.txt").file) == "alpha");
  CHECK(OpenError(*this, "\\Device\\Cdrom01\\a.txt", {}) == FileSystemError::kNoMount);
  CHECK(GetFs().FindLink("cdx:\\a.txt") == std::nullopt);

  auto again = GetFs().Link("CD:", "\\Device\\Other");
  REQUIRE_FALSE(again);
  CHECK(CodeOf(again.error()) == FileSystemError::kExists);
}

REX_TEST_CASE_METHOD(Fixture, "A link that never ends is kInvalidPath", "[filesystem][links]") {
  MountTemp(kRoot);
  REQUIRE(GetFs().Link("loop:", "loop:\\again"));
  CHECK(OpenError(*this, "loop:\\a.txt", {}) == FileSystemError::kInvalidPath);
}

REX_TEST_CASE_METHOD(Fixture, "The first registered mount wins", "[filesystem][mounts]") {
  WriteHost(GetTemp() / "outer" / "B" / "x.txt", "from outer");
  WriteHost(GetTemp() / "inner" / "x.txt", "from inner");
  REQUIRE(GetFs().Mount({"\\Device\\A", ProviderKind::kHostDirectory, GetTemp() / "outer"}));
  REQUIRE(GetFs().Mount({"\\Device\\A\\B", ProviderKind::kHostDirectory, GetTemp() / "inner"}));
  CHECK(ReadText(*Open("\\Device\\A\\B\\x.txt").file) == "from outer");

  auto duplicate =
      GetFs().Mount({"\\Device\\A", ProviderKind::kHostDirectory, GetTemp() / "inner"});
  REQUIRE_FALSE(duplicate);
  CHECK(CodeOf(duplicate.error()) == FileSystemError::kMountExists);

  REQUIRE(GetFs().GetMounts().size() == 2);
  CHECK(GetFs().GetMounts()[0].path == "\\Device\\A");

  REQUIRE(GetFs().Unmount("\\Device\\A"));
  CHECK(ReadText(*Open("\\Device\\A\\B\\x.txt").file) == "from inner");
  auto gone = GetFs().Unmount("\\Device\\A");
  REQUIRE_FALSE(gone);
  CHECK(CodeOf(gone.error()) == FileSystemError::kNoMount);
}

REX_TEST_CASE_METHOD(Fixture, "Dispositions against present and absent files",
                     "[filesystem][open]") {
  WriteHost(GetTemp() / "present.txt", "data");
  MountTemp(kRoot);
  const std::string present = "\\Device\\Test\\present.txt";
  const std::string absent = "\\Device\\Test\\absent.txt";
  auto reset = [&] {
    WriteHost(GetTemp() / "present.txt", "data");
    std::filesystem::remove(GetTemp() / "absent.txt");
  };
  auto open = [&](const std::string& path, Disposition disposition) {
    return GetFs().Open(path, {.disposition = disposition, .access = Access::kGenericAll});
  };

  struct Row {
    Disposition disposition;
    bool present_ok;
    OpenAction present_action;
    bool absent_ok;
    FileSystemError absent_error;
  };
  const Row rows[] = {
      {Disposition::kSupersede, true, OpenAction::kSuperseded, true, FileSystemError::kNotFound},
      {Disposition::kOpen, true, OpenAction::kOpened, false, FileSystemError::kNotFound},
      {Disposition::kCreate, false, OpenAction::kOpened, true, FileSystemError::kNotFound},
      {Disposition::kOpenIf, true, OpenAction::kOpened, true, FileSystemError::kNotFound},
      {Disposition::kOverwrite, true, OpenAction::kOverwritten, false, FileSystemError::kNotFound},
      {Disposition::kOverwriteIf, true, OpenAction::kOverwritten, true, FileSystemError::kNotFound},
  };
  for (const Row& row : rows) {
    INFO(static_cast<int>(row.disposition));
    reset();
    auto on_present = open(present, row.disposition);
    if (row.present_ok) {
      REQUIRE(on_present);
      CHECK(on_present->action == row.present_action);
      const bool kept = row.present_action == OpenAction::kOpened;
      CHECK(std::filesystem::file_size(GetTemp() / "present.txt") == (kept ? 4u : 0u));
    } else {
      REQUIRE_FALSE(on_present);
      CHECK(CodeOf(on_present.error()) == FileSystemError::kExists);
    }
    on_present = {};

    auto on_absent = open(absent, row.disposition);
    if (row.absent_ok) {
      REQUIRE(on_absent);
      CHECK(on_absent->action == OpenAction::kCreated);
      CHECK(std::filesystem::exists(GetTemp() / "absent.txt"));
    } else {
      REQUIRE_FALSE(on_absent);
      CHECK(CodeOf(on_absent.error()) == row.absent_error);
      CHECK_FALSE(std::filesystem::exists(GetTemp() / "absent.txt"));
    }
  }
}

REX_TEST_CASE_METHOD(Fixture, "Open creates files and directories in present parents",
                     "[filesystem][open]") {
  MountTemp(kRoot);
  const OpenOptions create{.disposition = Disposition::kCreate, .access = Access::kGenericWrite};
  CHECK(OpenError(*this, "\\Device\\Test\\one\\two\\file.txt", create) ==
        FileSystemError::kNotFound);
  REQUIRE(GetFs().CreateDirectories("\\Device\\Test\\one\\two"));
  auto file = Open("\\Device\\Test\\one\\two\\file.txt", create);
  CHECK(file.action == OpenAction::kCreated);
  CHECK(std::filesystem::is_regular_file(GetTemp() / "one" / "two" / "file.txt"));

  auto folder =
      Open("\\Device\\Test\\made", {.disposition = Disposition::kCreate, .create_directory = true});
  CHECK(std::filesystem::is_directory(GetTemp() / "made"));
  CHECK(rex::HasFlags(folder.file->GetInfo().attributes,
                      rex::filesystem::FileAttributes::kDirectory));
}

REX_TEST_CASE_METHOD(Fixture, "require_file refuses a directory", "[filesystem][open]") {
  std::filesystem::create_directories(GetTemp() / "folder");
  MountTemp(kRoot);
  CHECK(OpenError(*this, "\\Device\\Test\\folder", {.require_file = true}) ==
        FileSystemError::kIsDirectory);
  CHECK(Open("\\Device\\Test\\folder").file->GetInfo().name == "folder");
}

REX_TEST_CASE_METHOD(Fixture, "A write on a read only mount opens for read", "[filesystem][open]") {
  WriteHost(GetTemp() / "a.txt", "alpha");
  REQUIRE(GetFs().Mount({std::string(kRoot), ProviderKind::kHostDirectory, GetTemp()}));
  auto opened = Open("\\Device\\Test\\a.txt", {.access = Access::kGenericAll});
  CHECK_FALSE(rex::HasFlags(opened.file->GetAccess(), Access::kWriteData));
  CHECK(ReadText(*opened.file) == "alpha");
  const std::array<rex::u8, 1> patch{'x'};
  CHECK_FALSE(opened.file->Write(patch, 0));
  CHECK(OpenError(*this, "\\Device\\Test\\new.txt",
                  {.disposition = Disposition::kCreate, .access = Access::kGenericWrite}) ==
        FileSystemError::kAccessDenied);
}

REX_TEST_CASE_METHOD(Fixture, "OpenRelative opens and creates below an open directory",
                     "[filesystem][open]") {
  WriteHost(GetTemp() / "folder" / "c.txt", "charlie");
  MountTemp(kRoot);
  auto root = Open("\\Device\\Test\\folder");
  auto child = GetFs().OpenRelative(*root.file, "c.txt", {});
  REQUIRE(child);
  CHECK(ReadText(*child->file) == "charlie");

  auto created = GetFs().OpenRelative(
      *root.file, "d.txt", {.disposition = Disposition::kCreate, .access = Access::kGenericWrite});
  REQUIRE(created);
  CHECK(std::filesystem::exists(GetTemp() / "folder" / "d.txt"));
}

REX_TEST_CASE_METHOD(Fixture, "Stat, List, CreateDirectories, Rename and Remove",
                     "[filesystem][paths]") {
  WriteHost(GetTemp() / "a.txt", "alpha");
  WriteHost(GetTemp() / "b.bin", "bravo");
  MountTemp(kRoot);

  auto info = GetFs().Stat("\\Device\\Test\\a.txt");
  REQUIRE(info);
  CHECK(info->size == 5);
  CHECK(info->name == "a.txt");
  auto missing = GetFs().Stat("\\Device\\Test\\none.txt");
  REQUIRE_FALSE(missing);
  CHECK(CodeOf(missing.error()) == FileSystemError::kNotFound);

  auto list = GetFs().List(kRoot, "*.txt");
  REQUIRE(list);
  std::vector<std::string> names;
  while (auto entry = (*list)->Next()) {
    names.push_back(entry->name);
  }
  CHECK(names == std::vector<std::string>{"a.txt"});
  auto not_directory = GetFs().List("\\Device\\Test\\a.txt", "");
  REQUIRE_FALSE(not_directory);
  CHECK(CodeOf(not_directory.error()) == FileSystemError::kNotDirectory);

  REQUIRE(GetFs().CreateDirectories("\\Device\\Test\\x\\y"));
  REQUIRE(GetFs().CreateDirectories("\\Device\\Test\\x\\y"));
  CHECK(std::filesystem::is_directory(GetTemp() / "x" / "y"));

  REQUIRE(GetFs().Rename("\\Device\\Test\\b.bin", "\\Device\\Test\\x\\moved.bin", false));
  CHECK(std::filesystem::exists(GetTemp() / "x" / "moved.bin"));
  CHECK_FALSE(std::filesystem::exists(GetTemp() / "b.bin"));

  REQUIRE(GetFs().Remove("\\Device\\Test\\x"));
  CHECK_FALSE(std::filesystem::exists(GetTemp() / "x"));
  auto root = GetFs().Remove(kRoot);
  REQUIRE_FALSE(root);
  CHECK(CodeOf(root.error()) == FileSystemError::kAccessDenied);
}

REX_TEST_CASE("Standard mounts serve game, d, update and the null paths", "[filesystem][mounts]") {
  Fixture fixture;
  WriteHost(fixture.GetTemp() / "game" / "default.xex", "xex");
  WriteHost(fixture.GetTemp() / "update" / "patch.bin", "patch");
  rex::PathsConfig paths;
  paths.game_data_root = fixture.GetTemp() / "game";
  paths.update_data_root = fixture.GetTemp() / "update";
  REQUIRE(fixture.GetFs().ConfigureStandardMounts(paths));

  CHECK(ReadText(*fixture.Open("game:\\default.xex").file) == "xex");
  CHECK(ReadText(*fixture.Open("d:\\default.xex").file) == "xex");
  CHECK(ReadText(*fixture.Open("update:\\patch.bin").file) == "patch");
  for (std::string_view path : {"\\Device\\Harddisk0\\Partition0", "\\Device\\Harddisk0\\Cache0",
                                "\\Device\\Harddisk0\\Cache1"}) {
    INFO(path);
    CHECK(fixture.Open(path).file->GetInfo().size == 0);
  }
  CHECK(fixture.GetFs().GetMounts().size() == 3);
  CHECK(OpenError(fixture, "game:\\new.txt",
                  {.disposition = Disposition::kCreate, .access = Access::kGenericWrite}) ==
        FileSystemError::kAccessDenied);

  paths.update_data_root.clear();
  REQUIRE(fixture.GetFs().ConfigureStandardMounts(paths));
  CHECK(fixture.GetFs().GetMounts().size() == 2);
  CHECK(fixture.GetFs().FindLink("update:\\patch.bin") == std::nullopt);

  paths.game_data_root = fixture.GetTemp() / "missing";
  auto missing = fixture.GetFs().ConfigureStandardMounts(paths);
  REQUIRE_FALSE(missing);
  CHECK(CodeOf(missing.error()) == FileSystemError::kNotFound);
}

REX_TEST_CASE("game_mount_flags makes the game partition writable", "[filesystem][mounts]") {
  Fixture fixture(MountFlags::kWritable);
  std::filesystem::create_directories(fixture.GetTemp() / "game");
  rex::PathsConfig paths;
  paths.game_data_root = fixture.GetTemp() / "game";
  REQUIRE(fixture.GetFs().ConfigureStandardMounts(paths));
  auto created = fixture.Open(
      "game:\\new.txt", {.disposition = Disposition::kCreate, .access = Access::kGenericWrite});
  CHECK(created.action == OpenAction::kCreated);
  CHECK(std::filesystem::exists(fixture.GetTemp() / "game" / "new.txt"));
}
