/**
 * @file        rex/ui/overlay/console_overlay.h
 *
 * @brief       ImGui console overlay dialog for viewing captured log output.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/log_capture.h>
#include <rex/ui/imgui_dialog.h>
#include <imgui.h>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace rex::ui {

class ConsoleDialog : public ImGuiDialog {
 public:
  ConsoleDialog(ImGuiDrawer* imgui_drawer, std::shared_ptr<rex::LogCaptureSink> sink);
  ~ConsoleDialog();

  void ToggleVisible();
  bool IsVisible() const { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void ExecuteCommand(std::string_view cmd);
  void RefreshCategories();
  static int InputTextCallback(ImGuiInputTextCallbackData* data);

  bool visible_ = false;
  bool focus_input_next_frame_ = false;
  bool scroll_to_bottom_ = true;
  uint64_t last_generation_ = 0;

  std::shared_ptr<rex::LogCaptureSink> sink_;
  std::vector<rex::LogEntry> entries_;  // refreshed each frame when generation changes

  // Filters
  int min_level_ = 0;  // index into spdlog level enum; 0 = trace
  std::set<std::string> known_categories_;
  std::map<std::string, bool> category_filter_;  // category -> enabled

  // Command input
  char input_buf_[512] = {};
  std::deque<std::string> history_;
  int history_pos_ = -1;
  static constexpr size_t kMaxHistory = 32;
};

}  // namespace rex::ui
