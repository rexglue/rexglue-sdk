/**
 * @file        rexglue/ui/ui.cpp
 * @brief       Presentation layer implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <string>

#include <fmt/format.h>
#include <spdlog/details/log_msg.h>

#include <rex/logging.h>
#include <rex/platform/console.h>

namespace rexglue::ui {

namespace {

std::shared_ptr<PresentationSink> g_sink_ptr;
PresentationSink* g_sink = nullptr;

void RequireGlobalSink() {
  if (!g_sink) {
    throw std::logic_error("rexglue::ui not initialised; call ui::Init first");
  }
}

std::string_view LevelLetter(spdlog::level::level_enum lvl) {
  switch (lvl) {
    case spdlog::level::trace:
      return "T";
    case spdlog::level::debug:
      return "D";
    case spdlog::level::info:
      return "I";
    case spdlog::level::warn:
      return "W";
    case spdlog::level::err:
      return "E";
    case spdlog::level::critical:
      return "C";
    default:
      return "?";
  }
}

std::string_view LevelColor(spdlog::level::level_enum lvl) {
  switch (lvl) {
    case spdlog::level::trace:
      return color::kDim;
    case spdlog::level::debug:
      return color::kCyan;
    case spdlog::level::info:
      return color::kGreen;
    case spdlog::level::warn:
      return color::kBoldYellow;
    case spdlog::level::err:
    case spdlog::level::critical:
      return color::kBoldRed;
    default:
      return {};
  }
}

}  // namespace

PresentationSink::PresentationSink(std::ostream& out, bool tty, bool color)
    : out_(out), tty_(tty), color_(color) {}

void PresentationSink::sink_it_(const spdlog::details::log_msg& msg) {
  if (active_progress_ && tty_) {
    out_.write("\r\033[K", 4);
  }
  if (tty_) {
    out_.put('[');
    if (color_) {
      auto col = LevelColor(msg.level);
      out_.write(col.data(), static_cast<std::streamsize>(col.size()));
      auto letter = LevelLetter(msg.level);
      out_.write(letter.data(), static_cast<std::streamsize>(letter.size()));
      out_.write(color::kReset.data(), static_cast<std::streamsize>(color::kReset.size()));
    } else {
      auto letter = LevelLetter(msg.level);
      out_.write(letter.data(), static_cast<std::streamsize>(letter.size()));
    }
    out_.write("] ", 2);
  }
  out_.write(msg.payload.data(), static_cast<std::streamsize>(msg.payload.size()));
  out_.put('\n');
}

void PresentationSink::flush_() {
  out_.flush();
}

void PresentationSink::setActiveProgress(bool active) {
  std::lock_guard lock(this->mutex_);
  active_progress_ = active;
}

namespace detail {

GlobalSinkAccessor::GlobalSinkAccessor() {
  RequireGlobalSink();
  g_sink->sink_mutex().lock();
}

GlobalSinkAccessor::~GlobalSinkAccessor() {
  g_sink->sink_mutex().unlock();
}

void GlobalSinkAccessor::writeRaw(std::string_view s) {
  g_sink->sink_out().write(s.data(), static_cast<std::streamsize>(s.size()));
}

void GlobalSinkAccessor::writeLine(std::string_view s) {
  writeRaw(s);
  writeRaw("\n");
}

void GlobalSinkAccessor::flush() {
  g_sink->sink_out().flush();
}

bool GlobalSinkAccessor::color() const {
  return g_sink->color();
}

void GlobalSinkAccessor::writeColored(std::string_view code, std::string_view text) {
  if (g_sink->color()) {
    writeRaw(code);
    writeRaw(text);
    writeRaw(color::kReset);
  } else {
    writeRaw(text);
  }
}

void SetGlobalSinkForTesting(std::unique_ptr<PresentationSink> sink) {
  Shutdown();
  g_sink_ptr = std::shared_ptr<PresentationSink>(sink.release());
  g_sink = g_sink_ptr.get();
}

bool SinkIsTty() {
  RequireGlobalSink();
  return g_sink->tty();
}

void SetSinkActiveProgress(bool active) {
  RequireGlobalSink();
  g_sink->setActiveProgress(active);
}

}  // namespace detail

void Init(const InitOptions& opts) {
  if (g_sink)
    return;
  g_sink_ptr = std::make_shared<PresentationSink>(std::cerr, opts.tty, opts.color);
  g_sink = g_sink_ptr.get();
  rex::ReplaceConsoleSink(g_sink_ptr);

  if (opts.tty) {
    rex::platform::SetUTF8OutputCodePage();
    if (opts.color) {
      rex::platform::EnableANSIEscapes(stderr);
    }
  }
}

void Shutdown() {
  if (!g_sink)
    return;
  rex::ReplaceConsoleSink(nullptr);
  g_sink_ptr.reset();
  g_sink = nullptr;
}

void Banner(std::string_view title) {
  detail::GlobalSinkAccessor acc;
  acc.writeColored(color::kBold, title);
  acc.writeRaw("\n\n");
}

void KeyValueBlock(std::string_view header, std::span<const KeyValueRow> rows) {
  detail::GlobalSinkAccessor acc;
  if (!header.empty()) {
    acc.writeColored(color::kBold, header);
    acc.writeRaw("\n");
  }
  std::size_t key_width = 0;
  for (const auto& r : rows)
    key_width = std::max(key_width, r.key.size());
  for (const auto& r : rows) {
    acc.writeRaw("  ");
    acc.writeColored(color::kCyan, r.key);
    acc.writeRaw(":");
    if (key_width > r.key.size()) {
      acc.writeRaw(std::string(key_width - r.key.size() + 1, ' '));
    } else {
      acc.writeRaw(" ");
    }
    acc.writeLine(r.value);
  }
  acc.writeRaw("\n");
}

void ManualReviewList(std::string_view header, std::span<const ManualReviewRow> rows) {
  detail::GlobalSinkAccessor acc;
  if (!header.empty()) {
    acc.writeColored(color::kBoldYellow, header);
    acc.writeRaw("\n");
  }
  for (const auto& r : rows) {
    acc.writeRaw("  ");
    acc.writeColored(color::kCyan, r.location);
    acc.writeRaw("  ");
    acc.writeLine(r.detail);
    if (!r.hint.empty()) {
      acc.writeRaw("    ");
      acc.writeColored(color::kDim, r.hint);
      acc.writeRaw("\n");
    }
  }
  acc.writeRaw("\n");
}

void DoneSummary(std::chrono::milliseconds elapsed) {
  detail::GlobalSinkAccessor acc;
  acc.writeColored(color::kBoldGreen, fmt::format("Done in {:.1f}s.", elapsed.count() / 1000.0));
  acc.writeRaw("\n");
  acc.flush();
}

void FailureSummary(std::string_view reason, std::chrono::milliseconds elapsed) {
  detail::GlobalSinkAccessor acc;
  acc.writeColored(color::kBoldRed,
                   fmt::format("Failed: {} (after {:.1f}s)", reason, elapsed.count() / 1000.0));
  acc.writeRaw("\n");
  acc.flush();
}

}  // namespace rexglue::ui
