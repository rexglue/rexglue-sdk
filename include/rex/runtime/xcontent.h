/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 Tom Clay <tomc@tctechstuff.com>
 *              Modified for rexglue - Xbox 360 recompilation framework
 *
 * @changes     - Extracted content types from xbox.h
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <rex/assert.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/string.h>
#include <rex/types.h>

namespace rex::runtime {

using XNotificationID = u32;

enum class XLanguage : u32 {
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
  kSChinese = 10,
  kPolish = 11,
  kRussian = 12,
  kMaxLanguages = 13
};

enum class XContentType : u32 {
  kSavedGame = 0x00000001,
  kMarketplaceContent = 0x00000002,
  kPublisher = 0x00000003,
  kXbox360Title = 0x00001000,
  kIptvPauseBuffer = 0x00002000,
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

}  // namespace rex::runtime

namespace rex::runtime::xam {

// If set in XCONTENT_AGGREGATE_DATA, will be substituted with the running
// titles ID
// TODO: check if actual x360 kernel/xam has a value similar to this
constexpr u32 kCurrentlyRunningTitleId = 0xFFFFFFFF;

struct XCONTENT_DATA {
  be<u32> device_id;
  be<XContentType> content_type;
  union {
    // this should be be<u16>, but that stops copy constructor being
    // generated...
    u16 uint[128];
    char16_t chars[128];
  } display_name_raw;

  char file_name_raw[42];

  // Some games use this padding field as a null-terminator, as eg.
  // DLC packages usually fill the entire file_name_raw array
  // Not every game sets it to 0 though, so make sure any file_name_raw reads
  // only go up to 42 chars!
  u8 padding[2];

  bool operator==(const XCONTENT_DATA& other) const {
    // Package is located via device_id/content_type/file_name, so only need to
    // compare those
    return device_id == other.device_id && content_type == other.content_type &&
           file_name() == other.file_name();
  }

  std::u16string display_name() const {
    return memory::load_and_swap<std::u16string>(display_name_raw.uint);
  }

  std::string file_name() const {
    return std::string(file_name_raw, strnlen(file_name_raw, countof(file_name_raw)));
  }

  void set_display_name(const std::u16string_view value) {
    // Some games (e.g. 584108A9) require multiple null-terminators for it to
    // read the string properly, blanking the array should take care of that

    std::fill_n(display_name_raw.chars, countof(display_name_raw.chars), 0);
    rex::copy_and_swap_truncating(display_name_raw.chars, value, countof(display_name_raw.chars));
  }

  void set_file_name(const std::string_view value) {
    std::fill_n(file_name_raw, countof(file_name_raw), 0);
    rex::copy_unterminated(file_name_raw, value, rex::countof(file_name_raw));

    // Some games rely on padding field acting as a null-terminator...
    padding[0] = padding[1] = 0;
  }
};
static_assert_size(XCONTENT_DATA, 0x134);

struct XCONTENT_AGGREGATE_DATA : XCONTENT_DATA {
  be<u64> xuid;  // some titles store XUID here?
  be<u32> title_id;

  XCONTENT_AGGREGATE_DATA() = default;
  XCONTENT_AGGREGATE_DATA(const XCONTENT_DATA& other) {
    device_id = other.device_id;
    content_type = other.content_type;
    set_display_name(other.display_name());
    set_file_name(other.file_name());
    padding[0] = padding[1] = 0;
    xuid = 0;
    title_id = kCurrentlyRunningTitleId;
  }

  bool operator==(const XCONTENT_AGGREGATE_DATA& other) const {
    // Package is located via device_id/title_id/content_type/file_name, so only
    // need to compare those
    return device_id == other.device_id && title_id == other.title_id &&
           content_type == other.content_type && file_name() == other.file_name();
  }
};
static_assert_size(XCONTENT_AGGREGATE_DATA, 0x148);

}  // namespace rex::runtime::xam
