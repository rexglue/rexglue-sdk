#include <cstdio>

#include <rex/path_settings.h>
#include <rex/settings_registry.h>

namespace rex {

PathSettings::PathSettings() : SettingOwner("app") {}

PathSettings& GetPathSettings() {
  static PathSettings& settings = [] -> PathSettings& {
    static PathSettings owner;
    if (auto registered = REX_SETTINGS().Register(owner); !registered) {
      std::fprintf(stderr, "path settings are not registered, %s\n",
                   registered.error().message.c_str());
    }
    return owner;
  }();
  return settings;
}

}  // namespace rex
