/**
 * @file        base/memory_posix.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cerrno>
#include <cstddef>

#include <sys/mman.h>

namespace rex::memory::detail {

/**
 * Accepts a fixed address mmap result only when it ended up at the requested address. A kernel
 * before 4.17 treats MAP_FIXED_NOREPLACE as a hint.
 *
 * @param base_address The requested address, or null.
 * @param length The request size in bytes.
 * @param result The mmap return value.
 * @return result, or MAP_FAILED with EEXIST after a stray mapping is unmapped.
 */
inline void* AcceptFixedMapping(void* base_address, size_t length, void* result) noexcept {
  if (result != MAP_FAILED && base_address != nullptr && result != base_address) {
    munmap(result, length);
    errno = EEXIST;
    return MAP_FAILED;
  }
  return result;
}

}  // namespace rex::memory::detail
