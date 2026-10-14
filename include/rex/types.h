/**
 * @file        types.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

#include <rex/assert.h>
#include <rex/platform.h>

static_assert((std::endian::native == std::endian::big) ||
              (std::endian::native == std::endian::little));

namespace rex {

/** Unsigned 8 bit integer. */
using u8 = uint8_t;
/** Unsigned 16 bit integer. */
using u16 = uint16_t;
/** Unsigned 32 bit integer. */
using u32 = uint32_t;
/** Unsigned 64 bit integer. */
using u64 = uint64_t;

/** Signed 8 bit integer. */
using i8 = int8_t;
/** Signed 16 bit integer. */
using i16 = int16_t;
/** Signed 32 bit integer. */
using i32 = int32_t;
/** Signed 64 bit integer. */
using i64 = int64_t;

/** 32 bit IEEE 754 float. */
using f32 = float;
/** 64 bit IEEE 754 float. */
using f64 = double;

static_assert(sizeof(f32) == 4, "f32 must be 4 bytes");
static_assert(sizeof(f64) == 8, "f64 must be 8 bytes");

/**
 * Reverses the byte order of a trivially copyable value of 1, 2, 4 or 8 bytes.
 *
 * @tparam T The value type.
 * @param value The value to swap.
 * @return The value with its bytes reversed.
 */
template <class T>
constexpr T byte_swap(T value) noexcept {
  static_assert(sizeof(T) == 8 || sizeof(T) == 4 || sizeof(T) == 2 || sizeof(T) == 1,
                "byte_swap(T value): Type T has illegal size");
  if constexpr (sizeof(T) == 1) {
    return value;
  } else {
    using UintT =
        std::conditional_t<sizeof(T) == 8, u64, std::conditional_t<sizeof(T) == 4, u32, u16>>;
    return std::bit_cast<T>(std::byteswap(std::bit_cast<UintT>(value)));
  }
}

/**
 * Value held in memory in a fixed byte order and converted to native order on access.
 *
 * @tparam T The value type.
 * @tparam E The byte order in memory.
 */
template <typename T, std::endian E>
struct EndianValue {
  /** The native value type, read by MappedPtr::value. */
  using value_type = T;

  EndianValue() = default;

  /**
   * @param src The native value to hold.
   */
  EndianValue(const T& src) { Set(src); }

  EndianValue(const EndianValue&) = default;
  EndianValue& operator=(const EndianValue&) = default;

  /**
   * @return The value in native byte order.
   */
  operator T() const { return Get(); }

  /**
   * Stores a native value in byte order E.
   *
   * @param src The native value.
   */
  void Set(const T& src) {
    if constexpr (std::endian::native == E) {
      value = src;
    } else {
      value = rex::byte_swap(src);
    }
  }

  /**
   * @return The value in native byte order.
   */
  T Get() const {
    if constexpr (std::endian::native == E) {
      return value;
    }
    return rex::byte_swap(value);
  }

  /**
   * Loads the native value atomically.
   *
   * @param order The memory order of the load.
   * @return The value in native byte order.
   */
  T Load(std::memory_order order) const {
    static_assert(std::atomic_ref<T>::is_always_lock_free);
    REX_ASSERT(std::bit_cast<uintptr_t>(&value) % std::atomic_ref<T>::required_alignment == 0);
    const T raw = std::atomic_ref<T>(const_cast<T&>(value)).load(order);
    if constexpr (std::endian::native == E) {
      return raw;
    }
    return rex::byte_swap(raw);
  }

  /**
   * Stores a native value in byte order E atomically.
   *
   * @param src The native value.
   * @param order The memory order of the store.
   */
  void Store(T src, std::memory_order order) {
    static_assert(std::atomic_ref<T>::is_always_lock_free);
    REX_ASSERT(std::bit_cast<uintptr_t>(&value) % std::atomic_ref<T>::required_alignment == 0);
    if constexpr (std::endian::native != E) {
      src = rex::byte_swap(src);
    }
    std::atomic_ref<T>(value).store(src, order);
  }

  /**
   * Replaces the value atomically when it equals an expected native value.
   *
   * @param expected The native value to compare. Receives the current native value on failure.
   * @param desired The native value to store on success.
   * @param order The memory order of the exchange.
   * @return True when the value matched and desired was stored.
   */
  bool CompareExchange(T& expected, T desired, std::memory_order order) {
    static_assert(std::atomic_ref<T>::is_always_lock_free);
    REX_ASSERT(std::bit_cast<uintptr_t>(&value) % std::atomic_ref<T>::required_alignment == 0);
    T raw_expected = expected;
    if constexpr (std::endian::native != E) {
      raw_expected = rex::byte_swap(expected);
      desired = rex::byte_swap(desired);
    }
    if (std::atomic_ref<T>(value).compare_exchange_strong(raw_expected, desired, order)) {
      return true;
    }
    if constexpr (std::endian::native != E) {
      expected = rex::byte_swap(raw_expected);
    } else {
      expected = raw_expected;
    }
    return false;
  }

  /**
   * Adds to the native value.
   *
   * @param a The amount to add.
   * @return This object.
   */
  EndianValue<T, E>& operator+=(int a) {
    *this = *this + a;
    return *this;
  }

  /**
   * Subtracts from the native value.
   *
   * @param a The amount to subtract.
   * @return This object.
   */
  EndianValue<T, E>& operator-=(int a) {
    *this = *this - a;
    return *this;
  }

  /**
   * Prefix increment.
   *
   * @return This object.
   */
  EndianValue<T, E>& operator++() {
    *this += 1;
    return *this;
  }

  /**
   * Postfix increment.
   *
   * @return The value before the increment.
   */
  EndianValue<T, E> operator++(int) {
    *this += 1;
    return (*this - 1);
  }

  /**
   * Prefix decrement.
   *
   * @return This object.
   */
  EndianValue<T, E>& operator--() {
    *this -= 1;
    return *this;
  }

  /**
   * Postfix decrement.
   *
   * @return The value before the decrement.
   */
  EndianValue<T, E> operator--(int) {
    *this -= 1;
    return (*this + 1);
  }

  T value; /**< The raw value in byte order E. */
};

/**
 * Big endian value.
 *
 * @tparam T The value type.
 */
template <typename T>
using BE = EndianValue<T, std::endian::big>;

/**
 * Little endian value.
 *
 * @tparam T The value type.
 */
template <typename T>
using LE = EndianValue<T, std::endian::little>;

/**
 * True when T is a be<> type.
 *
 * @tparam T The type to test.
 */
template <typename T>
struct IsBEType : std::false_type {};

/**
 * Matches be<T>.
 *
 * @tparam T The wrapped value type.
 */
template <typename T>
struct IsBEType<rex::BE<T>> : std::true_type {};

/**
 * True when T is a be<> type.
 *
 * @tparam T The type to test.
 */
template <typename T>
inline constexpr bool is_be_type_v = IsBEType<T>::value;

/** 32 bit guest address. */
using GuestAddrT = u32;
/** Native host address. */
using host_addr_t = uintptr_t;

/** Single byte, which needs no byte swap. */
using be_u8 = u8;
/** Big endian u16. */
using be_u16 = BE<u16>;
/** Big endian u32. */
using be_u32 = BE<u32>;
/** Big endian u64. */
using be_u64 = BE<u64>;

/** Single byte, which needs no byte swap. */
using be_i8 = i8;
/** Big endian i16. */
using be_i16 = BE<i16>;
/** Big endian i32. */
using be_i32 = BE<i32>;
/** Big endian i64. */
using be_i64 = BE<i64>;

/** Big endian f32. */
using be_f32 = BE<f32>;
/** Big endian f64. */
using be_f64 = BE<f64>;

/**
 * Host pointer paired with the guest address it maps.
 *
 * @tparam T The pointee type.
 */
template <typename T>
class MappedPtr {
  T* host_ptr_ = nullptr;
  u32 guest_addr_ = 0;

 public:
  /** Null pointer at guest address 0. */
  MappedPtr() = default;

  /**
   * @param host_ptr The host pointer.
   * @param guest_addr The guest address it maps.
   */
  MappedPtr(T* host_ptr, u32 guest_addr) : host_ptr_(host_ptr), guest_addr_(guest_addr) {}

  /** Null pointer at guest address 0. */
  MappedPtr(std::nullptr_t) {}

  /**
   * @param host_ptr The host pointer.
   * @return A MappedPtr with guest address 0.
   */
  static MappedPtr GetFromHost(T* host_ptr) { return MappedPtr(host_ptr, 0); }

  /**
   * @return The guest address.
   */
  u32 GetGuestAddress() const { return guest_addr_; }

  /**
   * @return The host pointer.
   */
  T* GetHostAddress() const { return host_ptr_; }

  /**
   * @return The host pointer.
   */
  operator T*() const { return host_ptr_; }

  /**
   * @return The host pointer.
   */
  T* operator->() const { return host_ptr_; }

  /**
   * @return The pointee.
   */
  T& operator*() const { return *host_ptr_; }

  /**
   * @return The pointee, converted to native byte order when T is a be<> type.
   */
  auto GetValue() const {
    if constexpr (is_be_type_v<T>) {
      return static_cast<T::value_type>(*host_ptr_);
    } else {
      return *host_ptr_;
    }
  }

  /**
   * @return True when the host pointer is not null.
   */
  explicit operator bool() const { return host_ptr_ != nullptr; }

  /**
   * @return The guest address.
   */
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

  /**
   * @tparam U The pointer type to cast to.
   * @return The host pointer cast to U.
   */
  template <typename U>
  U As() const {
    return reinterpret_cast<U>(host_ptr_);
  }

  /**
   * @tparam U The element value type.
   * @return The host pointer as an array of be<U>.
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

  /**
   * Zeroes bytes at the host pointer. Does nothing when it is null.
   *
   * @param size Bytes to zero.
   */
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

  /**
   * @param host_ptr The host pointer.
   * @param guest_addr The guest address it maps.
   */
  MappedPtr(void* host_ptr, u32 guest_addr) : host_ptr_(host_ptr), guest_addr_(guest_addr) {}

  /** Null pointer at guest address 0. */
  MappedPtr(std::nullptr_t) {}

  /**
   * @param host_ptr The host pointer.
   * @return A MappedPtr with guest address 0.
   */
  static MappedPtr GetFromHost(void* host_ptr) { return MappedPtr(host_ptr, 0); }

  /**
   * @return The guest address.
   */
  u32 GetGuestAddress() const { return guest_addr_; }

  /**
   * @return The guest address.
   */
  u32 GetValue() const { return guest_addr_; }

  /**
   * @return The host pointer.
   */
  void* GetHostAddress() const { return host_ptr_; }

  /**
   * @return The host pointer.
   */
  operator void*() const { return host_ptr_; }

  /**
   * @return The host pointer as bytes.
   */
  operator u8*() const { return static_cast<u8*>(host_ptr_); }

  /**
   * @return True when the host pointer is not null.
   */
  explicit operator bool() const { return host_ptr_ != nullptr; }

  /**
   * @return The guest address.
   */
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

  /**
   * @tparam U The pointer type to cast to.
   * @return The host pointer cast to U.
   */
  template <typename U>
  U As() const {
    return reinterpret_cast<U>(host_ptr_);
  }

  /**
   * @tparam U The element value type.
   * @return The host pointer as an array of be<U>.
   */
  template <typename U>
  rex::BE<U>* AsArray() const {
    return reinterpret_cast<rex::BE<U>*>(host_ptr_);
  }

  /**
   * Zeroes bytes at the host pointer. Does nothing when it is null.
   *
   * @param size Bytes to zero.
   */
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

  /**
   * @param host_ptr The host pointer.
   * @param guest_addr The guest address it maps.
   */
  MappedPtr(char* host_ptr, u32 guest_addr) : host_ptr_(host_ptr), guest_addr_(guest_addr) {}

  /** Null pointer at guest address 0. */
  MappedPtr(std::nullptr_t) {}

  /**
   * @return The guest address.
   */
  u32 GetGuestAddress() const { return guest_addr_; }

  /**
   * @return The host pointer.
   */
  char* GetHostAddress() const { return host_ptr_; }

  /**
   * @return The host pointer.
   */
  operator char*() const { return host_ptr_; }

  /**
   * @return True when the host pointer is not null.
   */
  explicit operator bool() const { return host_ptr_ != nullptr; }

  /**
   * @return The string, or an empty view when the pointer is null.
   */
  std::string_view GetValue() const {
    return host_ptr_ ? std::string_view(host_ptr_) : std::string_view();
  }

  /**
   * @return The host pointer.
   */
  char* operator->() const { return host_ptr_; }

  /**
   * @return The first character.
   */
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

  /**
   * @param idx The character index.
   * @return The character at idx.
   */
  char& operator[](size_t idx) const { return host_ptr_[idx]; }

  /**
   * @tparam U The pointer type to cast to.
   * @return The host pointer cast to U.
   */
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

  /**
   * @param host_ptr The host pointer.
   * @param guest_addr The guest address it maps.
   */
  MappedPtr(char16_t* host_ptr, u32 guest_addr) : host_ptr_(host_ptr), guest_addr_(guest_addr) {}

  /** Null pointer at guest address 0. */
  MappedPtr(std::nullptr_t) {}

  /**
   * @return The guest address.
   */
  u32 GetGuestAddress() const { return guest_addr_; }

  /**
   * @return The host pointer.
   */
  char16_t* GetHostAddress() const { return host_ptr_; }

  /**
   * @return The host pointer.
   */
  operator char16_t*() const { return host_ptr_; }

  /**
   * @return True when the host pointer is not null.
   */
  explicit operator bool() const { return host_ptr_ != nullptr; }

  /**
   * @return The string, or an empty view when the pointer is null.
   */
  std::u16string_view GetValue() const {
    return host_ptr_ ? std::u16string_view(host_ptr_) : std::u16string_view();
  }

  /**
   * @return The host pointer.
   */
  char16_t* operator->() const { return host_ptr_; }

  /**
   * @return The first character.
   */
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

  /**
   * @param idx The character index.
   * @return The character at idx.
   */
  char16_t& operator[](size_t idx) const { return host_ptr_[idx]; }

  /**
   * @tparam U The pointer type to cast to.
   * @return The host pointer cast to U.
   */
  template <typename U>
  U As() const {
    return reinterpret_cast<U>(host_ptr_);
  }
};

/**
 * True when T is a MappedPtr.
 *
 * @tparam T The type to test.
 */
template <typename T>
struct IsMappedPtr : std::false_type {};

/**
 * Matches MappedPtr<T>.
 *
 * @tparam T The pointee type.
 */
template <typename T>
struct IsMappedPtr<MappedPtr<T>> : std::true_type {};

/**
 * True when T is a MappedPtr.
 *
 * @tparam T The type to test.
 */
template <typename T>
inline constexpr bool is_mapped_ptr_v = IsMappedPtr<T>::value;

template <typename T>
struct mapped_ptr_inner_type;

/**
 * Pointee type of a MappedPtr.
 *
 * @tparam T The pointee type.
 */
template <typename T>
struct mapped_ptr_inner_type<MappedPtr<T>> {
  /** The pointee type. */
  using type = T;
};

/**
 * Base for types that can move but never copy.
 */
class MoveOnly {
 protected:
  MoveOnly() = default;
  MoveOnly(MoveOnly&&) = default;
  MoveOnly& operator=(MoveOnly&&) = default;
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;
};

/**
 * Base for types that neither copy nor move.
 */
class NonCopyable {
 protected:
  NonCopyable() = default;
  NonCopyable(const NonCopyable&) = delete;
  NonCopyable& operator=(const NonCopyable&) = delete;
};

}  // namespace rex

/** Global rex::u8. */
using u8 = rex::u8;
/** Global rex::i8. */
using i8 = rex::i8;
/** Global rex::u16. */
using u16 = rex::u16;
/** Global rex::i16. */
using i16 = rex::i16;
/** Global rex::u32. */
using u32 = rex::u32;
/** Global rex::i32. */
using i32 = rex::i32;
/** Global rex::u64. */
using u64 = rex::u64;
/** Global rex::i64. */
using i64 = rex::i64;
/** Global rex::f32. */
using f32 = rex::f32;
/** Global rex::f64. */
using f64 = rex::f64;

/** Global rex::be_u16. */
using be_u16 = rex::be_u16;
/** Global rex::be_u32. */
using be_u32 = rex::be_u32;
/** Global rex::be_u64. */
using be_u64 = rex::be_u64;
/** Global rex::be_i16. */
using be_i16 = rex::be_i16;
/** Global rex::be_i32. */
using be_i32 = rex::be_i32;
/** Global rex::be_i64. */
using be_i64 = rex::be_i64;
/** Global rex::be_f32. */
using be_f32 = rex::be_f32;
/** Global rex::be_f64. */
using be_f64 = rex::be_f64;

/** MappedPtr to untyped guest memory. */
using MappedVoid = rex::MappedPtr<void>;
/** MappedPtr to a byte. */
using MappedU8 = rex::MappedPtr<rex::u8>;
/** MappedPtr to a big endian u16. */
using MappedU16 = rex::MappedPtr<rex::be_u16>;
/** MappedPtr to a big endian u32. */
using MappedU32 = rex::MappedPtr<rex::be_u32>;
/** MappedPtr to a big endian u64. */
using MappedU64 = rex::MappedPtr<rex::be_u64>;
/** MappedPtr to a big endian f32. */
using MappedF32 = rex::MappedPtr<rex::be_f32>;
/** MappedPtr to a big endian f64. */
using MappedF64 = rex::MappedPtr<rex::be_f64>;
/** MappedPtr to an 8 bit string. */
using MappedString = rex::MappedPtr<char>;
/** MappedPtr to a UTF-16 string. */
using MappedWString = rex::MappedPtr<char16_t>;

/**
 * Former name of rex::MappedPtr.
 *
 * @tparam T The pointee type.
 */
template <typename T>
using GuestPtrT = rex::MappedPtr<T>;
