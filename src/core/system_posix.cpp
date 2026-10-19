#include <cstdio>

#include <rex/system.h>

namespace rex {

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  const char* level = "INFO";
  switch (type) {
    case SimpleMessageBoxType::kInfo:
      level = "INFO";
      break;
    case SimpleMessageBoxType::kWarning:
      level = "WARNING";
      break;
    case SimpleMessageBoxType::kError:
      level = "ERROR";
      break;
  }
  std::fprintf(stderr, "[%s] %.*s\n", level, static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
}

}  // namespace rex
