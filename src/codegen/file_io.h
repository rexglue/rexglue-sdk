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
#include <string_view>

#include <rex/platform/file.h>
#include <rex/types.h>

namespace rex::codegen {

enum class WriteOutcome : u8 { kFailed, kUnchanged, kWritten };

/**
 * Writes a file only when its bytes differ, so an unchanged output keeps its mtime and does not
 * retrigger a build.
 *
 * @param path File to write.
 * @param content Bytes to write.
 * @return Unchanged, Written or Failed.
 */
inline WriteOutcome WriteIfChanged(const std::filesystem::path& path, std::string_view content) {
  if (auto existing = rex::platform::ReadHostFile(path);
      existing && std::string_view(reinterpret_cast<const char*>(existing->data()),
                                   existing->size()) == content) {
    return WriteOutcome::kUnchanged;
  }
  return rex::platform::WriteHostFile(path, content) ? WriteOutcome::kWritten
                                                     : WriteOutcome::kFailed;
}

}  // namespace rex::codegen
