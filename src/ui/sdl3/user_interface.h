#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/api.h>
#include <rex/cvar/registry.h>
#include <rex/gpu/backend.h>
#include <rex/hid.h>
#include <rex/logging.h>
#include <rex/logging/sink.h>
#include <rex/runtime/achievement_manager.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/user_interface.h>

#include "ui/sdl3/app_context_sdl.h"
#include "ui/sdl3/bind_table.h"
#include "ui/sdl3/console_host.h"
#include "ui/sdl3/hid_host.h"
#include "ui/sdl3/imgui_drawer.h"
#include "ui/sdl3/listener.h"
#include "ui/sdl3/overlay_context.h"
#include "ui/sdl3/overlay_registry.h"
#include "ui/sdl3/window.h"

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class AchievementToastDialog;
class ConsoleModel;
class ConsoleWindow;

class SDL3UserInterface final : public IUserInterface, public IDialogHost {
 public:
  SDL3UserInterface();
  ~SDL3UserInterface() override;

  void Connect(const Config& config, cvar::ICvarRegistry& cvars, hid::IBackend* hid,
               gpu::IBackend* gpu, runtime::IAchievementManager* achievements, ILogger* logger);
  void Disconnect();

  [[nodiscard]] Result<IWindow*> OpenWindow(const UIConfig& config,
                                            EventCallback callback) override;
  void CloseWindow() override;
  IWindow* GetWindow() override;
  void Run() override;
  bool PumpFrame() override;
  void Quit() override;
  bool IsUIThread() const override;
  bool Post(std::move_only_function<void()> work) override;
  void RunSynchronous(std::move_only_function<void()> work) override;
  ImFontAtlas* Fonts() override;
  ImGuiStyle* GetImGuiStyle() override;
  ImGuiIO* GetImGuiIO() override;
  Style& GetStyle() override;
  [[nodiscard]] Result<void> AddOverlays(std::span<const OverlayDesc> table) override;
  void ShowOverlay(std::string_view name, bool visible) override;
  void ToggleOverlay(std::string_view name) override;
  ImGuiDialog* Overlay(std::string_view name) override;
  bool AnyOverlayVisible() const override;
  void AddDialog(ImGuiDialog& dialog) override;
  void RemoveDialog(ImGuiDialog& dialog) override;
  bool HasDialogs() const override;
  void DrawOverlays(gpu::OverlayDrawContext& context) override;
  void SetPresenter(gpu::IPresenter* presenter) override;
  void SetOverlayDrawer(std::unique_ptr<gpu::IOverlayDrawer> drawer) override;
  gpu::IOverlayDrawer* OverlayDrawer() override;
  void SetFrameStats(FrameStatsProvider provider) override;
  void SetRuntime(Runtime* runtime) override;
  VideoMode GetVideoMode() const override;
  void ShowMessage(MessageKind kind, std::string_view title, std::string_view body) override;
  [[nodiscard]] Result<u32> ShowChoice(const ChoiceDesc& desc) override;
  void Prompt(PromptDesc desc, std::move_only_function<void(PromptResult)> done) override;
  void CancelPrompts() override;
  [[nodiscard]] Result<std::vector<std::filesystem::path>> ShowFileDialog(
      const FileDialogDesc& desc) override;
  std::vector<DisplayInfo> Displays() override;
  std::vector<HostLocale> PreferredLocales() override;

 private:
  friend class ConsoleDockHost;

  void ApplyWindowCvars();
  void EnsureToast();
  void ReleaseToast();
  [[nodiscard]] Result<u32> ShowBox(MessageKind kind, std::string_view title, std::string_view body,
                                    std::span<const std::string> buttons);

  const Config* config_ = nullptr;
  cvar::ICvarRegistry* cvars_ = nullptr;
  hid::IBackend* hid_ = nullptr;
  gpu::IBackend* gpu_ = nullptr;
  runtime::IAchievementManager* achievements_ = nullptr;
  ILogger* logger_ = nullptr;
  Runtime* runtime_ = nullptr;
  std::thread::id ui_thread_;
  std::unique_ptr<log::CaptureSink> log_capture_;
  BindTable binds_;
  EventCallback callback_;
  std::unique_ptr<SDLWindowedAppContext> context_;
  std::unique_ptr<Window> window_;
  std::unique_ptr<EventListener> listener_;
  std::unique_ptr<BindListener> bind_listener_;
  std::unique_ptr<HIDHost> hid_host_;
  std::unique_ptr<ImGuiDrawer> drawer_;
  std::unique_ptr<gpu::IOverlayDrawer> overlay_drawer_;
  gpu::IPresenter* presenter_ = nullptr;
  Style style_;
  Style applied_style_;
  FrameStatsProvider frame_stats_;
  std::unique_ptr<ConsoleModel> console_;
  std::unique_ptr<ConsoleDockHost> console_host_;
  std::unique_ptr<ConsoleWindow> console_window_;
  std::optional<ModuleOverlayContext> overlay_context_;
  std::unique_ptr<OverlayRegistry> overlays_;
  std::mutex toast_mutex_;
  std::unique_ptr<AchievementToastDialog> toast_;
  runtime::AchievementListenerHandle toast_listener_ = 0;
  std::vector<cvar::CallbackHandle> window_callbacks_;
  std::mutex post_mutex_;
  std::condition_variable post_idle_;
  u32 sync_calls_ = 0;
  SDLWindowedAppContext* post_target_ = nullptr;
  std::atomic<bool> open_{false};
  std::atomic<u32> dialog_count_{0};
};

}  // namespace rex::ui
