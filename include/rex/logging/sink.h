/**
 * @file        logging/sink.h
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
#include <string>
#include <vector>

#include <rex/logging/format.h>
#include <rex/logging/level.h>
#include <rex/macros.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::log {

/** Receives every written message. The logger calls it under its sink mutex, never concurrently. */
class ISink {
 public:
  virtual ~ISink() = default;

  /**
   * Writes a message.
   *
   * @param message The message.
   */
  virtual void Write(const Message& message) = 0;

  /** Writes buffered output. */
  virtual void Flush() = 0;
};

/** Standard output. Colors through ANSI codes, with virtual terminal mode on for a Windows console.
 */
class REX_API ConsoleSink final : public ISink {
 public:
  /**
   * Builds the sink.
   *
   * @param format What to write around each message.
   */
  explicit ConsoleSink(LineFormat format);

  /**
   * Writes a message.
   *
   * @param message The message.
   */
  void Write(const Message& message) override;

  /** Flushes standard output. */
  void Flush() override;

  /**
   * Sets the line format.
   *
   * @param format What to write around each message.
   */
  void SetFormat(LineFormat format);

 private:
  LineFormat format_;
  bool color_supported_ = false;
};

/** The debugger. OutputDebugStringW on Windows, standard error elsewhere. Never colored. */
class REX_API DebuggerSink final : public ISink {
 public:
  /**
   * Builds the sink.
   *
   * @param format What to write around each message. color is ignored.
   */
  explicit DebuggerSink(LineFormat format);

  /**
   * Writes a message.
   *
   * @param message The message.
   */
  void Write(const Message& message) override;

  /** Flushes standard error where it is the target. */
  void Flush() override;

  /**
   * Sets the line format.
   *
   * @param format What to write around each message. color is ignored.
   */
  void SetFormat(LineFormat format);

 private:
  LineFormat format_;
};

/**
 * A log file. Writes a 1 letter level column (T, V, D, I, W, E, C) after the timestamp, because a
 * file has no color.
 */
class REX_API FileSink final : public ISink, public NonCopyable {
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

  /**
   * Writes a message.
   *
   * @param message The message.
   */
  void Write(const Message& message) override;

  /** Writes buffered output to the file. */
  void Flush() override;

  /**
   * Returns the file path.
   *
   * @return The path passed to Open.
   */
  const std::filesystem::path& GetPath() const;

  /**
   * Wraps an open stream. Open is the usual way to build the sink.
   *
   * @param path The file the stream writes.
   * @param file The open stream.
   * @param format What to write around each message. color is ignored.
   */
  FileSink(std::filesystem::path path, std::ofstream file, LineFormat format);

 private:
  std::filesystem::path path_;
  std::ofstream file_;
  LineFormat format_;
};

/** 1 captured message, owning its text. */
struct CapturedMessage {
  Level level = Level::kInfo;                 /**< Message level. */
  std::string category;                       /**< Category name. */
  std::string text;                           /**< Formatted message. */
  std::chrono::system_clock::time_point time; /**< Time the message was written. */
  u32 thread_id = 0;                          /**< Host thread ID. */
  u64 sequence = 0;                           /**< Message sequence. */
};

/** Keeps the newest messages in memory for the console. Safe to read from any thread. */
class REX_API CaptureSink final : public ISink {
 public:
  /** Messages kept. */
  static constexpr size_t kCapacity = 4096;

  /**
   * Keeps a message, dropping the oldest at capacity.
   *
   * @param message The message.
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

  /**
   * Returns the sequence of the newest message kept.
   *
   * @return The sequence, 0 before the first message.
   */
  u64 GetLatest() const;

 private:
  mutable std::mutex mutex_;
  std::deque<CapturedMessage> messages_;
};

}  // namespace rex::log
