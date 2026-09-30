#include <algorithm>
#include <cstdio>
#include <format>
#include <iterator>
#include <utility>

#include "core/logger.h"

namespace rex::log {

namespace {

constexpr std::string_view kColorReset = "\x1b[0m";
constexpr size_t kCategoryWidth = 10;

void AppendTimestamp(std::string& out, const Record& record, TimestampStyle style) {
  if (style == TimestampStyle::kNone) {
    return;
  }
  const auto since_epoch = record.time.time_since_epoch();
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count() % 1000;
  if (style == TimestampStyle::kElapsed) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        record.time - GetLoggerInstance().start_time());
    std::format_to(std::back_inserter(out), "+{}.{:03d} ", elapsed.count() / 1000,
                   elapsed.count() % 1000);
    return;
  }
  const std::tm local = ToLocalTime(std::chrono::system_clock::to_time_t(record.time));
  if (style == TimestampStyle::kDateTime) {
    std::format_to(std::back_inserter(out), "{:04d}-{:02d}-{:02d} ", local.tm_year + 1900,
                   local.tm_mon + 1, local.tm_mday);
  }
  std::format_to(std::back_inserter(out), "{:02d}:{:02d}:{:02d}.{:03d} ", local.tm_hour,
                 local.tm_min, local.tm_sec, millis);
}

std::string Render(const Record& record, const LineFormat& format, bool color, char letter) {
  std::string out;
  const std::string_view prefix = color ? LevelColor(record.level) : std::string_view{};
  out += prefix;
  AppendTimestamp(out, record, format.timestamp);
  if (letter != '\0') {
    out += letter;
    out += ' ';
  }
  if (format.category) {
    std::format_to(std::back_inserter(out), "{:<{}} ", record.category, kCategoryWidth);
  }
  if (format.thread) {
    std::format_to(std::back_inserter(out), "t{:05d} ", record.thread_id);
  }
  out += record.text;
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

std::string FormatLine(const Record& record, const LineFormat& format) {
  return Render(record, format, format.color, '\0');
}

void ConsoleSink::Write(const Record& record) {
  std::string line = Render(record, format_, format_.color && color_supported_, '\0');
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), stdout);
}

void ConsoleSink::Flush() {
  std::fflush(stdout);
}

void ConsoleSink::set_format(LineFormat format) {
  format_ = format;
}

void DebuggerSink::set_format(LineFormat format) {
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
        ErrorCategory::IO, std::format("log file {} could not be opened", path.string()));
  }
  return std::make_unique<FileSink>(path, std::move(file), format);
}

FileSink::FileSink(std::filesystem::path path, std::ofstream file, LineFormat format)
    : path_(std::move(path)), file_(std::move(file)), format_(format) {}

void FileSink::Write(const Record& record) {
  file_ << Render(record, format_, false, LevelLetter(record.level)) << '\n';
}

void FileSink::Flush() {
  file_.flush();
}

const std::filesystem::path& FileSink::path() const {
  return path_;
}

void CaptureSink::Write(const Record& record) {
  std::lock_guard lock(mutex_);
  if (records_.size() == kCapacity) {
    records_.pop_front();
  }
  records_.push_back(CapturedRecord{record.level, std::string(record.category),
                                    std::string(record.text), record.time, record.thread_id,
                                    record.sequence});
}

void CaptureSink::Flush() {}

u64 CaptureSink::CopySince(u64 after, std::vector<CapturedRecord>& out) const {
  out.clear();
  std::lock_guard lock(mutex_);
  for (const CapturedRecord& record : records_) {
    if (record.sequence > after) {
      out.push_back(record);
    }
  }
  return records_.empty() ? after : std::max(after, records_.back().sequence);
}

u64 CaptureSink::latest() const {
  std::lock_guard lock(mutex_);
  return records_.empty() ? 0 : records_.back().sequence;
}

}  // namespace rex::log
