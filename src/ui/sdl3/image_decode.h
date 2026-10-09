/**
 * @file        ui/sdl3/image_decode.h
 * @brief       Minimal host-side PNG decode helper (wraps stb_image).
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <cstdint>
#include <vector>

#include <rex/macros.h>

namespace rex::ui {

/**
 * Decodes a PNG or any stb supported image byte buffer to tightly packed R8G8B8A8 pixels.
 *
 * @param data Encoded image bytes.
 * @param size Size of data in bytes.
 * @param out_width Receives the width in pixels on success.
 * @param out_height Receives the height in pixels on success.
 * @return width * height * 4 bytes on success, an empty vector on failure.
 */
std::vector<u8> DecodeImageRGBA(const u8* data, size_t size, int& out_width, int& out_height);

}  // namespace rex::ui
