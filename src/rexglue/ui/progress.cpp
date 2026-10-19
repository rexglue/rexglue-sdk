#include "progress.h"

#include "glyphs.h"
#include "ui.h"

#include <array>
#include <chrono>
#include <ctime>
#include <format>
#include <utility>

namespace rexglue::ui {

namespace {
constexpr std::string_view kEraseLine = "\r\033[K";
constexpr auto kSpinnerInterval = std::chrono::milliseconds(100);
}  // namespace

ProgressView::ProgressView(std::string_view title) : heading_(title) {
  {
    detail::GlobalSinkAccessor acc;
    acc.WriteColored(color::kBold, heading_);
    acc.WriteRaw("\n");
    tty_ = detail::IsSinkTTY();
  }
  if (tty_) {
    detail::SetSinkActiveProgress(true);
    spinner_thread_ = std::jthread([this](const std::stop_token& stop) { RunSpinner(stop); });
  }
}

ProgressView::~ProgressView() {
  if (tty_) {
    spinner_thread_.request_stop();
    spinner_thread_.join();
    detail::SetSinkActiveProgress(false);
  }
  std::lock_guard lock(state_mutex_);
  if (active_line_kind_ != ActiveLineKind::kNone) {
    detail::GlobalSinkAccessor acc;
    acc.WriteRaw(kEraseLine);
    active_line_kind_ = ActiveLineKind::kNone;
  }
}

void ProgressView::RenderActiveLine() {
  if (!tty_)
    return;
  detail::GlobalSinkAccessor acc;
  acc.WriteRaw(kEraseLine);
  spinner_frame_ = (spinner_frame_ + 1) % glyphs::kSpinnerFrames.size();
  acc.WriteColored(color::kCyan, glyphs::kSpinnerFrames[spinner_frame_]);
  acc.WriteRaw(" ");
  if (active_line_kind_ == ActiveLineKind::kModule) {
    if (current_module_ >= modules_.size())
      return;
    acc.WriteColored(color::kBold, modules_[current_module_].name);
    acc.WriteRaw(" [");
    acc.WriteColored(color::kDim, current_phase_.empty() ? std::string_view{"start"}
                                                         : std::string_view{current_phase_});
    acc.WriteRaw("]");
  } else if (active_line_kind_ == ActiveLineKind::kProjectPhase) {
    acc.WriteColored(color::kBold, std::format("project::{}", current_project_phase_));
  }
  acc.Flush();
}

void ProgressView::RunSpinner(const std::stop_token& stop) {
  std::unique_lock cv_lock(state_mutex_);
  while (!stop.stop_requested()) {
    if (active_line_kind_ != ActiveLineKind::kNone)
      RenderActiveLine();
    spinner_cv_.wait_for(cv_lock, stop, kSpinnerInterval, [] { return false; });
  }
}

void ProgressView::BinaryLoaded(const rex::codegen::BinaryIdentity& info) {
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

void ProgressView::ModuleStarted(std::string_view name, std::size_t index, std::size_t total) {
  (void)total;
  std::lock_guard lock(state_mutex_);
  current_module_ = index;
  if (modules_.size() <= index)
    modules_.resize(index + 1);
  modules_[index].name = std::string(name);
  current_phase_.clear();

  if (!tty_) {
    detail::GlobalSinkAccessor acc;
    acc.WriteLine(std::format("  start  {}", name));
    return;
  }

  active_line_kind_ = ActiveLineKind::kModule;
  RenderActiveLine();
}

void ProgressView::ModulePhaseStarted(std::string_view name) {
  std::lock_guard lock(state_mutex_);
  if (current_module_ >= modules_.size())
    return;
  modules_[current_module_].phases_seen.emplace_back(name);
  current_phase_ = std::string(name);

  if (!tty_) {
    detail::GlobalSinkAccessor acc;
    acc.WriteLine(std::format("  phase  {}: {}", modules_[current_module_].name, name));
    return;
  }

  active_line_kind_ = ActiveLineKind::kModule;
  RenderActiveLine();
}

void ProgressView::ModuleFinished(std::chrono::milliseconds elapsed) {
  std::lock_guard lock(state_mutex_);
  if (current_module_ >= modules_.size())
    return;
  modules_[current_module_].elapsed = elapsed;
  modules_[current_module_].finished = true;

  if (!tty_) {
    detail::GlobalSinkAccessor acc;
    acc.WriteLine(std::format("  done   {} ({:.1f}s)", modules_[current_module_].name,
                              std::chrono::duration<f64>(elapsed).count()));
    return;
  }

  detail::GlobalSinkAccessor acc;
  acc.WriteRaw(kEraseLine);
  acc.WriteRaw("  ");
  acc.WriteColored(color::kGreen, glyphs::kCheckMark);
  acc.WriteRaw(" ");
  acc.WriteRaw(modules_[current_module_].name);
  acc.WriteRaw(" ");
  acc.WriteColored(color::kDim,
                   std::format("({:.1f}s)", std::chrono::duration<f64>(elapsed).count()));
  acc.WriteRaw("\n");
  active_line_kind_ = ActiveLineKind::kNone;
}

void ProgressView::ProjectPhaseStarted(std::string_view name) {
  std::lock_guard lock(state_mutex_);
  current_project_phase_ = std::string(name);

  if (!tty_) {
    detail::GlobalSinkAccessor acc;
    acc.WriteLine(std::format("  start  project::{}", name));
    return;
  }

  active_line_kind_ = ActiveLineKind::kProjectPhase;
  RenderActiveLine();
}

void ProgressView::ProjectPhaseFinished() {
  std::lock_guard lock(state_mutex_);
  if (!tty_) {
    detail::GlobalSinkAccessor acc;
    acc.WriteLine("  done   project phase");
    return;
  }

  detail::GlobalSinkAccessor acc;
  acc.WriteRaw(kEraseLine);
  acc.WriteRaw("  ");
  acc.WriteColored(color::kGreen, glyphs::kCheckMark);
  acc.WriteRaw(" ");
  acc.WriteRaw(std::format("project::{}\n", current_project_phase_));
  active_line_kind_ = ActiveLineKind::kNone;
}

}  // namespace rexglue::ui
