#include <string>

#include "core/logger.h"
#include "core/platform_win.h"

namespace rex::log {

ConsoleSink::ConsoleSink(LineFormat format) : format_(format) {
  HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (output != INVALID_HANDLE_VALUE && output != nullptr && GetConsoleMode(output, &mode)) {
    color_supported_ = SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
  }
}

DebuggerSink::DebuggerSink(LineFormat format) : format_(format) {}

void DebuggerSink::Write(const Record& record) {
  LineFormat format = format_;
  format.color = false;
  std::string line = FormatLine(record, format);
  line += '\n';
  const int length =
      MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
  if (length <= 0) {
    return;
  }
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), wide.data(), length);
  OutputDebugStringW(wide.c_str());
}

void DebuggerSink::Flush() {}

std::tm ToLocalTime(std::time_t time) {
  std::tm local{};
  localtime_s(&local, &time);
  return local;
}

}  // namespace rex::log
