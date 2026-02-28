/**
 * @file        rex/logging/types.h
 * @brief       Log category types, constants, and configuration
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace rex {

/**
 * Lightweight handle identifying a log category.
 *
 * Internally a uint16_t index into the global category registry.
 * SDK built-in categories are constexpr globals; consumer categories
 * are obtained at runtime via RegisterLogCategory().
 */
struct LogCategoryId {
  uint16_t id;
  constexpr explicit LogCategoryId(uint16_t id) : id(id) {}
  constexpr bool operator==(const LogCategoryId&) const = default;
};

/**
 * Pre-allocated category IDs for SDK subsystems.
 * These are constexpr and resolve to array indices at zero cost.
 */
namespace log {
inline constexpr LogCategoryId Core{0};   /**< General/default messages */
inline constexpr LogCategoryId CPU{1};    /**< CPU emulation, PPC code */
inline constexpr LogCategoryId APU{2};    /**< Audio processing unit */
inline constexpr LogCategoryId GPU{3};    /**< Graphics processing unit */
inline constexpr LogCategoryId Kernel{4}; /**< Kernel/OS emulation */
inline constexpr LogCategoryId System{5}; /**< System emulation layer */
inline constexpr LogCategoryId FS{6};     /**< Filesystem operations */

/** Number of built-in SDK categories (consumer categories start after this). */
inline constexpr uint16_t kBuiltinCount = 7;
}  // namespace log

/**
 * Entry in the global category registry.
 * Each registered category has a human-readable name and its own spdlog logger.
 */
struct LogCategoryEntry {
  std::string name;                       /**< Category name (e.g. "core", "app.network") */
  std::shared_ptr<spdlog::logger> logger; /**< Per-category spdlog logger instance */
};

#if defined(NDEBUG)
inline constexpr auto kDefaultLogLevel = spdlog::level::info;
inline constexpr auto kVerboseLogLevel = spdlog::level::trace;
#else
inline constexpr auto kDefaultLogLevel = spdlog::level::debug;
inline constexpr auto kVerboseLogLevel = spdlog::level::trace;
#endif

/**
 * Configuration for the logging system.
 *
 * Fill out and pass to InitLogging(). All fields have sensible defaults.
 * String-keyed maps (category_levels, category_sinks) resolve by name so
 * they work for categories that haven't been registered yet.
 */
struct LogConfig {
  /** Global default log level applied to all categories unless overridden. */
  spdlog::level::level_enum default_level = spdlog::level::info;

  /** Whether to create a console (stdout) sink. */
  bool log_to_console = true;

  /** Whether the console sink uses ANSI color codes. */
  bool use_colors = true;

  /** Path to a log file, or nullptr for no file logging. */
  const char* log_file = nullptr;

  /** spdlog pattern string for the console sink. */
  std::string console_pattern = "[%^%l%$] [%n] [t%t] %v";

  /** spdlog pattern string for the file sink. */
  std::string file_pattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [t%t] %v";

  /** Messages at or above this level trigger an immediate flush. */
  spdlog::level::level_enum flush_level = spdlog::level::warn;

  /**
   * Per-category log level overrides.
   * Key is the category name (e.g. "core", "gpu", "app.network").
   * Categories not listed here use default_level.
   */
  std::map<std::string, spdlog::level::level_enum> category_levels;

  /**
   * Extra sinks added to ALL loggers alongside the default console/file sinks.
   * Useful for ring-buffer capture sinks, network sinks, etc.
   */
  std::vector<spdlog::sink_ptr> extra_sinks;

  /**
   * Per-category extra sinks. Key is category name.
   * Each vector of sinks is added ONLY to the specified category's logger.
   */
  std::map<std::string, std::vector<spdlog::sink_ptr>> category_sinks;

  /**
   * If true, per-category sinks in category_sinks REPLACE the default sinks
   * for that category rather than being added alongside them.
   * Default is false (additive).
   */
  bool category_sinks_exclusive = false;
};

/* =========================================================================
   Deprecated - LogCategory Enum Compatibility Layer
   =========================================================================
   These exist solely for backwards compatibility with code that uses
   the old LogCategory enum. Prefer LogCategoryId and the new API.
   ========================================================================= */

/** @deprecated Use LogCategoryId and rex::log:: constants instead. */
enum class LogCategory : size_t {
  Core,
  CPU,
  APU,
  GPU,
  Kernel,
  System,
  FS,
  Codegen,  // Kept in enum for ABI compat; not initialized by SDK
  Count
};

/** @deprecated Use FindCategory() or rex::log:: constants instead. */
std::optional<LogCategory> CategoryFromName(const std::string& name);

/** @deprecated Use GetLogger(LogCategoryId) instead. */
std::shared_ptr<spdlog::logger> GetLogger(LogCategory category);

/** @deprecated Use SetCategoryLevel(LogCategoryId, level) instead. */
void SetCategoryLevel(LogCategory category, spdlog::level::level_enum level);

}  // namespace rex
