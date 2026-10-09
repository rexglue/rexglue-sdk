/**
 * @file        embedded_metadata.h
 * @brief       Process-wide embedded metadata asset registry.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include <rex/macros.h>

namespace rex {

/** An embedded metadata asset found by path. */
struct EmbeddedMetadataAsset {
  /** Bytes of the asset, valid for the life of the process. */
  std::span<const u8> bytes;
};

/**
 * Registers a metadata asset embedded into the host executable.
 *
 * An empty path, null data, zero size or a path that normalizes to nothing is rejected.
 *
 * @param relative_path Path relative to the metadata root, such as "icons/custom/67.png".
 * @param data The asset bytes. Must stay valid for the life of the process.
 * @param size The asset size in bytes.
 * @return True when the asset was registered.
 */
REX_RUNTIME_API bool RegisterEmbeddedMetadataAsset(std::string_view relative_path, const u8* data,
                                                   std::size_t size);

/**
 * Looks up an embedded metadata asset. The path is normalized as in
 * `RegisterEmbeddedMetadataAsset`.
 *
 * @param relative_path Path relative to the metadata root.
 * @return The asset, or an empty optional when none is registered at the path.
 */
REX_RUNTIME_API std::optional<EmbeddedMetadataAsset> FindEmbeddedMetadataAsset(
    const std::filesystem::path& relative_path);

}  // namespace rex
