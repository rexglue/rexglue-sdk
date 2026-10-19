/**
 * @file        rexglue/commands/project_scan.h
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
#include <string_view>
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

std::string RenderReXGlueCMake(std::string_view project_name, std::string_view sdk_version,
                               std::string_view entrypoint_out_dir);

/**
 * Rewrites generated/rexglue.cmake when it differs from what the installed SDK renders.
 *
 * @param project_root Directory holding generated/rexglue.cmake.
 * @param project_name Name rendered into rexglue.cmake.
 * @param sdk_version Installed SDK version.
 * @param entrypoint_out_dir Output directory of the entry binary.
 * @return True when the file was rewritten.
 */
bool RefreshGeneratedCMake(const std::filesystem::path& project_root, std::string_view project_name,
                           std::string_view sdk_version, std::string_view entrypoint_out_dir);

/**
 * Finds source files that include headers the run no longer emits.
 *
 * @param src_dir Source directory to scan.
 * @param removed_basenames Names of the headers no longer emitted.
 * @return One warning per stale include.
 */
std::vector<SourceWarning> ScanStaleIncludes(
    const std::filesystem::path& src_dir, const std::unordered_set<std::string>& removed_basenames);

}  // namespace rexglue::cli
