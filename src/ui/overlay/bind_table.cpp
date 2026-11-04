#include "ui/overlay/bind_table.h"

#include <algorithm>

#include <rex/hid/input_keys.h>
#include <rex/logging/log_category.h>
#include <rex/utility.h>

namespace rex::ui {

namespace {

std::string BindLeaf(std::string_view name) {
  return std::string(name.starts_with("bind_") ? name.substr(5) : name);
}

}  // namespace

BindTable::~BindTable() {
  std::ranges::for_each(entries_, [this](Entry& entry) { RemoveFromRegistry(entry); });
  std::lock_guard lock(mutex_);
  settings_ = nullptr;
}

void BindTable::Attach(ISettingsRegistry* settings) {
  ISettingsRegistry* previous = nullptr;
  {
    std::lock_guard lock(mutex_);
    previous = settings_;
  }
  if (previous && previous != settings) {
    std::ranges::for_each(entries_, [this](Entry& entry) { RemoveFromRegistry(entry); });
  }
  {
    std::lock_guard lock(mutex_);
    settings_ = settings;
  }
  if (settings) {
    std::ranges::for_each(entries_, [this](Entry& entry) { AddToRegistry(entry); });
  }
}

void BindTable::SetInput(const hid::IInputSystem* input) {
  std::lock_guard lock(mutex_);
  input_ = input;
}

void BindTable::AddToRegistry(Entry& entry) {
  ISettingsRegistry* settings = nullptr;
  {
    std::lock_guard lock(mutex_);
    settings = settings_;
  }
  if (!settings || entry.added || !entry.callback) {
    return;
  }
  if (auto added = settings->Register(*entry.owner); !added) {
    REXUI_WARN("{}", added.error().message);
    return;
  }
  entry.added = true;
  entry.watch = entry.owner->chord.OnChange(
      [this, name = entry.name](const std::string& value) { OnChanged(name, value); });
  OnChanged(entry.name, entry.owner->chord.Get());
}

void BindTable::RemoveFromRegistry(Entry& entry) {
  entry.watch.Reset();
  if (!entry.added) {
    return;
  }
  ISettingsRegistry* settings = nullptr;
  {
    std::lock_guard lock(mutex_);
    settings = settings_;
  }
  if (settings) {
    settings->Unregister(*entry.owner);
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
    entry->setting_name = BindLeaf(name);
    entry->description = std::string(description);
    entry->callback = std::make_shared<rex::MoveOnlyFunction<void()>>(std::move(callback));
    entry->chord = default_key;
    entry->owner = std::make_unique<BindOwner>(
        entry->setting_name, input_ ? input_->FormatKeyChord(default_key) : std::string(),
        entry->description);
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

bool BindTable::Process(const hid::InputEvent& e) {
  std::shared_ptr<rex::MoveOnlyFunction<void()>> callback;
  {
    std::lock_guard lock(mutex_);
    for (const Entry& entry : entries_) {
      if (entry.callback && entry.chord.key != hid::VirtualKey::kNone && entry.chord.key == e.key &&
          entry.chord.modifiers == e.modifiers) {
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
  const hid::IInputSystem* input = nullptr;
  {
    std::lock_guard lock(mutex_);
    input = input_;
  }
  if (!input) {
    return;
  }
  auto chord = input->ParseKeyChord(value);
  if (!chord) {
    REXUI_WARN("bind {} keeps its chord, {}", name, chord.error().message);
    return;
  }
  std::lock_guard lock(mutex_);
  if (auto it = std::ranges::find(entries_, name, &Entry::name); it != entries_.end()) {
    it->chord = *chord;
  }
}

}  // namespace rex::ui
