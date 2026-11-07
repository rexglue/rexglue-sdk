/**
 * @file        memory/mapped_ptr.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     The guest address arithmetic is derived from Xenia's xenia/base/memory.h (Ben
 *              Vanik, 2020).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::memory {

/** Host pointer paired with the guest address it maps. */
template <typename T>
class MappedPtr {
  T* host_ptr_ = nullptr;
  u32 guest_addr_ = 0;

 public:
  /** Null pointer at guest address 0. */
  MappedPtr() = default;

  MappedPtr(T* host_ptr, u32 guest_addr) : host_ptr_(host_ptr), guest_addr_(guest_addr) {}

  /** Null pointer at guest address 0. */
  MappedPtr(std::nullptr_t) {}

  /**
   * @return A MappedPtr with guest address 0.
   */
  static MappedPtr FromHost(T* host_ptr) { return MappedPtr(host_ptr, 0); }

  u32 GetGuestAddress() const { return guest_addr_; }

  T* GetHostPtr() const { return host_ptr_; }

  operator T*() const { return host_ptr_; }

  T* operator->() const { return host_ptr_; }

  T& operator*() const { return *host_ptr_; }

  /**
   * @return The pointee, converted to native byte order when T is a BE<> type.
   */
  auto GetValue() const {
    if constexpr (is_be_type_v<T>) {
      return static_cast<T::value_type>(*host_ptr_);
    } else {
      return *host_ptr_;
    }
  }

  explicit operator bool() const { return host_ptr_ != nullptr; }

  explicit operator u32() const { return GetGuestAddress(); }

  /**
   * @param offset Elements to advance.
   * @return A MappedPtr with both addresses advanced by offset elements.
   */
  MappedPtr operator+(std::ptrdiff_t offset) const {
    return MappedPtr(host_ptr_ + offset, guest_addr_ + static_cast<u32>(offset * sizeof(T)));
  }

  /**
   * @param offset Elements to move back.
   * @return A MappedPtr with both addresses moved back by offset elements.
   */
  MappedPtr operator-(std::ptrdiff_t offset) const {
    return MappedPtr(host_ptr_ - offset, guest_addr_ - static_cast<u32>(offset * sizeof(T)));
  }

  template <typename U>
  U As() const {
    return reinterpret_cast<U>(host_ptr_);
  }

  /**
   * @return The host pointer as an array of big endian U values.
   */
  template <typename U>
  rex::BE<U>* AsArray() const {
    return reinterpret_cast<rex::BE<U>*>(host_ptr_);
  }

  /**
   * Zeroes sizeof(T) bytes at the host pointer. Does nothing when it is null.
   */
  void Zero() const {
    if (host_ptr_) {
      std::memset(host_ptr_, 0, sizeof(T));
    }
  }

  /** Zeroes bytes at the host pointer. Does nothing when it is null. */
  void Zero(size_t size) const {
    if (host_ptr_) {
      std::memset(host_ptr_, 0, size);
    }
  }
};

/**
 * Untyped MappedPtr. Offsets count bytes.
 */
template <>
class MappedPtr<void> {
  void* host_ptr_ = nullptr;
  u32 guest_addr_ = 0;

 public:
  /** Null pointer at guest address 0. */
  MappedPtr() = default;

  MappedPtr(void* host_ptr, u32 guest_addr) : host_ptr_(host_ptr), guest_addr_(guest_addr) {}

  /** Null pointer at guest address 0. */
  MappedPtr(std::nullptr_t) {}

  /**
   * @return A MappedPtr with guest address 0.
   */
  static MappedPtr FromHost(void* host_ptr) { return MappedPtr(host_ptr, 0); }

  u32 GetGuestAddress() const { return guest_addr_; }

  u32 GetValue() const { return guest_addr_; }

  void* GetHostPtr() const { return host_ptr_; }

  operator void*() const { return host_ptr_; }

  /**
   * @return The host pointer as bytes.
   */
  operator u8*() const { return static_cast<u8*>(host_ptr_); }

  explicit operator bool() const { return host_ptr_ != nullptr; }

  explicit operator u32() const { return guest_addr_; }

  /**
   * @tparam IntType The integral offset type.
   * @param offset Bytes to advance.
   * @return A MappedPtr with both addresses advanced by offset bytes.
   */
  template <std::integral IntType>
  MappedPtr operator+(IntType offset) const {
    return MappedPtr(static_cast<u8*>(host_ptr_) + static_cast<std::ptrdiff_t>(offset),
                     guest_addr_ + static_cast<u32>(offset));
  }

  /**
   * @tparam IntType The integral offset type.
   * @param offset Bytes to move back.
   * @return A MappedPtr with both addresses moved back by offset bytes.
   */
  template <std::integral IntType>
  MappedPtr operator-(IntType offset) const {
    return MappedPtr(static_cast<u8*>(host_ptr_) - static_cast<std::ptrdiff_t>(offset),
                     guest_addr_ - static_cast<u32>(offset));
  }

  template <typename U>
  U As() const {
    return reinterpret_cast<U>(host_ptr_);
  }

  /**
   * @return The host pointer as an array of big endian U values.
   */
  template <typename U>
  rex::BE<U>* AsArray() const {
    return reinterpret_cast<rex::BE<U>*>(host_ptr_);
  }

  /** Zeroes bytes at the host pointer. Does nothing when it is null. */
  void Zero(size_t size) const {
    if (host_ptr_) {
      std::memset(host_ptr_, 0, size);
    }
  }
};

/**
 * MappedPtr to a null terminated 8 bit string.
 */
template <>
class MappedPtr<char> {
  char* host_ptr_ = nullptr;
  u32 guest_addr_ = 0;

 public:
  /** Null pointer at guest address 0. */
  MappedPtr() = default;

  MappedPtr(char* host_ptr, u32 guest_addr) : host_ptr_(host_ptr), guest_addr_(guest_addr) {}

  /** Null pointer at guest address 0. */
  MappedPtr(std::nullptr_t) {}

  u32 GetGuestAddress() const { return guest_addr_; }

  char* GetHostPtr() const { return host_ptr_; }

  operator char*() const { return host_ptr_; }

  explicit operator bool() const { return host_ptr_ != nullptr; }

  /**
   * @return The string, or an empty view when the pointer is null.
   */
  std::string_view GetValue() const {
    return host_ptr_ ? std::string_view(host_ptr_) : std::string_view();
  }

  char* operator->() const { return host_ptr_; }

  char& operator*() const { return *host_ptr_; }

  /**
   * @param offset Characters to advance.
   * @return A MappedPtr with both addresses advanced by offset characters.
   */
  MappedPtr operator+(std::ptrdiff_t offset) const {
    return MappedPtr(host_ptr_ + offset, guest_addr_ + static_cast<u32>(offset));
  }

  /**
   * @param offset Characters to move back.
   * @return A MappedPtr with both addresses moved back by offset characters.
   */
  MappedPtr operator-(std::ptrdiff_t offset) const {
    return MappedPtr(host_ptr_ - offset, guest_addr_ - static_cast<u32>(offset));
  }

  char& operator[](size_t idx) const { return host_ptr_[idx]; }

  template <typename U>
  U As() const {
    return reinterpret_cast<U>(host_ptr_);
  }
};

/**
 * MappedPtr to a null terminated UTF-16 string.
 */
template <>
class MappedPtr<char16_t> {
  char16_t* host_ptr_ = nullptr;
  u32 guest_addr_ = 0;

 public:
  /** Null pointer at guest address 0. */
  MappedPtr() = default;

  MappedPtr(char16_t* host_ptr, u32 guest_addr) : host_ptr_(host_ptr), guest_addr_(guest_addr) {}

  /** Null pointer at guest address 0. */
  MappedPtr(std::nullptr_t) {}

  u32 GetGuestAddress() const { return guest_addr_; }

  char16_t* GetHostPtr() const { return host_ptr_; }

  operator char16_t*() const { return host_ptr_; }

  explicit operator bool() const { return host_ptr_ != nullptr; }

  /**
   * @return The string, or an empty view when the pointer is null.
   */
  std::u16string_view GetValue() const {
    return host_ptr_ ? std::u16string_view(host_ptr_) : std::u16string_view();
  }

  char16_t* operator->() const { return host_ptr_; }

  char16_t& operator*() const { return *host_ptr_; }

  /**
   * @param offset Characters to advance.
   * @return A MappedPtr with both addresses advanced by offset characters.
   */
  MappedPtr operator+(std::ptrdiff_t offset) const {
    return MappedPtr(host_ptr_ + offset, guest_addr_ + static_cast<u32>(offset * sizeof(char16_t)));
  }

  /**
   * @param offset Characters to move back.
   * @return A MappedPtr with both addresses moved back by offset characters.
   */
  MappedPtr operator-(std::ptrdiff_t offset) const {
    return MappedPtr(host_ptr_ - offset, guest_addr_ - static_cast<u32>(offset * sizeof(char16_t)));
  }

  char16_t& operator[](size_t idx) const { return host_ptr_[idx]; }

  template <typename U>
  U As() const {
    return reinterpret_cast<U>(host_ptr_);
  }
};

template <typename T>
struct IsMappedPtr : std::false_type {};

template <typename T>
struct IsMappedPtr<MappedPtr<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_mapped_ptr_v = IsMappedPtr<T>::value;

template <typename T>
struct MappedPtrInnerType;

/** Pointee type of a MappedPtr. */
template <typename T>
struct MappedPtrInnerType<MappedPtr<T>> {
  using type = T;
};

}  // namespace rex::memory

using MappedVoid = rex::memory::MappedPtr<void>;
using MappedU8 = rex::memory::MappedPtr<rex::u8>;
using MappedU16 = rex::memory::MappedPtr<rex::be_u16>;
using MappedU32 = rex::memory::MappedPtr<rex::be_u32>;
using MappedU64 = rex::memory::MappedPtr<rex::be_u64>;
using MappedF32 = rex::memory::MappedPtr<rex::be_f32>;
using MappedF64 = rex::memory::MappedPtr<rex::be_f64>;
using MappedString = rex::memory::MappedPtr<char>;
using MappedWString = rex::memory::MappedPtr<char16_t>;

template <typename T>
using GuestPtrT = rex::memory::MappedPtr<T>;

namespace rex::memory::detail {

/**
 * True on hosts whose MMU page is above 4 KiB, where a memory view cannot start at the physical
 * offset 0x1000 of the 0xE0000000 range. The view then maps physical memory from 0 and code adds
 * the offset. This is macOS on arm64, which has 16 KiB pages, and Linux on aarch64 when the
 * CMake option REX_PHYSICAL_OFFSET_IN_CODE is on. Windows maps the offset through placeholders,
 * which only need a page aligned offset, and every other host maps it with a 4 KiB page.
 */
#ifndef REX_PHYSICAL_OFFSET_IN_CODE
#error "REX_PHYSICAL_OFFSET_IN_CODE is set by the CMake option of the same name"
#endif
inline constexpr bool kPhysicalOffsetInCode = REX_PHYSICAL_OFFSET_IN_CODE != 0;

/**
 * Returns the offset code adds to the host address of a guest address: 0x1000 from 0xE0000000 up
 * on a host with kPhysicalOffsetInCode, else 0.
 */
constexpr u32 PhysicalHostOffset(u32 guest_addr) noexcept {
  return (kPhysicalOffsetInCode && guest_addr >= 0xE0000000u) ? 0x1000u : 0u;
}

/**
 * Inverts PhysicalHostOffset for a host offset from the virtual base.
 *
 * @param host_offset The host address minus the virtual base.
 */
constexpr u32 RemovePhysicalHostOffset(std::size_t host_offset) noexcept {
  constexpr std::size_t kOffset = PhysicalHostOffset(0xE0000000u);
  const bool shifted =
      host_offset >= 0xE0000000u + kOffset && host_offset < 0x100000000ull + kOffset;
  return static_cast<u32>(shifted ? host_offset - kOffset : host_offset);
}

}  // namespace rex::memory::detail

namespace rex::memory {

/** Translates a guest address to a host pointer with the arithmetic recompiled code uses. */
template <typename T = u8*>
inline T GuestPtr(u8* base, u32 guest_address) noexcept {
  return reinterpret_cast<T>(base + guest_address + detail::PhysicalHostOffset(guest_address));
}

/**
 * Translates a host pointer into guest memory back to its guest address. Inverts GuestPtr.
 *
 * @param host_address The host pointer, null for no address.
 * @return The guest address, 0 for a null pointer.
 */
inline u32 HostPtrToGuest(const u8* base, const void* host_address) noexcept {
  if (host_address == nullptr) {
    return 0;
  }
  return detail::RemovePhysicalHostOffset(reinterpret_cast<std::size_t>(host_address) -
                                          reinterpret_cast<std::size_t>(base));
}

/** The host base of guest memory, which translates between guest virtual and host addresses. */
class GuestMembase {
 public:
  constexpr GuestMembase() noexcept = default;

  explicit constexpr GuestMembase(u8* base) noexcept : base_(base) {}

  /**
   * @return The base, null for a default constructed value.
   */
  [[nodiscard]] constexpr u8* GetBase() const noexcept { return base_; }

  /** Translates a guest virtual address to a host pointer. */
  template <typename T = u8*>
  [[nodiscard]] T TranslateVirtual(u32 guest_address) const noexcept {
    return GuestPtr<T>(base_, guest_address);
  }

  /**
   * Translates a host pointer into guest virtual memory back to its guest address.
   *
   * @param host_address The host pointer, null for no address.
   * @return The guest virtual address, 0 for a null pointer.
   */
  [[nodiscard]] u32 HostToGuestVirtual(const void* host_address) const noexcept {
    return HostPtrToGuest(base_, host_address);
  }

 private:
  u8* base_ = nullptr;
};

}  // namespace rex::memory
