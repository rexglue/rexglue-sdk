/**
 * @file        filesystem/mount.h
 * @brief       Provider kinds, mount flags and the description of 1 mount
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

/** The provider that serves a mount. */
enum class ProviderKind : u32 {
  kHostDirectory, /**< A host folder. */
  kSTFS,          /**< An STFS or SVOD package file, told apart by the header. */
  kDiscImage,     /**< A GDFX disc image. */
  kNull,          /**< Answers a fixed list of paths with empty files. */
};

/** Mount behavior bits. */
enum class MountFlags : u32 {
  kNone = 0,     /**< Read only. */
  kWritable = 1, /**< Writes, creation and deletion are allowed. */
};

/** Description of 1 mount. */
struct MountEntry {
  std::string path; /**< Guest mount path, such as \\Device\\Cdrom0. */
  ProviderKind kind = ProviderKind::kHostDirectory; /**< The provider. */
  std::filesystem::path source;         /**< Host folder or container file, empty for kNull. */
  MountFlags flags = MountFlags::kNone; /**< Behavior bits. */
  std::vector<std::string> null_paths;  /**< Paths the null provider answers. */
};

}  // namespace rex::filesystem

/** Opts MountFlags into the flag operators. */
template <>
// NOLINTNEXTLINE(readability-identifier-naming): charter 12.1 names the trait
inline constexpr bool rex::is_flags_enum<rex::filesystem::MountFlags> = true;
