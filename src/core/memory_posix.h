/**
 * @file        core/memory_posix.h
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
 * Accepts the result of a fixed address mmap only when it landed at the requested address. A kernel
 * before 4.17 takes MAP_FIXED_NOREPLACE as a hint and maps elsewhere when the range is held.
 *
 * @param base_address Address the call asked for, or null when it asked for none.
 * @param length Size of the request in bytes.
 * @param result Return value of mmap.
 * @return result when it failed or landed at base_address, otherwise MAP_FAILED with errno set to
 * EEXIST after the stray mapping is removed.
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
