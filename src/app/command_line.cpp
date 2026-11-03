#include "app/command_line.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <optional>
#include <string_view>

#include <rex/logging/log.h>
#include <rex/setting.h>
#include <rex/settings.h>

namespace rex::app::detail {

namespace {

struct Flag {
  std::string_view name;
  std::string_view setting;
};

constexpr std::array<Flag, 7> kFlags = {{
    {"modules", ""},
    {"game_data_root", "title.game_root"},
    {"user_data_root", "app.user_root"},
    {"update_data_root", "title.update_root"},
    {"cache_root", "app.cache_root"},
    {"metadata_root", "title.metadata_root"},
    {"config_file", "app.config_file"},
}};

bool IsNamePart(std::string_view part) {
  if (part.empty() || !std::islower(static_cast<unsigned char>(part.front()))) {
    return false;
  }
  return std::ranges::all_of(part, [](char c) {
    return std::islower(static_cast<unsigned char>(c)) ||
           std::isdigit(static_cast<unsigned char>(c)) || c == '_';
  });
}

bool IsSettingName(std::string_view name) {
  if (name.find('.') == std::string_view::npos) {
    return false;
  }
  for (size_t begin = 0; begin <= name.size();) {
    const size_t end = std::min(name.find('.', begin), name.size());
    if (!IsNamePart(name.substr(begin, end - begin))) {
      return false;
    }
    begin = end + 1;
  }
  return true;
}

std::string JoinOptions(const std::vector<std::string>& options) {
  std::string joined;
  for (const std::string& option : options) {
    joined += joined.empty() ? "'" + option + "'" : ", '" + option + "'";
  }
  return joined;
}

}  // namespace

Result<void> ParseCommandLine(const CLIArgList& args, std::span<const CLIAction> actions,
                              std::vector<CLIActionHit>* hits) {
  Settings& settings = REX_SETTINGS();
  std::vector<std::string> unknown;
  std::optional<Error> refused;
  const auto set = [&](std::string_view name, std::string_view text) {
    if (auto result = settings.SetText(SettingLayer::kCommandLine, name, text);
        !result && !refused) {
      refused = result.error();
    }
  };
  for (const CLIOption& option : args.GetOptions()) {
    if (auto action = std::ranges::find(actions, option.name, &CLIAction::name);
        action != actions.end()) {
      if (hits) {
        hits->push_back({*action, option});
      }
      continue;
    }
    const std::string text =
        option.value ? std::format("--{}={}", option.name, *option.value) : "--" + option.name;
    if (auto flag = std::ranges::find(kFlags, option.name, &Flag::name); flag != kFlags.end()) {
      if (!option.value) {
        unknown.push_back(text);
      } else if (flag->setting.empty()) {
        REXLOG_WARN(core, "--{} sets no setting, drivers are chosen by driver settings",
                    flag->name);
      } else {
        set(flag->setting, *option.value);
      }
      continue;
    }
    std::string_view name = option.name;
    const bool negated = name.starts_with("no-");
    if (negated) {
      name.remove_prefix(3);
    }
    if (!IsSettingName(name)) {
      unknown.push_back(text);
      continue;
    }
    if (negated) {
      if (auto found = settings.Find(name); found && (*found)->GetType() != SettingType::kBool) {
        if (!refused) {
          refused = Error(ErrorCategory::kConfig,
                          std::format("--no-{} needs a bool setting, {} is not one", name, name));
        }
        continue;
      }
      set(name, "false");
    } else {
      set(name, option.value.value_or("true"));
    }
  }
  if (refused) {
    return std::unexpected(std::move(*refused));
  }
  if (!unknown.empty()) {
    return Err(ErrorCategory::kConfig, "unknown option " + JoinOptions(unknown));
  }
  return Ok();
}

Result<void> ValidateCLIActions(std::span<const CLIAction> actions) {
  for (size_t i = 0; i < actions.size(); ++i) {
    const std::string_view name = actions[i].name;
    if (!rex::detail::IsActionName(name)) {
      return Err(ErrorCategory::kConfig,
                 std::format("action '{}' is not a valid action name", name));
    }
    if (std::ranges::find(kFlags, name, &Flag::name) != kFlags.end()) {
      return Err(ErrorCategory::kConfig,
                 std::format("action '{}' shares its name with a command line flag", name));
    }
    const auto rest = actions.subspan(i + 1);
    if (std::ranges::find(rest, name, &CLIAction::name) != rest.end()) {
      return Err(ErrorCategory::kConfig, std::format("action '{}' is declared twice", name));
    }
  }
  return Ok();
}

}  // namespace rex::app::detail
