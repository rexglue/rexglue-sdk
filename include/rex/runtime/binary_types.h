/**
 * @file        runtime/binary_types.h
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

#include <rex/types.h>

namespace rex::runtime {

/** Category of a `BinarySymbol`. */
enum class BinarySymbolType {
  Function, /**< Code. */
  Data,     /**< Data. */
  Import,   /**< Import thunk. */
  Unknown   /**< Not classified. */
};

/** A named address in a module image. */
struct BinarySymbol {
  std::string name;                                  /**< Symbol name. */
  u32 address = 0;                                   /**< Guest address. */
  u32 size = 0;                                      /**< Size in bytes. */
  BinarySymbolType type = BinarySymbolType::Unknown; /**< Category of the symbol. */
};

/** A section of a module image. */
struct BinarySection {
  std::string name;              /**< Section name. */
  u32 virtual_address = 0;       /**< Guest address of the first byte. */
  u32 virtual_size = 0;          /**< Size in bytes. */
  const u8* host_data = nullptr; /**< Host address of the first byte. */
  bool executable = false;       /**< True when the section holds code. */
  bool writable = false;         /**< True when the section is writable. */
};

}  // namespace rex::runtime
