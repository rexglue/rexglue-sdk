/**
 * @file        filesystem/open.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>

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

class IFile;

/** NT access bits of an open request. */
enum class Access : u32 {
  kNone = 0,                   /**< No access. */
  kReadData = 0x00000001,      /**< Read the file bytes. */
  kWriteData = 0x00000002,     /**< Write the file bytes. */
  kAppendData = 0x00000004,    /**< Append to the file. */
  kAllAccess = 0x10000000,     /**< Read and write. */
  kExecuteAccess = 0x20000000, /**< Execute, no data access. */
  kWriteAccess = 0x40000000,   /**< Write, implies kWriteData. */
  kReadAccess = 0x80000000,    /**< Read, implies kReadData. */
};

/** What an open does when the path exists or is absent. NT numbering. */
enum class Disposition : u8 {
  kSupersede,   /**< Replace when present, create when absent. */
  kOpen,        /**< Open when present, fail when absent. */
  kCreate,      /**< Fail when present, create when absent. */
  kOpenIf,      /**< Open when present, create when absent. */
  kOverwrite,   /**< Truncate when present, fail when absent. */
  kOverwriteIf, /**< Truncate when present, create when absent. */
};

/** What an open did. NT numbering. */
enum class OpenAction : u8 {
  kSuperseded,   /**< The file was replaced. */
  kOpened,       /**< The file was opened. */
  kCreated,      /**< The file was created. */
  kOverwritten,  /**< The file was truncated. */
  kExists,       /**< The open failed because the file exists. */
  kDoesNotExist, /**< The open failed for any other reason. */
};

/** Options of IFileSystem::Open. */
struct OpenOptions {
  Disposition disposition = Disposition::kOpen; /**< Present or absent rule. */
  Access access = Access::kReadAccess;          /**< Requested access. */
  bool create_directory = false;                /**< True when a created entry is a directory. */
  bool require_file = false; /**< True when the path must not name a directory. */
};

/** Result of a successful open. */
struct Opened {
  std::unique_ptr<IFile> file;             /**< The open file. */
  OpenAction action = OpenAction::kOpened; /**< What the open did. */
};

}  // namespace rex::filesystem

/** Opts Access into the flag operators. */
template <>
inline constexpr bool rex::is_flags_enum<rex::filesystem::Access> = true;
