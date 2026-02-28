/**
 * @file        logging.h
 * @brief       spdlog-based logging infrastructure
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

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

/* Implementation macro — do not call directly. Uses raw pointer for zero
   ref-count overhead and gates on should_log() to skip format evaluation. */
#define REX_LOG_IMPL(cat, lvl, ...)                                                              \
  do {                                                                                           \
    auto* rex_log_ptr_ = ::rex::GetLoggerRaw(cat);                                               \
    if (rex_log_ptr_ && rex_log_ptr_->should_log(lvl))                                           \
      rex_log_ptr_->log(spdlog::source_loc{__FILE__, __LINE__, __FUNCTION__}, lvl, __VA_ARGS__); \
  } while (0)

/* =========================================================================
   Parameterized Logging Macros (Primary API)
   =========================================================================
   Usage: REXLOG_CAT_INFO(rex::log::GPU, "draw call {}", count);
   ========================================================================= */

/** @{ */
/** Log at TRACE level to the given category. */
#define REXLOG_CAT_TRACE(cat, ...) REX_LOG_IMPL(cat, spdlog::level::trace, __VA_ARGS__)
/** Log at DEBUG level to the given category. */
#define REXLOG_CAT_DEBUG(cat, ...) REX_LOG_IMPL(cat, spdlog::level::debug, __VA_ARGS__)
/** Log at INFO level to the given category. */
#define REXLOG_CAT_INFO(cat, ...) REX_LOG_IMPL(cat, spdlog::level::info, __VA_ARGS__)
/** Log at WARN level to the given category. */
#define REXLOG_CAT_WARN(cat, ...) REX_LOG_IMPL(cat, spdlog::level::warn, __VA_ARGS__)
/** Log at ERROR level to the given category. */
#define REXLOG_CAT_ERROR(cat, ...) REX_LOG_IMPL(cat, spdlog::level::err, __VA_ARGS__)
/** Log at CRITICAL level to the given category. */
#define REXLOG_CAT_CRITICAL(cat, ...) REX_LOG_IMPL(cat, spdlog::level::critical, __VA_ARGS__)
/** @} */

/* =========================================================================
   Function-Prefixed Parameterized Macros
   =========================================================================
   Usage: REXLOG_CAT_FN_INFO(rex::log::GPU, "state={}", s);
   Output: [INFO] [gpu] [t1234] [MyFunction] state=foo
   ========================================================================= */

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

/* =========================================================================
   Thread-ID Parameterized Macros
   =========================================================================
   Usage: REXLOG_CAT_TID_INFO(rex::log::Kernel, "syscall {}", name);
   Output: [INFO] [krnl] [t1234] [T:0000ABCD] [MyFunction] syscall NtFoo
   ========================================================================= */

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

/* =========================================================================
   Legacy Alias Macros — Core Category (no category parameter)
   =========================================================================
   These preserve backwards compatibility with all existing call sites.
   Usage: REXLOG_INFO("message {}", val);  // logs to Core category
   ========================================================================= */

/** @{ */
#define REXLOG_TRACE(...) REXLOG_CAT_TRACE(::rex::log::Core, __VA_ARGS__)
#define REXLOG_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::Core, __VA_ARGS__)
#define REXLOG_INFO(...) REXLOG_CAT_INFO(::rex::log::Core, __VA_ARGS__)
#define REXLOG_WARN(...) REXLOG_CAT_WARN(::rex::log::Core, __VA_ARGS__)
#define REXLOG_ERROR(...) REXLOG_CAT_ERROR(::rex::log::Core, __VA_ARGS__)
#define REXLOG_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::Core, __VA_ARGS__)
/** @} */

/* =========================================================================
   Legacy Alias Macros — Per-Subsystem (no category parameter)
   ========================================================================= */

/** @{ CPU subsystem logging macros */
#define REXCPU_TRACE(...) REXLOG_CAT_TRACE(::rex::log::CPU, __VA_ARGS__)
#define REXCPU_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::CPU, __VA_ARGS__)
#define REXCPU_INFO(...) REXLOG_CAT_INFO(::rex::log::CPU, __VA_ARGS__)
#define REXCPU_WARN(...) REXLOG_CAT_WARN(::rex::log::CPU, __VA_ARGS__)
#define REXCPU_ERROR(...) REXLOG_CAT_ERROR(::rex::log::CPU, __VA_ARGS__)
#define REXCPU_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::CPU, __VA_ARGS__)
/** @} */

/** @{ APU subsystem logging macros */
#define REXAPU_TRACE(...) REXLOG_CAT_TRACE(::rex::log::APU, __VA_ARGS__)
#define REXAPU_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::APU, __VA_ARGS__)
#define REXAPU_INFO(...) REXLOG_CAT_INFO(::rex::log::APU, __VA_ARGS__)
#define REXAPU_WARN(...) REXLOG_CAT_WARN(::rex::log::APU, __VA_ARGS__)
#define REXAPU_ERROR(...) REXLOG_CAT_ERROR(::rex::log::APU, __VA_ARGS__)
#define REXAPU_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::APU, __VA_ARGS__)
/** @} */

/** @{ GPU subsystem logging macros */
#define REXGPU_TRACE(...) REXLOG_CAT_TRACE(::rex::log::GPU, __VA_ARGS__)
#define REXGPU_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::GPU, __VA_ARGS__)
#define REXGPU_INFO(...) REXLOG_CAT_INFO(::rex::log::GPU, __VA_ARGS__)
#define REXGPU_WARN(...) REXLOG_CAT_WARN(::rex::log::GPU, __VA_ARGS__)
#define REXGPU_ERROR(...) REXLOG_CAT_ERROR(::rex::log::GPU, __VA_ARGS__)
#define REXGPU_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::GPU, __VA_ARGS__)
/** @} */

/** @{ Kernel subsystem logging macros */
#define REXKRNL_TRACE(...) REXLOG_CAT_TRACE(::rex::log::Kernel, __VA_ARGS__)
#define REXKRNL_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::Kernel, __VA_ARGS__)
#define REXKRNL_INFO(...) REXLOG_CAT_INFO(::rex::log::Kernel, __VA_ARGS__)
#define REXKRNL_WARN(...) REXLOG_CAT_WARN(::rex::log::Kernel, __VA_ARGS__)
#define REXKRNL_ERROR(...) REXLOG_CAT_ERROR(::rex::log::Kernel, __VA_ARGS__)
#define REXKRNL_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::Kernel, __VA_ARGS__)
/** @} */

/** @{ System subsystem logging macros */
#define REXSYS_TRACE(...) REXLOG_CAT_TRACE(::rex::log::System, __VA_ARGS__)
#define REXSYS_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::System, __VA_ARGS__)
#define REXSYS_INFO(...) REXLOG_CAT_INFO(::rex::log::System, __VA_ARGS__)
#define REXSYS_WARN(...) REXLOG_CAT_WARN(::rex::log::System, __VA_ARGS__)
#define REXSYS_ERROR(...) REXLOG_CAT_ERROR(::rex::log::System, __VA_ARGS__)
#define REXSYS_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::System, __VA_ARGS__)
/** @} */

/** @{ Filesystem subsystem logging macros */
#define REXFS_TRACE(...) REXLOG_CAT_TRACE(::rex::log::FS, __VA_ARGS__)
#define REXFS_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::FS, __VA_ARGS__)
#define REXFS_INFO(...) REXLOG_CAT_INFO(::rex::log::FS, __VA_ARGS__)
#define REXFS_WARN(...) REXLOG_CAT_WARN(::rex::log::FS, __VA_ARGS__)
#define REXFS_ERROR(...) REXLOG_CAT_ERROR(::rex::log::FS, __VA_ARGS__)
#define REXFS_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::FS, __VA_ARGS__)
/** @} */

/* =========================================================================
   Legacy Function-Prefixed Macros (Core category)
   ========================================================================= */

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

/* =========================================================================
   Legacy Kernel Thread-ID Macros
   ========================================================================= */

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

/* =========================================================================
   Fatal Error Macros
   ========================================================================= */

/** Log a critical error to Core and abort. */
#define REX_FATAL(fmt, ...)                                     \
  do {                                                          \
    REXLOG_CRITICAL("[FATAL] " fmt __VA_OPT__(, ) __VA_ARGS__); \
    if (auto _l = ::rex::GetLogger())                           \
      _l->flush();                                              \
    std::abort();                                               \
  } while (0)

/** Log a critical error to a specific category and abort. */
#define REX_FATAL_CAT(cat, fmt, ...)                                     \
  do {                                                                   \
    REXLOG_CAT_CRITICAL(cat, "[FATAL] " fmt __VA_OPT__(, ) __VA_ARGS__); \
    if (auto* _l = ::rex::GetLoggerRaw(cat))                             \
      _l->flush();                                                       \
    std::abort();                                                        \
  } while (0)

/** Log a critical error with function name and abort. */
#define REX_FATAL_FN(fmt, ...)                                                    \
  do {                                                                            \
    REXLOG_CRITICAL("[FATAL] {}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__); \
    if (auto _l = ::rex::GetLogger())                                             \
      _l->flush();                                                                \
    std::abort();                                                                 \
  } while (0)

/** Check condition and abort with fatal error if false. */
#define REX_FATAL_IF(cond, fmt, ...)                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      REXLOG_CRITICAL("[FATAL] {}: check failed: " #cond " - " fmt, \
                      __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__);     \
      if (auto _l = ::rex::GetLogger())                             \
        _l->flush();                                                \
      std::abort();                                                 \
    }                                                               \
  } while (0)

/* =========================================================================
   Assertion Macros
   ========================================================================= */

/** Log error and assert (debug-only crash). */
#define REX_ASSERT(cond, msg)                                \
  do {                                                       \
    if (!(cond)) {                                           \
      REXLOG_ERROR("Assertion failed: {} - {}", #cond, msg); \
      assert(cond);                                          \
    }                                                        \
  } while (0)

/** Log error and return a value if condition fails. */
#define REX_ASSERT_RET(cond, msg, retval)                    \
  do {                                                       \
    if (!(cond)) {                                           \
      REXLOG_ERROR("Assertion failed: {} - {}", #cond, msg); \
      return retval;                                         \
    }                                                        \
  } while (0)

/** Log error and return void if condition fails. */
#define REX_ASSERT_RET_VOID(cond, msg)                       \
  do {                                                       \
    if (!(cond)) {                                           \
      REXLOG_ERROR("Assertion failed: {} - {}", #cond, msg); \
      return;                                                \
    }                                                        \
  } while (0)

/* =========================================================================
   Formatting Helpers
   ========================================================================= */

namespace log {

/** Format a 32-bit address as hex (e.g. "0x0040F000"). */
inline std::string ptr(uint32_t addr) {
  return fmt::format("0x{:08X}", addr);
}

/** Format a 64-bit address as hex, auto-selecting width. */
inline std::string ptr(uint64_t addr) {
  if (addr > 0xFFFFFFFFULL) {
    return fmt::format("0x{:016X}", addr);
  }
  return fmt::format("0x{:08X}", static_cast<uint32_t>(addr));
}

/** Format a void pointer. */
inline std::string ptr(const void* p) {
  return fmt::format("{}", p);
}

/** Format a typed pointer. */
template <typename T>
inline std::string ptr(T* p) {
  return fmt::format("{}", static_cast<const void*>(p));
}

/** Format a 32-bit value as hex (e.g. "0x1A"). */
inline std::string hex(uint32_t val) {
  return fmt::format("0x{:X}", val);
}

/** Format a 64-bit value as hex. */
inline std::string hex(uint64_t val) {
  return fmt::format("0x{:X}", val);
}

/** Format a boolean as "true" or "false". */
inline const char* boolean(bool b) {
  return b ? "true" : "false";
}

}  // namespace log

/* =========================================================================
   Deprecated — LogCategory Enum Compatibility Layer
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
