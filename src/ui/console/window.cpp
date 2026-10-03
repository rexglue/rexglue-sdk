#include <rex/ui/console/window.h>

#include <imgui.h>

#include <rex/ui/console/view.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/style.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace rex::ui {

class ConsoleWindow::Dialog final : public ImGuiDialog {
 public:
  Dialog(ImGuiDrawer& drawer, ConsoleModel& model, ConsoleHost& host)
      : ImGuiDialog(&drawer), view_(model, host) {
    SetOwned(true);
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    if (ImGui::Begin("##console_window", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus)) {
      view_.Draw(imgui_drawer()->style().console);
    }
    ImGui::End();
  }

 private:
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

ConsoleWindow::ConsoleWindow() = default;

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

Result<std::unique_ptr<ConsoleWindow>> ConsoleWindow::Create(WindowedAppContext& context,
                                                             gpu::IBackend& backend,
                                                             ImGuiDrawer& theme,
                                                             ConsoleModel& model,
                                                             ConsoleHost& host) {
  using Created = std::unique_ptr<ConsoleWindow>;
  Created console(new ConsoleWindow());
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
  const ImGuiStyle theme_imgui_style = theme.imgui_style();
  const Style theme_style = theme.style();
  console->drawer_ = std::make_unique<ImGuiDrawer>(
      console->window_.get(), 0, nullptr,
      [theme_imgui_style, theme_style](ImGuiStyle& imgui_style, Style& style) {
        imgui_style = theme_imgui_style;
        style = theme_style;
      });
  console->drawer_->SetPresenterAndOverlayDrawer(console->presenter_.get(),
                                                 console->overlay_drawer_.get());
  console->dialog_ = std::make_unique<Dialog>(*console->drawer_, model, host);
  return console;
}

}  // namespace rex::ui
