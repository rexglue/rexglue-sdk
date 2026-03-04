/**
 * @file        o1heap_config.h
 * @brief       o1heap build configuration -- provides correct 64-bit CLZ.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

// On Windows, unsigned long is 32 bits even on x64.  o1heap.c auto-selects
// __builtin_clzl under clang-cl (because __clang__ is defined), but that
// gives a 32-bit CLZ while o1heap needs 64-bit (size_t-width).  This causes
// roundUpToPowerOf2() to produce astronomically large values, corrupting the
// heap bin structure on first allocation.
#pragma once

#if defined(__clang__) || defined(__GNUC__)
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
