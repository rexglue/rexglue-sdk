/**
 * @file        tests/core/container_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "support/temp_folder.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <rex/bit.h>
#include <rex/filesystem/container.h>
#include <rex/filesystem/error.h>
#include <rex/filesystem/xcontent.h>
#include <rex/math.h>
#include <rex/platform/file.h>

namespace rex {

enum class ContainerTestFlags : u8 {
  kNone = 0,
  kA = 1,
  kB = 2,
  kC = 4,
};

template <>
// NOLINTNEXTLINE(readability-identifier-naming): the trait uses snake case
inline constexpr bool is_flags_enum<ContainerTestFlags> = true;

}  // namespace rex

namespace {

namespace fs = rex::filesystem;
using rex::ContainerTestFlags;

const std::filesystem::path kFixtures = REXGLUE_FIXTURE_DIR;
const std::string kAText = "alpha\n";
const std::string kCText = "charlie in folder\n";

std::vector<rex::u8> BigBin() {
  std::vector<rex::u8> bytes(0x1800);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<rex::u8>(i % 251);
  }
  return bytes;
}

std::vector<rex::u8> Bytes(std::string_view text) {
  return {text.begin(), text.end()};
}

struct TempFolder : rex::test::TempFolder {
  TempFolder() : rex::test::TempFolder("rex_container_test_") {}
};

std::vector<rex::u8> ReadNode(const fs::Container& container, const fs::ContainerNode& node) {
  std::vector<rex::u8> bytes(node.size);
  auto read = container.Read(node, 0, bytes);
  REQUIRE(read);
  REQUIRE(*read == node.size);
  return bytes;
}

void CheckTree(const fs::Container& container) {
  const fs::ContainerNode& root = container.root();
  REQUIRE(root.children.size() == 3);
  const fs::ContainerNode* a = container.Find("a.txt");
  const fs::ContainerNode* big = container.Find("big.bin");
  const fs::ContainerNode* folder = container.Find("folder");
  const fs::ContainerNode* c = container.Find("folder\\c.txt");
  REQUIRE(a);
  REQUIRE(big);
  REQUIRE(folder);
  REQUIRE(c);
  CHECK(container.Find("FOLDER/C.TXT") == c);
  CHECK(container.Find("") == &root);
  CHECK(container.Find("missing") == nullptr);
  CHECK(container.Find("a.txt\\below") == nullptr);
  CHECK(rex::HasFlags(folder->attributes, fs::FileAttributes::kDirectory));
  CHECK(rex::HasFlags(a->attributes, fs::FileAttributes::kReadOnly));
  CHECK(container.PathOf(*c) == "folder\\c.txt");
  CHECK(ReadNode(container, *a) == Bytes(kAText));
  CHECK(ReadNode(container, *big) == BigBin());
  CHECK(ReadNode(container, *c) == Bytes(kCText));

  std::vector<rex::u8> middle(0x20);
  auto read = container.Read(*big, 0x0FF0, middle);
  REQUIRE(read);
  CHECK(*read == 0x20);
  const auto expected = BigBin();
  CHECK(std::equal(middle.begin(), middle.end(), expected.begin() + 0x0FF0));

  std::vector<rex::u8> past(4);
  auto end = container.Read(*a, kAText.size(), past);
  REQUIRE_FALSE(end);
  CHECK(fs::CodeOf(end.error()) == fs::Error::kEndOfFile);

  const auto view = container.View(*big);
  REQUIRE(view.size() == expected.size());
  CHECK(std::equal(view.begin(), view.end(), expected.begin()));
}

std::vector<rex::u8> Fixture(const char* name) {
  auto bytes = rex::platform::ReadWholeFile(kFixtures / name);
  REQUIRE(bytes);
  return *bytes;
}

std::filesystem::path WriteVariant(const TempFolder& temp, const char* name,
                                   const std::vector<rex::u8>& bytes) {
  const auto path = temp.path / name;
  REQUIRE(rex::platform::WriteWholeFile(path, bytes));
  return path;
}

}  // namespace

REX_TEST_CASE("is_flags_enum operators combine and test flags", "[core][bit]") {
  ContainerTestFlags flags = ContainerTestFlags::kA | ContainerTestFlags::kC;
  CHECK(rex::HasFlags(flags, ContainerTestFlags::kA));
  CHECK_FALSE(rex::HasFlags(flags, ContainerTestFlags::kB));
  CHECK(rex::HasFlags(flags, ContainerTestFlags::kA | ContainerTestFlags::kC));
  flags |= ContainerTestFlags::kB;
  CHECK(rex::HasFlags(flags, ContainerTestFlags::kB));
  flags &= ~ContainerTestFlags::kA;
  CHECK_FALSE(rex::HasFlags(flags, ContainerTestFlags::kA));
  flags ^= ContainerTestFlags::kC;
  CHECK(flags == ContainerTestFlags::kB);
  CHECK((ContainerTestFlags::kA & ContainerTestFlags::kB) == ContainerTestFlags::kNone);
}

REX_TEST_CASE("ReadWholeFile and WriteWholeFile round trip and create parents", "[core][file]") {
  TempFolder temp;
  const auto path = temp.path / "a" / "b" / "data.bin";
  const std::vector<rex::u8> bytes = {1, 2, 3, 250};
  REQUIRE(rex::platform::WriteWholeFile(path, bytes));
  auto read = rex::platform::ReadWholeFile(path);
  REQUIRE(read);
  CHECK(*read == bytes);
  REQUIRE(rex::platform::WriteWholeFile(path, std::span<const rex::u8>()));
  read = rex::platform::ReadWholeFile(path);
  REQUIRE(read);
  CHECK(read->empty());
  CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
  CHECK_FALSE(rex::platform::ReadWholeFile(temp.path / "missing"));
}

REX_TEST_CASE("ReadSTFSHeader reads the fixture and refuses short bytes", "[core][container]") {
  auto header = fs::ReadSTFSHeader(Fixtures() / "stfs_con.bin");
  REQUIRE(header);
  CHECK(header->header.magic == fs::XContentPackageType::kCon);
  CHECK(header->metadata.volume_type == fs::XContentVolumeType::kSTFS);
  CHECK(header->metadata.execution_info.title_id == 0x52455831u);

  const auto bytes = Fixture("stfs_con.bin");
  auto shorter = fs::ReadSTFSHeader(std::span(bytes).first(0x100));
  REQUIRE_FALSE(shorter);
  CHECK(fs::CodeOf(shorter.error()) == fs::Error::kCorrupt);
  CHECK_FALSE(fs::ReadSTFSHeader(kFixtures / "missing.bin"));
}

REX_TEST_CASE("Container opens the STFS fixture", "[core][container]") {
  auto container = fs::Container::Open(Fixtures() / "stfs_con.bin");
  REQUIRE(container);
  CHECK(container->kind() == fs::ContainerKind::kSTFS);
  REQUIRE(container->header());
  CheckTree(*container);
}

REX_TEST_CASE("Container opens the GDFX fixture", "[core][container]") {
  auto container = fs::Container::Open(Fixtures() / "gdfx.iso");
  REQUIRE(container);
  CHECK(container->kind() == fs::ContainerKind::kGDFX);
  CHECK(container->header() == nullptr);
  CheckTree(*container);
}

REX_TEST_CASE("Container extracts byte exact", "[core][container]") {
  for (const char* name : {"stfs_con.bin", "gdfx.iso"}) {
    TempFolder temp;
    auto container = fs::Container::Open(kFixtures / name);
    REQUIRE(container);
    const auto out = temp.path / "out";
    REQUIRE(container->Extract(out));
    CHECK(rex::platform::ReadWholeFile(out / "a.txt").value() == Bytes(kAText));
    CHECK(rex::platform::ReadWholeFile(out / "big.bin").value() == BigBin());
    CHECK(rex::platform::ReadWholeFile(out / "folder" / "c.txt").value() == Bytes(kCText));
  }
}

REX_TEST_CASE("Container refuses a dot dot name and removes its output", "[core][container]") {
  TempFolder temp;
  auto bytes = Fixture("gdfx.iso");
  const size_t entry = size_t{33} * 0x800;
  bytes[entry + 13] = 2;
  bytes[entry + 14] = '.';
  bytes[entry + 15] = '.';
  const auto path = WriteVariant(temp, "dotdot.iso", bytes);
  auto container = fs::Container::Open(path);
  REQUIRE(container);
  const auto out = temp.path / "out";
  auto extracted = container->Extract(out);
  REQUIRE_FALSE(extracted);
  CHECK(fs::CodeOf(extracted.error()) == fs::Error::kInvalidPath);
  CHECK_FALSE(std::filesystem::exists(out));
}

REX_TEST_CASE("Container walks a long GDFX sibling chain without recursion", "[core][container]") {
  constexpr rex::u32 kEntries = 16000;
  constexpr size_t kSector = 0x800;
  TempFolder temp;
  auto gdfx = Fixture("gdfx.iso");
  const size_t table = rex::round_up(gdfx.size(), kSector);
  const size_t table_size = size_t{kEntries} * 16;
  gdfx.resize(table + table_size, 0);
  for (rex::u32 i = 0; i < kEntries; ++i) {
    const size_t entry = table + (size_t{i} * 16);
    const rex::u16 next = i + 1 < kEntries ? static_cast<rex::u16>((i + 1) * 4) : 0;
    std::memcpy(&gdfx[entry + 2], &next, sizeof(next));
    gdfx[entry + 12] = 0x80;
    gdfx[entry + 13] = 1;
    gdfx[entry + 14] = 'f';
  }
  const size_t volume = 32 * kSector;
  const auto root_sector = static_cast<rex::u32>(table / kSector);
  const auto root_size = static_cast<rex::u32>(table_size);
  std::memcpy(&gdfx[volume + 20], &root_sector, sizeof(root_sector));
  std::memcpy(&gdfx[volume + 24], &root_size, sizeof(root_size));

  auto container = fs::Container::Open(WriteVariant(temp, "chain.iso", gdfx));
  REQUIRE(container);
  CHECK(container->root().children.size() == kEntries);
}

REX_TEST_CASE("Container reports truncated and out of range images as corrupt",
              "[core][container]") {
  TempFolder temp;
  auto stfs = Fixture("stfs_con.bin");
  stfs.resize(0xA800);
  auto truncated = fs::Container::Open(WriteVariant(temp, "short.bin", stfs));
  REQUIRE_FALSE(truncated);
  CHECK(fs::CodeOf(truncated.error()) == fs::Error::kCorrupt);

  auto gdfx = Fixture("gdfx.iso");
  const size_t folder_entry = (size_t{33} * 0x800) + (size_t{11} * 4);
  gdfx[folder_entry + 4] = 0xFF;
  gdfx[folder_entry + 5] = 0xFF;
  gdfx[folder_entry + 6] = 0xFF;
  auto out_of_range = fs::Container::Open(WriteVariant(temp, "range.iso", gdfx));
  REQUIRE_FALSE(out_of_range);
  CHECK(fs::CodeOf(out_of_range.error()) == fs::Error::kCorrupt);

  auto garbage = fs::Container::Open(WriteVariant(temp, "garbage.bin", {1, 2, 3, 4, 5}));
  REQUIRE_FALSE(garbage);
  CHECK(fs::CodeOf(garbage.error()) == fs::Error::kCorrupt);
}
