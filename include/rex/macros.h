/**
 * @file        macros.h
 * @brief       Public macro catalog, core family
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <expected>
#include <utility>

#include <rex/platform.h>

/**
 * Export visibility for module entry points.
 *
 * Expands to `__declspec(dllexport)` on Windows and `__attribute__((visibility("default")))`
 * elsewhere. Usage: `extern "C" REX_API void f();`
 */
#if REX_PLATFORM_WIN32
#define REX_API __declspec(dllexport)
#else
#define REX_API __attribute__((visibility("default")))
#endif

/**
 * Declares the interface name and version constants on an interface class.
 *
 * Usage: `REX_INTERFACE(IFileSystem, "rex.filesystem", 1);`
 * Expands to:
 * @code
 *   static constexpr const char* kInterfaceName = "rex.filesystem";
 *   static constexpr uint32_t kInterfaceVersion = 1
 * @endcode
 */
#define REX_INTERFACE(Class, name, version)           \
  static constexpr const char* kInterfaceName = name; \
  static constexpr uint32_t kInterfaceVersion = version

/**
 * Generates the 2 C exports of a module DLL for a class that declares
 * `static constexpr std::array<rex::InterfaceEntry, N> kInterfaces`.
 *
 * Called exactly once per module DLL at namespace scope.
 * Usage: `REX_DECLARE_MODULE(rex::graphics::xenos::XenosModule)`
 * Expands to `rex_create_interface(name, version, out_status)`, which searches the table and
 * reports Ok, NotFound or VersionMismatch, and `rex_enumerate_interfaces(out_entries, out_count)`,
 * which returns the table. Nothing runs at DLL load, the module instance is created by the
 * table's getters on first request.
 */
#define REX_DECLARE_MODULE(ModuleClass)                                                       \
  extern "C" REX_API void* rex_create_interface(const char* name, uint32_t version,           \
                                                int32_t* out_status) {                        \
    if (!name) {                                                                              \
      if (out_status)                                                                         \
        *out_status = static_cast<int32_t>(::rex::InterfaceStatus::NotFound);                 \
      return nullptr;                                                                         \
    }                                                                                         \
    for (const ::rex::InterfaceEntry& entry : ModuleClass::kInterfaces) {                     \
      if (std::strcmp(entry.name, name) != 0)                                                 \
        continue;                                                                             \
      if (entry.version != version) {                                                         \
        if (out_status)                                                                       \
          *out_status = static_cast<int32_t>(::rex::InterfaceStatus::VersionMismatch);        \
        return nullptr;                                                                       \
      }                                                                                       \
      if (out_status)                                                                         \
        *out_status = static_cast<int32_t>(::rex::InterfaceStatus::Ok);                       \
      return entry.get();                                                                     \
    }                                                                                         \
    if (out_status)                                                                           \
      *out_status = static_cast<int32_t>(::rex::InterfaceStatus::NotFound);                   \
    return nullptr;                                                                           \
  }                                                                                           \
  extern "C" REX_API void rex_enumerate_interfaces(const ::rex::InterfaceEntry** out_entries, \
                                                   uint32_t* out_count) {                     \
    if (out_entries)                                                                          \
      *out_entries = ModuleClass::kInterfaces.data();                                         \
    if (out_count)                                                                            \
      *out_count = static_cast<uint32_t>(ModuleClass::kInterfaces.size());                    \
  }

/**
 * Token pasting helpers for the unwrap macros.
 */
#define REX_TRY_CONCAT_IMPL(a, b) a##b
#define REX_TRY_CONCAT(a, b) REX_TRY_CONCAT_IMPL(a, b)

/**
 * Unwraps a rex::Result into a declaration, or returns its error from the enclosing function.
 *
 * Usage: `REX_TRY(auto file, Open(path));`
 * Expands to a uniquely named temporary holding the result, an early
 * `return std::unexpected(error)` when it failed, and `decl = *std::move(temporary);`.
 * It is a statement, not an expression.
 */
#define REX_TRY(decl, expr)                                                         \
  auto REX_TRY_CONCAT(_rex_try_, __LINE__) = (expr);                                \
  if (!REX_TRY_CONCAT(_rex_try_, __LINE__)) {                                       \
    return std::unexpected(std::move(REX_TRY_CONCAT(_rex_try_, __LINE__)).error()); \
  }                                                                                 \
  decl = *std::move(REX_TRY_CONCAT(_rex_try_, __LINE__))

/**
 * Returns the error of a rex::Status or rex::Result expression from the enclosing function.
 *
 * Usage: `REX_TRY_VOID(module->Init());`
 */
#define REX_TRY_VOID(expr)                                        \
  do {                                                            \
    auto&& _rex_try_status = (expr);                              \
    if (!_rex_try_status) {                                       \
      return std::unexpected(std::move(_rex_try_status).error()); \
    }                                                             \
  } while (0)

/**
 * Returns `error` from the enclosing function when `cond` is false.
 *
 * Usage: `REX_CHECK(ptr != nullptr, rex::Error(rex::ErrorCategory::Validation, "null"));`
 */
#define REX_CHECK(cond, error)       \
  do {                               \
    if (!(cond)) {                   \
      return std::unexpected(error); \
    }                                \
  } while (0)
