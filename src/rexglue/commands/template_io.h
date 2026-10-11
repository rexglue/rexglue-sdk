/**
 * @file        rexglue/commands/template_io.h
 * @brief       File reads and writes for the init command templates
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>

#include <rex/logging.h>
#include <rex/platform/file.h>
#include <rex/types.h>

namespace rexglue::cli {

inline bool write_file_atomic(const std::filesystem::path& path, const std::string& content) {
  auto written = rex::platform::WriteWholeFile(
      path, std::span(reinterpret_cast<const u8*>(content.data()), content.size()));
  if (!written) {
    REX_FAIL(rexglue, false, "Failed to write {}: {}", path.string(), written.error().message);
  }
  return true;
}

inline std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace rexglue::cli
