/**
 * @file        rex/ui/keybinds.h
 * @brief       Key binding registry with cvar-backed rebindable keys.
 *
 * Provides a global bind registry where components (like overlay dialogs)
 * can register named keybinds with default keys and callbacks. Binds are
 * backed by string CVARs in the "Keybinds" category, so they appear in
 * the settings overlay and can be saved to config files.
 *
 * @section keybinds_usage Usage
 *
 * @code
 * // Register a bind (typically in a constructor):
 * rex::ui::RegisterBind("bind_console", "Backtick",
 *                       "Toggle console overlay", [this]{ ToggleVisible(); });
 *
 * // Dispatch key events (typically in OnKeyDown):
 * rex::ui::ProcessKeyEvent(e);
 *
 * // Unregister (typically in a destructor):
 * rex::ui::UnregisterBind("bind_console");
 * @endcode
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <functional>
#include <string>
#include <string_view>

namespace rex::ui {

/**
 * Parse a human-readable key name to a VirtualKey enum value.
 * @param name  Key name string (e.g. "F3", "Backtick", "A", "Escape").
 * @return      Matching VirtualKey, or VirtualKey::kNone if unrecognized.
 */
VirtualKey ParseVirtualKey(std::string_view name);

/**
 * Register a named keybind with a default key and callback.
 *
 * Creates a string CVAR named @p name in the "Keybinds" category so the
 * binding is visible in the settings overlay and persisted to config.
 *
 * @param name         CVAR name for this bind (e.g. "bind_console").
 * @param default_key  Default key name (e.g. "Backtick", "F3").
 * @param description  Human-readable description for the settings UI.
 * @param callback     Function to invoke when the bound key is pressed.
 */
void RegisterBind(std::string_view name, std::string_view default_key, std::string_view description,
                  std::function<void()> callback);

/**
 * Remove a previously registered keybind.
 * @param name  The CVAR name used when registering the bind.
 */
void UnregisterBind(std::string_view name);

/**
 * Process a key-down event against all registered binds.
 *
 * Looks up each bind's current key from its CVAR, parses it, and compares
 * against the event's virtual key. If a match is found, the bind's callback
 * is invoked and the event is marked as handled.
 *
 * @param e  The key event to process.
 * @return   True if a bind matched and the event was consumed.
 */
bool ProcessKeyEvent(KeyEvent& e);

}  // namespace rex::ui
