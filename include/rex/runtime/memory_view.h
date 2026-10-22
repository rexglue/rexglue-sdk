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
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>

#include <rex/runtime/xmemory.h>
#include <rex/types.h>

namespace rex::runtime {

namespace detail {

/** A number a guest memory load or store can byte swap. */
template <typename T>
concept GuestScalar = std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;

}  // namespace detail

/**
 * Translates between guest virtual addresses and host addresses with the same results as Memory.
 * Valid while the Memory it was built from exists.
 */
class MemoryView {
 public:
  /**
   * @param memory The guest memory to view.
   */
  explicit MemoryView(const Memory& memory)
      : memory_(&memory), virtual_membase_(memory.GetVirtualMembase()) {}

  /**
   * Returns the host base of the guest virtual address space.
   *
   * @return The base.
   */
  u8* GetVirtualMembase() const { return virtual_membase_; }

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
   * Translates a guest virtual address to a host address only when the title could access it, as
   * Memory does.
   *
   * @tparam T The pointer type to return.
   * @param address The guest virtual address.
   * @param required The kMemoryProtect bits the page must grant, the read bit by default.
   * @return The host address, null when the address is 0, is not aligned for the pointee, is in no
   * heap, or its page does not grant every required bit.
   */
  template <typename T = u8*>
  T TryTranslateVirtual(u32 address, u32 required = kMemoryProtectRead) const {
    return memory_->TryTranslateVirtual<T>(address, required);
  }

  /**
   * Translates a host address inside the guest virtual address space to a guest virtual address.
   *
   * @param host_address The host address.
   * @return The guest virtual address.
   */
  u32 HostToGuestVirtual(const void* host_address) const {
    return HostPtrToGuest(virtual_membase_, host_address);
  }

  /**
   * Reads a big endian value from guest memory without checking the address.
   *
   * @tparam T The native type of the value.
   * @param address The guest virtual address, any alignment.
   * @return The value in native byte order.
   */
  template <typename T>
    requires(detail::GuestScalar<T>)
  T Load(u32 address) const {
    BE<T> raw;
    std::memcpy(&raw, TranslateVirtual<const u8*>(address), sizeof(raw));
    return raw.Get();
  }

  /**
   * Writes a value to guest memory in big endian order without checking the address.
   *
   * @tparam T The native type of the value.
   * @param address The guest virtual address, any alignment.
   * @param value The value in native byte order.
   */
  template <typename T>
    requires(detail::GuestScalar<T>)
  void Store(u32 address, T value) const {
    const BE<T> raw(value);
    std::memcpy(TranslateVirtual<u8*>(address), &raw, sizeof(raw));
  }

  /**
   * Reads a big endian value from guest memory only when the title could read every byte.
   *
   * @tparam T The native type of the value.
   * @param address The guest virtual address, any alignment.
   * @return The value in native byte order, nothing when the address is 0, wraps the address
   * space, or a byte is in no heap or on an unreadable page.
   */
  template <typename T>
    requires(detail::GuestScalar<T>)
  std::optional<T> TryLoad(u32 address) const {
    if (!IsAccessible(address, sizeof(T), kMemoryProtectRead)) {
      return std::nullopt;
    }
    return Load<T>(address);
  }

  /**
   * Writes a value to guest memory in big endian order only when the title could write every byte.
   * A write to a read only page faults on hardware, so this refuses it. A caller that knowingly
   * patches a read only page uses Store.
   *
   * @tparam T The native type of the value.
   * @param address The guest virtual address, any alignment.
   * @param value The value in native byte order.
   * @return True when the value was written, false when the address is 0, wraps the address space,
   * or a byte is in no heap or on a page that is not writable.
   */
  template <typename T>
    requires(detail::GuestScalar<T>)
  bool TryStore(u32 address, T value) const {
    if (!IsAccessible(address, sizeof(T), kMemoryProtectRead | kMemoryProtectWrite)) {
      return false;
    }
    Store<T>(address, value);
    return true;
  }

  /**
   * Reads a null terminated string from guest memory without checking the address.
   *
   * @param address The guest virtual address, 0 for an empty string.
   * @return The bytes before the terminator, a view into guest memory.
   */
  std::string_view GetCString(u32 address) const {
    if (address == 0) {
      return {};
    }
    return std::string_view(TranslateVirtual<const char*>(address));
  }

  /**
   * Reads a null terminated string from guest memory only as far as the title could read it.
   *
   * @param address The guest virtual address.
   * @param max_length The most bytes to read, no limit by default.
   * @return The bytes before the terminator, a view into guest memory. The view ends early at
   * max_length, at the end of the address space, or where the memory is in no heap or not
   * readable, and then it is not followed by a terminator. Nothing when the address is 0 or its
   * first byte cannot be read.
   */
  std::optional<std::string_view> TryGetCString(u32 address,
                                                size_t max_length = std::string_view::npos) const {
    const char* start = memory_->TryTranslateVirtual<const char*>(address);
    if (start == nullptr) {
      return std::nullopt;
    }
    constexpr u64 kBlock = 0x1000;
    size_t length = 0;
    while (length < max_length) {
      const u64 current = static_cast<u64>(address) + length;
      if (current > 0xFFFFFFFFull) {
        break;
      }
      if (length != 0 && current % kBlock == 0 &&
          memory_->TryTranslateVirtual<const char*>(static_cast<u32>(current)) != start + length) {
        break;
      }
      const size_t block = static_cast<size_t>(kBlock - current % kBlock);
      const size_t count = max_length - length < block ? max_length - length : block;
      if (const void* end = std::memchr(start + length, '\0', count)) {
        length = static_cast<size_t>(static_cast<const char*>(end) - start);
        break;
      }
      length += count;
    }
    return std::string_view(start, length);
  }

 private:
  bool IsAccessible(u32 address, u32 size, u32 required) const {
    const u32 last = address + (size - 1);
    return last >= address && memory_->TryTranslateVirtual<const u8*>(address, required) &&
           memory_->TryTranslateVirtual<const u8*>(last, required);
  }

  const Memory* memory_;
  u8* virtual_membase_;
};

}  // namespace rex::runtime
