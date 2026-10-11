/**
 * @file        memory/fourcc.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/memory.h (Ben Vanik, 2020).
 */

#pragma once

#include <stdexcept>
#include <string_view>

#include <rex/types.h>

namespace rex::memory {

/** 4 character code packed into 32 bits. */
using fourcc_t = u32;

/**
 * Packs 4 characters into a FourCC in host byte order.
 *
 * @param a First character, the most significant byte.
 * @param b Second character.
 * @param c Third character.
 * @param d Fourth character, the least significant byte.
 * @return The packed code. make_fourcc('a', 'b', 'c', 'd') is 0x61626364.
 */
constexpr fourcc_t make_fourcc(char a, char b, char c, char d) {
  return ((static_cast<fourcc_t>(a) << 24) | (static_cast<fourcc_t>(b) << 16) |
          (static_cast<fourcc_t>(c) << 8) | static_cast<fourcc_t>(d));
}

/**
 * Packs a 4 character string into a FourCC in host byte order.
 *
 * @param fourcc String of exactly 4 characters. Any other length throws std::runtime_error.
 * @return The packed code. make_fourcc("abcd") is 0x61626364.
 */
constexpr fourcc_t make_fourcc(const std::string_view fourcc) {
  if (fourcc.length() != 4) {
    throw std::runtime_error("Invalid fourcc length");
  }
  return make_fourcc(fourcc[0], fourcc[1], fourcc[2], fourcc[3]);
}

}  // namespace rex::memory
