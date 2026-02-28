/**
 * @file        codegen/codegen_logging.h
 * @brief       Codegen subsystem logging category registration
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging.h>

namespace rex::log {

/**
 * Log category for the code generation/recompilation subsystem.
 *
 * Registered at runtime via RegisterLogCategory(). The inline global
 * ensures registration happens exactly once (on first ODR-use).
 */
inline const LogCategoryId Codegen = RegisterLogCategory("codegen");

}  // namespace rex::log

/** @{ Codegen subsystem logging macros */
#define REXCODEGEN_TRACE(...) REXLOG_CAT_TRACE(::rex::log::Codegen, __VA_ARGS__)
#define REXCODEGEN_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::Codegen, __VA_ARGS__)
#define REXCODEGEN_INFO(...) REXLOG_CAT_INFO(::rex::log::Codegen, __VA_ARGS__)
#define REXCODEGEN_WARN(...) REXLOG_CAT_WARN(::rex::log::Codegen, __VA_ARGS__)
#define REXCODEGEN_ERROR(...) REXLOG_CAT_ERROR(::rex::log::Codegen, __VA_ARGS__)
#define REXCODEGEN_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::Codegen, __VA_ARGS__)
/** @} */
