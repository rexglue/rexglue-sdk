#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <rex/string.h>
#include <rex/system.h>

namespace rex {

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  UINT flags = MB_OK | MB_TOPMOST | MB_SETFOREGROUND;
  const wchar_t* title = L"ReXGlue";
  switch (type) {
    case SimpleMessageBoxType::kInfo:
      flags |= MB_ICONINFORMATION;
      break;
    case SimpleMessageBoxType::kWarning:
      flags |= MB_ICONWARNING;
      break;
    case SimpleMessageBoxType::kError:
      flags |= MB_ICONERROR;
      break;
  }
  auto wide = rex::ToUTF16(message);
  ::MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(wide.c_str()), title, flags);
}

}  // namespace rex
