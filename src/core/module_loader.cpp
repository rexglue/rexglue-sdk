/**
 * @file        core/module_loader.cpp
 * @brief       Module loader implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "module_loader.h"

#include <algorithm>
#include <charconv>
#include <cstring>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>

namespace rex::detail {

namespace {

std::string_view BuildPostfix() {
  constexpr std::string_view config = REXGLUE_BUILD_CONFIG;
  if (config == "Debug") {
    return "d";
  }
  if (config == "RelWithDebInfo") {
    return "rd";
  }
  return "";
}

// Splits "name@version". Returns false when the form is wrong.
bool SplitRequirement(std::string_view text, std::string& name, uint32_t& version) {
  auto at = text.rfind('@');
  if (at == std::string_view::npos || at == 0 || at + 1 >= text.size()) {
    return false;
  }
  name.assign(text.substr(0, at));
  auto digits = text.substr(at + 1);
  auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), version);
  return ec == std::errc() && ptr == digits.data() + digits.size();
}

}  // namespace

bool IsValidModuleName(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  if (name.find_first_of("/\\:") != std::string_view::npos) {
    return false;
  }
  if (name.find("..") != std::string_view::npos) {
    return false;
  }
  return true;
}

std::filesystem::path ModuleFileName(std::string_view name) {
#if REX_PLATFORM_WIN32
  return fmt::format("{}{}.dll", name, BuildPostfix());
#elif REX_PLATFORM_MAC
  return fmt::format("lib{}{}.dylib", name, BuildPostfix());
#else
  return fmt::format("lib{}{}.so", name, BuildPostfix());
#endif
}

uint64_t InterfaceRequestCount() {
  return ModuleLoader::Instance().request_count();
}

ModuleLoader& ModuleLoader::Instance() {
  static ModuleLoader instance;
  return instance;
}

Error ModuleLoader::ModuleError(std::string message) {
  REXLOG_ERROR("{}", message);
  return Error(ErrorCategory::Module, std::move(message));
}

Result<Api*> ModuleLoader::Init(const Config& config) {
  if (initialized_) {
    return Err<Api*>(ModuleError("rex::Init called while initialized"));
  }
  config_ = config;
  request_count_ = 0;
  loaded_.clear();
  registry_.clear();

  for (size_t i = 0; i < config_.modules.size(); ++i) {
    const auto& name = config_.modules[i];
    if (!IsValidModuleName(name)) {
      return Err<Api*>(ModuleError(fmt::format("module '{}' has an invalid name", name)));
    }
    for (size_t j = 0; j < i; ++j) {
      if (config_.modules[j] == name) {
        return Err<Api*>(ModuleError(fmt::format("module '{}' listed twice", name)));
      }
    }
  }
  if (auto status = LoadAll(); !status) {
    UnwindAll();
    return Err<Api*>(status.error());
  }
  if (auto status = BuildRegistry(); !status) {
    UnwindAll();
    return Err<Api*>(status.error());
  }
  if (auto status = CheckRequirements(); !status) {
    UnwindAll();
    return Err<Api*>(status.error());
  }
  if (auto status = ConnectAll(); !status) {
    UnwindAll();
    return Err<Api*>(status.error());
  }
  if (auto status = InitAll(); !status) {
    UnwindAll();
    return Err<Api*>(status.error());
  }
  initialized_ = true;
  return &api_;
}

Status ModuleLoader::LoadAll() {
  const auto folder = rex::filesystem::GetExecutableFolder();
  for (const auto& name : config_.modules) {
    Loaded entry;
    entry.name = name;
    const auto path = folder / ModuleFileName(name);
    if (!std::filesystem::exists(path)) {
      return Err(ModuleError(fmt::format("module '{}' not found at '{}'", name, path.string())));
    }
    if (!entry.library.Load(path, platform::SymbolResolution::kImmediate)) {
      return Err(ModuleError(fmt::format("module '{}' failed to load from '{}': {}", name,
                                         path.string(), entry.library.last_error())));
    }
    auto create = entry.library.GetSymbol<CreateInterfaceFn>(kCreateInterfaceSymbol);
    if (!create) {
      return Err(ModuleError(fmt::format("module '{}' has no {}", name, kCreateInterfaceSymbol)));
    }
    auto enumerate = entry.library.GetSymbol<EnumerateInterfacesFn>(kEnumerateInterfacesSymbol);
    if (!enumerate) {
      return Err(
          ModuleError(fmt::format("module '{}' has no {}", name, kEnumerateInterfacesSymbol)));
    }
    int32_t status = 0;
    void* module = create(IModule::kInterfaceName, IModule::kInterfaceVersion, &status);
    if (!module) {
      return Err(
          ModuleError(fmt::format("module '{}' does not serve {} version {} (status {})", name,
                                  IModule::kInterfaceName, IModule::kInterfaceVersion, status)));
    }
    entry.module = static_cast<IModule*>(module);
    const InterfaceEntry* entries = nullptr;
    uint32_t count = 0;
    enumerate(&entries, &count);
    entry.interfaces = std::span<const InterfaceEntry>(entries, count);
    loaded_.push_back(std::move(entry));
  }
  return Ok();
}

Status ModuleLoader::BuildRegistry() {
  for (size_t owner = 0; owner < loaded_.size(); ++owner) {
    for (const auto& entry : loaded_[owner].interfaces) {
      if (std::strcmp(entry.name, IModule::kInterfaceName) == 0) {
        continue;
      }
      for (const auto& existing : registry_) {
        if (existing.name == entry.name) {
          return Err(
              ModuleError(fmt::format("interface {} is served by both '{}' and '{}'", entry.name,
                                      loaded_[existing.owner].name, loaded_[owner].name)));
        }
      }
      registry_.push_back(Registered{entry.name, entry.version, entry.get, owner});
    }
  }
  return Ok();
}

Status ModuleLoader::CheckRequirements() const {
  for (size_t i = 0; i < loaded_.size(); ++i) {
    for (const char* requirement : loaded_[i].module->RequiredInterfaces()) {
      std::string name;
      uint32_t version = 0;
      if (!SplitRequirement(requirement, name, version)) {
        return Err(ModuleError(fmt::format("module '{}' has a malformed requirement '{}'",
                                           loaded_[i].name, requirement)));
      }
      bool served = false;
      for (const auto& reg : registry_) {
        if (reg.owner < i && reg.name == name && reg.version == version) {
          served = true;
          break;
        }
      }
      if (!served) {
        return Err(ModuleError(
            fmt::format("module '{}' requires {} version {}, which no module before it serves",
                        loaded_[i].name, name, version)));
      }
    }
  }
  return Ok();
}

Status ModuleLoader::ConnectAll() {
  for (auto& entry : loaded_) {
    auto status = entry.module->Connect(this, config_);
    if (!status) {
      return Err(ModuleError(
          fmt::format("module '{}' Connect failed: {}", entry.name, status.error().what())));
    }
    entry.connected = true;
  }
  return Ok();
}

Status ModuleLoader::InitAll() {
  for (auto& entry : loaded_) {
    auto status = entry.module->Init();
    if (!status) {
      return Err(ModuleError(
          fmt::format("module '{}' Init failed: {}", entry.name, status.error().what())));
    }
    entry.initialized = true;
  }
  return Ok();
}

void ModuleLoader::UnwindAll() {
  for (auto it = loaded_.rbegin(); it != loaded_.rend(); ++it) {
    if (it->initialized) {
      it->module->Shutdown();
      it->initialized = false;
    }
  }
  for (auto it = loaded_.rbegin(); it != loaded_.rend(); ++it) {
    if (it->connected) {
      it->module->Disconnect();
      it->connected = false;
    }
  }
  registry_.clear();
  for (auto it = loaded_.rbegin(); it != loaded_.rend(); ++it) {
    it->module = nullptr;
    it->interfaces = {};
    if (config_.unload_modules_at_shutdown) {
      it->library.Close();
    } else {
      retired_.push_back(std::move(it->library));
    }
  }
  loaded_.clear();
}

void ModuleLoader::Shutdown() {
  UnwindAll();
  initialized_ = false;
}

void* ModuleLoader::Get(const char* name, uint32_t version, InterfaceStatus* out_status) {
  ++request_count_;
  if (!name) {
    if (out_status)
      *out_status = InterfaceStatus::NotFound;
    return nullptr;
  }
  for (const auto& reg : registry_) {
    if (reg.name != name) {
      continue;
    }
    if (reg.version != version) {
      if (out_status)
        *out_status = InterfaceStatus::VersionMismatch;
      return nullptr;
    }
    if (out_status)
      *out_status = InterfaceStatus::Ok;
    return reg.get();
  }
  if (out_status)
    *out_status = InterfaceStatus::NotFound;
  return nullptr;
}

void* ModuleLoader::GetForApi(const char* name, uint32_t version) const {
  if (!initialized_ || !name) {
    return nullptr;
  }
  for (const auto& reg : registry_) {
    if (reg.name == name && reg.version == version) {
      return reg.get();
    }
  }
  return nullptr;
}

bool ModuleLoader::IsLoaded(std::string_view module_name) const {
  if (!initialized_) {
    return false;
  }
  return std::any_of(loaded_.begin(), loaded_.end(), [&](const Loaded& entry) {
    return entry.initialized && entry.name == module_name;
  });
}

}  // namespace rex::detail
