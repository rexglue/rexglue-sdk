/**
 * @file        ui/overlay/console_overlay.cpp
 *
 * @brief       Console overlay implementation. See console_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/console_overlay.h>
#include <rex/cvar.h>
#include <rex/ui/keybinds.h>
#include <imgui.h>
#include <algorithm>
#include <string>

namespace rex::ui {

static ImVec4 LevelColor(spdlog::level::level_enum level) {
  // todo(tomc): make these cvar driven
  switch (level) {
    case spdlog::level::trace:
      return {0.5f, 0.5f, 0.5f, 1.0f};  // grey
    case spdlog::level::debug:
      return {0.4f, 0.9f, 0.9f, 1.0f};  // cyan
    case spdlog::level::info:
      return {1.0f, 1.0f, 1.0f, 1.0f};  // white
    case spdlog::level::warn:
      return {1.0f, 1.0f, 0.0f, 1.0f};  // yellow
    case spdlog::level::err:
      return {1.0f, 0.4f, 0.4f, 1.0f};  // red
    case spdlog::level::critical:
      return {1.0f, 0.0f, 0.0f, 1.0f};  // bright red
    default:
      return {1.0f, 1.0f, 1.0f, 1.0f};
  }
}

ConsoleDialog::ConsoleDialog(ImGuiDrawer* imgui_drawer, std::shared_ptr<rex::LogCaptureSink> sink)
    : ImGuiDialog(imgui_drawer), sink_(std::move(sink)) {
  RegisterBind("bind_console", "Backtick", "Toggle console overlay", [this] { ToggleVisible(); });
}

ConsoleDialog::~ConsoleDialog() {
  UnregisterBind("bind_console");
}

void ConsoleDialog::ToggleVisible() {
  visible_ = !visible_;
  if (visible_) {
    focus_input_next_frame_ = true;
    scroll_to_bottom_ = true;
  }
}

void ConsoleDialog::RefreshCategories() {
  for (auto& entry : entries_) {
    if (entry.category.empty() || entry.category == "console")
      continue;
    if (known_categories_.insert(entry.category).second) {
      // New category discovered - enable by default.
      category_filter_[entry.category] = true;
    }
  }
}

int ConsoleDialog::InputTextCallback(ImGuiInputTextCallbackData* data) {
  auto* self = static_cast<ConsoleDialog*>(data->UserData);
  if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
    const int prev = self->history_pos_;
    if (data->EventKey == ImGuiKey_UpArrow) {
      if (self->history_pos_ == -1) {
        self->history_pos_ = static_cast<int>(self->history_.size()) - 1;
      } else if (self->history_pos_ > 0) {
        --self->history_pos_;
      }
    } else if (data->EventKey == ImGuiKey_DownArrow) {
      if (self->history_pos_ != -1) {
        if (++self->history_pos_ >= static_cast<int>(self->history_.size())) {
          self->history_pos_ = -1;
        }
      }
    }
    if (prev != self->history_pos_) {
      const char* hist =
          (self->history_pos_ >= 0) ? self->history_[self->history_pos_].c_str() : "";
      data->DeleteChars(0, data->BufTextLen);
      data->InsertChars(0, hist);
    }
  }
  return 0;
}

void ConsoleDialog::ExecuteCommand(std::string_view cmd) {
  // Trim whitespace.
  while (!cmd.empty() && cmd.front() == ' ')
    cmd.remove_prefix(1);
  while (!cmd.empty() && cmd.back() == ' ')
    cmd.remove_suffix(1);
  if (cmd.empty())
    return;

  // Record in history.
  if (history_.empty() || history_.back() != cmd) {
    if (history_.size() >= kMaxHistory)
      history_.pop_front();
    history_.push_back(std::string(cmd));
  }
  history_pos_ = -1;

  if (cmd == "help" || cmd == "?") {
    auto names = rex::cvar::ListFlags();
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
      const auto* info = rex::cvar::GetFlagInfo(n);
      std::string line = "  " + n;
      if (info)
        line += " = " + info->getter() + "  (" + info->description + ")";
      entries_.push_back({spdlog::level::info, "console", line});
    }
    return;
  }

  // Split on first space: "name value"
  auto sep = cmd.find(' ');
  if (sep == std::string_view::npos) {
    // No space: treat as "get" - show current value.
    std::string val = rex::cvar::GetFlagByName(cmd);
    if (val.empty() && !rex::cvar::GetFlagInfo(cmd)) {
      entries_.push_back(
          {spdlog::level::warn, "console", "[console] unknown cvar: " + std::string(cmd)});
    } else {
      entries_.push_back(
          {spdlog::level::info, "console", "[console] " + std::string(cmd) + " = " + val});
    }
    return;
  }

  std::string name(cmd.substr(0, sep));
  std::string value(cmd.substr(sep + 1));
  while (!value.empty() && value.front() == ' ')
    value.erase(value.begin());

  if (rex::cvar::SetFlagByName(name, value)) {
    entries_.push_back({spdlog::level::info, "console", "[console] " + name + " = " + value});
  } else {
    entries_.push_back({spdlog::level::warn, "console", "[console] unknown cvar: " + name});
  }
  scroll_to_bottom_ = true;
}

void ConsoleDialog::OnDraw(ImGuiIO& io) {
  if (!visible_)
    return;

  // Refresh entries if sink has new data.
  if (sink_) {
    uint64_t gen = sink_->generation();
    if (gen != last_generation_) {
      sink_->CopyEntries(entries_);
      last_generation_ = gen;
      RefreshCategories();
    }
  }

  const float window_height = io.DisplaySize.y * 0.45f;
  ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - window_height), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, window_height), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.80f);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar;

  if (!ImGui::Begin("Console##rex", &visible_, flags)) {
    ImGui::End();
    return;
  }

  // --- Filter bar ---
  static const char* kLevelNames[] = {"trace", "debug", "info", "warn", "error", "critical"};
  ImGui::Text("Level:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0f);
  ImGui::Combo("##lvl", &min_level_, kLevelNames, 6);
  ImGui::SameLine();
  ImGui::Text("Categories:");
  ImGui::SameLine();
  int cat_idx = 0;
  for (auto& [cat_name, enabled] : category_filter_) {
    ImGui::Checkbox(cat_name.c_str(), &enabled);
    if (++cat_idx < static_cast<int>(category_filter_.size()))
      ImGui::SameLine();
  }

  // --- Log area ---
  const float input_height = ImGui::GetFrameHeightWithSpacing() + 4.0f;
  ImGui::BeginChild("##log", ImVec2(0, -input_height), false, ImGuiWindowFlags_HorizontalScrollbar);

  bool at_bottom = (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f);

  for (auto& entry : entries_) {
    // Level filter.
    if (static_cast<int>(entry.level) < min_level_)
      continue;
    // Category filter.
    bool show_cat = false;
    auto it = category_filter_.find(entry.category);
    if (it != category_filter_.end()) {
      show_cat = it->second;
    }
    // "console" pseudo-category always shown.
    if (entry.category == "console")
      show_cat = true;
    if (!show_cat)
      continue;

    ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(entry.level));
    ImGui::TextUnformatted(entry.text.c_str());
    ImGui::PopStyleColor();
  }

  if (scroll_to_bottom_ || at_bottom) {
    ImGui::SetScrollHereY(1.0f);
    scroll_to_bottom_ = false;
  }
  ImGui::EndChild();

  // --- Command input ---
  ImGui::Separator();
  bool submit = false;
  ImGuiInputTextFlags input_flags =
      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
  ImGui::SetNextItemWidth(-1.0f);
  if (focus_input_next_frame_) {
    ImGui::SetKeyboardFocusHere();
    focus_input_next_frame_ = false;
  }
  if (ImGui::InputText("##cmd", input_buf_, sizeof(input_buf_), input_flags, InputTextCallback,
                       this)) {
    submit = true;
  }

  if (submit && input_buf_[0] != '\0') {
    ExecuteCommand(input_buf_);
    input_buf_[0] = '\0';
    ImGui::SetKeyboardFocusHere(-1);
  }

  ImGui::End();
}

}  // namespace rex::ui
