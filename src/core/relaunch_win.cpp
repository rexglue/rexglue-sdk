/**
 * @file        core/relaunch_win.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform/process.h>

#include <format>
#include <string>

#include <windows.h>

#include <rex/platform/file.h>

namespace rex::platform {
namespace {

std::wstring Widen(std::string_view text) {
  const i32 length =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<i32>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<i32>(text.size()), wide.data(), length);
  return wide;
}

void AppendQuoted(std::wstring& command, std::wstring_view arg) {
  command += L" \"";
  size_t backslashes = 0;
  for (wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    command.append(ch == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    command += ch;
    backslashes = 0;
  }
  command.append(backslashes * 2, L'\\');
  command += L'"';
}

}  // namespace

std::wstring detail::BuildCommandLine(std::wstring_view exe, std::span<const std::string> args) {
  std::wstring command = L"\"" + std::wstring(exe) + L"\"";
  for (const std::string& arg : args) {
    AppendQuoted(command, Widen(arg));
  }
  return command;
}

[[nodiscard]] Result<void> Relaunch(std::span<const std::string> args) {
  const std::wstring exe = platform::GetExecutablePath().wstring();
  std::wstring command = detail::BuildCommandLine(exe, args);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                      &startup, &process)) {
    return Err<void>(ErrorCategory::Platform,
                     std::format("CreateProcessW failed ({})", GetLastError()));
  }
  AllowSetForegroundWindow(process.dwProcessId);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return Ok();
}

bool CanRelaunch() {
  return true;
}

}  // namespace rex::platform
