/**
 * @file        ui/overlay.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <span>
#include <string_view>

#include <rex/types.h>
#include <rex/platform.h>
#include <rex/hid/key_chord.h>

namespace rex {
struct Config;
}  // namespace rex

namespace rex::cvar {
class ICvarRegistry;
}  // namespace rex::cvar

namespace rex::ui {

class ImGuiDialog;
class IUserInterface;

/** What an overlay may use. Valid for the overlay's whole life. */
struct OverlayContext {
  /** The user interface that shows the overlay. */
  IUserInterface& ui;
  /** Cvar registry that holds every module's cvars. */
  cvar::ICvarRegistry& cvars;
  /** The app's configuration. */
  const Config& config;
};

/** 1 overlay table entry. REX_OVERLAY builds it. */
struct OverlayDesc {
  /** Unique overlay name, also the keybind name bind.<name>. */
  std::string_view name;
  /** Default chord of the toggle bind. A chord whose key is VirtualKey::kNone registers no bind. */
  hid::KeyChord default_key;
  /** Constructs the overlay, or null for a standard overlay the ui module builds by name. */
  std::unique_ptr<ImGuiDialog> (*create)(const OverlayContext& context);
};

/** The debug, console, settings and achievements entries, with a null create. */
inline constexpr OverlayDesc kStandardOverlayEntries[] = {
    {"debug", {hid::VirtualKey::kF3}, nullptr},
    {"console", {hid::VirtualKey::kOem3}, nullptr},
    {"settings", {hid::VirtualKey::kF4}, nullptr},
    {"achievements", {hid::VirtualKey::kF7}, nullptr},
};

/**
 * The debug, console, settings and achievements overlays, with the default keys F3, the
 * backtick key, F4 and F7.
 */
inline constexpr std::span<const OverlayDesc> kStandardOverlays{kStandardOverlayEntries};

}  // namespace rex::ui

/**
 * Builds an overlay table entry. Class must be constructible from
 * const rex::ui::OverlayContext&.
 *
 * @param Class Overlay class derived from rex::ui::ImGuiDialog.
 * @param name Unique overlay name as a string literal.
 * @param default_key Default chord of the toggle bind as a hid::KeyChord expression, or a chord
 * with key VirtualKey::kNone for none. A chord with modifiers holds a comma inside its braces, so
 *        pass it as a named constant.
 *
 * @code
 * REX_OVERLAY(FooMapOverlay, "map", ::rex::hid::KeyChord{::rex::hid::VirtualKey::kM})
 * @endcode
 */
#define REX_OVERLAY(Class, name, default_key)                                                     \
  ::rex::ui::OverlayDesc {                                                                        \
    name, default_key,                                                                            \
        [](const ::rex::ui::OverlayContext& context) -> std::unique_ptr<::rex::ui::ImGuiDialog> { \
          return std::make_unique<Class>(context);                                                \
        }                                                                                         \
  }

/**
 * Declares a constant overlay table.
 *
 * @param table Name of the table variable.
 * @param ... REX_OVERLAY entries.
 *
 * @code
 * REX_OVERLAY_TABLE(kFooOverlays,
 *                   REX_OVERLAY(FooMapOverlay, "map",
 * ::rex::hid::KeyChord{::rex::hid::VirtualKey::kM}), REX_OVERLAY(FooStatsOverlay, "stats",
 * ::rex::hid::KeyChord{::rex::hid::VirtualKey::kF9}));
 * @endcode
 */
#define REX_OVERLAY_TABLE(table, ...)                                        \
  inline constexpr ::rex::ui::OverlayDesc table##_entries[] = {__VA_ARGS__}; \
  inline constexpr std::span<const ::rex::ui::OverlayDesc> table {           \
    table##_entries                                                          \
  }
