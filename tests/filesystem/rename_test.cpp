/**
 * @file        tests/filesystem/rename_test.cpp
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
  auto list = fixture.fs().List(path, "");
  REQUIRE(list);
  std::vector<std::string> names;
  while (auto entry = (*list)->Next()) {
    names.push_back(entry->name);
  }
  std::ranges::sort(names);
  return names;
}

std::string HostText(const std::filesystem::path& path) {
  auto bytes = rex::platform::ReadWholeFile(path);
  REQUIRE(bytes);
  return {bytes->begin(), bytes->end()};
}

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "Rename moves a file into another folder of the tree",
                     "[filesystem][rename]") {
  WriteHost(temp() / "from" / "a.txt", "alpha");
  WriteHost(temp() / "to" / "keep.txt", "keep");
  MountTemp(kRoot);

  REQUIRE(fs().Rename("\\Device\\Test\\from\\a.txt", "\\Device\\Test\\to\\b.txt", false));
  CHECK(Names(*this, "\\Device\\Test\\from").empty());
  CHECK(Names(*this, "\\Device\\Test\\to") == std::vector<std::string>{"b.txt", "keep.txt"});
  CHECK_FALSE(fs().Stat("\\Device\\Test\\from\\a.txt"));
  auto info = fs().Stat("\\Device\\Test\\to\\b.txt");
  REQUIRE(info);
  CHECK(info->path == "to\\b.txt");
  CHECK(ReadText(*Open("\\Device\\Test\\to\\b.txt").file) == "alpha");
  CHECK(HostText(temp() / "to" / "b.txt") == "alpha");
}

REX_TEST_CASE_METHOD(Fixture, "Rename of a folder waits for the files below it to close",
                     "[filesystem][rename]") {
  WriteHost(temp() / "old" / "sub" / "c.txt", "charlie");
  MountTemp(kRoot);

  auto child = Open("\\Device\\Test\\old\\sub\\c.txt");
  auto refused = fs().Rename("\\Device\\Test\\old", "\\Device\\Test\\new", false);
  REQUIRE_FALSE(refused);
  CHECK(CodeOf(refused.error()) == FileSystemError::kAccessDenied);
  CHECK(std::filesystem::exists(temp() / "old" / "sub" / "c.txt"));
  child.file.reset();

  REQUIRE(fs().Rename("\\Device\\Test\\old", "\\Device\\Test\\new", false));
  CHECK(HostText(temp() / "new" / "sub" / "c.txt") == "charlie");
  CHECK_FALSE(fs().Stat("\\Device\\Test\\old\\sub\\c.txt"));
  auto moved = Open("\\Device\\Test\\new\\sub\\c.txt");
  CHECK(moved.file->Info().absolute_path == "\\Device\\Test\\new\\sub\\c.txt");
  CHECK(ReadText(*moved.file) == "charlie");
}

REX_TEST_CASE_METHOD(Fixture, "Rename onto an existing name without replace is kExists",
                     "[filesystem][rename]") {
  WriteHost(temp() / "a.txt", "alpha");
  WriteHost(temp() / "b.txt", "bravo");
  MountTemp(kRoot);

  auto renamed = fs().Rename("\\Device\\Test\\a.txt", "\\Device\\Test\\b.txt", false);
  REQUIRE_FALSE(renamed);
  CHECK(CodeOf(renamed.error()) == FileSystemError::kExists);
  CHECK(HostText(temp() / "a.txt") == "alpha");
  CHECK(HostText(temp() / "b.txt") == "bravo");
}

REX_TEST_CASE_METHOD(Fixture, "Rename with replace overwrites a closed file",
                     "[filesystem][rename]") {
  WriteHost(temp() / "a.txt", "alpha");
  WriteHost(temp() / "b.txt", "bravo");
  MountTemp(kRoot);

  REQUIRE(fs().Rename("\\Device\\Test\\a.txt", "\\Device\\Test\\b.txt", true));
  CHECK_FALSE(std::filesystem::exists(temp() / "a.txt"));
  CHECK(HostText(temp() / "b.txt") == "alpha");
  CHECK(Names(*this, kRoot) == std::vector<std::string>{"b.txt"});
}

REX_TEST_CASE_METHOD(Fixture, "Rename refuses to replace an open file or a folder",
                     "[filesystem][rename]") {
  WriteHost(temp() / "a.txt", "alpha");
  WriteHost(temp() / "b.txt", "bravo");
  WriteHost(temp() / "dir" / "x.txt", "x");
  MountTemp(kRoot);

  auto held = Open("\\Device\\Test\\b.txt");
  auto open_target = fs().Rename("\\Device\\Test\\a.txt", "\\Device\\Test\\b.txt", true);
  REQUIRE_FALSE(open_target);
  CHECK(CodeOf(open_target.error()) == FileSystemError::kAccessDenied);
  held.file.reset();
  auto folder_target = fs().Rename("\\Device\\Test\\a.txt", "\\Device\\Test\\dir", true);
  REQUIRE_FALSE(folder_target);
  CHECK(CodeOf(folder_target.error()) == FileSystemError::kAccessDenied);
  CHECK(HostText(temp() / "a.txt") == "alpha");
  CHECK(HostText(temp() / "b.txt") == "bravo");
}

REX_TEST_CASE_METHOD(Fixture, "Rename to another mount is kNotSameDevice", "[filesystem][rename]") {
  WriteHost(temp() / "one" / "a.txt", "alpha");
  std::filesystem::create_directories(temp() / "two");
  REQUIRE(fs().Mount(
      {"\\Device\\One", ProviderKind::kHostDirectory, temp() / "one", MountFlags::kWritable}));
  REQUIRE(fs().Mount(
      {"\\Device\\Two", ProviderKind::kHostDirectory, temp() / "two", MountFlags::kWritable}));

  auto renamed = fs().Rename("\\Device\\One\\a.txt", "\\Device\\Two\\a.txt", false);
  REQUIRE_FALSE(renamed);
  CHECK(CodeOf(renamed.error()) == FileSystemError::kNotSameDevice);
  CHECK(std::filesystem::exists(temp() / "one" / "a.txt"));
}

REX_TEST_CASE_METHOD(Fixture, "Rename into its own subfolder is kInvalidPath",
                     "[filesystem][rename]") {
  WriteHost(temp() / "a" / "b" / "c.txt", "charlie");
  MountTemp(kRoot);

  auto renamed = fs().Rename("\\Device\\Test\\a", "\\Device\\Test\\a\\b\\a", false);
  REQUIRE_FALSE(renamed);
  CHECK(CodeOf(renamed.error()) == FileSystemError::kInvalidPath);
  CHECK(std::filesystem::exists(temp() / "a" / "b" / "c.txt"));
}

REX_TEST_CASE_METHOD(Fixture, "Rename through a link resolves the link", "[filesystem][rename]") {
  WriteHost(temp() / "sub" / "a.txt", "alpha");
  MountTemp(kRoot);
  REQUIRE(fs().Link("test:", kRoot));

  REQUIRE(fs().Rename("test:\\sub\\a.txt", "test:\\sub\\b.txt", false));
  CHECK(HostText(temp() / "sub" / "b.txt") == "alpha");
}

REX_TEST_CASE_METHOD(Fixture, "An open file renamed to a bare name stays in its folder",
                     "[filesystem][rename]") {
  WriteHost(temp() / "sub" / "a.txt", "alpha");
  MountTemp(kRoot);

  auto opened = Open("\\Device\\Test\\sub\\a.txt", {.access = Access::kGenericAll});
  REQUIRE(fs().Rename(*opened.file, "b.txt", false));
  CHECK(opened.file->Info().path == "sub\\b.txt");
  opened.file.reset();
  CHECK(HostText(temp() / "sub" / "b.txt") == "alpha");
}

REX_TEST_CASE_METHOD(Fixture, "An open file renamed below a root directory moves there",
                     "[filesystem][rename]") {
  WriteHost(temp() / "a.txt", "alpha");
  std::filesystem::create_directories(temp() / "dest");
  MountTemp(kRoot);

  auto file = Open("\\Device\\Test\\a.txt", {.access = Access::kGenericAll});
  auto root = Open("\\Device\\Test\\dest");
  REQUIRE(fs().Rename(*file.file, "b.txt", false, root.file.get()));
  CHECK(file.file->Info().path == "dest\\b.txt");
  file.file.reset();
  CHECK(HostText(temp() / "dest" / "b.txt") == "alpha");
}
