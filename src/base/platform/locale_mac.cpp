#include <rex/platform.h>
#include <rex/platform/locale.h>

static_assert(REX_PLATFORM_MAC, "This file is macOS only");

#include <array>
#include <string>

#include <CoreFoundation/CoreFoundation.h>

#include "base/platform/locale_parse.h"

namespace rex::platform {

std::vector<HostLocale> GetPreferredLocales() {
  std::vector<HostLocale> locales;
  CFArrayRef languages = CFLocaleCopyPreferredLanguages();
  if (!languages) {
    return locales;
  }
  for (CFIndex i = 0; i < CFArrayGetCount(languages); ++i) {
    const auto name = static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages, i));
    std::array<char, 64> buffer{};
    if (CFStringGetCString(name, buffer.data(), static_cast<CFIndex>(buffer.size()),
                           kCFStringEncodingUTF8)) {
      if (auto locale = detail::ParseLocaleTag(buffer.data())) {
        locales.push_back(std::move(*locale));
      }
    }
  }
  CFRelease(languages);
  return locales;
}

}  // namespace rex::platform
