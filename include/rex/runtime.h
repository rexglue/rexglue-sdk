/**
 * @file        runtime.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 * @remarks     Based on Xenia emulator.h/cc
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/embedded_metadata.h>
#include <rex/filesystem.h>
#include <rex/image_info.h>
#include <rex/macros.h>
#include <rex/memory.h>
#include <rex/platform/lock_order.h>
#include <rex/runtime/achievement_manager.h>
#include <rex/runtime/config.h>
#include <rex/runtime/content_manager.h>
#include <rex/runtime/dispatcher.h>
#include <rex/runtime/error.h>
#include <rex/runtime/module_registry.h>
#include <rex/runtime/notifier.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/process.h>
#include <rex/runtime/thread_registry.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>
#include <rex/ui/user_interface.h>
#include <rex/types.h>

struct PPCFuncMapping;

namespace rex {

struct Config;

namespace apu {
class IBackend;
class NullBackend;
}  // namespace apu

namespace hid {
class IBackend;
class NullBackend;
}  // namespace hid

namespace gpu {
class IBackend;
}  // namespace gpu

namespace runtime {
class APU;
class NullGPU;
class FunctionDispatcher;
class ObjectTable;
class ThreadRegistry;
class ModuleRegistry;
class Notifier;
class ContentManager;
class AchievementManager;
class Dispatcher;
class WorkspaceHeap;
namespace xam {
class AppManager;
class UserProfile;
}  // namespace xam

/** The display mode the title queries through VdQueryVideoMode and XGetVideoMode. */
using VideoMode = ui::VideoMode;
}  // namespace runtime

/** The subsystems Runtime::Setup builds the runtime over. */
struct RuntimeConfig {
  /** The GPU backend, owned by its module. Null runs on the runtime's null GPU. */
  gpu::IBackend* gpu = nullptr;
  /** The mount table. Required, Setup fails without it. */
  filesystem::IFileSystem* file_system = nullptr;
  /** Registers the Xam apps and the kernel modules. */
  std::function<void(Runtime&)> kernel_init;
  /** True to skip the GPU, for analysis tools. */
  bool tool_mode = false;
  /** The display mode the title queries. TitleApp fills it from the user interface. */
  runtime::VideoMode video_mode = {};
  /** The user interface dialogs show through. TitleApp sets it, analysis tools leave it null. */
  ui::IUserInterface* ui = nullptr;
};

/**
 * The runtime of a recompiled title. Owns the guest memory, the function dispatcher, the object
 * table, the thread and module registries, the notifier, the content and achievement managers,
 * the dispatcher, the process blocks, the APU and the HID backend.
 */
class REX_RUNTIME_API Runtime : public rex::NonCopyable {
 public:
  /**
   * @param config The configuration Init copied, read through API::config().
   */
  explicit Runtime(const Config& config);
  ~Runtime();

  /**
   * Returns the configuration the runtime was constructed with.
   *
   * @return The configuration.
   */
  const Config& config() const { return config_; }

  /**
   * Returns the guest memory.
   *
   * @return The memory, null before Setup.
   */
  runtime::Memory* memory() const { return memory_.get(); }

  /**
   * Returns the mount table.
   *
   * @return The file system, null before Setup.
   */
  filesystem::IFileSystem* file_system() const { return file_system_; }

  /**
   * Returns the GPU backend, the module's or the runtime's null GPU.
   *
   * @return The backend, null in tool mode.
   */
  gpu::IBackend* gpu() const { return gpu_; }

  /**
   * Returns the APU the XAudio and XMA exports reach.
   *
   * @return The APU, null in tool mode and before Setup.
   */
  runtime::APU* apu() const { return apu_.get(); }

  /**
   * Parks or resumes the audio worker and the XMA decoder.
   *
   * @param paused True to park.
   */
  void SetAudioPaused(bool paused);

  /**
   * Reports whether audio is parked.
   *
   * @return True while SetAudioPaused(true) holds.
   */
  bool audio_paused() const;

  /**
   * Returns the display mode Setup took from RuntimeConfig.
   *
   * @return The video mode.
   */
  const runtime::VideoMode& video_mode() const { return video_mode_; }

  /**
   * Returns the HID backend the XamInput exports reach, a loaded hid module or the runtime's
   * null provider.
   *
   * @return The backend. Valid between Setup and Shutdown.
   */
  hid::IBackend& hid() const { return *hid_; }

  /**
   * Returns the function dispatcher that runs guest functions and interrupts.
   *
   * @return The dispatcher, null before Setup.
   */
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_.get(); }

  /**
   * Returns the game data root.
   *
   * @return The path.
   */
  const std::filesystem::path& game_data_root() const { return game_data_root_; }

  /**
   * Returns the user data root.
   *
   * @return The path.
   */
  const std::filesystem::path& user_data_root() const { return user_data_root_; }

  /**
   * Returns the update data root.
   *
   * @return The path.
   */
  const std::filesystem::path& update_data_root() const { return update_data_root_; }

  /**
   * Returns the cache root.
   *
   * @return The path.
   */
  const std::filesystem::path& cache_root() const { return cache_root_; }

  /**
   * Returns the metadata root.
   *
   * @return The path.
   */
  const std::filesystem::path& metadata_root() const { return metadata_root_; }

  /**
   * Finds a metadata file or directory.
   *
   * @param relative_path The path under the metadata root.
   * @return The host path, empty when nothing is found.
   */
  std::optional<std::filesystem::path> FindMetadataPath(
      const std::filesystem::path& relative_path) const;

  /**
   * Finds a metadata asset embedded in the executable.
   *
   * @param relative_path The path under the metadata root.
   * @return The asset, empty when none is embedded.
   */
  std::optional<EmbeddedMetadataAsset> FindEmbeddedMetadata(
      const std::filesystem::path& relative_path) const;

  /**
   * Returns the user interface dialogs show through.
   *
   * @return The interface, or null for a runtime built without one.
   */
  ui::IUserInterface* ui() const { return ui_; }

  /**
   * Builds the memory, the function dispatcher, the object table, the registries, the managers and
   * the subsystems.
   *
   * @param config The subsystems to create. tool_mode skips the GPU.
   * @return Ok, or a Runtime error whose code is the status of the step that failed.
   */
  [[nodiscard]] Result<void> Setup(const RuntimeConfig& config = {});

  /**
   * Builds the runtime and the function dispatch table of a recompiled image.
   *
   * @param image_info The image the code was recompiled from.
   * @param config The subsystems to create.
   * @return Ok, or a Runtime error whose code is the status of the step that failed.
   */
  [[nodiscard]] Result<void> Setup(const PPCImageInfo& image_info,
                                   const RuntimeConfig& config = {});

  /**
   * Returns the codegen flags of the recompiled image.
   *
   * @return The flags.
   */
  const PPCCodegenFlags& codegen_flags() const { return codegen_flags_; }

  /**
   * Stops the subsystems and destroys the object table, the registries, the managers, the
   * dispatcher and the memory.
   */
  void Shutdown();

  /**
   * Loads an XEX image into guest memory and makes it the executable.
   *
   * @param module_path The guest path of the image.
   * @return Ok, or a Runtime error whose code is the status of the load.
   */
  [[nodiscard]] Result<void> LoadXEXImage(const std::string_view module_path);

  /**
   * Creates the suspended main thread of the loaded executable. The caller resumes it after any
   * pre launch hooks.
   *
   * @return The main thread handle with a count of 1 that the caller closes, 0 when it failed.
   */
  X_HANDLE PrepareModuleLaunch();

  /**
   * Returns the host base of guest virtual memory for recompiled code.
   *
   * @return The host address of guest address 0.
   */
  u8* virtual_membase() const;

  /**
   * Sets the function called on the thread that exited the title.
   *
   * @param callback Function receiving the reason.
   */
  void OnTitleExit(std::move_only_function<void(runtime::TitleExitReason)> callback);

  /**
   * Reports that the title exited. Only the first report reaches the callback.
   *
   * @param reason Why the title exited.
   */
  void ReportTitleExit(runtime::TitleExitReason reason);

  /**
   * Returns the object table that maps guest handles to kernel objects.
   *
   * @return The table. Valid between Setup and Shutdown.
   */
  runtime::IObjectTable& object_table();

  /**
   * Returns the thread registry.
   *
   * @return The registry. Valid between Setup and Shutdown.
   */
  runtime::IThreadRegistry& thread_registry();

  /**
   * Returns the title workspace.
   *
   * @return The workspace. Valid between Setup and Shutdown.
   */
  runtime::WorkspaceHeap& workspace();

  /**
   * Returns the module registry.
   *
   * @return The registry. Valid between Setup and Shutdown.
   */
  runtime::IModuleRegistry& module_registry();

  /**
   * Returns the notifier.
   *
   * @return The notifier. Valid between Setup and Shutdown.
   */
  runtime::INotifier& notifier();

  /**
   * Returns the content manager.
   *
   * @return The content manager. Valid between Setup and Shutdown.
   */
  runtime::IContentManager& content_manager();

  /**
   * Returns the achievement manager.
   *
   * @return The achievement manager. Valid between Setup and Shutdown.
   */
  runtime::IAchievementManager& achievement_manager();

  /**
   * Returns the dispatcher.
   *
   * @return The dispatcher. Valid between Setup and Shutdown.
   */
  runtime::IDispatcher& dispatcher();

  /**
   * Returns the guest address of the kernel globals.
   *
   * @return The address.
   */
  u32 GetKernelGuestGlobals() const { return kernel_guest_globals_; }

  /**
   * Returns the guest address of the title process block.
   *
   * @return The address.
   */
  u32 GetTitleProcess() const {
    return kernel_guest_globals_ + offsetof(runtime::KernelGuestGlobals, title_process);
  }

  /**
   * Returns the guest address of the system process block.
   *
   * @return The address.
   */
  u32 GetSystemProcess() const {
    return kernel_guest_globals_ + offsetof(runtime::KernelGuestGlobals, system_process);
  }

  /**
   * Returns the guest address of the title process block, 0 before Setup.
   *
   * @return The address.
   */
  u32 process_info_block_address() const { return kernel_guest_globals_ ? GetTitleProcess() : 0; }

  /**
   * Sets the process type of the title process block.
   *
   * @param value An X_PROCTYPE value.
   */
  void set_process_type(u32 value);

  /**
   * Returns the process type of the title process block.
   *
   * @return An X_PROCTYPE value.
   */
  u32 process_type() const;

  /**
   * Terminates the title. Runs the title terminate notifications, then wakes and stops every guest
   * thread. Does not return when called from a guest thread.
   */
  void TerminateTitle();

  /**
   * Reports a title exit, then terminates the title from the calling thread.
   *
   * @param reason Why the title exited.
   */
  void ExitTitle(runtime::TitleExitReason reason);

  /**
   * Reports whether Setup succeeded and Shutdown has not run since.
   *
   * @return True when the runtime owns its memory, interfaces and subsystems.
   */
  bool is_setup() const { return setup_complete_; }

  /**
   * Reports whether TerminateTitle runs.
   *
   * @return True from the start of TerminateTitle.
   */
  bool is_terminating_title() const { return terminating_title_.load(std::memory_order_acquire); }

  /**
   * Registers a title terminate notification. TerminateTitle calls it with its registration.
   *
   * @param registration Address of the title's EX_TITLE_TERMINATE_REGISTRATION.
   */
  void RegisterTitleTerminateNotification(u32 registration);

  /**
   * Removes a title terminate notification.
   *
   * @param registration Address of the title's EX_TITLE_TERMINATE_REGISTRATION.
   */
  void RemoveTitleTerminateNotification(u32 registration);

 private:
  void CreateConcerns();
  void DestroyConcerns();

  /**
   * Stops every guest thread except the caller and waits for them.
   *
   * @param timeout The time each thread has to stop before it is forced.
   */
  void StopGuestThreads(std::chrono::milliseconds timeout);

  /** Stops the guest threads, then the dispatcher and the audio worker. */
  void StopThreads();
  void InitializeProcess(runtime::X_KPROCESS* process, u32 process_type, u8 unk_18, u8 unk_19,
                         u8 unk_1A);
  void SetProcessTLSVars(runtime::X_KPROCESS* process, u32 num_slots, u32 tls_data_size,
                         u32 tls_raw_data_address);
  void SetExecutableModule(X_HANDLE module);
  void CreateWorkspace(u32 size);
  void LoadAchievementsData();
  void RunTerminateNotifications(std::chrono::milliseconds timeout);

  std::mutex title_exit_mutex_;
  std::move_only_function<void(runtime::TitleExitReason)> title_exit_callback_;
  bool title_exit_reported_ = false;

  const Config& config_;
  std::filesystem::path game_data_root_;
  std::filesystem::path user_data_root_;
  std::filesystem::path update_data_root_;
  std::filesystem::path cache_root_;
  std::filesystem::path metadata_root_;

  ui::IUserInterface* ui_ = nullptr;
  bool tool_mode_ = false;
  runtime::VideoMode video_mode_;
  PPCCodegenFlags codegen_flags_{};
  bool setup_complete_ = false;

  std::unique_ptr<runtime::Memory> memory_;
  std::unique_ptr<runtime::FunctionDispatcher> function_dispatcher_;
  filesystem::IFileSystem* file_system_ = nullptr;
  std::unique_ptr<runtime::xam::AppManager> app_manager_;
  std::unique_ptr<runtime::xam::UserProfile> user_profile_;
  std::unique_ptr<runtime::ObjectTable> object_table_;
  std::unique_ptr<runtime::ThreadRegistry> thread_registry_;
  std::unique_ptr<runtime::ModuleRegistry> module_registry_;
  std::unique_ptr<runtime::Notifier> notifier_;
  std::unique_ptr<runtime::ContentManager> content_manager_;
  std::unique_ptr<runtime::AchievementManager> achievement_manager_;
  std::unique_ptr<runtime::Dispatcher> dispatcher_;
  std::unique_ptr<runtime::WorkspaceHeap> workspace_;
  u32 kernel_guest_globals_ = 0;
  std::atomic<bool> terminating_title_{false};
  std::atomic<bool> terminate_claimed_{false};
  rex::platform::RankedMutex terminate_mutex_{rex::platform::LockRank::Leaf};
  std::vector<u32> terminate_registrations_;
  gpu::IBackend* gpu_ = nullptr;
  std::unique_ptr<runtime::NullGPU> null_gpu_;
  std::unique_ptr<apu::NullBackend> null_apu_;
  std::unique_ptr<runtime::APU> apu_;
  std::unique_ptr<hid::NullBackend> null_hid_;
  hid::IBackend* hid_ = nullptr;
};

}  // namespace rex
