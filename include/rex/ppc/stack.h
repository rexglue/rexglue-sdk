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

#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>
#include <rex/types.h>

namespace rex::ppc {

/**
 * Rounds a size up to 8 bytes. The PPC64 ABI requires 16 byte frame alignment, but r1 is
 * already frame aligned on entry, so individual pushes round to 8 bytes to keep doubleword
 * alignment.
 *
 * @param size The size in bytes.
 * @return The size rounded up to a multiple of 8.
 */
inline constexpr uint32_t stack_align(uint32_t size) {
  return (size + 7u) & ~7u;
}

/**
 * Pushes a byte swapped scalar onto the guest stack.
 *
 * @tparam T The arithmetic type to push.
 * @param ctx The context whose r1 is lowered.
 * @param base The guest memory base.
 * @param value The value to push.
 * @return The guest address of the pushed value.
 */
template <typename T>
  requires std::is_arithmetic_v<T>
inline uint32_t stack_push(PPCContext& ctx, uint8_t* base, T value) {
  uint32_t alloc = stack_align(sizeof(T));
  uint32_t new_r1 = ctx.r1.u32 - alloc;
  stack_bounds_check(ctx, base, new_r1);
  ctx.r1.u32 = new_r1;

  T swapped;
  if constexpr (sizeof(T) == 1) {
    swapped = value;
  } else if constexpr (sizeof(T) == 2) {
    u16 bits = std::bit_cast<u16>(value);
    bits = std::byteswap(bits);
    swapped = std::bit_cast<T>(bits);
  } else if constexpr (sizeof(T) == 4) {
    u32 bits = std::bit_cast<u32>(value);
    bits = std::byteswap(bits);
    swapped = std::bit_cast<T>(bits);
  } else if constexpr (sizeof(T) == 8) {
    u64 bits = std::bit_cast<u64>(value);
    bits = std::byteswap(bits);
    swapped = std::bit_cast<T>(bits);
  }
  std::memcpy(rex::runtime::GuestPtr(base, new_r1), &swapped, sizeof(T));
  return new_r1;
}

/**
 * Pushes a NUL terminated string onto the guest stack as raw bytes without a byte swap.
 *
 * @param ctx The context whose r1 is lowered.
 * @param base The guest memory base.
 * @param str The string to push.
 * @return The guest address of the pushed string.
 */
inline uint32_t stack_push_string(PPCContext& ctx, uint8_t* base, const char* str) {
  uint32_t len = static_cast<uint32_t>(std::strlen(str)) + 1;
  uint32_t alloc = stack_align(len);
  uint32_t new_r1 = ctx.r1.u32 - alloc;
  stack_bounds_check(ctx, base, new_r1);
  ctx.r1.u32 = new_r1;
  std::memcpy(rex::runtime::GuestPtr(base, new_r1), str, len);
  return new_r1;
}

/**
 * Pushes raw bytes onto the guest stack without a byte swap.
 *
 * @param ctx The context whose r1 is lowered.
 * @param base The guest memory base.
 * @param data The bytes to push.
 * @param len The byte count.
 * @return The guest address of the pushed bytes.
 */
inline uint32_t stack_push(PPCContext& ctx, uint8_t* base, const void* data, uint32_t len) {
  uint32_t alloc = stack_align(len);
  uint32_t new_r1 = ctx.r1.u32 - alloc;
  stack_bounds_check(ctx, base, new_r1);
  ctx.r1.u32 = new_r1;
  std::memcpy(rex::runtime::GuestPtr(base, new_r1), data, len);
  return new_r1;
}

/**
 * Pops bytes from the guest stack.
 *
 * @param ctx The context whose r1 is raised.
 * @param size The byte count passed to the matching push.
 */
inline void stack_pop(PPCContext& ctx, uint32_t size) {
  ctx.r1.u32 += stack_align(size);
}

/** Restores r1 when the scope ends, releasing what the scope pushed. */
class stack_guard : public rex::NonCopyable {
  PPCContext& ctx_;
  uint32_t saved_r1_;

 public:
  /**
   * Saves r1 of a context.
   *
   * @param ctx The context whose r1 is restored.
   */
  explicit stack_guard(PPCContext& ctx) : ctx_(ctx), saved_r1_(ctx.r1.u32) {}
  /** Saves r1 of the context of the calling thread. */
  stack_guard() : stack_guard(*rex::runtime::ThreadState::Get()->context()) {}
  ~stack_guard() { ctx_.r1.u32 = saved_r1_; }
};

namespace detail {
inline uint8_t* current_base() {
  return rex::runtime::ThreadState::Get()->memory()->virtual_membase();
}
}  // namespace detail

/**
 * Pushes a byte swapped scalar onto the guest stack of the calling thread.
 *
 * @tparam T The arithmetic type to push.
 * @param value The value to push.
 * @return The guest address of the pushed value.
 */
template <typename T>
  requires std::is_arithmetic_v<T>
inline uint32_t stack_push(T value) {
  return stack_push(*rex::runtime::ThreadState::Get()->context(), detail::current_base(), value);
}

/**
 * Pushes a NUL terminated string onto the guest stack of the calling thread as raw bytes.
 *
 * @param str The string to push.
 * @return The guest address of the pushed string.
 */
inline uint32_t stack_push_string(const char* str) {
  return stack_push_string(*rex::runtime::ThreadState::Get()->context(), detail::current_base(),
                           str);
}

/**
 * Pushes raw bytes onto the guest stack of the calling thread without a byte swap.
 *
 * @param data The bytes to push.
 * @param len The byte count.
 * @return The guest address of the pushed bytes.
 */
inline uint32_t stack_push(const void* data, uint32_t len) {
  return stack_push(*rex::runtime::ThreadState::Get()->context(), detail::current_base(), data,
                    len);
}

/**
 * Pops bytes from the guest stack of the calling thread.
 *
 * @param size The byte count passed to the matching push.
 */
inline void stack_pop(uint32_t size) {
  stack_pop(*rex::runtime::ThreadState::Get()->context(), size);
}

}  // namespace rex::ppc
