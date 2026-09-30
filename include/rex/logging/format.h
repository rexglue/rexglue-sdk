/**
 * @file        logging/format.h
 * @brief       Log records and the line format of text sinks
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include <rex/logging/level.h>
#include <rex/macros.h>
#include <rex/types.h>

namespace rex::log {

/** Timestamp written at the start of a line. */
enum class TimestampStyle : u8 {
  kNone,     /**< No timestamp. */
  kClock,    /**< Local wall clock with milliseconds. */
  kDateTime, /**< Local date and wall clock with milliseconds. */
  kElapsed,  /**< Seconds since the logger started, with a leading plus sign. */
};

/** What a text sink writes around each message. The level is never written as text. */
struct LineFormat {
  TimestampStyle timestamp = TimestampStyle::kNone; /**< Timestamp style. */
  bool category = true;                             /**< Category name column. */
  bool thread = false;                              /**< Host thread ID column. */
  bool color = true;                                /**< Level color, where the sink supports it. */
};

/** 1 message as sinks receive it. The views are valid for the duration of ISink::Write. */
struct Record {
  Level level = Level::kInfo;                 /**< Message level. */
  std::string_view category;                  /**< Category name. */
  std::string_view text;                      /**< Formatted message. */
  std::chrono::system_clock::time_point time; /**< Time the message was written. */
  u32 thread_id = 0;                          /**< Host thread ID. */
  u64 sequence = 0;                           /**< Process wide order, starting at 1. */
};

/**
 * Renders a record as 1 line without a line break.
 *
 * @param record The record.
 * @param format What to write around the message.
 * @return The line. Color is written as ANSI SGR codes.
 */
REX_API std::string FormatLine(const Record& record, const LineFormat& format);

}  // namespace rex::log
