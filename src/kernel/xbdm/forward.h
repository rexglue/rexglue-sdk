/**
 * @file        kernel/xbdm/forward.h
 * @brief       XBDM export symbols that forward to the rexxbdm bodies
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <format>
#include <span>
#include <string_view>

#include <rex/logging/log_category.h>
#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/result.h>

#include "xbdm/xbdm_bodies.h"

namespace rex::kernel {

/** One XBDM export symbol of the kernel and the body of rexxbdm it calls. */
class XBDMForward {
 public:
  /**
   * Adds the forward to the list that BindXBDMForwards walks.
   *
   * @param name The export symbol.
   * @param fallback What a call runs while no body is bound. It logs and returns as the body does.
   */
  XBDMForward(std::string_view name, PPCFunc* fallback);

  /** Calls the body, or the fallback when rexxbdm is not loaded. */
  void Call(PPCContext& ctx, u8* base) const {
    PPCFunc* function = function_.load(std::memory_order_acquire);
    (function ? function : fallback_)(ctx, base);
  }

 private:
  friend Result<void> BindXBDMForwards(std::span<const ::rex::xbdm::XBDMBody> bodies);
  friend void UnbindXBDMForwards();

  std::string_view name_;
  PPCFunc* fallback_;
  std::atomic<PPCFunc*> function_{nullptr};
  XBDMForward* next_ = nullptr;
};

/**
 * Binds every forward to the body of the same name.
 *
 * @param bodies The bodies the XBDM module serves.
 * @return Ok, or the name of an export that has a forward and no body or a body and no forward.
 */
[[nodiscard]] Result<void> BindXBDMForwards(std::span<const ::rex::xbdm::XBDMBody> bodies);

void UnbindXBDMForwards();

}  // namespace rex::kernel

/**
 * Defines the kernel symbol of an XBDM export that calls the body rexxbdm registered under the
 * same name. Without rexxbdm the symbol logs that it was called, as the body does. A program that
 * defines the symbol itself replaces it.
 *
 * @code
 * REX_XBDM_FORWARD(__imp__DmGo)
 * @endcode
 */
#define REX_XBDM_FORWARD(name)                                                          \
  namespace {                                                                           \
  REX_FUNC(rex_xbdm_fallback_##name) {                                                  \
    static constinit ::std::atomic_flag rex_stub_seen_;                                 \
    ::rex::log::detail::WriteStub(::rex::log::cpu, rex_stub_seen_, #name, {});          \
  }                                                                                     \
  ::rex::kernel::XBDMForward rex_xbdm_forward_##name{#name, &rex_xbdm_fallback_##name}; \
  }                                                                                     \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(name) {                                   \
    rex_xbdm_forward_##name.Call(ctx, base);                                            \
  }

/**
 * Defines the kernel symbol of an XBDM export whose body returns a fixed value. Without rexxbdm
 * the symbol logs and returns that value, as the body does.
 *
 * @code
 * REX_XBDM_FORWARD_RETURN(__imp__DmRegisterCommandProcessorEx, 0)
 * @endcode
 */
#define REX_XBDM_FORWARD_RETURN(name, value)                                            \
  namespace {                                                                           \
  REX_FUNC(rex_xbdm_fallback_##name) {                                                  \
    static constinit ::std::atomic_flag rex_stub_seen_;                                 \
    const auto rex_stub_value_ = (value);                                               \
    ::rex::log::detail::WriteStub(                                                      \
        ::rex::log::cpu, rex_stub_seen_, #name,                                         \
        ::std::format("returning {:#x}", static_cast<::rex::u32>(rex_stub_value_)));    \
    ctx.r3.u64 = rex_stub_value_;                                                       \
  }                                                                                     \
  ::rex::kernel::XBDMForward rex_xbdm_forward_##name{#name, &rex_xbdm_fallback_##name}; \
  }                                                                                     \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(name) {                                   \
    rex_xbdm_forward_##name.Call(ctx, base);                                            \
  }
