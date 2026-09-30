#include "list_command.h"

#include <memory>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rex/kernel/shim/registry.h>

namespace rexglue::cli {

namespace {

namespace shim = rex::kernel::shim;

struct ListShimArgs {
  std::string group;
  bool json = false;
};

rex::Result<void> ListShim(const ListShimArgs& args) {
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
          {"stub", rex::HasFlags(entry.flags, shim::Flag::Stub)},
          {"requires_heap", rex::HasFlags(entry.flags, shim::Flag::RequiresHeap)},
          {"description", std::string(entry.description)},
      });
    } else {
      fmt::print("{:<24}{:<12}{}\n", entry.name, traits.name, entry.description);
    }
  }
  if (args.json) {
    fmt::print("{}\n", rows.dump(2));
  }
  return {};
}

}  // namespace

void RegisterList(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending) {
  (void)ctx;
  auto* list = parent.add_subcommand("list", "List what the SDK provides")->fallthrough();
  list->require_subcommand(1);
  auto* shimCommand =
      list->add_subcommand("shim", "List the functions a title can replace with a shim")
          ->fallthrough();
  auto args = std::make_shared<ListShimArgs>();
  std::vector<std::string> groups;
  for (const auto& traits : shim::kGroups) {
    groups.emplace_back(traits.name);
  }
  shimCommand->add_option("--group", args->group, "Only list 1 group")
      ->type_name("NAME")
      ->check(CLI::IsMember(groups));
  shimCommand->add_flag("--json", args->json, "Print the list as JSON");
  shimCommand->callback([args, &pending]() { pending = [args]() { return ListShim(*args); }; });
}

}  // namespace rexglue::cli
