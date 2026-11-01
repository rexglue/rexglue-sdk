#include <cstdio>

#include <rex/platform/process.h>

namespace rex {

void ShowMessageBox(MessageType type, std::string_view message) {
  const char* level = "INFO";
  switch (type) {
    case MessageType::kInfo:
      level = "INFO";
      break;
    case MessageType::kWarning:
      level = "WARNING";
      break;
    case MessageType::kError:
      level = "ERROR";
      break;
  }
  std::fprintf(stderr, "[%s] %.*s\n", level, static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
}

}  // namespace rex
