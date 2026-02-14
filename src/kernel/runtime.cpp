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

#include <rex/runtime.h>
#include <rex/runtime/guest/context.h>  // PPCFuncMapping
#include <rex/runtime/guest/exceptions.h>      // SEH exception support

#include <rex/time/clock.h>
#include <rex/runtime/export_resolver.h>
#include <rex/runtime/processor.h>
#include <rex/thread.h>
#include <rex/kernel/kernel_state.h>
#include <rex/kernel/user_module.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xboxkrnl/module.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/kernel/xmemory.h>
#include <rex/kernel/xthread.h>
#include <rex/logging.h>
#include <rex/filesystem/vfs.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/null_device.h>
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#include <rex/audio/audio_system.h>
#include <rex/audio/nop/nop_audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>

namespace rex {

// Static instance for global access
Runtime* Runtime::instance_ = nullptr;

Runtime::Runtime(const std::filesystem::path& storage_root,
                 const std::filesystem::path& content_root)
    : storage_root_(storage_root),
      content_root_(content_root.empty() ? storage_root : content_root) {}

Runtime::~Runtime() { Shutdown(); }

X_STATUS Runtime::Setup(bool tool_mode) {
  // Initialize SEH exception support for hardware exception handling
  runtime::guest::initialize();
  runtime::guest::initialize_thread();

  // Initialize clock
  chrono::Clock::set_guest_tick_frequency(50000000);
  chrono::Clock::set_guest_system_time_base(chrono::Clock::QueryHostSystemTime());
  chrono::Clock::set_guest_time_scalar(1.0);

  // Enable threading affinity configuration
  thread::EnableAffinityConfiguration();

  // Guard against reinitialization
  if (memory_) {
    REXKRNL_ERROR("Runtime::Setup() called but already initialized");
    return X_STATUS_UNSUCCESSFUL;
  }

  tool_mode_ = tool_mode;

  // Create memory system first
  memory_ = std::make_unique<memory::Memory>();
  if (!memory_->Initialize()) {
    REXKRNL_ERROR("Failed to initialize memory system");
    memory_.reset();
    return X_STATUS_UNSUCCESSFUL;
  }


  export_resolver_ = std::make_unique<runtime::ExportResolver>();

  processor_ = std::make_unique<runtime::Processor>(memory_.get(), export_resolver_.get());
  REXKRNL_INFO("Processor initialized");

  // Create virtual file system
  file_system_ = std::make_unique<rex::filesystem::VirtualFileSystem>();

  // Create kernel state - this sets the global singleton
  kernel_state_ = std::make_unique<kernel::KernelState>(this);

  // Initialize input drivers (must be after tool_mode_ is set)
  kernel_state_->SetupInputDrivers();

  // HLE kernel modules.
#define LOAD_KERNEL_MODULE(t) \
  static_cast<void>(kernel_state_->LoadKernelModule<kernel::t>())
  LOAD_KERNEL_MODULE(xboxkrnl::XboxkrnlModule);
  LOAD_KERNEL_MODULE(xam::XamModule);
#undef LOAD_KERNEL_MODULE

  // Initialize the APU (Audio Processing Unit)
  const char* audio_backend_name = nullptr;
  if (!tool_mode_) {
    audio_system_ = audio::sdl::SDLAudioSystem::Create(processor_.get());
    audio_backend_name = "SDL";
  } else {
    audio_system_ = audio::nop::NopAudioSystem::Create(processor_.get());
    audio_backend_name = "NOP (tool mode)";
  }
  if (audio_system_) {
    X_STATUS audio_status = audio_system_->Setup(kernel_state_.get());
    if (XFAILED(audio_status)) {
      REXKRNL_WARN("Failed to initialize audio system (status {:08X}) - audio disabled",
                  audio_status);
      audio_system_.reset();
    } else {
      REXKRNL_INFO("Audio system initialized ({} backend)", audio_backend_name);
    }
  }

  // Set up VFS based on content_root
  if (!SetupVfs()) {
    REXKRNL_ERROR("Failed to set up VFS");
    return X_STATUS_UNSUCCESSFUL;
  }

  // Skip GPU initialization in tool mode (for analysis tools like codegen)
  if (tool_mode_) {
    REXKRNL_INFO("Runtime initialized in tool mode (no GPU)");
    return X_STATUS_SUCCESS;
  }

  // Create GPU system - with presentation if app_context is set
  bool with_presentation = (app_context_ != nullptr);
#if REX_HAS_D3D12
  graphics_system_ = std::make_unique<graphics::d3d12::D3D12GraphicsSystem>();
  REXKRNL_INFO("Using D3D12 GPU backend");
#elif REX_HAS_VULKAN
  graphics_system_ = std::make_unique<graphics::vulkan::VulkanGraphicsSystem>();
  REXKRNL_INFO("Using Vulkan GPU backend");
#else
  #error "No graphics backend enabled"
#endif
  X_STATUS gpu_status = graphics_system_->Setup( processor_.get(), kernel_state_.get(),
                                                 app_context_,
                                                 with_presentation);
  if (XFAILED(gpu_status)) {
    REXKRNL_ERROR("Failed to initialize GPU - required for runtime");
    graphics_system_.reset();
    return gpu_status;
  }
  REXKRNL_INFO("GPU system initialized (presentation={})", with_presentation);

  REXKRNL_INFO("Runtime initialized successfully");
  return X_STATUS_SUCCESS;
}

X_STATUS Runtime::Setup(uint32_t code_base, uint32_t code_size,
                        uint32_t image_base, uint32_t image_size,
                        const PPCFuncMapping* func_mappings) {
  // Guard against multiple singleton instances
  if (instance_ != nullptr) {
    REXKRNL_ERROR("Runtime::Setup() called but global instance already exists");
    return X_STATUS_UNSUCCESSFUL;
  }

  // First perform the basic setup
  X_STATUS status = Setup();
  if (status != X_STATUS_SUCCESS) {
    return status;
  }

  // Initialize function table in Processor for recompiled code dispatch
  if (!processor_->InitializeFunctionTable(code_base, code_size, image_base,
                                           image_size)) {
    REXKRNL_ERROR("Failed to initialize function table");
    return X_STATUS_UNSUCCESSFUL;
  }

  // Register all recompiled functions from the mapping table
  if (func_mappings) {
    int count = 0;
    for (int i = 0; func_mappings[i].guest != 0; ++i) {
      if (func_mappings[i].host != nullptr) {
        processor_->SetFunction(static_cast<uint32_t>(func_mappings[i].guest),
                                func_mappings[i].host);
        ++count;
      }
    }
    REXKRNL_DEBUG("Registered {} recompiled functions", count);
  }

  // Set the global instance for recompiled code access
  instance_ = this;

  REXKRNL_DEBUG("Runtime setup for recompiled code complete (code: {:08X}-{:08X})",
         code_base, code_base + code_size);
  return X_STATUS_SUCCESS;
}

void Runtime::Shutdown() {
  // Clear global instance
  if (instance_ == this) {
    instance_ = nullptr;
  }

  // Destroy in reverse order
  if (graphics_system_) {
    graphics_system_->Shutdown();
    graphics_system_.reset();
  }
  if (audio_system_) {
    audio_system_->Shutdown();
    audio_system_.reset();
  }
  kernel_state_.reset();
  processor_.reset();
  export_resolver_.reset();
  file_system_.reset();
  memory_.reset();
}

uint8_t* Runtime::virtual_membase() const {
  return memory_ ? memory_->virtual_membase() : nullptr;
}

bool Runtime::SetupVfs() {
  if (content_root_.empty()) {
    REXKRNL_WARN("Runtime::SetupVfs: No content_root specified, skipping VFS setup");
    return true;
  }

  auto abs_content_root = std::filesystem::absolute(content_root_);
  if (!std::filesystem::exists(abs_content_root)) {
    REXKRNL_ERROR("Runtime::SetupVfs: content_root does not exist: {}",
           abs_content_root.string());
    return false;
  }

  // Mount content_root as \Device\Harddisk0\Partition1
  auto mount_path = "\\Device\\Harddisk0\\Partition1";
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(mount_path,
                                                       abs_content_root, true);
  if (!device->Initialize()) {
    REXKRNL_ERROR("Runtime::SetupVfs: Failed to initialize host path device");
    return false;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    REXKRNL_ERROR("Runtime::SetupVfs: Failed to register host path device");
    return false;
  }
  REXKRNL_INFO("  Mounted {} at {}", abs_content_root.string(), mount_path);

  // Register symbolic links for game: and D:
  file_system_->RegisterSymbolicLink("game:", mount_path);
  file_system_->RegisterSymbolicLink("d:", mount_path);
  REXKRNL_DEBUG("  Registered symbolic links: game:, d:");

  // Setup NullDevice for raw HDD partition accesses
  // Cache/STFC code baked into games tries reading/writing to these
  // Using a NullDevice returns success to all IO requests, allowing games
  // to believe cache/raw disk was accessed successfully.
  // NOTE: Must be registered AFTER Partition1 so Partition1 requests don't
  // go to NullDevice (VFS resolves devices in registration order)
  auto null_paths = {std::string("\\Partition0"), std::string("\\Cache0"),
                     std::string("\\Cache1")};
  auto null_device =
      std::make_unique<rex::filesystem::NullDevice>("\\Device\\Harddisk0", null_paths);
  if (null_device->Initialize()) {
    file_system_->RegisterDevice(std::move(null_device));
    REXKRNL_DEBUG("  Registered NullDevice for \\Device\\Harddisk0\\{{Partition0,Cache0,Cache1}}");
  }

  // NOTE: Do NOT register a device for cache: paths
  // Games handle "device not found" gracefully but don't handle actual device
  // errors (like NAME_COLLISION) well. Let cache: fail cleanly.

  return true;
}

X_STATUS Runtime::LoadXexImage(const std::string_view module_path) {
  REXKRNL_INFO("Loading XEX image: {}", std::string(module_path));

  auto module =
      kernel::object_ref<kernel::UserModule>(new kernel::UserModule(kernel_state_.get()));
  X_STATUS status = module->LoadFromFile(module_path);
  if (XFAILED(status)) {
    REXKRNL_ERROR("Runtime::LoadXexImage: Failed to load module, status {:08X}",
           status);
    return status;
  }

  kernel_state_->SetExecutableModule(module);
  REXKRNL_DEBUG("  XEX image loaded successfully");
  return X_STATUS_SUCCESS;
}

kernel::object_ref<kernel::XThread> Runtime::LaunchModule() {
  auto executable = kernel_state_->GetExecutableModule();
  if (!executable) {
    REXKRNL_ERROR("Runtime::LaunchModule: No executable module loaded");
    return nullptr;
  }

  auto thread = kernel_state_->LaunchModule(executable);
  if (!thread) {
    REXKRNL_ERROR("Runtime::LaunchModule: Failed to launch module");
    return nullptr;
  }

  REXKRNL_DEBUG("  Module launched on thread '{}'", thread->name());
  return thread;
}

}  // namespace rex
