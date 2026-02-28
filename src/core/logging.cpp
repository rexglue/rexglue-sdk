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

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

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

// Category registry — indices 0..kBuiltinCount-1 are SDK built-ins.
std::vector<LogCategoryEntry> g_registry;

// Shared default sinks (stored for pattern changes)
spdlog::sink_ptr g_console_sink;
spdlog::sink_ptr g_file_sink;

// Extra global sinks added via AddSink()
std::vector<spdlog::sink_ptr> g_extra_sinks;

// Initialization state
bool g_initialized = false;
std::mutex g_mutex;

// Stored config (for resolving category_levels/category_sinks on late registration)
LogConfig g_config;

// Collect the default sinks into a vector for logger construction
std::vector<spdlog::sink_ptr> BuildDefaultSinks() {
  std::vector<spdlog::sink_ptr> sinks;
  if (g_console_sink)
    sinks.push_back(g_console_sink);
  if (g_file_sink)
    sinks.push_back(g_file_sink);
  for (auto& s : g_extra_sinks)
    sinks.push_back(s);
  return sinks;
}

// Build sinks for a specific category (handles per-category sinks from config)
std::vector<spdlog::sink_ptr> BuildCategorySinks(const std::string& name) {
  auto it = g_config.category_sinks.find(name);
  if (it != g_config.category_sinks.end()) {
    if (g_config.category_sinks_exclusive) {
      // Replace default sinks entirely
      return it->second;
    }
    // Additive: default sinks + category-specific sinks
    auto sinks = BuildDefaultSinks();
    for (auto& s : it->second)
      sinks.push_back(s);
    return sinks;
  }
  return BuildDefaultSinks();
}

// Resolve per-category level from config, or return default
spdlog::level::level_enum ResolveCategoryLevel(const std::string& name) {
  auto it = g_config.category_levels.find(name);
  if (it != g_config.category_levels.end())
    return it->second;
  return g_config.default_level;
}

// Create a logger and register it
std::shared_ptr<spdlog::logger> CreateCategoryLogger(const std::string& name) {
  auto sinks = BuildCategorySinks(name);
  auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
  logger->set_level(ResolveCategoryLevel(name));
  logger->flush_on(g_config.flush_level);
  spdlog::register_logger(logger);
  return logger;
}

}  // namespace

// ---- Built-in category short names (must match log:: constant IDs) ----
static constexpr const char* kBuiltinNames[] = {"core", "cpu", "apu", "gpu", "krnl", "sys", "fs"};

void InitLogging(const LogConfig& config) {
  std::lock_guard lock(g_mutex);

  if (g_initialized) {
    // Re-initialization: update levels and config only
    g_config = config;
    for (auto& entry : g_registry) {
      if (entry.logger) {
        entry.logger->set_level(ResolveCategoryLevel(entry.name));
        entry.logger->flush_on(config.flush_level);
      }
    }
    return;
  }

  g_config = config;

  // Create console sink
  if (config.log_to_console) {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sink->set_level(spdlog::level::trace);
    sink->set_pattern(config.console_pattern);
    g_console_sink = sink;
  }

  // Create file sink
  if (config.log_file) {
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.log_file, true);
    sink->set_level(spdlog::level::trace);
    sink->set_pattern(config.file_pattern);
    g_file_sink = sink;
  }

  // Populate extra global sinks from config
  g_extra_sinks = config.extra_sinks;

  // Ensure registry has room for built-in categories (don't truncate
  // any consumer categories that were registered during static init).
  if (g_registry.size() < log::kBuiltinCount) {
    g_registry.resize(log::kBuiltinCount);
  }
  for (uint16_t i = 0; i < log::kBuiltinCount; ++i) {
    g_registry[i].name = kBuiltinNames[i];
    g_registry[i].logger = CreateCategoryLogger(kBuiltinNames[i]);
  }

  // Set core as default logger for spdlog
  spdlog::set_default_logger(g_registry[0].logger);

  g_initialized = true;

  // Create loggers for any categories registered during static init
  // (they have a name but no logger since sinks didn't exist yet).
  for (size_t i = log::kBuiltinCount; i < g_registry.size(); ++i) {
    if (!g_registry[i].name.empty() && !g_registry[i].logger) {
      g_registry[i].logger = CreateCategoryLogger(g_registry[i].name);
    }
  }
}

void InitLogging(const char* log_file, spdlog::level::level_enum level) {
  LogConfig config;
  config.log_file = log_file;
  config.default_level = level;
  InitLogging(config);
}

void ShutdownLogging() {
  std::lock_guard lock(g_mutex);
  if (!g_initialized)
    return;

  for (auto& entry : g_registry) {
    if (entry.logger)
      entry.logger->flush();
  }

  spdlog::shutdown();

  g_registry.clear();
  g_console_sink.reset();
  g_file_sink.reset();
  g_extra_sinks.clear();
  g_initialized = false;
}

LogCategoryId RegisterLogCategory(const char* name) {
  std::lock_guard lock(g_mutex);

  // Reserve indices 0..kBuiltinCount-1 for SDK built-in categories so
  // consumer categories never collide with built-in IDs even when
  // registered during static initialization (before InitLogging).
  if (g_registry.size() < log::kBuiltinCount) {
    g_registry.resize(log::kBuiltinCount);
  }

  // Check for duplicates
  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == name) {
      return LogCategoryId{static_cast<uint16_t>(i)};
    }
  }

  uint16_t id = static_cast<uint16_t>(g_registry.size());
  LogCategoryEntry entry;
  entry.name = name;
  if (g_initialized) {
    entry.logger = CreateCategoryLogger(name);
  }
  g_registry.push_back(std::move(entry));
  return LogCategoryId{id};
}

std::optional<LogCategoryId> FindCategory(const std::string& name) {
  std::lock_guard lock(g_mutex);
  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == name) {
      return LogCategoryId{static_cast<uint16_t>(i)};
    }
  }
  return std::nullopt;
}

std::span<const LogCategoryEntry> GetAllCategories() {
  // No lock: only safe to call from main thread or after init.
  return {g_registry.data(), g_registry.size()};
}

spdlog::logger* GetLoggerRaw(LogCategoryId category) {
  if (!g_initialized)
    InitLogging();
  if (category.id < g_registry.size()) {
    return g_registry[category.id].logger.get();
  }
  return nullptr;
}

std::shared_ptr<spdlog::logger> GetLogger(LogCategoryId category) {
  if (!g_initialized)
    InitLogging();
  if (category.id < g_registry.size()) {
    return g_registry[category.id].logger;
  }
  return nullptr;
}

std::shared_ptr<spdlog::logger> GetLogger() {
  return GetLogger(log::Core);
}

void SetCategoryLevel(LogCategoryId category, spdlog::level::level_enum level) {
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      logger->set_level(level);
    }
  }
}

void SetAllLevels(spdlog::level::level_enum level) {
  for (auto& entry : g_registry) {
    if (entry.logger)
      entry.logger->set_level(level);
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

void AddSink(spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  g_extra_sinks.push_back(sink);
  for (auto& entry : g_registry) {
    if (entry.logger)
      entry.logger->sinks().push_back(sink);
  }
}

void AddSink(LogCategoryId category, spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      logger->sinks().push_back(sink);
    }
  }
}

void RemoveSink(spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  std::erase(g_extra_sinks, sink);
  for (auto& entry : g_registry) {
    if (entry.logger)
      std::erase(entry.logger->sinks(), sink);
  }
}

void RemoveSink(LogCategoryId category, spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      std::erase(logger->sinks(), sink);
    }
  }
}

void SetConsolePattern(const std::string& pattern) {
  if (g_console_sink)
    g_console_sink->set_pattern(pattern);
}

void SetFilePattern(const std::string& pattern) {
  if (g_file_sink)
    g_file_sink->set_pattern(pattern);
}

// ==========================================================================
// CLI Helpers
// ==========================================================================

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
  if (it != level_map.end())
    return it->second;
  return std::nullopt;
}

spdlog::level::level_enum ParseLogLevelOr(const std::string& level_str,
                                          spdlog::level::level_enum default_level) {
  return ParseLogLevel(level_str).value_or(default_level);
}

LogConfig BuildLogConfig(const char* log_file, const std::string& cli_level,
                         const std::map<std::string, std::string>& category_levels) {
  LogConfig config;
  config.log_file = log_file;

  // Build-type default
  config.default_level = kDefaultLogLevel;

  // Environment variable
  if (const char* env_level = std::getenv("REX_LOG_LEVEL")) {
    if (auto level = ParseLogLevel(env_level))
      config.default_level = *level;
  } else if (const char* env_level2 = std::getenv("SPDLOG_LEVEL")) {
    if (auto level = ParseLogLevel(env_level2))
      config.default_level = *level;
  }

  // CLI global level overrides environment
  if (!cli_level.empty()) {
    if (auto level = ParseLogLevel(cli_level))
      config.default_level = *level;
  }

  // Per-category CLI levels (string-keyed)
  for (const auto& [cat_name, level_str] : category_levels) {
    if (level_str.empty())
      continue;
    if (auto level = ParseLogLevel(level_str)) {
      config.category_levels[cat_name] = *level;
    }
  }

  return config;
}

// ==========================================================================
// Guest Thread ID
// ==========================================================================

uint32_t GetLogGuestThreadId() {
  // TODO: Link to actual guest context when available
  return 0;
}

// ==========================================================================
// Deprecated Compatibility Layer
// ==========================================================================

// Map old LogCategory enum to new LogCategoryId
static LogCategoryId CategoryToId(LogCategory category) {
  auto idx = static_cast<uint16_t>(std::to_underlying(category));
  return LogCategoryId{idx};
}

std::optional<LogCategory> CategoryFromName(const std::string& name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  static const std::unordered_map<std::string, LogCategory> name_map = {
      {"core", LogCategory::Core},     {"cpu", LogCategory::CPU},
      {"ppc", LogCategory::CPU},       {"apu", LogCategory::APU},
      {"audio", LogCategory::APU},     {"gpu", LogCategory::GPU},
      {"graphics", LogCategory::GPU},  {"kernel", LogCategory::Kernel},
      {"krnl", LogCategory::Kernel},   {"runtime", LogCategory::Kernel},
      {"system", LogCategory::System}, {"sys", LogCategory::System},
      {"fs", LogCategory::FS},         {"filesystem", LogCategory::FS},
      {"vfs", LogCategory::FS},
  };

  auto it = name_map.find(lower);
  if (it != name_map.end())
    return it->second;
  return std::nullopt;
}

std::shared_ptr<spdlog::logger> GetLogger(LogCategory category) {
  return GetLogger(CategoryToId(category));
}

void SetCategoryLevel(LogCategory category, spdlog::level::level_enum level) {
  SetCategoryLevel(CategoryToId(category), level);
}

}  // namespace rex
