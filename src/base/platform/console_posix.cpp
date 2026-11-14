#include <rex/platform.h>
#include <rex/platform/process.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <unistd.h>

namespace rex::platform {

bool IsTerminal(FILE* stream) {
  if (!stream) {
    return false;
  }
  return ::isatty(::fileno(stream)) != 0;
}

bool EnableANSIEscapes(FILE* stream) {
  return stream != nullptr;
}

void SetUTF8OutputCodePage() {}

}  // namespace rex::platform
