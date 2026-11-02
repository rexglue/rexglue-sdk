/**
 * @file        kernel/xam/forward.h
 * @brief       XAM export symbols that forward to the rexxam bodies
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <span>
#include <string_view>

#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/result.h>

#include "xam/xam_bodies.h"

namespace rex::kernel {

/** One XAM export symbol of the kernel and the body of rexxam it calls. */
class XAMForward {
 public:
  /** Adds the forward to the list that BindXAMForwards walks. */
  explicit XAMForward(std::string_view name);

  /** Calls the body. Without a bound body it logs an error and leaves the registers as they are. */
  void Call(PPCContext& ctx, u8* base) const {
    PPCFunc* function = function_.load(std::memory_order_acquire);
    if (function) {
      function(ctx, base);
    } else {
      ReportMissing();
    }
  }

 private:
  friend Result<void> BindXAMForwards(std::span<const xam::XAMBody> bodies);
  friend void UnbindXAMForwards();

  void ReportMissing() const;

  std::string_view name_;
  std::atomic<PPCFunc*> function_{nullptr};
  XAMForward* next_ = nullptr;
};

/**
 * Binds every forward to the body of the same name.
 *
 * @param bodies The bodies the XAM module serves.
 * @return Ok, or the name of an export that has a forward and no body or a body and no forward.
 */
[[nodiscard]] Result<void> BindXAMForwards(std::span<const xam::XAMBody> bodies);

void UnbindXAMForwards();

}  // namespace rex::kernel

/**
 * Defines the kernel symbol of an XAM export that calls the body rexxam registered under the same
 * name. A program that defines the symbol itself replaces it.
 *
 * @code
 * REX_XAM_FORWARD(__imp__XamGetSystemVersion)
 * @endcode
 */
#define REX_XAM_FORWARD(name)                              \
  namespace {                                              \
  ::rex::kernel::XAMForward rex_xam_forward_##name{#name}; \
  }                                                        \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(name) {      \
    rex_xam_forward_##name.Call(ctx, base);                \
  }
