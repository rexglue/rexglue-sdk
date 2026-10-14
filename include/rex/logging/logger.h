/**
 * @file        logging/logger.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <rex/logging/category.h>
#include <rex/logging/config.h>
#include <rex/logging/format.h>
#include <rex/logging/level.h>
#include <rex/logging/sink.h>
#include <rex/macros.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex {

/** Runtime control of the logger. Core serves it. */
class ILogger {
 public:
  REX_INTERFACE(ILogger, "rex.log", 1);

  virtual ~ILogger() = default;

  /**
   * Registers categories and adds a log.<name> cvar for each.
   *
   * @param categories Descriptors that outlive the registration.
   * @return Ok, or an error naming a duplicate name, a name that collides with a log cvar, or a
   *         full table.
   */
  [[nodiscard]] virtual Result<void> Register(std::span<const log::Category* const> categories) = 0;

  /**
   * Unregisters categories and removes their cvars. Their slots stay and follow the default level.
   *
   * @param categories Descriptors passed to Register.
   */
  virtual void Unregister(std::span<const log::Category* const> categories) = 0;

  /**
   * Sets a level. The default level applies to every category without its own level.
   *
   * @param category Category name, or empty for the default level.
   * @param level The level, or empty to follow the default again.
   * @return Ok, or an error for an unknown category or an empty default level.
   */
  [[nodiscard]] virtual Result<void> SetLevel(std::string_view category,
                                              std::optional<log::Level> level) = 0;

  /**
   * Returns a level.
   *
   * @param category Category name. Empty names the default level.
   * @return The effective level, or an error for an unknown category.
   */
  [[nodiscard]] virtual Result<log::Level> GetLevel(std::string_view category) const = 0;

  /**
   * Returns every category, registered or not, in the order they were first seen.
   *
   * @return 1 row per category.
   */
  virtual std::vector<log::CategoryInfo> GetCategories() const = 0;

  /**
   * Adds a sink. The caller keeps ownership and removes the sink before destroying it.
   *
   * @param sink The sink. Adding a sink twice has no effect.
   */
  virtual void AddSink(log::ISink& sink) = 0;

  /**
   * Removes a sink added with AddSink. Returns after any write in progress on it finishes.
   *
   * @param sink The sink.
   */
  virtual void RemoveSink(log::ISink& sink) = 0;

  /**
   * Sets the format of the console and debugger sinks the logger created from Config::log.
   *
   * @param format The format.
   */
  virtual void SetFormat(const log::LineFormat& format) = 0;

  /** Writes buffered output of every sink. */
  virtual void Flush() = 0;

  /**
   * Returns the log file.
   *
   * @return The path of the open log file, empty when no file is open.
   */
  virtual std::filesystem::path GetFilePath() const = 0;
};

namespace log {

/** Writes buffered output of every sink. For crash handlers that hold no API handle. */
REX_API void Flush();

/**
 * Starts the logger for a process that logs outside rex::Init, such as a command line tool. A
 * later rex::Init keeps these sinks and levels and adds only its categories and cvars.
 *
 * @param config The configuration.
 * @return Ok, or an error when the logger is already started or a category fails to register.
 */
[[nodiscard]] REX_API Result<void> Start(const LogConfig& config);

/** Stops a logger started with Start. Call it after the last rex::Shutdown. */
REX_API void Stop();

}  // namespace log

}  // namespace rex
