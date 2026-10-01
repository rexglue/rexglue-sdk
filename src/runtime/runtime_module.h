/**
 * @file        runtime/runtime_module.h
 * @brief       The module interface served by rexruntime.dll
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <span>

#include <rex/api.h>
#include <rex/cvar/registry.h>
#include <rex/module.h>
#include <rex/runtime.h>

namespace rex::runtime {

/**
 * The module rexruntime.dll serves. Connect registers the cvar tables of the subsystems rexruntime
 * holds and validates the runtime, input and graphics config slices.
 */
class RuntimeModule final : public IModule {
 public:
  /**
   * Returns the module instance for its interface table entry.
   *
   * @return The IModule of rexruntime.
   */
  static void* GetModule();

  /**
   * Sets the runtime whose interfaces the table serves.
   *
   * @param runtime The runtime, null when none is active.
   */
  static void SetActive(Runtime* runtime);

  /**
   * Returns the runtime whose interfaces the table serves.
   *
   * @return The runtime, null when none is active.
   */
  [[nodiscard]] static Runtime* active();

  static void* GetObjectTable();
  static void* GetThreadRegistry();
  static void* GetModuleRegistry();
  static void* GetNotifier();
  static void* GetContentManager();
  static void* GetAchievementManager();
  static void* GetDispatcher();

  /** The interfaces rexruntime.dll serves. The 7 runtime interfaces are null without a runtime. */
  static constexpr std::array<InterfaceEntry, 8> kInterfaces = {{
      {IModule::kInterfaceName, IModule::kInterfaceVersion, &GetModule},
      {IObjectTable::kInterfaceName, IObjectTable::kInterfaceVersion, &GetObjectTable},
      {IThreadRegistry::kInterfaceName, IThreadRegistry::kInterfaceVersion, &GetThreadRegistry},
      {IModuleRegistry::kInterfaceName, IModuleRegistry::kInterfaceVersion, &GetModuleRegistry},
      {INotifier::kInterfaceName, INotifier::kInterfaceVersion, &GetNotifier},
      {IContentManager::kInterfaceName, IContentManager::kInterfaceVersion, &GetContentManager},
      {IAchievementManager::kInterfaceName, IAchievementManager::kInterfaceVersion,
       &GetAchievementManager},
      {IDispatcher::kInterfaceName, IDispatcher::kInterfaceVersion, &GetDispatcher},
  }};

  /**
   * Returns the module name the loader resolves to rexruntime.dll.
   *
   * @return "rexruntime".
   */
  const char* Name() const override;

  /**
   * Lists the interfaces Connect fetches.
   *
   * @return The cvar registry and the mount table.
   */
  std::span<const char* const> RequiredInterfaces() const override;

  /**
   * Validates the config slices rexruntime reads and registers its cvar tables.
   *
   * @param factory Serves the cvar registry.
   * @param config The configuration rex::Init was called with.
   * @return Ok, or the first slice or registration error, with every table registered so far
   *         unregistered again.
   */
  Result<void> Connect(IInterfaceFactory* factory, const Config& config) override;

  /** Unregisters the cvar tables in reverse order. */
  void Disconnect() override;

  /**
   * Does nothing. The runtime is built by TitleApp::LaunchTitle.
   *
   * @return Ok.
   */
  Result<void> Init() override;

  /** Does nothing. */
  void Shutdown() override;

 private:
  cvar::ICvarRegistry* cvars_ = nullptr;
  ILogger* logger_ = nullptr;
  size_t registered_ = 0;
  std::atomic<Runtime*> runtime_{nullptr};
};

}  // namespace rex::runtime
