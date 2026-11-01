/**
 * @file        base/log_sink.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdio>
#include <format>
#include <iterator>
#include <utility>

#include "base/logger.h"

namespace rex::log {

namespace {

constexpr size_t kCategoryWidth = 10;

void AppendTimestamp(std::string& out, const Message& message, TimestampStyle style) {
  if (style == TimestampStyle::kNone) {
    return;
  }
  const auto since_epoch = message.time.time_since_epoch();
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count() % 1000;
  if (style == TimestampStyle::kElapsed) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        message.time - GetLoggerInstance().GetStartTime());
    std::format_to(std::back_inserter(out), "+{}.{:03d} ", elapsed.count() / 1000,
                   elapsed.count() % 1000);
    return;
  }
  const std::tm local = ToLocalTime(std::chrono::system_clock::to_time_t(message.time));
  if (style == TimestampStyle::kDateTime) {
    std::format_to(std::back_inserter(out), "{:04d}-{:02d}-{:02d} ", local.tm_year + 1900,
                   local.tm_mon + 1, local.tm_mday);
  }
  std::format_to(std::back_inserter(out), "{:02d}:{:02d}:{:02d}.{:03d} ", local.tm_hour,
                 local.tm_min, local.tm_sec, millis);
}

std::string Render(const Message& message, const LineFormat& format, bool color, char letter) {
  std::string out;
  const std::string_view prefix = color ? LevelColor(message.level) : std::string_view{};
  out += prefix;
  AppendTimestamp(out, message, format.timestamp);
  if (letter != '\0') {
    out += letter;
    out += ' ';
  }
  if (format.category) {
    std::format_to(std::back_inserter(out), "{:<{}} ", message.category, kCategoryWidth);
  }
  if (format.thread) {
    std::format_to(std::back_inserter(out), "t{:05d} ", message.thread_id);
  }
  out += message.text;
  if (!prefix.empty()) {
    out += kColorReset;
  }
  return out;
}

}  // namespace

std::string_view LevelColor(Level level) {
  switch (level) {
    case Level::kTrace:
      return "\x1b[90m";
    case Level::kDev:
      return "\x1b[35m";
    case Level::kDebug:
      return "\x1b[36m";
    case Level::kWarn:
      return "\x1b[33m";
    case Level::kError:
      return "\x1b[31m";
    case Level::kCritical:
      return "\x1b[97;41m";
    case Level::kInfo:
    case Level::kOff:
      return {};
  }
  return {};
}

char LevelLetter(Level level) {
  constexpr std::string_view kLetters = "TVDIWEC-";
  return kLetters[std::to_underlying(level)];
}

std::string FormatLine(const Message& message, const LineFormat& format) {
  return Render(message, format, format.color, '\0');
}

void ConsoleSink::Write(const Message& message) {
  std::string line = Render(message, format_, format_.color && color_supported_, '\0');
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), stdout);
}

void ConsoleSink::Flush() {
  std::fflush(stdout);
}

void ConsoleSink::SetFormat(LineFormat format) {
  format_ = format;
}

void DebuggerSink::SetFormat(LineFormat format) {
  format_ = format;
}

Result<std::unique_ptr<FileSink>> FileSink::Open(const std::filesystem::path& path,
                                                 LineFormat format) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream file(path, std::ios::out | std::ios::app | std::ios::binary);
  if (!file) {
    return Err<std::unique_ptr<FileSink>>(
        ErrorCategory::kIO, std::format("log file {} could not be opened", path.string()));
  }
  return std::make_unique<FileSink>(path, std::move(file), format);
}

FileSink::FileSink(std::filesystem::path path, std::ofstream file, LineFormat format)
    : path_(std::move(path)), file_(std::move(file)), format_(format) {}

void FileSink::Write(const Message& message) {
  file_ << Render(message, format_, false, LevelLetter(message.level)) << '\n';
}

void FileSink::Flush() {
  file_.flush();
}

const std::filesystem::path& FileSink::GetPath() const {
  return path_;
}

void CaptureSink::Write(const Message& message) {
  std::lock_guard lock(mutex_);
  if (messages_.size() == kCapacity) {
    messages_.pop_front();
  }
  messages_.push_back(CapturedMessage{message.level, std::string(message.category),
                                      std::string(message.text), message.time, message.thread_id,
                                      message.sequence});
}

void CaptureSink::Flush() {}

u64 CaptureSink::CopySince(u64 after, std::vector<CapturedMessage>& out) const {
  out.clear();
  std::lock_guard lock(mutex_);
  for (const CapturedMessage& message : messages_) {
    if (message.sequence > after) {
      out.push_back(message);
    }
  }
  return messages_.empty() ? after : std::max(after, messages_.back().sequence);
}

u64 CaptureSink::GetLatestSequence() const {
  std::lock_guard lock(mutex_);
  return messages_.empty() ? 0 : messages_.back().sequence;
}

}  // namespace rex::log
