/**
 * @file        ppc/pdata.h
 * @brief       Decoder for the entries of the exception directory.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>

namespace rex::ppc {

/** Function type held in the 2 high bits of a pdata word. */
enum class RuntimeFunctionType : u8 {
  SaveMillicode,    /**< Stores registers and returns. */
  NoHandler,        /**< A normal function. */
  RestoreMillicode, /**< Loads registers and returns. */
  Handler           /**< A function with handler data before it. */
};

/** One decoded pdata entry. */
struct RuntimeFunction {
  u32 begin;                /**< Address of the first instruction. */
  RuntimeFunctionType type; /**< Function type. */
  u32 length;               /**< Instruction count including the prologue. */
  u32 prolog;               /**< Instruction count of the prologue. */
};

/**
 * Decodes a pdata entry.
 *
 * @param begin The begin address word.
 * @param word The type, length and prologue word.
 * @return The decoded entry.
 */
constexpr RuntimeFunction DecodeRuntimeFunction(u32 begin, u32 word) {
  return RuntimeFunction{begin, static_cast<RuntimeFunctionType>(word >> 30),
                         (word >> 8) & 0x3FFFFFu, word & 0xFFu};
}

}  // namespace rex::ppc
