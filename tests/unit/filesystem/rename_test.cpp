/**
 * @file        tests/unit/filesystem/rename_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "rex_test.h"

#include "filesystem/fixture.h"

namespace {

using rex::filesystem::Access;
using rex::filesystem::CodeOf;
using rex::filesystem::FileSystemError;
using rex::filesystem::MountFlags;
using rex::filesystem::ProviderKind;
using rex::filesystem::test::Fixture;
using rex::filesystem::test::ReadText;
using rex::filesystem::test::WriteHost;

constexpr std::string_view kRoot = "\\Device\\Test";

std::vector<std::string> Names(Fixture& fixture, std::string_view path) {
  auto list = fixture.GetFs().List(path, "");
  REQUIRE(list);
  std::vector<std::string> names;
  while (auto entry = (*list)->Next()) {
    names.push_back(entry->name);
  }
  std::ranges::sort(names);
  return names;
}

std::string HostText(const std::filesystem::path& path) {
  auto bytes = rex::platform::ReadHostFile(path);
  REQUIRE(bytes);
  return {bytes->begin(), bytes->end()};
}

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "Rename moves a file into another folder of the tree",
                     "[filesystem][rename]") {
  WriteHost(GetTemp() / "from" / "a.txt", "alpha");
  WriteHost(GetTemp() / "to" / "keep.txt", "keep");
  MountTemp(kRoot);

  REQUIRE(GetFs().Rename("\\Device\\Test\\from\\a.txt", "\\Device\\Test\\to\\b.txt", false));
  CHECK(Names(*this, "\\Device\\Test\\from").empty());
  CHECK(Names(*this, "\\Device\\Test\\to") == std::vector<std::string>{"b.txt", "keep.txt"});
  CHECK_FALSE(GetFs().Stat("\\Device\\Test\\from\\a.txt"));
  auto info = GetFs().Stat("\\Device\\Test\\to\\b.txt");
  REQUIRE(info);
  CHECK(info->path == "to\\b.txt");
  CHECK(ReadText(*Open("\\Device\\Test\\to\\b.txt").file) == "alpha");
  CHECK(HostText(GetTemp() / "to" / "b.txt") == "alpha");
}

REX_TEST_CASE_METHOD(Fixture, "Rename of a folder waits for the files below it to close",
                     "[filesystem][rename]") {
  WriteHost(GetTemp() / "old" / "sub" / "c.txt", "charlie");
  MountTemp(kRoot);

  auto child = Open("\\Device\\Test\\old\\sub\\c.txt");
  auto refused = GetFs().Rename("\\Device\\Test\\old", "\\Device\\Test\\new", false);
  REQUIRE_FALSE(refused);
  CHECK(CodeOf(refused.error()) == FileSystemError::kAccessDenied);
  CHECK(std::filesystem::exists(GetTemp() / "old" / "sub" / "c.txt"));
  child.file.reset();

  REQUIRE(GetFs().Rename("\\Device\\Test\\old", "\\Device\\Test\\new", false));
  CHECK(HostText(GetTemp() / "new" / "sub" / "c.txt") == "charlie");
  CHECK_FALSE(GetFs().Stat("\\Device\\Test\\old\\sub\\c.txt"));
  auto moved = Open("\\Device\\Test\\new\\sub\\c.txt");
  CHECK(moved.file->GetInfo().absolute_path == "\\Device\\Test\\new\\sub\\c.txt");
  CHECK(ReadText(*moved.file) == "charlie");
}

REX_TEST_CASE_METHOD(Fixture, "Rename onto an existing name without replace is kExists",
                     "[filesystem][rename]") {
  WriteHost(GetTemp() / "a.txt", "alpha");
  WriteHost(GetTemp() / "b.txt", "bravo");
  MountTemp(kRoot);

  auto renamed = GetFs().Rename("\\Device\\Test\\a.txt", "\\Device\\Test\\b.txt", false);
  REQUIRE_FALSE(renamed);
  CHECK(CodeOf(renamed.error()) == FileSystemError::kExists);
  CHECK(HostText(GetTemp() / "a.txt") == "alpha");
  CHECK(HostText(GetTemp() / "b.txt") == "bravo");
}

REX_TEST_CASE_METHOD(Fixture, "Rename with replace overwrites a closed file",
                     "[filesystem][rename]") {
  WriteHost(GetTemp() / "a.txt", "alpha");
  WriteHost(GetTemp() / "b.txt", "bravo");
  MountTemp(kRoot);

  REQUIRE(GetFs().Rename("\\Device\\Test\\a.txt", "\\Device\\Test\\b.txt", true));
  CHECK_FALSE(std::filesystem::exists(GetTemp() / "a.txt"));
  CHECK(HostText(GetTemp() / "b.txt") == "alpha");
  CHECK(Names(*this, kRoot) == std::vector<std::string>{"b.txt"});
}

REX_TEST_CASE_METHOD(Fixture, "Rename refuses to replace an open file or a folder",
                     "[filesystem][rename]") {
  WriteHost(GetTemp() / "a.txt", "alpha");
  WriteHost(GetTemp() / "b.txt", "bravo");
  WriteHost(GetTemp() / "dir" / "x.txt", "x");
  MountTemp(kRoot);

  auto held = Open("\\Device\\Test\\b.txt");
  auto open_target = GetFs().Rename("\\Device\\Test\\a.txt", "\\Device\\Test\\b.txt", true);
  REQUIRE_FALSE(open_target);
  CHECK(CodeOf(open_target.error()) == FileSystemError::kAccessDenied);
  held.file.reset();
  auto folder_target = GetFs().Rename("\\Device\\Test\\a.txt", "\\Device\\Test\\dir", true);
  REQUIRE_FALSE(folder_target);
  CHECK(CodeOf(folder_target.error()) == FileSystemError::kAccessDenied);
  CHECK(HostText(GetTemp() / "a.txt") == "alpha");
  CHECK(HostText(GetTemp() / "b.txt") == "bravo");
}

REX_TEST_CASE_METHOD(Fixture, "Rename to another mount is kNotSameDevice", "[filesystem][rename]") {
  WriteHost(GetTemp() / "one" / "a.txt", "alpha");
  std::filesystem::create_directories(GetTemp() / "two");
  REQUIRE(GetFs().Mount(
      {"\\Device\\One", ProviderKind::kHostDirectory, GetTemp() / "one", MountFlags::kWritable}));
  REQUIRE(GetFs().Mount(
      {"\\Device\\Two", ProviderKind::kHostDirectory, GetTemp() / "two", MountFlags::kWritable}));

  auto renamed = GetFs().Rename("\\Device\\One\\a.txt", "\\Device\\Two\\a.txt", false);
  REQUIRE_FALSE(renamed);
  CHECK(CodeOf(renamed.error()) == FileSystemError::kNotSameDevice);
  CHECK(std::filesystem::exists(GetTemp() / "one" / "a.txt"));
}

REX_TEST_CASE_METHOD(Fixture, "Rename into its own subfolder is kInvalidPath",
                     "[filesystem][rename]") {
  WriteHost(GetTemp() / "a" / "b" / "c.txt", "charlie");
  MountTemp(kRoot);

  auto renamed = GetFs().Rename("\\Device\\Test\\a", "\\Device\\Test\\a\\b\\a", false);
  REQUIRE_FALSE(renamed);
  CHECK(CodeOf(renamed.error()) == FileSystemError::kInvalidPath);
  CHECK(std::filesystem::exists(GetTemp() / "a" / "b" / "c.txt"));
}

REX_TEST_CASE_METHOD(Fixture, "Rename through a link resolves the link", "[filesystem][rename]") {
  WriteHost(GetTemp() / "sub" / "a.txt", "alpha");
  MountTemp(kRoot);
  REQUIRE(GetFs().Link("test:", kRoot));

  REQUIRE(GetFs().Rename("test:\\sub\\a.txt", "test:\\sub\\b.txt", false));
  CHECK(HostText(GetTemp() / "sub" / "b.txt") == "alpha");
}

REX_TEST_CASE_METHOD(Fixture, "An open file renamed to a bare name stays in its folder",
                     "[filesystem][rename]") {
  WriteHost(GetTemp() / "sub" / "a.txt", "alpha");
  MountTemp(kRoot);

  auto opened = Open("\\Device\\Test\\sub\\a.txt", {.access = Access::kGenericAll});
  REQUIRE(GetFs().Rename(*opened.file, "b.txt", false));
  CHECK(opened.file->GetInfo().path == "sub\\b.txt");
  opened.file.reset();
  CHECK(HostText(GetTemp() / "sub" / "b.txt") == "alpha");
}

REX_TEST_CASE_METHOD(Fixture, "An open file renamed below a root directory moves there",
                     "[filesystem][rename]") {
  WriteHost(GetTemp() / "a.txt", "alpha");
  std::filesystem::create_directories(GetTemp() / "dest");
  MountTemp(kRoot);

  auto file = Open("\\Device\\Test\\a.txt", {.access = Access::kGenericAll});
  auto root = Open("\\Device\\Test\\dest");
  REQUIRE(GetFs().Rename(*file.file, "b.txt", false, root.file.get()));
  CHECK(file.file->GetInfo().path == "dest\\b.txt");
  file.file.reset();
  CHECK(HostText(GetTemp() / "dest" / "b.txt") == "alpha");
}
