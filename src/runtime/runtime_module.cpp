#include "runtime/runtime_module.h"

#include <format>
#include <string>
#include <string_view>

#include <rex/logging.h>
#include <rex/macros.h>

#include "runtime/cvar_table.h"

REXLOG_TABLE(kRuntimeLog, kernel, system, cpu, title, apu, hid, ui, console)

namespace rex::runtime {

namespace {

struct ScopeTable {
  std::string_view scope;
  std::span<const cvar::IVar* const> vars;
};

const std::array<ScopeTable, 3> kTables = {{
    {"runtime", cvars::kRuntimeCvars},
    {"apu", cvars::kAPUCvars},
    {"hid", {}},
}};

[[nodiscard]] Result<void> ValidateLayers(const Config& config) {
  auto fail = [](const std::string& message) {
    REX_FAIL(system, Err(Error(ErrorCategory::Config, message)), "{}", message);
  };
  if (config.runtime.heap_size_mb < 1 || config.runtime.heap_size_mb > 2048) {
    return fail(std::format("config field runtime.heap_size_mb = {} is outside 1 to 2048",
                            config.runtime.heap_size_mb));
  }
  if (config.apu.max_queued_frames < 4 || config.apu.max_queued_frames > 64) {
    return fail(std::format("config field apu.max_queued_frames = {} is outside 4 to 64",
                            config.apu.max_queued_frames));
  }
  return Ok();
}

RuntimeModule& Instance() {
  static RuntimeModule instance;
  return instance;
}

}  // namespace

void* RuntimeModule::GetModule() {
  return static_cast<IModule*>(&Instance());
}

void RuntimeModule::SetActive(Runtime* runtime) {
  Instance().runtime_.store(runtime, std::memory_order_release);
}

Runtime* RuntimeModule::active() {
  return Instance().runtime_.load(std::memory_order_acquire);
}

void* RuntimeModule::GetObjectTable() {
  Runtime* runtime = active();
  return runtime != nullptr ? static_cast<IObjectTable*>(&runtime->object_table()) : nullptr;
}

void* RuntimeModule::GetThreadRegistry() {
  Runtime* runtime = active();
  return runtime != nullptr ? static_cast<IThreadRegistry*>(&runtime->thread_registry()) : nullptr;
}

void* RuntimeModule::GetModuleRegistry() {
  Runtime* runtime = active();
  return runtime != nullptr ? static_cast<IModuleRegistry*>(&runtime->module_registry()) : nullptr;
}

void* RuntimeModule::GetNotifier() {
  Runtime* runtime = active();
  return runtime != nullptr ? static_cast<INotifier*>(&runtime->notifier()) : nullptr;
}

void* RuntimeModule::GetContentManager() {
  Runtime* runtime = active();
  return runtime != nullptr ? static_cast<IContentManager*>(&runtime->content_manager()) : nullptr;
}

void* RuntimeModule::GetAchievementManager() {
  Runtime* runtime = active();
  return runtime != nullptr ? static_cast<IAchievementManager*>(&runtime->achievement_manager())
                            : nullptr;
}

void* RuntimeModule::GetDispatcher() {
  Runtime* runtime = active();
  return runtime != nullptr ? static_cast<IDispatcher*>(&runtime->dispatcher()) : nullptr;
}

apu::IAPUBackend* RuntimeModule::apu_backend() {
  return Instance().apu_;
}

hid::IHIDBackend* RuntimeModule::hid_backend() {
  return Instance().hid_;
}

const char* RuntimeModule::Name() const {
  return "rex-runtime";
}

std::span<const char* const> RuntimeModule::RequiredInterfaces() const {
  static constexpr std::array<const char*, 2> kRequired = {"rex.cvar@1", "rex.filesystem@1"};
  return kRequired;
}

Result<void> RuntimeModule::Connect(IInterfaceFactory* factory, const Config& config) {
  REX_TRY_VOID(ValidateLayers(config));
  REX_TRY(cvars_, factory->Get<cvar::ICvarRegistry>());
  registered_ = 0;
  for (const ScopeTable& table : kTables) {
    if (auto status = cvars_->Register(table.scope, table.vars); !status) {
      Disconnect();
      return status;
    }
    ++registered_;
  }
  auto logger = factory->Get<ILogger>();
  if (!logger) {
    Disconnect();
    return Err(logger.error());
  }
  logger_ = *logger;
  if (auto status = logger_->Register(log::kRuntimeLog); !status) {
    logger_ = nullptr;
    Disconnect();
    return status;
  }
  auto defaults = [&]() -> Result<void> {
    REX_TRY_VOID(cvars_->SetDefault("runtime.writable_code_segments",
                                    config.runtime.writable_code_segments ? "true" : "false"));
    REX_TRY_VOID(
        cvars_->SetDefault("runtime.protect_zero", config.runtime.protect_zero ? "true" : "false"));
    REX_TRY_VOID(
        cvars_->SetDefault("apu.max_queued_frames", std::to_string(config.apu.max_queued_frames)));
    return Ok();
  };
  if (auto status = defaults(); !status) {
    Disconnect();
    return status;
  }
  if (auto backend = factory->Get<apu::IAPUBackend>(); backend) {
    apu_ = *backend;
  }
  if (auto backend = factory->Get<hid::IHIDBackend>(); backend) {
    hid_ = *backend;
  }
  return Ok();
}

void RuntimeModule::Disconnect() {
  apu_ = nullptr;
  hid_ = nullptr;
  if (logger_) {
    logger_->Unregister(log::kRuntimeLog);
  }
  logger_ = nullptr;
  if (cvars_) {
    for (size_t i = registered_; i > 0; --i) {
      cvars_->Unregister(kTables[i - 1].vars);
    }
  }
  registered_ = 0;
  cvars_ = nullptr;
}

Result<void> RuntimeModule::Init() {
  return Ok();
}

void RuntimeModule::Shutdown() {}

}  // namespace rex::runtime

REX_DECLARE_MODULE(rex::runtime::RuntimeModule)
