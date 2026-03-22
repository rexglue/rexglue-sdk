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
REXCVAR_DECLARE(bool, mnk_mode);

#include <algorithm>
#include <map>
#include <set>
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

static rex::ui::VirtualKey ImGuiKeyToVirtualKey(ImGuiKey key) {
  using VK = rex::ui::VirtualKey;
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kA) + (key - ImGuiKey_A));
  }
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
    return static_cast<VK>(static_cast<uint16_t>(VK::k0) + (key - ImGuiKey_0));
  }
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kF1) + (key - ImGuiKey_F1));
  }
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kNumpad0) + (key - ImGuiKey_Keypad0));
  }
  switch (key) {
    case ImGuiKey_Space:
      return VK::kSpace;
    case ImGuiKey_Enter:
      return VK::kReturn;
    case ImGuiKey_Escape:
      return VK::kEscape;
    case ImGuiKey_Tab:
      return VK::kTab;
    case ImGuiKey_Backspace:
      return VK::kBack;
    case ImGuiKey_Delete:
      return VK::kDelete;
    case ImGuiKey_Insert:
      return VK::kInsert;
    case ImGuiKey_Home:
      return VK::kHome;
    case ImGuiKey_End:
      return VK::kEnd;
    case ImGuiKey_PageUp:
      return VK::kPrior;
    case ImGuiKey_PageDown:
      return VK::kNext;
    case ImGuiKey_LeftArrow:
      return VK::kLeft;
    case ImGuiKey_RightArrow:
      return VK::kRight;
    case ImGuiKey_UpArrow:
      return VK::kUp;
    case ImGuiKey_DownArrow:
      return VK::kDown;
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift:
      return VK::kShift;
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl:
      return VK::kControl;
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt:
      return VK::kMenu;
    case ImGuiKey_CapsLock:
      return VK::kCapital;
    case ImGuiKey_NumLock:
      return VK::kNumLock;
    case ImGuiKey_ScrollLock:
      return VK::kScroll;
    case ImGuiKey_PrintScreen:
      return VK::kSnapshot;
    case ImGuiKey_Pause:
      return VK::kPause;
    case ImGuiKey_GraveAccent:
      return VK::kOem3;
    case ImGuiKey_Minus:
      return VK::kOemMinus;
    case ImGuiKey_Equal:
      return VK::kOemPlus;
    case ImGuiKey_LeftBracket:
      return VK::kOem4;
    case ImGuiKey_RightBracket:
      return VK::kOem6;
    case ImGuiKey_Backslash:
      return VK::kOem5;
    case ImGuiKey_Semicolon:
      return VK::kOem1;
    case ImGuiKey_Apostrophe:
      return VK::kOem7;
    case ImGuiKey_Comma:
      return VK::kOemComma;
    case ImGuiKey_Period:
      return VK::kOemPeriod;
    case ImGuiKey_Slash:
      return VK::kOem2;
    case ImGuiKey_KeypadDecimal:
      return VK::kDecimal;
    case ImGuiKey_KeypadDivide:
      return VK::kDivide;
    case ImGuiKey_KeypadMultiply:
      return VK::kMultiply;
    case ImGuiKey_KeypadSubtract:
      return VK::kSubtract;
    case ImGuiKey_KeypadAdd:
      return VK::kAdd;
    case ImGuiKey_KeypadEnter:
      return VK::kReturn;
    default:
      return VK::kNone;
  }
}

void SettingsDialog::OnDraw(ImGuiIO& /*io*/) {
  if (!visible_)
    return;

  auto& registry = rex::cvar::GetRegistry();

  // Collect sorted unique category paths.
  std::set<std::string> category_set;
  for (auto& entry : registry) {
    category_set.insert(entry.category);
  }

  // Build tree: for each category path like "Input/Keybinds/Controller",
  // also register the parent paths "Input" and "Input/Keybinds" as nodes.
  struct CatNode {
    std::string full_path;
    std::string label;  // leaf segment (e.g. "Controller")
    std::map<std::string, CatNode> children;
    bool has_direct_entries = false;
  };
  std::map<std::string, CatNode> tree;

  for (auto& cat : category_set) {
    std::map<std::string, CatNode>* level = &tree;
    std::string path_so_far;
    size_t start = 0;
    while (start < cat.size()) {
      size_t slash = cat.find('/', start);
      std::string segment =
          (slash == std::string::npos) ? cat.substr(start) : cat.substr(start, slash - start);
      if (!path_so_far.empty())
        path_so_far += "/";
      path_so_far += segment;
      auto& node = (*level)[segment];
      node.full_path = path_so_far;
      node.label = segment;
      if (path_so_far == cat)
        node.has_direct_entries = true;
      level = &node.children;
      start = (slash == std::string::npos) ? cat.size() : slash + 1;
    }
  }

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

  const float panel_width = 160.0f;
  ImGui::BeginChild("##cats", ImVec2(panel_width, -30.0f), true);

  // Recursive lambda to draw the category tree.
  std::function<void(const std::map<std::string, CatNode>&, int)> draw_tree;
  draw_tree = [&](const std::map<std::string, CatNode>& nodes, int depth) {
    for (auto& [key, node] : nodes) {
      if (node.children.empty()) {
        // Leaf node - selectable
        bool selected = (selected_category_ == node.full_path);
        if (depth > 0)
          ImGui::Indent(8.0f);
        if (ImGui::Selectable(node.label.c_str(), selected)) {
          selected_category_ = node.full_path;
        }
        if (depth > 0)
          ImGui::Unindent(8.0f);
      } else {
        // Parent node with children - use tree node
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow;
        if (node.has_direct_entries) {
          // Can be selected as well as expanded
          if (selected_category_ == node.full_path)
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        bool open = ImGui::TreeNodeEx(node.label.c_str(), flags);
        if (ImGui::IsItemClicked() && node.has_direct_entries) {
          selected_category_ = node.full_path;
        }
        if (open) {
          draw_tree(node.children, depth + 1);
          ImGui::TreePop();
        }
      }
    }
  };
  // Root node named after the config file
  std::string root_label = config_path_.stem().string();
  ImGuiTreeNodeFlags root_flags = ImGuiTreeNodeFlags_DefaultOpen;
  if (selected_category_.empty())
    root_flags |= ImGuiTreeNodeFlags_Selected;
  bool root_open = ImGui::TreeNodeEx(root_label.c_str(), root_flags);
  if (ImGui::IsItemClicked()) {
    selected_category_.clear();
  }
  if (root_open) {
    draw_tree(tree, 1);
    ImGui::TreePop();
  }

  ImGui::EndChild();

  ImGui::SameLine();

  // Helper: check if a CVAR's category matches the selected category.
  // Exact match or prefix match (e.g. selecting "Input" shows all "Input/*").
  auto category_matches = [&](const std::string& cat) -> bool {
    if (selected_category_.empty())
      return true;  // Root selected - show all
    if (cat == selected_category_)
      return true;
    if (cat.size() > selected_category_.size() &&
        cat.compare(0, selected_category_.size(), selected_category_) == 0 &&
        cat[selected_category_.size()] == '/') {
      return true;
    }
    return false;
  };

  // Helper: check if a category is a keybind category.
  auto is_keybind_category = [](const std::string& cat) -> bool {
    return cat == "Input/Keybinds" ||
           (cat.size() > 15 && cat.compare(0, 15, "Input/Keybinds/") == 0);
  };

  ImGui::BeginChild("##cvars", ImVec2(0, -30.0f), false);
  for (auto& entry : registry) {
    // Filter by category (unless searching).
    if (!searching) {
      if (!category_matches(entry.category)) {
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

    if (read_only)
      ImGui::BeginDisabled();

    std::string current_val = entry.getter();

    // Use description as display label if available, otherwise CVAR name
    const char* display_label =
        (!entry.description.empty()) ? entry.description.c_str() : entry.name.c_str();

    if (is_keybind_category(entry.category)) {
      // Grey out controller keybinds when MnK mode is disabled
      bool mnk_disabled = (entry.category == "Input/Keybinds/Controller" && !REXCVAR_GET(mnk_mode));
      if (mnk_disabled)
        ImGui::BeginDisabled();

      // Show description as label (e.g. "A button"), not the raw CVAR name
      ImGui::Text("%-20s", entry.description.c_str());
      ImGui::SameLine(240.0f);

      bool is_capturing = (capturing_bind_name_ == entry.name);

      if (is_capturing) {
        ImGui::Button("Press any key...##v", ImVec2(140.0f, 0));

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
          capturing_bind_name_.clear();
        } else {
          for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            auto imgui_key = static_cast<ImGuiKey>(k);
            if (imgui_key == ImGuiKey_Escape)
              continue;
            if (ImGui::IsKeyPressed(imgui_key)) {
              auto vk = ImGuiKeyToVirtualKey(imgui_key);
              std::string name = rex::ui::VirtualKeyToString(vk);
              if (!name.empty()) {
                rex::cvar::SetFlagByName(entry.name, name);
              }
              capturing_bind_name_.clear();
              break;
            }
          }
          for (int mb = 0; mb < 3; ++mb) {
            if (ImGui::IsMouseClicked(mb)) {
              const char* names[] = {"LMB", "RMB", "MMB"};
              rex::cvar::SetFlagByName(entry.name, names[mb]);
              capturing_bind_name_.clear();
              break;
            }
          }
        }
      } else {
        ImGui::SetNextItemWidth(80.0f);
        ImGui::Text("%-10s", current_val.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Rebind##v")) {
          capturing_bind_name_ = entry.name;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##v")) {
          rex::cvar::SetFlagByName(entry.name, entry.default_value);
        }
      }

      // Conflict detection
      if (!current_val.empty()) {
        int conflict_count = 0;
        for (auto& other : registry) {
          if (is_keybind_category(other.category) && other.name != entry.name &&
              other.getter() == current_val) {
            conflict_count++;
          }
        }
        if (conflict_count > 0) {
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "(!)");
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Key '%s' is also bound to %d other action(s)", current_val.c_str(),
                              conflict_count);
          }
        }
      }

      // Skip the generic name + lifecycle badge rendering for keybinds
      if (mnk_disabled)
        ImGui::EndDisabled();
      if (read_only)
        ImGui::EndDisabled();
      ImGui::PopID();
      continue;
    } else {
      // Non-keybind CVARs: colored label on left, value widget on right
      ImGui::TextColored(LifecycleColor(entry.lifecycle), "%-20s", entry.name.c_str());
      if (ImGui::IsItemHovered()) {
        const char* lifecycle_label = "";
        switch (entry.lifecycle) {
          case rex::cvar::Lifecycle::kHotReload:
            lifecycle_label = "Live - changes apply immediately";
            break;
          case rex::cvar::Lifecycle::kRequiresRestart:
            lifecycle_label = "Requires restart to take effect";
            break;
          case rex::cvar::Lifecycle::kInitOnly:
            lifecycle_label = "Read-only - set at initialization only";
            break;
        }
        if (!entry.description.empty()) {
          ImGui::SetTooltip("%s\n[%s]", entry.description.c_str(), lifecycle_label);
        } else {
          ImGui::SetTooltip("[%s]", lifecycle_label);
        }
      }
      ImGui::SameLine(240.0f);

      ImGui::SetNextItemWidth(160.0f);
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
      } else if (entry.type == rex::cvar::FlagType::Command) {
        if (ImGui::Button(std::string(entry.name + "##v").c_str())) {
          entry.command_callback();
        }
      } else {
        char buf[256];
        std::strncpy(buf, current_val.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
          rex::cvar::SetFlagByName(entry.name, buf);
        }
      }
    }

    if (read_only)
      ImGui::EndDisabled();

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
