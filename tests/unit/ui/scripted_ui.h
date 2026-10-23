/**
 * @file        tests/unit/ui/scripted_ui.h
 * @brief       A user interface double that answers prompts from a script
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include <rex/move_only_function.h>
#include <rex/ui/null_user_interface.h>
#include <rex/ui/user_interface.h>

namespace rex::test {

class ScriptedUI final : public rex::ui::IUserInterface {
 public:
  ScriptedUI() {
    auto opened =
        null_.OpenWindow(rex::ui::UIConfig{}, [](const rex::ui::UIEvent&) { return false; });
    window_ = opened ? *opened : nullptr;
  }

  rex::ui::PromptResult answer;
  std::vector<rex::ui::HostLocale> locales;
  std::atomic<rex::i32> prompts{0};
  bool hold = false;
  rex::MoveOnlyFunction<void(rex::ui::PromptResult)> held;

  void Prompt(rex::ui::PromptDesc request,
              rex::MoveOnlyFunction<void(rex::ui::PromptResult)> done) override {
    {
      std::lock_guard lock(desc_mutex_);
      desc_ = std::move(request);
    }
    if (hold) {
      held = std::move(done);
      ++prompts;
      return;
    }
    ++prompts;
    done(answer);
  }
  rex::ui::PromptDesc GetDesc() {
    std::lock_guard lock(desc_mutex_);
    return desc_;
  }

  rex::ui::IWindow* GetWindow() override { return window_; }

  [[nodiscard]] rex::Result<rex::ui::IWindow*> OpenWindow(
      const rex::ui::UIConfig& config, rex::ui::EventCallback callback) override {
    return null_.OpenWindow(config, std::move(callback));
  }
  void CloseWindow() override { null_.CloseWindow(); }
  void Run() override { null_.Run(); }
  bool PumpFrame() override { return null_.PumpFrame(); }
  void EndFrameLoop() override { null_.EndFrameLoop(); }
  bool IsUIThread() const override { return null_.IsUIThread(); }
  bool Post(rex::MoveOnlyFunction<void()> work) override { return null_.Post(std::move(work)); }
  void RunSynchronous(rex::MoveOnlyFunction<void()> work) override {
    null_.RunSynchronous(std::move(work));
  }
  ImFontAtlas* GetFonts() override { return null_.GetFonts(); }
  ImGuiStyle* GetImGuiStyle() override { return null_.GetImGuiStyle(); }
  ImGuiIO* GetImGuiIO() override { return null_.GetImGuiIO(); }
  rex::ui::Style& GetStyle() override { return null_.GetStyle(); }
  [[nodiscard]] rex::Result<void> AddOverlays(
      std::span<const rex::ui::OverlayDesc> table) override {
    return null_.AddOverlays(table);
  }
  void ShowOverlay(std::string_view name, bool visible) override {
    null_.ShowOverlay(name, visible);
  }
  void ToggleOverlay(std::string_view name) override { null_.ToggleOverlay(name); }
  rex::ui::ImGuiDialog* GetOverlay(std::string_view name) override {
    return null_.GetOverlay(name);
  }
  bool IsAnyOverlayVisible() const override { return null_.IsAnyOverlayVisible(); }
  void AddDialog(rex::ui::ImGuiDialog& dialog) override { null_.AddDialog(dialog); }
  void RemoveDialog(rex::ui::ImGuiDialog& dialog) override { null_.RemoveDialog(dialog); }
  bool HasDialogs() const override { return null_.HasDialogs(); }
  void DrawOverlays(rex::gpu::OverlayDrawContext& context) override { null_.DrawOverlays(context); }
  void SetPresenter(rex::gpu::IPresenter* presenter) override { null_.SetPresenter(presenter); }
  void SetOverlayDrawer(std::unique_ptr<rex::gpu::IOverlayDrawer> drawer) override {
    null_.SetOverlayDrawer(std::move(drawer));
  }
  rex::gpu::IOverlayDrawer* GetOverlayDrawer() override { return null_.GetOverlayDrawer(); }
  void SetFrameStats(rex::ui::FrameStatsProvider provider) override {
    null_.SetFrameStats(std::move(provider));
  }
  void SetRuntime(rex::Runtime* runtime) override { null_.SetRuntime(runtime); }
  rex::ui::VideoMode GetVideoMode() const override { return null_.GetVideoMode(); }
  void ShowMessage(rex::ui::MessageKind kind, std::string_view title,
                   std::string_view body) override {
    null_.ShowMessage(kind, title, body);
  }
  [[nodiscard]] rex::Result<rex::u32> ShowChoice(const rex::ui::ChoiceDesc& choice) override {
    return null_.ShowChoice(choice);
  }
  void CancelPrompts() override { null_.CancelPrompts(); }
  [[nodiscard]] rex::Result<std::vector<std::filesystem::path>> ShowFileDialog(
      const rex::ui::FileDialogDesc& file_desc) override {
    return null_.ShowFileDialog(file_desc);
  }
  std::vector<rex::ui::Monitor> GetDisplays() override { return null_.GetDisplays(); }
  std::vector<rex::ui::HostLocale> GetPreferredLocales() override { return locales; }

 private:
  rex::ui::NullUserInterface null_;
  rex::ui::IWindow* window_ = nullptr;
  std::mutex desc_mutex_;
  rex::ui::PromptDesc desc_;
};

}  // namespace rex::test
