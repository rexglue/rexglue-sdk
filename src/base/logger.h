/**
 * @file        base/logger.h
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
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/details/log_msg.h>
#include <spdlog/fwd.h>

#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>

namespace rex::log {

inline constexpr size_t kMaxCategories = 256;

/** Lines kept for sinks that join later. */
inline constexpr size_t kReplayLines = 4096;

/** Lines kept until the log file opens. The oldest are discarded beyond this limit. */
inline constexpr size_t kPreFileLines = 65536;

struct LogFileConfig {
  bool enabled = false;
  std::filesystem::path path;   /**< Explicit file, or empty for a sequential file in folder. */
  std::filesystem::path folder; /**< Folder of sequential files, or empty for the logs folder. */
  std::string name;             /**< Base name of sequential files, or empty for the executable. */
  u64 folder_budget = 0;        /**< Bytes of older runs kept in folder, 0 for no limit. */
};

class SinkLogger;

}  // namespace rex::log

namespace rex {

struct Logger::State {
  explicit State(Logger& logger);

  u16 Resolve(const log::LogCategory& category);

  log::Level GetSlotLevel(u16 slot) const { return levels[slot].load(std::memory_order_relaxed); }

  void Write(const log::Message& message) const;

  /** Keeps a message and hands it to every sink. */
  void Dispatch(const spdlog::details::log_msg& message, u64 message_sequence);

  /** Flushes every sink. Called by the engine, also from the periodic flusher. */
  void FlushSinks();

  /**
   * Writes a message without the level check. Gives up when the sink mutex stays held past the
   * timeout or the thread is inside a sink.
   *
   * @param timeout The longest wait for the sink mutex.
   * @return True when a sink took the message.
   */
  bool TryWrite(const log::LogCategory& category, log::Level severity, std::string_view text,
                std::chrono::milliseconds timeout);

  /**
   * Flushes every sink under the rules of TryWrite.
   *
   * @param timeout The longest wait for the sink mutex.
   */
  bool TryFlush(std::chrono::milliseconds timeout);

  /** Returns the log file descriptor, or -1 when none is open. Reads without locking. */
  int GetCrashFD() const { return crash_fd.load(std::memory_order_acquire); }

  std::chrono::system_clock::time_point GetStartTime() const { return start_time; }

  [[nodiscard]] Result<void> AddCategories(std::span<const log::LogCategory* const> categories);
  void RemoveCategories(std::span<const log::LogCategory* const> categories);
  bool AddSink(log::ILogSink& sink);
  void RemoveSink(log::ILogSink& sink);
  void Flush() const;
  void ApplySettled();
  [[nodiscard]] Result<void> OpenFile(const std::filesystem::path& log_root,
                                      std::string_view app_name);
  void ApplyDefaultLevel(std::string_view text);
  void ApplyCategoryLevel(std::string_view category, std::string_view text);

  struct Consumer {
    log::ILogSink* target;
    std::shared_ptr<spdlog::sinks::sink> adapter;
  };

  struct Slot {
    std::string name;
    std::string description;
    std::optional<log::Level> explicit_level;
    bool registered = false;
    std::unique_ptr<SettingOwner> setting;
  };

  std::optional<u16> FindLocked(std::string_view name) const;
  u16 CreateLocked(std::string_view name, std::string_view description);
  void RecomputeLocked(u16 slot);
  void RecomputeAllLocked();
  void RebuildSinksLocked();
  void FlushLocked();
  bool LockSinks(std::unique_lock<std::mutex>& lock, std::chrono::milliseconds timeout);
  void StopFlusher();
  void StartFlusher(std::chrono::seconds interval) const;
  void SetCrashFile(const std::filesystem::path& path);
  void Keep(const log::Message& message);
  void Replay(spdlog::sinks::sink& sink, bool notice);
  void EndSession();

  static State& Instance();

  Logger& owner;
  std::array<std::atomic<log::Level>, log::kMaxCategories> levels{};
  std::vector<Slot> slots;
  log::Level default_level = log::Level::kInfo;
  mutable std::mutex mutex;

  mutable std::mutex sinks_mutex;
  std::shared_ptr<log::SinkLogger> engine;
  std::vector<std::shared_ptr<spdlog::sinks::sink>> sinks;
  std::vector<Consumer> consumers;
  std::shared_ptr<spdlog::sinks::sink> debugger;
  std::shared_ptr<spdlog::sinks::sink> console;
  std::shared_ptr<spdlog::sinks::sink> file;
  std::string pattern;
  std::string rejected_pattern;
  u64 dispatch_sequence = 0;
  std::atomic<int> crash_fd{-1};
  std::deque<log::CapturedMessage> kept;
  bool keeping = true;
  bool file_resolved = false;
  u64 discarded = 0;
  log::Level flush_level = log::Level::kWarn;

  std::atomic<u64> sequence = 0;
  std::chrono::system_clock::time_point start_time;
  ScopeGuard level_watch;
  std::vector<ScopeGuard> watches;
};

}  // namespace rex

namespace rex::log {

Logger::State& GetLoggerInstance();

/** Returns the logs folder of the application root, without creating it. */
std::filesystem::path DefaultLogFolder();

/**
 * Picks the log file of a policy, creating its folder and pruning older runs.
 *
 * @return The file, or empty when the policy is disabled.
 */
std::filesystem::path ResolveLogFile(const LogFileConfig& config);

std::tm ToLocalTime(std::time_t time);

/**
 * Opens an append descriptor on a log file for the crash report.
 *
 * @return The descriptor, or -1 where the platform reports through the logger.
 */
int OpenCrashFile(const std::filesystem::path& path);

void CloseCrashFile(int fd);

}  // namespace rex::log
