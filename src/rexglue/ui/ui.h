/**
 * @file        rexglue/ui/ui.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/logging/logger.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rexglue::ui {

namespace detail {
class GlobalSinkAccessor;
}

/** Log sink that owns presentation output for the rexglue CLI. */
class PresentationSink final : public rex::log::ILogSink {
 public:
  PresentationSink(std::ostream& out, bool tty, bool color);

  void Write(const rex::log::Message& message) override;
  void Flush() override;

  bool IsTTY() const { return tty_; }
  bool IsColorEnabled() const { return color_; }

  void SetActiveProgress(bool active);

 private:
  friend class detail::GlobalSinkAccessor;

  std::mutex& GetSinkMutex() { return mutex_; }
  std::ostream& GetSinkOut() { return out_; }

  std::ostream& out_;
  bool tty_;
  bool color_;
  bool active_progress_ = false;
  std::mutex mutex_;
};

struct InitOptions {
  bool tty;
  bool color;
};

void Init(const InitOptions& opts);
void Shutdown();

/** Returns the sink to pass to Logger::AddSink. */
rex::log::ILogSink& GetLogSink();

void Banner(std::string_view title);

struct KeyValueRow {
  std::string_view key;
  std::string value;
};
void KeyValueBlock(std::string_view header, std::span<const KeyValueRow> rows);

struct ManualReviewRow {
  std::string location;
  std::string detail;
  std::string hint;
};
void ManualReviewList(std::string_view header, std::span<const ManualReviewRow> rows);

void DoneSummary(std::chrono::milliseconds elapsed);
void FailureSummary(std::string_view reason, std::chrono::milliseconds elapsed);

namespace detail {

void SetGlobalSinkForTesting(std::unique_ptr<PresentationSink> sink);

bool IsSinkTTY();

void SetSinkActiveProgress(bool active);

/** Holds the global sink's mutex while writing. */
class GlobalSinkAccessor : public rex::NonCopyable {
 public:
  GlobalSinkAccessor();

  void WriteLine(std::string_view s);
  void WriteRaw(std::string_view s);
  void Flush();

  bool GetColor() const;

  void WriteColored(std::string_view code, std::string_view text);

 private:
  std::unique_lock<std::mutex> lock_;
};

}  // namespace detail

namespace color {
inline constexpr std::string_view kReset = rex::log::kColorReset;
inline constexpr std::string_view kBold = "\033[1m";
inline constexpr std::string_view kDim = "\033[2m";
inline constexpr std::string_view kGreen = "\033[32m";
inline constexpr std::string_view kCyan = "\033[36m";
inline constexpr std::string_view kBoldGreen = "\033[1;32m";
inline constexpr std::string_view kBoldRed = "\033[1;31m";
inline constexpr std::string_view kBoldYellow = "\033[1;33m";
}  // namespace color

}  // namespace rexglue::ui
