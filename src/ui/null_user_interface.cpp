#include <rex/ui/null_user_interface.h>

#include <chrono>
#include <utility>

#include <rex/logging/log.h>
#include <rex/move_only_function.h>

namespace rex::ui {

class NullUserInterface::NullWindow final : public IWindow, public gpu::IPresentationHost {
 public:
  void SetTitle(std::string_view) override {}
  void SetAspectRatio(AspectRatio) override {}
  void SetMinimumSize(WindowSize) override {}
  void SetFullscreen(bool fullscreen) override { fullscreen_ = fullscreen; }
  bool IsFullscreen() const override { return fullscreen_; }
  void SetMonitor(i32) override {}
  void Raise() override {}
  void SetCursorVisibility(CursorVisibility) override {}
  void SetTextInputActive(bool) override {}
  u32 GetWidth() const override { return 0; }
  u32 GetHeight() const override { return 0; }
  bool IsFocused() const override { return false; }
  NativeWindow GetNative() override { return NativeWindow{}; }
  gpu::IPresentationHost& GetPresentation() override { return *this; }

  void RequestPaint() override {}
  void OnPresenterDetached() override {}

 private:
  bool fullscreen_ = false;
};

NullUserInterface::NullUserInterface() : ui_thread_(std::this_thread::get_id()) {}

NullUserInterface::~NullUserInterface() = default;

Result<IWindow*> NullUserInterface::OpenWindow(const UIConfig&, EventCallback) {
  if (!window_) {
    window_ = std::make_unique<NullWindow>();
  }
  closed_ = false;
  return window_.get();
}

void NullUserInterface::CloseWindow() {
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
  }
  window_.reset();
  wake_.notify_all();
}

IWindow* NullUserInterface::GetWindow() {
  return window_.get();
}

void NullUserInterface::Run() {
  while (PumpFrame()) {}
}

void NullUserInterface::RunPosted() {
  std::deque<rex::MoveOnlyFunction<void()>> work;
  {
    std::lock_guard lock(mutex_);
    work.swap(queue_);
  }
  for (auto& item : work) {
    item();
  }
}

bool NullUserInterface::PumpFrame() {
  RunPosted();
  {
    std::unique_lock lock(mutex_);
    if (closed_ || quit_) {
      return false;
    }
    wake_.wait_for(lock, std::chrono::milliseconds(16),
                   [this] { return !queue_.empty() || closed_ || quit_; });
  }
  RunPosted();
  std::lock_guard lock(mutex_);
  return !closed_ && !quit_;
}

void NullUserInterface::EndFrameLoop() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  wake_.notify_all();
}

bool NullUserInterface::IsUIThread() const {
  return std::this_thread::get_id() == ui_thread_;
}

bool NullUserInterface::Post(rex::MoveOnlyFunction<void()> work) {
  {
    std::lock_guard lock(mutex_);
    if (closed_ || quit_) {
      return false;
    }
    queue_.push_back(std::move(work));
  }
  wake_.notify_one();
  return true;
}

void NullUserInterface::RunSynchronous(rex::MoveOnlyFunction<void()> work) {
  if (IsUIThread()) {
    work();
    return;
  }
  enum class State : u8 { kQueued, kRunning, kFinished, kAbandoned };
  auto state = std::make_shared<State>(State::kQueued);
  auto shared_work = std::make_shared<rex::MoveOnlyFunction<void()>>(std::move(work));
  const bool queued = Post([this, state, shared_work] {
    {
      std::lock_guard lock(mutex_);
      if (*state == State::kAbandoned) {
        return;
      }
      *state = State::kRunning;
    }
    (*shared_work)();
    {
      std::lock_guard lock(mutex_);
      *state = State::kFinished;
    }
    wake_.notify_all();
  });
  if (!queued) {
    return;
  }
  std::unique_lock lock(mutex_);
  wake_.wait(lock, [&] {
    return *state == State::kFinished || ((closed_ || quit_) && *state == State::kQueued);
  });
  if (*state == State::kQueued) {
    *state = State::kAbandoned;
  }
}

ImFontAtlas* NullUserInterface::GetFonts() {
  return nullptr;
}

ImGuiStyle* NullUserInterface::GetImGuiStyle() {
  return nullptr;
}

ImGuiIO* NullUserInterface::GetImGuiIO() {
  return nullptr;
}

Style& NullUserInterface::GetStyle() {
  return style_;
}

Result<void> NullUserInterface::AddOverlays(std::span<const OverlayDesc>) {
  return Ok();
}

void NullUserInterface::ShowOverlay(std::string_view, bool) {}

void NullUserInterface::ToggleOverlay(std::string_view) {}

ImGuiDialog* NullUserInterface::GetOverlay(std::string_view) {
  return nullptr;
}

bool NullUserInterface::IsAnyOverlayVisible() const {
  return false;
}

void NullUserInterface::AddDialog(ImGuiDialog&) {}

void NullUserInterface::RemoveDialog(ImGuiDialog&) {}

bool NullUserInterface::HasDialogs() const {
  return false;
}

void NullUserInterface::DrawOverlays(gpu::OverlayDrawContext&) {}

void NullUserInterface::SetPresenter(gpu::IPresenter*) {}

void NullUserInterface::SetOverlayDrawer(std::unique_ptr<gpu::IOverlayDrawer> drawer) {
  overlay_drawer_ = std::move(drawer);
}

gpu::IOverlayDrawer* NullUserInterface::GetOverlayDrawer() {
  return overlay_drawer_.get();
}

void NullUserInterface::SetFrameStats(FrameStatsProvider provider) {
  frame_stats_ = std::move(provider);
}

void NullUserInterface::SetRuntime(Runtime*) {}

VideoMode NullUserInterface::GetVideoMode() const {
  return VideoMode{};
}

void NullUserInterface::ShowMessage(MessageKind kind, std::string_view title,
                                    std::string_view body) {
  switch (kind) {
    case MessageKind::kInfo:
      REXLOG_INFO(ui, "{}: {}", title, body);
      break;
    case MessageKind::kWarning:
      REXLOG_WARN(ui, "{}: {}", title, body);
      break;
    case MessageKind::kError:
      REXLOG_ERROR(ui, "{}: {}", title, body);
      break;
  }
}

Result<u32> NullUserInterface::ShowChoice(const ChoiceDesc& desc) {
  ShowMessage(desc.kind, desc.title, desc.body);
  return u32{0};
}

void NullUserInterface::Prompt(PromptDesc, rex::MoveOnlyFunction<void(PromptResult)> done) {
  done(PromptResult{});
}

void NullUserInterface::CancelPrompts() {}

Result<std::vector<std::filesystem::path>> NullUserInterface::ShowFileDialog(
    const FileDialogDesc&) {
  return std::vector<std::filesystem::path>{};
}

std::vector<Monitor> NullUserInterface::GetDisplays() {
  return {};
}

std::vector<HostLocale> NullUserInterface::GetPreferredLocales() {
  return {};
}

}  // namespace rex::ui
