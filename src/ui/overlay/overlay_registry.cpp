#include "ui/overlay/overlay_registry.h"

#include <algorithm>
#include <format>
#include <iterator>

#include "ui/overlay/bind_table.h"
#include "ui/imgui_host.h"
#include "ui/overlay/achievement_overlay.h"
#include "ui/overlay/console_overlay.h"
#include "ui/overlay/debug_overlay.h"
#include "ui/overlay/settings_overlay.h"
#include "ui/overlay/overlay_context.h"

namespace rex::ui {

namespace {

using StandardFactory = std::unique_ptr<IDialog> (*)(const ModuleOverlayContext& context);

struct StandardEntry {
  std::string_view name;
  StandardFactory create;
};

template <typename T>
std::unique_ptr<IDialog> BuildStandard(const ModuleOverlayContext& context) {
  return std::make_unique<T>(context);
}

constexpr StandardEntry kStandardFactories[] = {
    {"debug", &BuildStandard<DebugOverlayDialog>},
    {"console", &BuildStandard<ConsoleOverlayDialog>},
    {"settings", &BuildStandard<SettingsOverlayDialog>},
    {"achievements", &BuildStandard<AchievementsOverlayDialog>},
};

}  // namespace

OverlayRegistry::OverlayRegistry(ImGuiHost& drawer, BindTable& binds,
                                 const ModuleOverlayContext& context)
    : drawer_(drawer), binds_(binds), context_(context) {}

OverlayRegistry::~OverlayRegistry() {
  Clear();
}

void OverlayRegistry::Clear() {
  for (Entry& entry : entries_) {
    if (entry.bound) {
      binds_.Unregister(entry.name);
    }
    if (drawer_.Contains(*entry.overlay)) {
      drawer_.RemoveDialog(*entry.overlay);
    }
  }
  entries_.clear();
}

OverlayRegistry::Entry* OverlayRegistry::Find(std::string_view name) {
  auto it = std::ranges::find(entries_, name, &Entry::name);
  return it == entries_.end() ? nullptr : &*it;
}

Result<void> OverlayRegistry::Add(std::span<const OverlayConfig> table) {
  for (const OverlayConfig& config : table) {
    if (Find(config.name)) {
      return Err<void>(ErrorCategory::kConfig, std::format("overlay '{}' exists", config.name));
    }
    std::unique_ptr<IDialog> overlay;
    if (config.create) {
      overlay = config.create(context_.base.ui);
    } else if (auto it = std::ranges::find(kStandardFactories, config.name, &StandardEntry::name);
               it != std::ranges::end(kStandardFactories)) {
      overlay = it->create(context_);
    }
    if (!overlay) {
      return Err<void>(
          ErrorCategory::kConfig,
          std::format("overlay '{}' has no create function and is not a standard overlay",
                      config.name));
    }
    Entry entry;
    entry.name = std::string(config.name);
    entry.overlay = std::move(overlay);
    if (config.default_key.key != VirtualKey::kNone) {
      binds_.Register(entry.name, config.default_key,
                      std::format("Toggle the {} overlay", entry.name),
                      [this, name = entry.name] { Toggle(name); });
      entry.bound = true;
    }
    entries_.push_back(std::move(entry));
  }
  return Ok();
}

void OverlayRegistry::Show(std::string_view name, bool visible) {
  Entry* entry = Find(name);
  if (!entry || drawer_.Contains(*entry->overlay) == visible) {
    return;
  }
  if (visible) {
    drawer_.AddDialog(*entry->overlay);
    entry->overlay->OnShow();
  } else {
    drawer_.RemoveDialog(*entry->overlay);
  }
  if (on_change_) {
    on_change_();
  }
}

void OverlayRegistry::Toggle(std::string_view name) {
  if (Entry* entry = Find(name)) {
    const bool shown = drawer_.Contains(*entry->overlay);
    if (!shown && entry->overlay->OnToggle()) {
      return;
    }
    Show(name, !shown);
  }
}

IDialog* OverlayRegistry::Get(std::string_view name) {
  Entry* entry = Find(name);
  return entry ? entry->overlay.get() : nullptr;
}

bool OverlayRegistry::IsExclusiveVisible() const {
  return std::ranges::any_of(entries_, [this](const Entry& entry) {
    return drawer_.Contains(*entry.overlay) && entry.overlay->WantsExclusiveInput();
  });
}

bool OverlayRegistry::IsAnyVisible() const {
  return std::ranges::any_of(
      entries_, [this](const Entry& entry) { return drawer_.Contains(*entry.overlay); });
}

}  // namespace rex::ui
