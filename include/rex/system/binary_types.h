/**
 * @file        runtime/binary_types.h
 * @brief       Types for binary introspection (symbols, sections)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace rex::runtime {

enum class BinarySymbolType { Function, Data, Import, Unknown };

struct BinarySymbol {
  std::string name;
  uint32_t address = 0;
  uint32_t size = 0;
  BinarySymbolType type = BinarySymbolType::Unknown;
};

struct BinarySection {
  std::string name;
  uint32_t virtual_address = 0;
  uint32_t virtual_size = 0;
  const uint8_t* host_data = nullptr;  // Pointer to data in host memory
  bool executable = false;
  bool writable = false;
};

}  // namespace rex::runtime
