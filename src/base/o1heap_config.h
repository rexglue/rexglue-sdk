/**
 * @file        base/o1heap_config.h
 * @brief       o1heap configuration with a 64 bit CLZ
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#if defined(__clang__) || defined(__GNUC__)
/**
 * Counts leading zeros of a 64 bit value. The __builtin_clzl that o1heap selects under clang-cl is
 * 32 bit on Windows, which corrupts the heap bins.
 */
#define O1HEAP_CLZ(x) ((uint8_t)__builtin_clzll((unsigned long long)(x)))
#elif defined(_MSC_VER)
#include <intrin.h>
static __inline uint8_t o1heap_clz_(size_t x) {
  unsigned long index;
  _BitScanReverse64(&index, (unsigned __int64)x);
  return (uint8_t)(63U - index);
}
#define O1HEAP_CLZ(x) o1heap_clz_(x)
#endif
