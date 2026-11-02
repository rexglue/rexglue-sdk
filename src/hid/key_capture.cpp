#include "hid/key_capture.h"

#include <array>

namespace rex::hid {

namespace {

using VK = VirtualKey;

constexpr auto kBindableKeys = std::to_array<VK>({
    VK::kLButton, VK::kRButton,  VK::kMButton,  VK::kXButton1,  VK::kXButton2, VK::kBack,
    VK::kTab,     VK::kReturn,   VK::kCapital,  VK::kEscape,    VK::kSpace,    VK::kPrior,
    VK::kNext,    VK::kEnd,      VK::kHome,     VK::kLeft,      VK::kUp,       VK::kRight,
    VK::kDown,    VK::kInsert,   VK::kDelete,   VK::k0,         VK::k1,        VK::k2,
    VK::k3,       VK::k4,        VK::k5,        VK::k6,         VK::k7,        VK::k8,
    VK::k9,       VK::kA,        VK::kB,        VK::kC,         VK::kD,        VK::kE,
    VK::kF,       VK::kG,        VK::kH,        VK::kI,         VK::kJ,        VK::kK,
    VK::kL,       VK::kM,        VK::kN,        VK::kO,         VK::kP,        VK::kQ,
    VK::kR,       VK::kS,        VK::kT,        VK::kU,         VK::kV,        VK::kW,
    VK::kX,       VK::kY,        VK::kZ,        VK::kNumpad0,   VK::kNumpad1,  VK::kNumpad2,
    VK::kNumpad3, VK::kNumpad4,  VK::kNumpad5,  VK::kNumpad6,   VK::kNumpad7,  VK::kNumpad8,
    VK::kNumpad9, VK::kMultiply, VK::kAdd,      VK::kSubtract,  VK::kDivide,   VK::kF1,
    VK::kF2,      VK::kF3,       VK::kF4,       VK::kF5,        VK::kF6,       VK::kF7,
    VK::kF8,      VK::kF9,       VK::kF10,      VK::kF11,       VK::kF12,      VK::kOem1,
    VK::kOemPlus, VK::kOemComma, VK::kOemMinus, VK::kOemPeriod, VK::kOem2,     VK::kOem3,
    VK::kOem4,    VK::kOem5,     VK::kOem6,     VK::kOem7,
});

constexpr size_t kKeySlotCount = kBindableKeys.size();
constexpr size_t kSlotCount = kKeySlotCount + kPadButtonCount;

bool IsMouseButton(VK key, MouseButton& button) {
  switch (key) {
    case VK::kLButton:
      button = MouseButton::kLeft;
      return true;
    case VK::kRButton:
      button = MouseButton::kRight;
      return true;
    case VK::kMButton:
      button = MouseButton::kMiddle;
      return true;
    case VK::kXButton1:
      button = MouseButton::kX1;
      return true;
    case VK::kXButton2:
      button = MouseButton::kX2;
      return true;
    default:
      return false;
  }
}

constexpr bool IsAscending() {
  for (size_t i = 1; i < kBindableKeys.size(); ++i) {
    if (static_cast<u16>(kBindableKeys[i - 1]) >= static_cast<u16>(kBindableKeys[i])) {
      return false;
    }
  }
  return true;
}

static_assert(IsAscending(), "kBindableKeys must be in ascending VirtualKey order");

}  // namespace

std::span<const VirtualKey> GetBindableKeys() {
  return kBindableKeys;
}

KeyCapture::KeyCapture(const IInputSystem& input, KeyCaptureConfig config)
    : input_(input), config_(config), previous_down_(kSlotCount, false) {}

void KeyCapture::SetPadButtons(u32 buttons) {
  pad_buttons_ = buttons;
}

bool KeyCapture::IsSlotDown(size_t slot) const {
  if (slot >= kKeySlotCount) {
    return (pad_buttons_ >> (slot - kKeySlotCount) & 1u) != 0;
  }
  const VK key = kBindableKeys[slot];
  MouseButton button{};
  if (IsMouseButton(key, button)) {
    return input_.IsButtonDown(button);
  }
  return input_.IsKeyDown(key);
}

void KeyCapture::Begin() {
  for (size_t i = 0; i < kSlotCount; ++i) {
    previous_down_[i] = IsSlotDown(i);
  }
  wheel_seen_ = input_.GetWheel();
  capturing_ = true;
  pending_ = -1;
  canceled_ = false;
}

std::optional<InputSource> KeyCapture::Poll(std::chrono::steady_clock::time_point now) {
  if (!capturing_) {
    return std::nullopt;
  }

  if (pending_ >= 0) {
    const u32 cancel = config_.pad_cancel_mask;
    if (cancel != 0 && (pad_buttons_ & cancel) == cancel) {
      capturing_ = false;
      pending_ = -1;
      canceled_ = true;
      return std::nullopt;
    }
    if (IsSlotDown(static_cast<size_t>(pending_))) {
      return std::nullopt;
    }
    capturing_ = false;
    const auto hit = static_cast<size_t>(pending_);
    pending_ = -1;
    InputSource source;
    if (hit >= kKeySlotCount) {
      source.type = InputSourceType::kPadButton;
      source.code = static_cast<u16>(hit - kKeySlotCount);
      return source;
    }
    if (kBindableKeys[hit] == VK::kEscape && now - pending_since_ >= config_.cancel_hold) {
      canceled_ = true;
      return std::nullopt;
    }
    source.code = static_cast<u16>(kBindableKeys[hit]);
    source.modifiers = pending_modifiers_;
    return source;
  }

  for (size_t i = 0; i < kSlotCount; ++i) {
    const bool down = IsSlotDown(i);
    if (down && !previous_down_[i] && pending_ < 0) {
      pending_ = static_cast<i32>(i);
      pending_since_ = now;
      pending_modifiers_ = i < kKeySlotCount ? input_.GetModifiers() : KeyModifiers::kNone;
    }
    previous_down_[i] = down;
  }
  if (pending_ >= 0) {
    return std::nullopt;
  }

  const i32 wheel = input_.GetWheel();
  if (wheel == 0) {
    wheel_seen_ = 0;
    return std::nullopt;
  }
  if (wheel == wheel_seen_) {
    return std::nullopt;
  }
  capturing_ = false;
  InputSource source;
  source.type = InputSourceType::kMouseWheel;
  source.sign = wheel > 0 ? 1 : -1;
  return source;
}

bool KeyCapture::IsPending() const {
  return capturing_ && pending_ >= 0;
}

bool KeyCapture::WasCanceled() const {
  return canceled_;
}

}  // namespace rex::hid
