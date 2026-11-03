/**
 * @file        tests/unit/ui/xam_prompt_test.cpp
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

#include <rex/kernel/thread_registry.h>
#include <rex/kernel/types/io.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/string/text.h>
#include <rex/ui/window.h>
#include <rex/ui/ui_event.h>
#include <rex/utility.h>
#include <rex/ui/overlay_renderer.h>

#include <imgui.h>

#include "kernel/kernel_fixture.h"
#include "ui_harness.h"
#include "kernel/title_state.h"

using namespace rex::kernel;

namespace {

constexpr rex::u32 kIOPending = 0x3E5;

rex::u32 PutWide(rex::test::Caller& caller, rex::kernel::TitleRuntime& runtime,
                 std::u16string_view text) {
  const rex::u32 address =
      caller.Alloc(static_cast<rex::u32>((text.size() + 1) * sizeof(char16_t)));
  rex::string::CopyAndSwapTruncating(
      StateOf(runtime).GetMemory()->TranslateVirtual<char16_t*>(address), text, text.size() + 1);
  return address;
}

std::u16string ReadWide(rex::kernel::TitleRuntime& runtime, rex::u32 address) {
  return rex::memory::load_and_swap<std::u16string>(
      StateOf(runtime).GetMemory()->TranslateVirtual(address));
}

rex::u32 CallMessageBox(rex::test::Caller& caller, rex::kernel::TitleRuntime& runtime,
                        std::u16string_view title, std::u16string_view text, rex::u32 active_button,
                        rex::u32 result_ptr, rex::u32* status) {
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

rex::u32 CallMessageBoxEx(rex::test::Caller& caller, rex::kernel::TitleRuntime& runtime,
                          std::u16string_view text, rex::u32 active_button, rex::u32 result_ptr,
                          rex::u32* status) {
  const rex::u32 no = PutWide(caller, runtime, u"No");
  const rex::u32 yes = PutWide(caller, runtime, u"Yes");
  const rex::u32 buttons = caller.Alloc(8);
  caller.Store32(buttons, no);
  caller.Store32(buttons + 4, yes);
  *status = caller.XAM("XamShowMessageBoxUIEx", {0xFF, 0, PutWide(caller, runtime, text), 2,
                                                 buttons, active_button, 1, 0x55, result_ptr, 0});
  return caller.Load32(result_ptr);
}

rex::u32 WaitOverlapped(rex::kernel::TitleRuntime& runtime, rex::u32 overlapped) {
  auto* host = StateOf(runtime).GetMemory()->TranslateVirtual<XOVERLAPPED*>(overlapped);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (XOverlappedGetResult(host) == kIOPending && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return XOverlappedGetResult(host);
}

class NullRenderer final : public rex::IOverlayRenderer {
 public:
  void RenderOverlays(ImDrawData& draw_data) override {
    for (ImTextureData* texture : *draw_data.Textures) {
      if (texture->Status == ImTextureStatus_WantDestroy) {
        texture->SetStatus(ImTextureStatus_Destroyed);
      } else if (texture->Status != ImTextureStatus_OK) {
        texture->SetTexID(static_cast<ImTextureID>(1));
        texture->SetStatus(ImTextureStatus_OK);
      }
    }
  }
};

void Frames(rex::test::KernelFixture& fixture, int count) {
  NullRenderer renderer;
  for (int i = 0; i < count; ++i) {
    fixture.GetUI().DrawOverlays(renderer);
    fixture.Pump();
  }
}

void Press(rex::test::KernelFixture& fixture, rex::hid::VirtualKey key) {
  rex::test_ui::DeliverKey(rex::hid::InputEvent::Type::kKeyDown, key);
  fixture.Pump();
  Frames(fixture, 2);
  rex::test_ui::DeliverKey(rex::hid::InputEvent::Type::kKeyUp, key);
  fixture.Pump();
  Frames(fixture, 2);
}

void Type(rex::test::KernelFixture& fixture, char16_t character) {
  rex::test_ui::Deliver([=](rex::ui::IUIEventListener& listener) {
    rex::hid::InputEvent event{.type = rex::hid::InputEvent::Type::kKeyChar,
                               .character = static_cast<rex::u32>(character)};
    listener.OnInputEvent(event);
  });
  fixture.Pump();
  Frames(fixture, 2);
}

void Click(rex::test::KernelFixture& fixture, rex::i32 x, rex::i32 y) {
  using rex::hid::InputEvent;
  auto send = [&](InputEvent::Type type) {
    rex::test_ui::Deliver([=](rex::ui::IUIEventListener& listener) {
      InputEvent event{.type = type,
                       .button = rex::hid::MouseButton::kLeft,
                       .x = static_cast<rex::f32>(x),
                       .y = static_cast<rex::f32>(y)};
      listener.OnInputEvent(event);
    });
    fixture.Pump();
    Frames(fixture, 2);
  };
  send(InputEvent::Type::kMotion);
  send(InputEvent::Type::kButtonDown);
  send(InputEvent::Type::kButtonUp);
  Frames(fixture, 2);
}

void AttachUI(rex::test::KernelFixture& fixture) {
  rex::ui::IUISystem& ui = fixture.GetUI();
  REQUIRE(StateOf(fixture.GetRuntime())
              .AttachSystems(nullptr, &ui, fixture.GetUIControl(), nullptr, nullptr,
                             ui.GetVideoMode()));
}

void WaitForDialog(rex::test::KernelFixture& fixture) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!fixture.GetUI().HasDialogs() && std::chrono::steady_clock::now() < until) {
    Frames(fixture, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE(fixture.GetUI().HasDialogs());
  Frames(fixture, 4);
}

void WaitForGuest(rex::test::KernelFixture& fixture, const std::atomic<bool>& finished) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!finished.load() && std::chrono::steady_clock::now() < until) {
    Frames(fixture, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE(finished.load());
}

}  // namespace

REX_TEST_CASE("XamShowMessageBoxUI without a UI returns the focused button", "[ui][xam]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&](rex::test::Caller& caller) {
    const rex::u32 result_ptr = caller.Alloc(4);
    rex::u32 status = 0xFFFFFFFF;
    CHECK(CallMessageBox(caller, fixture.GetRuntime(), u"Pick", u"Body", 1, result_ptr, &status) ==
          1);
    CHECK(status == 0);
  });
}

REX_TEST_CASE("XamShowMessageBoxUIEx without a UI returns the focused button", "[ui][xam]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&](rex::test::Caller& caller) {
    const rex::u32 result_ptr = caller.Alloc(4);
    caller.Store32(result_ptr, 0xDEADBEEF);
    rex::u32 status = 0xFFFFFFFF;
    CHECK(CallMessageBoxEx(caller, fixture.GetRuntime(), u"Body", 1, result_ptr, &status) == 1);
    CHECK(status == 0);
  });
}

REX_TEST_CASE("XamShowKeyboardUI without a ui copies the default text", "[ui][xam]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([&](rex::test::Caller& caller) {
    const rex::u32 buffer = caller.Alloc(32);
    const rex::u32 overlapped = caller.Alloc(0x1C);
    caller.XAM("XamShowKeyboardUI", {0, 0, PutWide(caller, fixture.GetRuntime(), u"Default"), 0, 0,
                                     buffer, 16, overlapped});
    CHECK(WaitOverlapped(fixture.GetRuntime(), overlapped) == 0);
    CHECK(ReadWide(fixture.GetRuntime(), buffer) == u"Default");
  });
}

REX_TEST_CASE("xam message box returns the pressed button", "[ui][xam]") {
  static_cast<rex::test_ui::FakeUIData&>(rex::test_ui::g_ui) = rex::test_ui::FakeUIData{};
  rex::test_ui::FakeUIProvider provider;
  rex::test::KernelFixture fixture({}, {&provider}, {}, "fake");
  AttachUI(fixture);
  std::atomic<bool> finished{false};
  std::atomic<rex::u32> status{0xFFFFFFFF};
  std::atomic<rex::u32> picked{0xFFFFFFFF};
  std::thread guest([&] {
    fixture.RunOnGuestThread([&](rex::test::Caller& caller) {
      const rex::u32 result_ptr = caller.Alloc(4);
      rex::u32 code = 0xFFFFFFFF;
      picked = CallMessageBox(caller, fixture.GetRuntime(), u"Pick", u"Body", 0, result_ptr, &code);
      status = code;
    });
    finished = true;
  });
  WaitForDialog(fixture);
  const rex::ui::WindowSize size{rex::test_ui::g_ui.width, rex::test_ui::g_ui.height};
  for (rex::i32 y = static_cast<rex::i32>(size.height_px / 2) - 40;
       y < static_cast<rex::i32>(size.height_px / 2) + 60 && fixture.GetUI().HasDialogs(); y += 3) {
    Click(fixture, static_cast<rex::i32>(size.width_px / 2) + 18, y);
  }
  WaitForGuest(fixture, finished);
  guest.join();
  CHECK(status == 0);
  CHECK(picked == 1);
}

REX_TEST_CASE("xam keyboard prompt returns typed text", "[ui][xam]") {
  static_cast<rex::test_ui::FakeUIData&>(rex::test_ui::g_ui) = rex::test_ui::FakeUIData{};
  rex::test_ui::FakeUIProvider provider;
  rex::test::KernelFixture fixture({}, {&provider}, {}, "fake");
  AttachUI(fixture);
  std::atomic<bool> finished{false};
  std::atomic<rex::u32> completion{0xFFFFFFFF};
  std::u16string text;
  std::thread guest([&] {
    fixture.RunOnGuestThread([&](rex::test::Caller& caller) {
      const rex::u32 buffer = caller.Alloc(32);
      const rex::u32 overlapped = caller.Alloc(0x1C);
      caller.XAM("XamShowKeyboardUI",
                 {0, 0, PutWide(caller, fixture.GetRuntime(), u""), 0, 0, buffer, 16, overlapped});
      completion = WaitOverlapped(fixture.GetRuntime(), overlapped);
      text = ReadWide(fixture.GetRuntime(), buffer);
    });
    finished = true;
  });
  WaitForDialog(fixture);
  Type(fixture, u'h');
  Type(fixture, u'i');
  Press(fixture, rex::hid::VirtualKey::kReturn);
  WaitForGuest(fixture, finished);
  guest.join();
  CHECK(completion == 0);
  CHECK(text == u"hi");
}
