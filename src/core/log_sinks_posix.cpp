#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "core/logger.h"

namespace rex::log {

ConsoleSink::ConsoleSink(LineFormat format)
    : format_(format), color_supported_(isatty(fileno(stdout)) != 0) {}

DebuggerSink::DebuggerSink(LineFormat format) : format_(format) {}

void DebuggerSink::Write(const Record& record) {
  LineFormat format = format_;
  format.color = false;
  std::string line = FormatLine(record, format);
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), stderr);
}

void DebuggerSink::Flush() {
  std::fflush(stderr);
}

std::tm ToLocalTime(std::time_t time) {
  std::tm local{};
  localtime_r(&time, &local);
  return local;
}

int OpenCrashFile(const std::filesystem::path& path) {
  return open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
}

void CloseCrashFile(int fd) {
  if (fd >= 0) {
    close(fd);
  }
}

}  // namespace rex::log
