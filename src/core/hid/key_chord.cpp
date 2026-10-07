#include <rex/hid/key_chord.h>

#include <algorithm>
#include <array>
#include <format>

#include <rex/bit.h>

namespace rex::hid {

namespace {

struct ModifierPrefix {
  std::string_view name;
  KeyModifiers mod;
};

constexpr auto kModifierPrefixes = std::to_array<ModifierPrefix>({
    {"Shift", KeyModifiers::Shift},
    {"Ctrl", KeyModifiers::Ctrl},
    {"Control", KeyModifiers::Ctrl},
    {"Alt", KeyModifiers::Alt},
});

constexpr std::string_view kBlanks = " \t";

bool IsModifierKey(VirtualKey key) {
  return key == VirtualKey::kShift || key == VirtualKey::kControl || key == VirtualKey::kMenu;
}

std::string_view Trim(std::string_view text) {
  const size_t first = text.find_first_not_of(kBlanks);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(kBlanks) - first + 1);
}

}  // namespace

Result<KeyChord> ParseKeyChord(std::string_view text) {
  if (text.empty()) {
    return KeyChord{};
  }
  KeyChord chord;
  std::string_view rest = text;
  for (;;) {
    const size_t plus = rest.find('+');
    if (plus == 0 || plus == std::string_view::npos) {
      break;
    }
    const auto prefix =
        std::ranges::find(kModifierPrefixes, rest.substr(0, plus), &ModifierPrefix::name);
    if (prefix == kModifierPrefixes.end()) {
      break;
    }
    chord.mods |= prefix->mod;
    rest.remove_prefix(plus + 1);
  }
  chord.key = ParseVirtualKey(rest);
  if (chord.key == VirtualKey::kNone) {
    return Err<KeyChord>(ErrorCategory::Config,
                         std::format("no key named '{}' in '{}'", rest, text));
  }
  if (IsModifierKey(chord.key)) {
    return Err<KeyChord>(ErrorCategory::Config,
                         std::format("'{}' is only a modifier and needs a key", text));
  }
  return chord;
}

Result<std::vector<KeyChord>> ParseKeyChordList(std::string_view text) {
  std::vector<KeyChord> chords;
  while (!text.empty()) {
    const size_t comma = text.find(',');
    const std::string_view item = Trim(text.substr(0, comma));
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    if (item.empty()) {
      continue;
    }
    auto chord = ParseKeyChord(item);
    if (!chord) {
      return Err<std::vector<KeyChord>>(std::move(chord.error()));
    }
    chords.push_back(*chord);
  }
  return chords;
}

std::string KeyChordToString(KeyChord chord) {
  const std::string name = VirtualKeyToString(chord.key);
  if (name.empty()) {
    return {};
  }
  std::string text;
  if (HasFlags(chord.mods, KeyModifiers::Shift)) {
    text += "Shift+";
  }
  if (HasFlags(chord.mods, KeyModifiers::Ctrl)) {
    text += "Ctrl+";
  }
  if (HasFlags(chord.mods, KeyModifiers::Alt)) {
    text += "Alt+";
  }
  text += name;
  return text;
}

}  // namespace rex::hid
