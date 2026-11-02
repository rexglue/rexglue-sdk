/**
 * @file        xam/title_lifecycle.h
 * @brief       Kernel interfaces and lifecycle calls XAM receives for a title
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <memory>

#include <rex/kernel/types/content.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/module.h>
#include <rex/result.h>
#include <rex/title.h>
#include <rex/types.h>

namespace rex {
namespace filesystem {
class IFileSystem;
class IFileSystemFactory;
class IXDBFTitleData;
}  // namespace filesystem
namespace hid {
class IInputSystem;
}  // namespace hid
namespace kernel {
class INotifier;
class IModuleRegistry;
class IObjectTable;
class IOverlappedDispatcher;
class ISystemHeap;
class ITitleDebugOutput;
class ITitleExit;
class ITitleWorkspace;
class IThreadRegistry;
}  // namespace kernel
namespace ui {
class IUISystem;
}  // namespace ui
}  // namespace rex

namespace rex::xam {

class IAchievementManager;
class IContentManager;

/** Kernel interfaces and values XAM reads for a title. Each outlives the title. */
struct XAMKernelInterfaces {
  kernel::IObjectTable& objects;
  kernel::IThreadRegistry& threads;
  kernel::IModuleRegistry& modules;
  kernel::INotifier& notifier;
  kernel::IOverlappedDispatcher& overlapped_dispatcher;
  kernel::ITitleExit& exit;
  kernel::ITitleDebugOutput& debug;
  kernel::ISystemHeap& heap;
  kernel::ITitleWorkspace& workspace;
  filesystem::IFileSystem& file_system;
  filesystem::IFileSystemFactory& file_system_factory; /**< Opens content packages and XDBF data. */
  rex::memory::GuestMembase membase;
  std::filesystem::path user_data_root; /**< Root of the per user data, empty when there is none. */
};

/** Title load and unload calls from the kernel to XAM. */
REX_DECLARE_INTERFACE(IXAMTitleLifecycle, 1)
  virtual ~IXAMTitleLifecycle() = default;

  /**
   * Creates the XAM state of a title, kept until DetachTitle.
   *
   * @param title The kernel interfaces of the title. Each outlives the state.
   * @return Ok, or the reason the state could not be created.
   */
  [[nodiscard]] virtual Result<void> AttachTitle(const XAMKernelInterfaces& title) = 0;

  /**
   * Connects the systems of a launch.
   *
   * @param ui The UI system, null for none.
   * @param input The input system, null for none.
   * @param video_mode The display mode the title was launched with.
   */
  virtual void AttachSystems(rex::ui::IUISystem* ui, rex::hid::IInputSystem* input,
                             const VideoMode& video_mode) = 0;

  virtual void SetTitleLanguage(rex::kernel::XLanguage language) = 0;

  /** Disconnects the systems AttachSystems connected. */
  virtual void DetachSystems() = 0;

  /** Shuts down every socket of the title so blocked threads return. */
  virtual void ShutdownSockets() = 0;

  /** Stops and destroys the XAM apps before the title threads stop. */
  virtual void DestroyApps() = 0;

  virtual void DetachTitle() = 0;
};

REX_DECLARE_INTERFACE(IXAMTitleServices, 1)
  virtual ~IXAMTitleServices() = default;

  /**
   * @return The manager, valid between AttachTitle and DetachTitle.
   */
  [[nodiscard]] virtual IContentManager& GetContentManager() = 0;

  /**
   * @return The manager, valid between AttachTitle and DetachTitle.
   */
  [[nodiscard]] virtual IAchievementManager& GetAchievementManager() = 0;

  /** Returns the title ID of the executable, 0 when the executable has no execution info. */
  [[nodiscard]] virtual u32 GetTitleID() const = 0;

  /**
   * Returns the XDBF data of the executable, which reports invalid when the executable has no XDBF
   * section.
   */
  [[nodiscard]] virtual std::unique_ptr<filesystem::IXDBFTitleData> GetTitleXDBF() const = 0;
};

}  // namespace rex::xam
