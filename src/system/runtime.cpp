/**
 * @file        runtime/runtime.cpp
 * @brief       Runtime subsystem implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>          // PPCFuncMapping
#include <rex/platform/exceptions.h>  // SEH exception support
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/user_module.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/platform/thread.h>

#include <rex/api.h>

namespace rex {

// Static instance for global access
Runtime* Runtime::instance_ = nullptr;

Runtime* Runtime::instance() {
  return instance_;
}

Runtime::Runtime(const Config& config)
    : config_(config),
      game_data_root_(config.paths.game_data_root),
      user_data_root_(config.paths.user_data_root.empty() ? config.paths.game_data_root
                                                          : config.paths.user_data_root),
      update_data_root_(config.paths.update_data_root),
      cache_root_(config.paths.cache_root),
      metadata_root_(config.paths.metadata_root) {}

Runtime::~Runtime() {
  Shutdown();
}

std::optional<std::filesystem::path> Runtime::FindMetadataPath(
    const std::filesystem::path& relative_path) const {
  if (!metadata_root_.empty()) {
    std::filesystem::path candidate = metadata_root_ / relative_path;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
    return std::nullopt;
  }

  const std::filesystem::path candidates[] = {
      game_data_root_ / "metadata" / relative_path,
      game_data_root_.parent_path() / "metadata" / relative_path,
      game_data_root_ / relative_path,
  };
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::optional<EmbeddedMetadataAsset> Runtime::FindEmbeddedMetadata(
    const std::filesystem::path& relative_path) const {
  return FindEmbeddedMetadataAsset(relative_path);
}

X_STATUS Runtime::Setup(RuntimeConfig config) {
  if (instance_ != nullptr) {
    REX_FAIL(system, X_STATUS_UNSUCCESSFUL,
             "Runtime::Setup() called but global instance already exists");
  }
  instance_ = this;

  auto fail = [this](X_STATUS status, std::string_view reason) {
    REXLOG_ERROR(system, "Runtime::Setup failed: {}", reason);
    Shutdown();
    return status;
  };

  // Initialize SEH exception support for hardware exception handling
  rex::initialize_seh();

  // Initialize clock
  Clock::set_guest_tick_frequency(50000000);
  Clock::set_guest_system_time_base(Clock::QueryHostSystemTime());
  Clock::set_guest_time_scalar(1.0);

  // Enable threading affinity configuration
  platform::EnableAffinityConfiguration();

  tool_mode_ = config.tool_mode;

  // Create memory system first
  memory_ = std::make_unique<memory::Memory>();
  if (!memory_->Initialize()) {
    return fail(X_STATUS_UNSUCCESSFUL, "memory init failed");
  }

  function_dispatcher_ = std::make_unique<runtime::FunctionDispatcher>(memory_.get());

  if (!config.file_system) {
    return fail(X_STATUS_INVALID_PARAMETER, "RuntimeConfig::file_system is null");
  }
  file_system_ = config.file_system;

  // Create kernel state - this sets the global singleton
  kernel_state_ = std::make_unique<system::KernelState>(this);

  // Initialize input from injected config
  if (config.input_factory) {
    input_system_ = config.input_factory(tool_mode_);
    if (input_system_) {
      X_STATUS input_status = input_system_->Setup();
      if (XFAILED(input_status)) {
        REXLOG_WARN(system, "Failed to initialize input system (status {:08X}) - input disabled",
                    input_status);
        input_system_.reset();
      } else {
        REXLOG_DEBUG(system, "Input system initialized");
      }
    }
  }

  // HLE kernel modules and apps.
  if (config.kernel_init) {
    config.kernel_init(this, kernel_state_.get());
  }

  // Initialize the APU (Audio Processing Unit) from injected config
  if (config.audio_factory) {
    audio_system_ = config.audio_factory(function_dispatcher_.get());
    if (audio_system_) {
      X_STATUS audio_status = audio_system_->Setup(kernel_state_.get());
      if (XFAILED(audio_status)) {
        REXLOG_WARN(system, "Failed to initialize audio system (status {:08X}) - audio disabled",
                    audio_status);
        audio_system_.reset();
      } else {
        REXLOG_DEBUG(system, "Audio system initialized");
      }
    }
  }

  if (game_data_root_.empty()) {
    REXLOG_WARN(system, "Runtime::Setup: no game_data_root, the standard mounts are skipped");
  } else {
    PathsConfig paths = config_.paths;
    paths.game_data_root = game_data_root_;
    paths.update_data_root = update_data_root_;
    if (auto mounted = file_system_->ConfigureStandardMounts(paths); !mounted) {
      return fail(X_STATUS_UNSUCCESSFUL, mounted.error().message);
    }
  }

  // Skip GPU initialization in tool mode (for analysis tools like codegen)
  if (tool_mode_) {
    REXLOG_DEBUG(system, "Runtime initialized in tool mode (no GPU)");
    setup_complete_ = true;
    return X_STATUS_SUCCESS;
  }

  // Initialize GPU from injected config
  if (config.graphics) {
    graphics_system_ = config.graphics;
    bool with_presentation = (app_context_ != nullptr);
    X_STATUS gpu_status = graphics_system_->Setup(function_dispatcher_.get(), kernel_state_.get(),
                                                  app_context_, with_presentation);
    if (XFAILED(gpu_status)) {
      return fail(gpu_status, "GPU setup failed");
    }
    REXLOG_DEBUG(system, "GPU system initialized (presentation={})", with_presentation);
  } else {
    REXLOG_DEBUG(system, "Runtime initialized without graphics system (native rendering mode)");
  }

  REXLOG_DEBUG(system, "Runtime initialized successfully");
  setup_complete_ = true;
  return X_STATUS_SUCCESS;
}

X_STATUS Runtime::Setup(const rex::PPCImageInfo& image_info, RuntimeConfig config) {
  X_STATUS status = Setup(std::move(config));
  if (status != X_STATUS_SUCCESS) {
    return status;
  }

  codegen_flags_ = image_info.codegen_flags;

  if (!function_dispatcher_->InitializeFunctionTable(image_info.code_base, image_info.code_size,
                                                     image_info.image_base, image_info.image_size,
                                                     /*is_entrypoint=*/true)) {
    REXLOG_ERROR(system, "Failed to initialize function table");
    Shutdown();
    return X_STATUS_UNSUCCESSFUL;
  }

  if (image_info.func_mappings) {
    int count = 0;
    int duplicates = 0;
    int rejected = 0;
    for (int i = 0; image_info.func_mappings[i].guest != 0; ++i) {
      uint32_t guest = static_cast<uint32_t>(image_info.func_mappings[i].guest);
      auto* host = image_info.func_mappings[i].host;
      if (!host) {
        continue;
      }
      if (function_dispatcher_->GetFunction(guest)) {
        REXLOG_WARN(system, "func_mappings: duplicate guest address {:08X}", guest);
        ++duplicates;
      }
      if (!function_dispatcher_->SetFunction(guest, host)) {
        ++rejected;
      } else {
        ++count;
      }
    }
    REXLOG_DEBUG(system, "Registered {} recompiled functions ({} duplicates, {} rejected)", count,
                 duplicates, rejected);
    if (rejected > 0) {
      REXLOG_ERROR(system, "PPCImageInfo registration: {} func_mappings entries rejected",
                   rejected);
      Shutdown();
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  REXLOG_DEBUG(system, "Runtime setup complete (code: {:08X}-{:08X}, image: {:08X}-{:08X})",
               image_info.code_base, image_info.code_base + image_info.code_size,
               image_info.image_base, image_info.image_base + image_info.image_size);

  const auto& flags = image_info.codegen_flags;
  REXLOG_DEBUG(system,
               "Codegen config: skip_lr={} skip_msr={} ctr={} xer={} cr={} reserved={} "
               "non_argument={} non_volatile={}",
               flags.skip_lr, flags.skip_msr, flags.ctr_as_local, flags.xer_as_local,
               flags.cr_as_local, flags.reserved_as_local, flags.non_argument_as_local,
               flags.non_volatile_as_local);

  return X_STATUS_SUCCESS;
}

void Runtime::Shutdown() {
  if (!instance_ && !setup_complete_ && !memory_) {
    return;
  }

  if (instance_ == this) {
    instance_ = nullptr;
  }

  if (graphics_system_) {
    graphics_system_->Shutdown();
    graphics_system_ = nullptr;
  }
  if (audio_system_) {
    audio_system_->Shutdown();
    audio_system_.reset();
  }
  if (input_system_) {
    input_system_->Shutdown();
    input_system_.reset();
  }
  kernel_state_.reset();
  function_dispatcher_.reset();
  if (file_system_) {
    file_system_->Clear();
    file_system_ = nullptr;
  }
  memory_.reset();

  setup_complete_ = false;
}

uint8_t* Runtime::virtual_membase() const {
  return memory_ ? memory_->virtual_membase() : nullptr;
}

X_STATUS Runtime::LoadXEXImage(const std::string_view module_path) {
  REXLOG_DEBUG(system, "Loading XEX image: {}", std::string(module_path));

  auto module = system::object_ref<system::UserModule>(new system::UserModule(kernel_state_.get()));
  X_STATUS status = module->LoadFromFile(module_path);
  if (XFAILED(status)) {
    REX_FAIL(system, status, "Runtime::LoadXEXImage: Failed to load module, status {:08X}", status);
  }

  kernel_state_->SetExecutableModule(module);
  REXLOG_DEBUG(system, "  XEX image loaded successfully");
  return X_STATUS_SUCCESS;
}

system::object_ref<system::XThread> Runtime::PrepareModuleLaunch() {
  auto executable = kernel_state_->GetExecutableModule();
  if (!executable) {
    REX_FAIL(system, nullptr, "Runtime::PrepareModuleLaunch: No executable module loaded");
  }

  auto thread = kernel_state_->PrepareModuleLaunch(executable);
  if (!thread) {
    REX_FAIL(system, nullptr, "Runtime::PrepareModuleLaunch: Failed to prepare module");
  }

  REXLOG_DEBUG(system, "  Module prepared on thread '{}'", thread->name());
  return thread;
}

system::object_ref<system::XThread> Runtime::LaunchModule() {
  auto thread = PrepareModuleLaunch();
  if (thread) {
    thread->Resume();
    REXLOG_DEBUG(system, "  Module launched on thread '{}'", thread->name());
  }
  return thread;
}

void Runtime::OnTitleExit(std::move_only_function<void(runtime::TitleExitReason)> callback) {
  std::lock_guard lock(title_exit_mutex_);
  title_exit_callback_ = std::move(callback);
}

void Runtime::ReportTitleExit(runtime::TitleExitReason reason) {
  std::lock_guard lock(title_exit_mutex_);
  if (title_exit_reported_) {
    return;
  }
  title_exit_reported_ = true;
  if (title_exit_callback_) {
    title_exit_callback_(reason);
  }
}

}  // namespace rex
