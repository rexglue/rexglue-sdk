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

#include <rex/filesystem/info.h>
#include <rex/types.h>
#include <rex/module.h>

namespace rex::filesystem {

/**
 * Walks the live children of a directory that match a pattern, case insensitive, with the console
 * wildcard rules. A star matches any run, a question mark matches 1 character.
 */
REX_INTERFACE(IDirectory)
 public:
  /**
   * Returns the next matching child.
   *
   * @return The child, or empty when no child is left.
   */
  [[nodiscard]] virtual std::optional<FileStat> Next() = 0;

  /** Starts the walk again from the first child. */
  virtual void Restart() = 0;

  /**
   * Returns the child index after the last returned child.
   *
   * @return The index the kernel writes into FileIndex.
   */
  [[nodiscard]] virtual u64 GetPosition() const = 0;
};

}  // namespace rex::filesystem
