/**
 * @file        ppc/function.h
 * @brief       PPC calling convention translation, host/PPC wrappers, and hooks
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
#include <concepts>
#include <cstdint>
#include <tuple>
#include <type_traits>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

namespace rex {

//=============================================================================
// Type Traits (additional, types.h has is_be_type)
//=============================================================================

// PPCPointer Detection
template <typename T>
struct is_ppc_pointer : std::false_type {};

template <typename T>
struct is_ppc_pointer<PPCPointer<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_ppc_pointer_v = is_ppc_pointer<T>::value;

// Extract the inner type from PPCPointer<T>
template <typename T>
struct ppc_pointer_inner_type;

template <typename T>
struct ppc_pointer_inner_type<PPCPointer<T>> {
  using type = T;
};

// PPCValue Detection
template <typename T>
struct is_ppc_value : std::false_type {};

template <typename T>
struct is_ppc_value<PPCValue<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_ppc_value_v = is_ppc_value<T>::value;

// Extract the inner type from PPCValue<T>
template <typename T>
struct ppc_value_inner_type;

template <typename T>
struct ppc_value_inner_type<PPCValue<T>> {
  using type = T;
};

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
concept PPCPointerType = is_ppc_pointer_v<T>;

template <typename T>
concept PPCValueType = is_ppc_value_v<T>;

template <typename T>
concept PreciseType = is_precise_v<T>;

// A "plain" type: not a pointer, not be<T>, not PPCPointer, not PPCValue
template <typename T>
concept PlainType =
    !std::is_pointer_v<T> && !BigEndianType<T> && !PPCPointerType<T> && !PPCValueType<T>;

template <auto Func>
struct arg_count_t {
  static constexpr size_t value = std::tuple_size_v<decltype(function_args(Func))>;
};

//=============================================================================
// Physical Heap Offset (Windows Granularity Workaround)
//=============================================================================
// On Windows, allocation granularity is 64KB, so the 0x1000 file offset for
// the 0xE0 physical heap gets masked away. We compensate by adding 0x1000
// to host addresses when the guest address is >= 0xE0000000.

namespace detail {
constexpr uint32_t PhysicalHostOffset([[maybe_unused]] uint32_t guest_addr) noexcept {
#if REX_PLATFORM_WIN32
  return (guest_addr >= 0xE0000000u) ? 0x1000u : 0u;
#else
  return 0u;  // Linux has 4KB granularity, file offset works directly
#endif
}
}  // namespace detail

//=============================================================================
// Argument Translator
//=============================================================================

struct ArgTranslator {
  // Get integer argument value from register or stack
  static constexpr uint64_t GetIntegerArgumentValue(const PPCContext& ctx, uint8_t* base,
                                                    size_t arg) noexcept {
    if (arg <= 7) {
      switch (arg) {
        case 0:
          return ctx.r3.u32;
        case 1:
          return ctx.r4.u32;
        case 2:
          return ctx.r5.u32;
        case 3:
          return ctx.r6.u32;
        case 4:
          return ctx.r7.u32;
        case 5:
          return ctx.r8.u32;
        case 6:
          return ctx.r9.u32;
        case 7:
          return ctx.r10.u32;
        default:
          break;
      }
    }
    // Stack arguments at r1 + 0x54 + ((arg - 8) * 8)
    return __builtin_bswap32(
        *reinterpret_cast<uint32_t*>(base + ctx.r1.u32 + 0x54 + ((arg - 8) * 8)));
  }

  // Get float/double argument value from FPR
  static double GetPrecisionArgumentValue(const PPCContext& ctx, [[maybe_unused]] uint8_t* base,
                                          size_t arg) noexcept {
    switch (arg) {
      case 0:
        return ctx.f1.f64;
      case 1:
        return ctx.f2.f64;
      case 2:
        return ctx.f3.f64;
      case 3:
        return ctx.f4.f64;
      case 4:
        return ctx.f5.f64;
      case 5:
        return ctx.f6.f64;
      case 6:
        return ctx.f7.f64;
      case 7:
        return ctx.f8.f64;
      case 8:
        return ctx.f9.f64;
      case 9:
        return ctx.f10.f64;
      case 10:
        return ctx.f11.f64;
      case 11:
        return ctx.f12.f64;
      case 12:
        return ctx.f13.f64;
      [[unlikely]] default:
        break;
    }
    return 0;
  }

  // Set integer argument value
  static constexpr void SetIntegerArgumentValue(PPCContext& ctx, [[maybe_unused]] uint8_t* base,
                                                size_t arg, uint64_t value) noexcept {
    if (arg <= 7) {
      switch (arg) {
        case 0:
          ctx.r3.u64 = value;
          return;
        case 1:
          ctx.r4.u64 = value;
          return;
        case 2:
          ctx.r5.u64 = value;
          return;
        case 3:
          ctx.r6.u64 = value;
          return;
        case 4:
          ctx.r7.u64 = value;
          return;
        case 5:
          ctx.r8.u64 = value;
          return;
        case 6:
          ctx.r9.u64 = value;
          return;
        case 7:
          ctx.r10.u64 = value;
          return;
        [[unlikely]] default:
          break;
      }
    }
  }

  // Set float/double argument value
  static void SetPrecisionArgumentValue(PPCContext& ctx, [[maybe_unused]] uint8_t* base, size_t arg,
                                        double value) noexcept {
    switch (arg) {
      case 0:
        ctx.f1.f64 = value;
        return;
      case 1:
        ctx.f2.f64 = value;
        return;
      case 2:
        ctx.f3.f64 = value;
        return;
      case 3:
        ctx.f4.f64 = value;
        return;
      case 4:
        ctx.f5.f64 = value;
        return;
      case 5:
        ctx.f6.f64 = value;
        return;
      case 6:
        ctx.f7.f64 = value;
        return;
      case 7:
        ctx.f8.f64 = value;
        return;
      case 8:
        ctx.f9.f64 = value;
        return;
      case 9:
        ctx.f10.f64 = value;
        return;
      case 10:
        ctx.f11.f64 = value;
        return;
      case 11:
        ctx.f12.f64 = value;
        return;
      case 12:
        ctx.f13.f64 = value;
        return;
      [[unlikely]] default:
        break;
    }
  }

  // Get typed value (be<T> types)
  template <BigEndianType T>
  static constexpr T GetValue(PPCContext& ctx, uint8_t* base, size_t idx) noexcept {
    T result;
    result.value = static_cast<decltype(result.value)>(GetIntegerArgumentValue(ctx, base, idx));
    return result;
  }

  // Get typed value (PPCPointer<T>)
  template <PPCPointerType T>
  static T GetValue(PPCContext& ctx, uint8_t* base, size_t idx) noexcept {
    using inner_t = typename ppc_pointer_inner_type<T>::type;
    const auto v = GetIntegerArgumentValue(ctx, base, idx);
    g_memory_base = base;
    if (!v) {
      return T(nullptr);
    }
    uint32_t guest_addr = static_cast<uint32_t>(v);
    inner_t* host_ptr =
        reinterpret_cast<inner_t*>(base + guest_addr + detail::PhysicalHostOffset(guest_addr));
    return T(host_ptr, guest_addr);
  }

  // Get typed value (PPCValue<T>)
  template <PPCValueType T>
  static constexpr T GetValue(PPCContext& ctx, uint8_t* base, size_t idx) noexcept {
    using inner_t = typename ppc_value_inner_type<T>::type;
    if constexpr (is_precise_v<inner_t>) {
      return T(static_cast<inner_t>(GetPrecisionArgumentValue(ctx, base, idx)));
    } else {
      return T(static_cast<inner_t>(GetIntegerArgumentValue(ctx, base, idx)));
    }
  }

  // Get typed value (non-pointer, non-be<T>, non-PPCPointer, non-PPCValue)
  template <PlainType T>
  static constexpr T GetValue(PPCContext& ctx, uint8_t* base, size_t idx) noexcept {
    if constexpr (is_precise_v<T>) {
      return static_cast<T>(GetPrecisionArgumentValue(ctx, base, idx));
    } else {
      return static_cast<T>(GetIntegerArgumentValue(ctx, base, idx));
    }
  }

  // Get typed value (pointer - translates guest address to host pointer)
  template <typename T>
    requires std::is_pointer_v<T>
  static constexpr T GetValue(PPCContext& ctx, uint8_t* base, size_t idx) noexcept {
    const auto v = GetIntegerArgumentValue(ctx, base, idx);
    if (!v) {
      return nullptr;
    }
    uint32_t guest_addr = static_cast<uint32_t>(v);
    return reinterpret_cast<T>(base + guest_addr + detail::PhysicalHostOffset(guest_addr));
  }

  // Set typed value
  template <typename T>
  static constexpr void SetValue(PPCContext& ctx, uint8_t* base, size_t idx, T value) noexcept {
    if constexpr (is_precise_v<T>) {
      SetPrecisionArgumentValue(ctx, base, idx, value);
    } else if constexpr (std::is_null_pointer_v<T>) {
      SetIntegerArgumentValue(ctx, base, idx, 0);
    } else if constexpr (std::is_pointer_v<T>) {
      SetIntegerArgumentValue(ctx, base, idx,
                              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value) -
                                                    reinterpret_cast<uintptr_t>(base)));
    } else {
      SetIntegerArgumentValue(ctx, base, idx, value);
    }
  }
};

//=============================================================================
// Argument Gathering
//=============================================================================

struct Argument {
  int type{};     // 0 = integer, 1 = float
  int ordinal{};  // Position in integer or float argument list
};

// Helper to detect precise types (including PPCValue<float/double>)
template <typename T>
constexpr bool is_precise_type() {
  if constexpr (is_precise_v<T>) {
    return true;
  } else if constexpr (is_ppc_value_v<T>) {
    using inner_t = typename ppc_value_inner_type<T>::type;
    return is_precise_v<inner_t>;
  } else {
    return false;
  }
}

// Type-only gather helper - doesn't require constexpr-constructible types
template <typename... Args>
constexpr std::array<Argument, sizeof...(Args)> GatherFunctionArgumentsFromTypes() {
  std::array<Argument, sizeof...(Args)> args{};
  if constexpr (sizeof...(Args) > 0) {
    int floatOrdinal{};
    int intOrdinal{};
    size_t i{};
    (
        [&]<typename T>() {
          if constexpr (is_precise_type<T>()) {
            args[i] = {1, floatOrdinal++};
          } else {
            args[i] = {0, intOrdinal++};
          }
          i++;
        }.template operator()<Args>(),
        ...);
  }
  return args;
}

// Helper to extract args tuple types and call GatherFunctionArgumentsFromTypes
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
  static constexpr size_t value = GatherFunctionArguments<Func>()[I].ordinal;
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
  ArgTranslator::SetValue<T>(ctx, base, GatherFunctionArgumentsFromTypes<TArgs...>()[I].ordinal,
                             std::get<I>(tpl));
  _translate_args_to_guest<I + 1>(ctx, base, tpl);
}

//=============================================================================
// Host To PPC Function Wrapper
//=============================================================================
// Calls a native C++ function with arguments extracted from PPC context

extern thread_local PPCContext* g_current_ppc_context;

template <auto Func>
__attribute__((noinline)) void HostToGuestFunction(PPCContext& ctx, uint8_t* base) {
  using ret_t = decltype(std::apply(Func, function_args(Func)));

  // Set thread-local state so called code can use GuestToHostFunction
  PPCContext* prev_ctx = g_current_ppc_context;
  g_current_ppc_context = &ctx;
  g_memory_base = base;

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

  g_current_ppc_context = prev_ctx;
}

//=============================================================================
// PPC To Host Function Wrapper
//=============================================================================
// Calls a PPC function from host code with proper context setup

template <typename T, typename TFunction, typename... TArgs>
T GuestToHostFunction(const TFunction& func, TArgs&&... argv) {
  auto args = std::make_tuple(std::forward<TArgs>(argv)...);

  PPCContext* currentCtx = g_current_ppc_context;
  if (!currentCtx) {
    if constexpr (std::is_void_v<T>) {
      return;
    } else {
      return T{};
    }
  }

  auto* ks = ::rex::system::kernel_state();
  if (!ks || !ks->memory()) {
    if constexpr (std::is_void_v<T>) {
      return;
    } else {
      return T{};
    }
  }
  uint8_t* base = ks->memory()->virtual_membase();

  PPCContext newCtx{};
  newCtx.r1 = currentCtx->r1;
  newCtx.r13 = currentCtx->r13;
  newCtx.fpscr = currentCtx->fpscr;

  _translate_args_to_guest(newCtx, base, args);

  g_current_ppc_context = &newCtx;

  if constexpr (std::is_function_v<TFunction>) {
    func(newCtx, base);
  } else if constexpr (std::is_integral_v<TFunction>) {
    (void)func;
  } else {
    func(newCtx, base);
  }

  currentCtx->fpscr = newCtx.fpscr;
  g_current_ppc_context = currentCtx;

  if constexpr (std::is_void_v<T>) {
    return;
  } else if constexpr (std::is_pointer_v<T>) {
    uint32_t guest_addr = newCtx.r3.u32;
    return guest_addr ? reinterpret_cast<T>(base + guest_addr) : nullptr;
  } else if constexpr (is_precise_v<T>) {
    return static_cast<T>(newCtx.f1.f64);
  } else {
    return static_cast<T>(newCtx.r3.u64);
  }
}

}  // namespace rex

//=============================================================================
// PPC Function Hooking and Aliasing Macros
//=============================================================================

// Hook a PPC function name to a native C++ function
#define PPC_HOOK(subroutine, function)             \
  extern "C" PPC_FUNC(subroutine) {                \
    rex::HostToGuestFunction<function>(ctx, base); \
  }

// Create a simple stub that does nothing (with call counter)
#define PPC_STUB(subroutine)                                \
  extern "C" PPC_FUNC(subroutine) {                         \
    (void)base;                                             \
    static std::atomic<uint32_t> s_counter{0};              \
    uint32_t call_num = ++s_counter;                        \
    if (call_num <= 3)                                      \
      REXKRNL_WARN("{} [#{}] STUB", #subroutine, call_num); \
    ctx.r3.u64 = 0;                                         \
  }

// Create a stub that logs when called (with call counter)
#define PPC_STUB_LOG(subroutine, msg)                                  \
  extern "C" PPC_FUNC(subroutine) {                                    \
    (void)base;                                                        \
    static std::atomic<uint32_t> s_counter{0};                         \
    uint32_t call_num = ++s_counter;                                   \
    if (call_num <= 3)                                                 \
      REXKRNL_DEBUG("{} [#{}] STUB - {}", #subroutine, call_num, msg); \
    ctx.r3.u64 = 0;                                                    \
  }

// Create a stub that returns a specific value (with call counter)
#define PPC_STUB_RETURN(subroutine, value)                                    \
  extern "C" PPC_FUNC(subroutine) {                                           \
    (void)base;                                                               \
    static std::atomic<uint32_t> s_counter{0};                                \
    uint32_t call_num = ++s_counter;                                          \
    if (call_num <= 3)                                                        \
      REXKRNL_DEBUG("{} [#{}] STUB - returning {:#x}", #subroutine, call_num, \
                    static_cast<uint32_t>(value));                            \
    ctx.r3.u64 = (value);                                                     \
  }

//=============================================================================
// Kernel Export Constants & Convenience Macros
//=============================================================================

/// Maximum size of the loaded image name buffer (255 chars + NUL).
constexpr size_t kExLoadedImageNameSize = 255 + 1;

// Implemented function export: wraps PPC_HOOK for an xboxkrnl.exe export.
// Usage: XBOXKRNL_EXPORT(__imp__FunctionName, handler_function)
#define XBOXKRNL_EXPORT(name, function) PPC_HOOK(name, function)

// Stub function export: wraps PPC_STUB for an xboxkrnl.exe export.
// Usage: XBOXKRNL_EXPORT_STUB(__imp__FunctionName)
#define XBOXKRNL_EXPORT_STUB(name) PPC_STUB(name)

// Implemented function export: wraps PPC_HOOK for a xam.xex export.
// Usage: XAM_EXPORT(__imp__FunctionName, handler_function)
#define XAM_EXPORT(name, function) PPC_HOOK(name, function)

// Stub function export: wraps PPC_STUB for a xam.xex export.
// Usage: XAM_EXPORT_STUB(__imp__FunctionName)
#define XAM_EXPORT_STUB(name) PPC_STUB(name)
