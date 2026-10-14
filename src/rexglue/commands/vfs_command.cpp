#include "vfs_command.h"
#include "../ui/ui.h"

#include <array>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>

#include <CLI/CLI.hpp>
#include <format>

#include <rex/filesystem/container.h>

namespace rexglue::cli {

namespace {

namespace fs = rex::filesystem;

struct VFSDumpArgs {
  std::string source;
  std::string destination;
};

std::string KindName(fs::ContainerKind kind) {
  switch (kind) {
    case fs::ContainerKind::kSTFS:
      return "STFS";
    case fs::ContainerKind::kSVOD:
      return "SVOD";
    case fs::ContainerKind::kGDFX:
      return "GDFX";
  }
  return "unknown";
}

[[nodiscard]] rex::Result<void> Dump(const VFSDumpArgs& args) {
  auto container = fs::Container::Open(args.source);
  if (!container) {
    return rex::Err(container.error());
  }
  const auto nodes = container->GetNodes();
  const rex::u64 bytes = std::accumulate(
      nodes.begin(), nodes.end(), rex::u64{0},
      [](rex::u64 total, const fs::ContainerNode& node) { return total + node.size; });
  const std::array<ui::KeyValueRow, 4> rows = {{
      {"Source", args.source},
      {"Kind", KindName(container->GetKind())},
      {"Nodes", std::to_string(nodes.size())},
      {"Bytes", std::to_string(bytes)},
  }};
  ui::KeyValueBlock("Extracting container:", rows);
  return container->Extract(args.destination);
}

}  // namespace

void RegisterVFS(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending) {
  (void)ctx;
  auto* vfs = parent.add_subcommand("vfs", "Work with Xbox 360 container images")->fallthrough();
  vfs->require_subcommand(1);
  auto* dump =
      vfs->add_subcommand("dump", "Extract an STFS, SVOD or GDFX container")->fallthrough();
  auto args = std::make_shared<VFSDumpArgs>();
  dump->add_option("source", args->source, "Container file, or a folder that holds 1")
      ->type_name("PATH")
      ->required();
  dump->add_option("destination", args->destination, "Folder to extract into")
      ->type_name("PATH")
      ->required();
  dump->callback([args, &pending]() { pending = [args]() { return Dump(*args); }; });
}

}  // namespace rexglue::cli
