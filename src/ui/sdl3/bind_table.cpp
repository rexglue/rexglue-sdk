#include "ui/sdl3/bind_table.h"

#include <algorithm>

#include <rex/cvar.h>
#include <rex/hid/event.h>
#include <rex/logging/log.h>
#include <rex/move_only_function.h>

namespace rex::ui {

namespace {

std::string BindCvarName(std::string_view name) {
  const std::string_view leaf = name.starts_with("bind_") ? name.substr(5) : name;
  return "bind." + std::string(leaf);
}

hid::KeyModifiers ModifiersOf(const KeyEvent& e) {
  hid::KeyModifiers mods = hid::KeyModifiers::kNone;
  if (e.IsShiftPressed()) {
    mods |= hid::KeyModifiers::kShift;
  }
  if (e.IsCtrlPressed()) {
    mods |= hid::KeyModifiers::kCtrl;
  }
  if (e.IsAltPressed()) {
    mods |= hid::KeyModifiers::kAlt;
  }
  return mods;
}

}  // namespace

BindTable::~BindTable() {
  std::ranges::for_each(entries_, [this](Entry& entry) { RemoveFromRegistry(entry); });
  std::lock_guard lock(mutex_);
  registry_ = nullptr;
}

void BindTable::Attach(cvar::ICvarRegistry* registry) {
  cvar::ICvarRegistry* previous = nullptr;
  {
    std::lock_guard lock(mutex_);
    previous = registry_;
  }
  if (previous && previous != registry) {
    std::ranges::for_each(entries_, [this](Entry& entry) { RemoveFromRegistry(entry); });
  }
  {
    std::lock_guard lock(mutex_);
    registry_ = registry;
  }
  if (registry) {
    std::ranges::for_each(entries_, [this](Entry& entry) { AddToRegistry(entry); });
  }
}

void BindTable::AddToRegistry(Entry& entry) {
  cvar::ICvarRegistry* registry = nullptr;
  {
    std::lock_guard lock(mutex_);
    registry = registry_;
  }
  if (!registry || entry.added || !entry.callback) {
    return;
  }
  const cvar::VarBase* one[] = {entry.var.get()};
  if (auto added = registry->Add(one); !added) {
    REXLOG_WARN(ui, "{}", added.error().message);
    return;
  }
  entry.added = true;
  auto handle = registry->OnChange(
      entry.cvar_name, [this, name = entry.name](std::string_view, std::string_view value) {
        OnChanged(name, value);
      });
  if (!handle) {
    REXLOG_WARN(ui, "{} is not followed, {}", entry.cvar_name, handle.error().message);
  } else {
    entry.handle = std::move(*handle);
  }
  OnChanged(entry.name, entry.var->Get());
}

void BindTable::RemoveFromRegistry(Entry& entry) {
  entry.handle.Reset();
  if (!entry.added) {
    return;
  }
  cvar::ICvarRegistry* registry = nullptr;
  {
    std::lock_guard lock(mutex_);
    registry = registry_;
  }
  if (registry) {
    const cvar::VarBase* one[] = {entry.var.get()};
    registry->Unregister(one);
  }
  entry.added = false;
}

void BindTable::Register(std::string_view name, hid::KeyChord default_key,
                         std::string_view description, rex::MoveOnlyFunction<void()> callback) {
  Entry* entry = nullptr;
  {
    std::lock_guard lock(mutex_);
    entry = &entries_.emplace_back();
    entry->name = std::string(name);
    entry->cvar_name = BindCvarName(name);
    entry->description = std::string(description);
    entry->callback = std::make_shared<rex::MoveOnlyFunction<void()>>(std::move(callback));
    entry->chord = default_key;
    entry->var = std::make_unique<cvar::Var<std::string>>(
        entry->cvar_name, hid::KeyChordToString(default_key), entry->description);
  }
  AddToRegistry(*entry);
}

void BindTable::Unregister(std::string_view name) {
  Entry* entry = nullptr;
  {
    std::lock_guard lock(mutex_);
    auto it = std::ranges::find(entries_, name, &Entry::name);
    if (it == entries_.end()) {
      return;
    }
    entry = &*it;
  }
  RemoveFromRegistry(*entry);
  std::lock_guard lock(mutex_);
  entries_.erase(std::ranges::find(entries_, name, &Entry::name));
}

bool BindTable::Process(const KeyEvent& e) {
  const hid::KeyModifiers mods = ModifiersOf(e);
  std::shared_ptr<rex::MoveOnlyFunction<void()>> callback;
  {
    std::lock_guard lock(mutex_);
    for (const Entry& entry : entries_) {
      if (entry.callback && entry.chord.key != hid::VirtualKey::kNone &&
          entry.chord.key == e.GetVirtualKey() && entry.chord.mods == mods) {
        callback = entry.callback;
        break;
      }
    }
  }
  if (!callback) {
    return false;
  }
  (*callback)();
  return true;
}

void BindTable::OnChanged(std::string_view name, std::string_view value) {
  auto chord = hid::ParseKeyChord(value);
  if (!chord) {
    REXLOG_WARN(ui, "bind {} keeps its chord, {}", name, chord.error().message);
    return;
  }
  std::lock_guard lock(mutex_);
  if (auto it = std::ranges::find(entries_, name, &Entry::name); it != entries_.end()) {
    it->chord = *chord;
  }
}

}  // namespace rex::ui
