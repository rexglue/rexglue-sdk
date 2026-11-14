/**
 * @file        kernel/memory_view.h
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

#include <rex/kernel/heap.h>
#include <rex/types.h>
#include <rex/assert.h>
#include <rex/kernel/thread_state.h>

namespace rex::kernel {

namespace detail {

/** A number a guest memory load or store can byte swap. */
template <typename T>
concept GuestScalar = std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;

}  // namespace detail

/**
 * Translates between guest virtual addresses and host addresses with the same results as
 * GuestAddressSpace. Valid while the GuestAddressSpace it was built from exists.
 */
class MemoryView {
 public:
  explicit MemoryView(const GuestAddressSpace& memory)
      : memory_(&memory), membase_(memory.GetMembase()) {}

  u8* GetVirtualMembase() const { return membase_.GetBase(); }

  template <typename T = u8*>
  T TranslateVirtual(u32 address) const {
    return membase_.TranslateVirtual<T>(address);
  }

  /**
   * Translates a guest virtual address to a host address only when the title could access it, as
   * GuestAddressSpace does.
   *
   * @param required The kMemoryProtect bits the page must grant, the read bit by default.
   * @return The host address, null when the address is 0, misaligned, in no heap, or on a page
   * that lacks a required bit.
   */
  template <typename T = u8*>
  T TryTranslateVirtual(u32 address, u32 required = kMemoryProtectRead) const {
    return memory_->TryTranslateVirtual<T>(address, required);
  }

  /**
   * Translates a host address inside the guest virtual address space to a guest virtual address.
   */
  u32 HostToGuestVirtual(const void* host_address) const {
    return membase_.HostToGuestVirtual(host_address);
  }

  /**
   * Reads a big endian value from guest memory without checking the address.
   *
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
   * Writes a big endian value only when the title could write every byte. Store ignores the page
   * protection.
   *
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
   * @param max_length The most bytes to read, no limit by default.
   * @return A view into guest memory, cut short at max_length or at unreadable memory. No value
   * when the address is 0 or its first byte is unreadable.
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

  const GuestAddressSpace* memory_;
  rex::memory::GuestMembase membase_;
};

}  // namespace rex::kernel

namespace rex::kernel {

/**
 * Binds a type to a guest address. Every access resolves it through guest memory.
 *
 * @tparam T The type the title keeps at the address, laid out as the title stores it.
 */
template <typename T, u32 Address>
struct GuestGlobal {
  static_assert(Address != 0, "A guest global needs a nonzero address");
  static_assert(Address % alignof(T) == 0, "A guest global address must be aligned for its type");
  static_assert(std::is_trivially_copyable_v<T>, "A guest global holds a trivially copyable type");

  static constexpr u32 GetAddress() { return Address; }

  /** Resolves the value through the guest memory of the calling thread. */
  T* GetHost() const {
    auto* thread_state = rex::kernel::ThreadState::Get();
    REX_ASSERT(thread_state != nullptr);
    return rex::memory::GuestPtr<T*>(thread_state->GetAddressSpace()->GetVirtualMembase(), Address);
  }

  /** Resolves the value through a memory view, for a host thread with no bound runtime. */
  T* GetHost(const rex::kernel::MemoryView& view) const {
    return view.TranslateVirtual<T*>(Address);
  }

  /**
   * Like GetHost, but checks that the title could read the value.
   *
   * @return The host pointer, null when the address is in no heap, its page is not readable, or
   * the calling thread has no runtime.
   */
  T* TryGetHost() const {
    auto* thread_state = rex::kernel::ThreadState::Get();
    if (thread_state == nullptr || thread_state->GetAddressSpace() == nullptr) {
      return nullptr;
    }
    return thread_state->GetAddressSpace()->TryTranslateVirtual<T*>(Address);
  }

  /**
   * Like GetHost with a view, but checks that the title could read the value.
   *
   * @return The host pointer, null when the address is in no heap or its page is not readable.
   */
  T* TryGetHost(const rex::kernel::MemoryView& view) const {
    return view.TryTranslateVirtual<T*>(Address);
  }

  T* operator->() const { return GetHost(); }

  T& operator*() const { return *GetHost(); }
};

}  // namespace rex::kernel

/**
 * Declares a global the title keeps at a fixed guest address, at namespace scope. A type with a
 * comma needs an alias first.
 *
 * @param address The guest address, a nonzero literal aligned for the type.
 *
 * @code
 * REX_GUEST_GLOBAL(be_u32, kFrameCount, 0x82001234);
 * u32 frames = *kFrameCount.GetHost();
 * @endcode
 */
#define REX_GUEST_GLOBAL(type, name, address) \
  inline constexpr rex::kernel::GuestGlobal<type, address> name {}
