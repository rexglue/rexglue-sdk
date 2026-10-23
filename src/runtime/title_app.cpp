/**
 * @remarks     Derived from Xenia's ui/windowed_app (Ben Vanik, 2020).
 */

#include <rex/runtime/content_manager.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/thread_registry.h>
#include <rex/ui/title_app.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <mutex>

#include <rex/filesystem.h>
#include <rex/gpu.h>
#include <rex/gpu/backend_select.h>
#include <rex/kernel/init.h>
#include <rex/kernel/shim/heap.h>
#include <rex/logging/log.h>
#include <rex/paths.h>
#include <rex/platform.h>
#include <rex/platform/file.h>
#include <rex/platform/thread.h>
#include <rex/runtime.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xthread.h>
#include <rex/string/utf8.h>
#include <rex/system.h>
#include <rex/version.h>
#include <rex/move_only_function.h>

#include "core/app_state.h"

#if REX_PLATFORM_WIN32
#include <objbase.h>
#include <shellapi.h>
#endif

namespace rex {
namespace detail {

#if REX_PLATFORM_WIN32
bool InitializeCOM() {
  return SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
}

void ShutdownCOM() {
  CoUninitialize();
}
#else
bool InitializeCOM() {
  return false;
}

void ShutdownCOM() {}
#endif

}  // namespace detail

namespace ui {
namespace {

std::mutex g_invoke_mutex;
IUserInterface* g_invoke_target = nullptr;

void SetInvokeTarget(IUserInterface* target) {
  std::lock_guard lock(g_invoke_mutex);
  g_invoke_target = target;
}

std::string ExitMessage(runtime::TitleExitReason reason) {
  switch (reason) {
    case runtime::TitleExitReason::kNormalExit:
      return "The title has exited.";
    case runtime::TitleExitReason::kHardExit:
      return "The title returned to the dashboard.";
    case runtime::TitleExitReason::kTitleDataError:
      return "The title could not read its data from the disc.";
    case runtime::TitleExitReason::kBugCheck:
      return "The title stopped with a system error.";
  }
  return "The title has exited.";
}

}  // namespace

bool InvokeFunction(rex::MoveOnlyFunction<void()> fn) {
  std::lock_guard lock(g_invoke_mutex);
  if (!g_invoke_target) {
    return false;
  }
  return g_invoke_target->Post(std::move(fn));
}

ImFontAtlas& TitleUI::GetFonts() {
  return *app_.ui_->GetFonts();
}

ImGuiStyle& TitleUI::GetImGuiStyle() {
  return *app_.ui_->GetImGuiStyle();
}

Style& TitleUI::GetStyle() {
  return app_.ui_->GetStyle();
}

IWindow& TitleUI::GetWindow() {
  return *app_.ui_->GetWindow();
}

IUserInterface& TitleUI::GetUI() {
  return *app_.ui_;
}

void TitleUI::SetOverlayDrawer(std::unique_ptr<gpu::IOverlayDrawer> drawer) {
  app_.ui_->SetOverlayDrawer(std::move(drawer));
}

void TitleUI::SetFrameStats(FrameStatsProvider provider) {
  app_.ui_->SetFrameStats(std::move(provider));
}

TitleApp::TitleApp() = default;

TitleApp::~TitleApp() = default;

Result<void> TitleApp::Init(const CLIArgList& args, const PPCImage& image) {
  image_ = image;
  const std::filesystem::path exe = platform::GetExecutablePath();
  const std::string name = exe.stem().string();
  Config& cfg = GetConfig();
  cfg.modules = {"rexapu-sdl3", "rexhid-sdl3", "rexgpu-xenos", "rexui-sdl3"};
  auto resolved = ResolvePaths(cfg.paths, name);
  if (!resolved) {
    return std::unexpected(std::move(resolved.error()));
  }
  cfg.paths = std::move(*resolved);
  REX_TRY_VOID(App::Init(args));
  ui_ = GetAPI().Get<IUserInterface>();
  if (!ui_) {
    REXLOG_INFO(ui, "no module serves rex.ui, running headless");
    ui_ = &null_ui_;
  }
  SetInvokeTarget(ui_);
  return Ok();
}

Result<void> TitleApp::CreateTitleWindow() {
  if (IsShutdownRequested()) {
    return Ok();
  }
  if (!ui_) {
    return LaunchFailure("CreateTitleWindow needs Init first");
  }
  com_initialized_ = ::rex::detail::InitializeCOM();
  ::rex::detail::SetShutdownWake([ui = ui_] { ui->EndFrameLoop(); });

  gpu_ = GetAPI().Get<gpu::IGPUBackend>();
  if (gpu_) {
    if (auto presented = gpu_->SetupPresentation(*this); !presented) {
      return LaunchFailure(
          std::format("GPU presentation setup failed, {}", presented.error().message));
    }
  }

  if (ui_ == &null_ui_) {
    events_ready_ = true;
    return Ok();
  }

  UIConfig ui_config = GetAPI().GetConfig().ui;
  if (ui_config.title.empty()) {
    ui_config.title = platform::GetExecutablePath().stem().string() + " " + REXGLUE_BUILD_LABEL;
  }
  auto opened =
      ui_->OpenWindow(ui_config, [this](const UIEvent& event) { return OnWindowEvent(event); });
  if (!opened) {
    return LaunchFailure(std::format("window open failed, {}", opened.error().message));
  }

  gpu::IPresenter* presenter = gpu_ ? gpu_->GetPresenter() : nullptr;
  if (presenter) {
    if (auto drawer = gpu_->CreateOverlayDrawer(*presenter); drawer) {
      ui_->SetOverlayDrawer(std::move(*drawer));
    } else {
      REXLOG_ERROR(ui, "{}", drawer.error().message);
    }
  }

  TitleUI title_ui(*this);
  OnCreateWindow(title_ui);

  ui_->SetPresenter(presenter);
  events_ready_ = true;
  return Ok();
}

bool TitleApp::RunFrame() {
  if (!ui_) {
    return false;
  }
  const bool running = ui_->PumpFrame();
  return running && !IsShutdownRequested();
}

void TitleApp::Run() {
  if (!ui_ || IsShutdownRequested()) {
    return;
  }
  ui_->Run();
}

bool TitleApp::OnWindowEvent(const UIEvent& event) {
  const bool consumed = events_ready_ && OnEvent(event);
  if (!consumed && event.GetType() == UIEvent::Type::kClose) {
    RequestShutdown(0);
  }
  return consumed;
}

Result<void> TitleApp::LaunchFailure(std::string_view message) {
  return Fail("title", Error(ErrorCategory::kRuntime, std::string(message)));
}

Result<void> TitleApp::LaunchTitle() {
  if (IsShutdownRequested()) {
    return Ok();
  }
  if (!events_ready_) {
    return LaunchFailure("LaunchTitle needs CreateTitleWindow first");
  }
  const PathsConfig& paths = GetAPI().GetConfig().paths;
  if (paths.game_data_root.empty()) {
    return LaunchFailure("--game_data_root was not provided");
  }
  if (!std::filesystem::is_directory(paths.game_data_root)) {
    return LaunchFailure(
        std::format("--game_data_root {} does not exist", paths.game_data_root.string()));
  }

  runtime_ = std::make_unique<Runtime>(GetAPI().GetConfig());

  RuntimeDesc runtime_config;
  runtime_config.kernel_init = kernel::InitializeKernel;
  runtime_config.gpu = gpu_;
  runtime_config.file_system = GetAPI().Get<filesystem::IFileSystem>();
  runtime_config.video_mode = ui_->GetVideoMode();
  runtime_config.ui = ui_;
  if (auto setup = runtime_->Setup(image_, runtime_config); !setup) {
    return LaunchFailure(std::format("runtime setup failed, {}", setup.error().message));
  }
  if (image_.register_modules) {
    image_.register_modules(runtime_->GetModuleRegistry());
  }

  constexpr std::string_view kImage = "game:\\default.xex";
  if (!runtime_->GetFileSystem()->Stat(kImage)) {
    return LaunchFailure(std::format("Entrypoint XEX {} was not found", kImage));
  }
  if (auto loaded = runtime_->LoadXEXImage(kImage); !loaded) {
    return LaunchFailure(
        std::format("Failed to load XEX ({}), {}", kImage, loaded.error().message));
  }
  if (image_.shim_heap &&
      !kernel::shim::InitHeap(GetAPI().GetConfig().runtime.heap_size_mb, runtime_->GetMemory())) {
    return LaunchFailure("shim heap initialization failed");
  }

  ui_->SetRuntime(runtime_.get());

  if (auto launched = OnTitleLaunch(*runtime_); !launched) {
    return Fail("OnTitleLaunch", launched.error());
  }

  const X_HANDLE main_thread = runtime_->PrepareModuleLaunch();
  if (!main_thread) {
    return LaunchFailure("the title's main thread could not be created");
  }
  if (gpu_ && !runtime_->GetCacheRoot().empty()) {
    if (const u32 title_id = runtime_->GetContentManager().GetTitleID(); title_id != 0) {
      if (auto storage = gpu_->InitializeShaderStorage(runtime_->GetCacheRoot(), title_id, true);
          !storage) {
        REXLOG_WARN(ui, "{}", storage.error().message);
      }
    }
  }
  runtime_->OnTitleExit([this](runtime::TitleExitReason reason) {
    InvokeFunction([this, reason] { OnTitleExit(reason); });
  });
  runtime_->GetThreadRegistry().FindByHandle(main_thread)->Resume();
  title_monitor_ = platform::Spawn("Title Monitor", [this, main_thread](const std::stop_token&) {
    runtime::Wait(*runtime_, main_thread, 0, 0, 0, nullptr);
    runtime::CloseOwned(runtime_->GetObjectTable(), main_thread);
    runtime_->ReportTitleExit(runtime::TitleExitReason::kNormalExit);
  });
  return Ok();
}

void TitleApp::RunSynchronous(rex::MoveOnlyFunction<void()> work) {
  ui_->RunSynchronous(std::move(work));
}

void TitleApp::OnCreateWindow(TitleUI&) {}

bool TitleApp::OnEvent(const UIEvent&) {
  return false;
}

Result<void> TitleApp::OnTitleLaunch(Runtime&) {
  return Ok();
}

void TitleApp::OnTitleExit(runtime::TitleExitReason reason) {
  if (IsShutdownRequested() || !ui_ || !ui_->GetWindow()) {
    RequestShutdown(0);
    return;
  }
  ui_->Prompt(
      {.title = "Title exited", .body = ExitMessage(reason), .buttons = {"Restart", "Exit"}},
      [](const PromptResult& result) {
        if (result.canceled) {
          return;
        }
        if (result.button == 0) {
          RequestRestart();
        } else {
          RequestShutdown(0);
        }
      });
}

void TitleApp::ReportError(std::string_view message) {
  App::ReportError(message);
  if (GetConfig().ui.video_driver == "dummy") {
    return;
  }
  if (ui_ && ui_ != &null_ui_) {
    ui_->ShowMessage(MessageKind::kError, platform::GetExecutablePath().stem().string(), message);
  } else {
    ShowSimpleMessageBox(SimpleMessageBoxType::kError, message);
  }
}

platform::CrashReporterOptions TitleApp::GetCrashOptions() {
  return {.show_dialog = GetConfig().ui.video_driver != "dummy"};
}

void TitleApp::RequireModules(Config& config) {
  auto& modules = config.modules;
  auto filesystem = std::ranges::find(modules, "rexfilesystem");
  if (filesystem == modules.end()) {
    filesystem = modules.insert(modules.begin(), "rexfilesystem");
  }
  if (std::ranges::find(modules, "rexruntime") == modules.end()) {
    modules.insert(std::next(filesystem), "rexruntime");
  }
  const auto neutral = std::ranges::find(modules, "rexgpu-xenos");
  if (neutral == modules.end() || std::ranges::any_of(modules, [](const std::string& name) {
        return name.starts_with("rexgpu-xenos-");
      })) {
    return;
  }
  const Result<gpu::HostAPI> api = gpu::SelectHostAPI(config.gpu.backend, gpu::kPlatformHostAPIs);
  if (!api || *api == gpu::HostAPI::kNull) {
    return;
  }
  modules.insert(std::next(neutral),
                 *api == gpu::HostAPI::kD3D12 ? "rexgpu-xenos-d3d12" : "rexgpu-xenos-vulkan");
}

void TitleApp::Stop() {
  if (ui_) {
    ui_->EndFrameLoop();
    ui_->CancelPrompts();
    ui_->SetRuntime(nullptr);
  }
  if (runtime_ && runtime_->IsSetup()) {
    runtime_->TerminateTitle();
  }
  if (ui_) {
    ui_->CloseWindow();
  }
  title_monitor_ = {};
}

void TitleApp::Release() {
  events_ready_ = false;
  if (ui_) {
    ui_->SetPresenter(nullptr);
  }
  kernel::shim::ShutdownHeap();
  runtime_.reset();
  SetInvokeTarget(nullptr);
  ::rex::detail::SetShutdownWake({});
  ui_ = nullptr;
  if (com_initialized_) {
    ::rex::detail::ShutdownCOM();
    com_initialized_ = false;
  }
}

Runtime* TitleApp::GetRuntime() {
  return runtime_.get();
}

IWindow& TitleApp::GetWindow() {
  return *ui_->GetWindow();
}

IUserInterface& TitleApp::GetUI() {
  return *ui_;
}

}  // namespace ui
}  // namespace rex
