/**
 * @file        filesystem/info.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>
#include <string>

#include <rex/bit.h>
#include <rex/types.h>

namespace rex::filesystem {

using rex::HasFlags;
using rex::operator|;
using rex::operator&;
using rex::operator^;
using rex::operator~;
using rex::operator|=;
using rex::operator&=;
using rex::operator^=;

/** NT file attribute bits as the kernel writes them into guest structures. */
enum class FileAttributes : u16 {
  kNone = 0x0000,       /**< No attribute. */
  kReadOnly = 0x0001,   /**< Read only. */
  kHidden = 0x0002,     /**< Hidden. */
  kSystem = 0x0004,     /**< System file. */
  kDirectory = 0x0010,  /**< Directory. */
  kArchive = 0x0020,    /**< Archive. */
  kDevice = 0x0040,     /**< Device. */
  kNormal = 0x0080,     /**< Normal file with no other attribute. */
  kTemporary = 0x0100,  /**< Temporary file. */
  kCompressed = 0x0800, /**< Compressed. */
  kEncrypted = 0x4000,  /**< Encrypted. */
};

/** Information about a file or directory. */
struct FileInfo {
  std::string name;                                  /**< Last path component. */
  std::string path;                                  /**< Path relative to the mount. */
  std::string absolute_path;                         /**< Mount path joined with path. */
  FileAttributes attributes = FileAttributes::kNone; /**< Attribute bits. */
  u64 size = 0;                                      /**< Size in bytes. */
  u64 allocation_size = 0;                           /**< Size rounded up to the sector size. */
  u64 create_time = 0;                               /**< Creation time in FILETIME ticks. */
  u64 access_time = 0;                               /**< Last access time in FILETIME ticks. */
  u64 write_time = 0;                                /**< Last write time in FILETIME ticks. */
};

/** Bytes in a sector of a volume as titles see it. */
inline constexpr u32 kBytesPerSector = 0x200;

/** Times to change on a file. An empty field keeps the current time. */
struct FileTimes {
  // NOLINTNEXTLINE(readability-redundant-member-init) designated initializers omit this field
  std::optional<u64> create = {}; /**< New creation time in FILETIME ticks. */
  // NOLINTNEXTLINE(readability-redundant-member-init) designated initializers omit this field
  std::optional<u64> access = {}; /**< New last access time in FILETIME ticks. */
  std::optional<u64> write;       /**< New last write time in FILETIME ticks. */
};

/** Information about the volume a file resides on. */
struct VolumeInfo {
  std::string name;                    /**< File system name titles see, such as STFS. */
  u32 attributes = 0;                  /**< NT file system attribute bits. */
  u32 component_name_max_length = 0;   /**< Longest path component the volume accepts. */
  u32 total_allocation_units = 0;      /**< Allocation units in the volume. */
  u32 available_allocation_units = 0;  /**< Free allocation units. */
  u32 sectors_per_allocation_unit = 0; /**< Sectors per allocation unit. */
  u32 bytes_per_sector = 0;            /**< Bytes per sector. */
};

}  // namespace rex::filesystem

/** Opts FileAttributes into the flag operators. */
template <>
inline constexpr bool rex::is_flags_enum<rex::filesystem::FileAttributes> = true;
