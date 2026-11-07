/**
 * @file        kernel/title_state.h
 * @brief       What a TitleRuntime owns and the operations run on it
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 * @remarks     Based on Xenia emulator.h/cc
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/ppc/image.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/types/thread.h>
#include <rex/hid/input_system.h>
#include <rex/module.h>
#include <rex/utility.h>
#include <rex/platform.h>
#include <rex/platform/lock_order.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/result.h>
#include <rex/filesystem/mount.h>
#include <rex/filesystem/file_system.h>
#include <rex/filesystem/xdbf.h>
#include <rex/kernel/guest_memory.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/kernel_settings.h>
#include <rex/kernel/title_exit.h>
#include <rex/kernel/title_debug_output.h>
#include <rex/kernel/types/content.h>
#include <rex/title.h>
#include <rex/types.h>

#include "kernel/shim/heap.h"

struct PPCFuncMapping;

namespace rex {

class ISettingsRegistryControl;

namespace apu {
class IAudioSystem;
}  // namespace apu
namespace gpu {
class IGraphicsControl;
}  // namespace gpu
namespace kernel {
class TitleContext;
}  // namespace kernel
namespace ui {
class IUIControl;
class IUISystem;
}  // namespace ui

namespace xam {
class IXAMTitleLifecycle;
class IXAMTitleServices;
class IAchievementManager;
class IContentManager;
}  // namespace xam

namespace filesystem {
class IFileSystem;
}  // namespace filesystem

namespace kernel {
class APU;
class Dispatcher;
class FunctionDispatcher;
class IDPCQueue;
class IOverlappedDispatcher;
class IModuleRegistry;
class INotificationBroadcaster;
class IObjectTable;
class IThreadRegistry;
class GuestAddressSpace;
class ModuleRegistry;
class NotificationBroadcaster;
class ObjectTable;
class ThreadRegistry;
class WorkspaceHeap;
}  // namespace kernel

namespace kernel {

/**
 * Everything one TitleRuntime owns, and the operations that load, run and stop the title. The
 * public TitleRuntime forwards its calls here. The subsystem getters are valid between Load and
 * Unload.
 */
class TitleState : public ITitleExit, public ITitleDebugOutput, public rex::NonCopyable {
 public:
  /**
   * Creates an unloaded state for a title image.
   *
   * @param owner The public object that owns this state.
   * @param image The recompiled image the codegen header declares.
   */
  TitleState(TitleRuntime& owner, const ppc::PPCImage& image);

  /** Unloads the title when it is loaded. */
  ~TitleState();

  /**
   * Maps guest memory, mounts the game, loads default.xex and its modules, and reads content and
   * achievement data.
   *
   * @return Ok, or an error. Nothing stays mapped on an error.
   */
  [[nodiscard]] Result<void> Load();

  /**
   * Attaches the title to the input, audio, graphics and UI systems, starts the APU and the main
   * thread, and freezes the settings that cannot change after launch.
   *
   * @return Ok, or an error. The runtime is unloaded after an error.
   */
  [[nodiscard]] Result<void> Launch();

  /**
   * Ends every guest thread, detaches the title, saves content and frees guest memory. After
   * Launch it must run on the UI thread, because detaching the UI system waits for that thread.
   */
  void Unload();

  /** Returns whether the title is loaded. It is true between Load and Unload. */
  bool IsLoaded() const;

  /** Returns whether the title is running. It is true between Launch and Unload. */
  bool IsRunning() const;

  /**
   * @return The ID from the image, 0 when not loaded.
   */
  u32 GetTitleID() const;
  std::unique_ptr<rex::filesystem::IXDBFTitleData> GetTitleXDBF() const;

  /**
   * Builds the facts about the title that systems read at attach.
   *
   * @return The video mode, and the title ID when an executable is loaded, else 0.
   */
  TitleInfo GetTitleInfo();

  /**
   * @return The memory, null before Load.
   */
  kernel::GuestAddressSpace* GetAddressSpace() const { return memory_.get(); }

  GuestMemory& GetGuestMemory() const { return *guest_memory_; }

  /**
   * @return The file system, null before Load.
   */
  filesystem::IFileSystem* GetFileSystem() const;

  /** Returns the runtime that is loaded in the process, null when none is loaded. */
  [[nodiscard]] static TitleRuntime* GetActive();

  /**
   * Starts the GPU reading commands from the ring buffer.
   *
   * @param address Physical address of the ring.
   * @param size_log2 Log2 of its size in quadwords.
   */
  void InitializeGPURingBuffer(u32 address, u32 size_log2);

  /**
   * Writes the ring's read pointer to guest memory as commands complete.
   *
   * @param address Physical address to write it to.
   * @param block_size_log2 Log2 of the quadwords between writes.
   */
  void EnableGPUReadPointerWriteBack(u32 address, u32 block_size_log2);

  /** Returns the APU the XAudio and XMA exports reach, null before Launch. */
  kernel::APU* GetAPU() const { return apu_.get(); }

  /** Parks or resumes the audio worker and the XMA decoder. */
  void SetAudioPaused(bool paused);

  bool IsAudioPaused() const;

  const VideoMode& GetVideoMode() const { return video_mode_; }

  kernel::FunctionDispatcher* GetFunctionDispatcher() const { return function_dispatcher_.get(); }

  /**
   * Finds a metadata file or directory.
   *
   * @param relative_path The path under the metadata root.
   * @return The host path, empty when nothing is found.
   */
  std::optional<std::filesystem::path> FindMetadataPath(
      const std::filesystem::path& relative_path) const;

  /** Returns the UI system dialogs show through, or null for a runtime built without one. */
  rex::ui::IUISystem* GetUI() const { return ui_; }

  /**
   * Returns the language the title sees. It resolves kernel.user_language, which follows the host
   * language for 0 and falls back to English for a value that is not an XLanguage. English before
   * Load.
   */
  rex::kernel::XLanguage GetUserLanguage() const { return user_language_; }

  const ppc::PPCCodegenFlags& GetCodegenFlags() const { return codegen_flags_; }

  /**
   * Finds the argument registers a recompiled function of the image reads at its entry.
   *
   * @param host The recompiled function, as the image maps it.
   * @return The registers read, or no value when the image maps no such function or lists none.
   */
  std::optional<ppc::PPCCallUsage> GetCallUsage(PPCFunc* host) const;

  /** Stops the subsystems and destroys the memory and every other part the runtime owns. */
  void Shutdown();

  /**
   * Loads an XEX image into guest memory and makes it the executable.
   *
   * @return Ok, or a Runtime error whose code is the status of the load.
   */
  [[nodiscard]] Result<void> LoadXEXImage(const std::string_view module_path);

  /**
   * Creates the suspended main thread of the loaded executable. The caller resumes it.
   *
   * @return The main thread handle with a count of 1 that the caller closes, 0 when it failed.
   */
  X_HANDLE PrepareModuleLaunch();

  /**
   * Reports that the title is about to run. A caller that resumes the main thread from
   * PrepareModuleLaunch itself must call it before the resume.
   */
  void ReportTitleLaunched();

  /**
   * Returns whether the title is running. It turns false when ReportTitleExit starts, before the
   * OnTitleExit callback runs, and in Shutdown. It is true between ReportTitleLaunched and the
   * exit. Safe from any thread.
   */
  [[nodiscard]] bool IsTitleLaunched() const;

  /**
   * Sets the function called on the thread that exited the title.
   *
   * @param callback Function receiving the reason.
   * @return A guard that removes the callback.
   */
  [[nodiscard]] rex::ScopeGuard OnTitleExit(rex::MoveOnlyFunction<void(TitleExitReason)> callback);

  /** Reports that the title exited. Only the first report reaches the callback. */
  void ReportTitleExit(TitleExitReason reason);

  kernel::IObjectTable& GetObjectTable();

  kernel::IThreadRegistry& GetThreadRegistry();

  kernel::WorkspaceHeap& GetWorkspace();

  kernel::IModuleRegistry& GetModuleRegistry();

  kernel::INotificationBroadcaster& GetNotificationBroadcaster();

  rex::xam::IContentManager& GetContentManager();

  rex::xam::IAchievementManager& GetAchievementManager();

  kernel::IOverlappedDispatcher& GetOverlappedDispatcher();
  kernel::IDPCQueue& GetDPCQueue();

  u32 GetKernelGlobals() const { return kernel_guest_globals_; }

  /**
   * Returns the guest address of the type object that a kernel object of a type points at.
   *
   * @param type The type of the kernel object.
   * @return The address of the exported type global, such as ExEventObjectType, or 0 when the
   *         type has none or the globals are not set up.
   */
  u32 GetObjectTypeAddress(ObjectType type) const;

  u32 GetTitleProcess() const {
    return kernel_guest_globals_ + offsetof(kernel::KernelGuestGlobals, title_process);
  }

  u32 GetSystemProcess() const {
    return kernel_guest_globals_ + offsetof(kernel::KernelGuestGlobals, system_process);
  }

  /**
   * @return The address, 0 before Load.
   */
  u32 GetProcessInfoBlockAddress() const { return kernel_guest_globals_ ? GetTitleProcess() : 0; }

  /** Sets the process type of the title process block. */
  void SetProcessType(u32 value);

  /** Returns the process type of the title process block. */
  u32 GetProcessType() const;

  /**
   * Runs the title terminate notifications, then stops every guest thread. Does not return when
   * called from a guest thread.
   */
  void TerminateTitle() override;

  /** Reports a title exit, then terminates the title from the calling thread. */
  void ExitTitle(TitleExitReason reason) override;

  /** Logs debug text the title printed, in the title category. */
  void WriteTitleDebugString(std::string_view text) override;

  bool IsTerminatingTitle() const { return terminating_title_.load(std::memory_order_acquire); }

  /**
   * Registers a title terminate notification.
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

  KernelSettings& GetKernelSettings() const;

  kernel::shim::GuestHeap heap;
  std::atomic<u32> fiber_count{0}; /**< Fibers created, for unique IDs. */
  std::atomic<u32> audio_submit_logs{0};

  [[nodiscard]] Result<void> AttachSystems(gpu::IGraphicsControl* graphics, rex::ui::IUISystem* ui,
                                           ui::IUIControl* ui_control,
                                           rex::hid::IInputSystem* input,
                                           rex::apu::IAudioSystem* audio,
                                           const VideoMode& video_mode);

 private:
  TitleRuntime& owner_;

  [[nodiscard]] Result<void> LoadKernel();
  [[nodiscard]] Result<void> InstallImage();
  void DetachSystems();

  void ResolveRoots();
  [[nodiscard]] Result<void> CreateSubsystems();
  void DestroySubsystems();

  void StopGuestThreads(std::chrono::milliseconds timeout, bool include_dispatch = false);

  void StopThreads();
  static u32 GraphicsReadThunk(void* context, u32 address);
  static void GraphicsWriteThunk(void* context, u32 address, u32 value);
  void InitializeProcess(kernel::X_KPROCESS* process, u32 process_type, u8 unk_18, u8 unk_19,
                         u8 unk_1a);
  void SetProcessTLSVars(kernel::X_KPROCESS* process, u32 num_slots, u32 tls_data_size,
                         u32 tls_raw_data_address);
  void SetExecutableModule(X_HANDLE module);
  void CreateWorkspace(u32 size);
  [[nodiscard]] Result<void> CreateXAM();
  void ReleaseXAMApps();
  void DestroyXAM();
  void LoadAchievements();

  /**
   * Reports whether a terminate notification runs past the timeout.
   *
   * @param timeout The longest wait.
   * @return True when one runs past it.
   */
  bool RunTitleTerminateNotifications(std::chrono::milliseconds timeout);

  std::mutex title_exit_mutex_;
  rex::MoveOnlyFunction<void(TitleExitReason)> title_exit_callback_;
  u64 title_exit_serial_ = 0;
  bool title_exit_reported_ = false;
  std::atomic<bool> title_launched_{false};

  ppc::PPCImage image_{};
  bool has_image_ = false;
  bool running_ = false;
  std::jthread monitor_;
  rex::ui::IUISystem* attached_ui_ = nullptr;
  std::filesystem::path game_data_root_;
  filesystem::MountFlags game_flags_ = filesystem::MountFlags::kNone;
  std::filesystem::path user_data_root_;
  std::filesystem::path update_data_root_;
  std::filesystem::path metadata_root_;

  rex::ui::IUISystem* ui_ = nullptr;
  ui::IUIControl* ui_control_ = nullptr;
  rex::kernel::XLanguage user_language_ = rex::kernel::XLanguage::kEnglish;
  bool tool_mode_ = false;
  VideoMode video_mode_;
  ppc::PPCCodegenFlags codegen_flags_{};
  const PPCFuncMapping* func_mappings_ = nullptr;
  const ppc::PPCCallUsage* call_usage_ = nullptr;
  bool setup_complete_ = false;
  bool attached_ = false;

  std::unique_ptr<kernel::GuestAddressSpace> memory_;
  GuestMemory* guest_memory_ = nullptr;
  std::unique_ptr<kernel::FunctionDispatcher> function_dispatcher_;
  std::unique_ptr<filesystem::IFileSystem> file_system_;
  ISettingsRegistryControl* settings_control_ = nullptr;
  std::unique_ptr<kernel::ObjectTable> object_table_;
  std::unique_ptr<kernel::ThreadRegistry> thread_registry_;
  std::unique_ptr<kernel::ModuleRegistry> module_registry_;
  std::unique_ptr<kernel::NotificationBroadcaster> notifier_;
  rex::xam::IXAMTitleLifecycle* xam_ = nullptr;
  rex::xam::IXAMTitleServices* xam_managers_ = nullptr;
  std::unique_ptr<TitleContext> title_context_;
  std::unique_ptr<kernel::Dispatcher> dispatcher_;
  std::unique_ptr<kernel::WorkspaceHeap> workspace_;
  u32 kernel_guest_globals_ = 0;
  std::atomic<bool> terminating_title_{false};
  std::atomic<bool> terminate_claimed_{false};
  rex::platform::RankedMutex terminate_mutex_{rex::platform::LockRank::kLeaf};
  std::vector<u32> terminate_registrations_;
  gpu::IGraphicsControl* graphics_ = nullptr;
  bool graphics_attached_ = false;
  std::unique_ptr<kernel::APU> apu_;
};

/** Returns the kernel instance of a runtime, for the kernel, XAM and tests. */
TitleState& StateOf(const TitleRuntime& runtime);

}  // namespace kernel
}  // namespace rex
