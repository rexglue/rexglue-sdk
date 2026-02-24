/**
 * @file        ui/overlay/settings_overlay.cpp
 *
 * @brief       Settings overlay implementation. See settings_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/cvar.h>
#include <rex/ui/keybinds.h>
#include <imgui.h>
#include <algorithm>
#include <string>
#include <vector>

namespace rex::ui {

SettingsDialog::SettingsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path)
    : ImGuiDialog(imgui_drawer), config_path_(std::move(config_path)) {
  RegisterBind("bind_settings", "F4", "Toggle settings overlay", [this] { ToggleVisible(); });
}

SettingsDialog::~SettingsDialog() {
  UnregisterBind("bind_settings");
}

static const char* LifecycleBadge(rex::cvar::Lifecycle lc) {
  switch (lc) {
    case rex::cvar::Lifecycle::kHotReload:
      return " [live]";
    case rex::cvar::Lifecycle::kRequiresRestart:
      return " [restart]";
    case rex::cvar::Lifecycle::kInitOnly:
      return " [init-only]";
  }
  return "";
}

static ImVec4 LifecycleColor(rex::cvar::Lifecycle lc) {
  switch (lc) {
    case rex::cvar::Lifecycle::kHotReload:
      return {0.4f, 1.0f, 0.4f, 1.0f};
    case rex::cvar::Lifecycle::kRequiresRestart:
      return {1.0f, 1.0f, 0.4f, 1.0f};
    case rex::cvar::Lifecycle::kInitOnly:
      return {1.0f, 0.4f, 0.4f, 1.0f};
  }
  return {1.0f, 1.0f, 1.0f, 1.0f};
}

void SettingsDialog::OnDraw(ImGuiIO& /*io*/) {
  if (!visible_)
    return;

  auto& registry = rex::cvar::GetRegistry();

  // Collect sorted unique category names.
  std::vector<std::string> categories;
  for (auto& entry : registry) {
    if (std::find(categories.begin(), categories.end(), entry.category) == categories.end()) {
      categories.push_back(entry.category);
    }
  }
  std::sort(categories.begin(), categories.end());

  const std::string search(search_buf_);
  const bool searching = !search.empty();

  ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.85f);
  if (!ImGui::Begin("Settings##rex", &visible_, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }

  // Search bar at the top (full width).
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputText("##search", search_buf_, sizeof(search_buf_));
  ImGui::SameLine(0, 0);
  ImGui::Dummy(ImVec2(0, 0));

  ImGui::Separator();

  const float panel_width = 140.0f;
  ImGui::BeginChild("##cats", ImVec2(panel_width, -30.0f), true);
  for (int i = 0; i < static_cast<int>(categories.size()); ++i) {
    if (ImGui::Selectable(categories[i].c_str(), selected_category_ == i)) {
      selected_category_ = i;
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("##cvars", ImVec2(0, -30.0f), false);
  for (auto& entry : registry) {
    // Filter by category (unless searching).
    if (!searching) {
      if (selected_category_ >= 0 && selected_category_ < static_cast<int>(categories.size()) &&
          entry.category != categories[selected_category_]) {
        continue;
      }
    } else {
      // Search matches name or description (case-insensitive substring).
      std::string name_lower = entry.name;
      std::string search_lower = search;
      auto to_lower = [](std::string& s) {
        for (auto& c : s)
          c = static_cast<char>(std::tolower(c));
      };
      to_lower(name_lower);
      to_lower(search_lower);
      if (name_lower.find(search_lower) == std::string::npos &&
          entry.description.find(search_lower) == std::string::npos) {
        continue;
      }
    }

    bool read_only = (entry.lifecycle == rex::cvar::Lifecycle::kInitOnly);

    ImGui::PushID(entry.name.c_str());

    // Value control - width is right portion of the row.
    ImGui::SetNextItemWidth(160.0f);
    if (read_only)
      ImGui::BeginDisabled();

    std::string current_val = entry.getter();

    if (entry.type == rex::cvar::FlagType::Boolean) {
      bool v = (current_val == "true");
      if (ImGui::Checkbox("##v", &v)) {
        rex::cvar::SetFlagByName(entry.name, v ? "true" : "false");
      }
    } else if (entry.type == rex::cvar::FlagType::String &&
               !entry.constraints.allowed_values.empty()) {
      const auto& opts = entry.constraints.allowed_values;
      int cur_idx = 0;
      for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
        if (opts[i] == current_val) {
          cur_idx = i;
          break;
        }
      }
      if (ImGui::BeginCombo("##v", opts[cur_idx].c_str())) {
        for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
          bool sel = (i == cur_idx);
          if (ImGui::Selectable(opts[i].c_str(), sel)) {
            rex::cvar::SetFlagByName(entry.name, opts[i]);
          }
          if (sel)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
    } else if (entry.type == rex::cvar::FlagType::Int32 ||
               entry.type == rex::cvar::FlagType::Int64 ||
               entry.type == rex::cvar::FlagType::Uint32 ||
               entry.type == rex::cvar::FlagType::Uint64) {
      int v = std::atoi(current_val.c_str());
      int vmin =
          entry.constraints.min.has_value() ? static_cast<int>(*entry.constraints.min) : INT_MIN;
      int vmax =
          entry.constraints.max.has_value() ? static_cast<int>(*entry.constraints.max) : INT_MAX;
      if (ImGui::InputInt("##v", &v)) {
        v = std::clamp(v, vmin, vmax);
        rex::cvar::SetFlagByName(entry.name, std::to_string(v));
      }
    } else if (entry.type == rex::cvar::FlagType::Double) {
      double v = std::atof(current_val.c_str());
      if (ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.4f")) {
        if (entry.constraints.min)
          v = std::max(v, *entry.constraints.min);
        if (entry.constraints.max)
          v = std::min(v, *entry.constraints.max);
        rex::cvar::SetFlagByName(entry.name, std::to_string(v));
      }
    } else {
      // String (no allowed values)
      char buf[256];
      std::strncpy(buf, current_val.c_str(), sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
      if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        rex::cvar::SetFlagByName(entry.name, buf);
      }
    }

    if (read_only)
      ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Text("%s", entry.name.c_str());

    // Lifecycle badge.
    ImGui::SameLine();
    ImGui::TextColored(LifecycleColor(entry.lifecycle), "%s", LifecycleBadge(entry.lifecycle));

    // Description tooltip.
    if (!entry.description.empty() && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", entry.description.c_str());
    }

    ImGui::PopID();
  }
  ImGui::EndChild();

  // Bottom bar: Save button.
  ImGui::Separator();
  if (ImGui::Button("Save to config")) {
    rex::cvar::SaveConfig(config_path_);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", config_path_.filename().string().c_str());

  ImGui::End();
}

}  // namespace rex::ui
