/**
 * @file        rex/codegen/code_region.h
 * @brief       Lightweight code region struct (no PPC dependencies)
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

struct CodeRegion {
  uint32_t start;
  uint32_t end;

  bool contains(uint32_t addr) const { return addr >= start && addr < end; }
  uint32_t size() const { return end - start; }
};

}  // namespace rex::codegen
