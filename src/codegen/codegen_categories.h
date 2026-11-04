/**
 * @file        codegen/codegen_categories.h
 * @brief       The log category of the recompiler
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging/log.h>

REXLOG_CATEGORY(codegen, "Recompiler analysis and code generation")

#define REXCG_TRACE(fmt, ...) REXLOG_CAT_TRACE(::rex::log::codegen, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCG_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::codegen, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCG_DEBUG(fmt, ...) REXLOG_CAT_DEBUG(::rex::log::codegen, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCG_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::codegen, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCG_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::codegen, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCG_ERROR(fmt, ...) REXLOG_CAT_ERROR(::rex::log::codegen, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCG_CRITICAL(fmt, ...) \
  REXLOG_CAT_CRITICAL(::rex::log::codegen, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCG_WARN_ONCE(once, key, fmt, ...)                                \
  REXLOG_CAT_ONCE(::rex::log::codegen, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCG_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::codegen, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCG_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::codegen, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCG_CHECK(cond, fmt, ...)                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      REXCG_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                          \
  } while (0)
