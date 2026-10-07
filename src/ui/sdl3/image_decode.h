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

// Decodes a PNG (or any stb-supported image) byte buffer to tightly-packed
// R8G8B8A8 pixels. Returns an empty vector on failure; on success fills
// out_width/out_height and returns width*height*4 bytes.
std::vector<uint8_t> DecodeImageRGBA(const uint8_t* data, size_t size, int& out_width,
                                     int& out_height);

}  // namespace rex::ui
