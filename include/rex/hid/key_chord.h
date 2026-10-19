/**
 * @file        hid/key_chord.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rex/types.h>
#include <rex/platform.h>
#include <rex/hid/event.h>
#include <rex/hid/virtual_key.h>
#include <rex/export.h>
#include <rex/result.h>

namespace rex::hid {

/** A key and the exact modifiers that must be held with it. */
struct KeyChord {
  /** The key, VirtualKey::kNone for an unbound chord. */
  VirtualKey key = VirtualKey::kNone;
  /** The modifiers held with the key, no more and no fewer. */
  KeyModifiers modifiers = KeyModifiers::kNone;

  /**
   * Compares 2 chords.
   *
   * @param other The chord to compare with.
   * @return True when key and mods are equal.
   */
  constexpr bool operator==(const KeyChord& other) const = default;
};

/**
 * Parses a chord such as "F3", "Shift+Up" or "Ctrl+Alt+K". The prefixes Shift, Ctrl, Control and
 * Alt come in any order, then exactly 1 key name that ParseVirtualKey accepts. Names are case
 * sensitive. An empty string is the unbound chord.
 *
 * @param text The chord text.
 * @return The chord, or ErrorCategory::kConfig naming text when the key is unknown, missing or a
 *         bare modifier.
 */
[[nodiscard]] REX_API Result<KeyChord> ParseKeyChord(std::string_view text);

/**
 * Parses a comma separated list of chords such as "Q,I" or "Shift+W, A". Spaces and tabs around
 * an item are dropped and empty items are skipped.
 *
 * @param text The list text.
 * @return The chords in order, or the error of the first item ParseKeyChord rejects.
 */
[[nodiscard]] REX_API Result<std::vector<KeyChord>> ParseKeyChordList(std::string_view text);

/**
 * Writes a chord as text, modifiers in the order Shift, Ctrl, Alt and then the key name.
 *
 * @param chord The chord.
 * @return The text ParseKeyChord reads back, or empty when the key is VirtualKey::kNone or has
 *         no name.
 */
REX_API std::string KeyChordToString(KeyChord chord);

}  // namespace rex::hid
