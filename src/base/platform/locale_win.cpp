#include <rex/platform.h>
#include <rex/platform/locale.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows only");

#include <string>

#include <windows.h>

#include "base/platform/locale_parse.h"

namespace rex::platform {

std::vector<HostLocale> GetPreferredLocales() {
  std::vector<HostLocale> locales;
  ULONG count = 0;
  ULONG size = 0;
  if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || size == 0) {
    return locales;
  }
  std::wstring buffer(size, L'\0');
  if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size)) {
    return locales;
  }
  size_t start = 0;
  while (start < buffer.size() && buffer[start] != L'\0') {
    const size_t end = buffer.find(L'\0', start);
    std::string name;
    for (size_t i = start; i < end; ++i) {
      name.push_back(buffer[i] < 0x80 ? static_cast<char>(buffer[i]) : '?');
    }
    if (auto locale = detail::ParseLocaleTag(name)) {
      locales.push_back(std::move(*locale));
    }
    start = end + 1;
  }
  return locales;
}

}  // namespace rex::platform
