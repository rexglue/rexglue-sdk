#include <rex/input_keys.h>

#include <array>
#include <format>

namespace rex {

namespace {

constexpr auto kPadNames = std::to_array<std::string_view>({
    "PadDUp",      "PadDDown",   "PadDLeft", "PadDRight",  "PadStart",    "PadBack",
    "PadLS",       "PadRS",      "PadA",     "PadB",       "PadX",        "PadY",
    "PadLT",       "PadRT",      "PadLB",    "PadRB",      "LStickUp",    "LStickDown",
    "LStickRight", "LStickLeft", "RStickUp", "RStickDown", "RStickRight", "RStickLeft",
});

static_assert(kPadNames.size() == kPadButtonCount);

constexpr std::string_view kLeftAxesName = "LStick";
constexpr std::string_view kRightAxesName = "RStick";

}  // namespace

std::string_view GetPadButtonName(PadInput button) {
  const auto index = static_cast<size_t>(button);
  return index < kPadNames.size() ? kPadNames[index] : std::string_view{};
}

[[nodiscard]] Result<InputSource> ParseInputSource(std::string_view text) {
  if (text == kLeftAxesName || text == kRightAxesName) {
    InputSource source;
    source.type = InputSourceType::kPadAxes;
    source.code = static_cast<u16>(text == kLeftAxesName ? PadAxes::kLeft : PadAxes::kRight);
    return source;
  }
  for (size_t i = 0; i < kPadNames.size(); ++i) {
    if (text == kPadNames[i]) {
      InputSource source;
      source.type = InputSourceType::kPadButton;
      source.code = static_cast<u16>(i);
      return source;
    }
  }
  auto chord = ParseKeyChord(text);
  if (!chord) {
    return Err<InputSource>(std::move(chord.error()));
  }
  if (chord->key == VirtualKey::kNone) {
    return Err<InputSource>(ErrorCategory::kConfig, std::format("'{}' names no input", text));
  }
  InputSource source;
  source.modifiers = chord->modifiers;
  if (chord->key == VirtualKey::kWheelUp || chord->key == VirtualKey::kWheelDown) {
    source.type = InputSourceType::kMouseWheel;
    source.sign = chord->key == VirtualKey::kWheelUp ? 1 : -1;
    return source;
  }
  source.code = static_cast<u16>(chord->key);
  return source;
}

std::string InputSourceToString(const InputSource& source) {
  switch (source.type) {
    case InputSourceType::kPadButton:
      return std::string(GetPadButtonName(static_cast<PadInput>(source.code)));
    case InputSourceType::kPadAxes:
      if (source.code == static_cast<u16>(PadAxes::kLeft)) {
        return std::string(kLeftAxesName);
      }
      if (source.code == static_cast<u16>(PadAxes::kRight)) {
        return std::string(kRightAxesName);
      }
      return {};
    case InputSourceType::kMouseWheel:
      return KeyChordToString(
          {source.sign > 0 ? VirtualKey::kWheelUp : VirtualKey::kWheelDown, source.modifiers});
    case InputSourceType::kKey:
      return KeyChordToString({static_cast<VirtualKey>(source.code), source.modifiers});
  }
  return {};
}

}  // namespace rex
