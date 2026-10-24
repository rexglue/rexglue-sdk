/**
 * @file        tests/modules/alloc/alloc_module.h
 * @brief       Entry points of the test module that allocates and frees host memory
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace rex::test {

/** Type that needs more than the default alignment. */
struct alignas(64) OverAligned {
  char bytes[64];
};

/** Entry points the alloc test module exports as C symbols. */
struct AllocEntries {
  std::vector<int>* (*make_vector)(size_t count);     /**< Returns a vector made in the module. */
  void (*fill_string)(std::string& out, size_t size); /**< Grows a string in the module. */
  void (*delete_int)(int* value);                     /**< Deletes an int made by the caller. */
  void (*delete_array)(int* values);               /**< Deletes an int array made by the caller. */
  OverAligned* (*make_over_aligned)();             /**< Returns an over aligned block. */
  void (*delete_over_aligned)(OverAligned* value); /**< Deletes an over aligned block. */
  int* (*make_array)(size_t count);                /**< Returns an int array made in the module. */
};

}  // namespace rex::test
