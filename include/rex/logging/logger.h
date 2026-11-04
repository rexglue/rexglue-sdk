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

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/logging/log.h>
#include <rex/module.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/setting.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::log {

/** Timestamp written at the start of a line. */
enum class TimestampStyle : u8 {
  kNone,
  kClock,    /**< Local wall clock with milliseconds. */
  kDateTime, /**< Local date and wall clock with milliseconds. */
  kElapsed,  /**< Seconds since the logger started, with a leading plus sign. */
};

/** What a text sink writes around each message. The level is never written as text. */
struct LineFormat {
  TimestampStyle timestamp = TimestampStyle::kNone;
  bool category = true;
  bool thread = false; /**< Host thread ID column. */
  bool color = true;   /**< Level color, where the sink supports it. */
};

/** One message as sinks receive it. The views are valid during ISink::Write. */
struct Message {
  Level level = Level::kInfo;
  std::string_view category;
  std::string_view text;
  std::chrono::system_clock::time_point time;
  u32 thread_id = 0;
  u64 sequence = 0; /**< Process wide order, starting at 1. */
};

/**
 * Renders a message as a single line without a line break.
 *
 * @return The line. Color is written as ANSI SGR codes.
 */
REX_API std::string FormatLine(const Message& message, const LineFormat& format);

/** The ANSI SGR code that ends a colored run. */
inline constexpr std::string_view kColorReset = "\x1b[0m";

/** Returns the ANSI SGR prefix of a level, empty for Info. */
REX_API std::string_view LevelColor(Level level);

/**
 * Returns the file column letter of a level.
 */
REX_API char LevelLetter(Level level);

}  // namespace rex::log

namespace rex::log {

/** Receives every written message. The logger calls it under its sink mutex, never concurrently. */
class ISink : public NonCopyable {
 public:
  virtual ~ISink() = default;

  virtual void Write(const Message& message) = 0;

  virtual void Flush() = 0;
};

/** A log file with a level letter column (T, V, D, I, W, E, C) after the timestamp. */
class REX_API FileSink final : public ISink {
 public:
  /**
   * Opens a file for appending.
   *
   * @param path The file. Missing parent folders are created.
   * @param format What to write around each message. color is ignored.
   * @return The sink, or an error naming the path.
   */
  [[nodiscard]] static Result<std::unique_ptr<FileSink>> Open(const std::filesystem::path& path,
                                                              LineFormat format);

  void Write(const Message& message) override;

  void Flush() override;

  /**
   * @return The path passed to Open.
   */
  const std::filesystem::path& GetPath() const;

  /**
   * Wraps an open stream. Open is the usual way to build the sink.
   *
   * @param format What to write around each message. color is ignored.
   */
  FileSink(std::filesystem::path path, std::ofstream file, LineFormat format);

 private:
  std::filesystem::path path_;
  std::ofstream file_;
  LineFormat format_;
};

/** One captured message, owning its text. */
struct CapturedMessage {
  Level level = Level::kInfo;
  std::string category;
  std::string text;
  std::chrono::system_clock::time_point time;
  u32 thread_id = 0;
  u64 sequence = 0;
};

/** Keeps the newest messages in memory for the console. Safe to read from any thread. */
class REX_API CaptureSink final : public ISink {
 public:
  static constexpr size_t kCapacity = 4096;

  /**
   * Keeps a message, discarding the oldest at capacity.
   */
  void Write(const Message& message) override;

  /** Does nothing, the messages are in memory. */
  void Flush() override;

  /**
   * Copies the messages newer than a sequence, oldest first.
   *
   * @param after Sequence of the newest message the caller holds, 0 for all.
   * @param out Receives the messages. Cleared first.
   * @return The sequence of the newest message kept, or after when nothing is newer.
   */
  u64 CopySince(u64 after, std::vector<CapturedMessage>& out) const;

  /** Returns the sequence of the newest message kept, 0 before the first message. */
  u64 GetLatestSequence() const;

 private:
  mutable std::mutex mutex_;
  std::deque<CapturedMessage> messages_;
};

}  // namespace rex::log

namespace rex {

class ProcessLoggerControl;

/** The process logger as modules reach it. */
REX_DECLARE_INTERFACE(ILogger, 1)
/**
 * Adds categories, each with a setting log.categories.<name>, empty to follow log.level.
 *
 * @param categories The categories, which must outlive their removal.
 * @return Ok, or an error when a name is already added by another table.
 */
  [[nodiscard]] virtual Result<void> AddCategories(
      std::span<const log::Category* const> categories) = 0;

  /**
   * Removes categories and their settings. Later lines follow log.level.
   */
  virtual void RemoveCategories(std::span<const log::Category* const> categories) = 0;

  /**
   * Adds a sink. It receives the kept lines, then every new line.
   *
   * @param sink The sink, which must outlive the returned guard.
   * @return Guard that removes the sink and waits for a write in progress. Does nothing for a sink
   *         that was added twice.
   */
  [[nodiscard]] virtual ScopeGuard AddSink(log::ISink& sink) = 0;

  /** Flushes every sink. Safe from a crash handler. */
  virtual void Flush() = 0;

  /**
   * Writes a message to every sink.
   *
   * @param message The message, valid for the call.
   */
  virtual void Emit(const log::Message& message) = 0;
};

/**
 * It starts on first use and writes to the console and the debugger. It keeps lines until the log
 * file opens, then writes them to it, and keeps the last 4096 for sinks added later. The app opens
 * the file once app.log_root resolves.
 */
class REX_API Logger final : public SettingOwner, public ILogger {
 public:
  ~Logger() override;

  REX_SETTING(std::string, level, "info",
              "Lowest level written by a category without its own level",
              Allowed("trace", "dev", "debug", "info", "warn", "error", "critical", "off"));
  REX_SETTING(std::string, flush_level, "warn", "Lowest level that flushes every sink at once",
              Allowed("trace", "dev", "debug", "info", "warn", "error", "critical", "off"),
              kStartupOnly);
  REX_SETTING(i32, flush_interval, 1, "Seconds between flushes", Range{0, 60}, kStartupOnly);
  REX_SETTING(bool, console, true, "Write to the console", kStartupOnly);
  REX_SETTING(bool, debugger, true, "Write to an attached debugger", kStartupOnly);
  REX_SETTING(bool, file, true, "Write a log file under app.log_root", kStartupOnly);
  REX_SETTING(std::string, file_name, "", "File name, the app name when empty", kStartupOnly);
  REX_SETTING(i64, folder_budget, 0, "Bytes of old log files kept, 0 keeps all",
              Range{0, 9007199254740992.0}, kStartupOnly);
  REX_SETTING(std::string, pattern, "%^%Y-%m-%d %H:%M:%S.%e %Q %-10n %j %v%$",
              "Layout of each line, an spdlog pattern with %Q for the level letter and %j for the "
              "thread",
              kStartupOnly);
  REX_SETTING(bool, color, false, "Color the console", kStartupOnly);

  [[nodiscard]] Result<void> AddCategories(
      std::span<const log::Category* const> categories) override;

  void RemoveCategories(std::span<const log::Category* const> categories) override;

  [[nodiscard]] ScopeGuard AddSink(log::ISink& sink) override;

  void Flush() override;

  void Emit(const log::Message& message) override;

  struct State;

 private:
  Logger();
  friend REX_API Logger& GetLogger();
  friend class ProcessLoggerControl;

  void ApplySettled();
  [[nodiscard]] Result<void> OpenFile(const std::filesystem::path& log_root,
                                      std::string_view app_name);
  void EndSession();

  std::unique_ptr<State> state_;
};

/** Returns the process logger, starting it on the first call. */
REX_API Logger& GetLogger();

#define REX_LOGGER() ::rex::GetLogger()

}  // namespace rex
