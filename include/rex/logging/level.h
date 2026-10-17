/**
 * @file        logging/level.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <string_view>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::log {

/** Severity of a log message, least severe first. */
enum class Level : u8 {
  kTrace,    /**< Per instruction, per packet and per iteration detail. */
  kDev,      /**< Detail for people porting a title. ReX debug output and repeat stub calls. */
  kDebug,    /**< State changes and intermediate values. */
  kInfo,     /**< Operational events a user reads. At most a few lines per subsystem start. */
  kWarn,     /**< Recoverable problems, fallbacks and unsupported features. */
  kError,    /**< Failures that lose functionality. */
  kCritical, /**< Unrecoverable state, written right before an abort. */
  kOff,      /**< Threshold only. Nothing is written at it. */
};

/** Lowercase level names in level order, kOff last. */
inline constexpr std::array<std::string_view, 8> kLevelNames = {
    "trace", "dev", "debug", "info", "warn", "error", "critical", "off"};

/** Number of levels a message can carry, kTrace through kCritical. */
inline constexpr size_t kMessageLevelCount = 7;

/**
 * Parses a level name.
 *
 * @param text 1 of trace, dev, debug, info, warn, error, critical or off, in any case.
 * @return The level, or an error naming the text.
 */
[[nodiscard]] REX_API Result<Level> ParseLevel(std::string_view text);

/**
 * Returns the lowercase name of a level.
 *
 * @param level The level.
 * @return The name ParseLevel accepts.
 */
REX_API std::string_view LevelName(Level level);

}  // namespace rex::log
