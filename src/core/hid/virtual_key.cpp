#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include <rex/hid/virtual_key.h>

namespace rex::hid {

namespace {

struct KeyName {
  std::string_view name;
  VirtualKey key;
};

constexpr auto kKeyNames = std::to_array<KeyName>({
    {"F1", VirtualKey::kF1},
    {"F2", VirtualKey::kF2},
    {"F3", VirtualKey::kF3},
    {"F4", VirtualKey::kF4},
    {"F5", VirtualKey::kF5},
    {"F6", VirtualKey::kF6},
    {"F7", VirtualKey::kF7},
    {"F8", VirtualKey::kF8},
    {"F9", VirtualKey::kF9},
    {"F10", VirtualKey::kF10},
    {"F11", VirtualKey::kF11},
    {"F12", VirtualKey::kF12},
    {"F13", VirtualKey::kF13},
    {"F14", VirtualKey::kF14},
    {"F15", VirtualKey::kF15},
    {"F16", VirtualKey::kF16},
    {"F17", VirtualKey::kF17},
    {"F18", VirtualKey::kF18},
    {"F19", VirtualKey::kF19},
    {"F20", VirtualKey::kF20},
    {"F21", VirtualKey::kF21},
    {"F22", VirtualKey::kF22},
    {"F23", VirtualKey::kF23},
    {"F24", VirtualKey::kF24},
    {"A", VirtualKey::kA},
    {"B", VirtualKey::kB},
    {"C", VirtualKey::kC},
    {"D", VirtualKey::kD},
    {"E", VirtualKey::kE},
    {"F", VirtualKey::kF},
    {"G", VirtualKey::kG},
    {"H", VirtualKey::kH},
    {"I", VirtualKey::kI},
    {"J", VirtualKey::kJ},
    {"K", VirtualKey::kK},
    {"L", VirtualKey::kL},
    {"M", VirtualKey::kM},
    {"N", VirtualKey::kN},
    {"O", VirtualKey::kO},
    {"P", VirtualKey::kP},
    {"Q", VirtualKey::kQ},
    {"R", VirtualKey::kR},
    {"S", VirtualKey::kS},
    {"T", VirtualKey::kT},
    {"U", VirtualKey::kU},
    {"V", VirtualKey::kV},
    {"W", VirtualKey::kW},
    {"X", VirtualKey::kX},
    {"Y", VirtualKey::kY},
    {"Z", VirtualKey::kZ},
    {"0", VirtualKey::k0},
    {"1", VirtualKey::k1},
    {"2", VirtualKey::k2},
    {"3", VirtualKey::k3},
    {"4", VirtualKey::k4},
    {"5", VirtualKey::k5},
    {"6", VirtualKey::k6},
    {"7", VirtualKey::k7},
    {"8", VirtualKey::k8},
    {"9", VirtualKey::k9},
    {"Backtick", VirtualKey::kOem3},
    {"Minus", VirtualKey::kOemMinus},
    {"Plus", VirtualKey::kOemPlus},
    {"Comma", VirtualKey::kOemComma},
    {"Period", VirtualKey::kOemPeriod},
    {"Semicolon", VirtualKey::kOem1},
    {"Slash", VirtualKey::kOem2},
    {"Backslash", VirtualKey::kOem5},
    {"LBracket", VirtualKey::kOem4},
    {"RBracket", VirtualKey::kOem6},
    {"Quote", VirtualKey::kOem7},
    {"Escape", VirtualKey::kEscape},
    {"Return", VirtualKey::kReturn},
    {"Space", VirtualKey::kSpace},
    {"Tab", VirtualKey::kTab},
    {"Backspace", VirtualKey::kBack},
    {"Delete", VirtualKey::kDelete},
    {"Insert", VirtualKey::kInsert},
    {"Home", VirtualKey::kHome},
    {"End", VirtualKey::kEnd},
    {"PageUp", VirtualKey::kPrior},
    {"PageDown", VirtualKey::kNext},
    {"Left", VirtualKey::kLeft},
    {"Right", VirtualKey::kRight},
    {"Up", VirtualKey::kUp},
    {"Down", VirtualKey::kDown},
    {"Shift", VirtualKey::kShift},
    {"Control", VirtualKey::kControl},
    {"Alt", VirtualKey::kMenu},
    {"Numpad0", VirtualKey::kNumpad0},
    {"Numpad1", VirtualKey::kNumpad1},
    {"Numpad2", VirtualKey::kNumpad2},
    {"Numpad3", VirtualKey::kNumpad3},
    {"Numpad4", VirtualKey::kNumpad4},
    {"Numpad5", VirtualKey::kNumpad5},
    {"Numpad6", VirtualKey::kNumpad6},
    {"Numpad7", VirtualKey::kNumpad7},
    {"Numpad8", VirtualKey::kNumpad8},
    {"Numpad9", VirtualKey::kNumpad9},
    {"NumpadEnter", VirtualKey::kReturn},
    {"NumpadPlus", VirtualKey::kAdd},
    {"NumpadMinus", VirtualKey::kSubtract},
    {"NumpadStar", VirtualKey::kMultiply},
    {"NumpadSlash", VirtualKey::kDivide},
    {"PrintScreen", VirtualKey::kSnapshot},
    {"Pause", VirtualKey::kPause},
    {"CapsLock", VirtualKey::kCapital},
    {"NumLock", VirtualKey::kNumLock},
    {"ScrollLock", VirtualKey::kScroll},
    {"LMB", VirtualKey::kLButton},
    {"RMB", VirtualKey::kRButton},
    {"MMB", VirtualKey::kMButton},
});

}  // namespace

VirtualKey ParseVirtualKey(std::string_view name) {
  const auto it = std::ranges::find(kKeyNames, name, &KeyName::name);
  return it != kKeyNames.end() ? it->key : VirtualKey::kNone;
}

std::string VirtualKeyToString(VirtualKey vk) {
  const auto it = std::ranges::find(kKeyNames, vk, &KeyName::key);
  return it != kKeyNames.end() ? std::string(it->name) : std::string();
}

}  // namespace rex::hid
