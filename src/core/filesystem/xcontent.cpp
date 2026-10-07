/**
 * @file        core/filesystem/xcontent.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/stfs_xbox.h (Ben Vanik, 2021).
 */

#include <rex/filesystem/xcontent.h>

#include <cstring>
#include <chrono>
#include <format>

#include <rex/filesystem/error.h>
#include <rex/math.h>
#include <rex/memory/mapped_memory.h>
#include <rex/platform.h>
#include <rex/string.h>

namespace rex::filesystem {

namespace {

constexpr i64 kUnixToFileTimeSeconds = 11644473600LL;
constexpr i64 kTicksPerSecond = 10000000;

std::u16string ReadBigEndian(const be_u16* units, size_t count) {
  std::u16string result;
  for (size_t i = 0; i < count && units[i] != 0; ++i) {
    result.push_back(static_cast<char16_t>(static_cast<u16>(units[i])));
  }
  return result;
}

u32 LanguageSlot(XLanguage language) {
  const u32 slot = static_cast<u32>(language) - 1;
  if (slot >= XContentMetadata::kNumLanguagesV2) {
    return std::to_underlying(XLanguage::kEnglish) - 1;
  }
  return slot;
}

}  // namespace

u64 decode_fat_timestamp(u32 date, u32 time) {
  using namespace std::chrono;
  const year_month first = year{1980 + static_cast<i32>((date >> 9) & 0x7F)} / January;
  const year_month month = first + months{static_cast<i32>((date >> 5) & 0x0F) - 1};
  const sys_days day = sys_days{month / 1} + days{static_cast<i32>(date & 0x1F) - 1};
  const sys_seconds stamp = day + hours{static_cast<i32>((time >> 11) & 0x1F)} +
                            minutes{static_cast<i32>((time >> 5) & 0x3F)} +
                            seconds{static_cast<i32>((time & 0x1F) * 2)};
  return static_cast<u64>((stamp.time_since_epoch().count() + kUnixToFileTimeSeconds) *
                          kTicksPerSecond);
}

std::u16string XContentMetadata::display_name(XLanguage language) const {
  const u32 slot = LanguageSlot(language);
  if (slot < kNumLanguagesV1) {
    return ReadBigEndian(display_name_raw.uint[slot], 128);
  }
  if (metadata_version >= 2) {
    return ReadBigEndian(display_name_ex_raw.uint[slot - kNumLanguagesV1], 128);
  }
  return {};
}

std::u16string XContentMetadata::description(XLanguage language) const {
  const u32 slot = LanguageSlot(language);
  if (slot < kNumLanguagesV1) {
    return ReadBigEndian(description_raw.uint[slot], 128);
  }
  if (metadata_version >= 2) {
    return ReadBigEndian(description_ex_raw.uint[slot - kNumLanguagesV1], 128);
  }
  return {};
}

std::u16string XContentMetadata::publisher() const {
  return ReadBigEndian(publisher_raw.uint, 64);
}

std::u16string XContentMetadata::title_name() const {
  return ReadBigEndian(title_name_raw.uint, 64);
}

bool XContentMetadata::set_display_name(XLanguage language, std::u16string_view value) {
  const u32 slot = LanguageSlot(language);
  char16_t* destination = nullptr;
  if (slot < kNumLanguagesV1) {
    destination = display_name_raw.chars[slot];
  } else if (metadata_version >= 2) {
    destination = display_name_ex_raw.chars[slot - kNumLanguagesV1];
  }
  if (!destination) {
    return false;
  }
  copy_and_swap_truncating(destination, value, countof(display_name_raw.chars[0]));
  return true;
}

bool XContentMetadata::set_description(XLanguage language, std::u16string_view value) {
  const u32 slot = LanguageSlot(language);
  char16_t* destination = nullptr;
  if (slot < kNumLanguagesV1) {
    destination = description_raw.chars[slot];
  } else if (metadata_version >= 2) {
    destination = description_ex_raw.chars[slot - kNumLanguagesV1];
  }
  if (!destination) {
    return false;
  }
  copy_and_swap_truncating(destination, value, countof(description_raw.chars[0]));
  return true;
}

void XContentMetadata::set_publisher(std::u16string_view value) {
  copy_and_swap_truncating(publisher_raw.chars, value, countof(publisher_raw.chars));
}

void XContentMetadata::set_title_name(std::u16string_view value) {
  copy_and_swap_truncating(title_name_raw.chars, value, countof(title_name_raw.chars));
}

[[nodiscard]] Result<STFSHeader> ReadSTFSHeader(std::span<const u8> bytes) {
  if (bytes.size() < sizeof(STFSHeader)) {
    return Err<STFSHeader>(
        MakeError(Error::kCorrupt, std::format("package header needs {:#x} bytes, {:#x} present",
                                               sizeof(STFSHeader), bytes.size())));
  }
  STFSHeader header;
  std::memcpy(&header, bytes.data(), sizeof(STFSHeader));
  if (!header.header.is_magic_valid()) {
    return Err<STFSHeader>(MakeError(Error::kCorrupt, "package magic is not CON, LIVE or PIRS"));
  }
  return header;
}

[[nodiscard]] Result<STFSHeader> ReadSTFSHeader(const std::filesystem::path& path) {
  auto map = memory::MappedMemory::Open(path, memory::MappedMemory::Mode::kRead);
  if (!map) {
    return Err<STFSHeader>(
        MakeError(Error::kNotFound, std::format("cannot open '{}'", path_to_utf8(path))));
  }
  return ReadSTFSHeader(std::span<const u8>(map->data(), map->size()));
}

}  // namespace rex::filesystem
