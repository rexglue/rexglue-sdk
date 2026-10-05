/**
 * @file        codegen/tool_runtime.cpp
 * @brief       A tool mode Runtime over the mount table rex-filesystem serves
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "tool_runtime.h"

#include <format>
#include <utility>

#include <rex/api.h>
#include <rex/filesystem.h>
#include <rex/kernel/init.h>
#include <rex/macros.h>
#include <rex/runtime.h>

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

Result<ToolRuntime> OpenToolRuntime(const std::filesystem::path& game_root) {
  Config config;
  config.modules = {"rex-filesystem", "rex-runtime"};
  config.paths.game_data_root = game_root;
  REX_TRY(API * api, rex::Init(config));
  auto* file_system = api->Get<filesystem::IFileSystem>();
  if (!file_system) {
    rex::Shutdown();
    return Err<ToolRuntime>(
        ErrorCategory::Module,
        std::format("rex-filesystem does not serve {}@{}", filesystem::IFileSystem::kInterfaceName,
                    filesystem::IFileSystem::kInterfaceVersion));
  }
  auto runtime = std::make_unique<Runtime>(api->config());
  auto setup = runtime->Setup(RuntimeConfig{
      .file_system = file_system,
      .kernel_init = kernel::InitializeKernel,
      .tool_mode = true,
  });
  if (!setup) {
    runtime.reset();
    rex::Shutdown();
    return Err<ToolRuntime>(ErrorCategory::IO,
                            std::format("Failed to initialize Runtime: {}", setup.error().message));
  }
  return ToolRuntime(std::move(runtime), api);
}

}  // namespace rex::codegen
