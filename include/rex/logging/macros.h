/**
 * @file        rex/logging/macros.h
 * @brief       Logging macros - parameterized, legacy aliases, function-prefixed, thread-ID
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging/api.h>

/* Implementation macro - do not call directly. Uses raw pointer for zero
   ref-count overhead and gates on should_log() to skip format evaluation. */
#define REX_LOG_IMPL(cat, lvl, ...)                                                              \
  do {                                                                                           \
    auto* rex_log_ptr_ = ::rex::GetLoggerRaw(cat);                                               \
    if (rex_log_ptr_ && rex_log_ptr_->should_log(lvl))                                           \
      rex_log_ptr_->log(spdlog::source_loc{__FILE__, __LINE__, __FUNCTION__}, lvl, __VA_ARGS__); \
  } while (0)

/* --- Parameterized Macros (Primary API) --------------------------------- */

/** @{ */
#define REXLOG_CAT_TRACE(cat, ...) REX_LOG_IMPL(cat, spdlog::level::trace, __VA_ARGS__)
#define REXLOG_CAT_DEBUG(cat, ...) REX_LOG_IMPL(cat, spdlog::level::debug, __VA_ARGS__)
#define REXLOG_CAT_INFO(cat, ...) REX_LOG_IMPL(cat, spdlog::level::info, __VA_ARGS__)
#define REXLOG_CAT_WARN(cat, ...) REX_LOG_IMPL(cat, spdlog::level::warn, __VA_ARGS__)
#define REXLOG_CAT_ERROR(cat, ...) REX_LOG_IMPL(cat, spdlog::level::err, __VA_ARGS__)
#define REXLOG_CAT_CRITICAL(cat, ...) REX_LOG_IMPL(cat, spdlog::level::critical, __VA_ARGS__)
/** @} */

/* --- Function-Prefixed Parameterized Macros ----------------------------- */

/** @{ */
#define REXLOG_CAT_FN_TRACE(cat, fmt, ...) \
  REXLOG_CAT_TRACE(cat, "{}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_CAT_FN_DEBUG(cat, fmt, ...) \
  REXLOG_CAT_DEBUG(cat, "{}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_CAT_FN_INFO(cat, fmt, ...) \
  REXLOG_CAT_INFO(cat, "{}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_CAT_FN_WARN(cat, fmt, ...) \
  REXLOG_CAT_WARN(cat, "{}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_CAT_FN_ERROR(cat, fmt, ...) \
  REXLOG_CAT_ERROR(cat, "{}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_CAT_FN_CRITICAL(cat, fmt, ...) \
  REXLOG_CAT_CRITICAL(cat, "{}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
/** @} */

/* --- Thread-ID Parameterized Macros ------------------------------------- */

/** @{ */
#define REXLOG_CAT_TID_TRACE(cat, fmt, ...)                                  \
  REXLOG_CAT_TRACE(cat, "[T:{:08X}] {}: " fmt, ::rex::GetLogGuestThreadId(), \
                   __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_CAT_TID_DEBUG(cat, fmt, ...)                                  \
  REXLOG_CAT_DEBUG(cat, "[T:{:08X}] {}: " fmt, ::rex::GetLogGuestThreadId(), \
                   __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_CAT_TID_INFO(cat, fmt, ...)                                  \
  REXLOG_CAT_INFO(cat, "[T:{:08X}] {}: " fmt, ::rex::GetLogGuestThreadId(), \
                  __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_CAT_TID_WARN(cat, fmt, ...)                                  \
  REXLOG_CAT_WARN(cat, "[T:{:08X}] {}: " fmt, ::rex::GetLogGuestThreadId(), \
                  __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_CAT_TID_ERROR(cat, fmt, ...)                                  \
  REXLOG_CAT_ERROR(cat, "[T:{:08X}] {}: " fmt, ::rex::GetLogGuestThreadId(), \
                   __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_CAT_TID_CRITICAL(cat, fmt, ...)                                  \
  REXLOG_CAT_CRITICAL(cat, "[T:{:08X}] {}: " fmt, ::rex::GetLogGuestThreadId(), \
                      __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
/** @} */

/* --- Legacy Alias Macros - Core Category -------------------------------- */

/** @{ */
#define REXLOG_TRACE(...) REXLOG_CAT_TRACE(::rex::log::Core, __VA_ARGS__)
#define REXLOG_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::Core, __VA_ARGS__)
#define REXLOG_INFO(...) REXLOG_CAT_INFO(::rex::log::Core, __VA_ARGS__)
#define REXLOG_WARN(...) REXLOG_CAT_WARN(::rex::log::Core, __VA_ARGS__)
#define REXLOG_ERROR(...) REXLOG_CAT_ERROR(::rex::log::Core, __VA_ARGS__)
#define REXLOG_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::Core, __VA_ARGS__)
/** @} */

/* --- Legacy Alias Macros - Per-Subsystem -------------------------------- */

/** @{ CPU */
#define REXCPU_TRACE(...) REXLOG_CAT_TRACE(::rex::log::CPU, __VA_ARGS__)
#define REXCPU_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::CPU, __VA_ARGS__)
#define REXCPU_INFO(...) REXLOG_CAT_INFO(::rex::log::CPU, __VA_ARGS__)
#define REXCPU_WARN(...) REXLOG_CAT_WARN(::rex::log::CPU, __VA_ARGS__)
#define REXCPU_ERROR(...) REXLOG_CAT_ERROR(::rex::log::CPU, __VA_ARGS__)
#define REXCPU_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::CPU, __VA_ARGS__)
/** @} */

/** @{ APU */
#define REXAPU_TRACE(...) REXLOG_CAT_TRACE(::rex::log::APU, __VA_ARGS__)
#define REXAPU_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::APU, __VA_ARGS__)
#define REXAPU_INFO(...) REXLOG_CAT_INFO(::rex::log::APU, __VA_ARGS__)
#define REXAPU_WARN(...) REXLOG_CAT_WARN(::rex::log::APU, __VA_ARGS__)
#define REXAPU_ERROR(...) REXLOG_CAT_ERROR(::rex::log::APU, __VA_ARGS__)
#define REXAPU_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::APU, __VA_ARGS__)
/** @} */

/** @{ GPU */
#define REXGPU_TRACE(...) REXLOG_CAT_TRACE(::rex::log::GPU, __VA_ARGS__)
#define REXGPU_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::GPU, __VA_ARGS__)
#define REXGPU_INFO(...) REXLOG_CAT_INFO(::rex::log::GPU, __VA_ARGS__)
#define REXGPU_WARN(...) REXLOG_CAT_WARN(::rex::log::GPU, __VA_ARGS__)
#define REXGPU_ERROR(...) REXLOG_CAT_ERROR(::rex::log::GPU, __VA_ARGS__)
#define REXGPU_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::GPU, __VA_ARGS__)
/** @} */

/** @{ Kernel */
#define REXKRNL_TRACE(...) REXLOG_CAT_TRACE(::rex::log::Kernel, __VA_ARGS__)
#define REXKRNL_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::Kernel, __VA_ARGS__)
#define REXKRNL_INFO(...) REXLOG_CAT_INFO(::rex::log::Kernel, __VA_ARGS__)
#define REXKRNL_WARN(...) REXLOG_CAT_WARN(::rex::log::Kernel, __VA_ARGS__)
#define REXKRNL_ERROR(...) REXLOG_CAT_ERROR(::rex::log::Kernel, __VA_ARGS__)
#define REXKRNL_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::Kernel, __VA_ARGS__)
/** @} */

/** @{ System */
#define REXSYS_TRACE(...) REXLOG_CAT_TRACE(::rex::log::System, __VA_ARGS__)
#define REXSYS_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::System, __VA_ARGS__)
#define REXSYS_INFO(...) REXLOG_CAT_INFO(::rex::log::System, __VA_ARGS__)
#define REXSYS_WARN(...) REXLOG_CAT_WARN(::rex::log::System, __VA_ARGS__)
#define REXSYS_ERROR(...) REXLOG_CAT_ERROR(::rex::log::System, __VA_ARGS__)
#define REXSYS_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::System, __VA_ARGS__)
/** @} */

/** @{ Filesystem */
#define REXFS_TRACE(...) REXLOG_CAT_TRACE(::rex::log::FS, __VA_ARGS__)
#define REXFS_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::FS, __VA_ARGS__)
#define REXFS_INFO(...) REXLOG_CAT_INFO(::rex::log::FS, __VA_ARGS__)
#define REXFS_WARN(...) REXLOG_CAT_WARN(::rex::log::FS, __VA_ARGS__)
#define REXFS_ERROR(...) REXLOG_CAT_ERROR(::rex::log::FS, __VA_ARGS__)
#define REXFS_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::FS, __VA_ARGS__)
/** @} */

/* --- Legacy Function-Prefixed (Core) ------------------------------------ */

/** @{ */
#define REXLOGFN_TRACE(fmt, ...) \
  REXLOG_CAT_FN_TRACE(::rex::log::Core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXLOGFN_DEBUG(fmt, ...) \
  REXLOG_CAT_FN_DEBUG(::rex::log::Core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXLOGFN_INFO(fmt, ...) REXLOG_CAT_FN_INFO(::rex::log::Core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXLOGFN_WARN(fmt, ...) REXLOG_CAT_FN_WARN(::rex::log::Core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXLOGFN_ERROR(fmt, ...) \
  REXLOG_CAT_FN_ERROR(::rex::log::Core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXLOGFN_CRITICAL(fmt, ...) \
  REXLOG_CAT_FN_CRITICAL(::rex::log::Core, fmt __VA_OPT__(, ) __VA_ARGS__)
/** @} */

/* --- Custom Category Definition ----------------------------------------- */

/**
 * Define a custom log category with Meyers singleton, guaranteed to
 * initialize on first use regardless of static init order.
 *
 * Usage (in a header):
 *   REXLOG_DEFINE_CATEGORY(codegen)
 *   #define REXCODEGEN_TRACE(...) REXLOG_CAT_TRACE(::rex::log::codegen(), __VA_ARGS__)
 *   // ... etc for DEBUG, INFO, WARN, ERROR, CRITICAL
 *
 * Expands to an inline function rex::log::codegen() returning LogCategoryId.
 */
#define REXLOG_DEFINE_CATEGORY(name)                                          \
  namespace rex::log {                                                        \
  inline ::rex::LogCategoryId name() {                                        \
    static const ::rex::LogCategoryId id = ::rex::RegisterLogCategory(#name); \
    return id;                                                                \
  }                                                                           \
  }

/* --- Legacy Kernel Thread-ID -------------------------------------------- */

/** @{ */
#define REXKRNLFN_TRACE(fmt, ...) \
  REXLOG_CAT_TID_TRACE(::rex::log::Kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNLFN_DEBUG(fmt, ...) \
  REXLOG_CAT_TID_DEBUG(::rex::log::Kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNLFN_INFO(fmt, ...) \
  REXLOG_CAT_TID_INFO(::rex::log::Kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNLFN_WARN(fmt, ...) \
  REXLOG_CAT_TID_WARN(::rex::log::Kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNLFN_ERROR(fmt, ...) \
  REXLOG_CAT_TID_ERROR(::rex::log::Kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNLFN_CRITICAL(fmt, ...) \
  REXLOG_CAT_TID_CRITICAL(::rex::log::Kernel, fmt __VA_OPT__(, ) __VA_ARGS__)
/** @} */
