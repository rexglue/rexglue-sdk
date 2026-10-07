/**
 * @file        rex/embedded_metadata.h
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

struct EmbeddedMetadataAsset {
  std::span<const std::uint8_t> bytes;
};

// Registers a metadata asset embedded into the host executable. Paths are
// relative to the metadata root, for example "icons/custom/67.png".
REX_RUNTIME_API bool RegisterEmbeddedMetadataAsset(std::string_view relative_path,
                                                   const std::uint8_t* data, std::size_t size);

REX_RUNTIME_API std::optional<EmbeddedMetadataAsset> FindEmbeddedMetadataAsset(
    const std::filesystem::path& relative_path);

}  // namespace rex
