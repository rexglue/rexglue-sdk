/**
 * @file        codegen/code_region.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

namespace rex::codegen {

/** Half open address range of code, from start up to but excluding end. */
struct CodeRegion {
  uint32_t start; /**< First address of the range */
  uint32_t end;   /**< Address 1 past the range */

  bool contains(uint32_t addr) const { return addr >= start && addr < end; }
  uint32_t size() const { return end - start; }
};

}  // namespace rex::codegen
