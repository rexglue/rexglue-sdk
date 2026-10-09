#include "ui/sdl3/sdl_virtual_key.h"

#include <cstdint>

namespace rex::ui {

hid::VirtualKey TranslateSDLScancode(SDL_Scancode scancode) {
  // Contiguous ranges in both encodings.
  if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
    return hid::VirtualKey(static_cast<u32>(hid::VirtualKey::kA) + (scancode - SDL_SCANCODE_A));
  }
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
    return hid::VirtualKey(static_cast<u32>(hid::VirtualKey::k0) + 1 + (scancode - SDL_SCANCODE_1));
  }
  if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
    return hid::VirtualKey(static_cast<u32>(hid::VirtualKey::kF1) + (scancode - SDL_SCANCODE_F1));
  }
  if (scancode >= SDL_SCANCODE_F13 && scancode <= SDL_SCANCODE_F24) {
    return hid::VirtualKey(static_cast<u32>(hid::VirtualKey::kF13) + (scancode - SDL_SCANCODE_F13));
  }
  if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9) {
    // SDL numpad order is 1..9,0 and VK order is 0..9.
    return hid::VirtualKey(static_cast<u32>(hid::VirtualKey::kNumpad0) + 1 +
                           (scancode - SDL_SCANCODE_KP_1));
  }
  switch (scancode) {
    case SDL_SCANCODE_0:
      return hid::VirtualKey::k0;
    case SDL_SCANCODE_KP_0:
      return hid::VirtualKey::kNumpad0;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
      return hid::VirtualKey::kReturn;
    case SDL_SCANCODE_ESCAPE:
      return hid::VirtualKey::kEscape;
    case SDL_SCANCODE_BACKSPACE:
      return hid::VirtualKey::kBack;
    case SDL_SCANCODE_TAB:
      return hid::VirtualKey::kTab;
    case SDL_SCANCODE_SPACE:
      return hid::VirtualKey::kSpace;
    case SDL_SCANCODE_LEFT:
      return hid::VirtualKey::kLeft;
    case SDL_SCANCODE_UP:
      return hid::VirtualKey::kUp;
    case SDL_SCANCODE_RIGHT:
      return hid::VirtualKey::kRight;
    case SDL_SCANCODE_DOWN:
      return hid::VirtualKey::kDown;
    case SDL_SCANCODE_PAGEUP:
      return hid::VirtualKey::kPrior;
    case SDL_SCANCODE_PAGEDOWN:
      return hid::VirtualKey::kNext;
    case SDL_SCANCODE_HOME:
      return hid::VirtualKey::kHome;
    case SDL_SCANCODE_END:
      return hid::VirtualKey::kEnd;
    case SDL_SCANCODE_INSERT:
      return hid::VirtualKey::kInsert;
    case SDL_SCANCODE_DELETE:
      return hid::VirtualKey::kDelete;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
      return hid::VirtualKey::kShift;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
      return hid::VirtualKey::kControl;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT:
      return hid::VirtualKey::kMenu;
    case SDL_SCANCODE_LGUI:
      return hid::VirtualKey::kLWin;
    case SDL_SCANCODE_RGUI:
      return hid::VirtualKey::kRWin;
    case SDL_SCANCODE_APPLICATION:
      return hid::VirtualKey::kApps;
    case SDL_SCANCODE_CAPSLOCK:
      return hid::VirtualKey::kCapital;
    case SDL_SCANCODE_PAUSE:
      return hid::VirtualKey::kPause;
    case SDL_SCANCODE_PRINTSCREEN:
      return hid::VirtualKey::kSnapshot;
    case SDL_SCANCODE_SCROLLLOCK:
      return hid::VirtualKey::kScroll;
    case SDL_SCANCODE_NUMLOCKCLEAR:
      return hid::VirtualKey::kNumLock;
    case SDL_SCANCODE_KP_MULTIPLY:
      return hid::VirtualKey::kMultiply;
    case SDL_SCANCODE_KP_PLUS:
      return hid::VirtualKey::kAdd;
    case SDL_SCANCODE_KP_MINUS:
      return hid::VirtualKey::kSubtract;
    case SDL_SCANCODE_KP_PERIOD:
      return hid::VirtualKey::kDecimal;
    case SDL_SCANCODE_KP_DIVIDE:
      return hid::VirtualKey::kDivide;
    case SDL_SCANCODE_SEMICOLON:
      return hid::VirtualKey::kOem1;
    case SDL_SCANCODE_EQUALS:
      return hid::VirtualKey::kOemPlus;
    case SDL_SCANCODE_COMMA:
      return hid::VirtualKey::kOemComma;
    case SDL_SCANCODE_MINUS:
      return hid::VirtualKey::kOemMinus;
    case SDL_SCANCODE_PERIOD:
      return hid::VirtualKey::kOemPeriod;
    case SDL_SCANCODE_SLASH:
      return hid::VirtualKey::kOem2;
    case SDL_SCANCODE_GRAVE:
      return hid::VirtualKey::kOem3;
    case SDL_SCANCODE_LEFTBRACKET:
      return hid::VirtualKey::kOem4;
    case SDL_SCANCODE_BACKSLASH:
      return hid::VirtualKey::kOem5;
    case SDL_SCANCODE_RIGHTBRACKET:
      return hid::VirtualKey::kOem6;
    case SDL_SCANCODE_APOSTROPHE:
      return hid::VirtualKey::kOem7;
    default:
      return hid::VirtualKey::kNone;
  }
}

}  // namespace rex::ui
