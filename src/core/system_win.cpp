/**
 * @file        core/system_win.cpp
 * @brief       Windows implementations of rex/system.h platform helpers.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <system_error>
#include <thread>

#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system.h>

namespace rex {

namespace {

// ShellExecute may load shell extensions through COM and can stall, so it runs
// on its own thread instead of the caller's (usually the UI thread).
void ShellOpenDetached(std::wstring target, std::string what) {
  std::thread([target = std::move(target), what = std::move(what)]() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
    if (result <= 32) {
      REXLOG_WARN("Could not open {} (ShellExecute error {})", what, result);
    }
  }).detach();
}

}  // namespace

void LaunchWebBrowser(const std::string_view url) {
  const auto wide = rex::string::to_utf16(url);
  ShellOpenDetached(std::wstring(reinterpret_cast<const wchar_t*>(wide.c_str()), wide.size()),
                    std::string(url));
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  // Absolute first, so a relative file name still has a parent folder.
  std::error_code ec;
  auto target = std::filesystem::absolute(path, ec);
  if (ec) {
    target = path;
  }
  // A file opens its containing folder rather than the file itself.
  const auto folder = std::filesystem::is_regular_file(target, ec) ? target.parent_path() : target;
  ShellOpenDetached(folder.wstring(), rex::string::to_utf8(folder.u16string()));
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  UINT flags = MB_OK | MB_TOPMOST | MB_SETFOREGROUND;
  std::wstring title = L"ReXGlue";
  if (const auto configured = rex::string::to_utf16(GetMessageBoxTitle()); !configured.empty()) {
    title.assign(reinterpret_cast<const wchar_t*>(configured.c_str()), configured.size());
  }
  switch (type) {
    case SimpleMessageBoxType::Help:
      flags |= MB_ICONINFORMATION;
      break;
    case SimpleMessageBoxType::Warning:
      flags |= MB_ICONWARNING;
      break;
    case SimpleMessageBoxType::Error:
      flags |= MB_ICONERROR;
      break;
  }
  auto wide = rex::string::to_utf16(message);
  ::MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(wide.c_str()), title.c_str(), flags);
}

}  // namespace rex
