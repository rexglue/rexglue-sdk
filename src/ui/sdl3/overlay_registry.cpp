#include "ui/sdl3/overlay_registry.h"

#include <algorithm>
#include <format>
#include <iterator>

#include "ui/sdl3/bind_table.h"
#include "ui/sdl3/imgui_drawer.h"
#include "ui/sdl3/overlay/achievements_overlay.h"
#include "ui/sdl3/overlay/console_overlay.h"
#include "ui/sdl3/overlay/debug_overlay.h"
#include "ui/sdl3/overlay/settings_overlay.h"
#include "ui/sdl3/overlay_context.h"

namespace rex::ui {

namespace {

using StandardFactory = std::unique_ptr<ImGuiDialog> (*)(const ModuleOverlayContext& context);

struct StandardEntry {
  std::string_view name;
  StandardFactory create;
};

template <typename T>
std::unique_ptr<ImGuiDialog> BuildStandard(const ModuleOverlayContext& context) {
  return std::make_unique<T>(context);
}

constexpr StandardEntry kStandardFactories[] = {
    {"debug", &BuildStandard<DebugOverlayDialog>},
    {"console", &BuildStandard<ConsoleDialog>},
    {"settings", &BuildStandard<SettingsDialog>},
    {"achievements", &BuildStandard<AchievementsOverlayDialog>},
};

}  // namespace

OverlayRegistry::OverlayRegistry(ImGuiDrawer& drawer, BindTable& binds,
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
    if (entry.overlay->IsAttached()) {
      drawer_.RemoveDialog(*entry.overlay);
    }
  }
  entries_.clear();
}

OverlayRegistry::Entry* OverlayRegistry::Find(std::string_view name) {
  auto it = std::ranges::find(entries_, name, &Entry::name);
  return it == entries_.end() ? nullptr : &*it;
}

Result<void> OverlayRegistry::Add(std::span<const OverlayDesc> table) {
  for (const OverlayDesc& desc : table) {
    if (Find(desc.name)) {
      return Err<void>(ErrorCategory::kConfig, std::format("overlay '{}' exists", desc.name));
    }
    std::unique_ptr<ImGuiDialog> overlay;
    if (desc.create) {
      overlay = desc.create(context_.base);
    } else if (auto it = std::ranges::find(kStandardFactories, desc.name, &StandardEntry::name);
               it != std::ranges::end(kStandardFactories)) {
      overlay = it->create(context_);
    }
    if (!overlay) {
      return Err<void>(
          ErrorCategory::kConfig,
          std::format("overlay '{}' has no create function and is not a standard overlay",
                      desc.name));
    }
    Entry entry;
    entry.name = std::string(desc.name);
    entry.overlay = std::move(overlay);
    if (desc.default_key.key != hid::VirtualKey::kNone) {
      binds_.Register(entry.name, desc.default_key,
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
  if (!entry || entry->overlay->IsAttached() == visible) {
    return;
  }
  if (visible) {
    drawer_.AddDialog(*entry->overlay);
    entry->overlay->OnShow();
  } else {
    drawer_.RemoveDialog(*entry->overlay);
  }
}

void OverlayRegistry::Toggle(std::string_view name) {
  if (Entry* entry = Find(name)) {
    const bool shown = entry->overlay->IsAttached();
    if (!shown && entry->overlay->OnToggle()) {
      return;
    }
    Show(name, !shown);
  }
}

ImGuiDialog* OverlayRegistry::Get(std::string_view name) {
  Entry* entry = Find(name);
  return entry ? entry->overlay.get() : nullptr;
}

bool OverlayRegistry::IsAnyVisible() const {
  return std::ranges::any_of(entries_,
                             [](const Entry& entry) { return entry.overlay->IsAttached(); });
}

}  // namespace rex::ui
