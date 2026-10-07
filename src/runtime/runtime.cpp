/**
 * @file        runtime/runtime.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <bit>
#include <format>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/gpu/backend.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>  // PPCFuncMapping
#include <rex/platform/crash.h>
#include <rex/platform/exceptions.h>  // SEH exception support
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/user_module.h>
#include <rex/runtime/workspace_heap.h>
#include <rex/runtime/xam/app_manager.h>
#include <rex/runtime/xam/user_profile.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xthread.h>
#include <rex/platform/thread.h>

#include <rex/api.h>
#include <rex/apu.h>
#include <rex/hid.h>

#include "runtime/achievement_manager.h"
#include "runtime/apu.h"
#include "runtime/cvar_table.h"
#include "runtime/dispatcher.h"
#include "runtime/module_registry.h"
#include "runtime/notifier.h"
#include "runtime/null_gpu.h"
#include "runtime/object_table.h"
#include "runtime/runtime_module.h"
#include "runtime/thread_registry.h"
#include "runtime/xam/content_manager.h"

namespace rex {

namespace {

constexpr u32 kXNotificationSystemInputDevicesChanged = 0x00000012;

std::atomic<Runtime*> g_crash_runtime{nullptr};

std::string DescribeCrashAddress(u64 host_address) {
  constexpr u64 kGuestAddressSpace = u64{1} << 32;
  std::string text;
  Runtime* runtime = g_crash_runtime.load(std::memory_order_acquire);
  runtime::Memory* memory = runtime ? runtime->memory() : nullptr;
  if (memory && host_address) {
    const u64 base = reinterpret_cast<u64>(memory->virtual_membase());
    if (host_address >= base && host_address - base < kGuestAddressSpace) {
      text = std::format("guest address 0x{:08X}",
                         memory->HostToGuestVirtual(std::bit_cast<const void*>(host_address)));
    }
  }
  if (const runtime::ThreadRecord* thread = runtime::ThreadRecord::GetCurrentThread()) {
    text += std::format("{}guest thread {:08X} {}", text.empty() ? "" : ", ", thread->thread_id(),
                        thread->name());
  }
  return text;
}

rex::Error SetupError(X_STATUS status, std::string message) {
  return {ErrorCategory::Runtime, std::move(message), static_cast<int>(status)};
}

}  // namespace

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

Result<void> Runtime::Setup(const RuntimeConfig& config) {
  if (runtime::RuntimeModule::active() != nullptr) {
    return Err(SetupError(X_STATUS_UNSUCCESSFUL, "another runtime is active"));
  }
  constexpr u32 kMaxTitleMemoryMB = 512;
  if (config_.runtime.title_memory_mb > kMaxTitleMemoryMB) {
    return Err(SetupError(X_STATUS_INVALID_PARAMETER,
                          std::format("title_memory_mb {} is above {}",
                                      config_.runtime.title_memory_mb, kMaxTitleMemoryMB)));
  }

  auto fail = [this](X_STATUS status, std::string_view reason) -> Result<void> {
    Shutdown();
    return Err(SetupError(status, std::string(reason)));
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
  video_mode_ = config.video_mode;
  ui_ = config.ui;

  // Create memory system first
  memory_ = std::make_unique<runtime::Memory>();
  if (!memory_->Initialize()) {
    return fail(X_STATUS_UNSUCCESSFUL, "memory init failed");
  }
  memory_->SetBudget(config_.runtime.title_memory_mb);
  g_crash_runtime.store(this, std::memory_order_release);
  platform::SetCrashAddressDescriber(DescribeCrashAddress);

  function_dispatcher_ = std::make_unique<runtime::FunctionDispatcher>(memory_.get());

  if (!config.file_system) {
    return fail(X_STATUS_INVALID_PARAMETER, "RuntimeConfig::file_system is null");
  }
  file_system_ = config.file_system;

  CreateConcerns();

  hid_ = runtime::RuntimeModule::hid_backend();
  if (!hid_) {
    null_hid_ = std::make_unique<hid::NullBackend>();
    hid_ = null_hid_.get();
    REXLOG_INFO(hid, "no hid module is loaded, the title sees 1 idle pad on user 0");
  }
  hid_->SetConnectionListener([this](u32, bool) {
    notifier().BroadcastNotification(kXNotificationSystemInputDevicesChanged, 0);
  });

  // HLE kernel modules and apps.
  if (config.kernel_init) {
    config.kernel_init(*this);
  }

  if (!tool_mode_) {
    apu::IBackend* apu_backend = runtime::RuntimeModule::apu_backend();
    if (!apu_backend) {
      null_apu_ = std::make_unique<apu::NullBackend>();
      apu_backend = null_apu_.get();
      REXLOG_INFO(apu, "no apu module is loaded, audio plays to the null backend");
    }
    const auto queued = static_cast<u32>(REXCVAR_GET(runtime::cvars::max_queued_frames));
    apu_ = std::make_unique<runtime::APU>(*this, *apu_backend, queued);
    if (auto started = apu_->Setup(); !started) {
      REXLOG_WARN(apu, "the APU failed to start, audio is disabled: {}", started.error().message);
      apu_.reset();
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
    runtime::RuntimeModule::SetActive(this);
    return Ok();
  }

  // Initialize GPU from injected config
  gpu_ = config.gpu;
  if (!gpu_) {
    null_gpu_ = std::make_unique<runtime::NullGPU>();
    gpu_ = null_gpu_.get();
  }
  if (auto status = gpu_->SetupGuestGPU(*function_dispatcher_, *this); !status) {
    return fail(X_STATUS_UNSUCCESSFUL, std::format("GPU setup failed, {}", status.error().message));
  }
  REXLOG_DEBUG(system, "GPU initialized");

  REXLOG_DEBUG(system, "Runtime initialized successfully");
  setup_complete_ = true;
  runtime::RuntimeModule::SetActive(this);
  return Ok();
}

Result<void> Runtime::Setup(const rex::PPCImageInfo& image_info, const RuntimeConfig& config) {
  REX_TRY_VOID(Setup(config));

  codegen_flags_ = image_info.codegen_flags;

  if (!function_dispatcher_->InitializeFunctionTable(image_info.code_base, image_info.code_size,
                                                     image_info.image_base, image_info.image_size,
                                                     /*is_entrypoint=*/true)) {
    Shutdown();
    return Err(SetupError(X_STATUS_UNSUCCESSFUL, "the function table cannot be initialized"));
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
      Shutdown();
      return Err(SetupError(X_STATUS_UNSUCCESSFUL,
                            std::format("{} func_mappings entries were rejected", rejected)));
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

  return Ok();
}

void Runtime::Shutdown() {
  if (!setup_complete_ && !memory_) {
    return;
  }
  if (g_crash_runtime.load(std::memory_order_acquire) == this) {
    platform::SetCrashAddressDescriber(nullptr);
    g_crash_runtime.store(nullptr, std::memory_order_release);
  }
  if (gpu_) {
    gpu_->Shutdown();
    gpu_ = nullptr;
    null_gpu_.reset();
  }
  if (apu_) {
    apu_->Shutdown();
    apu_.reset();
  }
  null_apu_.reset();
  if (hid_) {
    hid_->SetConnectionListener({});
    hid_ = nullptr;
  }
  null_hid_.reset();
  if (runtime::RuntimeModule::active() == this) {
    runtime::RuntimeModule::SetActive(nullptr);
  }
  DestroyConcerns();
  function_dispatcher_.reset();
  if (file_system_) {
    file_system_->Clear();
    file_system_ = nullptr;
  }
  memory_.reset();
  setup_complete_ = false;
}

void Runtime::SetAudioPaused(bool paused) {
  if (apu_) {
    apu_->SetPaused(paused);
  }
}

bool Runtime::audio_paused() const {
  return apu_ && apu_->paused();
}

u8* Runtime::virtual_membase() const {
  return memory_ ? memory_->virtual_membase() : nullptr;
}

Result<void> Runtime::LoadXEXImage(const std::string_view module_path) {
  REXLOG_DEBUG(system, "Loading XEX image: {}", std::string(module_path));

  const X_HANDLE handle = runtime::CreateUserModule(*this);
  if (!handle) {
    return Err(SetupError(X_STATUS_NO_MEMORY, "no module object for the image"));
  }
  auto module = object_table_->Get<runtime::ModuleObject>(handle);
  X_STATUS status = runtime::UserModuleLoadFromFile(*this, *module, module_path);
  if (XFAILED(status)) {
    (void)object_table_->Close(handle);
    return Err(
        SetupError(status, std::format("cannot load {}, status {:08X}", module_path, status)));
  }

  SetExecutableModule(handle);
  REXLOG_DEBUG(system, "  XEX image loaded successfully");
  return Ok();
}

X_HANDLE Runtime::PrepareModuleLaunch() {
  const std::optional<X_HANDLE> executable = module_registry_->executable();
  if (!executable) {
    REX_FAIL(system, 0, "Runtime::PrepareModuleLaunch: No executable module loaded");
  }
  auto module = object_table_->Get<runtime::ModuleObject>(*executable);
  if (!module || !module->processor_module->is_executable()) {
    REX_FAIL(system, 0, "Runtime::PrepareModuleLaunch: Failed to prepare module");
  }
  SetExecutableModule(*executable);
  REXLOG_DEBUG(system, "Runtime: Preparing module launch...");
  auto record = std::make_unique<runtime::ThreadRecord>(*this, module->user.stack_size, 0,
                                                        module->user.entry_point, 0,
                                                        runtime::X_CREATE_SUSPENDED, true, true);
  record->set_name("Main XThread");
  Result<X_HANDLE> launched = thread_registry_->Launch(std::move(record));
  if (!launched) {
    REX_FAIL(system, 0, "Could not create launch thread: {:08X}", runtime::ToXStatus(launched));
  }
  if (auto thread = thread_registry_->FindByHandle(*launched)) {
    REXLOG_DEBUG(system, "  Module prepared on thread '{}'", thread->name());
  }
  return *launched;
}

runtime::IObjectTable& Runtime::object_table() {
  return *object_table_;
}

runtime::IThreadRegistry& Runtime::thread_registry() {
  return *thread_registry_;
}

runtime::WorkspaceHeap& Runtime::workspace() {
  return *workspace_;
}

runtime::IModuleRegistry& Runtime::module_registry() {
  return *module_registry_;
}

runtime::INotifier& Runtime::notifier() {
  return *notifier_;
}

runtime::IContentManager& Runtime::content_manager() {
  return *content_manager_;
}

runtime::IAchievementManager& Runtime::achievement_manager() {
  return *achievement_manager_;
}

runtime::IDispatcher& Runtime::dispatcher() {
  return *dispatcher_;
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
