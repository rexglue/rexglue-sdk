#include "tool_runtime.h"

#include <format>
#include <utility>

#include <rex/api.h>
#include <rex/filesystem.h>
#include <rex/kernel/init.h>
#include <rex/result.h>
#include <rex/runtime.h>
#include <rex/ui/user_interface.h>

namespace rex::codegen {

ToolRuntime::ToolRuntime(std::unique_ptr<Runtime> runtime, API* api)
    : runtime_(std::move(runtime)), api_(api) {}

ToolRuntime::ToolRuntime(ToolRuntime&& other) noexcept
    : runtime_(std::move(other.runtime_)), api_(std::exchange(other.api_, nullptr)) {}

ToolRuntime& ToolRuntime::operator=(ToolRuntime&& other) noexcept {
  if (this != &other) {
    Release();
    runtime_ = std::move(other.runtime_);
    api_ = std::exchange(other.api_, nullptr);
  }
  return *this;
}

ToolRuntime::~ToolRuntime() {
  Release();
}

void ToolRuntime::Release() {
  if (!runtime_) {
    return;
  }
  runtime_->Shutdown();
  runtime_.reset();
  api_ = nullptr;
  rex::Shutdown();
}

Result<ToolRuntime> OpenToolRuntime(const std::filesystem::path& game_root,
                                    std::span<const std::string> extra_modules,
                                    ui::IUserInterface* ui) {
  Config config;
  config.modules = {"rexfilesystem", "rexruntime"};
  config.modules.insert(config.modules.end(), extra_modules.begin(), extra_modules.end());
  if (!extra_modules.empty()) {
    config.ui.video_driver = "dummy";
  }
  config.paths.game_data_root = game_root;
  REX_TRY(API * api, rex::Init(config));
  auto* file_system = api->Get<filesystem::IFileSystem>();
  if (!file_system) {
    rex::Shutdown();
    return Err<ToolRuntime>(
        ErrorCategory::kModule,
        std::format("rexfilesystem does not serve {}@{}", filesystem::IFileSystem::kInterfaceName,
                    filesystem::IFileSystem::kInterfaceVersion));
  }
  auto runtime = std::make_unique<Runtime>(api->GetConfig());
  auto setup = runtime->Setup(RuntimeDesc{
      .file_system = file_system,
      .kernel_init = kernel::InitializeKernel,
      .tool_mode = true,
      .ui = ui != nullptr ? ui : api->Get<ui::IUserInterface>(),
  });
  if (!setup) {
    runtime.reset();
    rex::Shutdown();
    return Err<ToolRuntime>(ErrorCategory::kIO,
                            std::format("Failed to initialize Runtime: {}", setup.error().message));
  }
  return ToolRuntime(std::move(runtime), api);
}

}  // namespace rex::codegen
