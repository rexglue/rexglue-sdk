/**
 * @file        runtime/runtime_module.h
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
#include <rex/apu/backend.h>
#include <rex/hid/backend.h>
#include <rex/cvar/registry.h>
#include <rex/logging/logger.h>
#include <rex/macros.h>
#include <rex/module.h>
#include <rex/runtime.h>

namespace rex::runtime {

/**
 * The module rex-runtime.dll serves. Connect registers the cvar tables of the subsystems
 * rex-runtime holds, reserves the apu and hid scopes, validates the runtime and apu config sections
 * and fetches the backends the host service modules serve.
 */
class REX_RUNTIME_API RuntimeModule final : public IModule {
 public:
  /**
   * Returns the module instance for its interface table entry.
   *
   * @return The IModule of rex-runtime.
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

  /**
   * Returns the audio output a loaded apu module serves.
   *
   * @return The backend, null when no apu module is loaded.
   */
  [[nodiscard]] static apu::IAPUBackend* apu_backend();

  /**
   * Returns the HID backend a loaded hid module serves.
   *
   * @return The backend, null when no hid module is loaded.
   */
  [[nodiscard]] static hid::IHIDBackend* hid_backend();

  static void* GetObjectTable();
  static void* GetThreadRegistry();
  static void* GetModuleRegistry();
  static void* GetNotifier();
  static void* GetContentManager();
  static void* GetAchievementManager();
  static void* GetDispatcher();

  /** The interfaces rex-runtime.dll serves. The 7 runtime interfaces are null without a runtime. */
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
   * Returns the module name the loader resolves to rex-runtime.dll.
   *
   * @return "rex-runtime".
   */
  const char* Name() const override;

  /**
   * Lists the interfaces Connect fetches.
   *
   * @return The cvar registry and the mount table.
   */
  std::span<const char* const> RequiredInterfaces() const override;

  /**
   * Validates the config sections rex-runtime reads and registers its cvar tables.
   *
   * @param factory Serves the cvar registry.
   * @param config The configuration rex::Init was called with.
   * @return Ok, or the first section or registration error, with every table registered so far
   *         unregistered again.
   */
  [[nodiscard]] Result<void> Connect(IInterfaceFactory* factory, const Config& config) override;

  /** Unregisters the cvar tables in reverse order. */
  void Disconnect() override;

  /**
   * Does nothing. The runtime is built by TitleApp::LaunchTitle.
   *
   * @return Ok.
   */
  [[nodiscard]] Result<void> Init() override;

  /** Does nothing. */
  void Shutdown() override;

 private:
  cvar::ICvarRegistry* cvars_ = nullptr;
  ILogger* logger_ = nullptr;
  size_t registered_ = 0;
  std::atomic<Runtime*> runtime_{nullptr};
  apu::IAPUBackend* apu_ = nullptr;
  hid::IHIDBackend* hid_ = nullptr;
};

}  // namespace rex::runtime
