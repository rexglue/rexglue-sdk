/**
 * @file        codegen/file_io.h
 * @brief       Content-addressed output writing shared by the codegen writers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <rex/platform/file.h>
#include <rex/types.h>

namespace rex::codegen {

inline std::optional<std::string> ReadFileBytes(const std::filesystem::path& path) {
  auto bytes = rex::platform::ReadWholeFile(path);
  if (!bytes)
    return std::nullopt;
  return std::string(bytes->begin(), bytes->end());
}

inline bool WriteFileBytes(const std::filesystem::path& path, std::string_view content) {
  return rex::platform::WriteWholeFile(
             path, std::span(reinterpret_cast<const u8*>(content.data()), content.size()))
      .has_value();
}

enum class WriteOutcome { Failed, Unchanged, Written };

/**
 * Writes a file only when its bytes differ, so an unchanged output keeps its mtime and does not
 * retrigger a build.
 *
 * @param path File to write.
 * @param content Bytes to write.
 * @return Unchanged, Written or Failed.
 */
inline WriteOutcome WriteIfChanged(const std::filesystem::path& path, std::string_view content) {
  if (auto existing = ReadFileBytes(path); existing && *existing == content)
    return WriteOutcome::Unchanged;
  return WriteFileBytes(path, content) ? WriteOutcome::Written : WriteOutcome::Failed;
}

}  // namespace rex::codegen
