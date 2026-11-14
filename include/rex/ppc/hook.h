/**
 * @file        ppc/hook.h
 * @brief       Hook macros and typed hooks that wrap a recompiled function
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
#include <cstddef>
#include <string>
#include <utility>

#include <rex/memory/mapped_ptr.h>
#include <rex/platform.h>
#include <rex/logging/log_category.h>
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/call.h>
#include <rex/types.h>
#include <rex/assert.h>
#include <rex/ppc/image.h>
#include <rex/utility.h>

/**
 * Hooks a recompiled function with a native C++ function that takes plain types.
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
    rex::ppc::GuestToHostThunk<function>(ctx, base);             \
  }
#else
#define REX_HOOK(subroutine, function)                      \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(subroutine) { \
    rex::ppc::GuestToHostThunk<function>(ctx, base);        \
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
 * Defines a stub that logs a warning on the first call and at Dev level on later calls.
 *
 * @param subroutine The recompiled function symbol.
 *
 * @code
 * REX_STUB(ExampleFunction);
 * @endcode
 */
#define REX_STUB(subroutine)                                                         \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(subroutine) {                          \
    static constinit ::std::atomic_flag rex_stub_seen_;                              \
    ::rex::log::detail::WriteStub(::rex::log::cpu, rex_stub_seen_, #subroutine, {}); \
  }

/**
 * Defines a stub that logs a message on the first call and at Dev level on later calls.
 *
 * @param subroutine The recompiled function symbol.
 * @param msg The message logged with the stub.
 *
 * @code
 * REX_STUB_LOG(ExampleFunction, "skips the save");
 * @endcode
 */
#define REX_STUB_LOG(subroutine, msg)                                                 \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(subroutine) {                           \
    static constinit ::std::atomic_flag rex_stub_seen_;                               \
    ::rex::log::detail::WriteStub(::rex::log::cpu, rex_stub_seen_, #subroutine, msg); \
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
#define REX_STUB_RETURN(subroutine, value)                                           \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(subroutine) {                          \
    static constinit ::std::atomic_flag rex_stub_seen_;                              \
    const auto rex_stub_value_ = (value);                                            \
    ::rex::log::detail::WriteStub(                                                   \
        ::rex::log::cpu, rex_stub_seen_, #subroutine,                                \
        ::std::format("returning {:#x}", static_cast<::rex::u32>(rex_stub_value_))); \
    ctx.r3.u64 = rex_stub_value_;                                                    \
  }

/**
 * Defines a hook whose symbol a kernel export table links against.
 *
 * @param name The hook symbol.
 * @param function The host function the hook wraps.
 *
 * @code
 * REX_EXPORT(__imp__CloseHandle, rex::xam::CloseHandle_shim)
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
 * @param value The value returned in r3.
 *
 * @code
 * REX_EXPORT_STUB_RETURN(__imp__DmRegisterCommandProcessorEx, 0);
 * @endcode
 */
#define REX_EXPORT_STUB_RETURN(name, value) REX_STUB_RETURN(name, value)

/**
 * Declares a typed callable import of a recompiled function, with internal linkage so it cannot
 * collide with the generated weak alias on ELF.
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

namespace rex::ppc {

namespace detail {

/** The state of one running detour, linked to the detour it interrupted on the same thread. */
struct DetourFrame {
  PPCContext* ctx;
  u8* base;
  u32 entry_r1;
  u64 entry_lr;
  bool ran_original;     /**< Set once the body has run the original. */
  DetourFrame* previous; /**< The detour this one interrupted, null for the outermost. */
};

/**
 * Returns the innermost running detour of the calling thread, shared by every module, null when no
 * detour is running.
 */
REX_PPC_API DetourFrame*& GetInnermostDetourFrame();

/**
 * Reports the first register a detour body changed that the image keeps in a local. Warns once per
 * flag.
 *
 * @param flags The codegen flags of the running image.
 * @param at_entry The context at detour entry.
 * @param at_exit The context at detour exit.
 * @param warned The flag that limits the warning to once.
 * @return The name of the first changed register, empty when none changed.
 */
REX_PPC_API std::string CheckDetourLocalWrites(const char* name, const PPCCodegenFlags& flags,
                                               const PPCContext& at_entry,
                                               const PPCContext& at_exit, std::atomic_flag& warned);

/**
 * Runs CheckDetourLocalWrites with the codegen flags of the runtime bound to the calling thread.
 * Does nothing when no runtime is bound.
 *
 * @param at_entry The context at detour entry.
 * @param at_exit The context at detour exit.
 * @param warned The flag that limits the warning to once.
 */
REX_PPC_API void CheckBoundDetourLocalWrites(const char* name, const PPCContext& at_entry,
                                             const PPCContext& at_exit, std::atomic_flag& warned);

/**
 * Warns when a function reads an argument register that a detour did not declare.
 *
 * @param usage The argument registers the function reads at its entry.
 * @param declared_gpr The integer argument registers the detour declares, bit 0 is r3.
 * @param declared_fpr The float argument registers the detour declares, bit 0 is f1.
 * @return True when a register was missing and the warning was logged.
 */
REX_PPC_API bool CheckDetourArguments(const char* name, const PPCCallUsage& usage, u8 declared_gpr,
                                      u16 declared_fpr);

/**
 * Runs CheckDetourArguments with the usage of the runtime bound to the calling thread. Does nothing
 * when no runtime is bound or the image lists none.
 *
 * @param host The hook, as the image maps it.
 * @param declared_gpr The integer argument registers the detour declares, bit 0 is r3.
 * @param declared_fpr The float argument registers the detour declares, bit 0 is f1.
 */
REX_PPC_API void CheckBoundDetourArguments(const char* name, PPCFunc* host, u8 declared_gpr,
                                           u16 declared_fpr);

/** The argument registers the parameters of a detour cover. */
struct DeclaredArguments {
  u8 gpr_mask;  /**< Bit n is set for r(3 + n). */
  u16 fpr_mask; /**< Bit n is set for f(1 + n). */
};

/**
 * Returns the argument registers the parameters of a detour cover. A float covers the
 * integer register of its slot.
 */
template <typename Guest>
constexpr DeclaredArguments GetDeclaredArguments() {
  DeclaredArguments declared{};
  for (const ArgPlace& place : PlaceSignatureArgs(static_cast<Guest*>(nullptr))) {
    for (u32 slot = place.slot;
         slot < place.slot + place.slots && slot < ArgTranslator::kIntegerRegisters; ++slot) {
      declared.gpr_mask = static_cast<u8>(declared.gpr_mask | (1u << slot));
    }
    if (place.fpr >= 0) {
      declared.fpr_mask = static_cast<u16>(declared.fpr_mask | (1u << place.fpr));
    }
  }
  return declared;
}

/** Splits the signature of a detour body into its guest signature and its context parameter. */
template <typename F>
struct DetourBody;

/** A body without a context parameter. */
template <typename R, typename... A>
struct DetourBody<R (*)(A...)> {
  static constexpr bool kContext = false;
  using Result = R;
  using Guest = R(A...);         /**< The signature the original has. */
  using Args = std::tuple<A...>; /**< The parameters the registers are read into. */
};

/** A body whose first parameter is the context. */
template <typename R, typename... A>
struct DetourBody<R (*)(PPCContext&, A...)> {
  static constexpr bool kContext = true;
  using Result = R;
  using Guest = R(A...);         /**< The signature the original has. */
  using Args = std::tuple<A...>; /**< The parameters the registers are read into. */
};

/**
 * Runs the original on the context of the innermost detour.
 *
 * @return The value the original left in its result register.
 */
template <typename R, typename... Args>
R CallOriginal(DetourFrame& frame, PPCFunc& fn, Args... args) {
  static_assert(!HiddenReturnType<R>, "A detour cannot return a structure by value");
  PPCContext& ctx = *frame.ctx;
  u8* base = frame.base;
  frame.ran_original = true;
  auto arguments = std::make_tuple(args...);
  const u32 caller_r1 = ctx.r1.u32;
  const u64 caller_lr = ctx.lr;
  const bool opened = caller_r1 != frame.entry_r1;
  if (opened) {
    OpenCallFrame(ctx, base, (GetStackArgumentBytes<Args...>() + 15) & ~15u);
  }
  TranslateArgsToGuest(ctx, base, arguments);
  fn(ctx, base);
  if (opened) {
    ctx.r1.u32 = caller_r1;
    ctx.lr = caller_lr;
  }
  return LoadGuestResult<R>(base, ctx.r3, ctx.f1, ctx.v1, 0);
}

/** Reads the arguments of a detour from the register file and stack image. */
template <typename Guest, typename Args, size_t... I>
void ReadDetourArgs(PPCContext& ctx, u8* base, Args& args, std::index_sequence<I...>) noexcept {
  constexpr auto kPlaces = PlaceSignatureArgs(static_cast<Guest*>(nullptr));
  ((std::get<I>(args) =
        ArgTranslator::GetValue<std::tuple_element_t<I, Args>>(ctx, base, kPlaces[I])),
   ...);
}

/** Holds the frame of one running detour and restores r1, lr and the detour links at scope exit. */
class DetourScope : public rex::NonCopyable {
 public:
  /**
   * Links a frame for the detour.
   *
   * @param chained True when the body calls the original without the context, which needs the
   *        frame on the thread chain.
   */
  DetourScope(PPCContext& ctx, u8* base, bool chained)
      : frame_{&ctx,   base,  ctx.r1.u32,
               ctx.lr, false, chained ? GetInnermostDetourFrame() : nullptr},
        context_previous_(ctx.detour_frame),
        chained_(chained) {
    if (chained_) {
      GetInnermostDetourFrame() = &frame_;
    }
    ctx.detour_frame = &frame_;
  }

  /** Restores r1 and lr and unlinks the frame. */
  ~DetourScope() {
    frame_.ctx->r1.u32 = frame_.entry_r1;
    frame_.ctx->lr = frame_.entry_lr;
    frame_.ctx->detour_frame = context_previous_;
    if (chained_) {
      GetInnermostDetourFrame() = frame_.previous;
    }
  }

  const DetourFrame& GetFrame() const { return frame_; }

 private:
  DetourFrame frame_;
  DetourFrame* context_previous_;
  bool chained_;
};

}  // namespace detail

/** Typed callable for the original of a detoured function. Declared by REX_DETOUR. */
template <typename Sig>
struct DetourOriginal;

/** The original of a detoured function of signature R(Args...). */
template <typename R, typename... Args>
struct DetourOriginal<R(Args...)> {
  PPCFunc& fn;

  constexpr explicit DetourOriginal(PPCFunc& original) noexcept : fn(original) {}

  /**
   * Runs the original on the live context of the running detour, so its other results stay in the
   * registers.
   */
  R operator()(Args... args) const {
    detail::DetourFrame* frame = detail::GetInnermostDetourFrame();
    REX_ASSERT(frame != nullptr, "A detour original was called outside a detour body");
    return detail::CallOriginal<R>(*frame, fn, args...);
  }
};

/** The original of a detoured function whose body takes the context first. */
template <typename R, typename... Args>
struct DetourOriginal<R(PPCContext&, Args...)> {
  PPCFunc& fn;

  constexpr explicit DetourOriginal(PPCFunc& original) noexcept : fn(original) {}

  /** Runs the original on the register file of the running detour. */
  R operator()(PPCContext& ctx, Args... args) const {
    detail::DetourFrame* frame = ctx.detour_frame;
    REX_ASSERT(frame != nullptr, "A detour original was called outside a detour body");
    return detail::CallOriginal<R>(*frame, this->fn, args...);
  }
};

/**
 * Runs a detour body for a recompiled function.
 *
 * @tparam Body The detour body, optionally taking the context first.
 * @param host The hook, as the image maps it.
 */
template <auto Body>
__attribute__((noinline)) void RunDetour(PPCContext& ctx, u8* base, const char* name,
                                         [[maybe_unused]] PPCFunc* host) {
  using Traits = detail::DetourBody<decltype(Body)>;
  using R = Traits::Result;
  static_assert(!HiddenReturnType<R>, "A detour cannot return a structure by value");

  detail::CheckHookContext(ctx, name);
  if (ctx.stop_requested && ctx.stop_requested->load(std::memory_order_acquire)) {
    throw rex::ThreadStop{};
  }
#if !REX_PLATFORM_WIN32
  const rex::platform::HostCallDepth host_call_depth;
#endif
  typename Traits::Args args;
  detail::ReadDetourArgs<typename Traits::Guest>(
      ctx, base, args, std::make_index_sequence<std::tuple_size_v<typename Traits::Args>>());

#ifndef NDEBUG
  static constinit std::atomic_flag warned;
  static constinit std::atomic_flag arguments_checked;
  const PPCContext at_entry = ctx;
  if (!arguments_checked.test_and_set()) {
    constexpr detail::DeclaredArguments kDeclared =
        detail::GetDeclaredArguments<typename Traits::Guest>();
    detail::CheckBoundDetourArguments(name, host, kDeclared.gpr_mask, kDeclared.fpr_mask);
  }
#endif
  detail::DetourScope scope(ctx, base, !Traits::kContext);

  auto invoke = [&]() -> R {
    if constexpr (Traits::kContext) {
      return std::apply([&](auto&... values) -> R { return Body(ctx, values...); }, args);
    } else {
      return std::apply(Body, args);
    }
  };
  auto check_exit = [&] {
#ifndef NDEBUG
    const detail::DetourFrame& frame = scope.GetFrame();
    if (!frame.ran_original) {
      REXCPU_ERROR("REX_DETOUR {} returned without running its original", name);
    }
    REX_ASSERT(frame.ran_original, "REX_DETOUR returned without running its original");
    if (ctx.r1.u32 != frame.entry_r1) {
      REXCPU_ERROR("REX_DETOUR {} left a StackFrame open, r1 is {:#x} and was {:#x}", name,
                   ctx.r1.u32, frame.entry_r1);
    }
    REX_ASSERT(ctx.r1.u32 == frame.entry_r1, "REX_DETOUR left a StackFrame open");
    if (ctx.r2.u64 != at_entry.r2.u64) {
      REXCPU_ERROR("REX_DETOUR {} changed r2", name);
    }
    REX_ASSERT(ctx.r2.u64 == at_entry.r2.u64, "REX_DETOUR changed r2");
    if (ctx.r13.u64 != at_entry.r13.u64) {
      REXCPU_ERROR("REX_DETOUR {} changed r13", name);
    }
    REX_ASSERT(ctx.r13.u64 == at_entry.r13.u64, "REX_DETOUR changed r13");
    detail::CheckBoundDetourLocalWrites(name, at_entry, ctx, warned);
#else
    (void)name;
#endif
  };
  if constexpr (std::is_void_v<R>) {
    invoke();
    asm volatile("" ::: "memory");
    check_exit();
  } else {
    auto result = invoke();
    asm volatile("" ::: "memory");
    check_exit();
    StoreHostResult(ctx, base, 0, result);
  }
  if (ctx.stop_requested && ctx.stop_requested->load(std::memory_order_acquire)) {
    throw rex::ThreadStop{};
  }
}

}  // namespace rex::ppc

/**
 * Opens a profiling zone for a detoured function when profiling is enabled.
 *
 * @param name The recompiled function symbol.
 *
 * @code
 * REX_DETAIL_DETOUR_ZONE(ExampleFunction)
 * @endcode
 */
#ifdef REXGLUE_ENABLE_PROFILING
#define REX_DETAIL_DETOUR_ZONE(name) ZoneNamedN(___tracy_hook_zone, #name, TracyIsStarted);
#else
#define REX_DETAIL_DETOUR_ZONE(name)
#endif

/**
 * Defines a typed hook whose body reads the arguments as parameters and calls original(args...) on
 * every path, or original(ctx, args...) when the first parameter is PPCContext& ctx. No detour
 * stays running across a guest SwitchToFiber, since the detour chain is thread local.
 *
 * @param name The recompiled function symbol.
 * @param Ret The return type, written to r3 or f1 for the caller.
 * @param params The parameter list in parentheses, with names.
 *
 * @code
 * REX_DETOUR(ExampleFunction, u32, (u32 count)) {
 *   return original(count + 1) + 1;
 * }
 * @endcode
 */
// NOLINTBEGIN(bugprone-macro-parentheses): Ret and params declare a function
#define REX_DETOUR(name, Ret, params)                                                 \
  REX_EXTERN(__imp__##name);                                                          \
  struct name##_rex_detour {                                                          \
    static inline const rex::ppc::DetourOriginal<Ret params> original{__imp__##name}; \
    static Ret Body params;                                                           \
  };                                                                                  \
  REX_HOOK_RAW(name) {                                                                \
    REX_DETAIL_DETOUR_ZONE(name)                                                      \
    rex::ppc::RunDetour<&name##_rex_detour::Body>(ctx, base, #name, name);            \
  }                                                                                   \
  Ret name##_rex_detour::Body params
// NOLINTEND(bugprone-macro-parentheses)
