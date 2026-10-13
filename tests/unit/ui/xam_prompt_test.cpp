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

#include "rex_test.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/api.h>
#include <rex/runtime/thread_registry.h>
#include <rex/runtime/xio.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xthread.h>
#include <rex/string.h>
#include <rex/ui/null_user_interface.h>
#include <rex/ui/user_interface.h>
#include <rex/move_only_function.h>

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
  rex::ui::PromptDesc Desc() {
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
  void Quit() override { null_.Quit(); }
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
  rex::ui::ImGuiDialog* Overlay(std::string_view name) override { return null_.Overlay(name); }
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
  std::vector<rex::ui::DisplayInfo> GetDisplays() override { return null_.GetDisplays(); }
  std::vector<rex::ui::HostLocale> GetPreferredLocales() override {
    return null_.GetPreferredLocales();
  }

 private:
  rex::ui::NullUserInterface null_;
  rex::ui::IWindow* window_ = nullptr;
  std::mutex desc_mutex_;
  rex::ui::PromptDesc desc_;
};

rex::u32 PutWide(rex::testing::Caller& caller, rex::Runtime& runtime, std::u16string_view text) {
  const rex::u32 address =
      caller.Alloc(static_cast<rex::u32>((text.size() + 1) * sizeof(char16_t)));
  rex::copy_and_swap_truncating(runtime.GetMemory()->TranslateVirtual<char16_t*>(address), text,
                                text.size() + 1);
  return address;
}

std::u16string ReadWide(rex::Runtime& runtime, rex::u32 address) {
  return rex::memory::load_and_swap<std::u16string>(runtime.GetMemory()->TranslateVirtual(address));
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

rex::u32 WaitOverlapped(rex::Runtime& runtime, rex::u32 overlapped) {
  auto* host = runtime.GetMemory()->TranslateVirtual<XAM_OVERLAPPED*>(overlapped);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (XOverlappedGetResult(host) == kIOPending && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return XOverlappedGetResult(host);
}

}  // namespace

REX_TEST_CASE("XamShowMessageBoxUI returns the pressed button and sends the buttons", "[ui][xam]") {
  ScriptedUI scripted;
  scripted.answer = {.cancelled = false, .button = 1};
  rex::testing::KernelFixture fixture({}, {}, &scripted);
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 result_ptr = caller.Alloc(4);
    rex::u32 status = 0xFFFFFFFF;
    const rex::u32 chosen =
        MessageBox(caller, fixture.GetRuntime(), u"Pick", u"Body", 0, result_ptr, &status);
    CHECK(status == 0);
    CHECK(chosen == 1);
    CHECK(scripted.Desc().title == "Pick");
    CHECK(scripted.Desc().body == "Body");
    CHECK(scripted.Desc().buttons == std::vector<std::string>{"No", "Yes"});
    CHECK_FALSE(scripted.Desc().text_input);
  });
  CHECK(scripted.prompts == 1);
}

REX_TEST_CASE("XamShowMessageBoxUI returns the focused button when the prompt is cancelled",
              "[ui][xam]") {
  ScriptedUI scripted;
  scripted.answer = {.cancelled = true};
  rex::testing::KernelFixture fixture({}, {}, &scripted);
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 result_ptr = caller.Alloc(4);
    rex::u32 status = 0xFFFFFFFF;
    CHECK(MessageBox(caller, fixture.GetRuntime(), u"", u"Body", 1, result_ptr, &status) == 1);
    CHECK(status == 0);
    CHECK(scripted.Desc().title == "Message Box");
  });
}

REX_TEST_CASE("XamShowMessageBoxUI without a ui returns the focused button", "[ui][xam]") {
  rex::testing::KernelFixture fixture;
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 result_ptr = caller.Alloc(4);
    rex::u32 status = 0xFFFFFFFF;
    CHECK(MessageBox(caller, fixture.GetRuntime(), u"Pick", u"Body", 1, result_ptr, &status) == 1);
    CHECK(status == 0);
  });
}

REX_TEST_CASE("XamShowMessageBoxUI over a NullUserInterface returns the focused button",
              "[ui][xam]") {
  rex::ui::NullUserInterface null_ui;
  rex::testing::KernelFixture fixture({}, {}, &null_ui);
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 result_ptr = caller.Alloc(4);
    rex::u32 status = 0xFFFFFFFF;
    CHECK(MessageBox(caller, fixture.GetRuntime(), u"Pick", u"Body", 1, result_ptr, &status) == 1);
    CHECK(status == 0);
  });
}

REX_TEST_CASE("XamShowKeyboardUI writes the typed text and completes the overlapped", "[ui][xam]") {
  ScriptedUI scripted;
  scripted.answer = {.cancelled = false, .button = 0, .text = "Tom"};
  rex::testing::KernelFixture fixture({}, {}, &scripted);
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 buffer = caller.Alloc(32);
    const rex::u32 overlapped = caller.Alloc(0x1C);
    const rex::u32 status = caller.XAM(
        "XamShowKeyboardUI",
        {0, 0, PutWide(caller, fixture.GetRuntime(), u"Seed"), 0, 0, buffer, 16, overlapped});
    CHECK(status == 0x3E5);
    CHECK(WaitOverlapped(fixture.GetRuntime(), overlapped) == 0);
    CHECK(caller.Load32(overlapped + 0x18) == 0);
    CHECK(ReadWide(fixture.GetRuntime(), buffer) == u"Tom");
    CHECK(scripted.Desc().title == "Keyboard Input");
    CHECK(scripted.Desc().default_text == "Seed");
    CHECK(scripted.Desc().text_input);
    CHECK(scripted.Desc().max_length == 16);
  });
}

REX_TEST_CASE("XamShowKeyboardUI uses the description as the title when no title is given",
              "[ui][xam]") {
  ScriptedUI scripted;
  scripted.answer = {.cancelled = false, .button = 0, .text = "x"};
  rex::testing::KernelFixture fixture({}, {}, &scripted);
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 buffer = caller.Alloc(32);
    const rex::u32 overlapped = caller.Alloc(0x1C);
    caller.XAM("XamShowKeyboardUI",
               {0, 0, 0, 0, PutWide(caller, fixture.GetRuntime(), u"Enter a name"), buffer, 16,
                overlapped});
    WaitOverlapped(fixture.GetRuntime(), overlapped);
    CHECK(scripted.Desc().title == "Enter a name");
    CHECK(scripted.Desc().body.empty());
  });
}

REX_TEST_CASE("XamShowKeyboardUI reports a cancelled prompt as cancelled and leaves the buffer",
              "[ui][xam]") {
  ScriptedUI scripted;
  scripted.answer = {.cancelled = true};
  rex::testing::KernelFixture fixture({}, {}, &scripted);
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 buffer = caller.Alloc(32);
    const rex::u32 overlapped = caller.Alloc(0x1C);
    caller.XAM("XamShowKeyboardUI", {0, 0, 0, 0, 0, buffer, 16, overlapped});
    CHECK(WaitOverlapped(fixture.GetRuntime(), overlapped) == 0);
    CHECK(caller.Load32(overlapped + 0x18) == kCancelled);
    CHECK(caller.Load32(buffer) == 0);
  });
}

REX_TEST_CASE("XamShowKeyboardUI without a ui copies the default text", "[ui][xam]") {
  rex::testing::KernelFixture fixture;
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    const rex::u32 buffer = caller.Alloc(32);
    const rex::u32 overlapped = caller.Alloc(0x1C);
    caller.XAM("XamShowKeyboardUI", {0, 0, PutWide(caller, fixture.GetRuntime(), u"Seed"), 0, 0,
                                     buffer, 16, overlapped});
    CHECK(WaitOverlapped(fixture.GetRuntime(), overlapped) == 0);
    CHECK(ReadWide(fixture.GetRuntime(), buffer) == u"Seed");
  });
}

REX_TEST_CASE("A XAM message box on the module waits until the prompts are cancelled",
              "[ui][xam]") {
  const std::vector<std::string> modules{"rex-ui-sdl3"};
  rex::testing::KernelFixture fixture({}, modules);
  auto* ui = fixture.GetAPI().Get<rex::ui::IUserInterface>();
  REQUIRE(ui != nullptr);
  auto opened = ui->OpenWindow(fixture.GetAPI().GetConfig().ui,
                               [](const rex::ui::UIEvent&) { return false; });
  if (!opened) {
    SKIP("SDL dummy video driver unavailable");
  }
  std::atomic<rex::u32> status{0xFFFFFFFF};
  std::atomic<rex::u32> chosen{0xFFFFFFFF};
  std::jthread app_thread([&] {
    fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
      const rex::u32 result_ptr = caller.Alloc(4);
      rex::u32 value = 0;
      chosen = MessageBox(caller, fixture.GetRuntime(), u"Pick", u"Body", 1, result_ptr, &value);
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

REX_TEST_CASE("Teardown stops a prompt still running on the dispatcher", "[ui][xam][shutdown]") {
  ScriptedUI scripted;
  scripted.hold = true;
  {
    rex::testing::KernelFixture fixture({}, {}, &scripted);
    fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
      const rex::u32 buffer = caller.Alloc(32);
      const rex::u32 overlapped = caller.Alloc(0x1C);
      CHECK(caller.XAM("XamShowKeyboardUI", {0, 0, PutWide(caller, fixture.GetRuntime(), u"Seed"),
                                             0, 0, buffer, 16, overlapped}) == kIOPending);
    });
    const auto shown_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (scripted.prompts == 0 && std::chrono::steady_clock::now() < shown_by) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(scripted.prompts == 1);
  }
  scripted.held(rex::ui::PromptResult{});
}

REX_TEST_CASE("A stop ends a guest thread waiting on a prompt", "[ui][xam][shutdown]") {
  ScriptedUI scripted;
  scripted.hold = true;
  rex::testing::KernelFixture fixture({}, {}, &scripted);
  rex::u32 thread = 0;
  fixture.RunOnGuestThread([&](rex::testing::Caller& caller) {
    thread = caller.Spawn([&fixture](rex::testing::Caller& title) {
      const rex::u32 result_ptr = title.Alloc(4);
      rex::u32 status = 0;
      MessageBox(title, fixture.GetRuntime(), u"Pick", u"Body", 0, result_ptr, &status);
    });
  });
  const auto shown_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (scripted.prompts == 0 && std::chrono::steady_clock::now() < shown_by) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(scripted.prompts == 1);
  auto pin = fixture.GetRuntime().GetThreadRegistry().FindByHandle(thread);
  REQUIRE(pin);
  pin->RequestStop();
  const auto stopped_by = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (pin->IsRunning() && std::chrono::steady_clock::now() < stopped_by) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK_FALSE(pin->IsRunning());
  scripted.held(rex::ui::PromptResult{});
}
