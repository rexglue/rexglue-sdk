/**
 * @file        xam/xam_bodies.h
 * @brief       The XAM export bodies the kernel forwards to (private)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <span>
#include <string_view>

#include <rex/kernel/shim.h>
#include <rex/module.h>
#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/recompiled_code.h>

namespace rex::xam {

/** One implemented XAM export. */
struct XAMBody {
  std::string_view name; /**< The export symbol, such as __imp__XamGetSystemVersion. */
  PPCFunc* function;     /**< The body the kernel symbol forwards to. */
};

/** The bodies of the XAM exports. Only the private factory finds it. */
REX_DECLARE_PRIVATE_INTERFACE(IXAMExports, 1)
  virtual ~IXAMExports() = default;

  /**
   * Lists every implemented export.
   */
  [[nodiscard]] virtual std::span<const XAMBody> GetBodies() const = 0;
};

/** The bodies REX_XAM_BODY registered, served as IXAMExports. */
class XAMExports final : public IXAMExports {
 public:
  [[nodiscard]] std::span<const XAMBody> GetBodies() const override;
};

/** Adds a body to the list XAMExports serves. REX_XAM_BODY creates one per body. */
class XAMBodyRegistrar {
 public:
  XAMBodyRegistrar(std::string_view name, PPCFunc* function);
};

}  // namespace rex::xam

#ifdef REXGLUE_ENABLE_PROFILING
#include <tracy/Tracy.hpp>
#define REX_XAM_BODY_ZONE(name) ZoneNamedN(___tracy_hook_zone, #name, TracyIsStarted)
#else
#define REX_XAM_BODY_ZONE(name)
#endif

/**
 * Defines the body of an XAM export. The kernel symbol of the same name forwards to it.
 *
 * @param name The export symbol.
 * @param function The native function the body calls.
 *
 * @code
 * REX_XAM_BODY(__imp__XamGetSystemVersion, rex::xam::XamGetSystemVersion_entry)
 * @endcode
 */
#define REX_XAM_BODY(name, function)                                                        \
  namespace {                                                                               \
  REX_FUNC(rex_xam_body_##name) {                                                           \
    REX_XAM_BODY_ZONE(name);                                                                \
    rex::ppc::GuestToHostThunk<function>(ctx, base);                                        \
  }                                                                                         \
  const ::rex::xam::XAMBodyRegistrar rex_xam_registrar_##name{#name, &rex_xam_body_##name}; \
  }

/**
 * Defines a shim function and the body its rexshim_ symbol forwards to. The name must be in the
 * shim registry.
 *
 * @param ... The parameter list.
 *
 * @code
 * REX_XAM_SHIM_FN(GetFileSize, u32, u32 hFile, MappedU32 lpFileSizeHigh) {
 *   return 0;
 * }
 * @endcode
 */
#define REX_XAM_SHIM_FN(NAME, RET, ...)                                          \
  static_assert(::rex::kernel::shim::IsRegistered(#NAME),                        \
                "REX_XAM_SHIM_FN(" #NAME "): name is not in the shim registry"); \
  static RET NAME##_shim(__VA_ARGS__);                                           \
  REX_XAM_BODY(rexshim_##NAME, NAME##_shim) static RET NAME##_shim(__VA_ARGS__)
