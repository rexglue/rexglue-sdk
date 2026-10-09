/**
 * @file        ppc/function.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on XenonRecomp/UnleashedRecomp translation wrappers
 */

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/platform/exceptions.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>
#include <rex/types.h>

namespace rex::ppc {

//=============================================================================
// Type Traits (additional, types.h has is_be_type)
//=============================================================================

// Function argument helpers
template <typename R, typename... T>
constexpr std::tuple<T...> function_args(R (*)(T...)) noexcept {
  return std::tuple<T...>();
}

template <auto V>
static constexpr decltype(V) constant_v = V;

template <typename T>
static constexpr bool is_precise_v = std::is_same_v<T, float> || std::is_same_v<T, double>;

//=============================================================================
// Concepts for Type Constraints
//=============================================================================

template <typename T>
concept BigEndianType = is_be_type_v<T>;

template <typename T>
concept MappedPtrType = is_mapped_ptr_v<T>;

template <typename T>
concept PreciseType = is_precise_v<T>;

// A "plain" type: not a pointer, not be<T>, not MappedPtr
template <typename T>
concept PlainType = !std::is_pointer_v<T> && !BigEndianType<T> && !MappedPtrType<T>;

template <auto Func>
struct arg_count_t {
  static constexpr size_t value = std::tuple_size_v<decltype(function_args(Func))>;
};

//=============================================================================
// Argument Translator
//=============================================================================

/** Places of 1 argument in the guest calling convention. */
struct Argument {
  /** 1 for a float, 0 for an integer. */
  int type{};
  /** Index in r3 to r10 for an integer, in f1 to f13 for a float. */
  int ordinal{};
  /** Double word the argument takes in the argument image, which a float skips in r3 to r10. */
  int slot{};
};

struct ArgTranslator {
  /** Number of integer argument registers, r3 to r10. */
  static constexpr size_t kIntegerRegisters = 8;
  /** Number of float argument registers, f1 to f13. */
  static constexpr size_t kFloatRegisters = 13;

  /**
   * Returns the integer argument register for an argument image slot.
   *
   * @param ctx The register file.
   * @param slot The slot, under kIntegerRegisters.
   * @return r3 for slot 0 through r10 for slot 7.
   */
  static constexpr PPCRegister& IntegerRegister(PPCContext& ctx, size_t slot) noexcept {
    switch (slot) {
      case 0:
        return ctx.r3;
      case 1:
        return ctx.r4;
      case 2:
        return ctx.r5;
      case 3:
        return ctx.r6;
      case 4:
        return ctx.r7;
      case 5:
        return ctx.r8;
      case 6:
        return ctx.r9;
      default:
        return ctx.r10;
    }
  }

  /**
   * Returns a float argument register.
   *
   * @param ctx The register file.
   * @param ordinal The float argument index, under kFloatRegisters.
   * @return f1 for index 0 through f13 for index 12.
   */
  static constexpr PPCRegister& FloatRegister(PPCContext& ctx, size_t ordinal) noexcept {
    switch (ordinal) {
      case 0:
        return ctx.f1;
      case 1:
        return ctx.f2;
      case 2:
        return ctx.f3;
      case 3:
        return ctx.f4;
      case 4:
        return ctx.f5;
      case 5:
        return ctx.f6;
      case 6:
        return ctx.f7;
      case 7:
        return ctx.f8;
      case 8:
        return ctx.f9;
      case 9:
        return ctx.f10;
      case 10:
        return ctx.f11;
      case 11:
        return ctx.f12;
      default:
        return ctx.f13;
    }
  }

  /**
   * Returns the guest address of an argument image slot past the integer registers.
   *
   * @param ctx The register file at the callee's entry.
   * @param slot The slot, at least kIntegerRegisters.
   * @return The address of the slot's double word.
   */
  static constexpr u32 StackSlotAddress(const PPCContext& ctx, size_t slot) noexcept {
    return ctx.r1.u32 + 0x50 + static_cast<u32>((slot - kIntegerRegisters) * 8);
  }

  template <size_t Bytes = sizeof(u32)>
  static constexpr uint64_t GetIntegerArgumentValue(const PPCContext& ctx, uint8_t* base,
                                                    size_t slot) noexcept {
    if constexpr (Bytes == sizeof(u64)) {
      if (slot < kIntegerRegisters) {
        return IntegerRegister(const_cast<PPCContext&>(ctx), slot).u64;
      }
      return *rex::runtime::GuestPtr<be_u64*>(base, StackSlotAddress(ctx, slot));
    } else {
      if (slot < kIntegerRegisters) {
        return IntegerRegister(const_cast<PPCContext&>(ctx), slot).u32;
      }
      return *rex::runtime::GuestPtr<be_u32*>(base, StackSlotAddress(ctx, slot) + 4);
    }
  }

  static double GetPrecisionArgumentValue(const PPCContext& ctx, uint8_t* base, const Argument& arg,
                                          bool single) noexcept {
    if (static_cast<size_t>(arg.ordinal) < kFloatRegisters) {
      return FloatRegister(const_cast<PPCContext&>(ctx), arg.ordinal).f64;
    }
    const u32 address = StackSlotAddress(ctx, arg.slot);
    if (single) {
      return std::bit_cast<f32>(
          static_cast<u32>(*rex::runtime::GuestPtr<be_u32*>(base, address + 4)));
    }
    return std::bit_cast<f64>(static_cast<u64>(*rex::runtime::GuestPtr<be_u64*>(base, address)));
  }

  static constexpr void SetIntegerArgumentValue(PPCContext& ctx, uint8_t* base, size_t slot,
                                                uint64_t value) noexcept {
    if (slot < kIntegerRegisters) {
      IntegerRegister(ctx, slot).u64 = value;
      return;
    }
    *rex::runtime::GuestPtr<be_u32*>(base, StackSlotAddress(ctx, slot) + 4) =
        static_cast<u32>(value);
  }

  static void SetPrecisionArgumentValue(PPCContext& ctx, uint8_t* base, const Argument& arg,
                                        double value, bool single) noexcept {
    if (static_cast<size_t>(arg.ordinal) < kFloatRegisters) {
      FloatRegister(ctx, arg.ordinal).f64 = value;
      return;
    }
    const u32 address = StackSlotAddress(ctx, arg.slot);
    if (single) {
      *rex::runtime::GuestPtr<be_u32*>(base, address + 4) =
          std::bit_cast<u32>(static_cast<f32>(value));
    } else {
      *rex::runtime::GuestPtr<be_u64*>(base, address) = std::bit_cast<u64>(value);
    }
  }

  template <BigEndianType T>
  static constexpr T GetValue(PPCContext& ctx, uint8_t* base, const Argument& arg) noexcept {
    T result;
    result.value = static_cast<decltype(result.value)>(
        GetIntegerArgumentValue<sizeof(result.value)>(ctx, base, arg.slot));
    return result;
  }

  template <MappedPtrType T>
  static T GetValue(PPCContext& ctx, uint8_t* base, const Argument& arg) noexcept {
    using inner_t = typename mapped_ptr_inner_type<T>::type;
    const auto v = GetIntegerArgumentValue(ctx, base, arg.slot);
    if (!v) {
      return T(nullptr);
    }
    uint32_t guest_addr = static_cast<uint32_t>(v);
    inner_t* host_ptr = rex::runtime::GuestPtr<inner_t*>(base, guest_addr);
    return T(host_ptr, guest_addr);
  }

  template <PlainType T>
  static constexpr T GetValue(PPCContext& ctx, uint8_t* base, const Argument& arg) noexcept {
    if constexpr (is_precise_v<T>) {
      return static_cast<T>(GetPrecisionArgumentValue(ctx, base, arg, std::is_same_v<T, float>));
    } else {
      return static_cast<T>(GetIntegerArgumentValue<sizeof(T)>(ctx, base, arg.slot));
    }
  }

  template <typename T>
    requires std::is_pointer_v<T>
  static constexpr T GetValue(PPCContext& ctx, uint8_t* base, const Argument& arg) noexcept {
    const auto v = GetIntegerArgumentValue(ctx, base, arg.slot);
    if (!v) {
      return nullptr;
    }
    uint32_t guest_addr = static_cast<uint32_t>(v);
    return rex::runtime::GuestPtr<T>(base, guest_addr);
  }

  template <typename T>
  static constexpr void SetValue(PPCContext& ctx, uint8_t* base, const Argument& arg,
                                 T value) noexcept {
    if constexpr (is_precise_v<T>) {
      SetPrecisionArgumentValue(ctx, base, arg, value, std::is_same_v<T, float>);
    } else if constexpr (std::is_null_pointer_v<T>) {
      SetIntegerArgumentValue(ctx, base, arg.slot, 0);
    } else if constexpr (std::is_pointer_v<T>) {
      SetIntegerArgumentValue(ctx, base, arg.slot,
                              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value) -
                                                    reinterpret_cast<uintptr_t>(base)));
    } else {
      SetIntegerArgumentValue(ctx, base, arg.slot, value);
    }
  }
};

//=============================================================================
// Argument Gathering
//=============================================================================

template <typename T>
constexpr bool is_precise_type() {
  return is_precise_v<T>;
}

/**
 * Places each argument by the guest calling convention.
 *
 * Every argument takes the next double word of the argument image. A float takes the next of
 * f1 to f13 and leaves its slot in r3 to r10 unused.
 *
 * @tparam Args The argument types.
 * @return The places of each argument.
 */
template <typename... Args>
constexpr std::array<Argument, sizeof...(Args)> GatherFunctionArgumentsFromTypes() {
  std::array<Argument, sizeof...(Args)> args{};
  if constexpr (sizeof...(Args) > 0) {
    int floatOrdinal{};
    int slot{};
    (
        [&]<typename T>() {
          if constexpr (is_precise_type<T>()) {
            args[slot] = {1, floatOrdinal++, slot};
          } else {
            args[slot] = {0, slot, slot};
          }
          slot++;
        }.template operator()<Args>(),
        ...);
  }
  return args;
}

template <typename R, typename... Args>
constexpr auto GatherFromSignature(R (*)(Args...)) {
  return GatherFunctionArgumentsFromTypes<Args...>();
}

template <auto Func>
constexpr auto GatherFunctionArguments() {
  return GatherFromSignature(Func);
}

template <auto Func, size_t I>
struct arg_ordinal_t {
  static constexpr Argument value = GatherFunctionArguments<Func>()[I];
};

//=============================================================================
// Argument Translation
//=============================================================================

template <auto Func, int I = 0, typename... TArgs>
  requires(I >= sizeof...(TArgs))
void _translate_args_to_host([[maybe_unused]] PPCContext& ctx, [[maybe_unused]] uint8_t* base,
                             [[maybe_unused]] std::tuple<TArgs...>&) noexcept {}

template <auto Func, int I = 0, typename... TArgs>
  requires(I < sizeof...(TArgs))
void _translate_args_to_host(PPCContext& ctx, uint8_t* base, std::tuple<TArgs...>& tpl) noexcept {
  using T = std::tuple_element_t<I, std::remove_reference_t<decltype(tpl)>>;
  std::get<I>(tpl) = ArgTranslator::GetValue<T>(ctx, base, arg_ordinal_t<Func, I>::value);
  _translate_args_to_host<Func, I + 1>(ctx, base, tpl);
}

template <int I = 0, typename... TArgs>
  requires(I >= sizeof...(TArgs))
void _translate_args_to_guest([[maybe_unused]] PPCContext& ctx, [[maybe_unused]] uint8_t* base,
                              [[maybe_unused]] std::tuple<TArgs...>&) noexcept {}

template <int I = 0, typename... TArgs>
  requires(I < sizeof...(TArgs))
void _translate_args_to_guest(PPCContext& ctx, uint8_t* base, std::tuple<TArgs...>& tpl) noexcept {
  using T = std::tuple_element_t<I, std::remove_reference_t<decltype(tpl)>>;
  ArgTranslator::SetValue<T>(ctx, base, GatherFunctionArgumentsFromTypes<TArgs...>()[I],
                             std::get<I>(tpl));
  _translate_args_to_guest<I + 1>(ctx, base, tpl);
}

//=============================================================================

// Host To PPC Function Wrapper
//=============================================================================
// Calls a native C++ function with arguments extracted from PPC context

template <auto Func>
__attribute__((noinline)) void HostToGuestFunction(PPCContext& ctx, uint8_t* base) {
  using ret_t = decltype(std::apply(Func, function_args(Func)));

  auto* thread_state = rex::runtime::ThreadState::Get();
  if (thread_state && thread_state->stop_requested()) {
    throw rex::ThreadStop{};
  }
#if !REX_PLATFORM_WIN32
  const rex::platform::HostCallDepth host_call_depth;
#endif
  auto args = function_args(Func);
  _translate_args_to_host<Func>(ctx, base, args);

  if constexpr (std::is_same_v<ret_t, void>) {
    std::apply(Func, args);
  } else {
    auto v = std::apply(Func, args);

    // Memory barrier to ensure compiler doesn't reorder
    asm volatile("" ::: "memory");

    if constexpr (std::is_pointer<ret_t>()) {
      if (v != nullptr) {
        ctx.r3.u64 =
            static_cast<uint32_t>(reinterpret_cast<size_t>(v) - reinterpret_cast<size_t>(base));
      } else {
        ctx.r3.u64 = 0;
      }
    } else if constexpr (is_precise_v<ret_t>) {
      ctx.f1.f64 = v;
    } else {
      ctx.r3.u64 = static_cast<uint64_t>(v);
    }
  }
  if (thread_state && thread_state->stop_requested()) {
    throw rex::ThreadStop{};
  }
}

/** Bytes under r1 a leaf guest function may use without moving r1. */
inline constexpr u32 kRedZone = 1344;

/** Minimum frame of a guest function that calls others, 10 double words. */
inline constexpr u32 kLinkageSize = 0x50;

/** Return address a call from host code leaves in lr. */
inline constexpr u32 kHostReturnAddress = 0xBCBCBCBC;

//=============================================================================
// Stack bounds helper
//=============================================================================

/// Read stack_end_ptr from KPCR at r13 + 0x74.
/// Returns 0 if r13 is not set (e.g. in unit tests without a live thread).
inline uint32_t stack_limit_from_pcr(PPCContext& ctx, uint8_t* base) {
  if (ctx.r13.u32 == 0)
    return 0;
  uint32_t raw;
  std::memcpy(&raw, rex::runtime::GuestPtr(base, ctx.r13.u32 + 0x74), sizeof(raw));
  return std::byteswap(raw);
}

/**
 * Aborts when lowering r1 to a new value leaves the thread stack it is on.
 *
 * @param ctx The register file, r1 still at its current value.
 * @param base The guest memory base.
 * @param new_r1 The r1 the caller is about to set.
 */
inline void stack_bounds_check(PPCContext& ctx, uint8_t* base, uint32_t new_r1) {
  const uint32_t limit = stack_limit_from_pcr(ctx, base);
  if (limit == 0) {
    return;
  }
  const uint32_t stack_base = *rex::runtime::GuestPtr<be_u32*>(base, ctx.r13.u32 + 0x70);
  if (ctx.r1.u32 > limit && ctx.r1.u32 <= stack_base) {
    REX_FATAL_IF(cpu, new_r1 > limit, "PPC stack overflow: r1 ({:#x}) below stack_end_ptr ({:#x})",
                 new_r1, limit);
  }
}

/**
 * Returns a 16 byte aligned r1 a given number of bytes under the current one.
 *
 * Aborts when the frame would wrap below address 0 or cross the thread's stack limit.
 *
 * @param ctx The register file.
 * @param base The guest memory base, null when no thread state is bound.
 * @param size The bytes the frame needs.
 * @return The new r1.
 */
inline u32 FrameBelow(PPCContext& ctx, uint8_t* base, u32 size) {
  const u32 r1 = ctx.r1.u32;
  REX_FATAL_IF(cpu, size <= r1, "PPC stack underflow: r1 ({:#x}) below a {:#x} byte frame", r1,
               size);
  const u32 new_r1 = (r1 - size) & ~0xFu;
  if (base) {
    stack_bounds_check(ctx, base, new_r1);
  }
  return new_r1;
}

/**
 * Opens a call frame under r1 for a call from host code into recompiled code.
 *
 * @param ctx The register file.
 * @param base The guest memory base.
 * @param stack_bytes The stack argument bytes the call needs.
 * @return The caller's r1.
 */
inline u32 OpenCallFrame(PPCContext& ctx, uint8_t* base, u32 stack_bytes) {
  const u32 caller_r1 = ctx.r1.u32;
  ctx.r1.u64 = FrameBelow(ctx, base, kLinkageSize + stack_bytes);
  *rex::runtime::GuestPtr<be_u32*>(base, ctx.r1.u32) = caller_r1;
  ctx.lr = kHostReturnAddress;
  return caller_r1;
}

/**
 * Returns the stack argument bytes a call with these argument types needs.
 *
 * @tparam Args The argument types.
 * @return 8 bytes per argument past the eighth.
 */
template <typename... Args>
constexpr u32 StackArgumentBytes() {
  constexpr u32 slots = sizeof...(Args);
  return slots > 8 ? (slots - 8) * 8 : 0;
}

/**
 * Calls recompiled code on the thread's register file by the guest calling convention.
 *
 * @tparam R The return type, read from r3 or f1.
 * @tparam Args The argument types.
 * @param ctx The register file of the calling thread.
 * @param base The guest memory base.
 * @param fn The guest function.
 * @param args The arguments.
 * @return The value the guest function returned.
 */
template <typename R, typename... Args>
R Call(PPCContext& ctx, uint8_t* base, PPCFunc& fn, Args... args) {
  const PPCContext saved = ctx;
  OpenCallFrame(ctx, base, StackArgumentBytes<Args...>());
  auto tpl = std::make_tuple(args...);
  _translate_args_to_guest(ctx, base, tpl);
  fn(ctx, base);
  const PPCRegister r3 = ctx.r3;
  const PPCRegister f1 = ctx.f1;
  const PPCFPSCRRegister fpscr = ctx.fpscr;
  ctx = saved;
  ctx.fpscr = fpscr;
  if constexpr (std::is_void_v<R>) {
    return;
  } else if constexpr (std::is_pointer_v<R>) {
    return r3.u32 ? rex::runtime::GuestPtr<R>(base, r3.u32) : nullptr;
  } else if constexpr (is_precise_v<R>) {
    return static_cast<R>(f1.f64);
  } else {
    return static_cast<R>(r3.u64);
  }
}

/**
 * Calls recompiled code on the calling thread's register file.
 *
 * @tparam T The return type.
 * @param func The guest function.
 * @param argv The arguments.
 * @return The value the guest function returned.
 */
template <typename T, typename... TArgs>
T GuestToHostFunction(PPCFunc* func, TArgs&&... argv) {
  auto* ts = rex::runtime::ThreadState::Get();
  REX_ASSERT(ts && func, "GuestToHostFunction needs a bound thread state and a function");
  return Call<T>(*ts->context(), ts->memory()->virtual_membase(), *func,
                 std::forward<TArgs>(argv)...);
}

}  // namespace rex::ppc

/// Maximum size of the loaded image name buffer (255 chars + NUL).
constexpr size_t kExLoadedImageNameSize = 255 + 1;
