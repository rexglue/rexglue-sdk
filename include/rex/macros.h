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

#include <cstring>
#include <expected>
#include <utility>

#include <rex/platform.h>
#include <rex/types.h>

/**
 * Marks a declaration that rexcore exports. It expands to the export form while rexcore builds and
 * to the import form in every other binary.
 *
 * @code
 * class REX_API Clock;
 * REX_API void Break();
 * @endcode
 */
#if REX_PLATFORM_WIN32
#if defined(REX_BUILDING_CORE)
#define REX_API __declspec(dllexport)
#else
#define REX_API __declspec(dllimport)
#endif
#else
#define REX_API __attribute__((visibility("default")))
#endif

/**
 * Marks a C symbol that a module binary exports, such as the 2 entry points REX_DECLARE_MODULE
 * generates.
 *
 * @code
 * extern "C" REX_DETAIL_EXPORT void rexmod_test_set_sink(RexModTestSink sink);
 * @endcode
 */
#if REX_PLATFORM_WIN32
#define REX_DETAIL_EXPORT __declspec(dllexport)
#else
#define REX_DETAIL_EXPORT __attribute__((visibility("default")))
#endif

/**
 * Declares the interface name and version constants on an interface class.
 *
 * @param Class The interface class the constants belong to.
 * @param name The dotted interface name, a string literal.
 * @param version The interface version, an integer literal.
 *
 * @code
 * const char* name = IFileSystem::kInterfaceName;
 * rex::u32 version = IFileSystem::kInterfaceVersion;
 * @endcode
 */
#define REX_INTERFACE(Class, name, version)           \
  static constexpr const char* kInterfaceName = name; \
  static constexpr ::rex::u32 kInterfaceVersion = version

/**
 * Generates the 2 C exports of a module DLL for a class that declares
 * `static constexpr std::array<rex::InterfaceEntry, N> kInterfaces`.
 *
 * Called exactly once per module DLL at namespace scope. `rex_create_interface(name, version,
 * out_status)` searches the table and reports Ok, NotFound or VersionMismatch, and
 * `rex_enumerate_interfaces(out_entries, out_count)` returns the table. Nothing runs at DLL
 * load, the module instance is created by the table's getters on first request.
 *
 * @param ModuleClass The module class holding the interface table.
 *
 * @code
 * REX_DECLARE_MODULE(rex::graphics::XenosModule)
 * @endcode
 */
#define REX_DECLARE_MODULE(ModuleClass)                                                         \
  extern "C" REX_DETAIL_EXPORT void* rex_create_interface(const char* name, ::rex::u32 version, \
                                                          ::rex::i32* out_status) {             \
    if (!name) {                                                                                \
      if (out_status)                                                                           \
        *out_status = static_cast<::rex::i32>(::rex::InterfaceStatus::NotFound);                \
      return nullptr;                                                                           \
    }                                                                                           \
    for (const ::rex::InterfaceEntry& entry : ModuleClass::kInterfaces) {                       \
      if (std::strcmp(entry.name, name) != 0)                                                   \
        continue;                                                                               \
      if (entry.version != version) {                                                           \
        if (out_status)                                                                         \
          *out_status = static_cast<::rex::i32>(::rex::InterfaceStatus::VersionMismatch);       \
        return nullptr;                                                                         \
      }                                                                                         \
      if (out_status)                                                                           \
        *out_status = static_cast<::rex::i32>(::rex::InterfaceStatus::Ok);                      \
      return entry.get();                                                                       \
    }                                                                                           \
    if (out_status)                                                                             \
      *out_status = static_cast<::rex::i32>(::rex::InterfaceStatus::NotFound);                  \
    return nullptr;                                                                             \
  }                                                                                             \
  extern "C" REX_DETAIL_EXPORT void rex_enumerate_interfaces(                                   \
      const ::rex::InterfaceEntry** out_entries, ::rex::u32* out_count) {                       \
    if (out_entries)                                                                            \
      *out_entries = ModuleClass::kInterfaces.data();                                           \
    if (out_count)                                                                              \
      *out_count = static_cast<::rex::u32>(ModuleClass::kInterfaces.size());                    \
  }

/**
 * Pastes 2 tokens without expanding them first. Helper of REX_TRY_CONCAT.
 *
 * @param a The first token.
 * @param b The second token.
 *
 * The example yields the token _rex_try_12.
 *
 * @code
 * REX_TRY_CONCAT_IMPL(_rex_try_, 12)
 * @endcode
 */
#define REX_TRY_CONCAT_IMPL(a, b) a##b

/**
 * Pastes 2 tokens after expanding them, so `__LINE__` becomes the line number.
 *
 * @param a The first token.
 * @param b The second token.
 *
 * @code
 * auto REX_TRY_CONCAT(_rex_try_, __LINE__) = Open(path);
 * @endcode
 */
#define REX_TRY_CONCAT(a, b) REX_TRY_CONCAT_IMPL(a, b)

/**
 * Unwraps a rex::Result into a declaration, or returns its error from the enclosing function.
 *
 * Expands to a uniquely named temporary holding the result, an early
 * `return std::unexpected(error)` when it failed, and `decl = *std::move(temporary);`.
 * It is a statement, not an expression.
 *
 * @param decl The declaration that receives the value.
 * @param expr An expression of type rex::Result<T>.
 *
 * @code
 * REX_TRY(auto file, Open(path));
 * @endcode
 */
#define REX_TRY(decl, expr)                                                         \
  auto REX_TRY_CONCAT(_rex_try_, __LINE__) = (expr);                                \
  if (!REX_TRY_CONCAT(_rex_try_, __LINE__)) {                                       \
    return std::unexpected(std::move(REX_TRY_CONCAT(_rex_try_, __LINE__)).error()); \
  }                                                                                 \
  decl = *std::move(REX_TRY_CONCAT(_rex_try_, __LINE__)) /* NOLINT(bugprone-macro-parentheses) */

/**
 * Returns the error of a rex::Result expression from the enclosing function.
 *
 * @param expr An expression of type rex::Result<T>, usually rex::Result<void>.
 *
 * @code
 * REX_TRY_VOID(module->Init());
 * @endcode
 */
#define REX_TRY_VOID(expr)                                        \
  do {                                                            \
    auto&& _rex_try_status = (expr);                              \
    if (!_rex_try_status) {                                       \
      return std::unexpected(std::move(_rex_try_status).error()); \
    }                                                             \
  } while (0)

/**
 * Returns an error from the enclosing function when a condition is false.
 *
 * @param cond The condition that must hold.
 * @param error The rex::Error to return when it does not.
 *
 * @code
 * REX_CHECK(ptr != nullptr, rex::Error(rex::ErrorCategory::Validation, "null"));
 * @endcode
 */
#define REX_CHECK(cond, error)       \
  do {                               \
    if (!(cond)) {                   \
      return std::unexpected(error); \
    }                                \
  } while (0)
