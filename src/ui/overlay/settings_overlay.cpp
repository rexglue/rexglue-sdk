#include "ui/overlay/settings_overlay.h"
#include "ui/overlay/overlay_context.h"
#include <rex/path_settings.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/console_command.h>
#include <rex/hid/input_keys.h>
#include <rex/hid/binding.h>
#include <rex/hid/input_system.h>
#include <rex/logging/log_category.h>
#include <rex/string/text.h>
#include <rex/string/numeric.h>
#include <imgui.h>

#include <rex/ui/style.h>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rex::ui {

SettingsOverlayDialog::SettingsOverlayDialog(const ModuleOverlayContext& context)
    : ui_(context.base.ui),
      settings_(context.base.settings),
      key_bindings_(context.base.key_bindings),
      input_(context.base.input),
      input_settings_(context.base.input_settings) {
  config_path_ = REX_PATHS().config_file.Get();
}

SettingsOverlayDialog::~SettingsOverlayDialog() {}

static void WarnIfFailed(std::string_view name, const Result<void>& status) {
  if (!status) {
    REXUI_WARN("cannot change '{}', {}", name, status.error().message);
  }
}

static Color LifecycleColor(rex::SettingLife life, const SettingsStyle& style) {
  switch (life) {
    case rex::SettingLife::kLive:
      return style.lifecycle_hot_reload;
    case rex::SettingLife::kRestart:
      return style.lifecycle_restart;
    case rex::SettingLife::kStartupOnly:
    case rex::SettingLife::kUntilLaunch:
      return style.lifecycle_init_only;
  }
  return style.lifecycle_unknown;
}

static std::string ClampIntegerText(const rex::SettingConstraints& constraints, i64 value) {
  if (constraints.min) {
    value = std::max(value, static_cast<i64>(*constraints.min));
  }
  if (constraints.max) {
    value = std::min(value, static_cast<i64>(*constraints.max));
  }
  return std::to_string(value);
}

static std::string ClampUnsignedText(const rex::SettingConstraints& constraints, u64 value) {
  if (constraints.min) {
    value = std::max(value, static_cast<u64>(*constraints.min));
  }
  if (constraints.max) {
    value = std::min(value, static_cast<u64>(*constraints.max));
  }
  return std::to_string(value);
}

static rex::hid::VirtualKey ImGuiKeyToVirtualKey(ImGuiKey key) {
  using VK = rex::hid::VirtualKey;
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
    return static_cast<VK>(static_cast<u16>(VK::kA) + (key - ImGuiKey_A));
  }
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
    return static_cast<VK>(static_cast<u16>(VK::k0) + (key - ImGuiKey_0));
  }
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) {
    return static_cast<VK>(static_cast<u16>(VK::kF1) + (key - ImGuiKey_F1));
  }
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) {
    return static_cast<VK>(static_cast<u16>(VK::kNumpad0) + (key - ImGuiKey_Keypad0));
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

static rex::hid::KeyModifiers HeldModifiers(const ImGuiIO& io) {
  rex::hid::KeyModifiers mods = rex::hid::KeyModifiers::kNone;
  if (io.KeyShift) {
    mods |= rex::hid::KeyModifiers::kShift;
  }
  if (io.KeyCtrl) {
    mods |= rex::hid::KeyModifiers::kCtrl;
  }
  if (io.KeyAlt) {
    mods |= rex::hid::KeyModifiers::kAlt;
  }
  return mods;
}

bool SettingsOverlayDialog::Draw(ImGuiIO& io) {
  std::vector<rex::SettingBase*> vars = settings_.Enumerate();
#ifdef NDEBUG
  std::erase_if(vars, [](const rex::SettingBase* var) { return var->GetConstraints().debug_only; });
#endif
  const std::vector<rex::ConsoleCommand*> commands = settings_.EnumerateCommands();

  std::set<std::string> category_set;
  for (const rex::SettingBase* var : vars) {
    category_set.insert(var->GetOwner().GetScope());
  }
  for (const rex::ConsoleCommand* command : commands) {
    const std::string& full = command->GetName();
    category_set.insert(full.substr(0, full.rfind('.')));
  }

  struct CatNode {
    std::string full_path;
    std::string label;
    std::map<std::string, CatNode> children;
    bool has_direct_entries = false;
  };
  std::map<std::string, CatNode> tree;

  for (auto& cat : category_set) {
    std::map<std::string, CatNode>* level = &tree;
    std::string path_so_far;
    size_t start = 0;
    while (start < cat.size()) {
      size_t slash = cat.find('.', start);
      std::string segment =
          (slash == std::string::npos) ? cat.substr(start) : cat.substr(start, slash - start);
      if (!path_so_far.empty())
        path_so_far += ".";
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
  const Metrics& metrics = ui_.GetStyle().metrics;
  ImGui::SetNextWindowBgAlpha(metrics.window_alpha.panel);
  if (!ImGui::Begin("Settings##rex", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return true;
  }

  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputText("##search", search_buf_, sizeof(search_buf_));
  ImGui::SameLine(0, 0);
  ImGui::Dummy(ImVec2(0, 0));

  ImGui::Separator();

  const f32 panel_width = metrics.side_panel_width;
  ImGui::BeginChild("##cats", ImVec2(panel_width, -metrics.footer_height), true);

  std::function<void(const std::map<std::string, CatNode>&, int)> draw_tree;
  draw_tree = [&](const std::map<std::string, CatNode>& nodes, int depth) {
    for (auto& [key, node] : nodes) {
      if (node.children.empty()) {
        bool selected = (selected_category_ == node.full_path);
        if (depth > 0)
          ImGui::Indent(8.0f);
        if (ImGui::Selectable(node.label.c_str(), selected)) {
          selected_category_ = node.full_path;
        }
        if (depth > 0)
          ImGui::Unindent(8.0f);
      } else {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow;
        if (node.has_direct_entries) {
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

  auto category_matches = [&](const std::string& cat) -> bool {
    if (selected_category_.empty())
      return true;
    if (cat == selected_category_)
      return true;
    if (cat.size() > selected_category_.size() &&
        cat.compare(0, selected_category_.size(), selected_category_) == 0 &&
        cat[selected_category_.size()] == '.') {
      return true;
    }
    return false;
  };

  auto is_keybind = [](const rex::SettingBase* v) -> bool { return v->GetConstraints().key_chord; };

  ImGui::BeginChild("##cvars", ImVec2(0, -metrics.footer_height), false);
  for (rex::SettingBase* var : vars) {
    const rex::SettingConstraints& constraints = var->GetConstraints();
    const std::string& name = var->GetName();
    const std::string& description = var->GetHelp();
    if (!searching) {
      if (!category_matches(var->GetOwner().GetScope())) {
        continue;
      }
    } else {
      const std::string name_lower = rex::string::utf8_lower_ascii(name);
      const std::string search_lower = rex::string::utf8_lower_ascii(search);
      if (name_lower.find(search_lower) == std::string::npos &&
          description.find(search_lower) == std::string::npos) {
        continue;
      }
    }

    bool read_only = var->IsFrozen();

    ImGui::PushID(name.c_str());

    if (read_only)
      ImGui::BeginDisabled();

    std::string current_val = var->GetText();

    if (is_keybind(var)) {
      const bool mnk_disabled = !input_settings_ || !input_settings_->mnk_mode.Get();
      if (mnk_disabled)
        ImGui::BeginDisabled();

      ImGui::Text("%-20s", description.c_str());
      ImGui::SameLine(240.0f);

      bool is_capturing = (capturing_bind_name_ == name);

      if (is_capturing) {
        ImGui::Button("Press any key...##v", ImVec2(metrics.button_width, 0));

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
          capturing_bind_name_.clear();
        } else {
          const rex::hid::KeyModifiers held = HeldModifiers(io);
          for (rex::i32 k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            auto imgui_key = static_cast<ImGuiKey>(k);
            if (imgui_key == ImGuiKey_Escape)
              continue;
            if (ImGui::IsKeyPressed(imgui_key)) {
              const auto vk = ImGuiKeyToVirtualKey(imgui_key);
              if (vk == rex::hid::VirtualKey::kShift || vk == rex::hid::VirtualKey::kControl ||
                  vk == rex::hid::VirtualKey::kMenu) {
                continue;
              }
              const std::string text = input_->FormatKeyChord({vk, held});
              if (!text.empty()) {
                WarnIfFailed(name, settings_.SetText(name, text));
              }
              capturing_bind_name_.clear();
              break;
            }
          }
          constexpr rex::hid::VirtualKey kMouseKeys[] = {
              rex::hid::VirtualKey::kLButton, rex::hid::VirtualKey::kRButton,
              rex::hid::VirtualKey::kMButton, rex::hid::VirtualKey::kXButton1,
              rex::hid::VirtualKey::kXButton2};
          for (rex::i32 mb = 0; mb < 5; ++mb) {
            if (ImGui::IsMouseClicked(mb)) {
              WarnIfFailed(name,
                           settings_.SetText(name, input_->FormatKeyChord({kMouseKeys[mb], held})));
              capturing_bind_name_.clear();
              break;
            }
          }
        }
      } else {
        ImGui::SetNextItemWidth(80.0f);
        ImGui::Text("%-10s", current_val.c_str());
        ImGui::SameLine();
        if (input_ && ImGui::SmallButton("Rebind##v")) {
          capturing_bind_name_ = name;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##v")) {
          WarnIfFailed(name, settings_.SetText(name, var->GetDefaultText()));
        }
      }

      if (!current_val.empty()) {
        std::set<size_t> conflicts;
        if (const auto binding =
                key_bindings_ ? key_bindings_->FindBySetting(name) : std::nullopt) {
          for (const rex::hid::InputSource& source : key_bindings_->GetSources(*binding)) {
            if (const auto other = key_bindings_->FindConflict(*binding, source)) {
              conflicts.insert(*other);
            }
          }
        }
        const int conflict_count = static_cast<int>(conflicts.size());
        if (conflict_count > 0) {
          ImGui::SameLine();
          ImGui::TextColored(ToImVec4(ui_.GetStyle().settings.warning), "(!)");
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Key '%s' is also bound to %d other action(s)", current_val.c_str(),
                              conflict_count);
          }
        }
      }

      if (mnk_disabled)
        ImGui::EndDisabled();
      if (read_only)
        ImGui::EndDisabled();
      ImGui::PopID();
      continue;
    } else {
      ImGui::TextColored(ToImVec4(LifecycleColor(constraints.life, ui_.GetStyle().settings)),
                         "%-20s", name.c_str());
      if (ImGui::IsItemHovered()) {
        const char* lifecycle_label = "";
        switch (constraints.life) {
          case rex::SettingLife::kLive:
            lifecycle_label = "Changes apply immediately";
            break;
          case rex::SettingLife::kRestart:
            lifecycle_label = "Requires restart to take effect";
            break;
          case rex::SettingLife::kStartupOnly:
            lifecycle_label = "Read only after startup";
            break;
          case rex::SettingLife::kUntilLaunch:
            lifecycle_label = "Read only after the title launches";
            break;
        }
        if (!description.empty()) {
          ImGui::SetTooltip("%s\n[%s]", description.c_str(), lifecycle_label);
        } else {
          ImGui::SetTooltip("[%s]", lifecycle_label);
        }
      }
      ImGui::SameLine(240.0f);

      ImGui::SetNextItemWidth(metrics.value_width);
      if (var->GetType() == rex::SettingType::kBool) {
        bool v = rex::string::from_string<bool>(current_val, false);
        if (ImGui::Checkbox("##v", &v)) {
          WarnIfFailed(name, settings_.SetText(name, v ? "true" : "false"));
        }
      } else if (var->GetType() == rex::SettingType::kString && !constraints.allowed.empty()) {
        const auto& opts = constraints.allowed;
        int cur_idx = 0;
        for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
          if (opts[i] == current_val) {
            cur_idx = i;
            break;
          }
        }
        const std::string cur_label(opts[cur_idx]);
        if (ImGui::BeginCombo("##v", cur_label.c_str())) {
          for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
            bool sel = (i == cur_idx);
            const std::string opt(opts[i]);
            if (ImGui::Selectable(opt.c_str(), sel)) {
              WarnIfFailed(name, settings_.SetText(name, opt));
            }
            if (sel)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
      } else if (var->GetType() == rex::SettingType::kI32 ||
                 var->GetType() == rex::SettingType::kI64 ||
                 var->GetType() == rex::SettingType::kU32 ||
                 var->GetType() == rex::SettingType::kU64) {
        if (var->GetType() == rex::SettingType::kU32 || var->GetType() == rex::SettingType::kU64) {
          u64 v = rex::string::from_string<u64>(current_val);
          if (ImGui::InputScalar("##v", ImGuiDataType_U64, &v)) {
            WarnIfFailed(name, settings_.SetText(name, ClampUnsignedText(constraints, v)));
          }
        } else {
          i64 v = rex::string::from_string<i64>(current_val);
          if (ImGui::InputScalar("##v", ImGuiDataType_S64, &v)) {
            WarnIfFailed(name, settings_.SetText(name, ClampIntegerText(constraints, v)));
          }
        }
      } else if (var->GetType() == rex::SettingType::kF64) {
        f64 v = rex::string::from_string<f64>(current_val);
        if (ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.4f")) {
          if (constraints.min)
            v = std::max(v, *constraints.min);
          if (constraints.max)
            v = std::min(v, *constraints.max);
          WarnIfFailed(name, settings_.SetText(name, std::to_string(v)));
        }
      } else {
        char buf[256];
        rex::string::CopyTruncating(buf, current_val, sizeof(buf));
        if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
          WarnIfFailed(name, settings_.SetText(name, buf));
        }
      }
    }

    if (read_only)
      ImGui::EndDisabled();

    ImGui::PopID();
  }
  for (rex::ConsoleCommand* command : commands) {
    const std::string& name = command->GetName();
    const std::string scope = name.substr(0, name.rfind('.'));
    if (!searching ? !category_matches(scope)
                   : rex::string::utf8_lower_ascii(name).find(
                         rex::string::utf8_lower_ascii(search)) == std::string::npos) {
      continue;
    }
    ImGui::PushID(name.c_str());
    if (ImGui::Button(name.c_str())) {
      WarnIfFailed(name, command->Run(""));
    }
    if (ImGui::IsItemHovered() && !command->GetHelp().empty()) {
      ImGui::SetTooltip("%s", command->GetHelp().c_str());
    }
    ImGui::PopID();
  }
  ImGui::EndChild();

  ImGui::Separator();
  if (ImGui::Button("Save to config")) {
    if (auto status = settings_.Save(config_path_); !status) {
      REXUI_ERROR("{}", status.error().GetDescription());
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", config_path_.filename().string().c_str());

  ImGui::End();
  return true;
}

}  // namespace rex::ui
