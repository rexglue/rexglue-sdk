/**
 * @file        filesystem/directory.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>

#include <rex/filesystem/file_stat.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::filesystem {

/** Walks the children of a directory that match a case insensitive wildcard pattern. */
class IDirectory : public NonCopyable {
 public:
  virtual ~IDirectory() = default;

  /** Returns the next matching child, empty at the end. */
  [[nodiscard]] virtual std::optional<FileStat> Next() = 0;

  virtual void Restart() = 0;

  /** Returns the index after the last returned child. */
  [[nodiscard]] virtual u64 GetPosition() const = 0;
};

}  // namespace rex::filesystem
