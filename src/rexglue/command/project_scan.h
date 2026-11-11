/**
 * @file        rexglue/command/project_scan.h
 * @brief       Project tree checks that run on every codegen
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace rexglue::cli {

/** A source line that needs review. */
struct SourceWarning {
  std::filesystem::path file;
  std::size_t line_number;
  std::string detail;
  std::string hint;
};

/**
 * Finds source files that include headers the run no longer emits.
 *
 * @param removed_basenames Names of the headers no longer emitted.
 * @return One warning per stale include.
 */
std::vector<SourceWarning> ScanStaleIncludes(
    const std::filesystem::path& src_dir, const std::unordered_set<std::string>& removed_basenames);

}  // namespace rexglue::cli
