/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
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

/** Notification id, whose bit fields `XNotificationKey` splits. */
using XNotificationID = u32;

/** Title language ids. */
enum class XLanguage : u32 {
  kInvalid = 0,      /**< No language. */
  kEnglish = 1,      /**< English. */
  kJapanese = 2,     /**< Japanese. */
  kGerman = 3,       /**< German. */
  kFrench = 4,       /**< French. */
  kSpanish = 5,      /**< Spanish. */
  kItalian = 6,      /**< Italian. */
  kKorean = 7,       /**< Korean. */
  kTChinese = 8,     /**< Traditional Chinese. */
  kPortuguese = 9,   /**< Portuguese. */
  kSChinese = 10,    /**< Simplified Chinese. */
  kPolish = 11,      /**< Polish. */
  kRussian = 12,     /**< Russian. */
  kMaxLanguages = 13 /**< Number of language ids, not a language. */
};

/** Type of a content package. */
enum class XContentType : u32 {
  kSavedGame = 0x00000001,          /**< A previously saved game. */
  kMarketplaceContent = 0x00000002, /**< Content purchased from the Marketplace. */
  kPublisher = 0x00000003,          /**< Publisher specific content. */
  kXbox360Title = 0x00001000,       /**< Used only as the hex folder name of the package path. */
  kIptvPauseBuffer = 0x00002000,    /**< Used only as the hex folder name of the package path. */
  kXNACommunity = 0x00003000,       /**< Used only as the hex folder name of the package path. */
  kInstalledGame = 0x00004000,      /**< Used only as the hex folder name of the package path. */
  kXboxTitle = 0x00005000,          /**< Used only as the hex folder name of the package path. */
  kSocialTitle = 0x00006000,        /**< Used only as the hex folder name of the package path. */
  kGamesOnDemand = 0x00007000,      /**< Used only as the hex folder name of the package path. */
  kSUStoragePack = 0x00008000,      /**< Used only as the hex folder name of the package path. */
  kAvatarItem = 0x00009000,         /**< Avatar item. */
  kProfile = 0x00010000,            /**< Profile. */
  kGamerPicture = 0x00020000,       /**< Gamer picture. */
  kTheme = 0x00030000,              /**< Theme. */
  kCacheFile = 0x00040000,          /**< Used only as the hex folder name of the package path. */
  kStorageDownload = 0x00050000,    /**< Used only as the hex folder name of the package path. */
  kXboxSavedGame = 0x00060000,      /**< Used only as the hex folder name of the package path. */
  kXboxDownload = 0x00070000,       /**< Used only as the hex folder name of the package path. */
  kGameDemo = 0x00080000,           /**< Game demo. */
  kVideo = 0x00090000,              /**< Video. */
  kGameTitle = 0x000A0000,          /**< Used only as the hex folder name of the package path. */
  kInstaller = 0x000B0000,          /**< Used only as the hex folder name of the package path. */
  kGameTrailer = 0x000C0000,        /**< Game trailer. */
  kArcadeTitle = 0x000D0000,        /**< Used only as the hex folder name of the package path. */
  kXNA = 0x000E0000,                /**< Used only as the hex folder name of the package path. */
  kLicenseStore = 0x000F0000,       /**< Used only as the hex folder name of the package path. */
  kMovie = 0x00100000,              /**< Movie. */
  kTV = 0x00200000,                 /**< TV show. */
  kMusicVideo = 0x00300000,         /**< Music video. */
  kGameVideo = 0x00400000,          /**< Game video. */
  kPodcastVideo = 0x00500000,       /**< Podcast video. */
  kViralVideo = 0x00600000,         /**< Viral video. */
  kCommunityGame = 0x02000000,      /**< Used only as the hex folder name of the package path. */
};

}  // namespace rex::runtime

namespace rex::runtime::xam {

/** Placeholder in XCONTENT_AGGREGATE_DATA, substituted with the running title ID. */
constexpr u32 kCurrentlyRunningTitleId = 0xFFFFFFFF;

/** Metadata of a content package on a storage device, as the title lays it out. */
struct XCONTENT_DATA {
  be_u32 device_id; /**< Offset 0x0. Id of the storage device that holds the package. */
  be<XContentType> content_type; /**< Offset 0x4. Type of the package. */
  union {
    /** Plain u16 because be_u16 would stop the copy constructor being generated. */
    u16 uint[128];
    /** The same 256 bytes as uint, used to write the name through the swapping copy. */
    char16_t chars[128];
  } display_name_raw; /**< Offset 0x8. Big endian UTF-16 name shown to the player. */

  char file_name_raw[42]; /**< Offset 0x108. File name of the package. */

  /**
   * Downloadable content fills all of file_name_raw, so some titles use this padding as its null
   * terminator. Not every title zeroes it, so reads of file_name_raw stop at 42 chars.
   */
  u8 padding[2]; /**< Offset 0x132. Trailing bytes after the file name. */

  /**
   * Compares the fields that locate a package, device_id, content_type and file_name.
   *
   * @param other The data to compare with.
   * @return True when the fields match.
   */
  bool operator==(const XCONTENT_DATA& other) const {
    return device_id == other.device_id && content_type == other.content_type &&
           file_name() == other.file_name();
  }

  /**
   * Returns the display name converted to host byte order.
   *
   * @return The name.
   */
  std::u16string display_name() const {
    return memory::load_and_swap<std::u16string>(display_name_raw.uint);
  }

  /**
   * Returns the file name up to its first null or 42 characters.
   *
   * @return The name.
   */
  std::string file_name() const {
    return std::string(file_name_raw, strnlen(file_name_raw, countof(file_name_raw)));
  }

  /**
   * Sets the display name. The array is blanked first because some titles need multiple null
   * terminators to read the string.
   *
   * @param value The name.
   */
  void set_display_name(const std::u16string_view value) {
    std::fill_n(display_name_raw.chars, countof(display_name_raw.chars), 0);
    rex::copy_and_swap_truncating(display_name_raw.chars, value, countof(display_name_raw.chars));
  }

  /**
   * Sets the file name and zeroes padding, which some titles read as the null terminator.
   *
   * @param value The name.
   */
  void set_file_name(const std::string_view value) {
    std::fill_n(file_name_raw, countof(file_name_raw), 0);
    rex::copy_unterminated(file_name_raw, value, rex::countof(file_name_raw));

    padding[0] = padding[1] = 0;
  }
};
REX_STATIC_ASSERT_SIZE(XCONTENT_DATA, 0x134);

/** Content data with the owner of the package, as the aggregate enumerator returns it. */
struct XCONTENT_AGGREGATE_DATA : XCONTENT_DATA {
  be_u64 xuid;     /**< Offset 0x138. Id of the profile that owns the package, 0 when none. */
  be_u32 title_id; /**< Offset 0x140. Id of the title that owns the package. */

  XCONTENT_AGGREGATE_DATA() = default;
  /**
   * Builds aggregate data from content data, with xuid 0 and the running title id.
   *
   * @param other The content data to copy.
   */
  XCONTENT_AGGREGATE_DATA(const XCONTENT_DATA& other) {
    device_id = other.device_id;
    content_type = other.content_type;
    set_display_name(other.display_name());
    set_file_name(other.file_name());
    padding[0] = padding[1] = 0;
    xuid = 0;
    title_id = kCurrentlyRunningTitleId;
  }

  /**
   * Compares the fields that locate a package, device_id, title_id, content_type and file_name.
   *
   * @param other The data to compare with.
   * @return True when the fields match.
   */
  bool operator==(const XCONTENT_AGGREGATE_DATA& other) const {
    return device_id == other.device_id && title_id == other.title_id &&
           content_type == other.content_type && file_name() == other.file_name();
  }
};
REX_STATIC_ASSERT_SIZE(XCONTENT_AGGREGATE_DATA, 0x148);

}  // namespace rex::runtime::xam
