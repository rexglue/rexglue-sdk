/**
 * @file        filesystem/stfs.h
 * @brief       STFS and SVOD package layouts and container reader
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/stfs_xbox.h (Ben Vanik, 2021).
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/stfs_container_device.cc and
 */

#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <rex/platform.h>
#include <rex/assert.h>
#include <rex/math.h>
#include <rex/memory/endian.h>
#include <rex/string/text.h>
#include <rex/result.h>
#include <rex/filesystem/xex.h>
#include <rex/kernel/types/xex.h>
#include <rex/kernel/types/content.h>
#include <rex/types.h>
#include <rex/filesystem/file_stat.h>
#include <rex/utility.h>

namespace rex::filesystem {

/** Package magic, the first 4 bytes of a package. */
enum class XContentPackageType : u32 {
  kCon = 0x434F4E20,  /**< CON, signed by a console. */
  kPirs = 0x50495253, /**< PIRS, signed by Microsoft for offline content. */
  kLive = 0x4C495645, /**< LIVE, signed by Microsoft for Live content. */
};

// NOLINTNEXTLINE(performance-enum-size): the on disk XContent header stores 4 bytes
enum class XContentVolumeType : u32 {
  kSTFS = 0, /**< Secure transacted file system in one file. */
  kSVOD = 1, /**< Secure virtual optical disc, a header file and data fragments. */
};

inline constexpr u32 kSTFSBlockSize = 0x1000;

inline constexpr u32 kSTFSMaxBlocks = 4913000;

/** Blocks covered by one hash table at each of the three hash levels. */
inline constexpr std::array<u32, 3> kSTFSBlocksPerHashLevel = {170, 28900, kSTFSMaxBlocks};

#pragma pack(push, 1)

/** STFS volume descriptor inside the XContent metadata. */
struct STFSVolumeDescriptor {
  u8 descriptor_length; /**< Size of this descriptor, 0x24 when valid. */
  u8 version;
  union {
    u8 as_byte;
    struct {
      u8 read_only_format : 1;             /**< One backing block per hash table instead of two. */
      u8 root_active_index : 1;            /**< Top hash table uses the secondary block. */
      u8 directory_overallocated : 1;      /**< Directory blocks exceed the entries. */
      u8 directory_index_bounds_valid : 1; /**< Directory indices were bounds checked. */
    } bits;
  } flags;
  u16 file_table_block_count;        /**< Number of directory blocks, little endian. */
  u8 file_table_block_number_raw[3]; /**< First directory block, 24 bit little endian. */
  u8 top_hash_table_hash[0x14];      /**< SHA1 of the top hash table. */
  be_u32 total_block_count;
  be_u32 free_block_count;

  [[nodiscard]] constexpr u32 GetFileTableBlockNumber() const {
    return memory::LoadUint24LE(file_table_block_number_raw);
  }

  constexpr void SetFileTableBlockNumber(u32 value) {
    memory::StoreUint24LE(file_table_block_number_raw, value);
  }

  [[nodiscard]] constexpr bool IsValid() const {
    return descriptor_length == sizeof(STFSVolumeDescriptor);
  }
};
REX_STATIC_ASSERT_SIZE(STFSVolumeDescriptor, 0x24);

#pragma pack(pop)

enum class STFSHashState : u8 {
  kFree = 0,        /**< Unallocated and absent from the package. */
  kFreePresent = 1, /**< Unallocated and present in the package. */
  kInUse = 2,
};

/** A hash table entry, the hash and chain information of a block. */
struct STFSHashEntry {
  u8 sha1[0x14];
  be_u32 info_raw; /**< Packed chain and allocation fields. */

  /** Returns the next block of a level 0 chain, 0xFFFFFF at the end of the chain. */
  [[nodiscard]] u32 GetLevel0NextBlock() const { return info_raw & 0xFFFFFF; }

  void SetLevel0NextBlock(u32 value) { info_raw = (info_raw & ~0xFFFFFFu) | (value & 0xFFFFFF); }

  [[nodiscard]] STFSHashState GetLevel0AllocationState() const {
    return static_cast<STFSHashState>(static_cast<u8>((info_raw >> 30) & 0x3));
  }

  void SetLevel0AllocationState(STFSHashState value) {
    info_raw = (info_raw & ~0xC0000000u) | (static_cast<u32>(value) << 30);
  }

  [[nodiscard]] u32 GetLevelNFreeBlockCount() const { return info_raw & 0x7FFF; }

  void SetLevelNNumBlocksFree(u32 value) { info_raw = (info_raw & ~0x7FFFu) | (value & 0x7FFF); }

  [[nodiscard]] u32 GetLevelNNumBlocksUnk() const { return (info_raw >> 15) & 0x7FFF; }

  void SetLevelNNumBlocksUnk(u32 value) {
    info_raw = (info_raw & ~0x3FFF8000u) | ((value & 0x7FFF) << 15);
  }

  /**
   * Reports whether a higher level entry uses its secondary table.
   *
   * @return True when the active index bit is set.
   */
  [[nodiscard]] bool IsLevelNActiveIndexSet() const { return (info_raw & 0x40000000) != 0; }

  void SetLevelNActiveIndex(bool value) {
    info_raw = (info_raw & ~0x40000000u) | (value ? 0x40000000u : 0u);
  }

  [[nodiscard]] bool IsLevelNWritable() const { return (info_raw & 0x80000000) != 0; }

  void SetLevelNWriteable(bool value) {
    info_raw = (info_raw & ~0x80000000u) | (value ? 0x80000000u : 0u);
  }
};
REX_STATIC_ASSERT_SIZE(STFSHashEntry, 0x18);

struct STFSHashTable {
  STFSHashEntry entries[170]; /**< Entries for 170 blocks or tables. */
  be_u32 block_count;         /**< Level 0 blocks this table covers. */
  u8 padding[12];
};
REX_STATIC_ASSERT_SIZE(STFSHashTable, 0x1000);

struct STFSDirectoryEntry {
  char name[40]; /**< Entry name, not NUL terminated when 40 characters long. */
  struct {
    u8 name_length : 6;
    u8 contiguous : 1;
    u8 directory : 1;
  } flags;                         /**< Name length and entry type. */
  u8 valid_data_blocks_raw[3];     /**< Blocks holding data, 24 bit little endian. */
  u8 allocated_data_blocks_raw[3]; /**< Blocks allocated, 24 bit little endian. */
  u8 start_block_number_raw[3];    /**< First data block, 24 bit little endian. */
  be_u16 directory_index;          /**< Index of the parent entry, 0xFFFF for the root. */
  be_u32 length;
  be_u16 create_date;   /**< FAT creation date. */
  be_u16 create_time;   /**< FAT creation time. */
  be_u16 modified_date; /**< FAT modification date. */
  be_u16 modified_time; /**< FAT modification time. */

  [[nodiscard]] constexpr u32 GetValidDataBlocks() const {
    return memory::LoadUint24LE(valid_data_blocks_raw);
  }

  constexpr void SetValidDataBlocks(u32 value) {
    memory::StoreUint24LE(valid_data_blocks_raw, value);
  }

  [[nodiscard]] constexpr u32 GetAllocatedDataBlocks() const {
    return memory::LoadUint24LE(allocated_data_blocks_raw);
  }

  constexpr void SetAllocatedDataBlocks(u32 value) {
    memory::StoreUint24LE(allocated_data_blocks_raw, value);
  }

  [[nodiscard]] constexpr u32 GetStartBlockNumber() const {
    return memory::LoadUint24LE(start_block_number_raw);
  }

  constexpr void SetStartBlockNumber(u32 value) {
    memory::StoreUint24LE(start_block_number_raw, value);
  }
};
REX_STATIC_ASSERT_SIZE(STFSDirectoryEntry, 0x40);

/** Holds 64 entries. */
struct STFSDirectoryBlock {
  STFSDirectoryEntry entries[0x40]; /**< The entries, unused ones have a 0 name length. */
};
REX_STATIC_ASSERT_SIZE(STFSDirectoryBlock, 0x1000);

/** SVOD device descriptor inside the XContent metadata. */
struct SVODDeviceDescriptor {
  u8 descriptor_length;
  u8 block_cache_element_count; /**< Console block cache size. */
  u8 worker_thread_processor;   /**< Processor the console worker thread runs on. */
  u8 worker_thread_priority;
  u8 first_fragment_hash_entry[0x14]; /**< SHA1 of the first data fragment. */
  union {
    u8 as_byte;
    struct {
      u8 must_be_zero_for_future_usage : 6;
      u8 enhanced_gdf_layout : 1; /**< The EGDF layout with a 0x2000 byte offset. */
      u8 zero_for_downlevel_clients : 1;
    } bits;
  } features;
  u8 data_block_count_raw[3]; /**< Data blocks, 24 bit little endian. */
  u8 start_data_block_raw[3]; /**< First data block, 24 bit little endian. */
  u8 reserved[5];

  [[nodiscard]] constexpr u32 GetDataBlockCount() const {
    return memory::LoadUint24LE(data_block_count_raw);
  }

  [[nodiscard]] constexpr u32 GetStartDataBlock() const {
    return memory::LoadUint24LE(start_data_block_raw);
  }
};
REX_STATIC_ASSERT_SIZE(SVODDeviceDescriptor, 0x24);

struct XContentLicense {
  be_u64 licensee_id; /**< Profile or console the license binds to. */
  be_u32 license_bits;
  be_u32 license_flags; /**< License flags, 0 for an unused entry. */
};
REX_STATIC_ASSERT_SIZE(XContentLicense, 0x10);

/** Media metadata of a video package. */
struct XContentMedia {
  u8 series_id[0x10];
  u8 season_id[0x10];
  be_u16 season_number;
  be_u16 episode_number;
};
REX_STATIC_ASSERT_SIZE(XContentMedia, 0x24);

/** Avatar asset metadata of an avatar item package. */
struct XContentAvatarAsset {
  be_u32 sub_category;
  be_u32 colorizable; /**< Nonzero when the asset takes colors. */
  u8 asset_id[0x10];
  u8 skeleton_version_mask; /**< Skeleton versions the asset supports. */
  u8 reserved[0xB];
};
REX_STATIC_ASSERT_SIZE(XContentAvatarAsset, 0x24);

struct XContentAttributes {
  u8 profile_transfer : 1;
  u8 device_transfer : 1;
  u8 move_only_transfer : 1; /**< The package moves and never copies. */
  u8 kinect_enabled : 1;
  u8 disable_network_storage : 1;
  u8 deep_link_supported : 1;
  u8 reserved : 2;
};
REX_STATIC_ASSERT_SIZE(XContentAttributes, 1);

#pragma pack(push, 1)

/** Package metadata after the signed header. */
struct XContentMetadata {
  static constexpr u32 kThumbLengthV1 = 0x4000;
  static constexpr u32 kThumbLengthV2 = 0x3D00;
  static constexpr u32 kNumLanguagesV1 = 9;
  /** Languages of version 2 metadata, three of them inside the version 1 thumbnail space. */
  static constexpr u32 kNumLanguagesV2 = 12;

  BE<kernel::XContentType> content_type;
  be_u32 metadata_version; /**< Either 1 or 2. */
  be_u64 content_size;
  kernel::XEXOptExecutionInfo execution_info; /**< Title and media the package is for. */
  u8 console_id[5];
  be_u64 profile_id;
  union {
    STFSVolumeDescriptor stfs;
    SVODDeviceDescriptor svod;
  } volume_descriptor;    /**< Volume descriptor, chosen by volume_type. */
  be_u32 data_file_count; /**< SVOD data fragments. */
  be_u64 data_file_size;
  BE<XContentVolumeType> volume_type;
  be_u64 online_creator; /**< Live creator XUID. */
  be_u32 category;
  u8 reserved2[0x20];
  union {
    XContentMedia media_data;              /**< Video packages. */
    XContentAvatarAsset avatar_asset_data; /**< Avatar item packages. */
  } metadata_v2;                           /**< Type specific metadata. */
  u8 device_id[0x14];
  union {
    be_u16 uint[kNumLanguagesV1][128];    /**< Big endian code units. */
    char16_t chars[kNumLanguagesV1][128]; /**< The same storage as characters. */
  } display_name_raw;                     /**< Display names, one per version 1 language. */
  union {
    be_u16 uint[kNumLanguagesV1][128];    /**< Big endian code units. */
    char16_t chars[kNumLanguagesV1][128]; /**< The same storage as characters. */
  } description_raw;                      /**< Descriptions, one per version 1 language. */
  union {
    be_u16 uint[64];    /**< Big endian code units. */
    char16_t chars[64]; /**< The same storage as characters. */
  } publisher_raw;
  union {
    be_u16 uint[64];    /**< Big endian code units. */
    char16_t chars[64]; /**< The same storage as characters. */
  } title_name_raw;
  union {
    u8 as_byte;
    XContentAttributes bits;
  } flags;
  be_u32 thumbnail_size;        /**< Bytes of thumbnail in use. */
  be_u32 title_thumbnail_size;  /**< Bytes of title_thumbnail in use. */
  u8 thumbnail[kThumbLengthV2]; /**< Package thumbnail, a PNG. */
  union {
    be_u16 uint[kNumLanguagesV2 - kNumLanguagesV1][128];    /**< Big endian code units. */
    char16_t chars[kNumLanguagesV2 - kNumLanguagesV1][128]; /**< The same storage as characters. */
  } display_name_ex_raw;              /**< Display names of the three version 2 languages. */
  u8 title_thumbnail[kThumbLengthV2]; /**< Title thumbnail, a PNG. */
  union {
    be_u16 uint[kNumLanguagesV2 - kNumLanguagesV1][128];    /**< Big endian code units. */
    char16_t chars[kNumLanguagesV2 - kNumLanguagesV1][128]; /**< The same storage as characters. */
  } description_ex_raw; /**< Descriptions of the three version 2 languages. */

  /**
   * Returns the display name for a language, reading the English slot when it has none, empty when
   * the metadata version has no slot.
   */
  [[nodiscard]] std::u16string GetDisplayName(kernel::XLanguage language) const;

  /**
   * Returns the description for a language, reading the English slot when it has none, empty when
   * the metadata version has no slot.
   */
  [[nodiscard]] std::u16string GetDescription(kernel::XLanguage language) const;

  [[nodiscard]] std::u16string GetPublisher() const;

  [[nodiscard]] std::u16string GetTitleName() const;

  /**
   * Replaces the display name for a language, truncated to 128 code units.
   *
   * @return True when written, false when the language has no slot.
   */
  bool SetDisplayName(kernel::XLanguage language, std::u16string_view value);

  /**
   * Replaces the description for a language, truncated to 128 code units.
   *
   * @return True when written, false when the language has no slot.
   */
  bool SetDescription(kernel::XLanguage language, std::u16string_view value);

  /** Replaces the publisher name, truncated to 64 code units. */
  void SetPublisher(std::u16string_view value);

  /** Replaces the title name, truncated to 64 code units. */
  void SetTitleName(std::u16string_view value);
};
REX_STATIC_ASSERT_SIZE(XContentMetadata, 0x93D6);

namespace detail {

inline std::u16string ReadBigEndianUnits(const be_u16* units, size_t count) {
  std::u16string result;
  for (size_t i = 0; i < count && units[i] != 0; ++i) {
    result.push_back(static_cast<char16_t>(static_cast<u16>(units[i])));
  }
  return result;
}

inline std::optional<u32> FindLanguageSlot(kernel::XLanguage language) {
  if (language == kernel::XLanguage::kSChinese) {
    language = kernel::XLanguage::kSChineseDeprecated;
  }
  const u32 slot = static_cast<u32>(language) - 1;
  if (language == kernel::XLanguage::kInvalid || slot >= XContentMetadata::kNumLanguagesV2) {
    return std::nullopt;
  }
  return slot;
}

inline u32 LanguageSlot(kernel::XLanguage language) {
  return FindLanguageSlot(language).value_or(std::to_underlying(kernel::XLanguage::kEnglish) - 1);
}

}  // namespace detail

inline std::u16string XContentMetadata::GetDisplayName(kernel::XLanguage language) const {
  const u32 slot = detail::LanguageSlot(language);
  if (slot < kNumLanguagesV1) {
    return detail::ReadBigEndianUnits(display_name_raw.uint[slot], 128);
  }
  if (metadata_version >= 2) {
    return detail::ReadBigEndianUnits(display_name_ex_raw.uint[slot - kNumLanguagesV1], 128);
  }
  return {};
}

inline std::u16string XContentMetadata::GetDescription(kernel::XLanguage language) const {
  const u32 slot = detail::LanguageSlot(language);
  if (slot < kNumLanguagesV1) {
    return detail::ReadBigEndianUnits(description_raw.uint[slot], 128);
  }
  if (metadata_version >= 2) {
    return detail::ReadBigEndianUnits(description_ex_raw.uint[slot - kNumLanguagesV1], 128);
  }
  return {};
}

inline std::u16string XContentMetadata::GetPublisher() const {
  return detail::ReadBigEndianUnits(publisher_raw.uint, 64);
}

inline std::u16string XContentMetadata::GetTitleName() const {
  return detail::ReadBigEndianUnits(title_name_raw.uint, 64);
}

inline bool XContentMetadata::SetDisplayName(kernel::XLanguage language,
                                             std::u16string_view value) {
  const std::optional<u32> found = detail::FindLanguageSlot(language);
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
  string::CopyAndSwapTruncating(destination, value, CountOf(display_name_raw.chars[0]));
  return true;
}

inline bool XContentMetadata::SetDescription(kernel::XLanguage language,
                                             std::u16string_view value) {
  const std::optional<u32> found = detail::FindLanguageSlot(language);
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
  string::CopyAndSwapTruncating(destination, value, CountOf(description_raw.chars[0]));
  return true;
}

inline void XContentMetadata::SetPublisher(std::u16string_view value) {
  string::CopyAndSwapTruncating(publisher_raw.chars, value, CountOf(publisher_raw.chars));
}

inline void XContentMetadata::SetTitleName(std::u16string_view value) {
  string::CopyAndSwapTruncating(title_name_raw.chars, value, CountOf(title_name_raw.chars));
}

struct XContentHeader {
  BE<XContentPackageType> magic;
  u8 signature[0x228];
  XContentLicense licenses[0x10];
  u8 content_id[0x14]; /**< SHA1 of the metadata, the content ID. */
  be_u32 header_size;  /**< Header bytes including the metadata. */

  /** Reports whether the magic is CON, LIVE or PIRS. */
  [[nodiscard]] bool IsMagicValid() const {
    const XContentPackageType type = memory::load<BE<XContentPackageType>>(&magic);
    return type == XContentPackageType::kCon || type == XContentPackageType::kLive ||
           type == XContentPackageType::kPirs;
  }
};
REX_STATIC_ASSERT_SIZE(XContentHeader, 0x344);

#pragma pack(pop)

/** Header and metadata at the start of every package. */
struct XContentPackageHeader {
  XContentHeader header;
  XContentMetadata metadata;
};
REX_STATIC_ASSERT_SIZE(XContentPackageHeader, 0x971A);

}  // namespace rex::filesystem

namespace rex::filesystem {

enum class ContainerType : u8 {
  kSTFS, /**< STFS package in one file. */
  kSVOD, /**< SVOD package, a header file and data fragments. */
  kGDFX,
};

/** A contiguous run of file bytes inside a container data file. */
struct ContainerExtent {
  u32 file = 0;
  u64 offset = 0;
  u64 length = 0;
};

struct ContainerNode {
  /** Name (empty for the root), attributes, sizes and times. The path fields stay empty. */
  FileStat info;
  u32 parent = 0;                       /**< Index of the parent, 0 for the root. */
  std::vector<u32> children;            /**< Indices of the children in order. */
  std::vector<ContainerExtent> extents; /**< File bytes in order, empty for a directory. */
};

/** A mapped and parsed container image. Open one with IFileSystemFactory. */
class IContainer {
 public:
  virtual ~IContainer() = default;

  [[nodiscard]] virtual ContainerType GetType() const = 0;

  /**
   * @return The header, null for a GDFX image.
   */
  [[nodiscard]] virtual const XContentPackageHeader* GetHeader() const = 0;

  [[nodiscard]] virtual const std::filesystem::path& GetPath() const = 0;

  /** Returns the size of all data files together. */
  [[nodiscard]] virtual u64 GetTotalSize() const = 0;

  /**
   * @return The nodes, the root first.
   */
  [[nodiscard]] virtual std::span<const ContainerNode> GetNodes() const = 0;

  [[nodiscard]] virtual const ContainerNode& GetRoot() const = 0;

  /**
   * Returns the node at a path, or null when absent.
   *
   * @param path Case insensitive, split on backslash or slash, empty for the root.
   */
  [[nodiscard]] virtual const ContainerNode* Find(std::string_view path) const = 0;

  /**
   * @return The path with backslashes, empty for the root.
   */
  [[nodiscard]] virtual std::string GetNodePath(const ContainerNode& node) const = 0;

  /**
   * Copies bytes of a file node.
   *
   * @param offset Byte offset in the file.
   * @return The bytes copied, or kEndOfFile at or past the end of the file.
   */
  [[nodiscard]] virtual Result<size_t> Read(const ContainerNode& node, u64 offset,
                                            std::span<u8> buffer) const = 0;

  /** Returns the bytes of a file node without a copy, empty when the file is not one extent. */
  [[nodiscard]] virtual std::span<const u8> View(const ContainerNode& node) const = 0;

  /**
   * Writes every node under a host folder, removing the output when a step fails.
   *
   * @param destination The host folder, created when missing.
   * @return kInvalidPath for an unsafe node name, kIO for a host failure.
   */
  [[nodiscard]] virtual Result<void> Extract(const std::filesystem::path& destination) const = 0;
};

}  // namespace rex::filesystem
