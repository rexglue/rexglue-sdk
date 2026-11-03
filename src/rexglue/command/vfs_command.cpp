#include "vfs_command.h"
#include "../ui/ui.h"

#include <array>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>

#include <CLI/CLI.hpp>
#include <format>

#include <rex/filesystem/file_system_factory.h>
#include <rex/filesystem/stfs.h>
#include <rex/module.h>
#include <rex/result.h>

#include "codegen/tool_runtime.h"

namespace rexglue::cli {

namespace {

namespace fs = rex::filesystem;

struct VFSDumpOptions {
  std::string source;
  std::string destination;
};

std::string TypeName(fs::ContainerType type) {
  switch (type) {
    case fs::ContainerType::kSTFS:
      return "STFS";
    case fs::ContainerType::kSVOD:
      return "SVOD";
    case fs::ContainerType::kGDFX:
      return "GDFX";
  }
  return "unknown";
}

[[nodiscard]] rex::Result<void> Dump(const VFSDumpOptions& args, rex::codegen::ToolApp& tools) {
  REX_TRY_VOID(tools.Start());
  fs::IFileSystemFactory* files = REX_INTERFACE(fs::IFileSystemFactory);
  if (!files) {
    return rex::Err<void>(rex::ErrorCategory::kModule, "the file system module is not running");
  }
  auto opened = files->OpenContainer(args.source);
  if (!opened) {
    return rex::Err(opened.error());
  }
  const std::unique_ptr<fs::IContainer> container = std::move(*opened);
  const auto nodes = container->GetNodes();
  const rex::u64 bytes = std::accumulate(
      nodes.begin(), nodes.end(), rex::u64{0},
      [](rex::u64 total, const fs::ContainerNode& node) { return total + node.info.size; });
  const std::array<ui::KeyValueRow, 4> rows = {{
      {"Source", args.source},
      {"Type", TypeName(container->GetType())},
      {"Nodes", std::to_string(nodes.size())},
      {"Bytes", std::to_string(bytes)},
  }};
  ui::KeyValueBlock("Extracting container:", rows);
  return container->Extract(args.destination);
}

}  // namespace

void RegisterVFS(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending) {
  auto* vfs = parent.add_subcommand("vfs", "Work with Xbox 360 container images")->fallthrough();
  vfs->require_subcommand(1);
  auto* dump =
      vfs->add_subcommand("dump", "Extract an STFS, SVOD or GDFX container")->fallthrough();
  auto args = std::make_shared<VFSDumpOptions>();
  dump->add_option("source", args->source, "Container file, or a folder that holds 1")
      ->type_name("PATH")
      ->required();
  dump->add_option("destination", args->destination, "Folder to extract into")
      ->type_name("PATH")
      ->required();
  dump->callback([args, &pending, tools = ctx.tools]() {
    pending = [args, tools]() { return Dump(*args, *tools); };
  });
}

}  // namespace rexglue::cli
