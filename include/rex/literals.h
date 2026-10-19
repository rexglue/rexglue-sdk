/**
 * @file        literals.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/literals.h (Ben Vanik, 2021).
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace rex {

/**
 * Returns a size in bytes from a count of kibibytes.
 *
 * @param x Count of kibibytes.
 * @return x times 1024 bytes.
 */
constexpr size_t operator""_KiB(unsigned long long int x) {
  return 1024ULL * x;
}

/**
 * Returns a size in bytes from a count of mebibytes.
 *
 * @param x Count of mebibytes.
 * @return x times 1024 KiB, in bytes.
 */
constexpr size_t operator""_MiB(unsigned long long int x) {
  return 1024_KiB * x;
}

/**
 * Returns a size in bytes from a count of gibibytes.
 *
 * @param x Count of gibibytes.
 * @return x times 1024 MiB, in bytes.
 */
constexpr size_t operator""_GiB(unsigned long long int x) {
  return 1024_MiB * x;
}

/**
 * Returns a size in bytes from a count of tebibytes.
 *
 * @param x Count of tebibytes.
 * @return x times 1024 GiB, in bytes.
 */
constexpr size_t operator""_TiB(unsigned long long int x) {
  return 1024_GiB * x;
}

/**
 * Returns a size in bytes from a count of pebibytes.
 *
 * @param x Count of pebibytes.
 * @return x times 1024 TiB, in bytes.
 */
constexpr size_t operator""_PiB(unsigned long long int x) {
  return 1024_TiB * x;
}

}  // namespace rex
