#include "ui/sdl3/sdl_virtual_key.h"

#include <cstdint>

namespace rex::ui::sdl3 {

VirtualKey TranslateSDLScancode(SDL_Scancode scancode) {
  // Contiguous ranges in both encodings.
  if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
    return static_cast<VirtualKey>(static_cast<u32>(VirtualKey::kA) + (scancode - SDL_SCANCODE_A));
  }
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
    return static_cast<VirtualKey>(static_cast<u32>(VirtualKey::k0) + 1 +
                                   (scancode - SDL_SCANCODE_1));
  }
  if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
    return static_cast<VirtualKey>(static_cast<u32>(VirtualKey::kF1) +
                                   (scancode - SDL_SCANCODE_F1));
  }
  if (scancode >= SDL_SCANCODE_F13 && scancode <= SDL_SCANCODE_F24) {
    return static_cast<VirtualKey>(static_cast<u32>(VirtualKey::kF13) +
                                   (scancode - SDL_SCANCODE_F13));
  }
  if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9) {
    // SDL numpad order is 1..9,0 and VK order is 0..9.
    return static_cast<VirtualKey>(static_cast<u32>(VirtualKey::kNumpad0) + 1 +
                                   (scancode - SDL_SCANCODE_KP_1));
  }
  switch (scancode) {
    case SDL_SCANCODE_0:
      return VirtualKey::k0;
    case SDL_SCANCODE_KP_0:
      return VirtualKey::kNumpad0;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
      return VirtualKey::kReturn;
    case SDL_SCANCODE_ESCAPE:
      return VirtualKey::kEscape;
    case SDL_SCANCODE_BACKSPACE:
      return VirtualKey::kBack;
    case SDL_SCANCODE_TAB:
      return VirtualKey::kTab;
    case SDL_SCANCODE_SPACE:
      return VirtualKey::kSpace;
    case SDL_SCANCODE_LEFT:
      return VirtualKey::kLeft;
    case SDL_SCANCODE_UP:
      return VirtualKey::kUp;
    case SDL_SCANCODE_RIGHT:
      return VirtualKey::kRight;
    case SDL_SCANCODE_DOWN:
      return VirtualKey::kDown;
    case SDL_SCANCODE_PAGEUP:
      return VirtualKey::kPrior;
    case SDL_SCANCODE_PAGEDOWN:
      return VirtualKey::kNext;
    case SDL_SCANCODE_HOME:
      return VirtualKey::kHome;
    case SDL_SCANCODE_END:
      return VirtualKey::kEnd;
    case SDL_SCANCODE_INSERT:
      return VirtualKey::kInsert;
    case SDL_SCANCODE_DELETE:
      return VirtualKey::kDelete;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
      return VirtualKey::kShift;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
      return VirtualKey::kControl;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT:
      return VirtualKey::kMenu;
    case SDL_SCANCODE_LGUI:
      return VirtualKey::kLWin;
    case SDL_SCANCODE_RGUI:
      return VirtualKey::kRWin;
    case SDL_SCANCODE_APPLICATION:
      return VirtualKey::kApps;
    case SDL_SCANCODE_CAPSLOCK:
      return VirtualKey::kCapital;
    case SDL_SCANCODE_PAUSE:
      return VirtualKey::kPause;
    case SDL_SCANCODE_PRINTSCREEN:
      return VirtualKey::kSnapshot;
    case SDL_SCANCODE_SCROLLLOCK:
      return VirtualKey::kScroll;
    case SDL_SCANCODE_NUMLOCKCLEAR:
      return VirtualKey::kNumLock;
    case SDL_SCANCODE_KP_MULTIPLY:
      return VirtualKey::kMultiply;
    case SDL_SCANCODE_KP_PLUS:
      return VirtualKey::kAdd;
    case SDL_SCANCODE_KP_MINUS:
      return VirtualKey::kSubtract;
    case SDL_SCANCODE_KP_PERIOD:
      return VirtualKey::kDecimal;
    case SDL_SCANCODE_KP_DIVIDE:
      return VirtualKey::kDivide;
    case SDL_SCANCODE_SEMICOLON:
      return VirtualKey::kOem1;
    case SDL_SCANCODE_EQUALS:
      return VirtualKey::kOemPlus;
    case SDL_SCANCODE_COMMA:
      return VirtualKey::kOemComma;
    case SDL_SCANCODE_MINUS:
      return VirtualKey::kOemMinus;
    case SDL_SCANCODE_PERIOD:
      return VirtualKey::kOemPeriod;
    case SDL_SCANCODE_SLASH:
      return VirtualKey::kOem2;
    case SDL_SCANCODE_GRAVE:
      return VirtualKey::kOem3;
    case SDL_SCANCODE_LEFTBRACKET:
      return VirtualKey::kOem4;
    case SDL_SCANCODE_BACKSLASH:
      return VirtualKey::kOem5;
    case SDL_SCANCODE_RIGHTBRACKET:
      return VirtualKey::kOem6;
    case SDL_SCANCODE_APOSTROPHE:
      return VirtualKey::kOem7;
    default:
      return VirtualKey::kNone;
  }
}

}  // namespace rex::ui::sdl3
