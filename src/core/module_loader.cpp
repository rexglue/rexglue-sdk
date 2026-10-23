#include "module_loader.h"

#include "core/logger.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <string>

#include <format>

#include <rex/logging/logger.h>
#include <rex/platform/file.h>
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/platform/thread.h>

namespace rex::detail {

namespace {

std::string_view BuildPostfix() {
  constexpr std::string_view kConfig = REXGLUE_BUILD_CONFIG;
  if (kConfig == "Debug") {
    return "d";
  }
  if (kConfig == "RelWithDebInfo") {
    return "rd";
  }
  return "";
}

bool SplitRequirement(std::string_view text, std::string& name, u32& version) {
  auto at = text.rfind('@');
  if (at == std::string_view::npos || at == 0 || at + 1 >= text.size()) {
    return false;
  }
  name.assign(text.substr(0, at));
  auto digits = text.substr(at + 1);
  auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), version);
  return ec == std::errc() && ptr == digits.data() + digits.size();
}

std::string DescribeLimit(u64 value) {
  return value == std::numeric_limits<u64>::max() ? "unlimited" : std::to_string(value);
}

void LogOpenFileLimit() {
  const auto raised = platform::RaiseOpenFileLimit();
  if (!raised) {
    REXLOG_WARN(core, "cannot raise the open file limit, {}", raised.error().message);
    return;
  }
  if (raised->hard == 0) {
    return;
  }
  if (raised->after > raised->before) {
    REXLOG_INFO(core, "open file limit raised from {} to {}, hard limit {}",
                DescribeLimit(raised->before), DescribeLimit(raised->after),
                DescribeLimit(raised->hard));
  } else {
    REXLOG_INFO(core, "open file limit is {}, hard limit {}", DescribeLimit(raised->after),
                DescribeLimit(raised->hard));
  }
}

}  // namespace

void* GetCvarRegistry() {
  return static_cast<rex::cvar::ICvarRegistry*>(&ModuleLoader::GetInstance().GetCvars());
}

constexpr std::array<InterfaceEntry, 2> kCoreInterfaces = {{
    {cvar::ICvarRegistry::kInterfaceName, cvar::ICvarRegistry::kInterfaceVersion, &GetCvarRegistry},
    {ILogger::kInterfaceName, ILogger::kInterfaceVersion, &log::GetLoggerInterface},
}};
constexpr const char* kCoreOwner = "core";
constexpr size_t kCoreOwnerIndex = SIZE_MAX;

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

std::filesystem::path LibraryFileName(std::string_view name) {
#if REX_PLATFORM_WIN32
  return std::format("{}.dll", name);
#elif REX_PLATFORM_MAC
  return std::format("lib{}.dylib", name);
#else
  return std::format("lib{}.so", name);
#endif
}

std::filesystem::path ModuleFileName(std::string_view name) {
  return LibraryFileName(std::format("{}{}", name, BuildPostfix()));
}

u64 GetInterfaceRequestCount() {
  return ModuleLoader::GetInstance().GetRequestCount();
}

ModuleLoader& ModuleLoader::GetInstance() {
  static ModuleLoader instance;
  return instance;
}

Error ModuleLoader::ModuleError(std::string message) {
  REX_FAIL(core, Error(ErrorCategory::kModule, std::move(message)), "{}", message);
}

Result<API*> ModuleLoader::Connect(const Config& config) {
  if (initialized_ || connected_) {
    return Err<API*>(ModuleError("rex::Init called while initialized"));
  }
  config_ = config;
  request_count_ = 0;
  loaded_.clear();
  registry_.clear();
  cvars_->Reset();
  builtin_registered_ = false;
  consumer_tables_registered_ = 0;

  for (size_t i = 0; i < config_.modules.size(); ++i) {
    const auto& name = config_.modules[i];
    if (!IsValidModuleName(name)) {
      return Err<API*>(ModuleError(std::format("module '{}' has an invalid name", name)));
    }
    for (size_t j = 0; j < i; ++j) {
      if (config_.modules[j] == name) {
        return Err<API*>(ModuleError(std::format("module '{}' listed twice", name)));
      }
    }
  }
  if (auto status = RegisterConsumerTables(); !status) {
    UnregisterTables();
    return Err<API*>(status.error());
  }
  if (auto status = RegisterBuiltinCvars(*cvars_); !status) {
    UnregisterTables();
    return Err<API*>(status.error());
  }
  builtin_registered_ = true;
  if (auto status = cvars_->SetDefault("perf.profiler", config_.profiler ? "true" : "false");
      !status) {
    UnregisterTables();
    return Err<API*>(status.error());
  }
  if (auto status = StartLogger(config_.log, *cvars_); !status) {
    UnregisterTables();
    return Err<API*>(status.error());
  }
  LogOpenFileLimit();
  if (auto status = LoadAll(); !status) {
    UnwindAll();
    return Err<API*>(status.error());
  }
  if (auto status = BuildRegistry(); !status) {
    UnwindAll();
    return Err<API*>(status.error());
  }
  if (auto status = CheckRequirements(); !status) {
    UnwindAll();
    return Err<API*>(status.error());
  }
  if (auto status = ConnectAll(); !status) {
    UnwindAll();
    return Err<API*>(status.error());
  }
  if (auto status = ApplyCvarValues(); !status) {
    UnwindAll();
    return Err<API*>(status.error());
  }
  FinishLoggerStart(*cvars_);
  connected_ = true;
  return &api_;
}

Result<void> ModuleLoader::InitModules() {
  cvars_->Finalize();
  if (auto status = InitAll(); !status) {
    UnwindAll();
    connected_ = false;
    return status;
  }
  initialized_ = true;
  return Ok();
}

Result<API*> ModuleLoader::Init(const Config& config) {
  REX_TRY(API * api, Connect(config));
  REX_TRY_VOID(InitModules());
  return api;
}

Result<void> ModuleLoader::LoadAll() {
  const auto folder = rex::platform::GetExecutableFolder();
  for (const auto& name : config_.modules) {
    Loaded entry;
    entry.name = name;
    const auto path = folder / ModuleFileName(name);
    if (!std::filesystem::exists(path)) {
      return Err(ModuleError(std::format("module '{}' not found at '{}'", name, path.string())));
    }
    if (!entry.library.Load(path, platform::SymbolResolution::kImmediate)) {
      return Err(ModuleError(std::format("module '{}' failed to load from '{}', {}", name,
                                         path.string(), entry.library.GetLastError())));
    }
    auto identity = entry.library.GetSymbol<BuildIdentityFn>(kBuildIdentitySymbol);
    if (!identity) {
      return Err(ModuleError(std::format(
          "module '{}' at '{}' exports no {}, it was not built against this SDK build '{}'", name,
          path.string(), kBuildIdentitySymbol, kBuildIdentity)));
    }
    if (std::strcmp(identity(), kBuildIdentity) != 0) {
      return Err(ModuleError(std::format(
          "module '{}' at '{}' was built against SDK build '{}', this SDK build is '{}'", name,
          path.string(), identity(), kBuildIdentity)));
    }
    auto create = entry.library.GetSymbol<CreateInterfaceFn>(kCreateInterfaceSymbol);
    if (!create) {
      return Err(ModuleError(std::format("module '{}' has no {}", name, kCreateInterfaceSymbol)));
    }
    auto enumerate = entry.library.GetSymbol<EnumerateInterfacesFn>(kEnumerateInterfacesSymbol);
    if (!enumerate) {
      return Err(
          ModuleError(std::format("module '{}' has no {}", name, kEnumerateInterfacesSymbol)));
    }
    i32 status = 0;
    void* module = create(IModule::kInterfaceName, IModule::kInterfaceVersion, &status);
    if (!module) {
      return Err(
          ModuleError(std::format("module '{}' does not serve {} version {} (status {})", name,
                                  IModule::kInterfaceName, IModule::kInterfaceVersion, status)));
    }
    entry.module = static_cast<IModule*>(module);
    const InterfaceEntry* entries = nullptr;
    u32 count = 0;
    enumerate(&entries, &count);
    entry.interfaces = std::span<const InterfaceEntry>(entries, count);
    loaded_.push_back(std::move(entry));
  }
  return Ok();
}

Result<void> ModuleLoader::BuildRegistry() {
  for (const auto& entry : kCoreInterfaces) {
    registry_.push_back(Registered{entry.name, entry.version, entry.get, kCoreOwnerIndex});
  }
  for (size_t owner = 0; owner < loaded_.size(); ++owner) {
    for (const auto& entry : loaded_[owner].interfaces) {
      if (std::strcmp(entry.name, IModule::kInterfaceName) == 0) {
        continue;
      }
      for (const auto& existing : registry_) {
        if (existing.name == entry.name) {
          const char* first =
              existing.owner == kCoreOwnerIndex ? kCoreOwner : loaded_[existing.owner].name.c_str();
          return Err(ModuleError(std::format("interface {} is served by both '{}' and '{}'",
                                             entry.name, first, loaded_[owner].name)));
        }
      }
      registry_.push_back(Registered{entry.name, entry.version, entry.get, owner});
    }
  }
  return Ok();
}

Result<void> ModuleLoader::CheckRequirements() const {
  for (size_t i = 0; i < loaded_.size(); ++i) {
    for (const char* requirement : loaded_[i].module->GetRequiredInterfaces()) {
      std::string name;
      u32 version = 0;
      if (!SplitRequirement(requirement, name, version)) {
        return Err(ModuleError(std::format("module '{}' has a malformed requirement '{}'",
                                           loaded_[i].name, requirement)));
      }
      bool served = false;
      for (const auto& reg : registry_) {
        if ((reg.owner == kCoreOwnerIndex || reg.owner < i) && reg.name == name &&
            reg.version == version) {
          served = true;
          break;
        }
      }
      if (!served) {
        return Err(ModuleError(
            std::format("module '{}' requires {} version {}, which no module before it serves",
                        loaded_[i].name, name, version)));
      }
    }
  }
  return Ok();
}

Result<void> ModuleLoader::ConnectAll() {
  for (auto& entry : loaded_) {
    auto status = entry.module->Connect(this, config_);
    if (!status) {
      return Err(ModuleError(std::format("module '{}' Connect failed, {}", entry.name,
                                         status.error().GetDescription())));
    }
    entry.connected = true;
  }
  return Ok();
}

Result<void> ModuleLoader::InitAll() {
  for (auto& entry : loaded_) {
    auto status = entry.module->Init();
    if (!status) {
      return Err(ModuleError(
          std::format("module '{}' Init failed: {}", entry.name, status.error().GetDescription())));
    }
    entry.initialized = true;
  }
  return Ok();
}

void ModuleLoader::UnwindAll() {
  ShutdownModules();
  CloseLibraries();
  StopLogger();
}

void ModuleLoader::ShutdownModules() {
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
  DetachLoggerCvars();
  UnregisterTables();
  registry_.clear();
}

void ModuleLoader::CloseLibraries() {
  for (auto it = loaded_.rbegin(); it != loaded_.rend(); ++it) {
    it->module = nullptr;
    it->interfaces = {};
    it->library.Close();
  }
  loaded_.clear();
  initialized_ = false;
  connected_ = false;
}

void ModuleLoader::Shutdown() {
  UnwindAll();
}

Result<void> ModuleLoader::RegisterConsumerTables() {
  for (const CvarTable& table : config_.cvars.tables) {
    if (auto status = cvars_->Register(table.scope, table.vars); !status) {
      auto message = std::format("consumer table '{}' failed to register, {}", table.scope,
                                 status.error().message);
      REX_FAIL(core, Err(Error(ErrorCategory::kConfig, std::move(message), status.error().code)),
               "{}", message);
    }
    ++consumer_tables_registered_;
  }
  return Ok();
}

Result<void> ModuleLoader::ApplyCvarValues() {
  auto unknown = cvars_->ApplyOverrides(config_.cvars.values);
  if (!unknown) {
    REX_FAIL(core, Err(unknown.error()), "{}", unknown.error().message);
  }
  std::vector<std::string> hard;
  for (const CvarOverride& o : *unknown) {
    const char* source = o.source == cvar::CvarSource::kDefault       ? "Default"
                         : o.source == cvar::CvarSource::kConfig      ? "Config"
                         : o.source == cvar::CvarSource::kEnvironment ? "Environment"
                                                                      : "CommandLine";
    if (o.source == cvar::CvarSource::kConfig || o.source == cvar::CvarSource::kEnvironment) {
      REXLOG_WARN(core, "unknown cvar '{}' from {}", o.name, source);
    } else {
      hard.push_back(std::format("unknown cvar '{}' from {}", o.name, source));
    }
  }
  for (const std::string& name : config_.cvars.locked) {
    if (!cvars_->Find(name))
      hard.push_back(std::format("unknown cvar '{}' in locked", name));
  }
  if (!hard.empty()) {
    std::string message;
    for (size_t i = 0; i < hard.size(); ++i)
      message += (i ? ", " : "") + hard[i];
    REX_FAIL(core, Err(Error(ErrorCategory::kConfig, std::move(message))), "{}", message);
  }
  if (auto status = cvars_->ApplyLocks(config_.cvars.locked); !status) {
    return Err(status.error());
  }
  return Ok();
}

void ModuleLoader::UnregisterTables() {
  if (builtin_registered_) {
    UnregisterBuiltinCvars(*cvars_);
    builtin_registered_ = false;
  }
  for (size_t i = consumer_tables_registered_; i > 0; --i) {
    cvars_->Unregister(config_.cvars.tables[i - 1].vars);
  }
  consumer_tables_registered_ = 0;
  for (const std::string& name : cvars_->GetRegisteredNames()) {
    REXLOG_WARN(core, "cvar '{}' was still registered at Shutdown", name);
  }
  cvars_->Reset();
}

Result<void*> ModuleLoader::Get(std::string_view name, u32 version) {
  ++request_count_;
  for (const auto& reg : registry_) {
    if (reg.name != name) {
      continue;
    }
    if (reg.version != version) {
      return Err<void*>(
          Error(ErrorCategory::kModule,
                std::format("{} is served at version {}, not {}", name, reg.version, version),
                std::to_underlying(InterfaceStatus::kVersionMismatch)));
    }
    return reg.get();
  }
  return Err<void*>(Error(ErrorCategory::kModule, std::format("{}@{} is not served", name, version),
                          std::to_underlying(InterfaceStatus::kNotFound)));
}

void* ModuleLoader::GetForAPI(const char* name, u32 version) const {
  if (!(initialized_ || connected_) || !name) {
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
