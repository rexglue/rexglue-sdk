/**
 * @file        o1heap_config.h
 * @brief       o1heap build configuration that provides correct 64 bit CLZ.
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
 * Counts leading zeros of a size_t wide value.
 *
 * On Windows unsigned long is 32 bits even on x64, so the __builtin_clzl that o1heap.c selects
 * under clang-cl gives a 32 bit count. o1heap needs a 64 bit count, otherwise
 * roundUpToPowerOf2() yields oversized values that corrupt the heap bins on first allocation.
 *
 * @param x The value to count leading zeros of.
 * @return The leading zero count as uint8_t.
 */
#define O1HEAP_CLZ(x) ((uint8_t)__builtin_clzll((unsigned long long)(x)))
#elif defined(_MSC_VER)
#include <intrin.h>
/**
 * Counts leading zeros of a 64 bit value on MSVC.
 *
 * @param x The value to count leading zeros of.
 * @return The leading zero count.
 */
static __inline uint8_t o1heap_clz_(size_t x) {
  unsigned long index;
  _BitScanReverse64(&index, (unsigned __int64)x);
  return (uint8_t)(63U - index);
}
#define O1HEAP_CLZ(x) o1heap_clz_(x)
#endif
