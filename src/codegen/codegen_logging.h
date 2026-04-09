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

REXLOG_DEFINE_CATEGORY(codegen)

/** @{ Codegen subsystem logging macros */
#define REXCODEGEN_TRACE(...) REXLOG_CAT_TRACE(::rex::log::codegen(), __VA_ARGS__)
#define REXCODEGEN_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::codegen(), __VA_ARGS__)
#define REXCODEGEN_INFO(...) REXLOG_CAT_INFO(::rex::log::codegen(), __VA_ARGS__)
#define REXCODEGEN_WARN(...) REXLOG_CAT_WARN(::rex::log::codegen(), __VA_ARGS__)
#define REXCODEGEN_ERROR(...) REXLOG_CAT_ERROR(::rex::log::codegen(), __VA_ARGS__)
#define REXCODEGEN_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::codegen(), __VA_ARGS__)
/** @} */

/** @{ Codegen subsystem noisy logging macros */
#define REXCODEGEN_NOISY_TRACE(...) REXLOG_CAT_NOISY_TRACE(::rex::log::codegen(), __VA_ARGS__)
#define REXCODEGEN_NOISY_DEBUG(...) REXLOG_CAT_NOISY_DEBUG(::rex::log::codegen(), __VA_ARGS__)
/** @} */
