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
  REXLOG_INFO(console, "{}", args);
  return Ok();
}

Result<void> ConsoleCommandTable::Find(std::string_view args) {
  args = string::Trim(args);
  if (args.empty()) {
    REXLOG_INFO(console, "find: usage: find <substr>");
    return Ok();
  }
  const std::string needle(args);
  int matches = 0;
  for (const SettingBase* setting : settings_.Enumerate()) {
    if (setting->GetName().find(needle) != std::string::npos) {
      REXLOG_INFO(console, "  {}", setting->GetName());
      ++matches;
    }
  }
  for (const Command* command : settings_.EnumerateCommands()) {
    if (command->GetName().find(needle) != std::string::npos) {
      REXLOG_INFO(console, "  {}", command->GetName());
      ++matches;
    }
  }
  REXLOG_INFO(console, "find: {} match(es) for '{}'", matches, needle);
  return Ok();
}

}  // namespace rex::ui
