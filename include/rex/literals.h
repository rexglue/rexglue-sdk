/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

namespace rex::literals {

constexpr size_t operator""_KiB(unsigned long long int x) {
  return 1024ULL * x;
}

constexpr size_t operator""_MiB(unsigned long long int x) {
  return 1024_KiB * x;
}

constexpr size_t operator""_GiB(unsigned long long int x) {
  return 1024_MiB * x;
}

constexpr size_t operator""_TiB(unsigned long long int x) {
  return 1024_GiB * x;
}

constexpr size_t operator""_PiB(unsigned long long int x) {
  return 1024_TiB * x;
}

}  // namespace rex::literals
