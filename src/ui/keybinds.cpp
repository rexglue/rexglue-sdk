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

using rex::hid::VirtualKey;

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
    hid::VirtualKey vk = hid::ParseVirtualKey(entry.var->get());
    if (vk != hid::VirtualKey::kNone && e.virtual_key() == vk) {
      entry.callback();
      e.set_handled(true);
      return true;
    }
  }
  return false;
}

}  // namespace rex::ui
