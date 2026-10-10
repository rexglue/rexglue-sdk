/**
 * @file        ui/null_user_interface.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include <rex/ui/user_interface.h>
#include <rex/move_only_function.h>

namespace rex::ui {

/**
 * A ui provider that opens no real window and runs no ImGui code. TitleApp owns one and never
 * serves it as an interface. The thread that constructs it is its UI thread.
 */
class NullUserInterface final : public IUserInterface {
 public:
  /** Captures the calling thread as the UI thread. */
  NullUserInterface();

  /** Destroys the provider. */
  ~NullUserInterface() override;

  /**
   * Opens a window with no native handle.
   *
   * @param config Ignored.
   * @param callback Ignored, the window never raises an event.
   * @return The window, owned by the provider.
   */
  [[nodiscard]] Result<IWindow*> OpenWindow(const UIConfig& config,
                                            EventCallback callback) override;

  /** Closes the window and ends the frame loop. */
  void CloseWindow() override;

  /**
   * Returns the open window.
   *
   * @return The window, or nullptr when none is open.
   */
  IWindow* GetWindow() override;

  /** Runs frames until Quit or CloseWindow. */
  void Run() override;

  /**
   * Runs the posted work for 1 frame, waiting up to 16 milliseconds for new work.
   *
   * @return True while the loop should continue, false after Quit or CloseWindow.
   */
  bool PumpFrame() override;

  /** Ends the frame loop. Callable from any thread. */
  void Quit() override;

  /**
   * Reports whether the caller is on the thread that constructed the provider.
   *
   * @return True when the calling thread is the UI thread.
   */
  bool IsUIThread() const override;

  /**
   * Queues work for the next PumpFrame. Callable from any thread.
   *
   * @param work The work to run.
   * @return True when the work was queued, false after Quit or CloseWindow.
   */
  bool Post(rex::MoveOnlyFunction<void()> work) override;

  /**
   * Runs work on the UI thread and waits for it. Returns without running the work from another
   * thread after Quit or CloseWindow.
   *
   * @param work The work to run.
   */
  void RunSynchronous(rex::MoveOnlyFunction<void()> work) override;

  /**
   * Returns no font atlas.
   *
   * @return nullptr.
   */
  ImFontAtlas* Fonts() override;

  /**
   * Returns no ImGui style.
   *
   * @return nullptr.
   */
  ImGuiStyle* GetImGuiStyle() override;

  /**
   * Returns no ImGui IO.
   *
   * @return nullptr.
   */
  ImGuiIO* GetImGuiIO() override;

  /**
   * Returns the style the provider holds for consumers to patch.
   *
   * @return The overlay style.
   */
  Style& GetStyle() override;

  /**
   * Accepts a table and constructs nothing.
   *
   * @param table Ignored.
   * @return Ok.
   */
  [[nodiscard]] Result<void> AddOverlays(std::span<const OverlayDesc> table) override;

  /**
   * Does nothing.
   *
   * @param name Ignored.
   * @param visible Ignored.
   */
  void ShowOverlay(std::string_view name, bool visible) override;

  /**
   * Does nothing.
   *
   * @param name Ignored.
   */
  void ToggleOverlay(std::string_view name) override;

  /**
   * Returns no overlay.
   *
   * @param name Ignored.
   * @return nullptr.
   */
  ImGuiDialog* Overlay(std::string_view name) override;

  /**
   * Reports whether any overlay is shown.
   *
   * @return False.
   */
  bool AnyOverlayVisible() const override;

  /**
   * Does nothing.
   *
   * @param dialog Ignored.
   */
  void AddDialog(ImGuiDialog& dialog) override;

  /**
   * Does nothing.
   *
   * @param dialog Ignored.
   */
  void RemoveDialog(ImGuiDialog& dialog) override;

  /**
   * Reports whether any dialog or overlay is drawn.
   *
   * @return False.
   */
  bool HasDialogs() const override;

  /**
   * Draws nothing.
   *
   * @param context Ignored.
   */
  void DrawOverlays(gpu::OverlayDrawContext& context) override;

  /**
   * Does nothing.
   *
   * @param presenter Ignored.
   */
  void SetPresenter(gpu::IPresenter* presenter) override;

  /**
   * Keeps the drawer so a caller can read it back.
   *
   * @param drawer The drawer, or null to drop it.
   */
  void SetOverlayDrawer(std::unique_ptr<gpu::IOverlayDrawer> drawer) override;

  /**
   * Returns the drawer handed to SetOverlayDrawer.
   *
   * @return The drawer, or nullptr when none was set.
   */
  gpu::IOverlayDrawer* OverlayDrawer() override;

  /**
   * Keeps the provider so a caller can set it.
   *
   * @param provider The source, or an empty function for none.
   */
  void SetFrameStats(FrameStatsProvider provider) override;

  /**
   * Does nothing.
   *
   * @param runtime Ignored.
   */
  void SetRuntime(Runtime* runtime) override;

  /**
   * Returns the default video mode.
   *
   * @return A 1280 by 720 mode at 60 hertz.
   */
  VideoMode GetVideoMode() const override;

  /**
   * Logs the message at the level of its kind and shows no box.
   *
   * @param kind Severity.
   * @param title Message title.
   * @param body Message text.
   */
  void ShowMessage(MessageKind kind, std::string_view title, std::string_view body) override;

  /**
   * Logs the message and picks the first button.
   *
   * @param desc The box.
   * @return 0.
   */
  [[nodiscard]] Result<u32> ShowChoice(const ChoiceDesc& desc) override;

  /**
   * Completes the prompt as cancelled at once.
   *
   * @param desc Ignored.
   * @param done Called before the function returns.
   */
  void Prompt(PromptDesc desc, rex::MoveOnlyFunction<void(PromptResult)> done) override;

  /** Does nothing, the provider holds no prompt. */
  void CancelPrompts() override;

  /**
   * Shows no dialog.
   *
   * @param desc Ignored.
   * @return An empty list.
   */
  [[nodiscard]] Result<std::vector<std::filesystem::path>> ShowFileDialog(
      const FileDialogDesc& desc) override;

  /**
   * Lists no displays.
   *
   * @return An empty list.
   */
  std::vector<DisplayInfo> Displays() override;

  /**
   * Lists no locales.
   *
   * @return An empty list.
   */
  std::vector<HostLocale> PreferredLocales() override;

 private:
  class NullWindow;

  void RunPosted();

  std::thread::id ui_thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<rex::MoveOnlyFunction<void()>> queue_;
  std::unique_ptr<NullWindow> window_;
  std::unique_ptr<gpu::IOverlayDrawer> overlay_drawer_;
  FrameStatsProvider frame_stats_;
  Style style_;
  bool closed_ = false;
  bool quit_ = false;
};

}  // namespace rex::ui
