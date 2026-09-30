/**
 * @file        test_memory.cpp
 * @brief       Single shared guest Memory instance for the unit test binary
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include "test_memory.h"

#include <catch2/catch_test_macros.hpp>

#include <rex/logging.h>

namespace rex::testing {

rex::memory::Memory& GetTestMemory() {
  static rex::memory::Memory memory;
  static bool initialized = memory.Initialize();
  REQUIRE(initialized);
  return memory;
}

}  // namespace rex::testing
