/**
 * @file        base/utf8_helper.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <rex/string/text.h>

namespace rex::string {

/** Encodes a code point as UTF-8. */
std::string ToString(char32_t c);

std::pair<CodePointIterator, CodePointIterator> BuildIterators(const std::string_view view);

/** Returns code point iterators over the range between two iterators. */
std::pair<CodePointIterator, CodePointIterator> BuildIterators(const CodePointIterator begin,
                                                               const CodePointIterator end);

size_t ByteLength(CodePointIterator begin, CodePointIterator end);

bool StartsWithCodePoint(const std::string_view haystack, char32_t needle);

}  // namespace rex::string
