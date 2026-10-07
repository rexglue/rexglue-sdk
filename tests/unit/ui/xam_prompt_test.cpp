/**
 * @file        tests/unit/ui/xam_prompt_test.cpp
 * @brief       XamShowMessageBoxUI and XamShowKeyboardUI through the ui providers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <rex/api.h>
#include <rex/string.h>
#include <rex/ui/null_user_interface.h>
#include <rex/ui/user_interface.h>

#include "kernel/kernel_fixture.h"

using namespace rex::runtime;

namespace {

constexpr rex::u32 kIOPending = 0x3E5;
constexpr rex::u32 kCancelled = 0x4C7;

class ScriptedUI final : public rex::ui::IUserInterface {
 public:
  ScriptedUI() {
    auto opened =
        null_.OpenWindow(rex::ui::UIConfig{}, [](const rex::ui::UIEvent&) { return false; });
    window_ = opened ? *opened : nullptr;
  }

  rex::ui::PromptResult answer;
  rex::ui::PromptDesc desc;
  std::atomic<rex::i32> prompts{0};

  void Prompt(rex::ui::PromptDesc request,
              std::move_only_function<void(rex::ui::PromptResult)> done) override {
    desc = std::move(request);
    ++prompts;
    done(answer);
  }
  rex::ui::IWindow* GetWindow() override { return window_; }

  [[nodiscard]] rex::Result<rex::ui::IWindow*> OpenWindow(
      const rex::ui::UIConfig& config, rex::ui::EventCallback callback) override {
    return null_.OpenWindow(config, std::move(callback));
  }
  void CloseWindow() override { null_.CloseWindow(); }
  void Run() override { null_.Run(); }
  bool PumpFrame() override { return null_.PumpFrame(); }
  void Quit() override { null_.Quit(); }
  bool IsUIThread() const override { return null_.IsUIThread(); }
  bool Post(std::move_only_function<void()> work) override { return null_.Post(std::move(work)); }
  void RunSynchronous(std::move_only_function<void()> work) override {
    null_.RunSynchronous(std::move(work));
  }
  ImFontAtlas* Fonts() override { return null_.Fonts(); }
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
  rex::ui::ImGuiDialog* Overlay(std::string_view name) override { return null_.Overlay(name); }
  bool AnyOverlayVisible() const override { return null_.AnyOverlayVisible(); }
  void AddDialog(rex::ui::ImGuiDialog& dialog) override { null_.AddDialog(dialog); }
  void RemoveDialog(rex::ui::ImGuiDialog& dialog) override { null_.RemoveDialog(dialog); }
  bool HasDialogs() const override { return null_.HasDialogs(); }
  void DrawOverlays(rex::gpu::OverlayDrawContext& context) override { null_.DrawOverlays(context); }
  void SetPresenter(rex::gpu::IPresenter* presenter) override { null_.SetPresenter(presenter); }
  void SetOverlayDrawer(std::unique_ptr<rex::gpu::IOverlayDrawer> drawer) override {
    null_.SetOverlayDrawer(std::move(drawer));
  }
  rex::gpu::IOverlayDrawer* OverlayDrawer() override { return null_.OverlayDrawer(); }
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
  std::vector<rex::ui::DisplayInfo> Displays() override { return null_.Displays(); }
  std::vector<rex::ui::HostLocale> PreferredLocales() override { return null_.PreferredLocales(); }

 private:
  rex::ui::NullUserInterface null_;
  rex::ui::IWindow* window_ = nullptr;
};

rex::u32 PutWide(rex::testing::Caller& caller, rex::Runtime& runtime, std::u16string_view text) {
  const rex::u32 address =
      caller.Alloc(static_cast<rex::u32>((text.size() + 1) * sizeof(char16_t)));
  rex::copy_and_swap_truncating(runtime.memory()->TranslateVirtual<char16_t*>(address), text,
                                text.size() + 1);
  return address;
}

std::u16string ReadWide(rex::Runtime& runtime, rex::u32 address) {
  return rex::memory::load_and_swap<std::u16string>(runtime.memory()->TranslateVirtual(address));
}

rex::u32 MessageBox(rex::testing::Caller& caller, rex::Runtime& runtime, std::u16string_view title,
                    std::u16string_view text, rex::u32 active_button, rex::u32 result_ptr,
                    rex::u32* status) {
  const rex::u32 no = PutWide(caller, runtime, u"No");
  const rex::u32 yes = PutWide(caller, runtime, u"Yes");
  const rex::u32 buttons = caller.Alloc(8);
  caller.Store32(buttons, no);
  caller.Store32(buttons + 4, yes);
  *status =
      caller.XAM("XamShowMessageBoxUI",
                 {0, title.empty() ? 0 : PutWide(caller, runtime, title),
                  PutWide(caller, runtime, text), 2, buttons, active_button, 0, result_ptr, 0});
  return caller.Load32(result_ptr);
}

rex::u32 WaitOverlapped(rex::testing::Caller& caller, rex::u32 overlapped) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (caller.Load32(overlapped) == kIOPending && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return caller.Load32(overlapped);
}

}  // namespace

TEST_CASE("XamShowMessageBoxUI returns the pressed button and sends the buttons", "[xam][ui]") {
  ScriptedUI scripted;
  scripted.answer = {.cancelled = false, .button = 1};
  rex::testing::KernelFixture fixture({}, {}, &scripted);
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 result_ptr = caller.Alloc(4);
    rex::u32 status = 0xFFFFFFFF;
    const rex::u32 chosen =
        MessageBox(caller, fixture.runtime(), u"Pick", u"Body", 0, result_ptr, &status);
    CHECK(status == 0);
    CHECK(chosen == 1);
    CHECK(scripted.desc.title == "Pick");
    CHECK(scripted.desc.body == "Body");
    CHECK(scripted.desc.buttons == std::vector<std::string>{"No", "Yes"});
    CHECK_FALSE(scripted.desc.text_input);
  });
  CHECK(scripted.prompts == 1);
}

TEST_CASE("XamShowMessageBoxUI returns the focused button when the prompt is cancelled",
          "[xam][ui]") {
  ScriptedUI scripted;
  scripted.answer = {.cancelled = true};
  rex::testing::KernelFixture fixture({}, {}, &scripted);
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 result_ptr = caller.Alloc(4);
    rex::u32 status = 0xFFFFFFFF;
    CHECK(MessageBox(caller, fixture.runtime(), u"", u"Body", 1, result_ptr, &status) == 1);
    CHECK(status == 0);
    CHECK(scripted.desc.title == "Message Box");
  });
}

TEST_CASE("XamShowMessageBoxUI without a ui returns the focused button", "[xam][ui]") {
  rex::testing::KernelFixture fixture;
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 result_ptr = caller.Alloc(4);
    rex::u32 status = 0xFFFFFFFF;
    CHECK(MessageBox(caller, fixture.runtime(), u"Pick", u"Body", 1, result_ptr, &status) == 1);
    CHECK(status == 0);
  });
}

TEST_CASE("XamShowMessageBoxUI over a NullUserInterface returns the focused button", "[xam][ui]") {
  rex::ui::NullUserInterface null_ui;
  rex::testing::KernelFixture fixture({}, {}, &null_ui);
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 result_ptr = caller.Alloc(4);
    rex::u32 status = 0xFFFFFFFF;
    CHECK(MessageBox(caller, fixture.runtime(), u"Pick", u"Body", 1, result_ptr, &status) == 1);
    CHECK(status == 0);
  });
}

TEST_CASE("XamShowKeyboardUI writes the typed text and completes the overlapped", "[xam][ui]") {
  ScriptedUI scripted;
  scripted.answer = {.cancelled = false, .button = 0, .text = "Tom"};
  rex::testing::KernelFixture fixture({}, {}, &scripted);
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 buffer = caller.Alloc(32);
    const rex::u32 overlapped = caller.Alloc(0x1C);
    const rex::u32 status = caller.XAM(
        "XamShowKeyboardUI",
        {0, 0, PutWide(caller, fixture.runtime(), u"Seed"), 0, 0, buffer, 16, overlapped});
    CHECK(status == 0x3E5);
    CHECK(WaitOverlapped(caller, overlapped) == 0);
    CHECK(caller.Load32(overlapped + 0x18) == 0);
    CHECK(ReadWide(fixture.runtime(), buffer) == u"Tom");
    CHECK(scripted.desc.title == "Keyboard Input");
    CHECK(scripted.desc.default_text == "Seed");
    CHECK(scripted.desc.text_input);
    CHECK(scripted.desc.max_length == 16);
  });
}

TEST_CASE("XamShowKeyboardUI uses the description as the title when no title is given",
          "[xam][ui]") {
  ScriptedUI scripted;
  scripted.answer = {.cancelled = false, .button = 0, .text = "x"};
  rex::testing::KernelFixture fixture({}, {}, &scripted);
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 buffer = caller.Alloc(32);
    const rex::u32 overlapped = caller.Alloc(0x1C);
    caller.XAM(
        "XamShowKeyboardUI",
        {0, 0, 0, 0, PutWide(caller, fixture.runtime(), u"Enter a name"), buffer, 16, overlapped});
    WaitOverlapped(caller, overlapped);
    CHECK(scripted.desc.title == "Enter a name");
    CHECK(scripted.desc.body.empty());
  });
}

TEST_CASE("XamShowKeyboardUI reports a cancelled prompt as cancelled and leaves the buffer",
          "[xam][ui]") {
  ScriptedUI scripted;
  scripted.answer = {.cancelled = true};
  rex::testing::KernelFixture fixture({}, {}, &scripted);
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 buffer = caller.Alloc(32);
    const rex::u32 overlapped = caller.Alloc(0x1C);
    caller.XAM("XamShowKeyboardUI", {0, 0, 0, 0, 0, buffer, 16, overlapped});
    CHECK(WaitOverlapped(caller, overlapped) == 0);
    CHECK(caller.Load32(overlapped + 0x18) == kCancelled);
    CHECK(caller.Load32(buffer) == 0);
  });
}

TEST_CASE("XamShowKeyboardUI without a ui copies the default text", "[xam][ui]") {
  rex::testing::KernelFixture fixture;
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 buffer = caller.Alloc(32);
    const rex::u32 overlapped = caller.Alloc(0x1C);
    caller.XAM("XamShowKeyboardUI",
               {0, 0, PutWide(caller, fixture.runtime(), u"Seed"), 0, 0, buffer, 16, overlapped});
    CHECK(WaitOverlapped(caller, overlapped) == 0);
    CHECK(ReadWide(fixture.runtime(), buffer) == u"Seed");
  });
}

TEST_CASE("A XAM message box on the module waits until the prompts are cancelled", "[xam][ui]") {
  const std::vector<std::string> modules{"rex-ui-sdl3"};
  rex::testing::KernelFixture fixture({}, modules);
  auto* ui = fixture.api().Get<rex::ui::IUserInterface>();
  REQUIRE(ui != nullptr);
  auto opened =
      ui->OpenWindow(fixture.api().config().ui, [](const rex::ui::UIEvent&) { return false; });
  if (!opened) {
    SKIP("SDL dummy video driver unavailable");
  }
  std::atomic<rex::u32> status{0xFFFFFFFF};
  std::atomic<rex::u32> chosen{0xFFFFFFFF};
  std::jthread app_thread([&] {
    fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
      const rex::u32 result_ptr = caller.Alloc(4);
      rex::u32 value = 0;
      chosen = MessageBox(caller, fixture.runtime(), u"Pick", u"Body", 1, result_ptr, &value);
      status = value;
    });
  });
  for (rex::i32 frame = 0; frame < 2000 && !ui->HasDialogs(); ++frame) {
    ui->PumpFrame();
  }
  REQUIRE(ui->HasDialogs());
  ui->CancelPrompts();
  app_thread.join();
  CHECK(status == 0);
  CHECK(chosen == 1);
  ui->CloseWindow();
}
