/**
 * @file        ppc/detour.h
 * @brief       Typed hooks that wrap a recompiled function and run its original
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include <rex/assert.h>
#include <rex/export.h>
#include <rex/hook.h>
#include <rex/image_info.h>
#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime/thread_state.h>
#include <rex/types.h>

namespace rex::ppc {

namespace detail {

/** The state of 1 running detour, linked to the detour it interrupted on the same thread. */
struct DetourFrame {
  PPCContext* ctx;       /**< The register file the detour runs on. */
  u8* base;              /**< The guest memory base. */
  u32 entry_r1;          /**< The stack pointer at entry. */
  u64 entry_lr;          /**< The link register at entry. */
  bool ran_original;     /**< Set once the body has run the original. */
  DetourFrame* previous; /**< The detour this one interrupted, null for the outermost. */
};

/**
 * Returns the innermost running detour of the calling thread, shared by every module.
 *
 * @return The slot holding the innermost detour frame, null when no detour is running.
 */
REX_RUNTIME_API DetourFrame*& GetInnermostDetourFrame();

/**
 * Reports a register the image keeps in a local that a detour body changed through its context.
 * Warns once per flag.
 *
 * @param name The hooked function.
 * @param flags The codegen flags of the running image.
 * @param at_entry The context at detour entry.
 * @param at_exit The context at detour exit.
 * @param warned The flag that limits the warning to once.
 * @return The name of the first changed register, empty when none changed.
 */
REX_RUNTIME_API std::string CheckDetourLocalWrites(const char* name, const PPCCodegenFlags& flags,
                                                   const PPCContext& at_entry,
                                                   const PPCContext& at_exit,
                                                   std::atomic_flag& warned);

/**
 * Runs CheckDetourLocalWrites with the codegen flags of the runtime bound to the calling thread.
 * Does nothing when no runtime is bound.
 *
 * @param name The hooked function.
 * @param at_entry The context at detour entry.
 * @param at_exit The context at detour exit.
 * @param warned The flag that limits the warning to once.
 */
REX_RUNTIME_API void CheckBoundDetourLocalWrites(const char* name, const PPCContext& at_entry,
                                                 const PPCContext& at_exit,
                                                 std::atomic_flag& warned);

/**
 * Splits the signature of a detour body into its guest signature and its context parameter.
 *
 * @tparam F The body function pointer type.
 */
template <typename F>
struct DetourBody;

/**
 * A body without a context parameter.
 *
 * @tparam R The return type.
 * @tparam A The parameter types.
 */
template <typename R, typename... A>
struct DetourBody<R (*)(A...)> {
  static constexpr bool kContext = false; /**< True when the body takes the context. */
  using Result = R;                       /**< The return type. */
  using Guest = R(A...);                  /**< The signature the original has. */
  using Args = std::tuple<A...>;          /**< The parameters the registers are read into. */
};

/**
 * A body whose first parameter is the context.
 *
 * @tparam R The return type.
 * @tparam A The parameter types after the context.
 */
template <typename R, typename... A>
struct DetourBody<R (*)(PPCContext&, A...)> {
  static constexpr bool kContext = true; /**< True when the body takes the context. */
  using Result = R;                      /**< The return type. */
  using Guest = R(A...);                 /**< The signature the original has. */
  using Args = std::tuple<A...>;         /**< The parameters the registers are read into. */
};

/**
 * Runs the original on the context of the innermost detour.
 *
 * @tparam R The return type.
 * @tparam Args The argument types.
 * @param frame The innermost detour frame.
 * @param fn The original.
 * @param args The arguments.
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

/**
 * Reads the arguments of a detour from the register file and stack image.
 *
 * @tparam Guest The signature the original has.
 * @tparam Args The argument tuple type.
 * @tparam I The argument indices.
 * @param ctx The register file.
 * @param base The guest memory base.
 * @param args The tuple to fill.
 */
template <typename Guest, typename Args, size_t... I>
void ReadDetourArgs(PPCContext& ctx, u8* base, Args& args, std::index_sequence<I...>) noexcept {
  constexpr auto kPlaces = PlaceSignatureArgs(static_cast<Guest*>(nullptr));
  ((std::get<I>(args) =
        ArgTranslator::GetValue<std::tuple_element_t<I, Args>>(ctx, base, kPlaces[I])),
   ...);
}

/** Holds the frame of 1 running detour and restores the state the caller expects at scope exit. */
class DetourScope : public rex::NonCopyable {
 public:
  /**
   * Links a frame for the detour.
   *
   * @param ctx The register file.
   * @param base The guest memory base.
   */
  DetourScope(PPCContext& ctx, u8* base)
      : frame_{&ctx, base, ctx.r1.u32, ctx.lr, false, GetInnermostDetourFrame()} {
    GetInnermostDetourFrame() = &frame_;
  }

  /** Restores r1 and lr and unlinks the frame. */
  ~DetourScope() {
    frame_.ctx->r1.u32 = frame_.entry_r1;
    frame_.ctx->lr = frame_.entry_lr;
    GetInnermostDetourFrame() = frame_.previous;
  }

  /**
   * Returns the frame of the running detour.
   *
   * @return The frame.
   */
  const DetourFrame& GetFrame() const { return frame_; }

 private:
  DetourFrame frame_;
};

}  // namespace detail

/**
 * Typed callable for the original of a detoured function. Declared by REX_DETOUR.
 *
 * @tparam Sig The signature of the detour body.
 */
template <typename Sig>
struct DetourOriginal;

/**
 * The original of a detoured function of signature R(Args...).
 *
 * @tparam R The return type.
 * @tparam Args The parameter types.
 */
template <typename R, typename... Args>
struct DetourOriginal<R(Args...)> {
  PPCFunc& fn; /**< The recompiled original. */

  /**
   * Binds the original.
   *
   * @param original The recompiled original.
   */
  constexpr explicit DetourOriginal(PPCFunc& original) : fn(original) {}

  /**
   * Writes the arguments, runs the original on the live context of the running detour and returns
   * what it left in its result register. The context is not copied or restored, so the original's
   * other results stay in the registers.
   *
   * @param args The arguments.
   * @return The value of the result register.
   */
  R operator()(Args... args) const {
    detail::DetourFrame* frame = detail::GetInnermostDetourFrame();
    REX_ASSERT(frame != nullptr, "A detour original was called outside a detour body");
    return detail::CallOriginal<R>(*frame, fn, args...);
  }
};

/**
 * The original of a detoured function whose body takes the context first.
 *
 * @tparam R The return type.
 * @tparam Args The parameter types after the context.
 */
template <typename R, typename... Args>
struct DetourOriginal<R(PPCContext&, Args...)> : DetourOriginal<R(Args...)> {
  /**
   * Binds the original.
   *
   * @param original The recompiled original.
   */
  constexpr explicit DetourOriginal(PPCFunc& original) : DetourOriginal<R(Args...)>(original) {}
};

/**
 * Runs a detour body for a recompiled function. Reads the arguments, links a detour frame and
 * writes the result, with the stop checks and call depth of a hook.
 *
 * @tparam Body The detour body, optionally taking the context first.
 * @param ctx The register file.
 * @param base The guest memory base.
 * @param name The hooked function.
 */
template <auto Body>
__attribute__((noinline)) void RunDetour(PPCContext& ctx, u8* base, const char* name) {
  using Traits = detail::DetourBody<decltype(Body)>;
  using R = Traits::Result;
  static_assert(!HiddenReturnType<R>, "A detour cannot return a structure by value");

  detail::CheckHookContext(ctx, name);
  auto* thread_state = rex::runtime::ThreadState::Get();
  if (thread_state && thread_state->IsStopRequested()) {
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
  const PPCContext at_entry = ctx;
#endif
  detail::DetourScope scope(ctx, base);

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
      REXLOG_ERROR(cpu, "REX_DETOUR {} returned without running its original", name);
    }
    REX_ASSERT(frame.ran_original, "REX_DETOUR returned without running its original");
    if (ctx.r1.u32 != frame.entry_r1) {
      REXLOG_ERROR(cpu, "REX_DETOUR {} left a StackFrame open, r1 is {:#x} and was {:#x}", name,
                   ctx.r1.u32, frame.entry_r1);
    }
    REX_ASSERT(ctx.r1.u32 == frame.entry_r1, "REX_DETOUR left a StackFrame open");
    if (ctx.r2.u64 != at_entry.r2.u64) {
      REXLOG_ERROR(cpu, "REX_DETOUR {} changed r2", name);
    }
    REX_ASSERT(ctx.r2.u64 == at_entry.r2.u64, "REX_DETOUR changed r2");
    if (ctx.r13.u64 != at_entry.r13.u64) {
      REXLOG_ERROR(cpu, "REX_DETOUR {} changed r13", name);
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
  if (thread_state && thread_state->IsStopRequested()) {
    throw rex::ThreadStop{};
  }
}

}  // namespace rex::ppc

/**
 * Opens a profiling zone named after a detoured function. It expands to nothing unless profiling
 * is enabled.
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
 * Defines a typed hook that wraps a recompiled function and runs its original. The body reads the
 * arguments as parameters and calls original(args...) on every path. original writes the declared
 * arguments, runs the recompiled function on the live context and returns what it left in r3 or
 * f1, so a void detour passes the original's results through. A parameter list that starts with
 * PPCContext& ctx gives the body the register file. No detour stays running across a guest
 * SwitchToFiber, since the detour chain is thread local.
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
    rex::ppc::RunDetour<&name##_rex_detour::Body>(ctx, base, #name);                  \
  }                                                                                   \
  Ret name##_rex_detour::Body params
// NOLINTEND(bugprone-macro-parentheses)
