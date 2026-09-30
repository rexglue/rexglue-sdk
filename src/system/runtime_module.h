/**
 * @file        system/runtime_module.h
 * @brief       The rex.module@1 served by rexruntime.dll
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <span>

#include <rex/api.h>
#include <rex/cvar/registry.h>
#include <rex/module.h>

namespace rex::runtime {

/**
 * The module rexruntime.dll serves. Connect registers the cvar tables of the subsystems rexruntime
 * holds and validates the runtime, input and graphics config slices.
 */
class RuntimeModule final : public IModule {
 public:
  /**
   * Returns the module instance for the rex.module@1 table entry.
   *
   * @return The IModule of rexruntime.
   */
  static void* GetModule();

  /** The interfaces rexruntime.dll serves. */
  static constexpr std::array<InterfaceEntry, 1> kInterfaces = {{
      {IModule::kInterfaceName, IModule::kInterfaceVersion, &GetModule},
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
   * @return rex.cvar@1 and rex.filesystem@1.
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
  size_t registered_ = 0;
};

}  // namespace rex::runtime
