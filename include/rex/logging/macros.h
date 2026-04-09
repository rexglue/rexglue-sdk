/**
 * @file        rex/logging/macros.h
 * @brief       Logging macros - parameterized, per-subsystem aliases, category definition
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

#define REX_LOG_NOISY_IMPL(cat, lvl, ...) \
  do {                                    \
    if (!REXCVAR_GET(log_noisy))          \
      break;                              \
    REX_LOG_IMPL(cat, lvl, __VA_ARGS__);  \
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

/* --- Noisy Parameterized Macros (cvar-gated) ------------------------------ */

#define REXLOG_CAT_NOISY_TRACE(cat, ...) REX_LOG_NOISY_IMPL(cat, spdlog::level::trace, __VA_ARGS__)
#define REXLOG_CAT_NOISY_DEBUG(cat, ...) REX_LOG_NOISY_IMPL(cat, spdlog::level::debug, __VA_ARGS__)

/* --- Per-Subsystem Alias Macros - Core Category -------------------------- */

/** @{ */
#define REXLOG_TRACE(...) REXLOG_CAT_TRACE(::rex::log::core(), __VA_ARGS__)
#define REXLOG_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::core(), __VA_ARGS__)
#define REXLOG_INFO(...) REXLOG_CAT_INFO(::rex::log::core(), __VA_ARGS__)
#define REXLOG_WARN(...) REXLOG_CAT_WARN(::rex::log::core(), __VA_ARGS__)
#define REXLOG_ERROR(...) REXLOG_CAT_ERROR(::rex::log::core(), __VA_ARGS__)
#define REXLOG_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::core(), __VA_ARGS__)
/** @} */

/* --- Per-Subsystem Alias Macros ------------------------------------------ */

/** @{ CPU */
#define REXCPU_TRACE(...) REXLOG_CAT_TRACE(::rex::log::cpu(), __VA_ARGS__)
#define REXCPU_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::cpu(), __VA_ARGS__)
#define REXCPU_INFO(...) REXLOG_CAT_INFO(::rex::log::cpu(), __VA_ARGS__)
#define REXCPU_WARN(...) REXLOG_CAT_WARN(::rex::log::cpu(), __VA_ARGS__)
#define REXCPU_ERROR(...) REXLOG_CAT_ERROR(::rex::log::cpu(), __VA_ARGS__)
#define REXCPU_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::cpu(), __VA_ARGS__)
/** @} */

/** @{ APU */
#define REXAPU_TRACE(...) REXLOG_CAT_TRACE(::rex::log::apu(), __VA_ARGS__)
#define REXAPU_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::apu(), __VA_ARGS__)
#define REXAPU_INFO(...) REXLOG_CAT_INFO(::rex::log::apu(), __VA_ARGS__)
#define REXAPU_WARN(...) REXLOG_CAT_WARN(::rex::log::apu(), __VA_ARGS__)
#define REXAPU_ERROR(...) REXLOG_CAT_ERROR(::rex::log::apu(), __VA_ARGS__)
#define REXAPU_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::apu(), __VA_ARGS__)
/** @} */

/** @{ GPU */
#define REXGPU_TRACE(...) REXLOG_CAT_TRACE(::rex::log::gpu(), __VA_ARGS__)
#define REXGPU_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::gpu(), __VA_ARGS__)
#define REXGPU_INFO(...) REXLOG_CAT_INFO(::rex::log::gpu(), __VA_ARGS__)
#define REXGPU_WARN(...) REXLOG_CAT_WARN(::rex::log::gpu(), __VA_ARGS__)
#define REXGPU_ERROR(...) REXLOG_CAT_ERROR(::rex::log::gpu(), __VA_ARGS__)
#define REXGPU_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::gpu(), __VA_ARGS__)
/** @} */

/** @{ Kernel */
#define REXKRNL_TRACE(...) REXLOG_CAT_TRACE(::rex::log::krnl(), __VA_ARGS__)
#define REXKRNL_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::krnl(), __VA_ARGS__)
#define REXKRNL_INFO(...) REXLOG_CAT_INFO(::rex::log::krnl(), __VA_ARGS__)
#define REXKRNL_WARN(...) REXLOG_CAT_WARN(::rex::log::krnl(), __VA_ARGS__)
#define REXKRNL_ERROR(...) REXLOG_CAT_ERROR(::rex::log::krnl(), __VA_ARGS__)
#define REXKRNL_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::krnl(), __VA_ARGS__)
/** @} */

/** @{ System */
#define REXSYS_TRACE(...) REXLOG_CAT_TRACE(::rex::log::sys(), __VA_ARGS__)
#define REXSYS_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::sys(), __VA_ARGS__)
#define REXSYS_INFO(...) REXLOG_CAT_INFO(::rex::log::sys(), __VA_ARGS__)
#define REXSYS_WARN(...) REXLOG_CAT_WARN(::rex::log::sys(), __VA_ARGS__)
#define REXSYS_ERROR(...) REXLOG_CAT_ERROR(::rex::log::sys(), __VA_ARGS__)
#define REXSYS_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::sys(), __VA_ARGS__)
/** @} */

/** @{ Filesystem */
#define REXFS_TRACE(...) REXLOG_CAT_TRACE(::rex::log::fs(), __VA_ARGS__)
#define REXFS_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::fs(), __VA_ARGS__)
#define REXFS_INFO(...) REXLOG_CAT_INFO(::rex::log::fs(), __VA_ARGS__)
#define REXFS_WARN(...) REXLOG_CAT_WARN(::rex::log::fs(), __VA_ARGS__)
#define REXFS_ERROR(...) REXLOG_CAT_ERROR(::rex::log::fs(), __VA_ARGS__)
#define REXFS_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::fs(), __VA_ARGS__)
/** @} */

/* --- Noisy Aliases - Per-Subsystem ---------------------------------------- */

#define REXLOG_NOISY_TRACE(...) REXLOG_CAT_NOISY_TRACE(::rex::log::core(), __VA_ARGS__)
#define REXLOG_NOISY_DEBUG(...) REXLOG_CAT_NOISY_DEBUG(::rex::log::core(), __VA_ARGS__)
#define REXCPU_NOISY_TRACE(...) REXLOG_CAT_NOISY_TRACE(::rex::log::cpu(), __VA_ARGS__)
#define REXCPU_NOISY_DEBUG(...) REXLOG_CAT_NOISY_DEBUG(::rex::log::cpu(), __VA_ARGS__)
#define REXAPU_NOISY_TRACE(...) REXLOG_CAT_NOISY_TRACE(::rex::log::apu(), __VA_ARGS__)
#define REXAPU_NOISY_DEBUG(...) REXLOG_CAT_NOISY_DEBUG(::rex::log::apu(), __VA_ARGS__)
#define REXGPU_NOISY_TRACE(...) REXLOG_CAT_NOISY_TRACE(::rex::log::gpu(), __VA_ARGS__)
#define REXGPU_NOISY_DEBUG(...) REXLOG_CAT_NOISY_DEBUG(::rex::log::gpu(), __VA_ARGS__)
#define REXKRNL_NOISY_TRACE(...) REXLOG_CAT_NOISY_TRACE(::rex::log::krnl(), __VA_ARGS__)
#define REXKRNL_NOISY_DEBUG(...) REXLOG_CAT_NOISY_DEBUG(::rex::log::krnl(), __VA_ARGS__)
#define REXSYS_NOISY_TRACE(...) REXLOG_CAT_NOISY_TRACE(::rex::log::sys(), __VA_ARGS__)
#define REXSYS_NOISY_DEBUG(...) REXLOG_CAT_NOISY_DEBUG(::rex::log::sys(), __VA_ARGS__)
#define REXFS_NOISY_TRACE(...) REXLOG_CAT_NOISY_TRACE(::rex::log::fs(), __VA_ARGS__)
#define REXFS_NOISY_DEBUG(...) REXLOG_CAT_NOISY_DEBUG(::rex::log::fs(), __VA_ARGS__)

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

// For dynamic parents (defined via REXLOG_DEFINE_CATEGORY)
#define REXLOG_DEFINE_SUBCATEGORY(child, parent)                     \
  namespace rex::log {                                               \
  inline ::rex::LogCategoryId parent##_##child() {                   \
    static const ::rex::LogCategoryId id =                           \
        ::rex::RegisterLogSubcategory(#child, ::rex::log::parent()); \
    return id;                                                       \
  }                                                                  \
  }

/* --- Built-in SDK Categories ---------------------------------------------- */

REXLOG_DEFINE_CATEGORY(core)
REXLOG_DEFINE_CATEGORY(cpu)
REXLOG_DEFINE_CATEGORY(apu)
REXLOG_DEFINE_CATEGORY(gpu)
REXLOG_DEFINE_CATEGORY(krnl)
REXLOG_DEFINE_CATEGORY(sys)
REXLOG_DEFINE_CATEGORY(fs)
