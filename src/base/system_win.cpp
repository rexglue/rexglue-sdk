#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <rex/string/text.h>
#include <rex/platform/process.h>

namespace rex {

void ShowMessageBox(MessageType type, std::string_view message) {
  UINT flags = MB_OK | MB_TOPMOST | MB_SETFOREGROUND;
  const wchar_t* title = L"ReXGlue";
  switch (type) {
    case MessageType::kInfo:
      flags |= MB_ICONINFORMATION;
      break;
    case MessageType::kWarning:
      flags |= MB_ICONWARNING;
      break;
    case MessageType::kError:
      flags |= MB_ICONERROR;
      break;
  }
  auto wide = rex::string::ToUTF16(message);
  ::MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(wide.c_str()), title, flags);
}

}  // namespace rex
