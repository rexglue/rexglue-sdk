/**
 * @file        tests/ppc_words.h
 * @brief       Big endian instruction words for codegen tests
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <initializer_list>
#include <vector>

#include <rex/types.h>

namespace rex::test {

/**
 * Encodes instruction words as big endian bytes.
 *
 * @param words The words.
 * @return The bytes.
 */
inline std::vector<u8> Encode(std::initializer_list<u32> words) {
  std::vector<u8> data;
  for (const u32 word : words) {
    data.insert(data.end(), {static_cast<u8>(word >> 24), static_cast<u8>(word >> 16),
                             static_cast<u8>(word >> 8), static_cast<u8>(word)});
  }
  return data;
}

}  // namespace rex::test
