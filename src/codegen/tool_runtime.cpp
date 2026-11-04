#include "tool_runtime.h"

#include <utility>

#include <rex/apu/audio_system.h>
#include <rex/cli.h>
#include <rex/gpu/graphics_system.h>
#include <rex/hid/input_system.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_paths.h>
#include <rex/setting.h>
#include <rex/path_settings.h>
#include <rex/ppc/image.h>
#include <rex/ui/ui_system.h>

namespace rex::codegen {

ToolRuntime::ToolRuntime(std::unique_ptr<rex::kernel::TitleRuntime> runtime)
    : runtime_(std::move(runtime)) {}

ToolRuntime::ToolRuntime(ToolRuntime&& other) noexcept : runtime_(std::move(other.runtime_)) {}

ToolRuntime& ToolRuntime::operator=(ToolRuntime&& other) noexcept {
  if (this != &other) {
    if (runtime_) {
      runtime_->Unload();
    }
    runtime_ = std::move(other.runtime_);
  }
  return *this;
}

ToolRuntime::~ToolRuntime() {
  if (runtime_) {
    runtime_->Unload();
  }
}

ToolApp::ToolApp(std::string_view app_name, std::filesystem::path app_root)
    : App(app_name), app_root_(std::move(app_root)) {}

ToolApp::~ToolApp() {
  Shutdown();
}

Result<void> ToolApp::OnConfigure(SettingsRegistry&) {
  (void)REX_PATHS().user_root.SetText(SettingLayer::kTitle, "{title.game_root}");
  if (!app_root_.empty()) {
    (void)root.SetText(SettingLayer::kTitle, app_root_.string());
  }
  (void)GetUI().GetSettings().driver.SetText(SettingLayer::kTitle, "none");
  (void)GetInput().GetSettings().drivers.SetText(SettingLayer::kTitle, "none");
  (void)GetAudio().GetSettings().driver.SetText(SettingLayer::kTitle, "none");
  (void)GetGraphics().GetSettings().driver.SetText(SettingLayer::kTitle, "none");
  return Ok();
}

Result<void> ToolApp::Start() {
  if (!started_) {
    REX_TRY_VOID(Init(CLIArgList::Parse(std::span<const std::string>{})));
    started_ = true;
  }
  return Ok();
}

Result<ToolRuntime> ToolApp::Open(const std::filesystem::path& game_root, std::string_view entry,
                                  std::span<const std::string> imports) {
  REX_TRY_VOID(Start());
  auto runtime = std::make_unique<rex::kernel::TitleRuntime>(ppc::PPCImage{});
  rex::kernel::TitlePaths& title = runtime->GetTitlePaths();
  REX_TRY_VOID(title.game_root.Set(game_root));
  std::string joined;
  for (const std::string& file : imports) {
    joined += joined.empty() ? file : "|" + file;
  }
  REX_TRY_VOID(title.entry.Set(std::string(entry)));
  REX_TRY_VOID(title.imports.Set(joined));
  REX_TRY_VOID(runtime->Load());
  return ToolRuntime(std::move(runtime));
}

}  // namespace rex::codegen
