/**
 * @file        tests/file_text.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <rex/platform/file.h>
#include <rex/types.h>

namespace rex::test {

/**
 * Reads a host file.
 *
 * @return The bytes, empty when the file cannot be read.
 */
inline std::vector<u8> ReadBytes(const std::filesystem::path& path) {
  auto bytes = platform::ReadHostFile(path);
  return bytes ? std::move(*bytes) : std::vector<u8>{};
}

/**
 * Reads a host file as text.
 *
 * @return The text, empty when the file cannot be read.
 */
inline std::string ReadText(const std::filesystem::path& path) {
  const std::vector<u8> bytes = ReadBytes(path);
  return std::string(bytes.begin(), bytes.end());
}

/**
 * Writes a host file, creating the parent folders.
 *
 * @param text The contents.
 */
inline void WriteText(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  if (auto written = platform::WriteHostFile(path, text); !written) {
    throw std::runtime_error(written.error().GetDescription());
  }
}

}  // namespace rex::test
