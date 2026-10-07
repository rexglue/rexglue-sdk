#include "ui/sdl3/console/window.h"

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/style.h>

#include "ui/sdl3/console/view.h"
#include "ui/sdl3/imgui_drawer.h"
#include "ui/sdl3/window.h"
#include "ui/sdl3/window_listener.h"

namespace rex::ui {

class ConsoleWindow::Dialog final : public ImGuiDialog {
 public:
  Dialog(const Style& style, ConsoleModel& model, ConsoleHost& host)
      : style_(style), view_(model, host) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    if (ImGui::Begin("##console_window", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus)) {
      view_.Draw(style_.console);
    }
    ImGui::End();
  }

 private:
  Style style_;
  ConsoleView view_;
};

class ConsoleWindow::Listener final : public WindowListener {
 public:
  explicit Listener(ConsoleHost& host) : host_(host) {}

  bool OnCloseRequested(UIEvent&) override {
    host_.Dock();
    return false;
  }

 private:
  ConsoleHost& host_;
};

ConsoleWindow::ConsoleWindow(Key) {}

ConsoleWindow::~ConsoleWindow() {
  dialog_.reset();
  if (drawer_) {
    drawer_->SetPresenterAndOverlayDrawer(nullptr, nullptr);
    drawer_.reset();
  }
  overlay_drawer_.reset();
  if (window_) {
    window_->SetPresenter(nullptr);
    window_->RemoveListener(listener_.get());
  }
  presenter_.reset();
  window_.reset();
}

Result<std::unique_ptr<ConsoleWindow>> ConsoleWindow::Create(
    WindowedAppContext& context, gpu::IBackend& backend, const ImGuiStyle& imgui_style,
    const Style& style, ConsoleModel& model, ConsoleHost& host) {
  using Created = std::unique_ptr<ConsoleWindow>;
  Created console = std::make_unique<ConsoleWindow>(Key{});
  console->window_ = Window::Create(context, "Console");
  if (!console->window_) {
    return Err<Created>(ErrorCategory::Runtime, "console window creation failed");
  }
  console->listener_ = std::make_unique<Listener>(host);
  console->window_->AddListener(console->listener_.get());
  console->window_->SetDesiredLogicalSize(960, 540);
  if (!console->window_->Open()) {
    return Err<Created>(ErrorCategory::Runtime, "console window open failed");
  }
  auto presenter = backend.CreatePresenter();
  if (!presenter) {
    return std::unexpected(presenter.error());
  }
  console->presenter_ = std::move(*presenter);
  console->window_->SetPresenter(console->presenter_.get());
  auto overlay_drawer = backend.CreateOverlayDrawer(*console->presenter_);
  if (!overlay_drawer) {
    return std::unexpected(overlay_drawer.error());
  }
  console->overlay_drawer_ = std::move(*overlay_drawer);
  console->drawer_ = std::make_unique<ImGuiDrawer>(console->window_.get(), 0);
  console->drawer_->imgui_style() = imgui_style;
  console->drawer_->SetPresenterAndOverlayDrawer(console->presenter_.get(),
                                                 console->overlay_drawer_.get());
  console->dialog_ = std::make_unique<Dialog>(style, model, host);
  console->drawer_->AddDialog(*console->dialog_);
  return console;
}

}  // namespace rex::ui
