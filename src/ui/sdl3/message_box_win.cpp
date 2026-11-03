#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <rex/string/text.h>

#include "ui/sdl3/message_box.h"

namespace rex::ui::sdl3 {

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

UINT IconFlag(MessageType type) {
  switch (type) {
    case MessageType::kError:
      return MB_ICONERROR;
    case MessageType::kWarning:
      return MB_ICONWARNING;
    case MessageType::kInfo:
      return MB_ICONINFORMATION;
  }
  return MB_ICONINFORMATION;
}

[[nodiscard]] Result<u32> ShowTopmostBox(MessageType type, const std::string& title,
                                         const std::string& body) {
  const std::u16string wide_title = string::ToUTF16(title);
  const std::u16string wide_body = string::ToUTF16(body);
  const int pressed = MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(wide_body.c_str()),
                                  reinterpret_cast<const wchar_t*>(wide_title.c_str()),
                                  MB_OK | IconFlag(type) | MB_TOPMOST | MB_SETFOREGROUND);
  if (pressed == 0) {
    return Err<u32>(ErrorCategory::kPlatform, "MessageBoxW failed");
  }
  return 0u;
}

}  // namespace

[[nodiscard]] Result<u32> ShowNativeBox(MessageType type, const std::string& title,
                                        const std::string& body,
                                        std::span<const std::string> buttons, SDL_Window* parent) {
  (void)parent;
  std::vector<HWND> minimized;
  EnumWindows(MinimizeProcessWindow, reinterpret_cast<LPARAM>(&minimized));
  Result<u32> pressed = buttons.size() == 1 ? ShowTopmostBox(type, title, body)
                                            : ShowSDLBox(type, title, body, buttons, nullptr);
  for (HWND hwnd : minimized) {
    ShowWindowAsync(hwnd, SW_RESTORE);
  }
  return pressed;
}

}  // namespace rex::ui::sdl3
