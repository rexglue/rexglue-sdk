#include "ui/ui_system.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <format>

#include <rex/apu/audio_system.h>
#include <rex/path_settings.h>
#include <rex/gpu/graphics_system.h>
#include <rex/hid/input_system.h>
#include <rex/logging/log_category.h>
#include <rex/ui/ui_driver.h>
#include <rex/ui/dialog.h>
#include <rex/ui/window.h>
#include <rex/ui/ui_event.h>
#include <rex/logging/logger.h>
#include <rex/ui/style.h>
#include <rex/utility.h>
#include <rex/ui/overlay_renderer.h>

#include "ui/overlay/console/model.h"
#include "ui/overlay/console_host.h"
#include "ui/overlay/overlay_registry.h"
#include "ui/prompt.h"
#include "ui/ui_state.h"
#include "ui/video_mode.h"

namespace rex::ui {

namespace {

constexpr rex::ui::OverlayConfig kStandardOverlayEntries[] = {
    {"debug", {hid::VirtualKey::kF3}, nullptr},
    {"console", {hid::VirtualKey::kOem3}, nullptr},
    {"settings", {hid::VirtualKey::kF4}, nullptr},
    {"achievements", {hid::VirtualKey::kF7}, nullptr},
};

void LogMessage(MessageType type, std::string_view title, std::string_view body) {
  switch (type) {
    case MessageType::kError:
      REXUI_ERROR("{}: {}", title, body);
      break;
    case MessageType::kWarning:
      REXUI_WARN("{}: {}", title, body);
      break;
    case MessageType::kInfo:
      REXUI_INFO("{}: {}", title, body);
      break;
  }
}

}  // namespace

UISystem::State::State(UISystem& owner_system) : owner(owner_system) {}

UISystem::State::~State() = default;

rex::ui::UISettings::UISettings() : SettingOwner("ui") {}

UISystem::UISystem() : state_(std::make_unique<State>(*this)) {}

UISystem::~UISystem() {
  Stop();
}

bool UISystem::IsHeadless() const {
  return state_->headless.load();
}

bool UISystem::HasWindow() const {
  return state_->window_open.load();
}

bool UISystem::IsFocused() const {
  return state_->window_open.load() && state_->focused.load();
}

bool UISystem::IsUIThread() const {
  return state_->IsUIThread();
}

Result<NativeWindow> UISystem::GetNativeWindow() const {
  if (!state_->window_open.load() || !state_->driver) {
    return Err<NativeWindow>(ErrorCategory::kNotFound, "the window is not open");
  }
  return state_->driver->GetNativeWindow();
}

void UISystem::RaiseWindow() {
  PostTask([this] {
    if (state_->window_open.load()) {
      state_->driver->RaiseWindow();
    }
  });
}

void UISystem::State::WakeLoop() {
  post_wake.notify_all();
  if (driver && window_open.load()) {
    driver->WakeUp();
  }
}

void UISystem::State::ExecutePosts() {
  for (;;) {
    MoveOnlyFunction<void()> fn;
    {
      std::lock_guard lock(post_mutex);
      if (posts.empty()) {
        return;
      }
      fn = std::move(posts.front());
      posts.pop_front();
    }
    try {
      fn();
    } catch (const std::exception& error) {
      REXUI_ERROR("posted work failed, {}", error.what());
    }
  }
}

void UISystem::PostTask(MoveOnlyFunction<void()> fn) {
  {
    std::lock_guard lock(state_->post_mutex);
    if (!state_->stopped) {
      state_->posts.push_back(std::move(fn));
      fn = nullptr;
    }
  }
  if (fn) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      REXUI_WARN("work posted after the UI system stopped is dropped");
    }
    return;
  }
  state_->WakeLoop();
}

bool UISystem::RunSynchronous(MoveOnlyFunction<void()> fn) {
  State& s = *state_;
  if (s.stopped.load()) {
    return false;
  }
  if (s.IsUIThread() || !s.started.load()) {
    fn();
    return true;
  }
  struct Shared {
    std::mutex mutex;
    std::condition_variable done_cv;
    bool done = false;
    MoveOnlyFunction<void()> fn;
  };
  auto shared = std::make_shared<Shared>();
  shared->fn = std::move(fn);
  {
    std::lock_guard lock(s.post_mutex);
    if (s.stopped.load()) {
      return false;
    }
    ++s.sync_calls;
    s.posts.push_back([shared] {
      const auto finish = [&shared] {
        {
          std::lock_guard lock(shared->mutex);
          shared->done = true;
        }
        shared->done_cv.notify_all();
      };
      try {
        shared->fn();
      } catch (...) {
        finish();
        throw;
      }
      finish();
    });
  }
  s.WakeLoop();
  {
    std::unique_lock lock(shared->mutex);
    shared->done_cv.wait(lock, [&] { return shared->done; });
  }
  {
    std::lock_guard lock(s.post_mutex);
    --s.sync_calls;
  }
  s.sync_idle.notify_all();
  return true;
}

bool UISystem::ServePosts(std::chrono::milliseconds timeout) {
  State& s = *state_;
  if (!s.IsUIThread() || !s.started.load()) {
    return false;
  }
  s.ExecutePosts();
  {
    std::unique_lock lock(s.post_mutex);
    s.post_wake.wait_for(lock, timeout, [&] { return !s.posts.empty(); });
  }
  s.ExecutePosts();
  return true;
}

ScopeGuard UISystem::AddEventListener(rex::ui::IUIEventListener& listener) {
  State* state = state_.get();
  {
    std::lock_guard lock(state->listener_mutex);
    state->listeners.push_back(&listener);
  }
  return ScopeGuard([state, &listener] {
    std::lock_guard delivery(state->delivery_mutex);
    std::lock_guard lock(state->listener_mutex);
    std::erase(state->listeners, &listener);
  });
}

ImFontAtlas& UISystem::GetFonts() {
  return state_->imgui->GetFonts();
}

ImGuiStyle& UISystem::GetImGuiStyle() {
  return state_->imgui->GetImGuiStyle();
}

ImGuiIO& UISystem::GetImGuiIO() {
  return state_->imgui->GetIO();
}

ui::Style& UISystem::GetStyle() {
  return state_->style;
}

void UISystem::State::CreateImGui() {
  ui::ImGuiHost::Hooks hooks;
  hooks.set_text_input = [this](bool active) { driver->SetTextInputActive(active); };
  hooks.set_mouse_capture = [this](bool captured) { driver->CaptureMouse(captured); };
  hooks.get_clipboard = [this] { return driver->GetClipboardText(); };
  hooks.set_clipboard = [this](std::string_view text) { driver->SetClipboardText(text); };
  imgui = std::make_unique<ui::ImGuiHost>(std::move(hooks));
  imgui->SetDialogCounter(&dialog_count);
  imgui->SetDisplay(window_width, window_height, content_scale);
  ui::ApplyDefaultStyle(imgui->GetImGuiStyle(), style.palette, style.metrics);
  applied_style = style;
}

void UISystem::State::OpenWindow() {
  rex::ui::WindowConfig config;
  const std::string window_title = owner.GetSettings().title.Get();
  config.title = window_title.empty() ? std::string("ReXGlue") : window_title;
  i32 width = 1280;
  i32 height = 720;
  ui::detail::ResolveConfiguredSize(owner.GetSettings(), width, height);
  config.width = static_cast<u32>(std::max(width, 1));
  config.height = static_cast<u32>(std::max(height, 1));
  config.min_width = static_cast<u32>(owner.GetSettings().min_width.Get());
  config.min_height = static_cast<u32>(owner.GetSettings().min_height.Get());
  config.aspect_ratio = ui::detail::ParseAspectRatio(owner.GetSettings().aspect_ratio.Get());
  config.fullscreen = owner.GetSettings().fullscreen.Get();
  config.fullscreen_exclusive = owner.GetSettings().fullscreen_exclusive.Get();
  config.monitor = owner.GetSettings().monitor.Get();
  config.vulkan_surface = graphics && graphics->GetDriverName() == "vulkan";
  ui::detail::ResolveModeSize(owner.GetSettings(), config.mode_width, config.mode_height);
  if (auto opened = driver->OpenWindow(config); !opened) {
    REXUI_WARN("the window did not open, running headless, {}", opened.error().message);
    headless = true;
    return;
  }
  window_open = true;
  focused = driver->HasFocus();
  const ui::WindowSize size = driver->GetWindowSize();
  window_width = size.width_px;
  window_height = size.height_px;
  content_scale = driver->GetContentScale();
  if (resize_handler) {
    resize_handler(window_width, window_height);
  }
  CreateImGui();
  last_pointer_motion = std::chrono::steady_clock::now();

  ISettingsRegistry& settings = *registry;
  log_capture = std::make_unique<log::LogCaptureSink>();
  log_capture_guard = logger->AddSink(*log_capture);
  binds.SetInput(input);
  binds.Attach(&settings);
  std::filesystem::path history_path;
  history_path = REX_PATHS().user_root.Get() / "console_history.txt";
  console = std::make_unique<ui::ConsoleModel>(settings, *log_capture, std::move(history_path));
  console_commands = std::make_unique<ui::ConsoleCommandTable>(settings);
  if (auto added = settings.Register(*console_commands); !added) {
    REXUI_WARN("{}", added.error().message);
    console_commands.reset();
  }
  console_host = std::make_unique<ui::ConsoleDockHost>(owner);
  frame_stats = [this] { return graphics ? graphics->GetFrameStats() : rex::gpu::FrameStats{}; };
  overlay_context = std::make_unique<ui::ModuleOverlayContext>(ui::ModuleOverlayContext{
      ui::OverlayContext{owner, settings, input ? &input->GetKeyBindings() : nullptr, input,
                         input ? &input->GetSettings() : nullptr},
      *console, *console_host, *log_capture, achievements, frame_stats});
  overlays = std::make_unique<ui::OverlayRegistry>(*imgui, binds, *overlay_context);
  overlays->SetChangeCallback([this] { SyncOverlayBlock(); });
  if (auto added = overlays->Add(kStandardOverlayEntries); !added) {
    REXUI_WARN("{}", added.error().message);
  }
  WatchWindowSettings();
  if (input) {
    pointer_guard =
        input->SetPointerModeListener([this] { owner.PostTask([this] { RefreshCapture(); }); });
    hid::InputEvent resized{
        .type = hid::InputEvent::Type::kResize, .width = window_width, .height = window_height};
    input->OnInputEvent(resized);
    hid::InputEvent focus{.type = focused.load() ? hid::InputEvent::Type::kFocusGained
                                                 : hid::InputEvent::Type::kFocusLost};
    input->OnInputEvent(focus);
    RefreshCapture();
  }
  if (audio) {
    audio->SetFocused(focused.load());
  }
}

void UISystem::State::CloseWindow() {
  if (!window_open.load()) {
    return;
  }
  if (input) {
    pointer_guard.Reset();
    if (applied_pointer_mode != hid::PointerMode::kFree) {
      ApplyPointerMode(hid::PointerMode::kFree);
    }
    input->SetInputCaptured(true);
    if (block_acquired) {
      input->ReleaseUIBlock();
      block_acquired = false;
    }
  }
  watches.clear();
  ReleaseToast();
  achievements = nullptr;
  overlays.reset();
  overlay_context.reset();
  console_host.reset();
  console.reset();
  if (console_commands) {
    registry->Unregister(*console_commands);
    console_commands.reset();
  }
  binds.Attach(nullptr);
  binds.SetInput(nullptr);
  if (log_capture) {
    log_capture_guard.Reset();
    log_capture.reset();
  }
  if (imgui) {
    imgui->DestroyDialogs();
  }
  imgui.reset();
  overlay_visible_count = 0;
  window_open = false;
  driver->CloseUIWindow();
}

Result<void> UISystem::Start(rex::ui::IUIDriver* ui_driver, rex::hid::IInputSystem* input,
                             rex::apu::IAudioSystem& audio, rex::gpu::IGraphicsSystem& graphics,
                             ISettingsRegistry& settings, ILogger& logger) {
  State& s = *state_;
  if (s.started.load()) {
    return Err(ErrorCategory::kRuntime, "the UI system already started");
  }
  s.ui_thread = std::this_thread::get_id();
  s.driver = ui_driver;
  s.input = input;
  s.audio = &audio;
  s.graphics = &graphics;
  s.registry = &settings;
  s.logger = &logger;
  s.headless = !ui_driver || !ui_driver->HasDisplay();
  s.stopping = false;
  s.stopped = false;
  s.quit = false;
  s.started = true;
  if (!s.headless.load()) {
    s.OpenWindow();
  }
  return Ok();
}

void UISystem::BeginStop() {
  State& s = *state_;
  if (!s.started.load()) {
    return;
  }
  s.stopping = true;
  CancelPrompts();
}

void UISystem::Stop() {
  State& s = *state_;
  if (!s.started.load()) {
    return;
  }
  BeginStop();
  {
    std::lock_guard lock(s.post_mutex);
    s.stopped = true;
  }
  s.ExecutePosts();
  s.CloseWindow();
  s.ExecutePosts();
  {
    std::unique_lock lock(s.post_mutex);
    s.sync_idle.wait(lock, [&] { return s.sync_calls == 0; });
  }
  s.driver = nullptr;
  s.input = nullptr;
  s.audio = nullptr;
  s.graphics = nullptr;
  s.headless = true;
  s.started = false;
}

Result<void> UISystem::AddOverlays(std::span<const rex::ui::OverlayConfig> overlays) {
  if (!state_->overlays) {
    return Err(ErrorCategory::kRuntime, "no window is open");
  }
  return state_->overlays->Add(overlays);
}

void UISystem::ShowOverlay(std::string_view name, bool visible) {
  if (state_->overlays) {
    state_->overlays->Show(name, visible);
  }
}

void UISystem::ToggleOverlay(std::string_view name) {
  if (state_->overlays) {
    state_->overlays->Toggle(name);
  }
}

Result<ui::IDialog*> UISystem::GetOverlay(std::string_view name) {
  ui::IDialog* overlay = state_->overlays ? state_->overlays->Get(name) : nullptr;
  if (!overlay) {
    return Err<ui::IDialog*>(ErrorCategory::kNotFound, std::format("no overlay {}", name));
  }
  return overlay;
}

bool UISystem::IsAnyOverlayVisible() const {
  return state_->overlay_visible_count.load() != 0;
}

ScopeGuard UISystem::AddDialog(ui::IDialog& dialog) {
  State* state = state_.get();
  if (!state->imgui) {
    return ScopeGuard();
  }
  const u64 id = state->imgui->AddDialog(dialog);
  return ScopeGuard([state, &dialog, id] {
    if (state->imgui) {
      state->imgui->RemoveDialog(dialog, id);
    }
  });
}

void UISystem::AddDialog(std::unique_ptr<ui::IDialog> dialog) {
  if (state_->imgui) {
    state_->imgui->AddDialog(std::move(dialog));
  }
}

bool UISystem::HasDialogs() const {
  return state_->dialog_count.load(std::memory_order_relaxed) != 0;
}

void UISystem::DrawOverlays(IOverlayRenderer& renderer) {
  State& s = *state_;
  if (s.stopping.load() || s.dialog_count.load(std::memory_order_relaxed) == 0) {
    return;
  }
  std::lock_guard frame(s.frame_mutex);
  ImDrawData* data = nullptr;
  (void)RunSynchronous([&s, &data] {
    if (!s.imgui) {
      return;
    }
    if (!(s.style.palette == s.applied_style.palette) ||
        !(s.style.metrics == s.applied_style.metrics)) {
      ui::ApplyDefaultStyle(s.imgui->GetImGuiStyle(), s.style.palette, s.style.metrics);
      s.applied_style = s.style;
    }
    s.imgui->SetDisplay(s.window_width, s.window_height, s.content_scale);
    data = s.imgui->BuildFrame();
  });
  if (data) {
    renderer.RenderOverlays(*data);
  }
}

void UISystem::ShowMessage(MessageType type, std::string_view heading, std::string_view body) {
  LogMessage(type, heading, body);
  State& s = *state_;
  if (s.headless.load() || !s.driver) {
    return;
  }
  const std::string ok[] = {"OK"};
  if (const auto shown = s.driver->ShowMessageBox(type, heading, body, ok); !shown) {
    REXUI_DEBUG("message box not shown, {}", shown.error().message);
  }
}

Result<u32> UISystem::ShowChoice(const ui::ChoiceConfig& config) {
  LogMessage(config.type, config.title, config.body);
  State& s = *state_;
  if (s.headless.load() || !s.driver) {
    return Err<u32>(ErrorCategory::kPlatform, "there is no display for a message box");
  }
  const std::string ok[] = {"OK"};
  const std::span<const std::string> buttons =
      config.buttons.empty() ? std::span<const std::string>(ok) : std::span(config.buttons);
  return s.driver->ShowMessageBox(config.type, config.title, config.body, buttons);
}

Result<std::vector<std::filesystem::path>> UISystem::ShowFileDialog(
    const ui::FileDialogConfig& config) {
  State& s = *state_;
  if (s.headless.load() || !s.driver) {
    return Err<std::vector<std::filesystem::path>>(ErrorCategory::kPlatform,
                                                   "there is no display for a file dialog");
  }
  return s.driver->ShowFileDialog(config);
}

void UISystem::ShowPrompt(ui::PromptConfig config, MoveOnlyFunction<void(ui::PromptResult)> done) {
  State& s = *state_;
  if (!s.window_open.load()) {
    if (s.IsUIThread() || !s.started.load()) {
      done({});
    } else {
      PostTask([done = std::move(done)]() mutable { done({}); });
    }
    return;
  }
  PostTask([this, config = std::move(config), done = std::move(done)]() mutable {
    State& st = *state_;
    if (!st.imgui) {
      done({});
      return;
    }
    ++st.prompt_count;
    st.SyncOverlayBlock();
    st.imgui->AddDialog(std::make_unique<ui::PromptDialog>(
        std::move(config),
        ui::PromptDone([this, done = std::move(done)](ui::PromptResult r) mutable {
          done(std::move(r));
          State& inner = *state_;
          --inner.prompt_count;
          inner.SyncOverlayBlock();
        })));
  });
}

void UISystem::CancelPrompts() {
  State& s = *state_;
  if (!s.IsUIThread() && s.started.load() && !s.stopping.load()) {
    (void)RunSynchronous([this] { CancelPrompts(); });
    return;
  }
  if (s.imgui) {
    s.imgui->DestroyOwnedDialogs();
  }
}

std::vector<ui::Monitor> UISystem::GetDisplays() {
  State& s = *state_;
  if (s.headless.load() || !s.driver) {
    return {};
  }
  return s.driver->GetDisplays();
}

std::vector<ui::HostLocale> UISystem::GetPreferredLocales() {
  State& s = *state_;
  if (s.headless.load() || !s.driver) {
    return {};
  }
  return s.driver->GetPreferredLocales();
}

void UISystem::State::SyncOverlayBlock() {
  const bool visible = overlays && overlays->IsAnyVisible();
  overlay_visible_count = visible ? 1 : 0;
  const bool exclusive = overlays && overlays->IsExclusiveVisible();
  const bool want = exclusive || prompt_count > 0;
  if (!input) {
    return;
  }
  if (want && !block_acquired) {
    input->AcquireUIBlock();
    block_acquired = true;
  } else if (!want && block_acquired) {
    input->ReleaseUIBlock();
    block_acquired = false;
  }
  RefreshCapture();
}

}  // namespace rex::ui
