/**
 * @file        ui/title_app.cpp
 * @brief       The title app and its lifecycle steps
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's ui/windowed_app (Ben Vanik, 2020).
 */

#include <rex/ui/title_app.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <mutex>

#include <SDL3/SDL_hints.h>
#include <imgui.h>

#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/filesystem.h>
#include <rex/graphics/backend.h>
#include <rex/input/input_system.h>
#include <rex/kernel/crt/heap.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/logging/sink.h>
#include <rex/platform.h>
#include <rex/platform/thread.h>
#include <rex/runtime.h>
#include <rex/string/numeric.h>
#include <rex/system.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/achievement_toast.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context_sdl.h>
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
    const i32 length =
        WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, nullptr, 0, nullptr, nullptr);
    std::string arg(size_t(length > 0 ? length - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, arg.data(), length, nullptr, nullptr);
    args.push_back(std::move(arg));
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
SDLWindowedAppContext* g_invoke_target = nullptr;

void SetInvokeTarget(SDLWindowedAppContext* context) {
  std::lock_guard lock(g_invoke_mutex);
  g_invoke_target = context;
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

class TitleExitPrompt final : public ImGuiDialog {
 public:
  TitleExitPrompt(ImGuiDrawer* drawer, std::string message)
      : ImGuiDialog(drawer), message_(std::move(message)) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    ImGui::OpenPopup("Title exited");
    if (ImGui::BeginPopupModal("Title exited", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted(message_.c_str());
      if (ImGui::Button("Restart")) {
        RequestRestart();
        Close();
      }
      ImGui::SameLine();
      if (ImGui::Button("Exit")) {
        RequestShutdown(0);
        Close();
      }
      ImGui::EndPopup();
    }
  }

 private:
  std::string message_;
};

}  // namespace

bool InvokeFunction(std::move_only_function<void()> fn) {
  auto holder = std::make_shared<std::move_only_function<void()>>(std::move(fn));
  std::lock_guard lock(g_invoke_mutex);
  if (!g_invoke_target) {
    return false;
  }
  return g_invoke_target->CallInUIThreadDeferred([holder] { (*holder)(); });
}

class TitleApp::Listener final : public WindowListener, public WindowInputListener {
 public:
  explicit Listener(TitleApp& app) : app_(app) {}

  bool OnCloseRequested(UIEvent& e) override {
    if (!Notify(e)) {
      RequestShutdown(0);
    }
    return false;
  }
  void OnClosing(UIEvent& e) override { Notify(e); }
  void OnResize(UISetupEvent& e) override { Notify(e); }
  void OnGotFocus(UISetupEvent& e) override { Notify(e); }
  void OnLostFocus(UISetupEvent& e) override { Notify(e); }
  void OnDpiChanged(UISetupEvent& e) override { Notify(e); }
  void OnMinimized(UIEvent& e) override { Notify(e); }
  void OnRestored(UIEvent& e) override { Notify(e); }
  void OnFileDrop(FileDropEvent& e) override { Notify(e); }
  void OnKeyDown(KeyEvent& e) override { Forward(e); }
  void OnKeyUp(KeyEvent& e) override { Forward(e); }
  void OnKeyChar(KeyEvent& e) override { Forward(e); }
  void OnMouseDown(MouseEvent& e) override { Forward(e); }
  void OnMouseUp(MouseEvent& e) override { Forward(e); }
  void OnMouseMove(MouseEvent& e) override { Forward(e); }
  void OnMouseWheel(MouseEvent& e) override { Forward(e); }

 private:
  bool Notify(const UIEvent& e) { return app_.events_ready_ && app_.OnEvent(e); }

  template <typename T>
  void Forward(T& e) {
    if (Notify(e)) {
      e.set_handled(true);
    }
  }

  TitleApp& app_;
};

class TitleApp::BindListener final : public WindowInputListener {
 public:
  void OnKeyDown(KeyEvent& e) override { ProcessKeyEvent(e); }
};

ImFontAtlas& TitleUI::fonts() {
  return *app_.drawer_->GetIO().Fonts;
}

ImGuiStyle& TitleUI::style() {
  return app_.drawer_->imgui_style();
}

Style& TitleUI::theme() {
  return app_.drawer_->style();
}

Window& TitleUI::window() {
  return *app_.window_;
}

OverlayRegistry& TitleUI::overlays() {
  return *app_.overlays_;
}

void TitleUI::SetDrawer(std::unique_ptr<ImmediateDrawer> drawer) {
  app_.immediate_drawer_ = std::move(drawer);
}

void TitleUI::SetFrameStats(DebugOverlayDialog::FrameStatsProvider provider) {
  app_.frame_stats_ = std::move(provider);
}

TitleApp::TitleApp() = default;

TitleApp::~TitleApp() = default;

Result<void> TitleApp::Init(const CLIArgList& args, const PPCImageInfo& image) {
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  image_ = image;
  const std::filesystem::path exe = filesystem::GetExecutablePath();
  const std::string name = exe.stem().string();
  Config& cfg = config();
  cfg.modules = {"rexgpu-xenos"};
  cfg.paths.user_data_root = filesystem::GetUserFolder() / name;
  cfg.paths.cache_root = cfg.paths.user_data_root / "cache";
  cfg.paths.config_file = exe.parent_path() / (name + ".toml");
  REX_TRY_VOID(App::Init(args));
  log_capture_ = std::make_shared<LogCaptureSink>();
  AddSink(log_capture_);
  AttachBindRegistry(api().Get<cvar::ICvarRegistry>());
  return Ok();
}

Result<void> TitleApp::CreateTitleWindow() {
  if (ShutdownRequested()) {
    return Ok();
  }
  const Config& cfg = api().config();
  context_ = std::make_unique<SDLWindowedAppContext>();
  if (!context_->Initialize(cfg.ui)) {
    return LaunchFailure("SDL video initialization failed");
  }
  com_initialized_ = ::rex::detail::InitializeCOM();
  SetInvokeTarget(context_.get());
  ::rex::detail::SetShutdownWake([context = context_.get()] {
    InvokeFunction([context] {
      if (!context->HasQuitFromUIThread()) {
        context->QuitFromUIThread();
      }
    });
  });

  graphics_ = api().Get<graphics::IGraphicsBackend>();
  if (graphics_ && XFAILED(graphics_->SetupPresentation(context_.get()))) {
    return LaunchFailure("graphics presentation setup failed");
  }

  const std::string title = cfg.ui.title.empty() ? filesystem::GetExecutablePath().stem().string() +
                                                       " " + REXGLUE_BUILD_TITLE
                                                 : cfg.ui.title;
  window_ = Window::Create(*context_, title);
  if (!window_) {
    return LaunchFailure("window creation failed");
  }
  window_->SetTitle(title);
  window_->SetMinimumSize(cfg.ui.min_size);
  window_->SetAspectRatio(cfg.ui.aspect_ratio);
  listener_ = std::make_unique<Listener>(*this);
  bind_listener_ = std::make_unique<BindListener>();
  window_->AddListener(listener_.get());
  window_->AddInputListener(listener_.get(), 1000);
  window_->AddInputListener(bind_listener_.get(), 0);

  auto* cvars = api().Get<cvar::ICvarRegistry>();
  if (cvars->Get("ui.fullscreen").value_or("true") == "true") {
    window_->SetFullscreen(true);
  }
  window_->SetMonitor(from_string<i32>(cvars->Get("ui.monitor").value_or("0"), 0));
  auto on_window_cvar = [this, cvars](const char* name,
                                      std::function<void(std::string_view)> apply) {
    auto handle =
        cvars->OnChange(name, [apply = std::move(apply)](std::string_view, std::string_view value) {
          InvokeFunction([apply, text = std::string(value)] { apply(text); });
        });
    if (handle) {
      window_callbacks_.push_back(std::move(handle.value()));
    }
  };
  on_window_cvar("ui.fullscreen", [this](std::string_view value) {
    window_->SetFullscreen(from_string<bool>(value, false));
  });
  on_window_cvar("ui.fullscreen_exclusive",
                 [this](std::string_view) { window_->RefreshFullscreen(); });
  on_window_cvar("ui.monitor", [this](std::string_view value) {
    window_->SetMonitor(from_string<i32>(value, 0));
  });
  auto apply_size = [this](std::string_view) {
    u32 width = 0;
    u32 height = 0;
    Window::ResolveConfiguredLogicalSize(width, height);
    window_->SetDesiredLogicalSize(width, height);
  };
  on_window_cvar("ui.window_width", apply_size);
  on_window_cvar("ui.window_height", apply_size);
  on_window_cvar("ui.resolution", [this, apply_size](std::string_view value) {
    apply_size(value);
    window_->RefreshFullscreen();
  });
  auto apply_cursor = [this](std::string_view value) {
    const f64 seconds = from_string<f64>(value, 0.0);
    if (seconds > 0.0) {
      window_->SetCursorAutoHideDelayMs(u32(seconds * 1000.0));
      window_->SetCursorVisibility(Window::CursorVisibility::kAutoHidden);
    } else {
      window_->SetCursorVisibility(Window::CursorVisibility::kVisible);
    }
  };
  on_window_cvar("ui.cursor_timeout", apply_cursor);
  apply_cursor(cvars->Get("ui.cursor_timeout").value_or("0"));

  if (!window_->Open()) {
    return LaunchFailure("window open failed");
  }

  Presenter* presenter = graphics_ ? graphics_->presenter() : nullptr;
  if (presenter && graphics_->provider()) {
    immediate_drawer_ = graphics_->provider()->CreateImmediateDrawer();
    if (immediate_drawer_) {
      immediate_drawer_->SetPresenter(presenter);
    }
  }
  drawer_ = std::make_unique<ImGuiDrawer>(window_.get(), 64);
  overlay_context_ = std::make_unique<OverlayContext>(
      *drawer_, immediate_drawer_, *cvars, *log_capture_, api().config(), runtime_, frame_stats_);
  overlays_ = std::make_unique<OverlayRegistry>(*window_, *drawer_, *overlay_context_);

  TitleUI ui(*this);
  OnCreateWindow(ui);

  drawer_->SetPresenterAndImmediateDrawer(presenter, immediate_drawer_.get());
  if (presenter) {
    window_->SetPresenter(presenter);
  }
  events_ready_ = true;
  return Ok();
}

bool TitleApp::RunFrame() {
  if (context_ && !context_->HasQuitFromUIThread()) {
    context_->PumpEvents(std::chrono::milliseconds(16));
  }
  return !ShutdownRequested();
}

void TitleApp::Run() {
  if (!context_ || ShutdownRequested()) {
    return;
  }
  context_->RunMainMessageLoop();
}

bool TitleApp::CaptureActive() const {
  if (window_ && !window_->HasFocus()) {
    return false;
  }
  if (!overlays_ || !overlays_->AnyVisible()) {
    return true;
  }
  return !drawer_->GetIO().WantCaptureMouse;
}

Result<void> TitleApp::LaunchFailure(std::string message) {
  return Fail("title", Error(ErrorCategory::Runtime, std::move(message)));
}

Result<void> TitleApp::LaunchTitle() {
  if (ShutdownRequested()) {
    return Ok();
  }
  if (!window_) {
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
  runtime_->set_app_context(context_.get());
  runtime_->set_display_window(window_.get());
  runtime_->set_imgui_drawer(drawer_.get());

  RuntimeConfig runtime_config;
  runtime_config.audio_factory = REX_AUDIO_BACKEND(audio::sdl::SDLAudioSystem);
  runtime_config.input_factory = [this](bool tool_mode) {
    return input::CreateDefaultInputSystem(api().config().input, tool_mode);
  };
  runtime_config.kernel_init = kernel::InitializeKernel;
  runtime_config.graphics = graphics_;
  if (XFAILED(runtime_->Setup(image_, std::move(runtime_config)))) {
    return LaunchFailure("runtime setup failed");
  }
  if (auto* input = static_cast<input::InputSystem*>(runtime_->input_system())) {
    input->AttachWindow(window_.get());
    input->SetActiveCallback([this] { return CaptureActive(); });
  }
  if (image_.register_modules) {
    image_.register_modules(runtime_->kernel_state());
  }

  constexpr std::string_view kImage = "game:\\default.xex";
  if (!std::filesystem::is_regular_file(paths.game_data_root / "default.xex")) {
    return LaunchFailure(std::format("Entrypoint XEX not found: {}",
                                     (paths.game_data_root / "default.xex").string()));
  }
  if (XFAILED(runtime_->LoadXEXImage(kImage))) {
    return LaunchFailure(std::format("Failed to load XEX ({})", kImage));
  }
  if (image_.rexcrt_heap &&
      !kernel::crt::InitHeap(api().config().runtime.heap_size_mb, runtime_->memory())) {
    return LaunchFailure("rexcrt heap initialization failed");
  }

  if (immediate_drawer_) {
    toast_ = std::make_shared<AchievementToastDialog>(drawer_.get(), immediate_drawer_.get(),
                                                      runtime_.get());
    toast_->SetOwned(true);
    std::weak_ptr<AchievementNotificationDialog> notification = toast_;
    toast_listener_ = runtime_->kernel_state()->achievements().RegisterNotificationCallback(
        [notification](const system::AchievementEvent& event) {
          if (auto dialog = notification.lock()) {
            dialog->Push(event);
          }
        });
  }

  if (auto launched = OnTitleLaunch(*runtime_); !launched) {
    return Fail("OnTitleLaunch", launched.error());
  }

  auto main_thread = runtime_->PrepareModuleLaunch();
  if (!main_thread) {
    return LaunchFailure("the title's main thread could not be created");
  }
  if (graphics_ && !runtime_->cache_root().empty()) {
    if (const u32 title_id = runtime_->kernel_state()->title_id(); title_id != 0) {
      graphics_->InitializeShaderStorage(runtime_->cache_root(), title_id, true);
    }
  }
  runtime_->OnTitleExit([this](runtime::TitleExitReason reason) {
    InvokeFunction([this, reason] { OnTitleExit(reason); });
  });
  main_thread->Resume();
  title_monitor_ =
      platform::Spawn("Title Monitor",
                      [this, main_thread = std::move(main_thread)](const std::stop_token&) mutable {
                        main_thread->Wait(0, 0, 0, nullptr);
                        runtime_->ReportTitleExit(runtime::TitleExitReason::NormalExit);
                      });
  return Ok();
}

void TitleApp::OnCreateWindow(TitleUI&) {}

bool TitleApp::OnEvent(const UIEvent&) {
  return false;
}

Result<void> TitleApp::OnTitleLaunch(Runtime&) {
  return Ok();
}

void TitleApp::OnTitleExit(runtime::TitleExitReason reason) {
  if (ShutdownRequested() || !drawer_) {
    RequestShutdown(0);
    return;
  }
  new TitleExitPrompt(drawer_.get(), ExitMessage(reason));
}

void TitleApp::ReportError(std::string_view message) {
  App::ReportError(message);
  if (config().ui.video_driver != "dummy") {
    ShowSimpleMessageBox(SimpleMessageBoxType::Error, message);
  }
}

void TitleApp::RequireModules(Config& config) {
  if (std::ranges::find(config.modules, "rexruntime") == config.modules.end()) {
    config.modules.insert(config.modules.begin(), "rexruntime");
  }
}

void TitleApp::Stop() {
  if (context_ && !context_->HasQuitFromUIThread()) {
    context_->QuitFromUIThread();
  }
  if (toast_listener_ != 0 && runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->achievements().UnregisterCallback(toast_listener_);
    toast_listener_ = 0;
  }
  if (drawer_) {
    drawer_->DestroyDialogs();
  }
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  if (window_) {
    window_->RequestClose();
  }
  toast_.reset();
  title_monitor_ = {};
}

void TitleApp::Release() {
  events_ready_ = false;
  overlays_.reset();
  overlay_context_.reset();
  if (drawer_) {
    drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    drawer_.reset();
  }
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  if (window_) {
    window_->SetPresenter(nullptr);
    window_->RemoveInputListener(bind_listener_.get());
    window_->RemoveInputListener(listener_.get());
    window_->RemoveListener(listener_.get());
  }
  window_callbacks_.clear();
  window_.reset();
  runtime_.reset();
  AttachBindRegistry(nullptr);
  SetInvokeTarget(nullptr);
  ::rex::detail::SetShutdownWake({});
  context_.reset();
  if (com_initialized_) {
    ::rex::detail::ShutdownCOM();
    com_initialized_ = false;
  }
}

Runtime* TitleApp::runtime() {
  return runtime_.get();
}

Window& TitleApp::window() {
  return *window_;
}

ImGuiDrawer& TitleApp::drawer() {
  return *drawer_;
}

OverlayRegistry& TitleApp::overlays() {
  return *overlays_;
}

}  // namespace ui
}  // namespace rex
