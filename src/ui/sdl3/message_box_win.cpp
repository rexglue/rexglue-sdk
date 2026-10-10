#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <rex/string.h>

#include "ui/sdl3/message_box.h"

namespace rex::ui {

namespace {

BOOL CALLBACK MinimizeProcessWindow(HWND hwnd, LPARAM param) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && !IsIconic(hwnd) &&
      ::GetWindow(hwnd, GW_OWNER) == nullptr) {
    ShowWindowAsync(hwnd, SW_FORCEMINIMIZE);
    // NOLINTNEXTLINE(performance-no-int-to-ptr): EnumWindows passes the context as an LPARAM
    reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
  }
  return TRUE;
}

UINT IconFlag(MessageKind kind) {
  switch (kind) {
    case MessageKind::Error:
      return MB_ICONERROR;
    case MessageKind::Warning:
      return MB_ICONWARNING;
    case MessageKind::Info:
      return MB_ICONINFORMATION;
  }
  return MB_ICONINFORMATION;
}

[[nodiscard]] Result<u32> ShowTopmostBox(MessageKind kind, const std::string& title,
                                         const std::string& body) {
  const std::u16string wide_title = to_utf16(title);
  const std::u16string wide_body = to_utf16(body);
  const int pressed = MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(wide_body.c_str()),
                                  reinterpret_cast<const wchar_t*>(wide_title.c_str()),
                                  MB_OK | IconFlag(kind) | MB_TOPMOST | MB_SETFOREGROUND);
  if (pressed == 0) {
    return Err<u32>(ErrorCategory::Platform, "MessageBoxW failed");
  }
  return 0u;
}

}  // namespace

[[nodiscard]] Result<u32> ShowNativeBox(MessageKind kind, const std::string& title,
                                        const std::string& body,
                                        std::span<const std::string> buttons, SDL_Window* parent) {
  (void)parent;
  std::vector<HWND> minimized;
  EnumWindows(MinimizeProcessWindow, reinterpret_cast<LPARAM>(&minimized));
  Result<u32> pressed = buttons.size() == 1 ? ShowTopmostBox(kind, title, body)
                                            : ShowSDLBox(kind, title, body, buttons, nullptr);
  for (HWND hwnd : minimized) {
    ShowWindowAsync(hwnd, SW_RESTORE);
  }
  return pressed;
}

}  // namespace rex::ui
