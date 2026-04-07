/**
 * @file        ppc/stack.h
 * @brief       Guest PPC stack push/pop operations and scope guard.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <concepts>
#include <cstdint>
#include <cstring>

#include <rex/logging/assert.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/types.h>

namespace rex::ppc {

//=============================================================================
// Stack bounds helper
//=============================================================================

/// Read stack_end_ptr from KPCR at r13 + 0x74.
/// Returns 0 if r13 is not set (e.g. in unit tests without a live thread).
inline uint32_t stack_limit_from_pcr(PPCContext& ctx, uint8_t* base) {
  if (ctx.r13.u32 == 0)
    return 0;
  uint32_t raw;
  std::memcpy(&raw, base + ctx.r13.u32 + 0x74, sizeof(raw));
  return __builtin_bswap32(raw);
}

inline void stack_bounds_check(PPCContext& ctx, uint8_t* base, uint32_t new_r1) {
  uint32_t limit = stack_limit_from_pcr(ctx, base);
  if (limit != 0) {
    REX_FATAL_IF(new_r1 > limit, "PPC stack overflow: r1 ({:#x}) below stack_end_ptr ({:#x})",
                 new_r1, limit);
  }
}

// note(tomc): PPC64 ABI requires 16-byte frame alignment, but r1 is already frame-aligned
// on entry; individual pushes round to 8 bytes to maintain doubleword alignment.
inline constexpr uint32_t stack_align(uint32_t size) {
  return (size + 7u) & ~7u;
}

//=============================================================================
// Core push/pop (explicit ctx/base)
//=============================================================================

/// Push a byte-swapped scalar onto the guest stack.
template <typename T>
  requires std::is_arithmetic_v<T>
inline uint32_t stack_push(PPCContext& ctx, uint8_t* base, T value) {
  uint32_t alloc = stack_align(sizeof(T));
  uint32_t new_r1 = ctx.r1.u32 - alloc;
  stack_bounds_check(ctx, base, new_r1);
  ctx.r1.u32 = new_r1;

  // Byte-swap and write
  T swapped;
  if constexpr (sizeof(T) == 1) {
    swapped = value;
  } else if constexpr (sizeof(T) == 2) {
    uint16_t bits;
    std::memcpy(&bits, &value, 2);
    bits = __builtin_bswap16(bits);
    std::memcpy(&swapped, &bits, 2);
  } else if constexpr (sizeof(T) == 4) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    bits = __builtin_bswap32(bits);
    std::memcpy(&swapped, &bits, 4);
  } else if constexpr (sizeof(T) == 8) {
    uint64_t bits;
    std::memcpy(&bits, &value, 8);
    bits = __builtin_bswap64(bits);
    std::memcpy(&swapped, &bits, 8);
  }
  std::memcpy(base + new_r1, &swapped, sizeof(T));
  return new_r1;
}

/// Push a NUL-terminated string onto the guest stack (no byte-swap, raw bytes).
inline uint32_t stack_push_string(PPCContext& ctx, uint8_t* base, const char* str) {
  uint32_t len = static_cast<uint32_t>(std::strlen(str)) + 1;
  uint32_t alloc = stack_align(len);
  uint32_t new_r1 = ctx.r1.u32 - alloc;
  stack_bounds_check(ctx, base, new_r1);
  ctx.r1.u32 = new_r1;
  std::memcpy(base + new_r1, str, len);
  return new_r1;
}

/// Push raw bytes onto the guest stack (no byte-swap).
inline uint32_t stack_push(PPCContext& ctx, uint8_t* base, const void* data, uint32_t len) {
  uint32_t alloc = stack_align(len);
  uint32_t new_r1 = ctx.r1.u32 - alloc;
  stack_bounds_check(ctx, base, new_r1);
  ctx.r1.u32 = new_r1;
  std::memcpy(base + new_r1, data, len);
  return new_r1;
}

/// Pop bytes from the guest stack.
inline void stack_pop(PPCContext& ctx, uint32_t size) {
  ctx.r1.u32 += stack_align(size);
}

//=============================================================================
// Scope guard
//=============================================================================

/// RAII guard that saves r1 on construction and restores on destruction.
class stack_guard {
  PPCContext& ctx_;
  uint32_t saved_r1_;

 public:
  explicit stack_guard(PPCContext& ctx) : ctx_(ctx), saved_r1_(ctx.r1.u32) {}
  stack_guard() : stack_guard(*rex::runtime::ThreadState::Get()->context()) {}
  ~stack_guard() { ctx_.r1.u32 = saved_r1_; }

  stack_guard(const stack_guard&) = delete;
  stack_guard& operator=(const stack_guard&) = delete;
};

//=============================================================================
// Implicit ctx/base overloads (use current thread context)
//=============================================================================

namespace detail {
inline PPCContext& current_ctx() {
  return *rex::runtime::ThreadState::Get()->context();
}
inline uint8_t* current_base() {
  return rex::system::kernel_state()->memory()->virtual_membase();
}
}  // namespace detail

template <typename T>
  requires std::is_arithmetic_v<T>
inline uint32_t stack_push(T value) {
  return stack_push(detail::current_ctx(), detail::current_base(), value);
}

inline uint32_t stack_push_string(const char* str) {
  return stack_push_string(detail::current_ctx(), detail::current_base(), str);
}

inline uint32_t stack_push(const void* data, uint32_t len) {
  return stack_push(detail::current_ctx(), detail::current_base(), data, len);
}

inline void stack_pop(uint32_t size) {
  stack_pop(detail::current_ctx(), size);
}

}  // namespace rex::ppc
