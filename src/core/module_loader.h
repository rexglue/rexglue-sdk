/**
 * @file        core/module_loader.h
 * @brief       Internal module loader and interface registry behind rex::Init
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/macros.h>
#include <rex/api.h>
#include <rex/module.h>
#include <rex/platform/dynlib.h>
#include <rex/types.h>

#include "core/cvar_registry.h"

namespace rex::detail {

/**
 * Tests whether a module name is safe to resolve to a file.
 *
 * @param name The module name from Config::modules.
 * @return True when the name has no path separator, no drive colon and no ".." segment.
 */
REX_API bool IsValidModuleName(std::string_view name);

/**
 * Returns the file name of a module for the current build configuration and platform.
 *
 * @param name The module name.
 * @return The name with the build postfix and the platform library extension.
 */
REX_API std::filesystem::path ModuleFileName(std::string_view name);

/**
 * Returns the number of IInterfaceFactory::Get calls answered since the last Init.
 *
 * @return The request count.
 */
REX_API u64 InterfaceRequestCount();

class REX_API ModuleLoader final : public IInterfaceFactory {
 public:
  static ModuleLoader& Instance();

  Result<API*> Connect(const Config& config);
  Result<void> InitModules();
  Result<API*> Init(const Config& config);
  Config& mutable_config() { return config_; }
  void Shutdown();

  void* Get(const char* name, u32 version, InterfaceStatus* out_status) override;
  void* GetForAPI(const char* name, u32 version) const;
  bool IsLoaded(std::string_view module_name) const;
  const Config& config() const { return config_; }
  CvarRegistryImpl& cvars() { return *cvars_; }
  void UpdatePaths(const PathsConfig& paths) { config_.paths = paths; }
  bool initialized() const { return initialized_; }
  u64 request_count() const { return request_count_; }

 private:
  struct Loaded {
    std::string name;
    platform::DynamicLibrary library;
    IModule* module = nullptr;
    std::span<const InterfaceEntry> interfaces;
    bool connected = false;
    bool initialized = false;
  };
  struct Registered {
    std::string name;
    u32 version = 0;
    void* (*get)() = nullptr;
    size_t owner = 0;
  };

  ModuleLoader() : api_(this) {}

  Result<void> LoadAll();
  Result<void> BuildRegistry();
  Result<void> CheckRequirements() const;
  Result<void> ConnectAll();
  Result<void> InitAll();
  void UnwindAll();
  Result<void> RegisterConsumerTables();
  Result<void> ApplyCvarValues();
  void UnregisterTables();
  static Error ModuleError(std::string message);

  Config config_;
  API api_;
  std::vector<Loaded> loaded_;
  std::vector<Registered> registry_;
  u64 request_count_ = 0;
  bool initialized_ = false;
  bool connected_ = false;
  std::unique_ptr<CvarRegistryImpl> cvars_ = CreateCvarRegistry();
  bool builtin_registered_ = false;
  size_t consumer_tables_registered_ = 0;
};

/** Registers the log, clock and perf tables core owns. */
REX_API Result<void> RegisterBuiltinCvars(cvar::ICvarRegistry& registry);
/** Unregisters the tables RegisterBuiltinCvars registered, in reverse order. */
REX_API void UnregisterBuiltinCvars(cvar::ICvarRegistry& registry);

}  // namespace rex::detail
