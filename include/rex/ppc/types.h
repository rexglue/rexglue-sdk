/**
 * @file        ppc/types.h
 * @brief       PPC pointer types, address translation, and PPC register definitions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     PPCPointer types derived from Xenia (https://xenia.jp)
 *              Register semantics based on XenonRecomp/UnleashedRecomp
 */

#pragma once

#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

#include <simde/x86/avx.h>
#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

#include <rex/ppc/detail/fpscr.h>
#include <rex/types.h>

namespace rex {

//=============================================================================
// Type Traits
//=============================================================================

// Big-Endian Type Detection
template <typename T>
struct is_be_type : std::false_type {};

template <typename T>
struct is_be_type<rex::be<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_be_type_v = is_be_type<T>::value;

//=============================================================================
// PPCValue - Wrapper for scalar types with .value() method
//=============================================================================

template <typename T>
class PPCValue {
  T val_;

 public:
  PPCValue() : val_{} {}
  PPCValue(T v) : val_(v) {}

  T value() const { return val_; }

  operator T() const { return val_; }

  bool operator==(const PPCValue& other) const { return val_ == other.val_; }
  bool operator!=(const PPCValue& other) const { return val_ != other.val_; }
  bool operator<(const PPCValue& other) const { return val_ < other.val_; }
  bool operator>(const PPCValue& other) const { return val_ > other.val_; }
  bool operator<=(const PPCValue& other) const { return val_ <= other.val_; }
  bool operator>=(const PPCValue& other) const { return val_ >= other.val_; }

  template <typename U>
    requires std::convertible_to<U, T>
  bool operator==(U other) const {
    return val_ == static_cast<T>(other);
  }
  template <typename U>
    requires std::convertible_to<U, T>
  bool operator!=(U other) const {
    return val_ != static_cast<T>(other);
  }
  template <typename U>
    requires std::convertible_to<U, T>
  bool operator<(U other) const {
    return val_ < static_cast<T>(other);
  }
  template <typename U>
    requires std::convertible_to<U, T>
  bool operator>(U other) const {
    return val_ > static_cast<T>(other);
  }
  template <typename U>
    requires std::convertible_to<U, T>
  bool operator<=(U other) const {
    return val_ <= static_cast<T>(other);
  }
  template <typename U>
    requires std::convertible_to<U, T>
  bool operator>=(U other) const {
    return val_ >= static_cast<T>(other);
  }

  template <typename U>
    requires std::convertible_to<U, T>
  T operator+(U other) const {
    return val_ + static_cast<T>(other);
  }
  template <typename U>
    requires std::convertible_to<U, T>
  T operator-(U other) const {
    return val_ - static_cast<T>(other);
  }
  template <typename U>
    requires std::convertible_to<U, T>
  T operator*(U other) const {
    return val_ * static_cast<T>(other);
  }
  template <typename U>
    requires std::convertible_to<U, T>
  T operator/(U other) const {
    return val_ / static_cast<T>(other);
  }
  template <typename U>
    requires std::convertible_to<U, T> && std::integral<T>
  T operator%(U other) const {
    return val_ % static_cast<T>(other);
  }
  template <typename U>
    requires std::convertible_to<U, T> && std::integral<T>
  T operator&(U other) const {
    return val_ & static_cast<T>(other);
  }
  template <typename U>
    requires std::convertible_to<U, T> && std::integral<T>
  T operator|(U other) const {
    return val_ | static_cast<T>(other);
  }
  template <typename U>
    requires std::convertible_to<U, T> && std::integral<T>
  T operator^(U other) const {
    return val_ ^ static_cast<T>(other);
  }
  T operator<<(int shift) const { return val_ << shift; }
  T operator>>(int shift) const { return val_ >> shift; }
  T operator~() const { return ~val_; }

  explicit operator bool() const { return val_ != 0; }
};

//=============================================================================
// PPCPointer - Wraps host pointer with PPC address tracking
//=============================================================================

template <typename T>
class PPCPointer {
  T* host_ptr_;
  uint32_t guest_addr_;

 public:
  PPCPointer() : host_ptr_(nullptr), guest_addr_(0) {}
  PPCPointer(T* host_ptr, uint32_t guest_addr) : host_ptr_(host_ptr), guest_addr_(guest_addr) {}
  PPCPointer(std::nullptr_t) : host_ptr_(nullptr), guest_addr_(0) {}

  static PPCPointer from_host(T* host_ptr) { return PPCPointer(host_ptr, 0); }

  uint32_t guest_address() const { return guest_addr_; }
  T* host_address() const { return host_ptr_; }

  operator T*() const { return host_ptr_; }

  T* operator->() const { return host_ptr_; }
  T& operator*() const { return *host_ptr_; }

  auto value() const {
    if constexpr (is_be_type_v<T>) {
      return static_cast<typename T::value_type>(*host_ptr_);
    } else {
      return *host_ptr_;
    }
  }

  explicit operator bool() const { return host_ptr_ != nullptr; }
  explicit operator uint32_t() const { return guest_address(); }

  PPCPointer operator+(std::ptrdiff_t offset) const {
    return PPCPointer(host_ptr_ + offset, guest_addr_ + static_cast<uint32_t>(offset * sizeof(T)));
  }
  PPCPointer operator-(std::ptrdiff_t offset) const {
    return PPCPointer(host_ptr_ - offset, guest_addr_ - static_cast<uint32_t>(offset * sizeof(T)));
  }

  template <typename U>
  U as() const {
    return reinterpret_cast<U>(host_ptr_);
  }

  template <typename U>
  rex::be<U>* as_array() const {
    return reinterpret_cast<rex::be<U>*>(host_ptr_);
  }

  void Zero() const {
    if (host_ptr_) {
      std::memset(host_ptr_, 0, sizeof(T));
    }
  }

  void Zero(size_t size) const {
    if (host_ptr_) {
      std::memset(host_ptr_, 0, size);
    }
  }
};

//=============================================================================
// PPCPointer<void> Specialization
//=============================================================================

template <>
class PPCPointer<void> {
  void* host_ptr_;
  uint32_t guest_addr_;

 public:
  PPCPointer() : host_ptr_(nullptr), guest_addr_(0) {}
  PPCPointer(void* host_ptr, uint32_t guest_addr) : host_ptr_(host_ptr), guest_addr_(guest_addr) {}
  PPCPointer(std::nullptr_t) : host_ptr_(nullptr), guest_addr_(0) {}

  static PPCPointer from_host(void* host_ptr) { return PPCPointer(host_ptr, 0); }

  uint32_t guest_address() const { return guest_addr_; }
  uint32_t value() const { return guest_addr_; }
  void* host_address() const { return host_ptr_; }

  operator void*() const { return host_ptr_; }
  operator uint8_t*() const { return static_cast<uint8_t*>(host_ptr_); }
  explicit operator bool() const { return host_ptr_ != nullptr; }
  explicit operator uint32_t() const { return guest_addr_; }

  template <std::integral IntType>
  PPCPointer operator+(IntType offset) const {
    return PPCPointer(static_cast<uint8_t*>(host_ptr_) + static_cast<std::ptrdiff_t>(offset),
                      guest_addr_ + static_cast<uint32_t>(offset));
  }
  template <std::integral IntType>
  PPCPointer operator-(IntType offset) const {
    return PPCPointer(static_cast<uint8_t*>(host_ptr_) - static_cast<std::ptrdiff_t>(offset),
                      guest_addr_ - static_cast<uint32_t>(offset));
  }

  template <typename U>
  U as() const {
    return reinterpret_cast<U>(host_ptr_);
  }

  template <typename U>
  rex::be<U>* as_array() const {
    return reinterpret_cast<rex::be<U>*>(host_ptr_);
  }

  void Zero(size_t size) const {
    if (host_ptr_) {
      std::memset(host_ptr_, 0, size);
    }
  }
};

//=============================================================================
// PPCPointer<char> Specialization (strings)
//=============================================================================

template <>
class PPCPointer<char> {
  char* host_ptr_;
  uint32_t guest_addr_;

 public:
  PPCPointer() : host_ptr_(nullptr), guest_addr_(0) {}
  PPCPointer(char* host_ptr, uint32_t guest_addr) : host_ptr_(host_ptr), guest_addr_(guest_addr) {}
  PPCPointer(std::nullptr_t) : host_ptr_(nullptr), guest_addr_(0) {}

  uint32_t guest_address() const { return guest_addr_; }
  char* host_address() const { return host_ptr_; }

  operator char*() const { return host_ptr_; }
  explicit operator bool() const { return host_ptr_ != nullptr; }

  std::string_view value() const {
    return host_ptr_ ? std::string_view(host_ptr_) : std::string_view();
  }

  char* operator->() const { return host_ptr_; }
  char& operator*() const { return *host_ptr_; }

  PPCPointer operator+(std::ptrdiff_t offset) const {
    return PPCPointer(host_ptr_ + offset, guest_addr_ + static_cast<uint32_t>(offset));
  }
  PPCPointer operator-(std::ptrdiff_t offset) const {
    return PPCPointer(host_ptr_ - offset, guest_addr_ - static_cast<uint32_t>(offset));
  }

  char& operator[](size_t idx) const { return host_ptr_[idx]; }

  template <typename U>
  U as() const {
    return reinterpret_cast<U>(host_ptr_);
  }
};

//=============================================================================
// PPCPointer<char16_t> Specialization (wide strings)
//=============================================================================

template <>
class PPCPointer<char16_t> {
  char16_t* host_ptr_;
  uint32_t guest_addr_;

 public:
  PPCPointer() : host_ptr_(nullptr), guest_addr_(0) {}
  PPCPointer(char16_t* host_ptr, uint32_t guest_addr)
      : host_ptr_(host_ptr), guest_addr_(guest_addr) {}
  PPCPointer(std::nullptr_t) : host_ptr_(nullptr), guest_addr_(0) {}

  uint32_t guest_address() const { return guest_addr_; }
  char16_t* host_address() const { return host_ptr_; }

  operator char16_t*() const { return host_ptr_; }
  explicit operator bool() const { return host_ptr_ != nullptr; }

  std::u16string_view value() const {
    return host_ptr_ ? std::u16string_view(host_ptr_) : std::u16string_view();
  }

  char16_t* operator->() const { return host_ptr_; }
  char16_t& operator*() const { return *host_ptr_; }

  PPCPointer operator+(std::ptrdiff_t offset) const {
    return PPCPointer(host_ptr_ + offset,
                      guest_addr_ + static_cast<uint32_t>(offset * sizeof(char16_t)));
  }
  PPCPointer operator-(std::ptrdiff_t offset) const {
    return PPCPointer(host_ptr_ - offset,
                      guest_addr_ - static_cast<uint32_t>(offset * sizeof(char16_t)));
  }

  char16_t& operator[](size_t idx) const { return host_ptr_[idx]; }

  template <typename U>
  U as() const {
    return reinterpret_cast<U>(host_ptr_);
  }
};

//=============================================================================
// General Purpose Register
//=============================================================================

union Register {
  int8_t s8;
  uint8_t u8;
  int16_t s16;
  uint16_t u16;
  int32_t s32;
  uint32_t u32;
  int64_t s64;
  uint64_t u64;
  float f32;
  double f64;
};

//=============================================================================
// Fixed-Point Exception Register (XER)
//=============================================================================

struct XERRegister {
  uint8_t so;  // Summary Overflow
  uint8_t ov;  // Overflow
  uint8_t ca;  // Carry
};

//=============================================================================
// Condition Register (CR) Field
//=============================================================================

struct CRRegister {
  uint8_t lt;  // Less Than
  uint8_t gt;  // Greater Than
  uint8_t eq;  // Equal
  union {
    uint8_t so;  // Summary Overflow (for integer compares)
    uint8_t un;  // Unordered (for FP compares - NaN involved)
  };

  // Pack CR field into 4-bit value for serialization
  inline uint32_t raw() const noexcept { return (lt << 3) | (gt << 2) | (eq << 1) | so; }

  // Set CR field from 4-bit packed value
  inline void set_raw(uint32_t value) noexcept {
    lt = (value >> 3) & 1;
    gt = (value >> 2) & 1;
    eq = (value >> 1) & 1;
    so = value & 1;
  }

  template <typename T>
  inline void compare(T left, T right, const XERRegister& xer) noexcept {
    lt = left < right;
    gt = left > right;
    eq = left == right;
    so = xer.so;
  }

  inline void compare(double left, double right) noexcept {
    un = __builtin_isnan(left) || __builtin_isnan(right);
    lt = !un && (left < right);
    gt = !un && (left > right);
    eq = !un && (left == right);
  }

  inline void setFromMask(simde__m128 mask, int imm) noexcept {
    int m = simde_mm_movemask_ps(mask);
    lt = m == imm;  // all equal
    gt = 0;
    eq = m == 0;  // none equal
    so = 0;
  }

  inline void setFromMask(simde__m128i mask, int imm) noexcept {
    int m = simde_mm_movemask_epi8(mask);
    lt = m == imm;  // all equal
    gt = 0;
    eq = m == 0;  // none equal
    so = 0;
  }
};

//=============================================================================
// Vector Register (128-bit)
//=============================================================================

union alignas(0x10) VRegister {
  int8_t s8[16];
  uint8_t u8[16];
  int16_t s16[8];
  uint16_t u16[8];
  int32_t s32[4];
  uint32_t u32[4];
  int64_t s64[2];
  uint64_t u64[2];
  float f32[4];
  double f64[2];
};

//=============================================================================
// Floating-Point Status and Control Register (FPSCR)
//=============================================================================

// Rounding mode constants
constexpr uint32_t kRoundNearest = 0x00;
constexpr uint32_t kRoundTowardZero = 0x01;
constexpr uint32_t kRoundUp = 0x02;
constexpr uint32_t kRoundDown = 0x03;
constexpr uint32_t kRoundMask = 0x03;

struct FPSCRRegister {
  uint32_t csr;

  static constexpr size_t HostToGuest[] = {kRoundNearest, kRoundDown, kRoundUp, kRoundTowardZero};

  using Platform = ppc::detail::FPSCRPlatform;
  static constexpr size_t RoundShift = Platform::RoundShift;
  static constexpr size_t RoundMaskVal = Platform::RoundMaskVal;
  static constexpr size_t FlushMask = Platform::FlushMask;

  inline uint32_t getcsr() noexcept { return Platform::getcsr(); }

  inline void setcsr(uint32_t csr) noexcept { Platform::setcsr(csr); }

  inline uint32_t loadFromHost() noexcept {
    csr = getcsr();
    return HostToGuest[(csr & RoundMaskVal) >> RoundShift];
  }

  inline void storeFromGuest(uint32_t value) noexcept {
    csr &= ~RoundMaskVal;
    csr |= Platform::GuestToHost[value & kRoundMask];
    setcsr(csr);
  }

  inline void enableFlushModeUnconditional() noexcept {
    csr |= FlushMask;
    setcsr(csr);
  }

  inline void disableFlushModeUnconditional() noexcept {
    csr &= ~FlushMask;
    setcsr(csr);
  }

  inline void enableFlushMode() noexcept {
    if ((csr & FlushMask) != FlushMask) [[unlikely]] {
      csr |= FlushMask;
      setcsr(csr);
    }
  }

  inline void disableFlushMode() noexcept {
    if ((csr & FlushMask) != 0) [[unlikely]] {
      csr &= ~FlushMask;
      setcsr(csr);
    }
  }

  // Initialize MXCSR/FPCR with all FP exceptions masked
  inline void InitHost() noexcept {
    csr = getcsr();
    Platform::InitHostExceptions(csr);
    setcsr(csr);
  }
};

}  // namespace rex

//=============================================================================
// Global Type Aliases
//=============================================================================
using PPCRegister = rex::Register;
using PPCXERRegister = rex::XERRegister;
using PPCCRRegister = rex::CRRegister;
using PPCVRegister = rex::VRegister;
using PPCFPSCRRegister = rex::FPSCRRegister;

// Rounding mode macros
#define PPC_ROUND_NEAREST rex::kRoundNearest
#define PPC_ROUND_TOWARD_ZERO rex::kRoundTowardZero
#define PPC_ROUND_UP rex::kRoundUp
#define PPC_ROUND_DOWN rex::kRoundDown
#define PPC_ROUND_MASK rex::kRoundMask

//=============================================================================
// PPC Value Type Aliases (global scope)
//=============================================================================
using ppc_i32_t = rex::PPCValue<rex::i32>;
using ppc_u16_t = rex::PPCValue<rex::u16>;
using ppc_u32_t = rex::PPCValue<rex::u32>;
using ppc_u64_t = rex::PPCValue<rex::u64>;
using ppc_f32_t = rex::PPCValue<rex::f32>;
using ppc_f64_t = rex::PPCValue<rex::f64>;
using ppc_fn_t = rex::PPCValue<rex::u32>;       // guest function address
using ppc_unknown_t = rex::PPCValue<rex::u32>;  // untyped u32

//=============================================================================
// PPC Pointer Type Aliases (global scope)
//=============================================================================
using ppc_pvoid_t = rex::PPCPointer<void>;
using ppc_pu16_t = rex::PPCPointer<rex::be_u16>;
using ppc_pu32_t = rex::PPCPointer<rex::be_u32>;
using ppc_pu64_t = rex::PPCPointer<rex::be_u64>;
using ppc_pf32_t = rex::PPCPointer<rex::be_f32>;
using ppc_pf64_t = rex::PPCPointer<rex::be_f64>;
using ppc_pchar_t = rex::PPCPointer<char>;
using ppc_pchar16_t = rex::PPCPointer<char16_t>;

template <typename T>
using ppc_ptr_t = rex::PPCPointer<T>;

//=============================================================================
// PPC Result Types (global scope)
//=============================================================================
using ppc_i32_result_t = rex::i32;
using ppc_u32_result_t = rex::u32;
using ppc_ptr_result_t = rex::u32;
using ppc_hresult_result_t = rex::i32;

//=============================================================================
// fmt formatter for PPCValue
//=============================================================================

#include <fmt/format.h>

template <typename T>
struct fmt::formatter<rex::PPCValue<T>> : fmt::formatter<T> {
  template <typename FormatContext>
  auto format(const rex::PPCValue<T>& v, FormatContext& ctx) const {
    return fmt::formatter<T>::format(v.value(), ctx);
  }
};
