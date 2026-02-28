/**
 * @file        rex/logging/format.h
 * @brief       Formatting helpers for log messages - ptr(), hex(), boolean()
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>

#include <spdlog/fmt/fmt.h>

namespace rex::log {

/** Format a 32-bit address as hex (e.g. "0x0040F000"). */
inline std::string ptr(uint32_t addr) {
  return fmt::format("0x{:08X}", addr);
}

/** Format a 64-bit address as hex, auto-selecting width. */
inline std::string ptr(uint64_t addr) {
  if (addr > 0xFFFFFFFFULL) {
    return fmt::format("0x{:016X}", addr);
  }
  return fmt::format("0x{:08X}", static_cast<uint32_t>(addr));
}

/** Format a void pointer. */
inline std::string ptr(const void* p) {
  return fmt::format("{}", p);
}

/** Format a typed pointer. */
template <typename T>
inline std::string ptr(T* p) {
  return fmt::format("{}", static_cast<const void*>(p));
}

/** Format a 32-bit value as hex (e.g. "0x1A"). */
inline std::string hex(uint32_t val) {
  return fmt::format("0x{:X}", val);
}

/** Format a 64-bit value as hex. */
inline std::string hex(uint64_t val) {
  return fmt::format("0x{:X}", val);
}

/** Format a boolean as "true" or "false". */
inline const char* boolean(bool b) {
  return b ? "true" : "false";
}

}  // namespace rex::log
