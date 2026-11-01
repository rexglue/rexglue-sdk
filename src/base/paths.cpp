#include <rex/paths.h>
#include <rex/settings.h>

namespace rex {

PathSettings::PathSettings() : SettingOwner("app") {}

PathSettings& GetPathSettings() {
  static PathSettings& settings = [] -> PathSettings& {
    static PathSettings owner;
    (void)REX_SETTINGS().Register(owner);
    return owner;
  }();
  return settings;
}

}  // namespace rex
