/**
 * @file        system/runtime_module.cpp
 * @brief       The module interface served by rexruntime.dll
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "system/runtime_module.h"

#include <format>
#include <string>
#include <string_view>

#include <rex/logging.h>
#include <rex/macros.h>

#include "audio/cvar_table.h"
#include "input/cvar_table.h"
#include "system/cvar_table.h"
#include "ui/cvar_table.h"

REXLOG_TABLE(kRuntimeLog, kernel, system, cpu, title, audio, input, ui, console)

namespace rex::runtime {

namespace {

struct ScopeTable {
  std::string_view scope;
  std::span<const cvar::IVar* const> vars;
};

const std::array<ScopeTable, 8> kTables = {{
    {"runtime", cvars::kRuntimeCvars},
    {"audio", audio::cvars::kAudioCvars},
    {"input", input::cvars::kInputCvars},
    {"ui", ui::cvars::kUICvars},
    {"d3d12", ui::cvars::kD3D12Cvars},
    {"vulkan", ui::cvars::kVulkanCvars},
    {"console", ui::cvars::kConsoleCvars},
    {"bind", {}},
}};

Result<void> ValidateSlices(const Config& config) {
  auto fail = [](std::string message) {
    REX_FAIL(system, Err(Error(ErrorCategory::Config, std::move(message))), "{}", message);
  };
  if (config.runtime.heap_size_mb < 1 || config.runtime.heap_size_mb > 2048) {
    return fail(std::format("config field runtime.heap_size_mb = {} is outside 1 to 2048",
                            config.runtime.heap_size_mb));
  }
  if (config.graphics.resolution_scale_x < 1 || config.graphics.resolution_scale_x > 8) {
    return fail(std::format("config field graphics.resolution_scale_x = {} is outside 1 to 8",
                            config.graphics.resolution_scale_x));
  }
  if (config.graphics.resolution_scale_y < 1 || config.graphics.resolution_scale_y > 8) {
    return fail(std::format("config field graphics.resolution_scale_y = {} is outside 1 to 8",
                            config.graphics.resolution_scale_y));
  }
  const auto& backend = config.graphics.backend;
  if (backend != "any" && backend != "d3d12" && backend != "vulkan") {
    return fail(
        std::format("config field graphics.backend = '{}' is not any, d3d12 or vulkan", backend));
  }
#if !REX_PLATFORM_WIN32
  if (config.input.backend == input::Backend::XInput) {
    return fail("config field input.backend = XInput is Windows only");
  }
#endif
  return Ok();
}

RuntimeModule& Instance() {
  static RuntimeModule instance;
  return instance;
}

}  // namespace

void* RuntimeModule::GetModule() {
  return static_cast<IModule*>(&Instance());
}

const char* RuntimeModule::Name() const {
  return "rexruntime";
}

std::span<const char* const> RuntimeModule::RequiredInterfaces() const {
  static constexpr std::array<const char*, 2> kRequired = {"rex.cvar@1", "rex.filesystem@1"};
  return kRequired;
}

Result<void> RuntimeModule::Connect(IInterfaceFactory* factory, const Config& config) {
  REX_TRY_VOID(ValidateSlices(config));
  cvars_ = factory->Get<cvar::ICvarRegistry>();
  if (!cvars_) {
    return Err(ErrorCategory::Module, "rexruntime: rex.cvar is not served");
  }
  registered_ = 0;
  for (const ScopeTable& table : kTables) {
    if (auto status = cvars_->Register(table.scope, table.vars); !status) {
      Disconnect();
      return status;
    }
    ++registered_;
  }
  logger_ = factory->Get<ILogger>();
  if (!logger_) {
    Disconnect();
    return Err(ErrorCategory::Module, "rexruntime: the logger is not served");
  }
  if (auto status = logger_->Register(log::kRuntimeLog); !status) {
    logger_ = nullptr;
    Disconnect();
    return status;
  }
  const auto& window = config.ui.window;
  auto defaults = [&]() -> Result<void> {
    REX_TRY_VOID(cvars_->SetDefault("runtime.writable_code_segments",
                                    config.runtime.writable_code_segments ? "true" : "false"));
    REX_TRY_VOID(
        cvars_->SetDefault("runtime.protect_zero", config.runtime.protect_zero ? "true" : "false"));
    REX_TRY_VOID(cvars_->SetDefault("ui.window_width", std::to_string(window.width)));
    REX_TRY_VOID(cvars_->SetDefault("ui.window_height", std::to_string(window.height)));
    REX_TRY_VOID(cvars_->SetDefault("ui.fullscreen", window.fullscreen ? "true" : "false"));
    REX_TRY_VOID(cvars_->SetDefault("ui.fullscreen_exclusive",
                                    window.fullscreen_exclusive ? "true" : "false"));
    REX_TRY_VOID(cvars_->SetDefault("ui.monitor", std::to_string(window.monitor)));
    return Ok();
  };
  if (auto status = defaults(); !status) {
    Disconnect();
    return status;
  }
  return Ok();
}

void RuntimeModule::Disconnect() {
  if (logger_) {
    logger_->Unregister(log::kRuntimeLog);
  }
  logger_ = nullptr;
  if (cvars_) {
    for (size_t i = registered_; i > 0; --i) {
      cvars_->Unregister(kTables[i - 1].vars);
    }
  }
  registered_ = 0;
  cvars_ = nullptr;
}

Result<void> RuntimeModule::Init() {
  return Ok();
}

void RuntimeModule::Shutdown() {}

}  // namespace rex::runtime

REX_DECLARE_MODULE(rex::runtime::RuntimeModule)
