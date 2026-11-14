/**
 * @file        filesystem/file_open.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

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

/** NT access bits of an open request. */
enum class Access : u32 {
  kNone = 0,
  kReadData = 0x00000001,
  kWriteData = 0x00000002,
  kAppendData = 0x00000004,
  kAllAccess = 0x10000000,
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
  kSuperseded,
  kOpened,
  kCreated,
  kOverwritten,
  kExists,
  kDoesNotExist,
};

struct OpenOptions {
  Disposition disposition = Disposition::kOpen;
  Access access = Access::kReadAccess;
  bool create_directory = false;
  bool require_file = false;
};

}  // namespace rex::filesystem

template <>
inline constexpr bool rex::is_flags_enum<rex::filesystem::Access> = true;
