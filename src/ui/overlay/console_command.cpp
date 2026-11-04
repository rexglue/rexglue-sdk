#include "ui/overlay/console_command.h"

#include <string>

#include <rex/setting.h>
#include <rex/settings.h>
#include <rex/command.h>
#include <rex/logging/log.h>
#include <rex/string/text.h>

namespace rex::ui {

ConsoleCommandTable::ConsoleCommandTable(ISettings& settings)
    : SettingOwner("console"), settings_(settings) {}

Result<void> ConsoleCommandTable::Echo(std::string_view args) {
  REXCON_INFO("{}", args);
  return Ok();
}

Result<void> ConsoleCommandTable::Find(std::string_view args) {
  args = string::Trim(args);
  if (args.empty()) {
    REXCON_INFO("find: usage: find <substr>");
    return Ok();
  }
  const std::string needle(args);
  int matches = 0;
  for (const SettingBase* setting : settings_.Enumerate()) {
    if (setting->GetName().find(needle) != std::string::npos) {
      REXCON_INFO("  {}", setting->GetName());
      ++matches;
    }
  }
  for (const Command* command : settings_.EnumerateCommands()) {
    if (command->GetName().find(needle) != std::string::npos) {
      REXCON_INFO("  {}", command->GetName());
      ++matches;
    }
  }
  REXCON_INFO("find: {} match(es) for '{}'", matches, needle);
  return Ok();
}

}  // namespace rex::ui
