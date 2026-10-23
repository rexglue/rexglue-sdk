/**
 * @file        core/filesystem/xcontent.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/filesystem/xcontent_layout.h>

#include <optional>

#include <rex/math.h>
#include <rex/string.h>

namespace rex::filesystem {

namespace {

std::u16string ReadBigEndian(const be_u16* units, size_t count) {
  std::u16string result;
  for (size_t i = 0; i < count && units[i] != 0; ++i) {
    result.push_back(static_cast<char16_t>(static_cast<u16>(units[i])));
  }
  return result;
}

std::optional<u32> FindLanguageSlot(XLanguage language) {
  if (language == XLanguage::kSChinese) {
    language = XLanguage::kSChineseDeprecated;
  }
  const u32 slot = static_cast<u32>(language) - 1;
  if (language == XLanguage::kInvalid || slot >= XContentMetadata::kNumLanguagesV2) {
    return std::nullopt;
  }
  return slot;
}

u32 LanguageSlot(XLanguage language) {
  return FindLanguageSlot(language).value_or(std::to_underlying(XLanguage::kEnglish) - 1);
}

}  // namespace

std::u16string XContentMetadata::GetDisplayName(XLanguage language) const {
  const u32 slot = LanguageSlot(language);
  if (slot < kNumLanguagesV1) {
    return ReadBigEndian(display_name_raw.uint[slot], 128);
  }
  if (metadata_version >= 2) {
    return ReadBigEndian(display_name_ex_raw.uint[slot - kNumLanguagesV1], 128);
  }
  return {};
}

std::u16string XContentMetadata::GetDescription(XLanguage language) const {
  const u32 slot = LanguageSlot(language);
  if (slot < kNumLanguagesV1) {
    return ReadBigEndian(description_raw.uint[slot], 128);
  }
  if (metadata_version >= 2) {
    return ReadBigEndian(description_ex_raw.uint[slot - kNumLanguagesV1], 128);
  }
  return {};
}

std::u16string XContentMetadata::GetPublisher() const {
  return ReadBigEndian(publisher_raw.uint, 64);
}

std::u16string XContentMetadata::GetTitleName() const {
  return ReadBigEndian(title_name_raw.uint, 64);
}

bool XContentMetadata::SetDisplayName(XLanguage language, std::u16string_view value) {
  const std::optional<u32> found = FindLanguageSlot(language);
  if (!found) {
    return false;
  }
  const u32 slot = *found;
  char16_t* destination = nullptr;
  if (slot < kNumLanguagesV1) {
    destination = display_name_raw.chars[slot];
  } else if (metadata_version >= 2) {
    destination = display_name_ex_raw.chars[slot - kNumLanguagesV1];
  }
  if (!destination) {
    return false;
  }
  CopyAndSwapTruncating(destination, value, CountOf(display_name_raw.chars[0]));
  return true;
}

bool XContentMetadata::SetDescription(XLanguage language, std::u16string_view value) {
  const std::optional<u32> found = FindLanguageSlot(language);
  if (!found) {
    return false;
  }
  const u32 slot = *found;
  char16_t* destination = nullptr;
  if (slot < kNumLanguagesV1) {
    destination = description_raw.chars[slot];
  } else if (metadata_version >= 2) {
    destination = description_ex_raw.chars[slot - kNumLanguagesV1];
  }
  if (!destination) {
    return false;
  }
  CopyAndSwapTruncating(destination, value, CountOf(description_raw.chars[0]));
  return true;
}

void XContentMetadata::SetPublisher(std::u16string_view value) {
  CopyAndSwapTruncating(publisher_raw.chars, value, CountOf(publisher_raw.chars));
}

void XContentMetadata::SetTitleName(std::u16string_view value) {
  CopyAndSwapTruncating(title_name_raw.chars, value, CountOf(title_name_raw.chars));
}

}  // namespace rex::filesystem
