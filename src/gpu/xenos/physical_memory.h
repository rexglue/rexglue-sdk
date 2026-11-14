/**
 * @file        gpu/xenos/physical_memory.h
 * @brief       Pointer access to guest physical memory
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/guest.h>
#include <rex/types.h>

namespace rex::gpu {

/** Guest physical addresses mirror every 512 MB. */
inline constexpr u32 kPhysicalAddressMask = 0x1FFFFFFF;

/**
 * Returns guest physical memory to read in place, or null when the range runs past physical memory.
 *
 * @param address The guest physical address, mirrored every 512 MB.
 * @param length The bytes the caller reads.
 */
template <typename T = u8>
const T* GetPhysicalForRead(IPhysicalMemory& memory, u32 address, u32 length) {
  auto span = memory.GetReadOnlyPhysicalSpan({address & kPhysicalAddressMask, length});
  return span ? reinterpret_cast<const T*>(span->data()) : nullptr;
}

/**
 * Returns guest physical memory to read in place up to its end, for readers that decode their own
 * length, or null when the span is unavailable.
 *
 * @param address The guest physical address, mirrored every 512 MB.
 */
template <typename T = u8>
const T* GetPhysicalToEndForRead(IPhysicalMemory& memory, u32 address) {
  const u32 masked = address & kPhysicalAddressMask;
  return GetPhysicalForRead<T>(memory, masked, kPhysicalAddressMask + 1 - masked);
}

/**
 * Returns guest physical memory to write in place, or null when the range runs past physical
 * memory.
 *
 * @param address The guest physical address, mirrored every 512 MB.
 * @param length The bytes the caller writes.
 */
template <typename T = u8>
T* GetPhysicalForWrite(IPhysicalMemory& memory, u32 address, u32 length) {
  auto span = memory.GetWritablePhysicalSpan({address & kPhysicalAddressMask, length});
  return span ? reinterpret_cast<T*>(span->data()) : nullptr;
}

}  // namespace rex::gpu
