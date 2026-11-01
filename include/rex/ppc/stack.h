/**
 * @file        ppc/stack.h
 * @brief       Scoped guest stack scratch for host code that calls into guest code.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

#include <rex/assert.h>
#include <rex/memory/endian.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/ppc/context.h>
#include <rex/ppc/call.h>
#include <rex/types.h>
#include <rex/platform.h>
#include <rex/utility.h>

namespace rex::ppc {

class StackFrame;

namespace detail {

/** Rounds a size up to a multiple of 16. */
constexpr u32 stack_align(u32 size) {
  return (size + 15u) & ~15u;
}

/** Reports whether a type holds a host pointer, which a push would copy into guest memory. */
template <typename T>
struct HoldsHostPointer
    : std::bool_constant<std::is_pointer_v<T> || std::is_member_pointer_v<T> ||
                         std::is_null_pointer_v<T> || memory::is_mapped_ptr_v<T>> {};

/** Reports whether the elements of a C array hold a host pointer. */
template <typename T, size_t N>
struct HoldsHostPointer<T[N]> : HoldsHostPointer<T> {};

/** Reports whether the elements of a std::array hold a host pointer. */
template <typename T, size_t N>
struct HoldsHostPointer<std::array<T, N>> : HoldsHostPointer<T> {};

/** Reports whether a type, or the elements of an array of it, hold a host pointer. */
template <typename T>
inline constexpr bool kHoldsHostPointer = HoldsHostPointer<std::remove_cv_t<T>>::value;

/**
 * Returns the innermost open StackFrame of the calling thread, shared by every module, null when
 * none is open.
 */
REX_PPC_API StackFrame*& GetInnermostStackFrame();

/**
 * Returns the register file and memory base of the calling thread, whose register file must be
 * bound.
 */
inline BoundThread GetBoundThreadChecked() {
  const BoundThread bound = GetBoundThread();
  REX_ASSERT(bound.ctx != nullptr, "StackFrame needs a thread state bound to the calling thread");
  return bound;
}

}  // namespace detail

/**
 * Guest stack space pushed during a scope, released at scope exit. Frames close in reverse order
 * and none stays open across a guest SwitchToFiber.
 */
class StackFrame : public rex::NonCopyable {
 public:
  /** Binds the context and memory of the calling thread and saves r1. */
  StackFrame() : StackFrame(detail::GetBoundThreadChecked()) {}

  /**
   * Binds a context and saves its r1.
   *
   * @param ctx The context whose r1 is lowered by pushes and restored at scope exit.
   */
  StackFrame(PPCContext& ctx, u8* base)
      : ctx_(ctx), base_(base), saved_r1_(ctx.r1.u32), previous_(detail::GetInnermostStackFrame()) {
    detail::GetInnermostStackFrame() = this;
  }

  /** Restores r1. */
  ~StackFrame() {
    REX_ASSERT(detail::GetInnermostStackFrame() == this,
               "StackFrame closed out of order, an inner frame is still open");
    ctx_.r1.u32 = saved_r1_;
    detail::GetInnermostStackFrame() = previous_;
  }

  /**
   * Pushes zeroed storage for a value.
   *
   * @return The storage, mapped to its guest address.
   */
  template <typename T>
  memory::MappedPtr<T> Push() {
    static_assert(std::is_trivially_copyable_v<T>,
                  "StackFrame pushes only trivially copyable types");
    static_assert(!detail::kHoldsHostPointer<T>,
                  "StackFrame cannot push a pointer or a MappedPtr, it would copy a host address");
    const u32 address = Allocate(sizeof(T));
    T* host = rex::memory::GuestPtr<T*>(base_, address);
    std::memset(static_cast<void*>(host), 0, sizeof(T));
    return memory::MappedPtr<T>(host, address);
  }

  /**
   * Pushes a value. Arithmetic and enum values are stored big endian. Other types are copied as is,
   * so their fields must be big endian and a pointer inside is not caught.
   *
   * @return The storage, mapped to its guest address.
   */
  template <typename T>
  memory::MappedPtr<T> Push(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "StackFrame pushes only trivially copyable types");
    static_assert(!detail::kHoldsHostPointer<T>,
                  "StackFrame cannot push a pointer or a MappedPtr, it would copy a host address");
    const u32 address = Allocate(sizeof(T));
    T* host = rex::memory::GuestPtr<T*>(base_, address);
    if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
      const T swapped = rex::byte_swap(value);
      std::memcpy(static_cast<void*>(host), &swapped, sizeof(T));
    } else {
      std::memcpy(static_cast<void*>(host), &value, sizeof(T));
    }
    return memory::MappedPtr<T>(host, address);
  }

  /**
   * Pushes a NUL terminated byte string.
   *
   * @return The string, mapped to its guest address.
   */
  memory::MappedPtr<char> PushString(std::string_view text) {
    const u32 address = Allocate(static_cast<u32>(text.size()) + 1);
    char* host = rex::memory::GuestPtr<char*>(base_, address);
    std::memcpy(host, text.data(), text.size());
    host[text.size()] = '\0';
    return memory::MappedPtr<char>(host, address);
  }

  /**
   * Pushes a NUL terminated big endian UTF-16 string.
   *
   * @return The string, mapped to its guest address.
   */
  memory::MappedPtr<be_u16> PushWideString(std::u16string_view text) {
    const u32 address = Allocate((static_cast<u32>(text.size()) + 1) * sizeof(u16));
    be_u16* host = rex::memory::GuestPtr<be_u16*>(base_, address);
    std::ranges::transform(text, host,
                           [](char16_t unit) { return be_u16(static_cast<u16>(unit)); });
    host[text.size()] = u16{0};
    return memory::MappedPtr<be_u16>(host, address);
  }

  /**
   * Pushes raw bytes with no byte swap.
   *
   * @return The bytes, mapped to their guest address.
   */
  memory::MappedPtr<u8> PushBytes(std::span<const u8> bytes) {
    REX_ASSERT(!bytes.empty(), "PushBytes needs at least 1 byte");
    const u32 address = Allocate(static_cast<u32>(bytes.size()));
    u8* host = rex::memory::GuestPtr<u8*>(base_, address);
    std::memcpy(host, bytes.data(), bytes.size());
    return memory::MappedPtr<u8>(host, address);
  }

  PPCContext& GetContext() const { return ctx_; }

  /**
   * Returns the bound guest memory base.
   */
  u8* GetBase() const { return base_; }

 private:
  explicit StackFrame(BoundThread bound) : StackFrame(*bound.ctx, bound.base) {}

  u32 Allocate(u32 size) {
    const u32 new_r1 = ctx_.r1.u32 - detail::stack_align(size);
    stack_bounds_check(ctx_, base_, new_r1);
    ctx_.r1.u32 = new_r1;
    return new_r1;
  }

  PPCContext& ctx_;
  u8* base_;
  u32 saved_r1_;
  StackFrame* previous_;
};

}  // namespace rex::ppc
