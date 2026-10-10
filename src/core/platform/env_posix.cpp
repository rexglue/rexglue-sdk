#include <rex/platform.h>
#include <rex/platform/env.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <cstdlib>
#include <string>
#include <string_view>

#if REX_PLATFORM_MAC
extern char** environ;
#endif

namespace rex::platform {

std::optional<std::string> GetEnvironmentValue(std::string_view name) {
  std::string c_name(name);
  const char* value = std::getenv(c_name.c_str());
  if (!value) {
    return std::nullopt;
  }
  return std::string(value);
}

bool SetEnvironmentValue(std::string_view name, std::string_view value) {
  std::string c_name(name);
  std::string c_value(value);
  return ::setenv(c_name.c_str(), c_value.c_str(), 1) == 0;
}

bool UnsetEnvironmentValue(std::string_view name) {
  std::string c_name(name);
  return ::unsetenv(c_name.c_str()) == 0;
}

std::vector<std::pair<std::string, std::string>> EnumerateEnvironment() {
  std::vector<std::pair<std::string, std::string>> out;
  for (char** p = environ; p && *p; ++p) {
    std::string_view entry(*p);
    auto eq = entry.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    out.emplace_back(std::string(entry.substr(0, eq)), std::string(entry.substr(eq + 1)));
  }
  return out;
}

}  // namespace rex::platform
