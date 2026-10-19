/**
 * @file        tests/unit/runtime/xdbf_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <span>
#include <string_view>
#include <vector>

#include <rex/runtime/xdbf_reader.h>

namespace {

using rex::u16;
using rex::u32;
using rex::u64;
using rex::u8;
using rex::runtime::XDBFGameData;
using rex::runtime::XDBFReader;
using rex::runtime::XDBFSection;
using rex::runtime::XLanguage;

constexpr u64 kTitleId = 0x8000;
constexpr u64 kXstcId = 0x58535443;
constexpr u64 kXachId = 0x58414348;
constexpr size_t kSectionHeaderSize = 14;
constexpr size_t kStringEntrySize = 4;
constexpr size_t kAchievementSize = 0x24;

struct Entry {
  XDBFSection section;
  u64 id;
  u32 offset;
  u32 size;
};

class Bytes {
 public:
  Bytes& Put(u64 value, size_t width) {
    for (size_t i = width; i > 0; --i) {
      data.push_back(static_cast<u8>(value >> ((i - 1) * 8)));
    }
    return *this;
  }

  Bytes& Text(std::string_view text) {
    data.insert(data.end(), text.begin(), text.end());
    return *this;
  }

  Bytes& Zeros(size_t count) {
    data.insert(data.end(), count, 0);
    return *this;
  }

  std::vector<u8> data;
};

std::vector<u8> Database(std::span<const Entry> entries, u32 entry_count,
                         std::span<const u8> content, std::string_view magic = "XDBF") {
  Bytes bytes;
  bytes.Text(magic).Put(0x10000, 4).Put(entry_count, 4).Put(entries.size(), 4).Put(0, 4).Put(0, 4);
  for (const Entry& entry : entries) {
    bytes.Put(static_cast<u16>(entry.section), 2)
        .Put(entry.id, 8)
        .Put(entry.offset, 4)
        .Put(entry.size, 4);
  }
  bytes.data.insert(bytes.data.end(), content.begin(), content.end());
  return std::move(bytes.data);
}

std::vector<u8> Database(const Entry& entry, std::span<const u8> content) {
  return Database(std::span(&entry, 1), 1, content);
}

std::vector<u8> TitleStrings(u16 count, std::string_view title) {
  Bytes bytes;
  bytes.Text("XSTR").Put(1, 4).Put(0, 4).Put(count, 2);
  bytes.Put(kTitleId, 2).Put(title.size(), 2).Text(title);
  return std::move(bytes.data);
}

}  // namespace

REX_TEST_CASE("An XDBF reader rejects an entry table past its data", "[runtime][xdbf]") {
  const std::vector<u8> content(4);
  const std::vector<u8> data = Database({}, 1000, content);
  CHECK_FALSE(XDBFReader(data.data(), data.size()).IsValid());
}

REX_TEST_CASE("An XDBF reader rejects more used entries than its table holds", "[runtime][xdbf]") {
  const Entry entries[] = {{XDBFSection::kImage, 1, 0, 0}, {XDBFSection::kImage, 2, 0, 0}};
  const std::vector<u8> content(4);
  const std::vector<u8> data = Database(entries, 1, content);
  CHECK_FALSE(XDBFReader(data.data(), data.size()).IsValid());
}

REX_TEST_CASE("An XDBF entry past the data is an empty block", "[runtime][xdbf]") {
  const std::vector<u8> content(4);
  const std::vector<u8> data = Database({XDBFSection::kImage, 1, 0, 100}, content);
  const XDBFReader reader(data.data(), data.size());
  REQUIRE(reader.IsValid());
  CHECK_FALSE(reader.GetEntry(XDBFSection::kImage, 1));
}

REX_TEST_CASE("An invalid XDBF reader returns an empty block", "[runtime][xdbf]") {
  const Entry entry{XDBFSection::kImage, 1, 0, 4};
  const std::vector<u8> content(4);
  const std::vector<u8> data = Database(std::span(&entry, 1), 1, content, "XDBX");
  const XDBFReader reader(data.data(), data.size());
  REQUIRE_FALSE(reader.IsValid());
  CHECK_FALSE(reader.GetEntry(XDBFSection::kImage, 1));
}

REX_TEST_CASE("An XDBF string table header past its block reads no string", "[runtime][xdbf]") {
  const std::vector<u8> content = TitleStrings(1, "Name");
  const std::vector<u8> data =
      Database({XDBFSection::kStringTable, static_cast<u64>(XLanguage::kEnglish), 0, 10}, content);
  const XDBFGameData db(data.data(), data.size());
  REQUIRE(db.IsValid());
  CHECK(db.GetTitle(XLanguage::kEnglish).empty());
}

REX_TEST_CASE("An XDBF string table entry past its block reads no string", "[runtime][xdbf]") {
  const std::vector<u8> content = TitleStrings(1, "Name");
  const std::vector<u8> data =
      Database({XDBFSection::kStringTable, static_cast<u64>(XLanguage::kEnglish), 0,
                static_cast<u32>(kSectionHeaderSize)},
               content);
  const XDBFGameData db(data.data(), data.size());
  REQUIRE(db.IsValid());
  CHECK(db.GetTitle(XLanguage::kEnglish).empty());
}

REX_TEST_CASE("An XDBF string past its block reads no string", "[runtime][xdbf]") {
  const std::vector<u8> content = TitleStrings(1, "Name");
  const std::vector<u8> data =
      Database({XDBFSection::kStringTable, static_cast<u64>(XLanguage::kEnglish), 0,
                static_cast<u32>(kSectionHeaderSize + kStringEntrySize + 2)},
               content);
  const XDBFGameData db(data.data(), data.size());
  REQUIRE(db.IsValid());
  CHECK(db.GetTitle(XLanguage::kEnglish).empty());
}

REX_TEST_CASE("An XDBF achievement table stops at its block end", "[runtime][xdbf]") {
  Bytes content;
  content.Text("XACH").Put(1, 4).Put(0, 4).Put(2, 2).Zeros(2 * kAchievementSize);
  const std::vector<u8> data = Database(
      {XDBFSection::kMetadata, kXachId, 0, static_cast<u32>(kSectionHeaderSize + kAchievementSize)},
      content.data);
  const XDBFReader reader(data.data(), data.size());
  REQUIRE(reader.IsValid());
  CHECK(reader.GetAchievements().size() == 1);
}

REX_TEST_CASE("An XDBF language block smaller than the XSTC structure keeps the default language",
              "[runtime][xdbf]") {
  Bytes content;
  content.Text("XSTC").Put(1, 4).Put(4, 4).Put(static_cast<u32>(XLanguage::kJapanese), 4);
  const std::vector<u8> data = Database({XDBFSection::kMetadata, kXstcId, 0, 8}, content.data);
  const XDBFGameData db(data.data(), data.size());
  REQUIRE(db.IsValid());
  CHECK(db.GetDefaultLanguage() == XLanguage::kEnglish);
}

REX_TEST_CASE("An XDBF string table with another magic or version reads no string",
              "[runtime][xdbf]") {
  const std::vector<u8> good = TitleStrings(1, "Name");
  const Entry entry{XDBFSection::kStringTable, static_cast<u64>(XLanguage::kEnglish), 0,
                    static_cast<u32>(good.size())};
  {
    const std::vector<u8> data = Database(entry, good);
    const XDBFGameData db(data.data(), data.size());
    REQUIRE(db.IsValid());
    CHECK(db.GetTitle(XLanguage::kEnglish) == "Name");
  }
  auto bad_magic = good;
  bad_magic[3] = 'X';
  auto bad_version = good;
  bad_version[7] = 2;
  for (const auto& content : {bad_magic, bad_version}) {
    const std::vector<u8> data = Database(entry, content);
    const XDBFGameData db(data.data(), data.size());
    REQUIRE(db.IsValid());
    CHECK(db.GetTitle(XLanguage::kEnglish).empty());
  }
}

REX_TEST_CASE("An XDBF achievement table with another magic or version lists nothing",
              "[runtime][xdbf]") {
  const Entry entry{XDBFSection::kMetadata, kXachId, 0,
                    static_cast<u32>(kSectionHeaderSize + kAchievementSize)};
  for (const auto& [magic, version] :
       {std::pair<std::string_view, u32>{"XACH", 1}, std::pair<std::string_view, u32>{"XACX", 1},
        std::pair<std::string_view, u32>{"XACH", 2}}) {
    Bytes content;
    content.Text(magic).Put(version, 4).Put(0, 4).Put(1, 2).Zeros(kAchievementSize);
    const std::vector<u8> data = Database(entry, content.data);
    const XDBFReader reader(data.data(), data.size());
    REQUIRE(reader.IsValid());
    CHECK(reader.GetAchievements().size() == (magic == "XACH" && version == 1 ? 1 : 0));
  }
}

REX_TEST_CASE("An XDBF language block with another magic keeps the default language",
              "[runtime][xdbf]") {
  for (const std::string_view magic : {"XSTC", "XSTX"}) {
    Bytes content;
    content.Text(magic).Put(1, 4).Put(4, 4).Put(static_cast<u32>(XLanguage::kJapanese), 4);
    const std::vector<u8> data = Database({XDBFSection::kMetadata, kXstcId, 0, 16}, content.data);
    const XDBFGameData db(data.data(), data.size());
    REQUIRE(db.IsValid());
    CHECK(db.GetDefaultLanguage() ==
          (magic == "XSTC" ? XLanguage::kJapanese : XLanguage::kEnglish));
  }
}
