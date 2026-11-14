/**
 * @file        filesystem/container_parse.h
 * @brief       Container parsers behind Container::Open
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rex/filesystem/stfs.h>
#include <rex/filesystem/file_system_error.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::filesystem::detail {

/**
 * Copies a value out of a byte span.
 *
 * @param offset Byte offset of the value.
 * @return The value, or empty when it does not fit inside bytes.
 */
template <typename T>
std::optional<T> ReadAt(std::span<const u8> bytes, u64 offset) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
    return std::nullopt;
  }
  T value;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

inline bool IsInBounds(std::span<const u8> bytes, u64 offset, u64 length) {
  return offset <= bytes.size() && bytes.size() - offset >= length;
}

inline rex::Error Corrupt(std::string message) {
  return BuildError(FileSystemError::kCorrupt, std::move(message));
}

/**
 * Parses an STFS package into nodes. nodes holds the root on entry.
 *
 * @param header The package header.
 * @param file The whole package file.
 * @param nodes The node list to fill.
 * @return kCorrupt when a table, a chain or an index is out of range.
 */
[[nodiscard]] Result<void> ParseSTFS(const XContentPackageHeader& header, std::span<const u8> file,
                                     std::vector<ContainerNode>& nodes);

/**
 * Parses an SVOD package into nodes. nodes holds the root on entry.
 *
 * @param header The package header.
 * @param files Data files. A single file package has only the header file.
 * @param nodes The node list to fill.
 * @return kCorrupt when the magic block is missing or an entry is out of range.
 */
[[nodiscard]] Result<void> ParseSVOD(const XContentPackageHeader& header,
                                     std::span<const std::span<const u8>> files,
                                     std::vector<ContainerNode>& nodes);

/** Reports whether an image holds the GDFX magic at one of the candidate offsets. */
bool IsGDFX(std::span<const u8> image);

/**
 * Parses a GDFX disc image into nodes. nodes holds the root on entry.
 *
 * @param nodes The node list to fill.
 * @return kCorrupt when the magic is missing or an entry is out of range.
 */
[[nodiscard]] Result<void> ParseGDFX(std::span<const u8> image, std::vector<ContainerNode>& nodes);

}  // namespace rex::filesystem::detail
