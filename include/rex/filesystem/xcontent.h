/**
 * @file        filesystem/xcontent.h
 * @brief       On disk XContent, STFS and SVOD package structures
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/stfs_xbox.h (Ben Vanik, 2021).
 */

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include <rex/macros.h>
#include <rex/result.h>
#include <rex/filesystem/xex_types.h>
#include <rex/runtime/xcontent.h>
#include <rex/types.h>

namespace rex::filesystem {

using rex::runtime::XContentType;
using rex::runtime::XLanguage;

/**
 * Converts a FAT date and time to Windows FILETIME ticks.
 *
 * @param date FAT date, 7 bits of years since 1980, 4 bits of month, 5 bits of day.
 * @param time FAT time, 5 bits of hours, 6 bits of minutes, 5 bits of 2 second units.
 * @return 100 nanosecond intervals since 1601-01-01 UTC, or 0 when the date is invalid.
 */
REX_API u64 decode_fat_timestamp(u32 date, u32 time);

/**
 * Reads a 24 bit big endian value.
 *
 * @param p The 3 source bytes.
 * @return The value.
 */
constexpr u32 load_uint24_be(const u8* p) {
  return (static_cast<u32>(p[0]) << 16) | (static_cast<u32>(p[1]) << 8) | static_cast<u32>(p[2]);
}

/**
 * Reads a 24 bit little endian value.
 *
 * @param p The 3 source bytes.
 * @return The value.
 */
constexpr u32 load_uint24_le(const u8* p) {
  return (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[1]) << 8) | static_cast<u32>(p[0]);
}

/**
 * Writes a 24 bit little endian value.
 *
 * @param p The 3 destination bytes.
 * @param value The value, the top 8 bits are dropped.
 */
constexpr void store_uint24_le(u8* p, u32 value) {
  p[2] = static_cast<u8>((value >> 16) & 0xFF);
  p[1] = static_cast<u8>((value >> 8) & 0xFF);
  p[0] = static_cast<u8>(value & 0xFF);
}

/** Package magic, the first 4 bytes of a package. */
enum class XContentPackageType : u32 {
  kCon = 0x434F4E20,  /**< CON, signed by a console. */
  kPirs = 0x50495253, /**< PIRS, signed by Microsoft for offline content. */
  kLive = 0x4C495645, /**< LIVE, signed by Microsoft for Live content. */
};

/** Volume layout of a package. */
enum class XContentVolumeType : u32 {
  kSTFS = 0, /**< Secure transacted file system, 1 file. */
  kSVOD = 1, /**< Secure virtual optical disc, a header file and data fragments. */
};

#pragma pack(push, 1)

/** STFS volume descriptor inside the XContent metadata. */
struct STFSVolumeDescriptor {
  u8 descriptor_length; /**< Size of this descriptor, 0x24 when valid. */
  u8 version;           /**< Descriptor version. */
  /** Volume flags. */
  union {
    u8 as_byte; /**< The flags as 1 byte. */
    /** The flags as bits. */
    struct {
      u8 read_only_format : 1;             /**< 1 backing block per hash table instead of 2. */
      u8 root_active_index : 1;            /**< Top hash table uses the secondary block. */
      u8 directory_overallocated : 1;      /**< Directory blocks exceed the entries. */
      u8 directory_index_bounds_valid : 1; /**< Directory indices were bounds checked. */
    } bits;
  } flags;
  u16 file_table_block_count;        /**< Number of directory blocks, little endian. */
  u8 file_table_block_number_raw[3]; /**< First directory block, 24 bit little endian. */
  u8 top_hash_table_hash[0x14];      /**< SHA1 of the top hash table. */
  be<u32> total_block_count;         /**< Data blocks in the volume. */
  be<u32> free_block_count;          /**< Unallocated data blocks. */

  /**
   * Returns the first directory block.
   *
   * @return The block number.
   */
  [[nodiscard]] constexpr u32 file_table_block_number() const {
    return load_uint24_le(file_table_block_number_raw);
  }

  /**
   * Replaces the first directory block.
   *
   * @param value The block number.
   */
  constexpr void set_file_table_block_number(u32 value) {
    store_uint24_le(file_table_block_number_raw, value);
  }

  /**
   * Reports whether the descriptor length matches the structure.
   *
   * @return True when descriptor_length is 0x24.
   */
  [[nodiscard]] constexpr bool is_valid() const {
    return descriptor_length == sizeof(STFSVolumeDescriptor);
  }
};
static_assert(sizeof(STFSVolumeDescriptor) == 0x24);

#pragma pack(pop)

/** Allocation state of an STFS data block. */
enum class STFSHashState : u8 {
  kFree = 0,  /**< Unallocated and absent from the package. */
  kFree2 = 1, /**< Unallocated and present in the package. */
  kInUse = 2, /**< Allocated. */
};

/** 1 hash table entry, the hash and chain information of 1 block. */
struct STFSHashEntry {
  u8 sha1[0x14];    /**< SHA1 of the block. */
  be<u32> info_raw; /**< Packed chain and allocation fields. */

  /**
   * Returns the next block of a level 0 chain.
   *
   * @return The block number, 0xFFFFFF at the end of the chain.
   */
  [[nodiscard]] u32 level0_next_block() const { return info_raw & 0xFFFFFF; }

  /**
   * Replaces the next block of a level 0 chain.
   *
   * @param value The block number, 24 bits.
   */
  void set_level0_next_block(u32 value) { info_raw = (info_raw & ~0xFFFFFFu) | (value & 0xFFFFFF); }

  /**
   * Returns the allocation state of a level 0 entry.
   *
   * @return The state.
   */
  [[nodiscard]] STFSHashState level0_allocation_state() const {
    return static_cast<STFSHashState>(static_cast<u8>((info_raw >> 30) & 0x3));
  }

  /**
   * Replaces the allocation state of a level 0 entry.
   *
   * @param value The state.
   */
  void set_level0_allocation_state(STFSHashState value) {
    info_raw = (info_raw & ~0xC0000000u) | (static_cast<u32>(value) << 30);
  }

  /**
   * Returns the free block count of a higher level entry.
   *
   * @return The count.
   */
  [[nodiscard]] u32 levelN_num_blocks_free() const { return info_raw & 0x7FFF; }

  /**
   * Replaces the free block count of a higher level entry.
   *
   * @param value The count, 15 bits.
   */
  void set_levelN_num_blocks_free(u32 value) {
    info_raw = (info_raw & ~0x7FFFu) | (value & 0x7FFF);
  }

  /**
   * Returns the second block count of a higher level entry, meaning unknown.
   *
   * @return The count.
   */
  [[nodiscard]] u32 levelN_num_blocks_unk() const { return (info_raw >> 15) & 0x7FFF; }

  /**
   * Replaces the second block count of a higher level entry.
   *
   * @param value The count, 15 bits.
   */
  void set_levelN_num_blocks_unk(u32 value) {
    info_raw = (info_raw & ~0x3FFF8000u) | ((value & 0x7FFF) << 15);
  }

  /**
   * Reports whether a higher level entry uses its secondary table.
   *
   * @return True when the active index bit is set.
   */
  [[nodiscard]] bool levelN_active_index() const { return (info_raw & 0x40000000) != 0; }

  /**
   * Replaces the active index bit of a higher level entry.
   *
   * @param value True to use the secondary table.
   */
  void set_levelN_active_index(bool value) {
    info_raw = (info_raw & ~0x40000000u) | (value ? 0x40000000u : 0u);
  }

  /**
   * Reports whether a higher level entry is writable.
   *
   * @return True when the writable bit is set.
   */
  [[nodiscard]] bool levelN_writeable() const { return (info_raw & 0x80000000) != 0; }

  /**
   * Replaces the writable bit of a higher level entry.
   *
   * @param value True to mark the entry writable.
   */
  void set_levelN_writeable(bool value) {
    info_raw = (info_raw & ~0x80000000u) | (value ? 0x80000000u : 0u);
  }
};
static_assert(sizeof(STFSHashEntry) == 0x18);

/** 1 STFS hash table block. */
struct STFSHashTable {
  STFSHashEntry entries[170]; /**< Entries for 170 blocks or tables. */
  be<u32> num_blocks;         /**< Level 0 blocks this table covers. */
  u8 padding[12];             /**< Unused. */
};
static_assert(sizeof(STFSHashTable) == 0x1000);

/** 1 STFS directory entry. */
struct STFSDirectoryEntry {
  char name[40]; /**< Entry name, not NUL terminated when 40 characters long. */
  /** Name length and entry kind. */
  struct {
    u8 name_length : 6; /**< Characters of name in use. */
    u8 contiguous : 1;  /**< Data blocks are consecutive. */
    u8 directory : 1;   /**< The entry is a directory. */
  } flags;
  u8 valid_data_blocks_raw[3];     /**< Blocks holding data, 24 bit little endian. */
  u8 allocated_data_blocks_raw[3]; /**< Blocks allocated, 24 bit little endian. */
  u8 start_block_number_raw[3];    /**< First data block, 24 bit little endian. */
  be<u16> directory_index;         /**< Index of the parent entry, 0xFFFF for the root. */
  be<u32> length;                  /**< File size in bytes. */
  be<u16> create_date;             /**< FAT creation date. */
  be<u16> create_time;             /**< FAT creation time. */
  be<u16> modified_date;           /**< FAT modification date. */
  be<u16> modified_time;           /**< FAT modification time. */

  /**
   * Returns the blocks holding data.
   *
   * @return The count.
   */
  [[nodiscard]] constexpr u32 valid_data_blocks() const {
    return load_uint24_le(valid_data_blocks_raw);
  }

  /**
   * Replaces the blocks holding data.
   *
   * @param value The count, 24 bits.
   */
  constexpr void set_valid_data_blocks(u32 value) { store_uint24_le(valid_data_blocks_raw, value); }

  /**
   * Returns the blocks allocated.
   *
   * @return The count.
   */
  [[nodiscard]] constexpr u32 allocated_data_blocks() const {
    return load_uint24_le(allocated_data_blocks_raw);
  }

  /**
   * Replaces the blocks allocated.
   *
   * @param value The count, 24 bits.
   */
  constexpr void set_allocated_data_blocks(u32 value) {
    store_uint24_le(allocated_data_blocks_raw, value);
  }

  /**
   * Returns the first data block.
   *
   * @return The block number.
   */
  [[nodiscard]] constexpr u32 start_block_number() const {
    return load_uint24_le(start_block_number_raw);
  }

  /**
   * Replaces the first data block.
   *
   * @param value The block number, 24 bits.
   */
  constexpr void set_start_block_number(u32 value) {
    store_uint24_le(start_block_number_raw, value);
  }
};
static_assert(sizeof(STFSDirectoryEntry) == 0x40);

/** 1 STFS directory block of 64 entries. */
struct STFSDirectoryBlock {
  STFSDirectoryEntry entries[0x40]; /**< The entries, unused ones have a 0 name length. */
};
static_assert(sizeof(STFSDirectoryBlock) == 0x1000);

/** SVOD device descriptor inside the XContent metadata. */
struct SVODDeviceDescriptor {
  u8 descriptor_length;               /**< Size of this descriptor. */
  u8 block_cache_element_count;       /**< Console block cache size. */
  u8 worker_thread_processor;         /**< Console worker thread processor. */
  u8 worker_thread_priority;          /**< Console worker thread priority. */
  u8 first_fragment_hash_entry[0x14]; /**< SHA1 of the first data fragment. */
  /** Layout features. */
  union {
    u8 as_byte; /**< The features as 1 byte. */
    /** The features as bits. */
    struct {
      u8 must_be_zero_for_future_usage : 6; /**< Reserved. */
      u8 enhanced_gdf_layout : 1;           /**< The EGDF layout with a 0x2000 byte offset. */
      u8 zero_for_downlevel_clients : 1;    /**< Reserved. */
    } bits;
  } features;
  u8 num_data_blocks_raw[3];  /**< Data blocks, 24 bit little endian. */
  u8 start_data_block_raw[3]; /**< First data block, 24 bit little endian. */
  u8 reserved[5];             /**< Unused. */

  /**
   * Returns the data block count.
   *
   * @return The count.
   */
  [[nodiscard]] constexpr u32 num_data_blocks() const {
    return load_uint24_le(num_data_blocks_raw);
  }

  /**
   * Returns the first data block.
   *
   * @return The block number.
   */
  [[nodiscard]] constexpr u32 start_data_block() const {
    return load_uint24_le(start_data_block_raw);
  }
};
static_assert(sizeof(SVODDeviceDescriptor) == 0x24);

/** 1 package license entry. */
struct XContentLicense {
  be<u64> licensee_id;   /**< Profile or console the license binds to. */
  be<u32> license_bits;  /**< License mask. */
  be<u32> license_flags; /**< License flags, 0 for an unused entry. */
};
static_assert(sizeof(XContentLicense) == 0x10);

/** Media metadata of a video package. */
struct XContentMediaData {
  u8 series_id[0x10];     /**< Series GUID. */
  u8 season_id[0x10];     /**< Season GUID. */
  be<u16> season_number;  /**< Season number. */
  be<u16> episode_number; /**< Episode number. */
};
static_assert(sizeof(XContentMediaData) == 0x24);

/** Avatar asset metadata of an avatar item package. */
struct XContentAvatarAssetData {
  be<u32> sub_category;     /**< Asset category. */
  be<u32> colorizable;      /**< Nonzero when the asset takes colors. */
  u8 asset_id[0x10];        /**< Asset GUID. */
  u8 skeleton_version_mask; /**< Skeleton versions the asset supports. */
  u8 reserved[0xB];         /**< Unused. */
};
static_assert(sizeof(XContentAvatarAssetData) == 0x24);

/** Package transfer and feature bits. */
struct XContentAttributes {
  u8 profile_transfer : 1;        /**< The package moves with its profile. */
  u8 device_transfer : 1;         /**< The package moves between devices. */
  u8 move_only_transfer : 1;      /**< The package moves and never copies. */
  u8 kinect_enabled : 1;          /**< The title uses Kinect. */
  u8 disable_network_storage : 1; /**< The package stays off network storage. */
  u8 deep_link_supported : 1;     /**< The package supports deep links. */
  u8 reserved : 2;                /**< Unused. */
};
static_assert(sizeof(XContentAttributes) == 1);

#pragma pack(push, 1)

/** Package metadata after the signed header. */
struct XContentMetadata {
  /** Thumbnail bytes in version 1 metadata. */
  static constexpr u32 kThumbLengthV1 = 0x4000;
  /** Thumbnail bytes in version 2 metadata. */
  static constexpr u32 kThumbLengthV2 = 0x3D00;
  /** Languages of version 1 metadata. */
  static constexpr u32 kNumLanguagesV1 = 9;
  /** Languages of version 2 metadata, 3 of them inside the version 1 thumbnail space. */
  static constexpr u32 kNumLanguagesV2 = 12;

  be<XContentType> content_type;          /**< Content type of the package. */
  be<u32> metadata_version;               /**< 1 or 2. */
  be<u64> content_size;                   /**< Content bytes. */
  xex2_opt_execution_info execution_info; /**< Title and media the package is for. */
  u8 console_id[5];                       /**< Console that created the package. */
  be<u64> profile_id;                     /**< Profile that created the package. */
  /** Volume descriptor, chosen by volume_type. */
  union {
    STFSVolumeDescriptor stfs; /**< STFS layout. */
    SVODDeviceDescriptor svod; /**< SVOD layout. */
  } volume_descriptor;
  be<u32> data_file_count;            /**< SVOD data fragments. */
  be<u64> data_file_size;             /**< SVOD data bytes. */
  be<XContentVolumeType> volume_type; /**< Layout of the volume. */
  be<u64> online_creator;             /**< Live creator XUID. */
  be<u32> category;                   /**< Content category. */
  u8 reserved2[0x20];                 /**< Unused. */
  /** Type specific metadata. */
  union {
    XContentMediaData media_data;              /**< Video packages. */
    XContentAvatarAssetData avatar_asset_data; /**< Avatar item packages. */
  } metadata_v2;
  u8 device_id[0x14]; /**< Device the package was created on. */
  /** Display names, 1 per version 1 language. */
  union {
    be<u16> uint[kNumLanguagesV1][128];   /**< Big endian code units. */
    char16_t chars[kNumLanguagesV1][128]; /**< The same storage as characters. */
  } display_name_raw;
  /** Descriptions, 1 per version 1 language. */
  union {
    be<u16> uint[kNumLanguagesV1][128];   /**< Big endian code units. */
    char16_t chars[kNumLanguagesV1][128]; /**< The same storage as characters. */
  } description_raw;
  /** Publisher name. */
  union {
    be<u16> uint[64];   /**< Big endian code units. */
    char16_t chars[64]; /**< The same storage as characters. */
  } publisher_raw;
  /** Title name. */
  union {
    be<u16> uint[64];   /**< Big endian code units. */
    char16_t chars[64]; /**< The same storage as characters. */
  } title_name_raw;
  /** Transfer and feature bits. */
  union {
    u8 as_byte;              /**< The bits as 1 byte. */
    XContentAttributes bits; /**< The bits by name. */
  } flags;
  be<u32> thumbnail_size;       /**< Bytes of thumbnail in use. */
  be<u32> title_thumbnail_size; /**< Bytes of title_thumbnail in use. */
  u8 thumbnail[kThumbLengthV2]; /**< Package thumbnail, a PNG. */
  /** Display names of the 3 version 2 languages. */
  union {
    be<u16> uint[kNumLanguagesV2 - kNumLanguagesV1][128];   /**< Big endian code units. */
    char16_t chars[kNumLanguagesV2 - kNumLanguagesV1][128]; /**< The same storage as characters. */
  } display_name_ex_raw;
  u8 title_thumbnail[kThumbLengthV2]; /**< Title thumbnail, a PNG. */
  /** Descriptions of the 3 version 2 languages. */
  union {
    be<u16> uint[kNumLanguagesV2 - kNumLanguagesV1][128];   /**< Big endian code units. */
    char16_t chars[kNumLanguagesV2 - kNumLanguagesV1][128]; /**< The same storage as characters. */
  } description_ex_raw;

  /**
   * Returns the display name in a language.
   *
   * @param language The language. A language without a slot reads the English slot.
   * @return The name, empty when the metadata version has no slot for the language.
   */
  [[nodiscard]] REX_API std::u16string display_name(XLanguage language) const;

  /**
   * Returns the description in a language.
   *
   * @param language The language. A language without a slot reads the English slot.
   * @return The description, empty when the metadata version has no slot for the language.
   */
  [[nodiscard]] REX_API std::u16string description(XLanguage language) const;

  /**
   * Returns the publisher name.
   *
   * @return The name.
   */
  [[nodiscard]] REX_API std::u16string publisher() const;

  /**
   * Returns the title name.
   *
   * @return The name.
   */
  [[nodiscard]] REX_API std::u16string title_name() const;

  /**
   * Replaces the display name in a language, truncated to 128 code units.
   *
   * @param language The language. A language without a slot writes the English slot.
   * @param value The new name.
   * @return True when the metadata version has a slot for the language.
   */
  REX_API bool set_display_name(XLanguage language, std::u16string_view value);

  /**
   * Replaces the description in a language, truncated to 128 code units.
   *
   * @param language The language. A language without a slot writes the English slot.
   * @param value The new description.
   * @return True when the metadata version has a slot for the language.
   */
  REX_API bool set_description(XLanguage language, std::u16string_view value);

  /**
   * Replaces the publisher name, truncated to 64 code units.
   *
   * @param value The new name.
   */
  REX_API void set_publisher(std::u16string_view value);

  /**
   * Replaces the title name, truncated to 64 code units.
   *
   * @param value The new name.
   */
  REX_API void set_title_name(std::u16string_view value);
};
static_assert(sizeof(XContentMetadata) == 0x93D6);

/** Signed package header. */
struct XContentHeader {
  be<XContentPackageType> magic;  /**< Package magic. */
  u8 signature[0x228];            /**< Signature over the header. */
  XContentLicense licenses[0x10]; /**< License entries. */
  u8 content_id[0x14];            /**< SHA1 of the metadata, the content ID. */
  be<u32> header_size;            /**< Header bytes including the metadata. */

  /**
   * Reports whether the magic is CON, LIVE or PIRS.
   *
   * @return True when the magic names a package.
   */
  [[nodiscard]] bool is_magic_valid() const {
    return magic == XContentPackageType::kCon || magic == XContentPackageType::kLive ||
           magic == XContentPackageType::kPirs;
  }
};
static_assert(sizeof(XContentHeader) == 0x344);

#pragma pack(pop)

/** Header and metadata at the start of every package. */
struct STFSHeader {
  XContentHeader header;     /**< Signed header. */
  XContentMetadata metadata; /**< Metadata. */
};
static_assert(sizeof(STFSHeader) == 0x971A);

/**
 * Reads the package header from the first bytes of a package.
 *
 * @param bytes At least sizeof(STFSHeader) bytes from the start of the package.
 * @return The header, or kCorrupt when the bytes are short or the magic is not a package.
 */
[[nodiscard]] REX_API Result<STFSHeader> ReadSTFSHeader(std::span<const u8> bytes);

/**
 * Reads the package header of a host file.
 *
 * @param path The package file.
 * @return The header, kNotFound when the file cannot be opened, or kCorrupt as the span form.
 */
[[nodiscard]] REX_API Result<STFSHeader> ReadSTFSHeader(const std::filesystem::path& path);

}  // namespace rex::filesystem
