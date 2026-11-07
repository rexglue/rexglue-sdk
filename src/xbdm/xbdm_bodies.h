/**
 * @file        xbdm/xbdm_bodies.h
 * @brief       The XBDM export bodies the kernel forwards to (private)
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
#include <rex/module.h>
#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/recompiled_code.h>

namespace rex::xbdm {

/** One XBDM export body. */
struct XBDMBody {
  std::string_view name; /**< The export symbol, such as __imp__DmGo. */
  PPCFunc* function;     /**< The body the kernel symbol forwards to. */
};

/** The bodies of the XBDM exports. Only the private factory finds it. */
REX_DECLARE_PRIVATE_INTERFACE(IXBDMExports, 1)
  virtual ~IXBDMExports() = default;

  /**
   * Lists every export body.
   */
  [[nodiscard]] virtual std::span<const XBDMBody> GetBodies() const = 0;
};

/** The bodies REX_XBDM_BODY registered, served as IXBDMExports. */
class XBDMExports final : public IXBDMExports {
 public:
  [[nodiscard]] std::span<const XBDMBody> GetBodies() const override;
};

/** Adds a body to the list XBDMExports serves. REX_XBDM_BODY creates one per body. */
class XBDMBodyRegistrar {
 public:
  XBDMBodyRegistrar(std::string_view name, PPCFunc* function);
};

}  // namespace rex::xbdm

/**
 * Defines the body of an XBDM export that only logs that it was called. The kernel symbol of the
 * same name forwards to it.
 *
 * @param name The export symbol.
 *
 * @code
 * REX_XBDM_BODY(__imp__DmGo)
 * @endcode
 */
#define REX_XBDM_BODY(name)                                                                     \
  namespace {                                                                                   \
  REX_FUNC(rex_xbdm_body_##name) {                                                              \
    static constinit ::std::atomic_flag rex_stub_seen_;                                         \
    ::rex::log::detail::WriteStub(::rex::log::cpu, rex_stub_seen_, #name, {});                  \
  }                                                                                             \
  const ::rex::xbdm::XBDMBodyRegistrar rex_xbdm_registrar_##name{#name, &rex_xbdm_body_##name}; \
  }

/**
 * Defines the body of an XBDM export that logs that it was called and returns a fixed value. The
 * kernel symbol of the same name forwards to it.
 *
 * @param name The export symbol.
 * @param value The value returned in r3.
 *
 * @code
 * REX_XBDM_BODY_RETURN(__imp__DmRegisterCommandProcessorEx, 0)
 * @endcode
 */
#define REX_XBDM_BODY_RETURN(name, value)                                                       \
  namespace {                                                                                   \
  REX_FUNC(rex_xbdm_body_##name) {                                                              \
    static constinit ::std::atomic_flag rex_stub_seen_;                                         \
    const auto rex_stub_value_ = (value);                                                       \
    ::rex::log::detail::WriteStub(                                                              \
        ::rex::log::cpu, rex_stub_seen_, #name,                                                 \
        ::std::format("returning {:#x}", static_cast<::rex::u32>(rex_stub_value_)));            \
    ctx.r3.u64 = rex_stub_value_;                                                               \
  }                                                                                             \
  const ::rex::xbdm::XBDMBodyRegistrar rex_xbdm_registrar_##name{#name, &rex_xbdm_body_##name}; \
  }
