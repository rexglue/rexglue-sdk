/**
 * @file        core/logging.cpp
 * @brief       Logging infrastructure implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(log_level, "info", "Log",
                      "Global log level: trace, debug, info, warn, error, critical, off")
    .allowed({"trace", "debug", "info", "warn", "error", "critical", "off"});

REXCVAR_DEFINE_STRING(log_file, "", "Log", "Log file path (empty = no file logging)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(log_verbose, false, "Log", "Enable verbose logging (sets level to trace)")
    .debug_only();

namespace rex {

namespace {
// Per-category loggers
std::array<std::shared_ptr<spdlog::logger>, static_cast<size_t>(LogCategory::Count)> g_loggers;

// Shared sinks for all loggers
std::vector<spdlog::sink_ptr> g_sinks;

// Initialization state
bool g_initialized = false;
std::mutex g_init_mutex;

// Stored configuration
LogConfig g_config;
}  // namespace

void InitLogging(const LogConfig& config) {
  std::lock_guard<std::mutex> lock(g_init_mutex);

  if (g_initialized) {
    // Re-initialization: update levels only
    for (size_t i = 0; i < static_cast<size_t>(LogCategory::Count); ++i) {
      auto level = config.category_levels[i].value_or(config.default_level);
      if (g_loggers[i]) {
        g_loggers[i]->set_level(level);
      }
    }
    g_config = config;
    return;
  }

  g_config = config;

  // Create shared sinks
  if (config.log_to_console) {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::trace);        // Let logger control filtering
    console_sink->set_pattern("[%^%l%$] [%n] [t%t] %v");  // Include logger name and thread ID
    g_sinks.push_back(console_sink);
  }

  if (config.log_file) {
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.log_file, true);
    file_sink->set_level(spdlog::level::trace);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [t%t] %v");
    g_sinks.push_back(file_sink);
  }

  // Create per-category loggers
  for (size_t i = 0; i < static_cast<size_t>(LogCategory::Count); ++i) {
    auto cat = static_cast<LogCategory>(i);
    std::string name = CategoryName(cat);

    auto logger = std::make_shared<spdlog::logger>(name, g_sinks.begin(), g_sinks.end());

    // Set level from config or default
    auto level = config.category_levels[i].value_or(config.default_level);
    logger->set_level(level);

    // Configure flush behavior
    if (level <= spdlog::level::debug) {
      logger->flush_on(spdlog::level::trace);
    } else {
      logger->flush_on(spdlog::level::warn);
    }

    // Register with spdlog for global access
    spdlog::register_logger(logger);
    g_loggers[i] = logger;
  }

  // Set core as default logger for spdlog
  spdlog::set_default_logger(g_loggers[static_cast<size_t>(LogCategory::Core)]);

  g_initialized = true;

  REXLOG_DEBUG("Rex logging initialized with {} categories",
               static_cast<size_t>(LogCategory::Count));
}

void InitLogging(const char* log_file, spdlog::level::level_enum level) {
  LogConfig config;
  config.log_file = log_file;
  config.default_level = level;
  InitLogging(config);
}

void ShutdownLogging() {
  std::lock_guard<std::mutex> lock(g_init_mutex);

  if (!g_initialized)
    return;

  // Flush all loggers
  for (auto& logger : g_loggers) {
    if (logger) {
      logger->flush();
    }
  }

  // Drop all loggers from spdlog registry
  spdlog::shutdown();

  // Clear our state
  for (auto& logger : g_loggers) {
    logger.reset();
  }
  g_sinks.clear();
  g_initialized = false;
}

std::shared_ptr<spdlog::logger> GetLogger(LogCategory category) {
  if (!g_initialized) {
    InitLogging();
  }
  return g_loggers[static_cast<size_t>(category)];
}

std::shared_ptr<spdlog::logger> GetLogger() {
  return GetLogger(LogCategory::Core);
}

void SetCategoryLevel(LogCategory category, spdlog::level::level_enum level) {
  if (auto logger = g_loggers[static_cast<size_t>(category)]) {
    logger->set_level(level);
  }
}

void SetAllLevels(spdlog::level::level_enum level) {
  for (auto& logger : g_loggers) {
    if (logger) {
      logger->set_level(level);
    }
  }
}

void RegisterLogLevelCallback() {
  rex::cvar::RegisterChangeCallback("log_level", [](std::string_view, std::string_view value) {
    if (auto level = ParseLogLevel(std::string(value))) {
      SetAllLevels(*level);
      REXLOG_DEBUG("Log level changed to {}", value);
    }
  });
}

//=============================================================================
// CLI Helper Functions
//=============================================================================

std::optional<spdlog::level::level_enum> ParseLogLevel(const std::string& level_str) {
  static const std::unordered_map<std::string, spdlog::level::level_enum> level_map = {
      {"trace", spdlog::level::trace},  {"debug", spdlog::level::debug},
      {"info", spdlog::level::info},    {"warn", spdlog::level::warn},
      {"warning", spdlog::level::warn}, {"error", spdlog::level::err},
      {"err", spdlog::level::err},      {"critical", spdlog::level::critical},
      {"off", spdlog::level::off},
  };

  std::string lower = level_str;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  auto it = level_map.find(lower);
  if (it != level_map.end()) {
    return it->second;
  }
  return std::nullopt;
}

spdlog::level::level_enum ParseLogLevelOr(const std::string& level_str,
                                          spdlog::level::level_enum default_level) {
  return ParseLogLevel(level_str).value_or(default_level);
}

std::optional<LogCategory> CategoryFromName(const std::string& name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Map various names to categories
  static const std::unordered_map<std::string, LogCategory> name_map = {
      {"core", LogCategory::Core},      {"cpu", LogCategory::CPU},
      {"ppc", LogCategory::CPU},                                         // Alias
      {"apu", LogCategory::APU},        {"audio", LogCategory::APU},     // Alias
      {"gpu", LogCategory::GPU},        {"graphics", LogCategory::GPU},  // Alias
      {"kernel", LogCategory::Kernel},  {"krnl", LogCategory::Kernel},   // Alias
      {"runtime", LogCategory::Kernel},  // Alias (maps legacy category)
      {"system", LogCategory::System},  {"sys", LogCategory::System},     // Alias
      {"fs", LogCategory::FS},          {"filesystem", LogCategory::FS},  // Alias
      {"vfs", LogCategory::FS},                                           // Alias
  };

  auto it = name_map.find(lower);
  if (it != name_map.end()) {
    return it->second;
  }
  return std::nullopt;
}

LogConfig BuildLogConfig(const char* log_file, const std::string& cli_level,
                         const std::map<std::string, std::string>& category_levels) {
  LogConfig config;
  config.log_file = log_file;

  // Step 1: Start with build-type default
  config.default_level = kDefaultLogLevel;

  // Step 2: Check environment variable
  if (const char* env_level = std::getenv("REX_LOG_LEVEL")) {
    if (auto level = ParseLogLevel(env_level)) {
      config.default_level = *level;
    }
  } else if (const char* env_level = std::getenv("SPDLOG_LEVEL")) {
    // Only use SPDLOG_LEVEL for global level if it's a simple level string
    if (auto level = ParseLogLevel(env_level)) {
      config.default_level = *level;
    }
  }

  // Step 3: CLI global level overrides environment
  if (!cli_level.empty()) {
    if (auto level = ParseLogLevel(cli_level)) {
      config.default_level = *level;
    }
  }

  // Step 4: Per-category CLI levels
  for (const auto& [cat_name, level_str] : category_levels) {
    if (level_str.empty())
      continue;

    auto cat = CategoryFromName(cat_name);
    auto level = ParseLogLevel(level_str);

    if (cat && level) {
      config.category_levels[static_cast<size_t>(*cat)] = *level;
    }
  }

  return config;
}

//=============================================================================
// Guest Thread ID (stub - real implementation in runtime)
//=============================================================================

// This is a weak symbol that can be overridden by the runtime library
// when running in guest context. Default returns 0.
uint32_t GetLogGuestThreadId() {
  // TODO: Link to actual guest context when available
  return 0;
}

}  // namespace rex
