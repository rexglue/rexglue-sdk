#include "base/process_control.h"

#include <rex/logging/logger.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

#include "base/settings/settings_internal.h"

namespace rex {

void ProcessSettingsControl::Freeze(SettingLife life) {
  if (life == SettingLife::kStartupOnly) {
    g_startup_frozen = true;
  }
  if (life == SettingLife::kUntilLaunch) {
    g_launch_frozen = true;
  }
}

void ProcessSettingsControl::Thaw(SettingLife life) {
  if (life == SettingLife::kUntilLaunch) {
    g_launch_frozen = false;
  }
}

void ProcessSettingsControl::ApplyEnvironment() {
  REX_SETTINGS().ApplyEnvironment();
}

std::vector<std::string> ProcessSettingsControl::TakeNotices() {
  return REX_SETTINGS().TakeNotices();
}

std::vector<UnknownSetting> ProcessSettingsControl::TakeUnknown() {
  return REX_SETTINGS().TakeUnknown();
}

void ProcessSettingsControl::EndSession() {
  REX_SETTINGS().EndSession();
}

void ProcessLoggerControl::ApplySettled() {
  REX_LOGGER().ApplySettled();
}

Result<void> ProcessLoggerControl::OpenFile(const std::filesystem::path& log_root,
                                            std::string_view app_name) {
  return REX_LOGGER().OpenFile(log_root, app_name);
}

void ProcessLoggerControl::EndSession() {
  REX_LOGGER().EndSession();
}

ProcessSettingsControl& GetProcessSettingsControl() {
  static ProcessSettingsControl control;
  return control;
}

ProcessLoggerControl& GetProcessLoggerControl() {
  static ProcessLoggerControl control;
  return control;
}

}  // namespace rex
