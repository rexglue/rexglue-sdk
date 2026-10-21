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
#include <rex/memory/byte_swap.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>
#include <rex/types.h>

namespace rex::ppc {

class StackFrame;

namespace detail {

/**
 * Rounds a size up to 16 bytes, which keeps r1 16 byte aligned across a push.
 *
 * @param size The size in bytes.
 * @return The size rounded up to a multiple of 16.
 */
constexpr u32 stack_align(u32 size) {
  return (size + 15u) & ~15u;
}

/**
 * Reports whether a type holds a host pointer, which a push would copy into guest memory.
 *
 * @tparam T The type to test.
 */
template <typename T>
struct HoldsHostPointer : std::bool_constant<std::is_pointer_v<T> || std::is_member_pointer_v<T> ||
                                             std::is_null_pointer_v<T> || is_mapped_ptr_v<T>> {};

/**
 * Reports whether the elements of a C array hold a host pointer.
 *
 * @tparam T The element type.
 * @tparam N The element count.
 */
template <typename T, size_t N>
struct HoldsHostPointer<T[N]> : HoldsHostPointer<T> {};

/**
 * Reports whether the elements of a std::array hold a host pointer.
 *
 * @tparam T The element type.
 * @tparam N The element count.
 */
template <typename T, size_t N>
struct HoldsHostPointer<std::array<T, N>> : HoldsHostPointer<T> {};

/**
 * Reports whether a type, or the elements of an array of it, hold a host pointer.
 *
 * @tparam T The type to test.
 */
template <typename T>
inline constexpr bool kHoldsHostPointer = HoldsHostPointer<std::remove_cv_t<T>>::value;

/**
 * Returns the innermost open StackFrame of the calling thread, shared by every module.
 *
 * @return The slot holding the innermost frame, null when none is open.
 */
REX_RUNTIME_API StackFrame*& GetInnermostStackFrame();

/**
 * Returns the thread state bound to the calling thread.
 *
 * @return The thread state, which must be bound.
 */
inline rex::runtime::ThreadState& GetBoundThreadState() {
  rex::runtime::ThreadState* state = rex::runtime::ThreadState::Get();
  REX_ASSERT(state != nullptr, "StackFrame needs a thread state bound to the calling thread");
  return *state;
}

}  // namespace detail

/**
 * Owns the guest stack space pushed during a scope and gives it back at scope exit. No frame stays
 * open across a guest SwitchToFiber, and frames close in the reverse order they opened.
 */
class StackFrame : public rex::NonCopyable {
 public:
  /** Binds the context and memory of the calling thread and saves r1. */
  StackFrame() : StackFrame(detail::GetBoundThreadState()) {}

  /**
   * Binds a context and saves its r1.
   *
   * @param ctx The context whose r1 is lowered by pushes and restored at scope exit.
   * @param base The guest memory base.
   */
  StackFrame(PPCContext& ctx, u8* base)
      : ctx_(ctx), base_(base), saved_r1_(ctx.r1.u32), previous_(detail::GetInnermostStackFrame()) {
    detail::GetInnermostStackFrame() = this;
  }

  /** Restores r1. Debug builds assert that this is the innermost open frame of the thread. */
  ~StackFrame() {
    REX_ASSERT(detail::GetInnermostStackFrame() == this,
               "StackFrame closed out of order, an inner frame is still open");
    ctx_.r1.u32 = saved_r1_;
    detail::GetInnermostStackFrame() = previous_;
  }

  /**
   * Pushes zeroed storage for a value.
   *
   * @tparam T The type of the value.
   * @return The storage, mapped to its guest address.
   */
  template <typename T>
  MappedPtr<T> Push() {
    static_assert(std::is_trivially_copyable_v<T>,
                  "StackFrame pushes only trivially copyable types");
    static_assert(!detail::kHoldsHostPointer<T>,
                  "StackFrame cannot push a pointer or a MappedPtr, it would copy a host address");
    const u32 address = Allocate(sizeof(T));
    T* host = rex::runtime::GuestPtr<T*>(base_, address);
    std::memset(static_cast<void*>(host), 0, sizeof(T));
    return MappedPtr<T>(host, address);
  }

  /**
   * Pushes a value. An arithmetic or enum value is stored big endian and any other type is copied
   * as is, so its fields must already be big endian. A pointer inside a struct is not detected.
   *
   * @tparam T The type of the value.
   * @param value The value to store.
   * @return The storage, mapped to its guest address.
   */
  template <typename T>
  MappedPtr<T> Push(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "StackFrame pushes only trivially copyable types");
    static_assert(!detail::kHoldsHostPointer<T>,
                  "StackFrame cannot push a pointer or a MappedPtr, it would copy a host address");
    const u32 address = Allocate(sizeof(T));
    T* host = rex::runtime::GuestPtr<T*>(base_, address);
    if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
      const T swapped = rex::byte_swap(value);
      std::memcpy(static_cast<void*>(host), &swapped, sizeof(T));
    } else {
      std::memcpy(static_cast<void*>(host), &value, sizeof(T));
    }
    return MappedPtr<T>(host, address);
  }

  /**
   * Pushes a NUL terminated byte string.
   *
   * @param text The characters to store.
   * @return The string, mapped to its guest address.
   */
  MappedPtr<char> PushString(std::string_view text) {
    const u32 address = Allocate(static_cast<u32>(text.size()) + 1);
    char* host = rex::runtime::GuestPtr<char*>(base_, address);
    std::memcpy(host, text.data(), text.size());
    host[text.size()] = '\0';
    return MappedPtr<char>(host, address);
  }

  /**
   * Pushes a NUL terminated big endian UTF-16 string.
   *
   * @param text The code units to store.
   * @return The string, mapped to its guest address.
   */
  MappedPtr<be_u16> PushWideString(std::u16string_view text) {
    const u32 address = Allocate((static_cast<u32>(text.size()) + 1) * sizeof(u16));
    be_u16* host = rex::runtime::GuestPtr<be_u16*>(base_, address);
    std::ranges::transform(text, host,
                           [](char16_t unit) { return be_u16(static_cast<u16>(unit)); });
    host[text.size()] = u16{0};
    return MappedPtr<be_u16>(host, address);
  }

  /**
   * Pushes raw bytes with no byte swap.
   *
   * @param bytes The bytes to store.
   * @return The bytes, mapped to their guest address.
   */
  MappedPtr<u8> PushBytes(std::span<const u8> bytes) {
    REX_ASSERT(!bytes.empty(), "PushBytes needs at least 1 byte");
    const u32 address = Allocate(static_cast<u32>(bytes.size()));
    u8* host = rex::runtime::GuestPtr<u8*>(base_, address);
    std::memcpy(host, bytes.data(), bytes.size());
    return MappedPtr<u8>(host, address);
  }

  /**
   * Returns the bound context.
   *
   * @return The context.
   */
  PPCContext& GetContext() const { return ctx_; }

  /**
   * Returns the bound guest memory base.
   *
   * @return The base.
   */
  u8* GetBase() const { return base_; }

 private:
  explicit StackFrame(rex::runtime::ThreadState& state)
      : StackFrame(*state.GetContext(), state.GetMemory()->GetVirtualMembase()) {}

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
