#include "base/module_group.h"

#include <algorithm>
#include <format>
#include <functional>

#include <rex/platform/dynlib.h>
#include <rex/platform/file.h>

#include <rex/logging/logger.h>
#include <rex/settings_registry.h>

#include "base/library_name.h"
#include "base/process_control.h"

namespace rex {

namespace {

/** The build of the SDK this host was compiled from. */
constexpr std::string_view kHostBuild = REXGLUE_BUILD_IDENTITY;

struct Entry {
  platform::DynamicLibrary library;
  std::vector<InterfaceType> types;
  IModule* module = nullptr;
  CreateInterfaceFn create = nullptr;
  std::string label;
  /** The build the library reports, or empty when the library reports none. */
  std::string build;
  std::unique_ptr<IInterfaceFactory> factory;
  bool connected = false;
  bool inited = false;
};

struct Served {
  IInterface* iface;
  InterfaceType type;
};

bool Matches(const InterfaceType& type, std::string_view name, u32 version) {
  return type.version == version && name == type.name;
}

IInterface* Query(Entry& entry, std::string_view name, u32 version) {
  if (Matches(IModule::kInterface, name, version)) {
    return entry.module;
  }
  if (entry.create) {
    const std::string text(name);
    return entry.create(text.c_str(), version);
  }
  return entry.module->QueryInterface(name, version);
}

}  // namespace

struct ModuleGroup::State {
  std::vector<std::unique_ptr<Entry>> entries;
  std::vector<Served> served;
  std::function<void(IModule&, bool)> observer;
  std::unique_ptr<IInterfaceFactory> private_factory;
  bool connected = false;
  bool inited = false;

  void StopEntry(Entry& entry) const {
    if (observer) {
      observer(*entry.module, false);
    }
    entry.module->Shutdown();
    entry.inited = false;
  }

  bool IsPrivate(std::string_view name, u32 version) const {
    for (const Served& host : served) {
      if (host.type.is_private && Matches(host.type, name, version)) {
        return true;
      }
    }
    for (const auto& entry : entries) {
      for (const InterfaceType& type : entry->types) {
        if (type.is_private && Matches(type, name, version)) {
          return true;
        }
      }
    }
    return false;
  }

  IInterface* FindBefore(size_t limit, std::string_view name, u32 version, bool allow_private) {
    if (!allow_private && IsPrivate(name, version)) {
      return nullptr;
    }
    for (const Served& host : served) {
      if (Matches(host.type, name, version)) {
        return host.iface;
      }
    }
    for (size_t i = 0; i < limit && i < entries.size(); ++i) {
      if (IInterface* found = Query(*entries[i], name, version)) {
        return found;
      }
    }
    return nullptr;
  }

  void ShutdownAll() {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      if ((*it)->inited) {
        StopEntry(**it);
      }
    }
    inited = false;
  }

  void DisconnectAll() {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      if ((*it)->connected) {
        (*it)->module->Disconnect();
        (*it)->connected = false;
      }
    }
    connected = false;
  }

  class Prefix final : public IInterfaceFactory {
   public:
    Prefix(State& state, const Entry& self) : state_(state), self_(self) {}
    void Open() { open_ = true; }
    IInterface* Find(std::string_view name, u32 version) override {
      if (state_.IsPrivate(name, version)) {
        return nullptr;
      }
      return state_.FindFor(self_, open_, name, version);
    }
    std::vector<IInterface*> FindAll(std::string_view name, u32 version) override {
      if (state_.IsPrivate(name, version)) {
        return {};
      }
      return state_.FindAllFor(self_, open_, name, version);
    }

   private:
    State& state_;
    const Entry& self_;
    bool open_ = false;
  };

  IInterface* FindFor(const Entry& self, bool open, std::string_view name, u32 version) {
    for (const Served& host : served) {
      if (Matches(host.type, name, version)) {
        return host.iface;
      }
    }
    for (const auto& other : entries) {
      if (other.get() == &self) {
        if (!open) {
          break;
        }
        continue;
      }
      if (open && !other->connected) {
        continue;
      }
      if (IInterface* found = Query(*other, name, version)) {
        return found;
      }
    }
    return nullptr;
  }

  std::vector<IInterface*> FindAllFor(const Entry& self, bool open, std::string_view name,
                                      u32 version) {
    std::vector<IInterface*> found;
    for (const auto& other : entries) {
      if (other.get() == &self) {
        if (!open) {
          break;
        }
        continue;
      }
      if (open && !other->connected) {
        continue;
      }
      if (IInterface* iface = Query(*other, name, version)) {
        found.push_back(iface);
      }
    }
    return found;
  }

  /**
   * Adds a line to an error message that names the build of the host and of every loaded library,
   * when they are not all the same. A mixed install is then named where the failure is.
   *
   * @param message The error message.
   * @param pending A library that is loaded but not in the list yet, or null.
   * @return The message, with the line when the builds differ.
   */
  std::string WithBuilds(std::string message, const Entry* pending = nullptr) const {
    std::vector<std::pair<std::string_view, std::string_view>> builds{{"host", kHostBuild}};
    const auto add = [&](const Entry& entry) {
      if (entry.library) {
        builds.emplace_back(entry.label, entry.build.empty() ? std::string_view("unknown")
                                                             : std::string_view(entry.build));
      }
    };
    for (const auto& entry : entries) {
      add(*entry);
    }
    if (pending) {
      add(*pending);
    }
    const bool same = std::ranges::all_of(
        builds, [&](const auto& build) { return build.second == builds.front().second; });
    if (same) {
      return message;
    }
    message += "\nbuilds: ";
    for (size_t i = 0; i < builds.size(); ++i) {
      message += std::format("{}{} {}", i ? ", " : "", builds[i].first, builds[i].second);
    }
    return message;
  }

  Result<void> ConnectEntry(size_t index) {
    Entry& entry = *entries[index];
    auto prefix_owner = std::make_unique<Prefix>(*this, entry);
    Prefix& prefix = *prefix_owner;
    entry.factory = std::move(prefix_owner);
    for (const InterfaceType& required : entry.module->GetRequiredInterfaces()) {
      if (!prefix.Find(required.name, required.version)) {
        return Err(ErrorCategory::kModule,
                   WithBuilds(std::format("module '{}' requires {} version {}, which the host "
                                          "and the earlier modules do not serve",
                                          entry.label, required.name, required.version)));
      }
    }
    if (auto connected_module = entry.module->Connect(prefix); !connected_module) {
      return Err(ErrorCategory::kModule,
                 WithBuilds(std::format("module '{}' failed to connect, {}", entry.label,
                                        connected_module.error().message)));
    }
    entry.connected = true;
    prefix.Open();
    return Ok();
  }

  Result<void> InitEntry(size_t index) {
    Entry& entry = *entries[index];
    if (auto started = entry.module->Init(); !started) {
      return Err(ErrorCategory::kModule,
                 WithBuilds(std::format("module '{}' failed to init, {}", entry.label,
                                        started.error().message)));
    }
    entry.inited = true;
    if (observer) {
      observer(*entry.module, true);
    }
    return Ok();
  }

  Result<void> ConnectPending() {
    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i]->connected) {
        continue;
      }
      if (auto connected_entry = ConnectEntry(i); !connected_entry) {
        ShutdownAll();
        DisconnectAll();
        return connected_entry;
      }
    }
    connected = true;
    return Ok();
  }

  Result<void> InitPending() {
    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i]->inited) {
        continue;
      }
      if (auto started = InitEntry(i); !started) {
        ShutdownAll();
        DisconnectAll();
        return started;
      }
    }
    inited = true;
    return Ok();
  }

  Result<void> SettleBatch(size_t first) {
    if (!connected) {
      return Ok();
    }
    const size_t last = entries.size();
    const auto fail = [&](Result<void> failure) {
      for (size_t i = entries.size(); i-- > first;) {
        Entry& entry = *entries[i];
        if (entry.inited) {
          StopEntry(entry);
        }
      }
      for (size_t i = entries.size(); i-- > first;) {
        Entry& entry = *entries[i];
        if (entry.connected) {
          entry.module->Disconnect();
          entry.connected = false;
        }
      }
      entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(first), entries.end());
      return failure;
    };
    for (size_t i = first; i < last; ++i) {
      if (Result<void> connected_entry = ConnectEntry(i); !connected_entry) {
        return fail(std::move(connected_entry));
      }
    }
    if (inited) {
      for (size_t i = first; i < last; ++i) {
        if (Result<void> started = InitEntry(i); !started) {
          return fail(std::move(started));
        }
      }
    }
    return Ok();
  }

  Result<void> Settle(size_t index) {
    if (!connected) {
      return Ok();
    }
    Result<void> result = ConnectEntry(index);
    if (result && inited) {
      result = InitEntry(index);
    }
    if (!result) {
      if (entries[index]->connected) {
        entries[index]->module->Disconnect();
      }
      entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return result;
  }
};

namespace {

class PrivateFactory final : public IInterfaceFactory {
 public:
  explicit PrivateFactory(std::function<IInterface*(std::string_view, u32)> find,
                          std::function<std::vector<IInterface*>(std::string_view, u32)> find_all)
      : find_(std::move(find)), find_all_(std::move(find_all)) {}

  IInterface* Find(std::string_view name, u32 version) override { return find_(name, version); }
  std::vector<IInterface*> FindAll(std::string_view name, u32 version) override {
    return find_all_(name, version);
  }

 private:
  std::function<IInterface*(std::string_view, u32)> find_;
  std::function<std::vector<IInterface*>(std::string_view, u32)> find_all_;
};

}  // namespace

ModuleGroup::ModuleGroup() : state_(std::make_unique<State>()) {
  State* state = state_.get();
  state_->private_factory = std::make_unique<PrivateFactory>(
      [state](std::string_view name, u32 version) {
        return state->FindBefore(state->entries.size(), name, version, true);
      },
      [state](std::string_view name, u32 version) {
        std::vector<IInterface*> found;
        for (const auto& entry : state->entries) {
          if (IInterface* iface = Query(*entry, name, version)) {
            found.push_back(iface);
          }
        }
        return found;
      });
  state_->served.push_back({&REX_SETTINGS(), ISettingsRegistry::kInterface});
  state_->served.push_back({&REX_LOGGER(), ILogger::kInterface});
  state_->served.push_back({&GetProcessSettingsControl(), ISettingsRegistryControl::kInterface});
  state_->served.push_back({&GetProcessLoggerControl(), ILoggerControl::kInterface});
}

ModuleGroup::~ModuleGroup() {
  state_->ShutdownAll();
  state_->DisconnectAll();
  while (!state_->entries.empty()) {
    state_->entries.pop_back();
  }
}

void ModuleGroup::Serve(IInterface& iface, InterfaceType type) {
  state_->served.push_back({&iface, type});
}

Result<void> ModuleGroup::Add(IModule& module) {
  auto entry = std::make_unique<Entry>();
  entry->module = &module;
  entry->types = detail::ListServedInterfaces(module);
  entry->label = std::string(module.GetName());
  state_->entries.push_back(std::move(entry));
  return state_->Settle(state_->entries.size() - 1);
}

Result<void> ModuleGroup::Add(std::span<IModule* const> modules) {
  const size_t first = state_->entries.size();
  for (IModule* module : modules) {
    auto entry = std::make_unique<Entry>();
    entry->module = module;
    entry->types = detail::ListServedInterfaces(*module);
    entry->label = std::string(module->GetName());
    state_->entries.push_back(std::move(entry));
  }
  return state_->SettleBatch(first);
}

size_t ModuleGroup::Mark() const {
  return state_->entries.size();
}

void ModuleGroup::ShutdownAfter(size_t mark) {
  for (size_t i = state_->entries.size(); i-- > mark;) {
    Entry& entry = *state_->entries[i];
    if (entry.inited) {
      state_->StopEntry(entry);
    }
  }
}

void ModuleGroup::Observe(std::function<void(IModule&, bool)> observer) {
  state_->observer = std::move(observer);
}

Result<void> ModuleGroup::Load(std::string_view module_name) {
  if (std::ranges::any_of(state_->entries, [&](const std::unique_ptr<Entry>& entry) {
        return static_cast<bool>(entry->library) && entry->label == module_name;
      })) {
    return Ok();
  }
  std::vector<std::filesystem::path> folders;
  const std::filesystem::path file =
      detail::FindLibraryFile(detail::ModuleFileName(module_name), folders);
  if (file.empty()) {
    std::string tried;
    for (const std::filesystem::path& folder : folders) {
      tried += std::format("{}'{}'", tried.empty() ? "" : ", ", folder.string());
    }
    return Err(ErrorCategory::kModule,
               std::format("module '{}' not found in {}", module_name, tried));
  }
  auto entry = std::make_unique<Entry>();
  entry->label = std::string(module_name);
  if (!entry->library.Load(file, platform::SymbolResolution::kImmediate)) {
    return Err(ErrorCategory::kModule, state_->WithBuilds(std::format(
                                           "module '{}' failed to load from '{}', {}", module_name,
                                           file.string(), entry->library.GetLastError())));
  }
  using IdentityFn = const char* (*)();
  if (auto identity = entry->library.GetSymbol<IdentityFn>(kBuildIdentitySymbol)) {
    if (const char* build = identity()) {
      entry->build = build;
    }
  }
  auto create = entry->library.GetSymbol<CreateInterfaceFn>(kCreateInterfaceSymbol);
  auto enumerate = entry->library.GetSymbol<EnumerateInterfacesFn>(kEnumerateInterfacesSymbol);
  if (!create || !enumerate) {
    return Err(ErrorCategory::kModule,
               state_->WithBuilds(
                   std::format("module '{}' exports no {}", module_name,
                               create ? kEnumerateInterfacesSymbol : kCreateInterfaceSymbol),
                   entry.get()));
  }
  std::vector<InterfaceType> types(enumerate(nullptr, 0));
  enumerate(types.data(), static_cast<u32>(types.size()));
  const bool serves_module = std::ranges::any_of(types, [](const InterfaceType& type) {
    return Matches(IModule::kInterface, type.name, type.version);
  });
  if (!serves_module) {
    const auto other = std::ranges::find_if(types, [](const InterfaceType& type) {
      return std::string_view(type.name) == IModule::kInterface.name;
    });
    const std::string reason =
        other == types.end()
            ? std::format("module '{}' does not serve {}", module_name, IModule::kInterface.name)
            : std::format("module '{}' serves {} version {}, and this host needs version {}",
                          module_name, IModule::kInterface.name, other->version,
                          IModule::kInterface.version);
    return Err(ErrorCategory::kModule, state_->WithBuilds(reason, entry.get()));
  }
  entry->types = types;
  entry->create = create;
  entry->module =
      static_cast<IModule*>(create(IModule::kInterface.name, IModule::kInterface.version));
  if (!entry->module) {
    return Err(ErrorCategory::kModule,
               state_->WithBuilds(
                   std::format("module '{}' returned no {}", module_name, IModule::kInterface.name),
                   entry.get()));
  }
  REXLOG_INFO(
      "module '{}' loaded from '{}', SDK build {}", module_name, file.string(),
      entry->build.empty() ? std::string_view("unknown build") : std::string_view(entry->build));
  state_->entries.push_back(std::move(entry));
  return state_->Settle(state_->entries.size() - 1);
}

Result<void> ModuleGroup::Connect() {
  return state_->ConnectPending();
}

Result<void> ModuleGroup::Init() {
  return state_->InitPending();
}

void ModuleGroup::Shutdown() {
  state_->ShutdownAll();
}

void ModuleGroup::Disconnect() {
  state_->ShutdownAll();
  state_->DisconnectAll();
}

IInterfaceFactory& ModuleGroup::GetPrivateFactory() {
  return *state_->private_factory;
}

IInterface* ModuleGroup::Find(std::string_view name, u32 version) {
  return state_->FindBefore(state_->entries.size(), name, version, false);
}

std::vector<IInterface*> ModuleGroup::FindAll(std::string_view name, u32 version) {
  std::vector<IInterface*> found;
  if (state_->IsPrivate(name, version)) {
    return found;
  }
  for (const auto& entry : state_->entries) {
    if (IInterface* iface = Query(*entry, name, version)) {
      found.push_back(iface);
    }
  }
  return found;
}

}  // namespace rex
