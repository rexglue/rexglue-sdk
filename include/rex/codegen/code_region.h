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

#include <rex/types.h>

namespace rex::codegen {

/** Half open address range of code, from start up to but excluding end. */
struct CodeRegion {
  u32 start; /**< First address of the range */
  u32 end;   /**< Address 1 past the range */

  /**
   * Reports whether an address is inside the range.
   *
   * @param addr Address to test.
   * @return True when addr is at or after start and before end.
   */
  bool Contains(u32 addr) const { return addr >= start && addr < end; }
  /** Returns the size of the range in bytes. */
  u32 GetSize() const { return end - start; }
};

}  // namespace rex::codegen
