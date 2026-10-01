/**
 * @file        runtime/memory_view.h
 * @brief       A copyable view that translates guest addresses without the memory object
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>

#include <rex/runtime/xmemory.h>
#include <rex/types.h>

namespace rex::runtime {

/**
 * Translates between guest virtual addresses and host addresses with the same results as Memory.
 * Valid while the Memory it was built from lives. Host code that only translates holds a view,
 * host code that allocates holds the runtime.
 */
class MemoryView {
 public:
  /**
   * @param memory The guest memory to view.
   */
  explicit MemoryView(const Memory& memory) : virtual_membase_(memory.virtual_membase()) {}

  /**
   * Returns the host base of the guest virtual address space.
   *
   * @return The base.
   */
  u8* virtual_membase() const { return virtual_membase_; }

  /**
   * Translates a guest virtual address to a host address.
   *
   * @tparam T The pointer type to return.
   * @param address The guest virtual address.
   * @return The host address.
   */
  template <typename T = u8*>
  T TranslateVirtual(u32 address) const {
    return GuestPtr<T>(virtual_membase_, address);
  }

  /**
   * Translates a host address inside the guest virtual address space to a guest virtual address.
   *
   * @param host_address The host address.
   * @return The guest virtual address.
   */
  u32 HostToGuestVirtual(const void* host_address) const {
    return detail::RemovePhysicalHostOffset(reinterpret_cast<std::size_t>(host_address) -
                                            reinterpret_cast<std::size_t>(virtual_membase_));
  }

 private:
  u8* virtual_membase_;
};

}  // namespace rex::runtime
