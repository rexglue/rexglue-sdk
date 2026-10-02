/**
 * @file        ui/sdl_virtual_key.cpp
 * @brief       SDL3 scancode to Win32-style VirtualKey translation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/sdl_virtual_key.h>

#include <cstdint>

namespace rex::ui {

VirtualKey TranslateSDLScancode(SDL_Scancode scancode) {
  // Contiguous ranges in both encodings.
  if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
    return VirtualKey(uint32_t(VirtualKey::kA) + (scancode - SDL_SCANCODE_A));
  }
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
    return VirtualKey(uint32_t(VirtualKey::k0) + 1 + (scancode - SDL_SCANCODE_1));
  }
  if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
    return VirtualKey(uint32_t(VirtualKey::kF1) + (scancode - SDL_SCANCODE_F1));
  }
  if (scancode >= SDL_SCANCODE_F13 && scancode <= SDL_SCANCODE_F24) {
    return VirtualKey(uint32_t(VirtualKey::kF13) + (scancode - SDL_SCANCODE_F13));
  }
  if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9) {
    // SDL numpad order is 1..9,0; VK order is 0..9.
    return VirtualKey(uint32_t(VirtualKey::kNumpad0) + 1 + (scancode - SDL_SCANCODE_KP_1));
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

SDL_Scancode TranslateVirtualKeyToSDLScancode(VirtualKey vk) {
  // Contiguous ranges in both encodings.
  if (vk >= VirtualKey::kA && vk <= VirtualKey::kZ) {
    return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (uint32_t(vk) - uint32_t(VirtualKey::kA)));
  }
  if (vk >= VirtualKey::k1 && vk <= VirtualKey::k9) {
    return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (uint32_t(vk) - uint32_t(VirtualKey::k1)));
  }
  if (vk >= VirtualKey::kF1 && vk <= VirtualKey::kF12) {
    return static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (uint32_t(vk) - uint32_t(VirtualKey::kF1)));
  }
  if (vk >= VirtualKey::kF13 && vk <= VirtualKey::kF24) {
    return static_cast<SDL_Scancode>(SDL_SCANCODE_F13 +
                                     (uint32_t(vk) - uint32_t(VirtualKey::kF13)));
  }
  if (vk >= VirtualKey::kNumpad1 && vk <= VirtualKey::kNumpad9) {
    // SDL numpad order is 1..9,0; VK order is 0..9.
    return static_cast<SDL_Scancode>(SDL_SCANCODE_KP_1 +
                                     (uint32_t(vk) - uint32_t(VirtualKey::kNumpad1)));
  }
  switch (vk) {
    case VirtualKey::k0:
      return SDL_SCANCODE_0;
    case VirtualKey::kNumpad0:
      return SDL_SCANCODE_KP_0;
    case VirtualKey::kReturn:
      return SDL_SCANCODE_RETURN;
    case VirtualKey::kEscape:
      return SDL_SCANCODE_ESCAPE;
    case VirtualKey::kBack:
      return SDL_SCANCODE_BACKSPACE;
    case VirtualKey::kTab:
      return SDL_SCANCODE_TAB;
    case VirtualKey::kSpace:
      return SDL_SCANCODE_SPACE;
    case VirtualKey::kLeft:
      return SDL_SCANCODE_LEFT;
    case VirtualKey::kUp:
      return SDL_SCANCODE_UP;
    case VirtualKey::kRight:
      return SDL_SCANCODE_RIGHT;
    case VirtualKey::kDown:
      return SDL_SCANCODE_DOWN;
    case VirtualKey::kPrior:
      return SDL_SCANCODE_PAGEUP;
    case VirtualKey::kNext:
      return SDL_SCANCODE_PAGEDOWN;
    case VirtualKey::kHome:
      return SDL_SCANCODE_HOME;
    case VirtualKey::kEnd:
      return SDL_SCANCODE_END;
    case VirtualKey::kInsert:
      return SDL_SCANCODE_INSERT;
    case VirtualKey::kDelete:
      return SDL_SCANCODE_DELETE;
    case VirtualKey::kLShift:
    case VirtualKey::kShift:
      return SDL_SCANCODE_LSHIFT;
    case VirtualKey::kRShift:
      return SDL_SCANCODE_RSHIFT;
    case VirtualKey::kLControl:
    case VirtualKey::kControl:
      return SDL_SCANCODE_LCTRL;
    case VirtualKey::kRControl:
      return SDL_SCANCODE_RCTRL;
    case VirtualKey::kLMenu:
    case VirtualKey::kMenu:
      return SDL_SCANCODE_LALT;
    case VirtualKey::kRMenu:
      return SDL_SCANCODE_RALT;
    case VirtualKey::kLWin:
      return SDL_SCANCODE_LGUI;
    case VirtualKey::kRWin:
      return SDL_SCANCODE_RGUI;
    case VirtualKey::kApps:
      return SDL_SCANCODE_APPLICATION;
    case VirtualKey::kCapital:
      return SDL_SCANCODE_CAPSLOCK;
    case VirtualKey::kPause:
      return SDL_SCANCODE_PAUSE;
    case VirtualKey::kSnapshot:
      return SDL_SCANCODE_PRINTSCREEN;
    case VirtualKey::kScroll:
      return SDL_SCANCODE_SCROLLLOCK;
    case VirtualKey::kNumLock:
      return SDL_SCANCODE_NUMLOCKCLEAR;
    case VirtualKey::kMultiply:
      return SDL_SCANCODE_KP_MULTIPLY;
    case VirtualKey::kAdd:
      return SDL_SCANCODE_KP_PLUS;
    case VirtualKey::kSubtract:
      return SDL_SCANCODE_KP_MINUS;
    case VirtualKey::kDecimal:
      return SDL_SCANCODE_KP_PERIOD;
    case VirtualKey::kDivide:
      return SDL_SCANCODE_KP_DIVIDE;
    case VirtualKey::kOem1:
      return SDL_SCANCODE_SEMICOLON;
    case VirtualKey::kOemPlus:
      return SDL_SCANCODE_EQUALS;
    case VirtualKey::kOemComma:
      return SDL_SCANCODE_COMMA;
    case VirtualKey::kOemMinus:
      return SDL_SCANCODE_MINUS;
    case VirtualKey::kOemPeriod:
      return SDL_SCANCODE_PERIOD;
    case VirtualKey::kOem2:
      return SDL_SCANCODE_SLASH;
    case VirtualKey::kOem3:
      return SDL_SCANCODE_GRAVE;
    case VirtualKey::kOem4:
      return SDL_SCANCODE_LEFTBRACKET;
    case VirtualKey::kOem5:
      return SDL_SCANCODE_BACKSLASH;
    case VirtualKey::kOem6:
      return SDL_SCANCODE_RIGHTBRACKET;
    case VirtualKey::kOem7:
      return SDL_SCANCODE_APOSTROPHE;
    default:
      return SDL_SCANCODE_UNKNOWN;
  }
}

}  // namespace rex::ui
