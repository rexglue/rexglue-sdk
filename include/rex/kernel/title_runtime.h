/**
 * @file        kernel/title_runtime.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 * @remarks     Based on Xenia emulator.h/cc
 */

#pragma once

#include <memory>

#include <rex/platform.h>
#include <rex/utility.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/kernel/guest_memory.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/types/content.h>
#include <rex/types.h>
#include <rex/result.h>
#include <rex/kernel/title_exit.h>

struct PPCFuncMapping;

namespace rex {
namespace filesystem {
class IFileSystem;
class IXDBFTitleData;
}  // namespace filesystem
namespace ppc {
struct PPCImage;
}  // namespace ppc
}  // namespace rex

namespace rex::kernel {

class TitleRuntime;
class TitlePaths;
class RuntimeSettings;
class KernelSettings;
class TitleContext;

class APU;
class FunctionDispatcher;
class ObjectTable;
class ThreadRegistry;
class ModuleRegistry;
class NotificationBroadcaster;
class Dispatcher;
class WorkspaceHeap;
class GuestAddressSpace;
class ISystemHeap;
class ITitleWorkspace;
class IObjectTable;
class IThreadRegistry;
class IModuleRegistry;
class INotificationBroadcaster;
class IDPCQueue;
class IOverlappedDispatcher;
class TitleState;

}  // namespace rex::kernel

namespace rex::xam {
class ContentManager;
class AchievementManager;
class AppManager;
class UserProfile;
class IContentManager;
class IAchievementManager;
}  // namespace rex::xam

namespace rex::kernel {

/**
 * Load maps guest memory and reads the image. Launch attaches the title to the graphics, UI, input
 * and audio systems of an App and starts the main thread. Unload undoes both, so Load and Launch
 * may run again. Tools call Load only. Only one TitleRuntime is loaded at a time.
 *
 * The service accessors are valid from Load until Unload.
 */
class REX_KERNEL_API TitleRuntime final : public rex::NonCopyable {
 public:
  /**
   * Creates an unloaded runtime.
   *
   * @param image The recompiled image the codegen header declares.
   */
  explicit TitleRuntime(const ppc::PPCImage& image);

  /** Unloads the title when it is loaded. */
  ~TitleRuntime();

  /**
   * Maps guest memory and loads default.xex, its modules, and its content and achievement data.
   *
   * @return Ok, or an error. Nothing stays mapped on an error.
   */
  [[nodiscard]] Result<void> Load();

  /**
   * Attaches the title to the graphics, UI, input and audio systems of the app, freezes the
   * settings that cannot change after launch and starts the main thread.
   *
   * @return Ok, or an error. The runtime is unloaded after an error.
   */
  [[nodiscard]] Result<void> Launch();

  /**
   * Ends every guest thread, saves content and frees guest memory. Does nothing when not loaded.
   * After Launch it must run on the UI thread.
   */
  void Unload();

  /**
   * @return True between Load and Unload.
   */
  bool IsLoaded() const;

  /**
   * @return True between Launch and Unload.
   */
  bool IsRunning() const;

  /**
   * @return The ID from the image, 0 when not loaded.
   */
  u32 GetTitleID() const;

  /**
   * Returns the XDBF data of the executable, which reports invalid when the executable has no XDBF
   * section.
   */
  [[nodiscard]] std::unique_ptr<rex::filesystem::IXDBFTitleData> GetTitleXDBF() const;

  /** Returns the language the kernel reports. English before Load. */
  rex::kernel::XLanguage GetUserLanguage() const;

  /**
   * Parks or resumes the audio worker and the XMA decoder.
   *
   * @param paused True to park.
   */
  void SetAudioPaused(bool paused);

  bool IsAudioPaused() const;

  /**
   * Sets the function called on the thread that exited the title.
   *
   * @param callback Function receiving the reason. A new callback replaces the earlier one.
   * @return The guard. Destroying it removes the callback. It must not be destroyed from inside
   *         the callback.
   */
  [[nodiscard]] rex::ScopeGuard OnTitleExit(rex::MoveOnlyFunction<void(TitleExitReason)> callback);

  GuestMemory& GetGuestMemory() const;

  kernel::IModuleRegistry& GetModuleRegistry();

  rex::xam::IContentManager& GetContentManager();

  rex::xam::IAchievementManager& GetAchievementManager();

  kernel::IObjectTable& GetObjectTable();

  kernel::IThreadRegistry& GetThreadRegistry();

  kernel::ISystemHeap& GetGuestHeap();

  kernel::ITitleWorkspace& GetGuestWorkspace();

  /** Returns the host base of the title guest memory. It is valid from Load until Unload. */
  rex::memory::GuestMembase GetMembase();

  kernel::ITitleExit& GetTitleExit();

  kernel::INotificationBroadcaster& GetNotificationBroadcaster();

  filesystem::IFileSystem& GetFileSystem();

  /**
   * Finds the recompiled function registered at a guest address.
   *
   * @return The function, valid from Load until Unload, or kNotFound when none is registered at
   *         the address.
   */
  [[nodiscard]] Result<::PPCFunc*> FindFunction(u32 address) const;

  /**
   * @return The settings, scope "title".
   */
  TitlePaths& GetTitlePaths();

  /**
   * @return The settings, scope "runtime".
   */
  RuntimeSettings& GetRuntimeSettings();

  /**
   * @return The settings, scope "kernel".
   */
  KernelSettings& GetKernelSettings();

  friend TitleState& StateOf(const TitleRuntime& runtime);

 private:
  std::unique_ptr<kernel::TitleState> state_;
};

/**
 * Returns the runtime of the process, from any thread, null before Load completes and from the
 * first step of Shutdown.
 *
 * Valid only until Shutdown starts, because the call does not keep the runtime alive. Unlike
 * GetCurrent(), it needs no title or worker thread and never aborts.
 */
[[nodiscard]] REX_KERNEL_API TitleRuntime* GetProcessRuntime();

#define REX_RUNTIME() ::rex::kernel::GetProcessRuntime()

namespace detail {

[[nodiscard]] inline GuestMemory* GetProcessGuestMemory() {
  TitleRuntime* runtime = GetProcessRuntime();
  return runtime ? &runtime->GetGuestMemory() : nullptr;
}

}  // namespace detail

/** The guest memory of the process runtime, null when there is no runtime. */
#define REX_GUEST_MEMORY() ::rex::kernel::detail::GetProcessGuestMemory()

}  // namespace rex::kernel
