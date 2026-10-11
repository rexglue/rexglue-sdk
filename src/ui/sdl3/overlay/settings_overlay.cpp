#include "ui/sdl3/overlay/settings_overlay.h"
#include "ui/sdl3/overlay_context.h"
#include <rex/api.h>
#include <rex/cvar.h>
#include <rex/hid/key_chord.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/string/numeric.h>
#include <imgui.h>
#include <rex/ui/color.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace rex::ui {

SettingsDialog::SettingsDialog(const ModuleOverlayContext& context)
    : ui_(context.base.ui),
      registry_(context.base.cvars),
      config_path_(context.base.config.paths.config_file) {}

SettingsDialog::~SettingsDialog() {}

template <typename T, typename Wide>
static std::string ClampIntegerText(const rex::cvar::Desc& desc, std::string_view text) {
  Wide value = std::clamp<Wide>(rex::from_string<Wide>(text), std::numeric_limits<T>::min(),
                                std::numeric_limits<T>::max());
  if (desc.constraints.min && static_cast<f64>(value) < *desc.constraints.min) {
    value = static_cast<Wide>(*desc.constraints.min);
  }
  if (desc.constraints.max && static_cast<f64>(value) > *desc.constraints.max) {
    value = static_cast<Wide>(*desc.constraints.max);
  }
  return std::to_string(value);
}

std::string ClampIntegerCvarText(const rex::cvar::Desc& desc, std::string_view text) {
  switch (desc.type) {
    case rex::cvar::Type::Int32:
      return ClampIntegerText<i32, i64>(desc, text);
    case rex::cvar::Type::UInt32:
      return ClampIntegerText<u32, u64>(desc, text);
    case rex::cvar::Type::UInt64:
      return ClampIntegerText<u64, u64>(desc, text);
    default:
      return ClampIntegerText<i64, i64>(desc, text);
  }
}

static void WarnIfFailed(std::string_view name, const Result<void>& status) {
  if (!status) {
    REXLOG_WARN(ui, "cannot change '{}', {}", name, status.error().message);
  }
}

static Color LifecycleColor(rex::cvar::Lifecycle lc, const SettingsStyle& style) {
  switch (lc) {
    case rex::cvar::Lifecycle::HotReload:
      return style.lifecycle_hot_reload;
    case rex::cvar::Lifecycle::RequiresRestart:
      return style.lifecycle_restart;
    case rex::cvar::Lifecycle::InitOnly:
      return style.lifecycle_init_only;
  }
  return style.lifecycle_unknown;
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
  rex::hid::KeyModifiers mods = rex::hid::KeyModifiers::None;
  if (io.KeyShift) {
    mods |= rex::hid::KeyModifiers::Shift;
  }
  if (io.KeyCtrl) {
    mods |= rex::hid::KeyModifiers::Ctrl;
  }
  if (io.KeyAlt) {
    mods |= rex::hid::KeyModifiers::Alt;
  }
  return mods;
}

void SettingsDialog::OnDraw(ImGuiIO& io) {
  const std::vector<const rex::cvar::IVar*> vars = registry_.Enumerate();

  std::set<std::string> category_set;
  for (const rex::cvar::IVar* var : vars) {
    category_set.insert(std::string(var->desc().scope));
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
  const Metrics& metrics = ui_.GetStyle().metrics;
  ImGui::SetNextWindowBgAlpha(metrics.window_alpha.panel);
  if (!ImGui::Begin("Settings##rex", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
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
        cat[selected_category_.size()] == '/') {
      return true;
    }
    return false;
  };

  auto is_keybind = [](const rex::cvar::IVar* v) -> bool {
    return v->desc().name.starts_with("hid.keybind_");
  };

  ImGui::BeginChild("##cvars", ImVec2(0, -metrics.footer_height), false);
  for (const rex::cvar::IVar* var : vars) {
    const auto& desc = var->desc();
    const std::string name(desc.name);
    const std::string description(desc.description);
    if (!searching) {
      if (!category_matches(std::string(desc.scope))) {
        continue;
      }
    } else {
      std::string name_lower = name;
      std::string search_lower = search;
      auto to_lower = [](std::string& s) {
        for (auto& c : s)
          c = static_cast<char>(std::tolower(c));
      };
      to_lower(name_lower);
      to_lower(search_lower);
      if (name_lower.find(search_lower) == std::string::npos &&
          description.find(search_lower) == std::string::npos) {
        continue;
      }
    }

    bool read_only = (desc.lifecycle == rex::cvar::Lifecycle::InitOnly);

    ImGui::PushID(name.c_str());

    if (read_only)
      ImGui::BeginDisabled();

    std::string current_val = var->Render();

    if (is_keybind(var)) {
      bool mnk_disabled = registry_.Get("hid.mnk_mode").value_or("false") != "true";
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
              const std::string text = rex::hid::KeyChordToString({vk, held});
              if (!text.empty()) {
                WarnIfFailed(desc.name, registry_.Set(desc.name, text));
              }
              capturing_bind_name_.clear();
              break;
            }
          }
          constexpr rex::hid::VirtualKey kMouseKeys[] = {rex::hid::VirtualKey::kLButton,
                                                         rex::hid::VirtualKey::kRButton,
                                                         rex::hid::VirtualKey::kMButton};
          for (rex::i32 mb = 0; mb < 3; ++mb) {
            if (ImGui::IsMouseClicked(mb)) {
              WarnIfFailed(
                  desc.name,
                  registry_.Set(desc.name, rex::hid::KeyChordToString({kMouseKeys[mb], held})));
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
          capturing_bind_name_ = name;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##v")) {
          WarnIfFailed(desc.name, registry_.Set(desc.name, var->DefaultText()));
        }
      }

      if (!current_val.empty()) {
        int conflict_count = 0;
        for (const rex::cvar::IVar* other : vars) {
          if (is_keybind(other) && other->desc().name != desc.name &&
              other->Render() == current_val) {
            conflict_count++;
          }
        }
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
      ImGui::TextColored(ToImVec4(LifecycleColor(desc.lifecycle, ui_.GetStyle().settings)), "%-20s",
                         name.c_str());
      if (ImGui::IsItemHovered()) {
        const char* lifecycle_label = "";
        switch (desc.lifecycle) {
          case rex::cvar::Lifecycle::HotReload:
            lifecycle_label = "Changes apply immediately";
            break;
          case rex::cvar::Lifecycle::RequiresRestart:
            lifecycle_label = "Requires restart to take effect";
            break;
          case rex::cvar::Lifecycle::InitOnly:
            lifecycle_label = "Read-only - set at initialization only";
            break;
        }
        if (!description.empty()) {
          ImGui::SetTooltip("%s\n[%s]", description.c_str(), lifecycle_label);
        } else {
          ImGui::SetTooltip("[%s]", lifecycle_label);
        }
      }
      ImGui::SameLine(240.0f);

      ImGui::SetNextItemWidth(metrics.button_width_wide);
      if (desc.type == rex::cvar::Type::Bool) {
        bool v = rex::from_string<bool>(current_val, false);
        if (ImGui::Checkbox("##v", &v)) {
          WarnIfFailed(desc.name, registry_.Set(desc.name, v ? "true" : "false"));
        }
      } else if (desc.type == rex::cvar::Type::String && !desc.constraints.allowed.empty()) {
        const auto& opts = desc.constraints.allowed;
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
              WarnIfFailed(desc.name, registry_.Set(desc.name, opt));
            }
            if (sel)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
      } else if (desc.type == rex::cvar::Type::Int32 || desc.type == rex::cvar::Type::Int64 ||
                 desc.type == rex::cvar::Type::UInt32 || desc.type == rex::cvar::Type::UInt64) {
        if (desc.type == rex::cvar::Type::UInt32 || desc.type == rex::cvar::Type::UInt64) {
          u64 v = rex::from_string<u64>(current_val);
          if (ImGui::InputScalar("##v", ImGuiDataType_U64, &v)) {
            WarnIfFailed(desc.name,
                         registry_.Set(desc.name, ClampIntegerCvarText(desc, std::to_string(v))));
          }
        } else {
          i64 v = rex::from_string<i64>(current_val);
          if (ImGui::InputScalar("##v", ImGuiDataType_S64, &v)) {
            WarnIfFailed(desc.name,
                         registry_.Set(desc.name, ClampIntegerCvarText(desc, std::to_string(v))));
          }
        }
      } else if (desc.type == rex::cvar::Type::Double) {
        f64 v = rex::from_string<f64>(current_val);
        if (ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.4f")) {
          if (desc.constraints.min)
            v = std::max(v, *desc.constraints.min);
          if (desc.constraints.max)
            v = std::min(v, *desc.constraints.max);
          WarnIfFailed(desc.name, registry_.Set(desc.name, std::to_string(v)));
        }
      } else if (desc.type == rex::cvar::Type::Command) {
        if (ImGui::Button((name + "##v").c_str())) {
          WarnIfFailed(desc.name, registry_.Invoke(desc.name, ""));
        }
      } else {
        char buf[256];
        rex::copy_truncating(buf, current_val, sizeof(buf));
        if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
          WarnIfFailed(desc.name, registry_.Set(desc.name, buf));
        }
      }
    }

    if (read_only)
      ImGui::EndDisabled();

    ImGui::PopID();
  }
  ImGui::EndChild();

  ImGui::Separator();
  if (ImGui::Button("Save to config")) {
    if (auto status = rex::SaveConfigFile(registry_, config_path_); !status) {
      REXLOG_ERROR(ui, "{}", status.error().what());
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", config_path_.filename().string().c_str());

  ImGui::End();
}

}  // namespace rex::ui
