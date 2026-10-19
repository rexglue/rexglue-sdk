/**
 * @file        rexglue/ui/progress.h
 * @brief       ProgressView task list renderer for the codegen pipeline
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/codegen/progress_reporter.h>
#include <rex/types.h>

namespace rexglue::ui {

/** Per module task list view that implements rex::codegen::IProgressReporter. */
class ProgressView final : public rex::codegen::IProgressReporter {
 public:
  explicit ProgressView(std::string_view title);
  ~ProgressView() override;

  void BinaryLoaded(const rex::codegen::BinaryInfo& info) override;
  void ModuleStarted(std::string_view name, std::size_t index, std::size_t total) override;
  void PhaseChanged(std::string_view name) override;
  void ModuleFinished(std::chrono::milliseconds elapsed) override;
  void ProjectPhaseStarted(std::string_view name) override;
  void ProjectPhaseFinished() override;

 private:
  struct ModuleEntry {
    std::string name;
    std::vector<std::string> phases_seen;
    std::chrono::milliseconds elapsed{0};
    bool finished = false;
  };

  enum class ActiveLineKind : u8 { kNone, kModule, kProjectPhase };

  void RenderActiveLine();
  void RunSpinner(const std::stop_token& stop);

  std::mutex state_mutex_;
  std::string title_;
  std::vector<ModuleEntry> modules_;
  std::size_t current_module_ = 0;
  std::string current_phase_;
  std::string current_project_phase_;
  std::size_t spinner_frame_ = 0;
  ActiveLineKind active_line_kind_ = ActiveLineKind::kNone;
  bool tty_ = false;

  std::condition_variable_any spinner_cv_;
  std::jthread spinner_thread_;
};

}  // namespace rexglue::ui
