/**
 * @file        filesystem/mount.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <string>
#include <vector>

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

enum class ProviderType : u8 {
  kHostDirectory,
  kSTFS,      /**< An STFS or SVOD package file, detected from the header. */
  kDiscImage, /**< A GDFX disc image. */
  kNull,      /**< Answers a fixed list of paths with empty files. */
};

enum class MountFlags : u8 {
  kNone = 0,     /**< Read only. */
  kWritable = 1, /**< Writes, creation and deletion are allowed. */
};

struct MountEntry {
  std::string path; /**< Guest mount path, such as \\Device\\Cdrom0. */
  ProviderType type = ProviderType::kHostDirectory;
  std::filesystem::path source = {}; /**< Host folder or container file, empty for kNull. */
  MountFlags flags = MountFlags::kNone;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated initializers omit this field
  std::vector<std::string> null_paths = {}; /**< Paths the null provider answers. */
};

}  // namespace rex::filesystem

template <>
inline constexpr bool rex::is_flags_enum<rex::filesystem::MountFlags> = true;
