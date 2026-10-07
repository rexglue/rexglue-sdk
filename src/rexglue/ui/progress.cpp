/**
 * @file        rexglue/ui/progress.cpp
 * @brief       ProgressView implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "progress.h"

#include "glyphs.h"
#include "ui.h"

#include <array>
#include <chrono>
#include <ctime>
#include <format>

namespace rexglue::ui {

namespace {
constexpr std::string_view kEraseLine = "\r\033[K";
constexpr auto kSpinnerInterval = std::chrono::milliseconds(100);
}  // namespace

ProgressView::ProgressView(std::string_view title) : title_(title) {
  {
    detail::GlobalSinkAccessor acc;
    acc.writeColored(color::kBold, title_);
    acc.writeRaw("\n");
    tty_ = detail::SinkIsTty();
  }
  if (tty_) {
    detail::SetSinkActiveProgress(true);
    spinner_thread_ = std::thread(&ProgressView::spinnerLoop, this);
  }
}

ProgressView::~ProgressView() {
  if (tty_) {
    stop_spinner_.store(true, std::memory_order_release);
    spinner_cv_.notify_all();
    if (spinner_thread_.joinable())
      spinner_thread_.join();
    detail::SetSinkActiveProgress(false);
  }
  std::lock_guard lock(state_mutex_);
  if (active_line_kind_ != ActiveLineKind::None) {
    detail::GlobalSinkAccessor acc;
    acc.writeRaw(kEraseLine);
    active_line_kind_ = ActiveLineKind::None;
  }
}

void ProgressView::clearActiveLine() {
  if (active_line_kind_ == ActiveLineKind::None || !tty_)
    return;
  detail::GlobalSinkAccessor acc;
  acc.writeRaw(kEraseLine);
  active_line_kind_ = ActiveLineKind::None;
}

void ProgressView::renderActiveLine() {
  if (!tty_)
    return;
  detail::GlobalSinkAccessor acc;
  acc.writeRaw(kEraseLine);
  spinner_frame_ = (spinner_frame_ + 1) % glyphs::kSpinnerFrames.size();
  acc.writeColored(color::kCyan, glyphs::kSpinnerFrames[spinner_frame_]);
  acc.writeRaw(" ");
  if (active_line_kind_ == ActiveLineKind::Module) {
    if (current_module_ >= modules_.size())
      return;
    acc.writeColored(color::kBold, modules_[current_module_].name);
    acc.writeRaw(" [");
    acc.writeColored(color::kDim, current_phase_.empty() ? std::string_view{"start"}
                                                         : std::string_view{current_phase_});
    acc.writeRaw("]");
  } else if (active_line_kind_ == ActiveLineKind::ProjectPhase) {
    acc.writeColored(color::kBold, std::format("project::{}", current_project_phase_));
  }
  acc.flush();
}

void ProgressView::spinnerLoop() {
  std::unique_lock cv_lock(state_mutex_);
  while (!stop_spinner_.load(std::memory_order_acquire)) {
    if (active_line_kind_ != ActiveLineKind::None)
      renderActiveLine();
    spinner_cv_.wait_for(cv_lock, kSpinnerInterval,
                         [this] { return stop_spinner_.load(std::memory_order_acquire); });
  }
}

void ProgressView::binaryInfo(const rex::codegen::BinaryInfo& info) {
  std::string filetime;
  if (info.pe_time_date_stamp == 0) {
    filetime = "(none)";
  } else {
    std::time_t t = static_cast<std::time_t>(info.pe_time_date_stamp);
    filetime = std::format("{:%F %T} UTC", std::chrono::sys_seconds{std::chrono::seconds{t}});
  }

  std::array<KeyValueRow, 4> rows{{
      {"Title ID", std::format("{:08X}", info.title_id)},
      {"Media ID", std::format("{:08X}", info.media_id)},
      {"Version", std::format("{}.{}.{}.{}", info.version_major, info.version_minor,
                              info.version_build, info.version_qfe)},
      {"Filetime", std::move(filetime)},
  }};
  KeyValueBlock(info.name, rows);
}

void ProgressView::moduleStarted(std::string_view name, std::size_t index, std::size_t total) {
  (void)total;
  std::lock_guard lock(state_mutex_);
  current_module_ = index;
  if (modules_.size() <= index)
    modules_.resize(index + 1);
  modules_[index].name = std::string(name);
  current_phase_.clear();

  if (!tty_) {
    detail::GlobalSinkAccessor acc;
    acc.writeLine(std::format("  start  {}", name));
    return;
  }

  active_line_kind_ = ActiveLineKind::Module;
  renderActiveLine();
}

void ProgressView::phaseChanged(std::string_view name) {
  std::lock_guard lock(state_mutex_);
  if (current_module_ >= modules_.size())
    return;
  modules_[current_module_].phases_seen.emplace_back(name);
  current_phase_ = std::string(name);

  if (!tty_) {
    detail::GlobalSinkAccessor acc;
    acc.writeLine(std::format("  phase  {}: {}", modules_[current_module_].name, name));
    return;
  }

  active_line_kind_ = ActiveLineKind::Module;
  renderActiveLine();
}

void ProgressView::moduleFinished(std::chrono::milliseconds elapsed) {
  std::lock_guard lock(state_mutex_);
  if (current_module_ >= modules_.size())
    return;
  modules_[current_module_].elapsed = elapsed;
  modules_[current_module_].finished = true;

  if (!tty_) {
    detail::GlobalSinkAccessor acc;
    acc.writeLine(std::format("  done   {} ({:.1f}s)", modules_[current_module_].name,
                              elapsed.count() / 1000.0));
    return;
  }

  detail::GlobalSinkAccessor acc;
  acc.writeRaw(kEraseLine);
  acc.writeRaw("  ");
  acc.writeColored(color::kGreen, glyphs::kCheckMark);
  acc.writeRaw(" ");
  acc.writeRaw(modules_[current_module_].name);
  acc.writeRaw(" ");
  acc.writeColored(color::kDim, std::format("({:.1f}s)", elapsed.count() / 1000.0));
  acc.writeRaw("\n");
  active_line_kind_ = ActiveLineKind::None;
}

void ProgressView::projectPhaseStarted(std::string_view name) {
  std::lock_guard lock(state_mutex_);
  current_project_phase_ = std::string(name);

  if (!tty_) {
    detail::GlobalSinkAccessor acc;
    acc.writeLine(std::format("  start  project::{}", name));
    return;
  }

  active_line_kind_ = ActiveLineKind::ProjectPhase;
  renderActiveLine();
}

void ProgressView::projectPhaseFinished() {
  std::lock_guard lock(state_mutex_);
  if (!tty_) {
    detail::GlobalSinkAccessor acc;
    acc.writeLine("  done   project phase");
    return;
  }

  detail::GlobalSinkAccessor acc;
  acc.writeRaw(kEraseLine);
  acc.writeRaw("  ");
  acc.writeColored(color::kGreen, glyphs::kCheckMark);
  acc.writeRaw(" ");
  acc.writeRaw(std::format("project::{}\n", current_project_phase_));
  active_line_kind_ = ActiveLineKind::None;
}

}  // namespace rexglue::ui
