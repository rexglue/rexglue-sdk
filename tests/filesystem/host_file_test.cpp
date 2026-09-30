/**
 * @file        tests/filesystem/host_file_test.cpp
 * @brief       Tests for the host file handle access modes
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "filesystem/fixture.h"
#include "filesystem/host_file.h"

namespace {

using rex::u8;
using rex::filesystem::Access;
using rex::filesystem::HostFile;
using rex::filesystem::test::TempFolder;
using rex::filesystem::test::WriteHost;

constexpr std::string_view kPayload = "rexglue";

std::filesystem::path WritePayload(const TempFolder& temp) {
  const auto path = temp.path / "payload.bin";
  WriteHost(path, kPayload);
  return path;
}

}  // namespace

TEST_CASE("OpenExisting read only handle reads", "[filesystem][host]") {
  TempFolder temp;
  auto handle = HostFile::OpenExisting(WritePayload(temp), Access::kGenericRead, false);
  REQUIRE(handle);

  std::array<u8, 16> buffer{};
  auto read = (*handle)->Read(0, buffer);
  REQUIRE(read);
  CHECK(*read == kPayload.size());
  CHECK(std::string_view(reinterpret_cast<const char*>(buffer.data()), *read) == kPayload);
}

TEST_CASE("OpenExisting read and write handle keeps read access", "[filesystem][host]") {
  TempFolder temp;
  auto handle = HostFile::OpenExisting(
      WritePayload(temp),
      Access::kGenericRead | Access::kGenericWrite | Access::kReadData | Access::kWriteData, false);
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

TEST_CASE("OpenExisting kGenericAll grants both directions", "[filesystem][host]") {
  TempFolder temp;
  auto handle = HostFile::OpenExisting(WritePayload(temp), Access::kGenericAll, false);
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

TEST_CASE("OpenExisting write only handle writes at the requested offset", "[filesystem][host]") {
  TempFolder temp;
  const auto path = WritePayload(temp);
  auto handle = HostFile::OpenExisting(path, Access::kGenericWrite | Access::kAppendData, false);
  REQUIRE(handle);

  const std::array<u8, 1> patch{'Q'};
  auto written = (*handle)->Write(1, patch);
  REQUIRE(written);
  CHECK(*written == 1);
  (*handle)->Flush();
  handle->reset();

  auto reader = HostFile::OpenExisting(path, Access::kGenericRead, false);
  REQUIRE(reader);
  std::array<u8, 16> buffer{};
  auto read = (*reader)->Read(0, buffer);
  REQUIRE(read);
  CHECK(*read == kPayload.size());
  CHECK(buffer[1] == patch[0]);
}

TEST_CASE("OpenExisting write only handle refuses reads", "[filesystem][host]") {
  TempFolder temp;
  auto handle = HostFile::OpenExisting(WritePayload(temp), Access::kGenericWrite, false);
  REQUIRE(handle);

  std::array<u8, 16> buffer{};
  CHECK_FALSE((*handle)->Read(0, buffer));
}

TEST_CASE("OpenExisting of a missing file is kNotFound", "[filesystem][host]") {
  TempFolder temp;
  auto handle = HostFile::OpenExisting(temp.path / "missing.bin", Access::kGenericRead, false);
  REQUIRE_FALSE(handle);
  CHECK(rex::filesystem::CodeOf(handle.error()) == rex::filesystem::Error::kNotFound);
}
