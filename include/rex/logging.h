/**
 * @file        logging.h
 * @brief       Unified spdlog-based logging infrastructure
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
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rex/cvar.h>

// Logging CVAR declarations (defined in flags.cpp)
REXCVAR_DECLARE(std::string, log_level);
REXCVAR_DECLARE(std::string, log_file);
REXCVAR_DECLARE(bool, log_verbose);
REXCVAR_DECLARE(bool, enable_console);
namespace rex {

//=============================================================================
// Log Level Guidelines
//=============================================================================
// TRACE    - Per-instruction, per-iteration detail (massive output)
// DEBUG    - Development info, function entry/exit, intermediate state
// INFO     - Normal operational events, progress updates
// WARN     - Recoverable issues, fallback behaviors, unsupported features
// ERROR    - Serious problems affecting functionality
// CRITICAL - Fatal errors, memory corruption, unrecoverable state
//=============================================================================

//=============================================================================
// Logging Categories (Subsystems)
//=============================================================================

enum class LogCategory : size_t {
  Core,     // General/default messages (rex.core)
  CPU,      // CPU emulation, PPC code (rex.cpu)
  APU,      // Audio processing unit (rex.apu)
  GPU,      // Graphics processing unit (rex.gpu)
  Kernel,   // Kernel/OS emulation (rex.krnl)
  System,   // System emulation layer (rex.sys)
  FS,       // Filesystem operations (rex.fs)
  Codegen,  // Code generation/recompilation (rex.codegen)
  Count     // Sentinel for array sizing
};

// Short category names for logger registration
inline constexpr const char* kCategoryNames[] = {"core", "cpu", "apu", "gpu",
                                                 "krnl", "sys", "fs",  "codegen"};

/// Get string name for a category
constexpr const char* CategoryName(LogCategory cat) {
  return kCategoryNames[std::to_underlying(cat)];
}

//=============================================================================
// Default Runtime Log Levels
//=============================================================================

#if defined(NDEBUG)
inline constexpr auto kDefaultLogLevel = spdlog::level::info;
inline constexpr auto kVerboseLogLevel = spdlog::level::trace;
#else
inline constexpr auto kDefaultLogLevel = spdlog::level::debug;
inline constexpr auto kVerboseLogLevel = spdlog::level::trace;
#endif

//=============================================================================
// Logging Configuration
//=============================================================================

struct LogConfig {
  const char* log_file = nullptr;
  spdlog::level::level_enum default_level = spdlog::level::info;
  bool log_to_console = true;
  bool use_colors = true;

  // Per-category levels (nullopt = use default_level)
  std::array<std::optional<spdlog::level::level_enum>, std::to_underlying(LogCategory::Count)>
      category_levels{};
};

//=============================================================================
// Initialization Functions
//=============================================================================

/// Initialize logging with full configuration
void InitLogging(const LogConfig& config);

/// Initialize logging with simple parameters (backward compatible)
void InitLogging(const char* log_file = nullptr,
                 spdlog::level::level_enum level = spdlog::level::info);

/// Shutdown logging (flush and cleanup)
void ShutdownLogging();

/// Get logger for a specific category
std::shared_ptr<spdlog::logger> GetLogger(LogCategory category);

/// Get the default (Core) logger
std::shared_ptr<spdlog::logger> GetLogger();

/// Set log level for a specific category at runtime
void SetCategoryLevel(LogCategory category, spdlog::level::level_enum level);

/// Set log level for all categories at runtime
void SetAllLevels(spdlog::level::level_enum level);

/// Register CVAR change callback for log_level
/// Call this after InitLogging() to enable runtime level changes
void RegisterLogLevelCallback();

//=============================================================================
// CLI Helper Functions
//=============================================================================

/// Parse a log level string to spdlog level enum
/// @param level_str String like "trace", "debug", "info", "warn", "error", "critical"
/// @return Parsed level, or nullopt if invalid
std::optional<spdlog::level::level_enum> ParseLogLevel(const std::string& level_str);

/// Parse log level from string, returning default on failure
spdlog::level::level_enum ParseLogLevelOr(const std::string& level_str,
                                          spdlog::level::level_enum default_level);

/// Get category enum from string name
/// @param name Category name (e.g., "core", "cpu", "gpu", "kernel", "fs")
/// @return Category enum, or nullopt if not found
std::optional<LogCategory> CategoryFromName(const std::string& name);

/// Build LogConfig from CLI arguments and environment
/// Precedence: CLI > Environment (REX_LOG_LEVEL) > Default (info)
LogConfig BuildLogConfig(const char* log_file, const std::string& cli_level,
                         const std::map<std::string, std::string>& category_levels);

//=============================================================================
// Guest Thread ID for Logging
//=============================================================================

/// Get the current guest thread ID for logging
uint32_t GetLogGuestThreadId();

//=============================================================================
// Internal Macros (DO NOT USE DIRECTLY)
//=============================================================================
// Use REXLOG_* or REX{SUBSYS}_* macros instead.
//
// We use spdlog::logger::log() directly instead of SPDLOG_LOGGER_* macros
// to bypass compile-time stripping via SPDLOG_ACTIVE_LEVEL.

#define REX_LOG_IMPL(cat, lvl, ...)                                                          \
  do {                                                                                       \
    if (auto _rex_log = ::rex::GetLogger(::rex::LogCategory::cat)) {                         \
      _rex_log->log(spdlog::source_loc{__FILE__, __LINE__, __FUNCTION__}, lvl, __VA_ARGS__); \
    }                                                                                        \
  } while (0)

#define REX_LOG_TRACE_CAT(cat, ...) REX_LOG_IMPL(cat, spdlog::level::trace, __VA_ARGS__)
#define REX_LOG_DEBUG_CAT(cat, ...) REX_LOG_IMPL(cat, spdlog::level::debug, __VA_ARGS__)
#define REX_LOG_INFO_CAT(cat, ...) REX_LOG_IMPL(cat, spdlog::level::info, __VA_ARGS__)
#define REX_LOG_WARN_CAT(cat, ...) REX_LOG_IMPL(cat, spdlog::level::warn, __VA_ARGS__)
#define REX_LOG_ERROR_CAT(cat, ...) REX_LOG_IMPL(cat, spdlog::level::err, __VA_ARGS__)
#define REX_LOG_CRITICAL_CAT(cat, ...) REX_LOG_IMPL(cat, spdlog::level::critical, __VA_ARGS__)

//=============================================================================
// Generic Logging Macros (Core category)
//=============================================================================

#define REXLOG_TRACE(...) REX_LOG_TRACE_CAT(Core, __VA_ARGS__)
#define REXLOG_DEBUG(...) REX_LOG_DEBUG_CAT(Core, __VA_ARGS__)
#define REXLOG_INFO(...) REX_LOG_INFO_CAT(Core, __VA_ARGS__)
#define REXLOG_WARN(...) REX_LOG_WARN_CAT(Core, __VA_ARGS__)
#define REXLOG_ERROR(...) REX_LOG_ERROR_CAT(Core, __VA_ARGS__)
#define REXLOG_CRITICAL(...) REX_LOG_CRITICAL_CAT(Core, __VA_ARGS__)

//=============================================================================
// CPU Subsystem Macros
//=============================================================================

#define REXCPU_TRACE(...) REX_LOG_TRACE_CAT(CPU, __VA_ARGS__)
#define REXCPU_DEBUG(...) REX_LOG_DEBUG_CAT(CPU, __VA_ARGS__)
#define REXCPU_INFO(...) REX_LOG_INFO_CAT(CPU, __VA_ARGS__)
#define REXCPU_WARN(...) REX_LOG_WARN_CAT(CPU, __VA_ARGS__)
#define REXCPU_ERROR(...) REX_LOG_ERROR_CAT(CPU, __VA_ARGS__)
#define REXCPU_CRITICAL(...) REX_LOG_CRITICAL_CAT(CPU, __VA_ARGS__)

//=============================================================================
// APU Subsystem Macros
//=============================================================================

#define REXAPU_TRACE(...) REX_LOG_TRACE_CAT(APU, __VA_ARGS__)
#define REXAPU_DEBUG(...) REX_LOG_DEBUG_CAT(APU, __VA_ARGS__)
#define REXAPU_INFO(...) REX_LOG_INFO_CAT(APU, __VA_ARGS__)
#define REXAPU_WARN(...) REX_LOG_WARN_CAT(APU, __VA_ARGS__)
#define REXAPU_ERROR(...) REX_LOG_ERROR_CAT(APU, __VA_ARGS__)
#define REXAPU_CRITICAL(...) REX_LOG_CRITICAL_CAT(APU, __VA_ARGS__)

//=============================================================================
// GPU Subsystem Macros
//=============================================================================

#define REXGPU_TRACE(...) REX_LOG_TRACE_CAT(GPU, __VA_ARGS__)
#define REXGPU_DEBUG(...) REX_LOG_DEBUG_CAT(GPU, __VA_ARGS__)
#define REXGPU_INFO(...) REX_LOG_INFO_CAT(GPU, __VA_ARGS__)
#define REXGPU_WARN(...) REX_LOG_WARN_CAT(GPU, __VA_ARGS__)
#define REXGPU_ERROR(...) REX_LOG_ERROR_CAT(GPU, __VA_ARGS__)
#define REXGPU_CRITICAL(...) REX_LOG_CRITICAL_CAT(GPU, __VA_ARGS__)

//=============================================================================
// Kernel Subsystem Macros
//=============================================================================

#define REXKRNL_TRACE(...) REX_LOG_TRACE_CAT(Kernel, __VA_ARGS__)
#define REXKRNL_DEBUG(...) REX_LOG_DEBUG_CAT(Kernel, __VA_ARGS__)
#define REXKRNL_INFO(...) REX_LOG_INFO_CAT(Kernel, __VA_ARGS__)
#define REXKRNL_WARN(...) REX_LOG_WARN_CAT(Kernel, __VA_ARGS__)
#define REXKRNL_ERROR(...) REX_LOG_ERROR_CAT(Kernel, __VA_ARGS__)
#define REXKRNL_CRITICAL(...) REX_LOG_CRITICAL_CAT(Kernel, __VA_ARGS__)

//=============================================================================
// System Library Macros (rexsystem)
//=============================================================================

#define REXSYS_TRACE(...) REX_LOG_TRACE_CAT(System, __VA_ARGS__)
#define REXSYS_DEBUG(...) REX_LOG_DEBUG_CAT(System, __VA_ARGS__)
#define REXSYS_INFO(...) REX_LOG_INFO_CAT(System, __VA_ARGS__)
#define REXSYS_WARN(...) REX_LOG_WARN_CAT(System, __VA_ARGS__)
#define REXSYS_ERROR(...) REX_LOG_ERROR_CAT(System, __VA_ARGS__)
#define REXSYS_CRITICAL(...) REX_LOG_CRITICAL_CAT(System, __VA_ARGS__)

//=============================================================================
// Filesystem Subsystem Macros
//=============================================================================

#define REXFS_TRACE(...) REX_LOG_TRACE_CAT(FS, __VA_ARGS__)
#define REXFS_DEBUG(...) REX_LOG_DEBUG_CAT(FS, __VA_ARGS__)
#define REXFS_INFO(...) REX_LOG_INFO_CAT(FS, __VA_ARGS__)
#define REXFS_WARN(...) REX_LOG_WARN_CAT(FS, __VA_ARGS__)
#define REXFS_ERROR(...) REX_LOG_ERROR_CAT(FS, __VA_ARGS__)
#define REXFS_CRITICAL(...) REX_LOG_CRITICAL_CAT(FS, __VA_ARGS__)

//=============================================================================
// Codegen Subsystem Macros
//=============================================================================

#define REXCODEGEN_TRACE(...) REX_LOG_TRACE_CAT(Codegen, __VA_ARGS__)
#define REXCODEGEN_DEBUG(...) REX_LOG_DEBUG_CAT(Codegen, __VA_ARGS__)
#define REXCODEGEN_INFO(...) REX_LOG_INFO_CAT(Codegen, __VA_ARGS__)
#define REXCODEGEN_WARN(...) REX_LOG_WARN_CAT(Codegen, __VA_ARGS__)
#define REXCODEGEN_ERROR(...) REX_LOG_ERROR_CAT(Codegen, __VA_ARGS__)
#define REXCODEGEN_CRITICAL(...) REX_LOG_CRITICAL_CAT(Codegen, __VA_ARGS__)

//=============================================================================
// Function-Prefixed Logging Macros
//=============================================================================

// Generic (Core) with function prefix
#define REXLOGFN_TRACE(fmt, ...) REXLOG_TRACE("{}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOGFN_DEBUG(fmt, ...) REXLOG_DEBUG("{}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOGFN_INFO(fmt, ...) REXLOG_INFO("{}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOGFN_WARN(fmt, ...) REXLOG_WARN("{}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOGFN_ERROR(fmt, ...) REXLOG_ERROR("{}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXLOGFN_CRITICAL(fmt, ...) \
  REXLOG_CRITICAL("{}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)

// Kernel with function prefix and guest thread ID
#define REXKRNLFN_TRACE(fmt, ...)                                    \
  REXKRNL_TRACE("[T:{:08X}] {}: " fmt, ::rex::GetLogGuestThreadId(), \
                __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNLFN_DEBUG(fmt, ...)                                    \
  REXKRNL_DEBUG("[T:{:08X}] {}: " fmt, ::rex::GetLogGuestThreadId(), \
                __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNLFN_INFO(fmt, ...)                                    \
  REXKRNL_INFO("[T:{:08X}] {}: " fmt, ::rex::GetLogGuestThreadId(), \
               __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNLFN_WARN(fmt, ...)                                    \
  REXKRNL_WARN("[T:{:08X}] {}: " fmt, ::rex::GetLogGuestThreadId(), \
               __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNLFN_ERROR(fmt, ...)                                    \
  REXKRNL_ERROR("[T:{:08X}] {}: " fmt, ::rex::GetLogGuestThreadId(), \
                __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)
#define REXKRNLFN_CRITICAL(fmt, ...)                                    \
  REXKRNL_CRITICAL("[T:{:08X}] {}: " fmt, ::rex::GetLogGuestThreadId(), \
                   __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__)

//=============================================================================
// Fatal Error Macros
//=============================================================================

/// Log critical error and abort
#define REX_FATAL(fmt, ...)                                     \
  do {                                                          \
    REXLOG_CRITICAL("[FATAL] " fmt __VA_OPT__(, ) __VA_ARGS__); \
    if (auto _l = ::rex::GetLogger())                           \
      _l->flush();                                              \
    std::abort();                                               \
  } while (0)

/// Log critical error with function prefix and abort
#define REX_FATAL_FN(fmt, ...)                                                    \
  do {                                                                            \
    REXLOG_CRITICAL("[FATAL] {}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__); \
    if (auto _l = ::rex::GetLogger())                                             \
      _l->flush();                                                                \
    std::abort();                                                                 \
  } while (0)

/// Check condition and abort with fatal error if false
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

//=============================================================================
// Assertion Macros
//=============================================================================

/// Log error and assert (Debug-only crash)
#define REX_ASSERT(cond, msg)                                \
  do {                                                       \
    if (!(cond)) {                                           \
      REXLOG_ERROR("Assertion failed: {} - {}", #cond, msg); \
      assert(cond);                                          \
    }                                                        \
  } while (0)

/// Log error and return a value if condition fails
#define REX_ASSERT_RET(cond, msg, retval)                    \
  do {                                                       \
    if (!(cond)) {                                           \
      REXLOG_ERROR("Assertion failed: {} - {}", #cond, msg); \
      return retval;                                         \
    }                                                        \
  } while (0)

/// Log error and return void if condition fails
#define REX_ASSERT_RET_VOID(cond, msg)                       \
  do {                                                       \
    if (!(cond)) {                                           \
      REXLOG_ERROR("Assertion failed: {} - {}", #cond, msg); \
      return;                                                \
    }                                                        \
  } while (0)

//=============================================================================
// Formatting Helpers
//=============================================================================

namespace log {

/// Format address as hex (0x prefix, 8 digits for 32-bit)
inline std::string ptr(uint32_t addr) {
  return fmt::format("0x{:08X}", addr);
}

/// Format address as hex (0x prefix, appropriate width)
inline std::string ptr(uint64_t addr) {
  if (addr > 0xFFFFFFFFULL) {
    return fmt::format("0x{:016X}", addr);
  }
  return fmt::format("0x{:08X}", static_cast<uint32_t>(addr));
}

/// Format pointer
inline std::string ptr(const void* p) {
  return fmt::format("{}", p);
}

template <typename T>
inline std::string ptr(T* p) {
  return fmt::format("{}", static_cast<const void*>(p));
}

/// Format value as hex (0x prefix)
inline std::string hex(uint32_t val) {
  return fmt::format("0x{:X}", val);
}

inline std::string hex(uint64_t val) {
  return fmt::format("0x{:X}", val);
}

/// Format boolean as "true"/"false"
inline const char* boolean(bool b) {
  return b ? "true" : "false";
}

}  // namespace log

}  // namespace rex
