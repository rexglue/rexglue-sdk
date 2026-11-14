/**
 * @remarks     Derived from Xenia's ui/windowed_app (Ben Vanik, 2020).
 */

#include <rex/app/title_app.h>

#include <filesystem>
#include <format>
#include <string>

#include <rex/platform.h>
#include <rex/platform/file.h>
#include <rex/platform/process.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_paths.h>
#include <rex/kernel/title_exit.h>
#include <rex/ui/ui_system.h>

#include "app/lifecycle.h"
#include "ui/ui_control.h"

#if REX_PLATFORM_WIN32
#include <objbase.h>
#include <shellapi.h>
#endif

namespace rex {
namespace app::detail {

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

}  // namespace app::detail
}  // namespace rex

namespace rex::app {

namespace {

std::string ExitMessage(rex::kernel::TitleExitReason reason) {
  switch (reason) {
    case rex::kernel::TitleExitReason::kNormalExit:
      return "The title has exited.";
    case rex::kernel::TitleExitReason::kHardExit:
      return "The title returned to the dashboard.";
    case rex::kernel::TitleExitReason::kTitleDataError:
      return "The title could not read its data from the disc.";
    case rex::kernel::TitleExitReason::kBugCheck:
      return "The title stopped with a system error.";
  }
  return "The title has exited.";
}

}  // namespace

struct TitleApp::State {
  explicit State(const ppc::PPCImage& image) : runtime(image) {}

  rex::kernel::TitleRuntime runtime;
  ScopeGuard exit_guard;
  bool com_initialized = false;
  bool unloading = false;
  bool stopped = false;
};

TitleApp::TitleApp(std::string_view app_name, const ppc::PPCImage& image)
    : App(app_name), state_(std::make_unique<State>(image)) {
  state_->com_initialized = ::rex::app::detail::InitializeCOM();
}

TitleApp::~TitleApp() {
  if (state_->com_initialized) {
    ::rex::app::detail::ShutdownCOM();
  }
}

rex::kernel::TitleRuntime& TitleApp::GetTitleRuntime() {
  return state_->runtime;
}

AppSysRequirements TitleApp::GetRequirements() const {
  AppSysRequirements needs;
  needs.graphics_family = GraphicsFamily::kXenos;
  needs.audio = true;
  needs.input = true;
  needs.ui = true;
  return needs;
}

void TitleApp::OnRun() {
  if (IsShutdownRequested()) {
    return;
  }
  while (RunFrame()) {}
}

Result<void> TitleApp::LaunchTitle() {
  if (IsShutdownRequested() || state_->unloading || state_->stopped) {
    return Ok();
  }
  const auto failure = [this](std::string_view message) {
    return Fail("title", Error(ErrorCategory::kRuntime, std::string(message)));
  };
  rex::kernel::TitleRuntime& title = state_->runtime;
  const std::filesystem::path game_root = title.GetTitlePaths().game_root.Get();
  if (game_root.empty()) {
    return failure("--game_data_root was not provided");
  }
  if (!std::filesystem::is_directory(game_root)) {
    return failure(std::format("--game_data_root {} does not exist", game_root.string()));
  }

  SetPhase(AppPhase::kTitleLaunch);
  const auto launched = [&]() -> Result<void> {
    if (auto loaded = title.Load(); !loaded) {
      return failure(loaded.error().message);
    }
    state_->exit_guard = title.OnTitleExit([this](rex::kernel::TitleExitReason reason) {
      GetUI().PostTask([this, reason] { OnTitleExit(reason); });
    });
    if (auto hooked = OnTitleLaunch(title); !hooked) {
      title.Unload();
      return Fail("OnTitleLaunch", hooked.error());
    }
    if (auto started = title.Launch(); !started) {
      title.Unload();
      return failure(started.error().message);
    }
    return Ok();
  }();
  SetPhase(launched ? AppPhase::kTitleRunning : AppPhase::kRunning);
  return launched;
}

void TitleApp::UnloadTitle() {
  state_->unloading = true;
  SetPhase(AppPhase::kTitleStop);
  GetUI().CancelPrompts();
  state_->runtime.Unload();
  SetPhase(AppPhase::kRunning);
  state_->unloading = false;
}

void TitleApp::RestartTitle() {
  if (IsShutdownRequested() || state_->unloading || state_->stopped) {
    return;
  }
  UnloadTitle();
  if (!LaunchTitle()) {
    ::rex::app::RequestShutdown(1);
  }
}

Result<void> TitleApp::OnTitleLaunch(rex::kernel::TitleRuntime&) {
  return Ok();
}

void TitleApp::OnTitleExit(rex::kernel::TitleExitReason reason) {
  if (state_->unloading || state_->stopped) {
    return;
  }
  if (IsShutdownRequested() || !GetUI().HasWindow()) {
    ::rex::app::RequestShutdown(0);
    return;
  }
  GetUI().ShowPrompt(
      {.title = "Title exited", .body = ExitMessage(reason), .buttons = {"Restart", "Exit"}},
      [this](const ui::PromptResult& result) {
        if (result.canceled) {
          return;
        }
        if (result.button == 0) {
          GetUI().PostTask([this] { RestartTitle(); });
        } else {
          ::rex::app::RequestShutdown(0);
        }
      });
}

void TitleApp::ReportError(std::string_view message) {
  App::ReportError(message);
  if (rex::ui::IUISystem* ui = FindUI(); ui && ui->HasWindow()) {
    ui->ShowMessage(MessageType::kError, GetName(), message);
    return;
  }
  if (!IsHeadlessRequested()) {
    detail::ShowStartupBox(MessageType::kError, std::format("{}\n\n{}", GetName(), message));
  }
}

platform::CrashReporterOptions TitleApp::GetCrashOptions() {
  const rex::ui::IUISystem* ui = FindUI();
  return {.show_dialog = (ui && ui->HasWindow()) || !IsHeadlessRequested()};
}

void TitleApp::OnStop() {
  state_->stopped = true;
  if (rex::ui::IUISystem* ui = FindUI()) {
    GetControlFactory().Get<ui::IUIControl>()->EndFrameLoop();
    ui->CancelPrompts();
  }
  state_->runtime.Unload();
}

void TitleApp::OnRelease() {
  if (state_->com_initialized) {
    ::rex::app::detail::ShutdownCOM();
    state_->com_initialized = false;
  }
}

}  // namespace rex::app
