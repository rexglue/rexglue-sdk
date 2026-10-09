/**
 * @file        core/filesystem/container_parse.h
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

#include <rex/filesystem/container.h>
#include <rex/filesystem/error.h>
#include <rex/filesystem/xcontent.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::filesystem::detail {

/**
 * Copies a trivially copyable value out of a byte span.
 *
 * @tparam T The value type.
 * @param bytes The source.
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

/**
 * Reports whether a byte range lies inside a span.
 *
 * @param bytes The span.
 * @param offset First byte of the range.
 * @param length Bytes in the range.
 * @return True when the whole range is inside bytes.
 */
inline bool InBounds(std::span<const u8> bytes, u64 offset, u64 length) {
  return offset <= bytes.size() && bytes.size() - offset >= length;
}

/**
 * Builds a kCorrupt error.
 *
 * @param message Human readable detail.
 * @return The error.
 */
inline rex::Error Corrupt(std::string message) {
  return MakeError(Error::kCorrupt, std::move(message));
}

/**
 * Parses an STFS package into nodes. nodes holds the root on entry.
 *
 * @param header The package header.
 * @param file The whole package file.
 * @param nodes The node list to fill.
 * @return kCorrupt when a table, a chain or an index is out of range.
 */
[[nodiscard]] Result<void> ParseSTFS(const STFSHeader& header, std::span<const u8> file,
                                     std::vector<ContainerNode>& nodes);

/**
 * Parses an SVOD package into nodes. nodes holds the root on entry.
 *
 * @param header The package header.
 * @param files The data files, the header file alone for a single file package.
 * @param nodes The node list to fill.
 * @return kCorrupt when the magic block is missing or an entry is out of range.
 */
[[nodiscard]] Result<void> ParseSVOD(const STFSHeader& header,
                                     std::span<const std::span<const u8>> files,
                                     std::vector<ContainerNode>& nodes);

/**
 * Reports whether an image holds the GDFX magic at 1 of the candidate offsets.
 *
 * @param image The whole image.
 * @return True when the magic is found.
 */
bool IsGDFX(std::span<const u8> image);

/**
 * Parses a GDFX disc image into nodes. nodes holds the root on entry.
 *
 * @param image The whole image.
 * @param nodes The node list to fill.
 * @return kCorrupt when the magic is missing or an entry is out of range.
 */
[[nodiscard]] Result<void> ParseGDFX(std::span<const u8> image, std::vector<ContainerNode>& nodes);

}  // namespace rex::filesystem::detail
