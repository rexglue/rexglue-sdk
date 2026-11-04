/**
 * @file        rexglue/log_categories.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>

#include <rex/logging/log_category.h>

#include "codegen/codegen_categories.h"

REXLOG_CATEGORY(rexglue, "The rexglue command line tool")

#define REXGLUE_TRACE(fmt, ...) \
  REXLOG_CAT_TRACE(::rex::log::rexglue, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGLUE_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::rexglue, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGLUE_DEBUG(fmt, ...) \
  REXLOG_CAT_DEBUG(::rex::log::rexglue, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGLUE_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::rexglue, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGLUE_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::rexglue, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGLUE_ERROR(fmt, ...) \
  REXLOG_CAT_ERROR(::rex::log::rexglue, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGLUE_CRITICAL(fmt, ...) \
  REXLOG_CAT_CRITICAL(::rex::log::rexglue, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGLUE_WARN_ONCE(once, key, fmt, ...)                              \
  REXLOG_CAT_ONCE(::rex::log::rexglue, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGLUE_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::rexglue, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGLUE_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::rexglue, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGLUE_CHECK(cond, fmt, ...)                                            \
  do {                                                                           \
    if (!(cond)) {                                                               \
      REXGLUE_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                            \
  } while (0)

namespace rexglue {

/** Categories the tool owns, not those of connected modules. */
inline constexpr std::array<const rex::log::LogCategory*, 2> kLogCategories = {&rex::log::codegen,
                                                                               &rex::log::rexglue};

}  // namespace rexglue
