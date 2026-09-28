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
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/api.h>
#include <rex/module.h>
#include <rex/platform/dynlib.h>

namespace rex::detail {

// True when the name has no path separators, no drive colon and no ".." segment.
bool IsValidModuleName(std::string_view name);

// "<name><postfix>.<ext>" for the current build configuration and platform.
std::filesystem::path ModuleFileName(std::string_view name);

// Number of IInterfaceFactory::Get calls the loader has answered since the last Init.
uint64_t InterfaceRequestCount();

class ModuleLoader final : public IInterfaceFactory {
 public:
  static ModuleLoader& Instance();

  Result<Api*> Init(const Config& config);
  void Shutdown();

  void* Get(const char* name, uint32_t version, InterfaceStatus* out_status) override;
  void* GetForApi(const char* name, uint32_t version) const;
  bool IsLoaded(std::string_view module_name) const;
  bool initialized() const { return initialized_; }
  uint64_t request_count() const { return request_count_; }

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
    uint32_t version = 0;
    void* (*get)() = nullptr;
    size_t owner = 0;
  };

  ModuleLoader() : api_(this) {}

  Status LoadAll();
  Status BuildRegistry();
  Status CheckRequirements() const;
  Status ConnectAll();
  Status InitAll();
  void UnwindAll();
  static Error ModuleError(std::string message);

  Config config_;
  Api api_;
  std::vector<Loaded> loaded_;
  std::vector<platform::DynamicLibrary> retired_;
  std::vector<Registered> registry_;
  uint64_t request_count_ = 0;
  bool initialized_ = false;
};

}  // namespace rex::detail
