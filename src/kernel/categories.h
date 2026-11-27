/**
 * @file        kernel/categories.h
 * @brief       The log categories of the kernel exports
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging/log_category.h>

REXLOG_CATEGORY(kernel, "Kernel exports of xboxkrnl")

#define REXKRNL_TRACE(fmt, ...) REXLOG_CAT_TRACE(::rex::log::kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNL_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNL_DEBUG(fmt, ...) REXLOG_CAT_DEBUG(::rex::log::kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNL_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNL_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNL_ERROR(fmt, ...) REXLOG_CAT_ERROR(::rex::log::kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNL_CRITICAL(fmt, ...) \
  REXLOG_CAT_CRITICAL(::rex::log::kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNL_WARN_ONCE(once, key, fmt, ...)                             \
  REXLOG_CAT_ONCE(::rex::log::kernel, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNL_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::kernel, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNL_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNL_CHECK(cond, fmt, ...)                                            \
  do {                                                                           \
    if (!(cond)) {                                                               \
      REXKRNL_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                            \
  } while (0)
