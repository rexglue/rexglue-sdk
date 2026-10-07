/**
 * @file        core/logger.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace rex::log {

/** Slots in the level table. */
inline constexpr size_t kMaxCategories = 256;

/** Entries kept before the logger starts. */
inline constexpr size_t kBootstrapRecords = 512;

/** The logger. 1 instance per process, returned by GetLoggerInstance. */
class Logger final : public ILogger {
 public:
  Logger();

  [[nodiscard]] Result<void> Register(std::span<const Category* const> categories) override;
  void Unregister(std::span<const Category* const> categories) override;
  [[nodiscard]] Result<void> SetLevel(std::string_view category,
                                      std::optional<Level> level) override;
  [[nodiscard]] Result<Level> GetLevel(std::string_view category) const override;
  std::vector<CategoryInfo> Categories() const override;
  void AddSink(ISink& sink) override;
  void RemoveSink(ISink& sink) override;
  void SetFormat(const LineFormat& format) override;
  void Flush() override;
  std::filesystem::path FilePath() const override;

  /**
   * Resolves the slot of a category copy, creating an unregistered slot for an unknown name.
   *
   * @param category The copy.
   * @return The slot.
   */
  u16 Resolve(const Category& category);

  /**
   * Returns the effective level of a slot.
   *
   * @param slot The slot.
   * @return The level.
   */
  Level SlotLevel(u16 slot) const { return levels_[slot].load(std::memory_order_relaxed); }

  /**
   * Writes a message to every sink.
   *
   * @param category The category.
   * @param level The level.
   * @param text The message.
   */
  void Write(const Category& category, Level level, std::string_view text);

  /**
   * Writes a message unless the sink mutex stays held past the timeout or the calling thread is
   * inside a sink. Bypasses the level check.
   *
   * @param category The log category of the message.
   * @param level The severity of the message.
   * @param text The formatted message text.
   * @param timeout How long to retry the sink mutex.
   * @return True when a sink took the message.
   */
  bool TryWrite(const Category& category, Level level, std::string_view text,
                std::chrono::milliseconds timeout);

  /**
   * Flushes every sink under the same rules as TryWrite.
   *
   * @param timeout How long to retry the sink mutex.
   * @return True when the sinks were flushed.
   */
  bool TryFlush(std::chrono::milliseconds timeout);

  /**
   * Opens the sinks of a configuration, registers core and consumer categories and attaches the
   * log cvars.
   *
   * @param config The configuration.
   * @param cvars The registry holding the log scope.
   * @return Ok, or the first registration error.
   */
  [[nodiscard]] Result<void> Start(const LogConfig& config, cvar::ICvarRegistry& cvars);

  /**
   * Opens the log file with the log.file value, replays the bootstrap entries into it and starts
   * the flush thread. Runs after the loader applies the cvar values.
   *
   * @param cvars The registry holding the log scope.
   */
  void Finish(cvar::ICvarRegistry& cvars);

  /**
   * Stops what Start did. A standalone logger keeps its sinks and levels and loses only the cvars.
   */
  void Stop();

  /**
   * Starts the logger for a process that logs outside rex::Init.
   *
   * @param config The configuration.
   * @return Ok, or an error when the logger is already started or a registration fails.
   */
  [[nodiscard]] Result<void> StartStandalone(const LogConfig& config);

  /** Stops a standalone logger and returns to bootstrap output. */
  void StopStandalone();

  /**
   * Returns the time the logger was created.
   *
   * @return The time.
   */
  std::chrono::system_clock::time_point start_time() const { return start_time_; }

 private:
  struct Slot {
    std::string name;
    std::string description;
    std::optional<Level> explicit_level;
    bool registered = false;
    std::string cvar_name;
    std::unique_ptr<cvar::Var<std::string>> cvar;
    cvar::CallbackHandle callback;
  };

  std::optional<u16> FindLocked(std::string_view name) const;
  u16 CreateLocked(std::string_view name, std::string_view description);
  void RecomputeLocked(u16 slot);
  void RecomputeAllLocked();
  [[nodiscard]] Result<void> ApplyLevel(std::string_view category, std::optional<Level> level);
  void ApplyCvarLevel(std::string_view category, std::string_view value);
  void AttachCvar(u16 slot);
  void DetachCvar(Slot& slot);
  void RebuildSinksLocked();
  void FlushLocked();
  bool LockSinks(std::unique_lock<std::mutex>& lock, std::chrono::milliseconds timeout);
  void RunFlushThread(const std::stop_token& stop, std::chrono::seconds interval);
  void OpenSinks(const LogConfig& config);
  void ApplyConfigLevels(const LogConfig& config);
  void HookCvars(cvar::ICvarRegistry& cvars);
  void UnhookCvars();
  void OpenFile(const LogFileConfig& file, std::chrono::seconds interval);
  [[nodiscard]] Result<void> RegisterStarted(const LogConfig& config);
  void StopAll();

  std::array<std::atomic<Level>, kMaxCategories> levels_{};
  std::vector<Slot> slots_;
  Level default_level_ = Level::kInfo;
  std::map<std::string, Level, std::less<>> config_levels_;
  mutable std::mutex mutex_;

  mutable std::mutex sinks_mutex_;
  std::vector<ISink*> sinks_;
  std::vector<ISink*> consumer_sinks_;
  std::vector<ISink*> config_sinks_;
  std::unique_ptr<DebuggerSink> bootstrap_debugger_;
  std::unique_ptr<DebuggerSink> debugger_;
  std::unique_ptr<ConsoleSink> console_;
  std::unique_ptr<FileSink> file_;
  std::deque<CapturedRecord> bootstrap_;
  bool buffering_ = true;
  bool started_ = false;
  bool standalone_ = false;
  Level flush_level_ = Level::kWarn;
  LogFileConfig file_config_;
  std::chrono::seconds flush_interval_{0};

  std::atomic<u64> sequence_ = 0;
  std::chrono::system_clock::time_point start_time_;

  cvar::ICvarRegistry* cvars_ = nullptr;
  cvar::CallbackHandle level_callback_;
  std::vector<const Category*> started_categories_;

  std::mutex flush_mutex_;
  std::condition_variable_any flush_wake_;
  std::jthread flush_thread_;
};

/**
 * Returns the process logger.
 *
 * @return The logger.
 */
Logger& GetLoggerInstance();

/**
 * Returns the logger as its interface pointer, for the core interface table.
 *
 * @return The ILogger pointer.
 */
void* GetLoggerInterface();

/**
 * Picks the log file of a policy, creating its folder and pruning older runs.
 *
 * @param config The file policy.
 * @return The file, empty when the policy is disabled.
 */
std::filesystem::path ResolveLogFile(const LogFileConfig& config);

/**
 * Converts a time to the local calendar time.
 *
 * @param time Seconds since the epoch.
 * @return The local time.
 */
std::tm ToLocalTime(std::time_t time);

/**
 * Returns the ANSI SGR prefix of a level, empty for Info.
 *
 * @param level The level.
 * @return The escape sequence.
 */
std::string_view LevelColor(Level level);

/**
 * Returns the 1 letter file column of a level.
 *
 * @param level The level.
 * @return The letter.
 */
char LevelLetter(Level level);

}  // namespace rex::log

namespace rex::detail {

/**
 * Starts the process logger. ModuleLoader::Connect calls it before any module loads.
 *
 * @param config The log configuration.
 * @param cvars The core cvar registry.
 * @return Ok, or the first registration error.
 */
[[nodiscard]] Result<void> StartLogger(const LogConfig& config, cvar::ICvarRegistry& cvars);

/**
 * Opens the log file and starts the flush thread. ModuleLoader::Connect calls it after it applies
 * the cvar values.
 *
 * @param cvars The core cvar registry.
 */
void FinishLoggerStart(cvar::ICvarRegistry& cvars);

/** Stops the process logger. ModuleLoader::UnwindAll calls it after every module disconnects. */
void StopLogger();

}  // namespace rex::detail
