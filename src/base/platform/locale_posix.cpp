#include <rex/platform.h>
#include <rex/platform/locale.h>

static_assert(REX_PLATFORM_LINUX, "This file is Linux only");

#include <string>

#include <rex/platform/process.h>

#include "base/platform/locale_parse.h"

namespace rex::platform {

std::vector<HostLocale> GetPreferredLocales() {
  const auto read = [](std::string_view name) {
    return GetEnvironmentValue(name).value_or(std::string{});
  };
  return detail::ParseLocaleEnvironment(read("LANGUAGE"), read("LC_ALL"), read("LC_MESSAGES"),
                                        read("LANG"));
}

}  // namespace rex::platform
