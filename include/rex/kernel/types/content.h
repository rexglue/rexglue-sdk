/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/types/content.h
 * @brief       Content package and language IDs
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <rex/assert.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/string/text.h>
#include <rex/types.h>

namespace rex::kernel {

/** Type of a content package and the hex folder name of its path. */
enum class XContentType : u32 {
  kInvalid = 0x00000000, /**< No content type. */
  kSavedGame = 0x00000001,
  kMarketplaceContent = 0x00000002,
  kPublisher = 0x00000003,
  kXbox360Title = 0x00001000,
  kIPTVPauseBuffer = 0x00002000,
  kXNACommunity = 0x00003000,
  kInstalledGame = 0x00004000,
  kXboxTitle = 0x00005000,
  kSocialTitle = 0x00006000,
  kGamesOnDemand = 0x00007000,
  kSUStoragePack = 0x00008000,
  kAvatarItem = 0x00009000,
  kProfile = 0x00010000,
  kGamerPicture = 0x00020000,
  kTheme = 0x00030000,
  kCacheFile = 0x00040000,
  kStorageDownload = 0x00050000,
  kXboxSavedGame = 0x00060000,
  kXboxDownload = 0x00070000,
  kGameDemo = 0x00080000,
  kVideo = 0x00090000,
  kGameTitle = 0x000A0000,
  kInstaller = 0x000B0000,
  kGameTrailer = 0x000C0000,
  kArcadeTitle = 0x000D0000,
  kXNA = 0x000E0000,
  kLicenseStore = 0x000F0000,
  kMovie = 0x00100000,
  kTV = 0x00200000,
  kMusicVideo = 0x00300000,
  kGameVideo = 0x00400000,
  kPodcastVideo = 0x00500000,
  kViralVideo = 0x00600000,
  kCommunityGame = 0x02000000,
};

/** Placeholder in XCONTENT_AGGREGATE_DATA, substituted with the running title ID. */
constexpr u32 kRunningTitleID = 0xFFFFFFFF;

/** Metadata of a content package on a storage device, as the title lays it out. */
struct XCONTENT_DATA {
  be_u32 device_id;
  BE<XContentType> content_type;
  union {
    /** Plain u16 so the copy constructor is generated. */
    u16 code_units[128];
    /** The same 256 bytes, written through the swapping copy. */
    char16_t chars[128];
  } display_name_raw; /**< Big endian UTF-16 name shown to the player. */

  char file_name_raw[42];

  /** Some titles use this padding as the null terminator of a full file_name_raw. */
  u8 padding[2];

  /** Compares the fields that locate a package, device_id, content_type and file_name. */
  bool operator==(const XCONTENT_DATA& other) const {
    return device_id == other.device_id && content_type == other.content_type &&
           rex::string::utf8_equal_case(GetFileName(), other.GetFileName());
  }

  /**
   * Returns the display name converted to host byte order.
   */
  std::u16string GetDisplayName() const {
    return memory::load_and_swap<std::u16string>(display_name_raw.code_units);
  }

  /**
   * Returns the file name up to its first null or 42 characters.
   */
  std::string GetFileName() const {
    return std::string(file_name_raw, strnlen(file_name_raw, CountOf(file_name_raw)));
  }

  /**
   * Sets the display name, blanking the array first because some titles need several terminators.
   */
  void SetDisplayName(const std::u16string_view value) {
    std::fill_n(display_name_raw.chars, CountOf(display_name_raw.chars), 0);
    rex::string::CopyAndSwapTruncating(display_name_raw.chars, value,
                                       CountOf(display_name_raw.chars));
  }

  /** Sets the file name and zeroes padding, which some titles read as the null terminator. */
  void SetFileName(const std::string_view value) {
    std::fill_n(file_name_raw, CountOf(file_name_raw), 0);
    rex::string::CopyUnterminated(file_name_raw, value, rex::CountOf(file_name_raw));

    padding[0] = padding[1] = 0;
  }
};
REX_STATIC_ASSERT_SIZE(XCONTENT_DATA, 0x134);

/** Content data with the owner of the package, as the aggregate enumerator returns it. */
struct XCONTENT_AGGREGATE_DATA : XCONTENT_DATA {
  be_u64 xuid;     /**< ID of the profile that owns the package, 0 when none. */
  be_u32 title_id; /**< ID of the title that owns the package. */

  XCONTENT_AGGREGATE_DATA() = default;
  /** Builds aggregate data from content data, with xuid 0 and the running title ID. */
  XCONTENT_AGGREGATE_DATA(const XCONTENT_DATA& other) {
    device_id = other.device_id;
    content_type = other.content_type;
    SetDisplayName(other.GetDisplayName());
    SetFileName(other.GetFileName());
    padding[0] = padding[1] = 0;
    xuid = 0;
    title_id = kRunningTitleID;
  }

  /** Compares the fields that locate a package, device_id, title_id, content_type and file_name. */
  bool operator==(const XCONTENT_AGGREGATE_DATA& other) const {
    return device_id == other.device_id && title_id == other.title_id &&
           content_type == other.content_type &&
           rex::string::utf8_equal_case(GetFileName(), other.GetFileName());
  }
};
REX_STATIC_ASSERT_SIZE(XCONTENT_AGGREGATE_DATA, 0x148);

/** Title language IDs, the XC_LANGUAGE_ values of the XDK header xconfig.h. */
enum class XLanguage : u8 {
  kInvalid = 0,
  kEnglish = 1,
  kJapanese = 2,
  kGerman = 3,
  kFrench = 4,
  kSpanish = 5,
  kItalian = 6,
  kKorean = 7,
  kTChinese = 8,
  kPortuguese = 9,
  kSChineseDeprecated = 10, /**< Placeholder the SDK keeps for backward compatibility. */
  kPolish = 11,
  kRussian = 12,
  kSwedish = 13,
  kTurkish = 14,
  kNorwegian = 15,
  kDutch = 16,
  kSChinese = 17,
  kMaxLanguages = 18 /**< Number of language IDs, not a language. */
};

/**
 * Converts a language ID read from guest memory, a file or a config value.
 *
 * @param value The language ID.
 * @return The language, or XLanguage::kInvalid when value names none.
 */
constexpr XLanguage ToXLanguage(u32 value) noexcept {
  return value < std::to_underlying(XLanguage::kMaxLanguages) ? static_cast<XLanguage>(value)
                                                              : XLanguage::kInvalid;
}

}  // namespace rex::kernel
