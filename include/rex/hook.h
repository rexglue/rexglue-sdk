/**
 * @file        hook.h
 * @brief       Hook authoring API: macros, typed imports, RAII helpers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstring>
#include <tuple>
#include <type_traits>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/types.h>

//=============================================================================
// Hook Macros
//=============================================================================

// Hook a recompiled function with an auto-marshaled native C++ function.
// The native function uses plain types (u32, mapped_u32, etc.) and
// HostToGuestFunction handles register translation automatically.
#ifdef REXGLUE_ENABLE_PROFILING
#include <tracy/Tracy.hpp>
#define REX_HOOK(subroutine, function)                  \
  extern "C" REX_FUNC(subroutine) {                     \
    ZoneNamedN(___tracy_hook_zone, #subroutine, true);  \
    rex::ppc::HostToGuestFunction<function>(ctx, base); \
  }
#else
#define REX_HOOK(subroutine, function)                  \
  extern "C" REX_FUNC(subroutine) {                     \
    rex::ppc::HostToGuestFunction<function>(ctx, base); \
  }
#endif

// Define a raw hook with direct ctx/base access.
#define REX_HOOK_RAW(name) extern "C" REX_FUNC(name)

// Stub: logs a warning when called.
#define REX_STUB(subroutine)              \
  extern "C" REX_WEAK_FUNC(subroutine) {  \
    (void)base;                           \
    REXKRNL_WARN("{} STUB", #subroutine); \
  }

#define REX_STUB_LOG(subroutine, msg)               \
  extern "C" REX_WEAK_FUNC(subroutine) {            \
    (void)base;                                     \
    REXKRNL_WARN("{} STUB - {}", #subroutine, msg); \
  }

#define REX_STUB_RETURN(subroutine, value)                                                \
  extern "C" REX_WEAK_FUNC(subroutine) {                                                  \
    (void)base;                                                                           \
    REXKRNL_WARN("{} STUB - returning {:#x}", #subroutine, static_cast<uint32_t>(value)); \
    ctx.r3.u64 = (value);                                                                 \
  }

// Export: hook + register in global registry for kernel ordinal lookup.
#define REX_EXPORT(name, function) \
  REX_HOOK(name, function)         \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);

#define REX_EXPORT_STUB(name) \
  REX_STUB(name)              \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);

namespace rex {

//=============================================================================
// CallFrame - Lightweight isolated calling context
//=============================================================================
// For side calls that must not disturb the outer hook's register state.
// Stack-allocated, NOT zero-initialized. Only copies the registers a callee
// actually needs from the parent context.

struct CallFrame {
  PPCContext ctx;
  PPCContext& parent_;

  explicit CallFrame(PPCContext& parent) : parent_(parent) {
    ctx.r1 = parent.r1;
    ctx.r13 = parent.r13;
    ctx.fpscr = parent.fpscr;
  }

  ~CallFrame() { parent_.fpscr = ctx.fpscr; }

  operator PPCContext&() { return ctx; }

  CallFrame(const CallFrame&) = delete;
  CallFrame& operator=(const CallFrame&) = delete;
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

    auto* ks = rex::system::kernel_state();
    uint8_t* base = ks->memory()->virtual_membase();

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

namespace rex {

//=============================================================================
// StackFrame - RAII guest stack allocation
//=============================================================================

class [[deprecated("Use rex::ppc::stack_push / stack_guard instead")]] StackFrame {
  PPCContext& ctx_;
  u32 size_;

 public:
  explicit StackFrame(PPCContext& ctx, u32 size) : ctx_(ctx), size_(size) { ctx_.r1.u32 -= size; }

  ~StackFrame() { ctx_.r1.u32 += size_; }

  u32 addr() const { return ctx_.r1.u32; }
  u32 addr(u32 offset) const { return ctx_.r1.u32 + offset; }

  void write_string(u32 offset, const char* str, u8* base) const {
    std::memcpy(rex::memory::GuestPtr(base, addr(offset)), str, std::strlen(str) + 1);
  }

  void write(u32 offset, const void* data, size_t len, u8* base) const {
    std::memcpy(rex::memory::GuestPtr(base, addr(offset)), data, len);
  }

  template <typename... A>
  size_t write_fmt(u32 offset, u8* base, fmt::format_string<A...> fmtstr, A&&... args) const {
    char* dst = rex::memory::GuestPtr<char*>(base, addr(offset));
    auto result = fmt::format_to_n(dst, size_ - offset - 1, fmtstr, std::forward<A>(args)...);
    *result.out = '\0';
    return result.size;
  }

  StackFrame(const StackFrame&) = delete;
  StackFrame& operator=(const StackFrame&) = delete;
};

}  // namespace rex

//=============================================================================
// REX_IMPORT - Typed callable import of a recompiled function
//=============================================================================
// Three explicit arguments, no hidden prefix transformations:
//   symbol:   the exact linker symbol to reference
//   callable: the name of the typed callable variable
//   sig:      the function signature (e.g. u32(u32, u32))

#define REX_IMPORT(symbol, callable, sig)         \
  REX_EXTERN(symbol);                             \
  inline rex::ppc::ImportFunction<sig> callable { \
    symbol                                        \
  }

//=============================================================================
// Legacy Compat Aliases
//=============================================================================

#define PPC_HOOK(s, f) REX_HOOK(s, f)
#define PPC_STUB(s) REX_STUB(s)
#define PPC_STUB_LOG(s, m) REX_STUB_LOG(s, m)
#define PPC_STUB_RETURN(s, v) REX_STUB_RETURN(s, v)
#define XBOXKRNL_EXPORT(n, f) REX_EXPORT(n, f)
#define XBOXKRNL_EXPORT_STUB(n) REX_EXPORT_STUB(n)
#define XAM_EXPORT(n, f) REX_EXPORT(n, f)
#define XAM_EXPORT_STUB(n) REX_EXPORT_STUB(n)
#define REXCRT_EXPORT(n, f) REX_HOOK(n, f)
#define REXCRT_EXPORT_STUB(n) REX_STUB(n)
