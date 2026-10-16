#include "ui/sdl3/user_interface.h"

#include <chrono>
#include <filesystem>
#include <format>

#include <rex/assert.h>
#include <rex/gpu.h>
#include <rex/logging/logger.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/string/numeric.h>
#include <rex/ui/imgui_style.h>
#include <rex/move_only_function.h>

#include "ui/sdl3/console/model.h"
#include "ui/sdl3/console/window.h"
#include "ui/sdl3/overlay/achievement_toast.h"
#include "ui/sdl3/prompts.h"
#include "ui/sdl3/video_mode.h"

namespace rex::ui {

SDL3UserInterface::SDL3UserInterface() = default;

SDL3UserInterface::~SDL3UserInterface() {
  Disconnect();
}

void SDL3UserInterface::Connect(const Config& config, cvar::ICvarRegistry& cvars,
                                hid::IHIDBackend* hid, gpu::IGPUBackend* gpu,
                                runtime::IAchievementManager* achievements, ILogger* logger) {
  ui_thread_ = std::this_thread::get_id();
  config_ = &config;
  cvars_ = &cvars;
  hid_ = hid;
  gpu_ = gpu;
  achievements_ = achievements;
  logger_ = logger;
  log_capture_ = std::make_unique<log::CaptureSink>();
  if (logger_) {
    logger_->AddSink(*log_capture_);
  }
  binds_.Attach(cvars_);
}

void SDL3UserInterface::Disconnect() {
  if (!cvars_) {
    return;
  }
  CloseWindow();
  ReleaseToast();
  binds_.Attach(nullptr);
  if (logger_ && log_capture_) {
    logger_->RemoveSink(*log_capture_);
  }
  log_capture_.reset();
  logger_ = nullptr;
  achievements_ = nullptr;
  gpu_ = nullptr;
  hid_ = nullptr;
  cvars_ = nullptr;
  config_ = nullptr;
}

bool SDL3UserInterface::IsUIThread() const {
  return std::this_thread::get_id() == ui_thread_;
}

Result<IWindow*> SDL3UserInterface::OpenWindow(const UIConfig& config, EventCallback callback) {
  REX_ASSERT(IsUIThread());
  if (window_) {
    return Err<IWindow*>(ErrorCategory::kRuntime, "a window is already open");
  }
  auto fail = [this](std::string message) {
    CloseWindow();
    return Err<IWindow*>(ErrorCategory::kRuntime, std::move(message));
  };
  callback_ = std::move(callback);
  auto context = std::make_unique<SDLWindowedAppContext>();
  if (!context->Initialize(config)) {
    return Err<IWindow*>(ErrorCategory::kRuntime, "SDL video initialization failed");
  }
  context_ = std::move(context);
  {
    std::lock_guard lock(post_mutex_);
    post_target_ = context_.get();
  }

  window_ = Window::Create(*context_, config.title);
  if (!window_) {
    return fail("window creation failed");
  }
  window_->SetTitle(config.title);
  window_->SetMinimumSize(config.min_size);
  window_->SetAspectRatio(config.aspect_ratio);
  listener_ = std::make_unique<EventListener>(callback_);
  bind_listener_ = std::make_unique<BindListener>(binds_);
  window_->AddListener(listener_.get());
  window_->AddInputListener(listener_.get(), 1000);
  window_->AddInputListener(bind_listener_.get(), 0);
  ApplyWindowCvars();
  if (!window_->Open()) {
    return fail("window open failed");
  }

  drawer_ = std::make_unique<ImGuiDrawer>(window_.get(), 64);
  drawer_->SetDialogCounter(&dialog_count_);
  ApplyDefaultStyle(drawer_->GetImGuiStyle(), style_.palette, style_.metrics);
  applied_style_ = style_;
  console_ = std::make_unique<ConsoleModel>(*cvars_, *log_capture_,
                                            config_->paths.user_data_root / "console_history.txt");
  console_host_ = std::make_unique<ConsoleDockHost>(*this);
  overlay_context_.emplace(ModuleOverlayContext{OverlayContext{*this, *cvars_, *config_}, *console_,
                                                *console_host_, *log_capture_, runtime_,
                                                frame_stats_});
  overlays_ = std::make_unique<OverlayRegistry>(*drawer_, binds_, *overlay_context_);
  if (hid_) {
    hid_host_ = std::make_unique<HIDHost>(*this, *window_, *drawer_, *overlays_, *hid_);
    window_->AddListener(hid_host_.get());
    window_->AddInputListener(hid_host_.get(), 0);
  }
  open_ = true;
  return window_.get();
}

void SDL3UserInterface::ApplyWindowCvars() {
  if (cvars_->Get("ui.fullscreen").value_or("true") == "true") {
    window_->SetFullscreen(true);
  }
  window_->SetMonitor(from_string<i32>(cvars_->Get("ui.monitor").value_or("0"), 0));
  auto on_window_cvar = [this](const char* name, std::function<void(std::string_view)> apply) {
    auto handle = cvars_->OnChange(
        name, [this, apply = std::move(apply)](std::string_view, std::string_view value) {
          Post([this, apply, text = std::string(value)] {
            if (window_) {
              apply(text);
            }
          });
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
    const f64 seconds = from_string<f64>(value);
    if (seconds > 0.0) {
      window_->SetCursorAutoHideDelayMs(static_cast<u32>(seconds * 1000.0));
      window_->SetCursorVisibility(CursorVisibility::kAutoHidden);
    } else {
      window_->SetCursorVisibility(CursorVisibility::kVisible);
    }
  };
  on_window_cvar("ui.cursor_timeout", apply_cursor);
  apply_cursor(cvars_->Get("ui.cursor_timeout").value_or("0"));
}

void SDL3UserInterface::CloseWindow() {
  if (!context_ && !window_) {
    return;
  }
  REX_ASSERT(IsUIThread());
  open_ = false;
  if (context_ && !context_->HasQuitFromUIThread()) {
    context_->QuitFromUIThread();
  }
  ReleaseToast();
  if (drawer_) {
    drawer_->DestroyDialogs();
  }
  if (window_) {
    window_->RequestClose();
  }
  if (hid_host_) {
    hid_host_->Detach();
    if (window_) {
      window_->RemoveInputListener(hid_host_.get());
      window_->RemoveListener(hid_host_.get());
    }
    hid_host_.reset();
  }
  console_window_.reset();
  overlays_.reset();
  overlay_context_.reset();
  console_.reset();
  console_host_.reset();
  if (drawer_) {
    drawer_->SetPresenterAndOverlayDrawer(nullptr, nullptr);
    drawer_.reset();
  }
  overlay_drawer_.reset();
  presenter_ = nullptr;
  window_callbacks_.clear();
  if (window_) {
    window_->SetPresenter(nullptr);
    window_->RemoveInputListener(bind_listener_.get());
    window_->RemoveInputListener(listener_.get());
    window_->RemoveListener(listener_.get());
    window_.reset();
  }
  bind_listener_.reset();
  listener_.reset();
  {
    std::unique_lock lock(post_mutex_);
    post_target_ = nullptr;
    post_idle_.wait(lock, [this] { return sync_calls_ == 0; });
  }
  context_.reset();
  callback_ = nullptr;
}

IWindow* SDL3UserInterface::GetWindow() {
  return window_.get();
}

void SDL3UserInterface::Run() {
  REX_ASSERT(IsUIThread());
  if (context_ && !context_->HasQuitFromUIThread()) {
    context_->RunMainMessageLoop();
  }
}

bool SDL3UserInterface::PumpFrame() {
  REX_ASSERT(IsUIThread());
  if (!context_ || context_->HasQuitFromUIThread()) {
    return false;
  }
  context_->PumpEvents(std::chrono::milliseconds(16));
  return !context_->HasQuitFromUIThread();
}

void SDL3UserInterface::Quit() {
  std::lock_guard lock(post_mutex_);
  SDLWindowedAppContext* target = post_target_;
  if (!target) {
    return;
  }
  target->CallInUIThreadDeferred([target] {
    if (!target->HasQuitFromUIThread()) {
      target->QuitFromUIThread();
    }
  });
}

bool SDL3UserInterface::Post(rex::MoveOnlyFunction<void()> work) {
  auto holder = std::make_shared<rex::MoveOnlyFunction<void()>>(std::move(work));
  std::lock_guard lock(post_mutex_);
  if (!post_target_) {
    return false;
  }
  return post_target_->CallInUIThreadDeferred([holder] { (*holder)(); });
}

void SDL3UserInterface::RunSynchronous(rex::MoveOnlyFunction<void()> work) {
  if (IsUIThread()) {
    work();
    return;
  }
  SDLWindowedAppContext* target = nullptr;
  {
    std::lock_guard lock(post_mutex_);
    target = post_target_;
    if (!target) {
      return;
    }
    ++sync_calls_;
  }
  target->CallInUIThreadSynchronous([&work] { work(); });
  {
    std::lock_guard lock(post_mutex_);
    --sync_calls_;
  }
  post_idle_.notify_all();
}

ImFontAtlas* SDL3UserInterface::GetFonts() {
  return drawer_ ? drawer_->GetIO().Fonts : nullptr;
}

ImGuiIO* SDL3UserInterface::GetImGuiIO() {
  return drawer_ ? &drawer_->GetIO() : nullptr;
}

ImGuiStyle* SDL3UserInterface::GetImGuiStyle() {
  return drawer_ ? &drawer_->GetImGuiStyle() : nullptr;
}

Style& SDL3UserInterface::GetStyle() {
  return style_;
}

Result<void> SDL3UserInterface::AddOverlays(std::span<const OverlayDesc> table) {
  REX_ASSERT(IsUIThread());
  if (!overlays_) {
    return Err<void>(ErrorCategory::kRuntime, "no window is open");
  }
  return overlays_->Add(table);
}

void SDL3UserInterface::ShowOverlay(std::string_view name, bool visible) {
  REX_ASSERT(IsUIThread());
  if (overlays_) {
    overlays_->Show(name, visible);
  }
}

void SDL3UserInterface::ToggleOverlay(std::string_view name) {
  REX_ASSERT(IsUIThread());
  if (overlays_) {
    overlays_->Toggle(name);
  }
}

ImGuiDialog* SDL3UserInterface::Overlay(std::string_view name) {
  REX_ASSERT(IsUIThread());
  return overlays_ ? overlays_->Get(name) : nullptr;
}

bool SDL3UserInterface::IsAnyOverlayVisible() const {
  REX_ASSERT(IsUIThread());
  return overlays_ && overlays_->IsAnyVisible();
}

void SDL3UserInterface::AddDialog(ImGuiDialog& dialog) {
  REX_ASSERT(IsUIThread());
  if (drawer_) {
    drawer_->AddDialog(dialog);
  }
}

void SDL3UserInterface::RemoveDialog(ImGuiDialog& dialog) {
  REX_ASSERT(IsUIThread());
  if (drawer_) {
    drawer_->RemoveDialog(dialog);
  }
}

bool SDL3UserInterface::HasDialogs() const {
  return dialog_count_.load(std::memory_order_relaxed) != 0;
}

void SDL3UserInterface::DrawOverlays(gpu::OverlayDrawContext& context) {
  REX_ASSERT(IsUIThread());
  if (drawer_) {
    drawer_->Draw(context);
  }
}

void SDL3UserInterface::SetPresenter(gpu::IPresenter* presenter) {
  REX_ASSERT(IsUIThread());
  presenter_ = presenter;
  if (!drawer_) {
    return;
  }
  if (!(style_.palette == applied_style_.palette) || !(style_.metrics == applied_style_.metrics)) {
    ApplyDefaultStyle(drawer_->GetImGuiStyle(), style_.palette, style_.metrics);
    applied_style_ = style_;
  }
  drawer_->SetPresenterAndOverlayDrawer(presenter, overlay_drawer_.get());
  if (presenter) {
    window_->SetPresenter(presenter);
    if (hid_host_) {
      hid_host_->Attach();
    }
  }
}

void SDL3UserInterface::SetOverlayDrawer(std::unique_ptr<gpu::IOverlayDrawer> drawer) {
  REX_ASSERT(IsUIThread());
  if (drawer_) {
    drawer_->SetOverlayDrawer(nullptr);
  }
  overlay_drawer_ = std::move(drawer);
  if (drawer_ && presenter_) {
    drawer_->SetOverlayDrawer(overlay_drawer_.get());
  }
  EnsureToast();
}

gpu::IOverlayDrawer* SDL3UserInterface::GetOverlayDrawer() {
  return overlay_drawer_.get();
}

void SDL3UserInterface::SetFrameStats(FrameStatsProvider provider) {
  REX_ASSERT(IsUIThread());
  frame_stats_ = std::move(provider);
}

void SDL3UserInterface::SetRuntime(Runtime* runtime) {
  REX_ASSERT(IsUIThread());
  if (!runtime) {
    ReleaseToast();
    achievements_ = nullptr;
  }
  runtime_ = runtime;
  if (runtime) {
    achievements_ = &runtime->GetAchievementManager();
    EnsureToast();
  }
}

void SDL3UserInterface::EnsureToast() {
  if (!achievements_ || !drawer_ || !overlay_drawer_ || !runtime_ || !overlay_context_) {
    return;
  }
  std::lock_guard lock(toast_mutex_);
  if (toast_) {
    return;
  }
  toast_ = std::make_unique<AchievementToastDialog>(*overlay_context_);
  drawer_->AddDialog(*toast_);
  toast_listener_ =
      achievements_->RegisterNotificationCallback([this](const runtime::AchievementEvent& event) {
        std::lock_guard callback_lock(toast_mutex_);
        if (toast_) {
          toast_->Push(event);
        }
      });
}

void SDL3UserInterface::ReleaseToast() {
  if (toast_listener_ != 0 && achievements_) {
    achievements_->UnregisterCallback(toast_listener_);
  }
  toast_listener_ = 0;
  std::lock_guard lock(toast_mutex_);
  toast_.reset();
}

VideoMode SDL3UserInterface::GetVideoMode() const {
  VideoMode mode;
  detail::ConfiguredVideoMode(mode.width, mode.height);
  mode.refresh_rate = detail::GetConfiguredRefreshRate();
  return mode;
}

void SDL3UserInterface::Prompt(PromptDesc desc, rex::MoveOnlyFunction<void(PromptResult)> done) {
  (void)Post([this, desc = std::move(desc), done = PromptDone(std::move(done))]() mutable {
    if (!drawer_) {
      return;
    }
    drawer_->AddDialog(std::make_unique<PromptDialog>(std::move(desc), std::move(done)));
  });
}

void SDL3UserInterface::CancelPrompts() {
  if (!IsUIThread()) {
    RunSynchronous([this] { CancelPrompts(); });
    return;
  }
  if (drawer_) {
    drawer_->DestroyOwnedDialogs();
  }
}

}  // namespace rex::ui
