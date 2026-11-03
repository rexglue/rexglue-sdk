#include "list_command.h"

#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <rex/kernel/shim.h>

namespace rexglue::cli {

namespace {

namespace shim = rex::kernel::shim;

struct ListShimOptions {
  std::string group;
  bool json = false;
};

[[nodiscard]] rex::Result<void> ListShim(const ListShimOptions& args) {
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& entry : shim::kRegistry) {
    const auto& traits = shim::TraitsOf(entry.group);
    if (!args.group.empty() && traits.name != args.group) {
      continue;
    }
    if (args.json) {
      rows.push_back({
          {"name", std::string(entry.name)},
          {"group", std::string(traits.name)},
          {"stub", rex::HasFlags(entry.flags, shim::Flag::kStub)},
          {"requires_heap", rex::HasFlags(entry.flags, shim::Flag::kRequiresHeap)},
          {"description", std::string(entry.description)},
      });
    } else {
      std::fputs(
          std::format("{:<24}{:<12}{}\n", entry.name, traits.name, entry.description).c_str(),
          stdout);
    }
  }
  if (args.json) {
    std::fputs((rows.dump(2) + "\n").c_str(), stdout);
  }
  return {};
}

}  // namespace

void RegisterList(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending) {
  (void)ctx;
  auto* list = parent.add_subcommand("list", "List what the SDK provides")->fallthrough();
  list->require_subcommand(1);
  auto* shim_command =
      list->add_subcommand("shim", "List the functions a title can replace with a shim")
          ->fallthrough();
  auto args = std::make_shared<ListShimOptions>();
  std::vector<std::string> groups;
  groups.reserve(std::size(shim::kGroups));
  for (const auto& traits : shim::kGroups) {
    groups.emplace_back(traits.name);
  }
  shim_command->add_option("--group", args->group, "Only list 1 group")
      ->type_name("NAME")
      ->check(CLI::IsMember(groups));
  shim_command->add_flag("--json", args->json, "Print the list as JSON");
  shim_command->callback([args, &pending]() { pending = [args]() { return ListShim(*args); }; });
}

}  // namespace rexglue::cli
