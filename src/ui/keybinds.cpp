/**
 * @file        ui/keybinds.cpp
 * @brief       Key binding implementation. See keybinds.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/keybinds.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <memory>
#include <mutex>
#include <string>
#include <deque>
#include <unordered_map>

namespace rex::ui {

using rex::ui::VirtualKey;

static const std::unordered_map<std::string, VirtualKey> kKeyNames = {
    // Function keys
    {"F1", VirtualKey::kF1},
    {"F2", VirtualKey::kF2},
    {"F3", VirtualKey::kF3},
    {"F4", VirtualKey::kF4},
    {"F5", VirtualKey::kF5},
    {"F6", VirtualKey::kF6},
    {"F7", VirtualKey::kF7},
    {"F8", VirtualKey::kF8},
    {"F9", VirtualKey::kF9},
    {"F10", VirtualKey::kF10},
    {"F11", VirtualKey::kF11},
    {"F12", VirtualKey::kF12},
    {"F13", VirtualKey::kF13},
    {"F14", VirtualKey::kF14},
    {"F15", VirtualKey::kF15},
    {"F16", VirtualKey::kF16},
    {"F17", VirtualKey::kF17},
    {"F18", VirtualKey::kF18},
    {"F19", VirtualKey::kF19},
    {"F20", VirtualKey::kF20},
    {"F21", VirtualKey::kF21},
    {"F22", VirtualKey::kF22},
    {"F23", VirtualKey::kF23},
    {"F24", VirtualKey::kF24},
    // Letters
    {"A", VirtualKey::kA},
    {"B", VirtualKey::kB},
    {"C", VirtualKey::kC},
    {"D", VirtualKey::kD},
    {"E", VirtualKey::kE},
    {"F", VirtualKey::kF},
    {"G", VirtualKey::kG},
    {"H", VirtualKey::kH},
    {"I", VirtualKey::kI},
    {"J", VirtualKey::kJ},
    {"K", VirtualKey::kK},
    {"L", VirtualKey::kL},
    {"M", VirtualKey::kM},
    {"N", VirtualKey::kN},
    {"O", VirtualKey::kO},
    {"P", VirtualKey::kP},
    {"Q", VirtualKey::kQ},
    {"R", VirtualKey::kR},
    {"S", VirtualKey::kS},
    {"T", VirtualKey::kT},
    {"U", VirtualKey::kU},
    {"V", VirtualKey::kV},
    {"W", VirtualKey::kW},
    {"X", VirtualKey::kX},
    {"Y", VirtualKey::kY},
    {"Z", VirtualKey::kZ},
    // Digits
    {"0", VirtualKey::k0},
    {"1", VirtualKey::k1},
    {"2", VirtualKey::k2},
    {"3", VirtualKey::k3},
    {"4", VirtualKey::k4},
    {"5", VirtualKey::k5},
    {"6", VirtualKey::k6},
    {"7", VirtualKey::k7},
    {"8", VirtualKey::k8},
    {"9", VirtualKey::k9},
    // OEM / special
    {"Backtick", VirtualKey::kOem3},
    {"Minus", VirtualKey::kOemMinus},
    {"Plus", VirtualKey::kOemPlus},
    {"Comma", VirtualKey::kOemComma},
    {"Period", VirtualKey::kOemPeriod},
    {"Semicolon", VirtualKey::kOem1},
    {"Slash", VirtualKey::kOem2},
    {"Backslash", VirtualKey::kOem5},
    {"LBracket", VirtualKey::kOem4},
    {"RBracket", VirtualKey::kOem6},
    {"Quote", VirtualKey::kOem7},
    // Control
    {"Escape", VirtualKey::kEscape},
    {"Return", VirtualKey::kReturn},
    {"Space", VirtualKey::kSpace},
    {"Tab", VirtualKey::kTab},
    {"Backspace", VirtualKey::kBack},
    {"Delete", VirtualKey::kDelete},
    {"Insert", VirtualKey::kInsert},
    {"Home", VirtualKey::kHome},
    {"End", VirtualKey::kEnd},
    {"PageUp", VirtualKey::kPrior},
    {"PageDown", VirtualKey::kNext},
    // Navigation
    {"Left", VirtualKey::kLeft},
    {"Right", VirtualKey::kRight},
    {"Up", VirtualKey::kUp},
    {"Down", VirtualKey::kDown},
    // Modifier
    {"Shift", VirtualKey::kShift},
    {"Control", VirtualKey::kControl},
    {"Alt", VirtualKey::kMenu},
    // Numpad
    {"Numpad0", VirtualKey::kNumpad0},
    {"Numpad1", VirtualKey::kNumpad1},
    {"Numpad2", VirtualKey::kNumpad2},
    {"Numpad3", VirtualKey::kNumpad3},
    {"Numpad4", VirtualKey::kNumpad4},
    {"Numpad5", VirtualKey::kNumpad5},
    {"Numpad6", VirtualKey::kNumpad6},
    {"Numpad7", VirtualKey::kNumpad7},
    {"Numpad8", VirtualKey::kNumpad8},
    {"Numpad9", VirtualKey::kNumpad9},
    {"NumpadEnter", VirtualKey::kReturn},
    {"NumpadPlus", VirtualKey::kAdd},
    {"NumpadMinus", VirtualKey::kSubtract},
    {"NumpadStar", VirtualKey::kMultiply},
    {"NumpadSlash", VirtualKey::kDivide},
    {"PrintScreen", VirtualKey::kSnapshot},
    {"Pause", VirtualKey::kPause},
    {"CapsLock", VirtualKey::kCapital},
    {"NumLock", VirtualKey::kNumLock},
    {"ScrollLock", VirtualKey::kScroll},
    // Mouse buttons
    {"LMB", VirtualKey::kLButton},
    {"RMB", VirtualKey::kRButton},
    {"MMB", VirtualKey::kMButton},
};

VirtualKey ParseVirtualKey(std::string_view name) {
  auto it = kKeyNames.find(std::string(name));
  return (it != kKeyNames.end()) ? it->second : VirtualKey::kNone;
}

std::string VirtualKeyToString(VirtualKey vk) {
  for (const auto& [name, key] : kKeyNames) {
    if (key == vk) {
      return name;
    }
  }
  return {};
}

struct BindEntry {
  std::string name;
  std::string cvar_name;
  std::string description;
  std::unique_ptr<rex::cvar::Var<std::string>> var;
  std::function<void()> callback;
  bool added = false;
};

static std::mutex g_binds_mutex;
static std::deque<BindEntry> g_binds;
static rex::cvar::ICvarRegistry* g_bind_registry = nullptr;

static std::string BindCvarName(std::string_view name) {
  std::string_view leaf = name.starts_with("bind_") ? name.substr(5) : name;
  return "bind." + std::string(leaf);
}

static void AddToRegistry(BindEntry& entry) {
  if (!g_bind_registry || entry.added || !entry.callback)
    return;
  const rex::cvar::IVar* one[] = {entry.var.get()};
  if (auto status = g_bind_registry->Add(one); !status) {
    REXLOG_WARN(ui, "{}", status.error().what());
    return;
  }
  entry.added = true;
}

static void RemoveFromRegistry(BindEntry& entry) {
  if (!g_bind_registry || !entry.added)
    return;
  const rex::cvar::IVar* one[] = {entry.var.get()};
  g_bind_registry->Unregister(one);
  entry.added = false;
}

void RegisterBind(std::string_view name, std::string_view default_key, std::string_view description,
                  std::function<void()> callback) {
  std::lock_guard lock(g_binds_mutex);
  auto& entry = g_binds.emplace_back();
  entry.name = std::string(name);
  entry.cvar_name = BindCvarName(name);
  entry.description = std::string(description);
  entry.callback = std::move(callback);
  entry.var = std::make_unique<rex::cvar::Var<std::string>>(entry.cvar_name, default_key,
                                                            entry.description);
  AddToRegistry(entry);
}

void UnregisterBind(std::string_view name) {
  std::lock_guard lock(g_binds_mutex);
  auto it = std::ranges::find(g_binds, name, &BindEntry::name);
  if (it == g_binds.end()) {
    return;
  }
  RemoveFromRegistry(*it);
  g_binds.erase(it);
}

void AttachBindRegistry(rex::cvar::ICvarRegistry* registry) {
  std::lock_guard lock(g_binds_mutex);
  if (g_bind_registry && g_bind_registry != registry) {
    for (auto& entry : g_binds)
      RemoveFromRegistry(entry);
  }
  g_bind_registry = registry;
  for (auto& entry : g_binds)
    AddToRegistry(entry);
}

bool ProcessKeyEvent(KeyEvent& e) {
  std::lock_guard lock(g_binds_mutex);
  for (auto& entry : g_binds) {
    if (!entry.callback)
      continue;
    VirtualKey vk = ParseVirtualKey(entry.var->get());
    if (vk != VirtualKey::kNone && e.virtual_key() == vk) {
      entry.callback();
      e.set_handled(true);
      return true;
    }
  }
  return false;
}

}  // namespace rex::ui
