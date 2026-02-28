/**
 * @file        rex/logging/api.h
 * @brief       Logging system function declarations and CVAR declarations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging/types.h>

#include <span>

#include <rex/cvar.h>

// Logging CVAR declarations (defined in logging.cpp)
REXCVAR_DECLARE(std::string, log_level);
REXCVAR_DECLARE(std::string, log_file);
REXCVAR_DECLARE(bool, log_verbose);
REXCVAR_DECLARE(bool, enable_console);

namespace rex {

/* =========================================================================
   Log Level Guidelines
   =========================================================================
   TRACE    - Per-instruction, per-iteration detail (massive output)
   DEBUG    - Development info, function entry/exit, intermediate state
   INFO     - Normal operational events, progress updates
   WARN     - Recoverable issues, fallback behaviors, unsupported features
   ERROR    - Serious problems affecting functionality
   CRITICAL - Fatal errors, memory corruption, unrecoverable state
   ========================================================================= */

/**
 * Initialize the logging system with full configuration.
 *
 * Creates shared sinks and loggers for all built-in categories.
 * Safe to call multiple times; subsequent calls update levels only.
 *
 * @param config  Logging configuration.
 */
void InitLogging(const LogConfig& config);

/**
 * Initialize logging with simple parameters (convenience overload).
 *
 * @param log_file  Path to log file, or nullptr for console-only.
 * @param level     Default log level for all categories.
 */
void InitLogging(const char* log_file = nullptr,
                 spdlog::level::level_enum level = spdlog::level::info);

/**
 * Flush all loggers and shut down the logging system.
 */
void ShutdownLogging();

/**
 * Register a new log category at runtime.
 *
 * The returned handle can be used with REXLOG_CAT_* macros and all
 * category-accepting API functions. The category's logger is created
 * with the current default sinks and any matching entries from the
 * stored LogConfig::category_levels and LogConfig::category_sinks.
 *
 * Thread-safe but intended to be called during startup.
 *
 * @param name  Human-readable category name (e.g. "app.network").
 * @return      Handle for the new category.
 */
LogCategoryId RegisterLogCategory(const char* name);

/**
 * Look up a category by name.
 *
 * @param name  Category name to search for (case-sensitive).
 * @return      Category handle, or std::nullopt if not found.
 */
std::optional<LogCategoryId> FindCategory(const std::string& name);

/**
 * Get a read-only view of all registered categories.
 *
 * @return  Span over the registry entries in registration order.
 */
std::span<const LogCategoryEntry> GetAllCategories();

/**
 * Get the raw logger pointer for a category (zero overhead).
 *
 * This is the fast path used by logging macros. Returns nullptr if
 * the category is not yet initialized.
 *
 * @param category  Category handle.
 * @return          Raw pointer to the spdlog logger (not owning).
 */
spdlog::logger* GetLoggerRaw(LogCategoryId category);

/**
 * Get the shared logger pointer for a category.
 *
 * Prefer GetLoggerRaw() in hot paths. This overload is useful when
 * you need to hold the logger or manipulate its sinks.
 *
 * @param category  Category handle.
 * @return          Shared pointer to the spdlog logger.
 */
std::shared_ptr<spdlog::logger> GetLogger(LogCategoryId category);

/**
 * Get the default (Core) logger.
 *
 * @return  Shared pointer to the Core category logger.
 */
std::shared_ptr<spdlog::logger> GetLogger();

/**
 * Set the log level for a specific category at runtime.
 *
 * @param category  Category handle.
 * @param level     New log level.
 */
void SetCategoryLevel(LogCategoryId category, spdlog::level::level_enum level);

/**
 * Set the log level for all registered categories.
 *
 * @param level  New log level.
 */
void SetAllLevels(spdlog::level::level_enum level);

/**
 * Register a CVAR change callback for the "log_level" CVAR.
 * Call this after InitLogging() to enable runtime level changes.
 */
void RegisterLogLevelCallback();

/**
 * Add a sink to all current and future loggers.
 *
 * @param sink  Shared pointer to the spdlog sink.
 */
void AddSink(spdlog::sink_ptr sink);

/**
 * Add a sink to a specific category's logger only.
 *
 * @param category  Category handle.
 * @param sink      Shared pointer to the spdlog sink.
 */
void AddSink(LogCategoryId category, spdlog::sink_ptr sink);

/**
 * Remove a sink from all loggers.
 *
 * @param sink  The sink to remove (matched by pointer identity).
 */
void RemoveSink(spdlog::sink_ptr sink);

/**
 * Remove a sink from a specific category's logger.
 *
 * @param category  Category handle.
 * @param sink      The sink to remove (matched by pointer identity).
 */
void RemoveSink(LogCategoryId category, spdlog::sink_ptr sink);

/**
 * Update the format pattern on the console sink.
 *
 * @param pattern  spdlog pattern string.
 */
void SetConsolePattern(const std::string& pattern);

/**
 * Update the format pattern on the file sink.
 *
 * @param pattern  spdlog pattern string.
 */
void SetFilePattern(const std::string& pattern);

/**
 * Parse a log level string to spdlog level enum.
 *
 * Accepts: "trace", "debug", "info", "warn"/"warning", "error"/"err",
 * "critical", "off". Case-insensitive.
 *
 * @param level_str  Level name string.
 * @return           Parsed level, or std::nullopt if invalid.
 */
std::optional<spdlog::level::level_enum> ParseLogLevel(const std::string& level_str);

/**
 * Parse log level from string, returning a default on failure.
 *
 * @param level_str      Level name string.
 * @param default_level  Fallback level if parsing fails.
 * @return               Parsed level or default_level.
 */
spdlog::level::level_enum ParseLogLevelOr(const std::string& level_str,
                                          spdlog::level::level_enum default_level);

/**
 * Build a LogConfig from CLI arguments and environment variables.
 *
 * Precedence: CLI args > environment (REX_LOG_LEVEL) > build-type default.
 *
 * @param log_file         Path to log file, or nullptr.
 * @param cli_level        Global level from CLI (empty string = not set).
 * @param category_levels  Per-category level overrides from CLI.
 * @return                 Populated LogConfig.
 */
LogConfig BuildLogConfig(const char* log_file, const std::string& cli_level,
                         const std::map<std::string, std::string>& category_levels);

/**
 * Get the current guest thread ID for logging.
 *
 * This is a weak symbol that returns 0 by default. Override in the
 * runtime library to return the actual guest thread ID.
 *
 * @return  Guest thread ID, or 0 if not in guest context.
 */
uint32_t GetLogGuestThreadId();

}  // namespace rex
