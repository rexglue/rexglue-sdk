/**
 * @file        tests/unit/xam/content_package_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "rex_test.h"

#include <rex/filesystem/file_system_error.h>
#include <rex/filesystem/file_system_factory.h>
#include <rex/filesystem/stfs.h>
#include <rex/module.h>
#include <rex/kernel/title_runtime.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/user_profile.h>

#include "fixture_dir.h"
#include "kernel/kernel_fixture.h"
#include "xam/xam_state.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

constexpr rex::u32 kPackageTitleId = 0x52455831;

constexpr std::streamoff kContentIdOffset = 0x32C;

std::filesystem::path CopyPackage(const KernelFixture& fixture, const std::string& name) {
  const std::filesystem::path copy = fixture.GetRoot() / name;
  std::filesystem::copy_file(rex::test::GetFixtureFolder() / "stfs_con.bin", copy,
                             std::filesystem::copy_options::overwrite_existing);
  return copy;
}

size_t CountEntries(const std::filesystem::path& root) {
  size_t count = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    (void)entry;
    ++count;
  }
  return count;
}

bool HasFileNamed(const std::filesystem::path& root, std::string_view name) {
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (entry.path().filename() == name) {
      return true;
    }
  }
  return false;
}

constexpr rex::xam::InstallOptions kAnyTitle{.require_running_title = false};
constexpr rex::xam::InstallOptions kAnyTitleInPlace{.require_running_title = false,
                                                    .extract = false};

bool IsListed(rex::xam::IContentManager& manager,
              const rex::kernel::XCONTENT_AGGREGATE_DATA& data) {
  for (const auto& listed : manager.ListContent(1, 0, data.content_type, data.title_id)) {
    if (listed.GetFileName() == data.GetFileName()) {
      return true;
    }
  }
  return false;
}

}  // namespace

REX_TEST_CASE("InspectPackage reads the header and writes nothing", "[xam][content]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&](Caller& c) {
    auto& manager = c.GetRuntime().GetContentManager();
    const std::filesystem::path package = CopyPackage(fixture, "package.bin");
    std::array<rex::u8, 20> content_id{};
    for (size_t i = 0; i < content_id.size(); ++i) {
      content_id[i] = static_cast<rex::u8>(0xA0 + i);
    }
    {
      std::fstream stream(package, std::ios::in | std::ios::out | std::ios::binary);
      REQUIRE(stream.is_open());
      stream.seekp(kContentIdOffset);
      stream.write(reinterpret_cast<const char*>(content_id.data()),
                   static_cast<std::streamsize>(content_id.size()));
    }
    const size_t before = CountEntries(fixture.GetRoot());
    auto info = manager.InspectPackage(package);
    REQUIRE(info);
    CHECK(info->title_id == kPackageTitleId);
    CHECK(info->content_id == content_id);
    auto* files = REX_INTERFACE(rex::filesystem::IFileSystemFactory);
    REQUIRE(files);
    auto header = files->ReadPackageHeader(package);
    REQUIRE(header);
    CHECK(std::equal(info->content_id.begin(), info->content_id.end(),
                     std::begin(header->header.content_id)));
    CHECK(CountEntries(fixture.GetRoot()) == before);
    CHECK(manager.ListInstalledPackages(kPackageTitleId).empty());

    auto disc = manager.InspectPackage(rex::test::GetFixtureFolder() / "gdfx.iso");
    REQUIRE_FALSE(disc);
    CHECK(rex::filesystem::CodeOf(disc.error()) == rex::filesystem::FileSystemError::kCorrupt);
    CHECK_FALSE(manager.InspectPackage(rex::test::GetFixtureFolder() / "missing.bin"));
  });
}

REX_TEST_CASE("InstallContent refuses a package of another title unless told otherwise",
              "[xam][content]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&](Caller& c) {
    auto& manager = c.GetRuntime().GetContentManager();
    const std::filesystem::path package = CopyPackage(fixture, "package.bin");
    auto refused = manager.InstallContent(package);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().category == rex::ErrorCategory::kValidation);
    CHECK(manager.ListInstalledPackages(kPackageTitleId).empty());

    REQUIRE(manager.InstallContent(package, kAnyTitle));
    CHECK(manager.ListInstalledPackages(kPackageTitleId).size() == 1);
  });
}

REX_TEST_CASE("A disabled package looks like one that is not installed", "[xam][content]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&](Caller& c) {
    auto& manager = c.GetRuntime().GetContentManager();
    auto installed = manager.InstallContent(CopyPackage(fixture, "package.bin"), kAnyTitle);
    REQUIRE(installed);
    const rex::kernel::XCONTENT_AGGREGATE_DATA data = *installed;
    const auto info_content_id =
        manager.InspectPackage(rex::test::GetFixtureFolder() / "stfs_con.bin")->content_id;
    CHECK(IsListed(manager, data));
    CHECK(manager.HasContent(0, data));

    REQUIRE(manager.SetContentEnabled(data, false));
    CHECK_FALSE(IsListed(manager, data));
    CHECK_FALSE(manager.HasContent(0, data));
    CHECK_FALSE(manager.OpenContent("dlc", 0, data));
    const auto packages = manager.ListInstalledPackages(kPackageTitleId);
    REQUIRE(packages.size() == 1);
    CHECK(packages[0].info.content_id == info_content_id);
    CHECK_FALSE(packages[0].enabled);
    CHECK_FALSE(packages[0].in_place);

    REQUIRE(manager.SetContentEnabled(data, true));
    CHECK(IsListed(manager, data));
    CHECK(manager.HasContent(0, data));
    REQUIRE(manager.OpenContent("dlc", 0, data));
    REQUIRE(manager.CloseContent("dlc"));
    CHECK(manager.ListInstalledPackages(kPackageTitleId)[0].enabled);
  });
}

REX_TEST_CASE("A profile flag wins over the all profiles flag", "[xam][content]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&](Caller& c) {
    auto& manager = c.GetRuntime().GetContentManager();
    const u64 profile = rex::xam::CurrentXAM().GetUserProfile().GetXUID();
    auto installed = manager.InstallContent(CopyPackage(fixture, "package.bin"), kAnyTitle);
    REQUIRE(installed);
    const rex::kernel::XCONTENT_AGGREGATE_DATA data = *installed;

    REQUIRE(manager.SetContentEnabled(data, false));
    REQUIRE(manager.SetContentEnabled(data, true, profile));
    CHECK(manager.HasContent(0, data));

    REQUIRE(manager.SetContentEnabled(data, true));
    REQUIRE(manager.SetContentEnabled(data, false, profile));
    CHECK_FALSE(manager.HasContent(0, data));

    REQUIRE(manager.SetContentEnabled(data, false, profile + 1));
    REQUIRE(manager.SetContentEnabled(data, true, profile));
    CHECK(manager.HasContent(0, data));
  });
}

REX_TEST_CASE("An in place package opens and reads without extracting", "[xam][content]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&](Caller& c) {
    auto& manager = c.GetRuntime().GetContentManager();
    auto installed = manager.InstallContent(CopyPackage(fixture, "inplace.bin"), kAnyTitleInPlace);
    REQUIRE(installed);
    const auto packages = manager.ListInstalledPackages(kPackageTitleId);
    REQUIRE(packages.size() == 1);
    CHECK(packages[0].in_place);
    CHECK_FALSE(HasFileNamed(fixture.GetRoot(), "a.txt"));

    REQUIRE(manager.OpenContent("dlc", 0, *installed));
    auto stat = c.GetRuntime().GetFileSystem().Stat("dlc:\\a.txt");
    REQUIRE(stat);
    CHECK(stat->size > 0);
    auto opened = c.GetRuntime().GetFileSystem().Open("dlc:\\a.txt", {});
    REQUIRE(opened);
    std::vector<rex::u8> bytes(static_cast<size_t>(stat->size));
    auto read = opened->file->Read(bytes, 0);
    REQUIRE(read);
    CHECK(*read == bytes.size());
    opened->file.reset();
    REQUIRE(manager.CloseContent("dlc"));
    CHECK_FALSE(HasFileNamed(fixture.GetRoot(), "a.txt"));
  });
}

REX_TEST_CASE("Deleting an in place package leaves the original file", "[xam][content]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&](Caller& c) {
    auto& manager = c.GetRuntime().GetContentManager();
    const std::filesystem::path source = CopyPackage(fixture, "inplace.bin");
    auto installed = manager.InstallContent(source, kAnyTitleInPlace);
    REQUIRE(installed);
    REQUIRE(manager.SetContentEnabled(*installed, false));

    REQUIRE(manager.DeleteContent(0, *installed));
    CHECK(std::filesystem::exists(source));
    CHECK(manager.ListInstalledPackages(kPackageTitleId).empty());
    CHECK_FALSE(manager.HasContent(0, *installed));

    auto again = manager.InstallContent(source, kAnyTitleInPlace);
    REQUIRE(again);
    CHECK(manager.ListInstalledPackages(kPackageTitleId)[0].enabled);
  });
}

REX_TEST_CASE("OnContentChanged runs once per change", "[xam][content]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&](Caller& c) {
    auto& manager = c.GetRuntime().GetContentManager();
    int changes = 0;
    {
      auto guard = manager.OnContentChanged([&changes] { ++changes; });
      auto installed = manager.InstallContent(CopyPackage(fixture, "package.bin"), kAnyTitle);
      REQUIRE(installed);
      CHECK(changes == 1);
      REQUIRE(manager.SetContentEnabled(*installed, false));
      CHECK(changes == 2);
      REQUIRE(manager.SetContentEnabled(*installed, false));
      CHECK(changes == 2);
      REQUIRE(manager.DeleteContent(0, *installed));
      CHECK(changes == 3);
    }
    REQUIRE(manager.InstallContent(CopyPackage(fixture, "package.bin"), kAnyTitle));
    CHECK(changes == 3);
  });
}
