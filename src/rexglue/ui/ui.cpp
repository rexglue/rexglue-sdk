#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <string>

#include <format>

#include <rex/logging/log_category.h>
#include <rex/platform/process.h>

namespace rexglue::ui {

namespace {

std::shared_ptr<PresentationSink> g_sink_ptr;
PresentationSink* g_sink = nullptr;

void RequireGlobalSink() {
  if (!g_sink) {
    throw std::logic_error("rexglue::ui not initialized. Call ui::Init first");
  }
}

}  // namespace

PresentationSink::PresentationSink(std::ostream& out, bool tty, bool color)
    : out_(out), tty_(tty), color_(color) {}

void PresentationSink::Write(const rex::log::Message& message) {
  std::lock_guard lock(mutex_);
  if (active_progress_ && tty_) {
    out_.write("\r\033[K", 4);
  }
  if (tty_) {
    out_.put('[');
    if (color_) {
      const auto col = rex::log::LevelColor(message.level);
      out_.write(col.data(), static_cast<std::streamsize>(col.size()));
      out_.put(rex::log::LevelLetter(message.level));
      out_.write(color::kReset.data(), static_cast<std::streamsize>(color::kReset.size()));
    } else {
      out_.put(rex::log::LevelLetter(message.level));
    }
    out_.write("] ", 2);
  }
  out_.write(message.text.data(), static_cast<std::streamsize>(message.text.size()));
  out_.put('\n');
}

void PresentationSink::Flush() {
  std::lock_guard lock(mutex_);
  out_.flush();
}

void PresentationSink::SetActiveProgress(bool active) {
  std::lock_guard lock(mutex_);
  active_progress_ = active;
}

namespace detail {

GlobalSinkAccessor::GlobalSinkAccessor() {
  RequireGlobalSink();
  lock_ = std::unique_lock(g_sink->GetSinkMutex());
}

void GlobalSinkAccessor::WriteRaw(std::string_view s) {
  g_sink->GetSinkOut().write(s.data(), static_cast<std::streamsize>(s.size()));
}

void GlobalSinkAccessor::WriteLine(std::string_view s) {
  WriteRaw(s);
  WriteRaw("\n");
}

void GlobalSinkAccessor::Flush() {
  g_sink->GetSinkOut().flush();
}

bool GlobalSinkAccessor::GetColor() const {
  return g_sink->IsColorEnabled();
}

void GlobalSinkAccessor::WriteColored(std::string_view code, std::string_view text) {
  if (g_sink->IsColorEnabled()) {
    WriteRaw(code);
    WriteRaw(text);
    WriteRaw(color::kReset);
  } else {
    WriteRaw(text);
  }
}

void SetGlobalSinkForTesting(std::unique_ptr<PresentationSink> sink) {
  Shutdown();
  g_sink_ptr = std::shared_ptr<PresentationSink>(sink.release());
  g_sink = g_sink_ptr.get();
}

bool IsSinkTTY() {
  RequireGlobalSink();
  return g_sink->IsTTY();
}

void SetSinkActiveProgress(bool active) {
  RequireGlobalSink();
  g_sink->SetActiveProgress(active);
}

}  // namespace detail

void Init(const InitOptions& opts) {
  if (g_sink)
    return;
  g_sink_ptr = std::make_shared<PresentationSink>(std::cerr, opts.tty, opts.color);
  g_sink = g_sink_ptr.get();

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
  g_sink_ptr.reset();
  g_sink = nullptr;
}

rex::log::ILogSink& GetLogSink() {
  RequireGlobalSink();
  return *g_sink;
}

void Banner(std::string_view title) {
  detail::GlobalSinkAccessor acc;
  acc.WriteColored(color::kBold, title);
  acc.WriteRaw("\n\n");
}

void KeyValueBlock(std::string_view header, std::span<const KeyValueRow> rows) {
  detail::GlobalSinkAccessor acc;
  if (!header.empty()) {
    acc.WriteColored(color::kBold, header);
    acc.WriteRaw("\n");
  }
  std::size_t key_width = 0;
  for (const auto& r : rows)
    key_width = std::max(key_width, r.key.size());
  for (const auto& r : rows) {
    acc.WriteRaw("  ");
    acc.WriteColored(color::kCyan, r.key);
    acc.WriteRaw(":");
    if (key_width > r.key.size()) {
      acc.WriteRaw(std::string(key_width - r.key.size() + 1, ' '));
    } else {
      acc.WriteRaw(" ");
    }
    acc.WriteLine(r.value);
  }
  acc.WriteRaw("\n");
}

void ManualReviewList(std::string_view header, std::span<const ManualReviewRow> rows) {
  detail::GlobalSinkAccessor acc;
  if (!header.empty()) {
    acc.WriteColored(color::kBoldYellow, header);
    acc.WriteRaw("\n");
  }
  for (const auto& r : rows) {
    acc.WriteRaw("  ");
    acc.WriteColored(color::kCyan, r.location);
    acc.WriteRaw("  ");
    acc.WriteLine(r.detail);
    if (!r.hint.empty()) {
      acc.WriteRaw("    ");
      acc.WriteColored(color::kDim, r.hint);
      acc.WriteRaw("\n");
    }
  }
  acc.WriteRaw("\n");
}

void DoneSummary(std::chrono::milliseconds elapsed) {
  detail::GlobalSinkAccessor acc;
  acc.WriteColored(color::kBoldGreen,
                   std::format("Done in {:.1f}s.", std::chrono::duration<f64>(elapsed).count()));
  acc.WriteRaw("\n");
  acc.Flush();
}

void FailureSummary(std::string_view reason, std::chrono::milliseconds elapsed) {
  detail::GlobalSinkAccessor acc;
  acc.WriteColored(color::kBoldRed, std::format("Failed: {} (after {:.1f}s)", reason,
                                                std::chrono::duration<f64>(elapsed).count()));
  acc.WriteRaw("\n");
  acc.Flush();
}

}  // namespace rexglue::ui
