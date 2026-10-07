/**
 * @file        logging/macros.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <format>

#include <rex/logging/category.h>
#include <rex/logging/level.h>

/**
 * Declares a log category as the inline constant initialized object rex::log::name. Used at
 * global scope, in a header that every file writing to the category includes.
 *
 * @param name Category name, a lowercase identifier.
 * @param description Shown by the console and by ILogger::Categories.
 *
 * @code
 * REXLOG_CATEGORY(bd, "reblue recompiled code")
 * @endcode
 */
#define REXLOG_CATEGORY(name, description)                        \
  namespace rex::log {                                            \
  inline constinit ::rex::log::Category name{#name, description}; \
  }

/**
 * Declares rex::log::table, the array of category addresses a module passes to
 * ILogger::Register. Used at global scope.
 *
 * @param table Name of the table.
 * @param ... Category names.
 *
 * @code
 * REXLOG_TABLE(kGPULog, gpu)
 * logger->Register(rex::log::kGPULog);
 * @endcode
 */
#define REXLOG_TABLE(table, ...)                                            \
  namespace rex::log {                                                      \
  inline constexpr auto table = ::rex::log::detail::MakeTable(__VA_ARGS__); \
  }

/**
 * Writes a message when the category is enabled at the level. The arguments are not evaluated
 * otherwise. The REXLOG_<LEVEL> macros expand to it.
 *
 * @param category Category name.
 * @param level A rex::log::Level.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_DETAIL_WRITE(gpu, ::rex::log::Level::kInfo, "{} pipelines", count);
 * @endcode
 */
#define REXLOG_DETAIL_WRITE(category, level, fmt, ...)                          \
  do {                                                                          \
    if (::rex::log::category.Enabled(level)) {                                  \
      ::rex::log::detail::Write(::rex::log::category, level,                    \
                                ::std::format(fmt __VA_OPT__(, ) __VA_ARGS__)); \
    }                                                                           \
  } while (0)

/**
 * Writes a Trace message.
 *
 * @param category Category name.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_TRACE(gpu, "packet {:08X}", header);
 * @endcode
 */
#define REXLOG_TRACE(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kTrace, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Dev message.
 *
 * @param category Category name.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_DEV(title, "{}", text);
 * @endcode
 */
#define REXLOG_DEV(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kDev, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Debug message.
 *
 * @param category Category name.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_DEBUG(system, "thread {} created", id);
 * @endcode
 */
#define REXLOG_DEBUG(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kDebug, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes an Info message.
 *
 * @param category Category name.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_INFO(ui, "SDL video driver {}", name);
 * @endcode
 */
#define REXLOG_INFO(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kInfo, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Warn message.
 *
 * @param category Category name.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_WARN(audio, "falling back to {} channels", channels);
 * @endcode
 */
#define REXLOG_WARN(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kWarn, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes an Error message.
 *
 * @param category Category name.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_ERROR(filesystem, "{} could not be opened", path);
 * @endcode
 */
#define REXLOG_ERROR(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kError, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Critical message.
 *
 * @param category Category name.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_CRITICAL(core, "heap corrupted at {:08X}", address);
 * @endcode
 */
#define REXLOG_CRITICAL(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kCritical, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes an Error message and returns a value from the enclosing function.
 *
 * @param category Category name.
 * @param value Returned value.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REX_FAIL(kernel, X_STATUS_INVALID_PARAMETER, "{} is out of range", index);
 * @endcode
 */
#define REX_FAIL(category, value, fmt, ...)                 \
  do {                                                      \
    REXLOG_ERROR(category, fmt __VA_OPT__(, ) __VA_ARGS__); \
    return value;                                           \
  } while (0)

#include <rex/logging/categories.h>
