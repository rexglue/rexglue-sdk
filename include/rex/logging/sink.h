/**
 * @file        rex/logging/sink.h
 * @brief       Thread-safe ring-buffer spdlog sink for in-memory log capture
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/logging/types.h>

#include <spdlog/sinks/base_sink.h>

#include <array>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace rex {

struct LogEntry {
  spdlog::level::level_enum level = spdlog::level::info;
  std::string category;  // e.g. "core", "gpu"
  std::string text;      // formatted message only (no timestamp/level prefix)
};

// Thread-safe ring-buffer spdlog sink.
// Capacity is fixed at 2048 entries; oldest entries are overwritten.
class LogCaptureSink : public spdlog::sinks::base_sink<std::mutex> {
 public:
  static constexpr size_t kCapacity = 2048;

  // Copy all current entries in chronological order into `out`.
  // Safe to call from any thread (e.g. the UI/render thread).
  void CopyEntries(std::vector<LogEntry>& out) const {
    std::lock_guard lock(mutable_mutex_);
    out.clear();
    out.reserve(count_);
    if (count_ < kCapacity) {
      // Buffer not yet full - entries start at index 0.
      for (size_t i = 0; i < count_; ++i) {
        out.push_back(buf_[i]);
      }
    } else {
      // Full - oldest entry is at write_pos_ (wraps around).
      for (size_t i = 0; i < kCapacity; ++i) {
        out.push_back(buf_[(write_pos_ + i) % kCapacity]);
      }
    }
  }

  // Returns true if new entries have been added since the last call
  // to CopyEntries (checked by comparing generation counter).
  uint64_t generation() const {
    std::lock_guard lock(mutable_mutex_);
    return generation_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    std::string cat(msg.logger_name.begin(), msg.logger_name.end());

    // Format just the payload (no timestamp, level, or logger name).
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    // Strip trailing newline spdlog adds.
    std::string text(formatted.begin(), formatted.end());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
      text.pop_back();
    }

    buf_[write_pos_] = LogEntry{msg.level, std::move(cat), std::move(text)};
    write_pos_ = (write_pos_ + 1) % kCapacity;
    if (count_ < kCapacity)
      ++count_;
    ++generation_;
  }

  void flush_() override {}

 private:
  std::array<LogEntry, kCapacity> buf_{};
  size_t write_pos_ = 0;
  size_t count_ = 0;
  uint64_t generation_ = 0;
  mutable std::mutex mutable_mutex_;
};

}  // namespace rex
