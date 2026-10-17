/**
 * @file        tests/unit/filesystem/host_file_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <atomic>
#include <cstdio>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "rex_test.h"

#if defined(_WIN32)
#include <share.h>
#endif

#include "filesystem/fixture.h"
#include "filesystem/host_file.h"

namespace {

using rex::u8;
using rex::filesystem::Access;
using rex::filesystem::HostFile;
using rex::filesystem::HostInfo;
using rex::filesystem::HostList;
using rex::filesystem::HostStat;
using rex::filesystem::test::TempFolder;
using rex::filesystem::test::WriteHost;

constexpr std::string_view kPayload = "rexglue";

std::filesystem::path WritePayload(const TempFolder& temp) {
  const auto path = temp.path / "payload.bin";
  WriteHost(path, kPayload);
  return path;
}

}  // namespace

REX_TEST_CASE("OpenExisting read only handle reads", "[filesystem][host]") {
  TempFolder temp;
  auto handle = HostFile::OpenExisting(WritePayload(temp), Access::kGenericRead);
  REQUIRE(handle);

  std::array<u8, 16> buffer{};
  auto read = (*handle)->Read(0, buffer);
  REQUIRE(read);
  CHECK(*read == kPayload.size());
  CHECK(std::string_view(reinterpret_cast<const char*>(buffer.data()), *read) == kPayload);
}

REX_TEST_CASE("OpenExisting read and write handle keeps read access", "[filesystem][host]") {
  TempFolder temp;
  auto handle =
      HostFile::OpenExisting(WritePayload(temp), Access::kGenericRead | Access::kGenericWrite |
                                                     Access::kReadData | Access::kWriteData);
  REQUIRE(handle);

  std::array<u8, 16> buffer{};
  auto read = (*handle)->Read(0, buffer);
  REQUIRE(read);
  CHECK(*read == kPayload.size());

  const std::array<u8, 1> patch{'X'};
  auto written = (*handle)->Write(0, patch);
  REQUIRE(written);
  CHECK(*written == 1);

  read = (*handle)->Read(0, std::span(buffer).first(1));
  REQUIRE(read);
  CHECK(buffer[0] == patch[0]);
}

REX_TEST_CASE("OpenExisting kGenericAll grants both directions", "[filesystem][host]") {
  TempFolder temp;
  auto handle = HostFile::OpenExisting(WritePayload(temp), Access::kGenericAll);
  REQUIRE(handle);

  const std::array<u8, 1> patch{'Z'};
  auto written = (*handle)->Write(0, patch);
  REQUIRE(written);
  CHECK(*written == 1);

  std::array<u8, 1> buffer{};
  auto read = (*handle)->Read(0, buffer);
  REQUIRE(read);
  CHECK(buffer[0] == patch[0]);
}

REX_TEST_CASE("OpenExisting write only handle writes at the requested offset",
              "[filesystem][host]") {
  TempFolder temp;
  const auto path = WritePayload(temp);
  auto handle = HostFile::OpenExisting(path, Access::kGenericWrite | Access::kAppendData);
  REQUIRE(handle);

  const std::array<u8, 1> patch{'Q'};
  auto written = (*handle)->Write(1, patch);
  REQUIRE(written);
  CHECK(*written == 1);
  REQUIRE((*handle)->Flush());
  handle->reset();

  auto reader = HostFile::OpenExisting(path, Access::kGenericRead);
  REQUIRE(reader);
  std::array<u8, 16> buffer{};
  auto read = (*reader)->Read(0, buffer);
  REQUIRE(read);
  CHECK(*read == kPayload.size());
  CHECK(buffer[1] == patch[0]);
}

REX_TEST_CASE("OpenExisting write only handle refuses reads", "[filesystem][host]") {
  TempFolder temp;
  auto handle = HostFile::OpenExisting(WritePayload(temp), Access::kGenericWrite);
  REQUIRE(handle);

  std::array<u8, 16> buffer{};
  CHECK_FALSE((*handle)->Read(0, buffer));
}

REX_TEST_CASE("A read at or past the end returns 0 bytes", "[filesystem][host]") {
  TempFolder temp;
  auto handle = HostFile::OpenExisting(WritePayload(temp), Access::kGenericRead);
  REQUIRE(handle);

  std::array<u8, 16> buffer{};
  auto at_end = (*handle)->Read(kPayload.size(), buffer);
  REQUIRE(at_end);
  CHECK(*at_end == 0);
  auto past_end = (*handle)->Read(kPayload.size() + 100, buffer);
  REQUIRE(past_end);
  CHECK(*past_end == 0);
}

REX_TEST_CASE("SetLength is exact while another thread reads", "[filesystem][host]") {
  TempFolder temp;
  auto handle =
      HostFile::OpenExisting(WritePayload(temp), Access::kGenericRead | Access::kGenericWrite |
                                                     Access::kReadData | Access::kWriteData);
  REQUIRE(handle);
  HostFile& file = **handle;

  std::atomic<bool> stop = false;
  std::atomic<bool> read_failed = false;
  std::jthread reader([&] {
    std::array<u8, 1> byte{};
    while (!stop.load(std::memory_order_relaxed)) {
      if (!file.Read(0, byte)) {
        read_failed = true;
      }
    }
  });
  bool exact = true;
  for (u64 i = 0; i < 2000 && exact; ++i) {
    const u64 length = (i % 2 == 0) ? 4096 : 8192;
    REQUIRE(file.SetLength(length));
    std::array<u8, 1> byte{};
    auto last = file.Read(length - 1, byte);
    auto after = file.Read(length, byte);
    exact = last && *last == 1 && after && *after == 0;
  }
  stop = true;
  CHECK(exact);
  CHECK_FALSE(read_failed);
}

REX_TEST_CASE("HostList reports a link to a folder as a folder", "[filesystem][host]") {
  TempFolder temp;
  std::filesystem::create_directories(temp.path / "real");
  std::error_code ec;
  std::filesystem::create_directory_symlink(temp.path / "real", temp.path / "link", ec);
  if (ec) {
    SKIP("the host refuses folder links, " << ec.message());
  }

  const std::vector<HostInfo> children = HostList(temp.path);
  REQUIRE(children.size() == 2);
  for (const HostInfo& child : children) {
    CHECK(child.directory);
    CHECK(child.size == 0);
  }
}

REX_TEST_CASE("HostStat names the entry as the folder holds it", "[filesystem][host][case]") {
  TempFolder temp;
  std::filesystem::create_directories(temp.path / "Save1");
  const std::optional<HostInfo> info = HostStat(temp.path / "SAVE1");
  if (!info) {
    SKIP("the host tells names apart by case");
  }
  CHECK(info->name == "Save1");
  std::filesystem::create_directories(temp.path / "Save2");
  CHECK_FALSE(HostStat(temp.path / "Sav*"));
  CHECK_FALSE(HostStat(temp.path / "Save?"));
}

#if defined(_WIN32)
REX_TEST_CASE("OpenExisting of a file another handle locks is kSharingViolation",
              "[filesystem][host]") {
  TempFolder temp;
  const auto path = WritePayload(temp);
  FILE* locked = _wfsopen(path.c_str(), L"r+b", _SH_DENYRW);
  REQUIRE(locked);
  auto handle = HostFile::OpenExisting(path, Access::kGenericRead);
  std::fclose(locked);
  REQUIRE_FALSE(handle);
  CHECK(rex::filesystem::CodeOf(handle.error()) ==
        rex::filesystem::FileSystemError::kSharingViolation);
}
#endif

REX_TEST_CASE("OpenExisting of a missing file is kNotFound", "[filesystem][host]") {
  TempFolder temp;
  auto handle = HostFile::OpenExisting(temp.path / "missing.bin", Access::kGenericRead);
  REQUIRE_FALSE(handle);
  CHECK(rex::filesystem::CodeOf(handle.error()) == rex::filesystem::FileSystemError::kNotFound);
}
