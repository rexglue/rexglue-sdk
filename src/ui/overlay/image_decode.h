/**
 * @file        ui/overlay/image_decode.h
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

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::ui {

/**
 * Decodes a PNG to tightly packed R8G8B8A8 pixels.
 *
 * @param out_width Receives the width in pixels on success.
 * @param out_height Receives the height in pixels on success.
 * @return width * height * 4 bytes on success, an empty vector on failure.
 */
std::vector<u8> DecodeImageRGBA(const u8* data, size_t size, int& out_width, int& out_height);

}  // namespace rex::ui
