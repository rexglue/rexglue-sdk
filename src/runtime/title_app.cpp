/**
 * @remarks     Derived from Xenia's ui/windowed_app (Ben Vanik, 2020).
 */

#include <rex/ui/title_app.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <mutex>

#include <rex/filesystem.h>
#include <rex/gpu.h>
#include <rex/kernel/init.h>
#include <rex/kernel/shim/heap.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/file.h>
#include <rex/platform/thread.h>
#include <rex/runtime.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xthread.h>
#include <rex/string/utf8.h>
#include <rex/system.h>
#include <rex/version.h>

#include "core/app_state.h"

#if REX_PLATFORM_WIN32
#include <objbase.h>
#include <shellapi.h>
#endif

namespace rex {
namespace detail {

#if REX_PLATFORM_WIN32
CLIArgList WindowsCommandLine() {
  i32 count = 0;
  LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!wide) {
    return CLIArgList::Parse(std::span<const std::string>{});
  }
  std::vector<std::string> args;
  for (i32 index = 1; index < count; ++index) {
    args.push_back(rex::to_utf8(reinterpret_cast<const char16_t*>(wide[index])));
  }
  LocalFree(static_cast<HLOCAL>(wide));
  return CLIArgList::Parse(args);
}

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
    case runtime::TitleExitReason::NormalExit:
      return "The title has exited.";
    case runtime::TitleExitReason::HardExit:
      return "The title returned to the dashboard.";
    case runtime::TitleExitReason::TitleDataError:
      return "The title could not read its data from the disc.";
  }
  return "The title has exited.";
}

}  // namespace

bool InvokeFunction(std::move_only_function<void()> fn) {
  std::lock_guard lock(g_invoke_mutex);
  if (!g_invoke_target) {
    return false;
  }
  return g_invoke_target->Post(std::move(fn));
}

ImFontAtlas& TitleUI::Fonts() {
  return *app_.ui_->Fonts();
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

IUserInterface& TitleUI::Overlays() {
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

Result<void> TitleApp::Init(const CLIArgList& args, const PPCImageInfo& image) {
  image_ = image;
  const std::filesystem::path exe = platform::GetExecutablePath();
  const std::string name = exe.stem().string();
  Config& cfg = config();
  cfg.modules = {"rex-apu-sdl3", "rex-hid-sdl3", "rex-gpu-xenos", "rex-ui-sdl3"};
  cfg.paths.user_data_root = platform::GetUserFolder() / name;
  cfg.paths.cache_root = cfg.paths.user_data_root / "cache";
  cfg.paths.config_file = exe.parent_path() / (name + ".toml");
  REX_TRY_VOID(App::Init(args));
  ui_ = api().Get<IUserInterface>();
  if (!ui_) {
    REXLOG_INFO(ui, "no module serves rex.ui, running headless");
    ui_ = &null_ui_;
  }
  SetInvokeTarget(ui_);
  return Ok();
}

Result<void> TitleApp::CreateTitleWindow() {
  if (ShutdownRequested()) {
    return Ok();
  }
  if (!ui_) {
    return LaunchFailure("CreateTitleWindow needs Init first");
  }
  com_initialized_ = ::rex::detail::InitializeCOM();
  ::rex::detail::SetShutdownWake([ui = ui_] { ui->Quit(); });

  gpu_ = api().Get<gpu::IBackend>();
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

  UIConfig ui_config = api().config().ui;
  if (ui_config.title.empty()) {
    ui_config.title = platform::GetExecutablePath().stem().string() + " " + REXGLUE_BUILD_TITLE;
  }
  auto opened =
      ui_->OpenWindow(ui_config, [this](const UIEvent& event) { return OnWindowEvent(event); });
  if (!opened) {
    return LaunchFailure(std::format("window open failed, {}", opened.error().message));
  }

  gpu::IPresenter* presenter = gpu_ ? gpu_->presenter() : nullptr;
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
  return running && !ShutdownRequested();
}

void TitleApp::Run() {
  if (!ui_ || ShutdownRequested()) {
    return;
  }
  ui_->Run();
}

bool TitleApp::OnWindowEvent(const UIEvent& event) {
  const bool consumed = events_ready_ && OnEvent(event);
  if (!consumed && event.type() == UIEvent::Type::Close) {
    RequestShutdown(0);
  }
  return consumed;
}

Result<void> TitleApp::LaunchFailure(std::string_view message) {
  return Fail("title", Error(ErrorCategory::Runtime, std::string(message)));
}

Result<void> TitleApp::LaunchTitle() {
  if (ShutdownRequested()) {
    return Ok();
  }
  if (!events_ready_) {
    return LaunchFailure("LaunchTitle needs CreateTitleWindow first");
  }
  const PathsConfig& paths = api().config().paths;
  if (paths.game_data_root.empty()) {
    return LaunchFailure("--game_data_root was not provided");
  }
  if (!std::filesystem::is_directory(paths.game_data_root)) {
    return LaunchFailure(
        std::format("--game_data_root does not exist: {}", paths.game_data_root.string()));
  }

  runtime_ = std::make_unique<Runtime>(api().config());

  RuntimeConfig runtime_config;
  runtime_config.kernel_init = kernel::InitializeKernel;
  runtime_config.gpu = gpu_;
  runtime_config.file_system = api().Get<filesystem::IFileSystem>();
  runtime_config.video_mode = ui_->GetVideoMode();
  runtime_config.ui = ui_;
  if (auto setup = runtime_->Setup(image_, runtime_config); !setup) {
    return LaunchFailure(std::format("runtime setup failed, {}", setup.error().message));
  }
  if (image_.register_modules) {
    image_.register_modules(runtime_->module_registry());
  }

  constexpr std::string_view kImage = "game:\\default.xex";
  if (!std::filesystem::is_regular_file(paths.game_data_root / "default.xex")) {
    return LaunchFailure(std::format("Entrypoint XEX not found: {}",
                                     (paths.game_data_root / "default.xex").string()));
  }
  if (auto loaded = runtime_->LoadXEXImage(kImage); !loaded) {
    return LaunchFailure(
        std::format("Failed to load XEX ({}), {}", kImage, loaded.error().message));
  }
  if (image_.shim_heap &&
      !kernel::shim::InitHeap(api().config().runtime.heap_size_mb, runtime_->memory())) {
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
  if (gpu_ && !runtime_->cache_root().empty()) {
    if (const u32 title_id = runtime_->content_manager().title_id(); title_id != 0) {
      if (auto storage = gpu_->InitializeShaderStorage(runtime_->cache_root(), title_id, true);
          !storage) {
        REXLOG_WARN(ui, "{}", storage.error().message);
      }
    }
  }
  runtime_->OnTitleExit([this](runtime::TitleExitReason reason) {
    InvokeFunction([this, reason] { OnTitleExit(reason); });
  });
  runtime_->thread_registry().FindByHandle(main_thread)->Resume();
  title_monitor_ = platform::Spawn("Title Monitor", [this, main_thread](const std::stop_token&) {
    runtime::WaitOn(*runtime_, main_thread, 0, 0, 0, nullptr);
    (void)runtime_->object_table().Close(main_thread);
    runtime_->ReportTitleExit(runtime::TitleExitReason::NormalExit);
  });
  return Ok();
}

void TitleApp::RunSynchronous(std::move_only_function<void()> work) {
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
  if (ShutdownRequested() || !ui_ || !ui_->GetWindow()) {
    RequestShutdown(0);
    return;
  }
  ui_->Prompt(
      {.title = "Title exited", .body = ExitMessage(reason), .buttons = {"Restart", "Exit"}},
      [](const PromptResult& result) {
        if (result.cancelled) {
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
  if (config().ui.video_driver == "dummy") {
    return;
  }
  if (ui_ && ui_ != &null_ui_) {
    ui_->ShowMessage(MessageKind::Error, platform::GetExecutablePath().stem().string(), message);
  } else {
    ShowSimpleMessageBox(SimpleMessageBoxType::Error, message);
  }
}

platform::CrashReporterOptions TitleApp::CrashOptions() {
  return {.show_dialog = config().ui.video_driver != "dummy"};
}

void TitleApp::RequireModules(Config& config) {
  auto& modules = config.modules;
  auto filesystem = std::ranges::find(modules, "rex-filesystem");
  if (filesystem == modules.end()) {
    filesystem = modules.insert(modules.begin(), "rex-filesystem");
  }
  if (std::ranges::find(modules, "rex-runtime") == modules.end()) {
    modules.insert(std::next(filesystem), "rex-runtime");
  }
}

void TitleApp::Stop() {
  if (ui_) {
    ui_->Quit();
    ui_->CancelPrompts();
    ui_->SetRuntime(nullptr);
  }
  if (runtime_ && runtime_->is_setup()) {
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

IUserInterface& TitleApp::UI() {
  return *ui_;
}

}  // namespace ui
}  // namespace rex
