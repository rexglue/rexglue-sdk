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

//=============================================================================
// Hook Macros
//=============================================================================

// Hook a recompiled function with an auto-marshaled native C++ function.
// The native function uses plain types (u32, mapped_u32 and others) and
// HostToGuestFunction handles register translation automatically.
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

// Define a raw hook with direct ctx/base access.
#define REX_HOOK_RAW(name) extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(name)

// Stub: logs a warning on the first call and Dev after.
#define REX_STUB(subroutine)                                        \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(subroutine) {         \
    (void)base;                                                     \
    static constinit ::std::atomic_flag rex_stub_seen_;             \
    ::rex::log::detail::WriteStub(rex_stub_seen_, #subroutine, {}); \
  }

#define REX_STUB_LOG(subroutine, msg)                                \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(subroutine) {          \
    (void)base;                                                      \
    static constinit ::std::atomic_flag rex_stub_seen_;              \
    ::rex::log::detail::WriteStub(rex_stub_seen_, #subroutine, msg); \
  }

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
 */
#define REX_EXPORT(name, function) REX_HOOK(name, function)

/**
 * Defines a stub whose symbol a kernel export table links against.
 *
 * @param name The hook symbol.
 */
#define REX_EXPORT_STUB(name) REX_STUB(name)

/**
 * Defines a stub that returns a fixed value and whose symbol a kernel export table links against.
 *
 * @param name The hook symbol.
 * @param retval The value returned in r3.
 */
#define REX_EXPORT_STUB_RETURN(name, retval) REX_STUB_RETURN(name, retval)

namespace rex {

//=============================================================================
// CallFrame - Lightweight isolated calling context
//=============================================================================
// For side calls that must not disturb the outer hook's register state.
// Stack-allocated, NOT zero-initialized. Only copies the registers a callee
// needs from the parent context.

struct CallFrame : public rex::NonCopyable {
  PPCContext ctx;
  PPCContext& parent_;

  explicit CallFrame(PPCContext& parent) : parent_(parent) {
    ctx.r1 = parent.r1;
    ctx.r13 = parent.r13;
    ctx.fpscr = parent.fpscr;
  }

  ~CallFrame() { parent_.fpscr = ctx.fpscr; }

  operator PPCContext&() { return ctx; }
};

}  // namespace rex

namespace rex::ppc {

//=============================================================================
// ImportFunction - Zero-overhead typed callable for recompiled functions
//=============================================================================

template <typename S>
struct ImportFunction;

template <typename R, typename... Args>
struct ImportFunction<R(Args...)> {
  PPCFunc& fn;

  R operator()(PPCContext& ctx, uint8_t* base, Args... args) const {
    auto tpl = std::make_tuple(args...);
    _translate_args_to_guest(ctx, base, tpl);
    fn(ctx, base);
    if constexpr (std::is_void_v<R>) {
      return;
    } else if constexpr (is_precise_v<R>) {
      return static_cast<R>(ctx.f1.f64);
    } else {
      return static_cast<R>(ctx.r3.u64);
    }
  }

  R operator()(rex::CallFrame& frame, uint8_t* base, Args... args) const {
    return (*this)(frame.ctx, base, args...);
  }

  /// Auto-isolating call: retrieves ctx/base internally, creates isolated
  /// context with 0x70 frame, calls function, returns result.
  R operator()(Args... args) const {
    auto* ts = rex::runtime::ThreadState::Get();
    PPCContext* parentCtx = ts->context();

    uint8_t* base = ts->memory()->virtual_membase();

    PPCContext ctx{};
    ctx.r1 = parentCtx->r1;
    ctx.r1.u32 -= 0x70;
    ctx.r13 = parentCtx->r13;
    ctx.fpscr = parentCtx->fpscr;

    auto tpl = std::make_tuple(args...);
    _translate_args_to_guest(ctx, base, tpl);
    fn(ctx, base);

    parentCtx->fpscr = ctx.fpscr;

    if constexpr (std::is_void_v<R>) {
      return;
    } else if constexpr (is_precise_v<R>) {
      return static_cast<R>(ctx.f1.f64);
    } else {
      return static_cast<R>(ctx.r3.u64);
    }
  }
};

}  // namespace rex::ppc

//=============================================================================
// REX_IMPORT - Typed callable import of a recompiled function
//=============================================================================
// Three explicit arguments, no hidden prefix transformations:
//   symbol:   the exact linker symbol to reference
//   callable: the name of the typed callable variable
//   sig:      the function signature (e.g. u32(u32, u32))
//
// The callable is internal-linkage on purpose: on ELF, a namespace-scope C++
// variable is not name-mangled, so an external-linkage callable named after a
// guest function would collide with the generated weak function alias and
// hijack its function table entry (MSVC decorates variables, hiding the bug).

#define REX_IMPORT(symbol, callable, sig)                          \
  REX_EXTERN(symbol);                                              \
  [[maybe_unused]] static rex::ppc::ImportFunction<sig> callable { \
    symbol                                                         \
  }
