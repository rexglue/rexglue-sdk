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

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/api.h>
#include <rex/module.h>
#include <rex/platform/dynlib.h>
#include <rex/types.h>

#include "core/cvar_registry.h"

namespace rex::detail {

/**
 * Reports whether a module name is safe to resolve to a file.
 *
 * @param name The module name from Config::modules.
 * @return True when the name has no path separator, no colon and no ".." segment.
 */
REX_API bool IsValidModuleName(std::string_view name);

/**
 * Returns the file name of a shared library for the current platform.
 *
 * @param name The library target name.
 * @return The name with the platform library prefix and extension.
 */
REX_API std::filesystem::path LibraryFileName(std::string_view name);

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
REX_API u64 GetInterfaceRequestCount();

class REX_API ModuleLoader final : public IInterfaceFactory {
 public:
  static ModuleLoader& GetInstance();

  [[nodiscard]] Result<API*> Connect(const Config& config);
  [[nodiscard]] Result<void> InitModules();
  [[nodiscard]] Result<API*> Init(const Config& config);
  Config& GetMutableConfig() { return config_; }
  void Shutdown();

  /** Shuts down and disconnects every module in reverse load order and resets the cvar tables. */
  void ShutdownModules();

  /** Closes every module library in reverse load order. */
  void CloseLibraries();

  [[nodiscard]] Result<void*> Get(std::string_view name, u32 version) override;
  void* GetForAPI(const char* name, u32 version) const;
  bool IsLoaded(std::string_view module_name) const;
  const Config& GetConfig() const { return config_; }
  CvarRegistryImpl& GetCvars() { return *cvars_; }
  void UpdatePaths(const PathsConfig& paths) { config_.paths = paths; }
  bool GetInitialized() const { return initialized_; }
  u64 GetRequestCount() const { return request_count_; }

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

  [[nodiscard]] Result<void> LoadAll();
  [[nodiscard]] Result<void> BuildRegistry();
  [[nodiscard]] Result<void> CheckRequirements() const;
  [[nodiscard]] Result<void> ConnectAll();
  [[nodiscard]] Result<void> InitAll();
  void UnwindAll();
  [[nodiscard]] Result<void> RegisterConsumerTables();
  [[nodiscard]] Result<void> ApplyCvarValues();
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

/**
 * Registers the log, clock and perf tables core owns.
 *
 * @param registry The registry to fill.
 * @return Ok, or the registration error.
 */
[[nodiscard]] REX_API Result<void> RegisterBuiltinCvars(cvar::ICvarRegistry& registry);
/**
 * Unregisters the tables RegisterBuiltinCvars registered, in reverse order.
 *
 * @param registry The registry RegisterBuiltinCvars filled.
 */
REX_API void UnregisterBuiltinCvars(cvar::ICvarRegistry& registry);

}  // namespace rex::detail
