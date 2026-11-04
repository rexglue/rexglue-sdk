#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_paths.h>
#include <rex/kernel/runtime_settings.h>
#include <rex/kernel/kernel_settings.h>
#include <rex/kernel/title_exit.h>

#include <format>
#include <mutex>
#include <optional>

#include <rex/logging/log.h>
#include <rex/filesystem/file_system.h>
#include <rex/filesystem/xdbf.h>
#include "kernel/categories.h"
#include <rex/logging/logger.h>
#include <rex/platform/perf.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/result.h>
#include <rex/setting.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/utility.h>
#include "kernel/settings_access.h"
#include "kernel/rexkernel_module.h"
#include "kernel/title_state.h"
#include <rex/kernel/types/content.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/achievement_manager.h>

namespace rex::kernel {

namespace {

[[maybe_unused]] const bool g_tag_named = (memory::SetTagName(kernel::kRuntime, "Runtime"), true);

[[maybe_unused]] const bool g_counters_registered = [] {
  struct Entry {
    const char* name;
    platform::CounterType type;
  };
  constexpr Entry kCounters[] = {
      {"xma_frames_decoded", platform::CounterType::kAccumulator},
      {"functions_dispatched", platform::CounterType::kAccumulator},
      {"interrupt_dispatches", platform::CounterType::kAccumulator},
      {"active_threads", platform::CounterType::kGauge},
      {"apc_queue_depth", platform::CounterType::kAccumulator},
      {"critical_region_contentions", platform::CounterType::kGauge},
  };
  for (const Entry& entry : kCounters) {
    if (auto counter = platform::RegisterCounter(entry.name, entry.type); !counter) {
      REXSYS_WARN("{}", counter.error().message);
    }
  }
  return true;
}();

}  // namespace

TitlePaths::TitlePaths() : SettingOwner("title") {}

RuntimeSettings::RuntimeSettings() : SettingOwner("runtime") {}

KernelSettings::KernelSettings() : SettingOwner("kernel") {}

TitlePaths& GetTitlePaths() {
  static TitlePaths paths;
  return paths;
}

RuntimeSettings& GetRuntimeSettings() {
  static RuntimeSettings settings;
  return settings;
}

TitleRuntime::TitleRuntime(const ppc::PPCImage& image)
    : state_(std::make_unique<kernel::TitleState>(*this, image)) {}

TitleRuntime::~TitleRuntime() {
  state_->Unload();
  state_.reset();
}

Result<void> TitleRuntime::Load() {
  return state_->Load();
}

TitleState& StateOf(const TitleRuntime& runtime) {
  return *runtime.state_;
}

Result<void> TitleRuntime::Launch() {
  return state_->Launch();
}

void TitleRuntime::Unload() {
  state_->Unload();
}

bool TitleRuntime::IsLoaded() const {
  return state_->IsLoaded();
}

bool TitleRuntime::IsRunning() const {
  return state_->IsRunning();
}

u32 TitleRuntime::GetTitleID() const {
  return state_->GetTitleID();
}

std::unique_ptr<rex::filesystem::IXDBFTitleData> TitleRuntime::GetTitleXDBF() const {
  return state_->GetTitleXDBF();
}

rex::kernel::XLanguage TitleRuntime::GetUserLanguage() const {
  return state_->GetUserLanguage();
}

void TitleRuntime::SetAudioPaused(bool paused) {
  state_->SetAudioPaused(paused);
}

bool TitleRuntime::IsAudioPaused() const {
  return state_->IsAudioPaused();
}

rex::ScopeGuard TitleRuntime::OnTitleExit(rex::MoveOnlyFunction<void(TitleExitReason)> callback) {
  return state_->OnTitleExit(std::move(callback));
}

GuestMemory& TitleRuntime::GetGuestMemory() const {
  return state_->GetGuestMemory();
}

kernel::IModuleRegistry& TitleRuntime::GetModuleRegistry() {
  return state_->GetModuleRegistry();
}

rex::xam::IContentManager& TitleRuntime::GetContentManager() {
  return state_->GetContentManager();
}

rex::xam::IAchievementManager& TitleRuntime::GetAchievementManager() {
  return state_->GetAchievementManager();
}

kernel::IObjectTable& TitleRuntime::GetObjectTable() {
  return state_->GetObjectTable();
}

kernel::IThreadRegistry& TitleRuntime::GetThreadRegistry() {
  return state_->GetThreadRegistry();
}

kernel::ITitleWorkspace& TitleRuntime::GetGuestWorkspace() {
  return state_->GetWorkspace();
}

kernel::ISystemHeap& TitleRuntime::GetGuestHeap() {
  return *state_->GetMemory();
}

rex::memory::GuestMembase TitleRuntime::GetMembase() {
  return state_->GetMemory()->GetMembase();
}

kernel::ITitleExit& TitleRuntime::GetTitleExit() {
  return *state_;
}

kernel::INotifier& TitleRuntime::GetNotifier() {
  return state_->GetNotifier();
}

filesystem::IFileSystem& TitleRuntime::GetFileSystem() {
  return *state_->GetFileSystem();
}

Result<::PPCFunc*> TitleRuntime::FindFunction(u32 address) const {
  kernel::FunctionDispatcher* dispatcher = state_->GetFunctionDispatcher();
  ::PPCFunc* func = dispatcher ? dispatcher->GetFunction(address) : nullptr;
  if (!func) {
    return Err<::PPCFunc*>(ErrorCategory::kNotFound, std::format("no function at {:08X}", address));
  }
  return func;
}

TitlePaths& TitleRuntime::GetTitlePaths() {
  return rex::kernel::GetTitlePaths();
}

RuntimeSettings& TitleRuntime::GetRuntimeSettings() {
  return rex::kernel::GetRuntimeSettings();
}

KernelSettings& TitleRuntime::GetKernelSettings() {
  return StateOf(*this).GetKernelSettings();
}

TitleRuntime* GetProcessRuntime() {
  return kernel::TitleState::GetActive();
}

}  // namespace rex::kernel
