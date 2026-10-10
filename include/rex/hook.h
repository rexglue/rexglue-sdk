/**
 * @file        hook.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstring>
#include <format>
#include <tuple>
#include <type_traits>

#include <rex/logging.h>
#include <rex/macros.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/runtime/thread_state.h>
#include <rex/types.h>

/**
 * Hooks a recompiled function with a native C++ function. The native function uses plain types
 * (u32, mapped_u32 and others) and HostToGuestFunction translates the registers.
 *
 * @param subroutine The recompiled function symbol.
 * @param function The native function the hook calls.
 *
 * @code
 * REX_HOOK(ExampleFunction, ExampleHook);
 * @endcode
 */
#ifdef REXGLUE_ENABLE_PROFILING
#include <tracy/Tracy.hpp>
#define REX_HOOK(subroutine, function)                           \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(subroutine) {      \
    ZoneNamedN(___tracy_hook_zone, #subroutine, TracyIsStarted); \
    rex::ppc::HostToGuestFunction<function>(ctx, base);          \
  }
#else
#define REX_HOOK(subroutine, function)                      \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(subroutine) { \
    rex::ppc::HostToGuestFunction<function>(ctx, base);     \
  }
#endif

/**
 * Defines a raw hook with access to ctx and base.
 *
 * @param name The recompiled function symbol.
 *
 * @code
 * REX_HOOK_RAW(ExampleFunction) {
 *   ctx.r3.u64 = 0;
 * }
 * @endcode
 */
#define REX_HOOK_RAW(name) extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(name)

/**
 * Defines a stub that logs a warning on the first call and at Dev level after.
 *
 * @param subroutine The recompiled function symbol.
 *
 * @code
 * REX_STUB(ExampleFunction);
 * @endcode
 */
#define REX_STUB(subroutine)                                        \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(subroutine) {         \
    (void)base;                                                     \
    static constinit ::std::atomic_flag rex_stub_seen_;             \
    ::rex::log::detail::WriteStub(rex_stub_seen_, #subroutine, {}); \
  }

/**
 * Defines a stub that logs a message on the first call and at Dev level after.
 *
 * @param subroutine The recompiled function symbol.
 * @param msg The message logged with the stub.
 *
 * @code
 * REX_STUB_LOG(ExampleFunction, "skips the save");
 * @endcode
 */
#define REX_STUB_LOG(subroutine, msg)                                \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(subroutine) {          \
    (void)base;                                                      \
    static constinit ::std::atomic_flag rex_stub_seen_;              \
    ::rex::log::detail::WriteStub(rex_stub_seen_, #subroutine, msg); \
  }

/**
 * Defines a stub that returns a fixed value in r3 and logs it like REX_STUB.
 *
 * @param subroutine The recompiled function symbol.
 * @param value The value returned in r3.
 *
 * @code
 * REX_STUB_RETURN(ExampleFunction, 0);
 * @endcode
 */
#define REX_STUB_RETURN(subroutine, value)                                 \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(subroutine) {                \
    (void)base;                                                            \
    static constinit ::std::atomic_flag rex_stub_seen_;                    \
    ::rex::log::detail::WriteStub(                                         \
        rex_stub_seen_, #subroutine,                                       \
        ::std::format("returning {:#x}", static_cast<::rex::u32>(value))); \
    ctx.r3.u64 = (value);                                                  \
  }

/**
 * Defines a hook whose symbol a kernel export table links against.
 *
 * @param name The hook symbol.
 * @param function The host function the hook wraps.
 *
 * @code
 * REX_EXPORT(__imp__CloseHandle, rex::kernel::xam::CloseHandle_shim)
 * @endcode
 */
#define REX_EXPORT(name, function) REX_HOOK(name, function)

/**
 * Defines a stub whose symbol a kernel export table links against.
 *
 * @param name The hook symbol.
 *
 * @code
 * REX_EXPORT_STUB(__imp__AniBlockOnAnimation);
 * @endcode
 */
#define REX_EXPORT_STUB(name) REX_STUB(name)

/**
 * Defines a stub that returns a fixed value and whose symbol a kernel export table links against.
 *
 * @param name The hook symbol.
 * @param retval The value returned in r3.
 *
 * @code
 * REX_EXPORT_STUB_RETURN(__imp__DmRegisterCommandProcessorEx, 0);
 * @endcode
 */
#define REX_EXPORT_STUB_RETURN(name, retval) REX_STUB_RETURN(name, retval)

namespace rex::ppc {

/**
 * Typed callable for a recompiled function.
 *
 * @tparam S The function signature.
 */
template <typename S>
struct ImportFunction;

/**
 * Typed callable for a recompiled function of signature R(Args...).
 *
 * @tparam R The return type.
 * @tparam Args The parameter types.
 */
template <typename R, typename... Args>
struct ImportFunction<R(Args...)> {
  PPCFunc& fn; /**< The recompiled function. */

  /**
   * Calls the function on a given context.
   *
   * @param ctx The context the call runs on.
   * @param base The memory base.
   * @param args The arguments.
   * @return The function result.
   */
  R operator()(PPCContext& ctx, u8* base, Args... args) const {
    return Call<R>(ctx, base, fn, args...);
  }

  /**
   * Calls the function on the current thread's context.
   *
   * @param args The arguments.
   * @return The function result.
   */
  R operator()(Args... args) const {
    auto* ts = rex::runtime::ThreadState::Get();
    return Call<R>(*ts->context(), ts->memory()->virtual_membase(), fn, args...);
  }
};

}  // namespace rex::ppc

/**
 * Declares a typed callable import of a recompiled function.
 *
 * The callable has internal linkage. On ELF a namespace scope C++ variable is not name mangled,
 * so an external linkage callable named after a guest function would collide with the generated
 * weak function alias and hijack its function table entry.
 *
 * @param symbol The exact linker symbol to reference.
 * @param callable The name of the typed callable variable.
 * @param sig The function signature, such as u32(u32, u32).
 *
 * @code
 * REX_IMPORT(ExampleFunction, example_function, u32(u32, u32));
 * @endcode
 */
// NOLINTBEGIN(bugprone-macro-parentheses): callable is a declared name
#define REX_IMPORT(symbol, callable, sig)                          \
  REX_EXTERN(symbol);                                              \
  [[maybe_unused]] static rex::ppc::ImportFunction<sig> callable { \
    symbol                                                         \
  }
// NOLINTEND(bugprone-macro-parentheses)
