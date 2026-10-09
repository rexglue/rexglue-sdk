/**
 * @file        rexglue/ui/glyphs.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <string_view>

namespace rexglue::ui::glyphs {

/** Check mark, U+2713. */
inline constexpr std::string_view kCheckMark = "\xE2\x9C\x93";

/** Braille spinner frames. */
inline constexpr std::array<std::string_view, 10> kSpinnerFrames = {
    "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC",
    "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F",
};

}  // namespace rexglue::ui::glyphs
