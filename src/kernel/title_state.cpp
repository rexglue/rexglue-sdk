#include <atomic>
#include <bit>
#include <format>

#include <rex/chrono.h>
#include <rex/filesystem/mount.h>
#include <rex/filesystem/file_system.h>
#include "gpu/graphics_control.h"
#include <rex/kernel/types/thread.h>
#include <rex/ppc/mmio.h>
#include <rex/logging/log_category.h>
#include <rex/ppc/context.h>
#include <rex/ppc/seh.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/xam/achievement_manager.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/language.h>
#include <rex/xam/title_lifecycle.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_paths.h>
#include <rex/kernel/runtime_settings.h>
#include <rex/kernel/kernel_settings.h>
#include <rex/kernel/title_exit.h>
#include <rex/setting.h>
#include <rex/kernel/guest_allocator.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/platform/thread.h>

#include <rex/utility.h>

#include "kernel/title_context.h"
#include "kernel/apu.h"
#include <rex/assert.h>
#include <rex/kernel/object/object.h>
#include <rex/apu/audio_system.h>
#include <rex/kernel/thread_registry.h>
#include "base/settings/settings_control.h"
#include "ui/ui_control.h"
#include "kernel/thread_wait.h"
#include "kernel/kernel_init.h"
#include "kernel/shim/heap.h"
#include <rex/title.h>
#include <rex/ui/ui_system.h>
#include <rex/path_settings.h>

#include "kernel/rexkernel_module.h"
#include "kernel/dispatcher.h"
#include "kernel/title_state.h"
#include "kernel/module_registry.h"
#include "kernel/notification_broadcaster.h"
#include "kernel/object_table.h"
#include "kernel/thread_registry.h"
#include "kernel/settings_access.h"
#include "kernel/categories.h"
#include <rex/kernel/types/content.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/object/notification_broadcaster.h>
#include <rex/filesystem/file_system_factory.h>

namespace rex::kernel {

namespace {

rex::Error SetupError(X_STATUS status, std::string message) {
  return {ErrorCategory::kRuntime, std::move(message), static_cast<int>(status)};
}

rex::kernel::XLanguage ResolveUserLanguage(u32 value, rex::ui::IUISystem* ui) {
  if (value == 0) {
    const KernelServices* services = GetKernelServices();
    return ui && services && services->language ? services->language->DetectHostLanguage(*ui)
                                                : rex::kernel::XLanguage::kEnglish;
  }
  const rex::kernel::XLanguage language = rex::kernel::ToXLanguage(value);
  if (language == rex::kernel::XLanguage::kInvalid) {
    REXSYS_WARN("kernel.user_language {} is not a title language, using English", value);
    return rex::kernel::XLanguage::kEnglish;
  }
  return language;
}

std::atomic<TitleRuntime*> g_active{nullptr};
std::atomic<ui::IUIControl*> g_ui_control{nullptr};

}  // namespace

TitleState::TitleState(TitleRuntime& owner, const ppc::PPCImage& image) : owner_(owner) {
  image_ = image;
  has_image_ = true;
}

TitleState::~TitleState() {
  Unload();
}

void TitleState::ResolveRoots() {
  const TitlePaths& paths = GetTitlePaths();
  game_data_root_ = paths.game_root.Get();
  game_flags_ =
      paths.game_writable.Get() ? filesystem::MountFlags::kWritable : filesystem::MountFlags::kNone;
  user_data_root_ = REX_PATHS().user_root.Get();
  update_data_root_ = paths.update_root.Get();
  metadata_root_ = paths.metadata_root.GetLayer() == SettingLayer::kDefault
                       ? std::filesystem::path()
                       : paths.metadata_root.Get();
}

filesystem::IFileSystem* TitleState::GetFileSystem() const {
  return file_system_.get();
}

bool ServeUI(std::chrono::milliseconds timeout) {
  ui::IUIControl* control = g_ui_control.load(std::memory_order_acquire);
  return control && control->ServePosts(timeout);
}

TitleRuntime* TitleState::GetActive() {
  return g_active.load(std::memory_order_acquire);
}

std::optional<std::filesystem::path> TitleState::FindMetadataPath(
    const std::filesystem::path& relative_path) const {
  if (!metadata_root_.empty()) {
    std::filesystem::path candidate = metadata_root_ / relative_path;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
    return std::nullopt;
  }

  const std::filesystem::path candidates[] = {
      game_data_root_ / "metadata" / relative_path,
      game_data_root_.parent_path() / "metadata" / relative_path,
      game_data_root_ / relative_path,
  };
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
  }
  return std::nullopt;
}

Result<void> TitleState::LoadKernel() {
  if (GetActive() != nullptr) {
    return Err(SetupError(X_STATUS_UNSUCCESSFUL, "another runtime is active"));
  }
  const KernelServices* services = GetKernelServices();
  if (!services) {
    return Err(SetupError(X_STATUS_UNSUCCESSFUL, "the kernel module is not running"));
  }
  if (!services->xam || !services->managers) {
    return Err(SetupError(X_STATUS_UNSUCCESSFUL,
                          "the rexxam module is not loaded, a title cannot launch without it"));
  }
  ResolveRoots();

  auto fail = [this](X_STATUS status, std::string_view reason) -> Result<void> {
    Shutdown();
    return Err(SetupError(status, std::string(reason)));
  };

  rex::InitializeSEH();

  Clock::SetGuestTickFrequency(50000000);
  Clock::SetGuestSystemTimeBase(Clock::QueryHostSystemTime());
  Clock::SetGuestTimeScalar(1.0);

  platform::WidenProcessAffinity();

  ui_ = nullptr;
  user_language_ = ResolveUserLanguage(GetKernelSettings().user_language.Get(), ui_);

  memory_ = std::make_unique<kernel::GuestAddressSpace>();
  if (!memory_->Initialize()) {
    return fail(X_STATUS_UNSUCCESSFUL, "memory init failed");
  }
  memory_->SetBudget(rex::kernel::GetRuntimeSettings().title_memory_mb.Get());
  guest_memory_ = new GuestMemory(*memory_);

  function_dispatcher_ = std::make_unique<kernel::FunctionDispatcher>(memory_.get());

  file_system_ = services->file_system->Create();

  if (auto created = CreateSubsystems(); !created) {
    return fail(X_STATUS_UNSUCCESSFUL, created.error().message);
  }

  if (auto initialized = kernel::InitializeKernel(owner_); !initialized) {
    return fail(X_STATUS_UNSUCCESSFUL, initialized.error().message);
  }

  if (game_data_root_.empty()) {
    REXSYS_WARN("TitleRuntime::Load: no game_data_root, the standard mounts are skipped");
  } else {
    if (auto mounted =
            file_system_->ConfigureStandardMounts(game_data_root_, update_data_root_, game_flags_);
        !mounted) {
      return fail(X_STATUS_UNSUCCESSFUL, mounted.error().message);
    }
  }

  setup_complete_ = true;
  g_active.store(&owner_, std::memory_order_release);
  if (tool_mode_) {
    REXSYS_DEBUG("Runtime initialized in tool mode (no GPU)");
  }
  return Ok();
}

Result<void> TitleState::AttachSystems(gpu::IGraphicsControl* graphics, rex::ui::IUISystem* ui,
                                       ui::IUIControl* ui_control, rex::hid::IInputSystem* input,
                                       rex::apu::IAudioSystem* audio, const VideoMode& video_mode) {
  if (!setup_complete_ || attached_) {
    return Err(SetupError(X_STATUS_UNSUCCESSFUL, "the runtime is not loaded or is attached"));
  }
  auto fail = [this](X_STATUS status, std::string_view reason) -> Result<void> {
    Shutdown();
    return Err(SetupError(status, std::string(reason)));
  };

  video_mode_ = video_mode;
  ui_ = ui;
  ui_control_ = ui_control;
  if (ui_control_) {
    g_ui_control.store(ui_control_, std::memory_order_release);
  }
  user_language_ = ResolveUserLanguage(GetKernelSettings().user_language.Get(), ui_);

  xam_->SetTitleLanguage(user_language_);
  xam_->AttachSystems(ui_, input, video_mode_);

  if (!audio) {
    REXAPU_INFO("no audio system is attached, the title gets no audio clients");
  }
  apu_ = std::make_unique<kernel::APU>(owner_, audio);
  if (auto started = apu_->Setup(); !started) {
    REXAPU_WARN("the APU failed to start, audio is disabled, {}", started.error().message);
    apu_.reset();
  }

  title_context_ = std::make_unique<TitleContext>(owner_);
  graphics_ = graphics;
  if (graphics_) {
    if (auto status = graphics_->AttachTitle(GetTitleInfo(), *title_context_, *title_context_,
                                             *title_context_);
        !status) {
      graphics_ = nullptr;
      return fail(X_STATUS_UNSUCCESSFUL,
                  std::format("graphics attach failed, {}", status.error().message));
    }
    graphics_attached_ = true;
    rex::ppc::AttachMMIO(rex::ppc::MMIOBlock::kGPU, {this, GraphicsReadThunk, GraphicsWriteThunk});
  }
  REXSYS_DEBUG("GPU initialized");

  attached_ = true;
  REXSYS_DEBUG("Runtime initialized successfully");
  return Ok();
}

std::optional<ppc::PPCCallUsage> TitleState::GetCallUsage(PPCFunc* host) const {
  if (func_mappings_ == nullptr || call_usage_ == nullptr || host == nullptr) {
    return std::nullopt;
  }
  for (const PPCFuncMapping* mapping = func_mappings_; mapping->address != 0; ++mapping) {
    if (mapping->host != host) {
      continue;
    }
    for (const ppc::PPCCallUsage* usage = call_usage_; usage->address != 0; ++usage) {
      if (usage->address == mapping->address) {
        return *usage;
      }
    }
    return std::nullopt;
  }
  return std::nullopt;
}

Result<void> TitleState::InstallImage() {
  const rex::ppc::PPCImage& image_info = image_;

  codegen_flags_ = image_info.codegen_flags;
  func_mappings_ = image_info.func_mappings;
  call_usage_ = image_info.call_usage;
  if (image_info.recompiled_code) {
    function_dispatcher_->AddRecompiledCode(image_info.recompiled_code());
  }

  if (!function_dispatcher_->InitializeFunctionTable(image_info.code_base, image_info.code_size,
                                                     image_info.image_base, image_info.image_size,
                                                     /*is_entrypoint=*/true)) {
    Shutdown();
    return Err(SetupError(X_STATUS_UNSUCCESSFUL, "the function table cannot be initialized"));
  }

  if (image_info.func_mappings) {
    int count = 0;
    int duplicates = 0;
    int rejected = 0;
    for (int i = 0; image_info.func_mappings[i].address != 0; ++i) {
      u32 address = static_cast<u32>(image_info.func_mappings[i].address);
      auto* host = image_info.func_mappings[i].host;
      if (!host) {
        continue;
      }
      if (function_dispatcher_->GetFunction(address)) {
        REXSYS_WARN("func_mappings: duplicate guest address {:08X}", address);
        ++duplicates;
      }
      if (!function_dispatcher_->SetFunction(address, host)) {
        ++rejected;
      } else {
        ++count;
      }
    }
    REXSYS_DEBUG("Registered {} recompiled functions ({} duplicates, {} rejected)", count,
                 duplicates, rejected);
    if (rejected > 0) {
      Shutdown();
      return Err(SetupError(X_STATUS_UNSUCCESSFUL,
                            std::format("{} func_mappings entries were rejected", rejected)));
    }
  }

  REXSYS_DEBUG("Runtime setup complete (code {:08X} to {:08X}, image {:08X} to {:08X})",
               image_info.code_base, image_info.code_base + image_info.code_size,
               image_info.image_base, image_info.image_base + image_info.image_size);

  const auto& flags = image_info.codegen_flags;
  REXSYS_DEBUG(
      "Codegen config skip_lr={} skip_msr={} ctr={} xer={} cr={} reserved={} "
      "non_argument={} non_volatile={}",
      flags.skip_lr, flags.skip_msr, flags.ctr_as_local, flags.xer_as_local, flags.cr_as_local,
      flags.reserved_as_local, flags.non_argument_as_local, flags.non_volatile_as_local);

  return Ok();
}

u32 TitleState::GraphicsReadThunk(void* context, u32 address) {
  return static_cast<TitleState*>(context)->graphics_->ReadRegister((address & 0xFFFF) / 4);
}

void TitleState::GraphicsWriteThunk(void* context, u32 address, u32 value) {
  static_cast<TitleState*>(context)->graphics_->WriteRegister((address & 0xFFFF) / 4, value);
}

void TitleState::InitializeGPURingBuffer(u32 address, u32 size_log2) {
  if (graphics_) {
    graphics_->InitializeRingBuffer(address, size_log2);
  }
}

void TitleState::EnableGPUReadPointerWriteBack(u32 address, u32 block_size_log2) {
  if (graphics_) {
    graphics_->EnableReadPointerWriteBack(address, block_size_log2);
  }
}

void TitleState::DetachSystems() {
  if (graphics_attached_) {
    rex::ppc::DetachMMIO(rex::ppc::MMIOBlock::kGPU, this);
    graphics_->DetachTitle();
    graphics_attached_ = false;
  }
  graphics_ = nullptr;
  if (ui_control_) {
    ui::IUIControl* control = ui_control_;
    g_ui_control.compare_exchange_strong(control, nullptr, std::memory_order_acq_rel);
    ui_control_ = nullptr;
  }
  title_context_.reset();
  if (apu_) {
    apu_->Shutdown();
    apu_.reset();
  }
  if (xam_) {
    xam_->DetachSystems();
  }
  attached_ = false;
}

void TitleState::Shutdown() {
  title_launched_.store(false, std::memory_order_release);
  if (settings_control_) {
    settings_control_->Thaw(SettingLife::kUntilLaunch);
    settings_control_ = nullptr;
  }
  TitleRuntime* self = &owner_;
  g_active.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
  if (!setup_complete_ && !memory_) {
    return;
  }
  StopThreads();
  DetachSystems();
  DestroySubsystems();
  function_dispatcher_.reset();
  if (file_system_) {
    file_system_->Clear();
    file_system_.reset();
  }
  delete guest_memory_;
  guest_memory_ = nullptr;
  heap.Shutdown();
  fiber_count = 0;
  audio_submit_logs = 0;
  memory_.reset();
  setup_complete_ = false;
  terminating_title_.store(false, std::memory_order_release);
  terminate_claimed_.store(false, std::memory_order_release);
  {
    std::lock_guard lock(title_exit_mutex_);
    title_exit_callback_ = nullptr;
    title_exit_reported_ = false;
  }
}

void TitleState::SetAudioPaused(bool paused) {
  if (apu_) {
    apu_->SetPaused(paused);
  }
}

bool TitleState::IsAudioPaused() const {
  return apu_ && apu_->IsPaused();
}

Result<void> TitleState::LoadXEXImage(const std::string_view module_path) {
  REXSYS_DEBUG("Loading XEX image {}", std::string(module_path));

  const X_HANDLE handle = kernel::ModuleObject::CreateUserModule(owner_);
  if (!handle) {
    return Err(SetupError(X_STATUS_NO_MEMORY, "no module object for the image"));
  }
  auto module = object_table_->Get<kernel::ModuleObject>(handle);
  X_STATUS status = module->LoadFromFile(owner_, module_path);
  if (XFAILED(status)) {
    kernel::CloseOwned(*object_table_, handle);
    return Err(
        SetupError(status, std::format("cannot load {}, status {:08X}", module_path, status)));
  }

  SetExecutableModule(handle);
  REXSYS_DEBUG("  XEX image loaded successfully");
  return Ok();
}

X_HANDLE TitleState::PrepareModuleLaunch() {
  const std::optional<X_HANDLE> executable = module_registry_->GetExecutable();
  if (!executable) {
    REXSYS_FAIL(0, "TitleRuntime::PrepareModuleLaunch: No executable module loaded");
  }
  auto module = object_table_->Get<kernel::ModuleObject>(*executable);
  if (!module || !module->processor_module->IsExecutable()) {
    REXSYS_FAIL(0, "TitleRuntime::PrepareModuleLaunch: Failed to prepare module");
  }
  SetExecutableModule(*executable);
  REXSYS_DEBUG("Runtime: Preparing module launch...");
  auto thread = std::make_unique<kernel::KernelThread>(owner_, module->user.stack_size, 0,
                                                       module->user.entry_point, 0,
                                                       kernel::kXCreateSuspended, true, true);
  thread->SetName("Main XThread");
  Result<X_HANDLE> launched = thread_registry_->Launch(std::move(thread));
  if (!launched) {
    REXSYS_FAIL(0, "Could not create launch thread, {:08X}", kernel::ToXStatus(launched));
  }
  if (auto launch_thread = thread_registry_->FindByHandle(*launched)) {
    REXSYS_DEBUG("  Module prepared on thread '{}'", launch_thread->GetName());
  }
  return *launched;
}

IObjectTable& TitleState::GetObjectTable() {
  return *object_table_;
}

IThreadRegistry& TitleState::GetThreadRegistry() {
  return *thread_registry_;
}

WorkspaceHeap& TitleState::GetWorkspace() {
  return *workspace_;
}

IModuleRegistry& TitleState::GetModuleRegistry() {
  return *module_registry_;
}

INotificationBroadcaster& TitleState::GetNotificationBroadcaster() {
  return *notifier_;
}

rex::xam::IContentManager& TitleState::GetContentManager() {
  return xam_managers_->GetContentManager();
}

rex::xam::IAchievementManager& TitleState::GetAchievementManager() {
  return xam_managers_->GetAchievementManager();
}

IOverlappedDispatcher& TitleState::GetOverlappedDispatcher() {
  return *dispatcher_;
}

IDPCQueue& TitleState::GetDPCQueue() {
  return *dispatcher_;
}

rex::ScopeGuard TitleState::OnTitleExit(rex::MoveOnlyFunction<void(TitleExitReason)> callback) {
  u64 serial = 0;
  {
    std::lock_guard lock(title_exit_mutex_);
    title_exit_callback_ = std::move(callback);
    serial = ++title_exit_serial_;
  }
  return rex::ScopeGuard([this, serial] {
    std::lock_guard lock(title_exit_mutex_);
    if (title_exit_serial_ == serial) {
      title_exit_callback_ = nullptr;
    }
  });
}

KernelSettings& TitleState::GetKernelSettings() const {
  const KernelServices* services = GetKernelServices();
  REXSYS_CHECK(services, "the kernel settings exist while the kernel module is connected");
  return *services->kernel_settings;
}

void TitleState::ReportTitleLaunched() {
  if (!settings_control_) {
    const KernelServices* services = GetKernelServices();
    if (services && services->controls) {
      settings_control_ = services->controls->Get<ISettingsRegistryControl>();
      if (settings_control_) {
        settings_control_->Freeze(SettingLife::kUntilLaunch);
      }
    }
  }
  title_launched_.store(true, std::memory_order_release);
}

bool TitleState::IsTitleLaunched() const {
  return title_launched_.load(std::memory_order_acquire);
}

void TitleState::ReportTitleExit(TitleExitReason reason) {
  std::lock_guard lock(title_exit_mutex_);
  if (title_exit_reported_) {
    return;
  }
  title_exit_reported_ = true;
  title_launched_.store(false, std::memory_order_release);
  if (title_exit_callback_) {
    title_exit_callback_(reason);
  }
}

Result<void> TitleState::Load() {
  if (!has_image_) {
    return Err(ErrorCategory::kRuntime, "the runtime was not built from an image");
  }
  if (setup_complete_) {
    return Err(ErrorCategory::kRuntime, "the title is already loaded");
  }
  auto fail = [this](std::string message) -> Result<void> {
    Shutdown();
    return Err(ErrorCategory::kRuntime, std::move(message));
  };

  tool_mode_ = image_.code_size == 0;
  if (auto loaded = LoadKernel(); !loaded) {
    return fail(std::format("runtime load failed, {}", loaded.error().message));
  }
  if (!tool_mode_) {
    if (auto installed = InstallImage(); !installed) {
      return fail(std::format("runtime load failed, {}", installed.error().message));
    }
  }
  if (image_.register_modules) {
    image_.register_modules(GetModuleRegistry());
  }
  const TitlePaths& paths = GetTitlePaths();
  const std::string imports = paths.imports.Get();
  for (size_t start = 0; start < imports.size();) {
    const size_t end = imports.find('|', start);
    const std::string_view candidate =
        std::string_view(imports).substr(start, end == std::string::npos ? end : end - start);
    if (!candidate.empty()) {
      GetModuleRegistry().AddImportCandidate(std::format("game:\\{}", candidate));
    }
    start = end == std::string::npos ? imports.size() : end + 1;
  }
  if (const std::string entry = paths.entry.Get(); !entry.empty()) {
    const std::string image = std::format("game:\\{}", entry);
    if (!GetFileSystem()->Stat(image)) {
      return fail(std::format("Entrypoint XEX {} was not found", image));
    }
    if (auto loaded = LoadXEXImage(image); !loaded) {
      return fail(std::format("Failed to load XEX ({}), {}", image, loaded.error().message));
    }
  }
  if (image_.shim_heap &&
      !kernel::shim::InitHeap(rex::kernel::GetRuntimeSettings().heap_size_mb.Get(),
                              GetAddressSpace())) {
    return fail("shim heap initialization failed");
  }
  return Ok();
}

Result<void> TitleState::Launch() {
  if (!IsLoaded() || running_) {
    return Err(ErrorCategory::kRuntime, "the title is not loaded or is running");
  }
  const KernelServices* services = GetKernelServices();
  if (!services || !services->controls) {
    return Err(ErrorCategory::kRuntime, "the kernel was given no controls");
  }
  IInterfaceFactory& controls = *services->controls;
  rex::ui::IUISystem* ui = controls.Get<rex::ui::IUISystem>();
  ui::IUIControl* ui_control = controls.Get<ui::IUIControl>();
  if (!ui || !ui_control) {
    return Err(ErrorCategory::kRuntime, "the app serves no UI system");
  }
  if (!controls.Get<ISettingsRegistryControl>()) {
    return Err(ErrorCategory::kRuntime, "the app serves no settings control");
  }
  rex::hid::IInputSystem* input = controls.Get<rex::hid::IInputSystem>();
  rex::apu::IAudioSystem* audio = controls.Get<rex::apu::IAudioSystem>();
  if (auto attached = AttachSystems(controls.Get<gpu::IGraphicsControl>(), ui, ui_control, input,
                                    audio, ui->GetVideoMode());
      !attached) {
    Unload();
    return Err(ErrorCategory::kRuntime,
               std::format("runtime attach failed, {}", attached.error().message));
  }
  attached_ui_ = ui;
  if (auto attached = ui_control->AttachTitle(GetTitleInfo(), *title_context_); !attached) {
    attached_ui_ = nullptr;
    Unload();
    return attached;
  }

  const X_HANDLE main_thread = PrepareModuleLaunch();
  if (!main_thread) {
    Unload();
    return Err(ErrorCategory::kRuntime, "the title's main thread could not be created");
  }
  ReportTitleLaunched();
  running_ = true;
  GetThreadRegistry().FindByHandle(main_thread)->Resume();
  monitor_ = platform::Spawn("Title Monitor", [this, main_thread](const std::stop_token&) {
    kernel::Wait(owner_, main_thread, 0, 0, 0, nullptr);
    kernel::CloseOwned(GetObjectTable(), main_thread);
    ReportTitleExit(TitleExitReason::kNormalExit);
  });
  return Ok();
}

void TitleState::Unload() {
  if (!setup_complete_ && !memory_) {
    return;
  }
  REX_ASSERT(!attached_ui_ || attached_ui_->IsUIThread(), "Unload runs on the UI thread");
  {
    std::lock_guard lock(title_exit_mutex_);
    title_exit_callback_ = nullptr;
  }
  if (attached_ui_) {
    ui_control_->DetachTitle();
    attached_ui_ = nullptr;
  }
  if (running_ && setup_complete_) {
    TerminateTitle();
  }
  monitor_ = {};
  running_ = false;
  Shutdown();
}

bool TitleState::IsLoaded() const {
  return setup_complete_;
}

bool TitleState::IsRunning() const {
  return running_;
}

TitleInfo TitleState::GetTitleInfo() {
  TitleInfo info;
  info.video_mode = video_mode_;
  if (GetModuleRegistry().GetExecutable()) {
    info.title_id = xam_managers_->GetTitleID();
  }
  return info;
}

u32 TitleState::GetTitleID() const {
  return setup_complete_ ? xam_managers_->GetTitleID() : 0;
}

std::unique_ptr<rex::filesystem::IXDBFTitleData> TitleState::GetTitleXDBF() const {
  return xam_managers_->GetTitleXDBF();
}

void TitleState::LoadAchievements() {
  std::vector<Achievement> achievements;

  const std::unique_ptr<rex::filesystem::IXDBFTitleData> db = xam_managers_->GetTitleXDBF();
  if (db->IsValid()) {
    const rex::kernel::XLanguage language = db->GetSupportedLanguage(user_language_);
    for (const auto& entry : db->GetAchievements()) {
      Achievement info;
      info.id = entry.id;
      info.label = db->GetStringTableEntry(language, entry.label_id);
      info.description = db->GetStringTableEntry(language, entry.description_id);
      info.unachieved_description = db->GetStringTableEntry(language, entry.unachieved_id);
      info.image_id = entry.image_id;
      info.gamerscore = entry.gamerscore;
      info.flags = entry.flags;
      achievements.push_back(std::move(info));
    }
  }

  xam_managers_->GetAchievementManager().ReplaceAchievements(std::move(achievements));
  if (auto metadata_path = FindMetadataPath("achievements.toml")) {
    if (auto loaded = xam_managers_->GetAchievementManager().LoadMetadataFile(*metadata_path);
        !loaded) {
      REXSYS_WARN("achievements metadata {} ignored, {}", rex::string::PathToUTF8(*metadata_path),
                  loaded.error().message);
    }
  }

  const auto user_root = user_data_root_;
  if (!user_root.empty()) {
    xam_managers_->GetAchievementManager().LoadUnlockStateFrom(user_root,
                                                               xam_managers_->GetTitleID());
  }
}

Result<void> TitleState::CreateXAM() {
  std::filesystem::path user_data_root = user_data_root_;
  if (!user_data_root.empty()) {
    user_data_root = std::filesystem::absolute(user_data_root);
  }
  xam_ = GetKernelServices()->xam;
  xam_managers_ = GetKernelServices()->managers;
  if (auto attached = xam_->AttachTitle(rex::xam::XAMKernelInterfaces{
          .objects = *object_table_,
          .threads = *thread_registry_,
          .modules = *module_registry_,
          .notifier = *notifier_,
          .overlapped_dispatcher = *dispatcher_,
          .exit = *this,
          .debug = *this,
          .heap = *memory_,
          .workspace = *workspace_,
          .file_system = *file_system_,
          .file_system_factory = *GetKernelServices()->file_system,
          .membase = memory_->GetMembase(),
          .user_data_root = std::move(user_data_root),
      });
      !attached) {
    return attached;
  }
  xam_->SetTitleLanguage(user_language_);
  return {};
}

void TitleState::WriteTitleDebugString(std::string_view text) {
  REXTITLE_DEV("{}", text);
}

void TitleState::ReleaseXAMApps() {
  if (xam_) {
    xam_->DestroyApps();
  }
}

void TitleState::DestroyXAM() {
  if (xam_) {
    xam_->DetachTitle();
    xam_ = nullptr;
    xam_managers_ = nullptr;
  }
}

}  // namespace rex::kernel
