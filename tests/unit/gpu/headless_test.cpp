/**
 * @file        tests/unit/gpu/headless_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "object_app.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#include <rex/app/app.h>
#include <rex/apu/audio_system.h>
#include <rex/cli.h>
#include <rex/gpu/graphics_system.h>
#include <rex/hid/input_system.h>
#include <rex/ppc/mmio.h>
#include <rex/kernel/title_runtime.h>
#include <rex/setting.h>
#include <rex/kernel/heap.h>
#include <rex/ui/ui_system.h>
#include <rex/xenos/register.h>
#include <rex/xenos/packet.h>

#include "kernel/title_entries.h"
#include "kernel/title_fakes.h"
#include "kernel/title_image.h"
#include "temp_folder.h"
#include "test_memory.h"
#include "kernel/title_state.h"

namespace {

constexpr rex::u32 kWritePointerRegister = 0x01C5;

class HeadlessApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  explicit HeadlessApp(std::string driver)
      : ObjectApp("headless"), driver_(std::move(driver)), folder_("rex_headless_") {
    rex::test::WriteTitle(folder_.path / "game");
  }

  std::function<void(HeadlessApp&)> body;

  std::exception_ptr failure;

  rex::Result<void> Start() { return Init(rex::CLIArgList::Parse(std::vector<std::string>{})); }

  int Finish() {
    const int code = Shutdown();
    if (failure) {
      std::rethrow_exception(failure);
    }
    return code;
  }

 protected:
  rex::AppSysRequirements GetRequirements() const override {
    rex::AppSysRequirements needs;
    needs.graphics_family = rex::GraphicsFamily::kXenos;
    return needs;
  }

  rex::Result<void> OnConfigure(rex::Settings& settings) override {
    (void)settings.SetText(rex::SettingLayer::kTitle, "title.game_root",
                           (folder_.path / "game").string());
    (void)root.SetText(rex::SettingLayer::kTitle, folder_.path.string());
    (void)GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "none");
    (void)GetInput().GetSettings().drivers.SetText(rex::SettingLayer::kTitle, "none");
    (void)GetAudio().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "none");
    return settings.SetText(rex::SettingLayer::kTitle, "gpu.driver", driver_);
  }

  rex::Result<void> OnInit() override {
    try {
      if (body) {
        body(*this);
      }
    } catch (...) {
      failure = std::current_exception();
      return rex::Err(rex::ErrorCategory::kRuntime, "the test body threw");
    }
    return rex::Ok();
  }

 private:
  std::string driver_;
  rex::test::TempFolder folder_;
};

struct Fixture {
  Fixture() {
    rex::test::ReleaseTestMemory();
    rex::test::g_entry.Reset();
  }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "headless xenos title runs to first swap", "[gpu][headless]") {
  HeadlessApp app("null");
  app.body = [](HeadlessApp& self) {
    rex::gpu::IGraphicsSystem& graphics = self.GetGraphics();
    rex::kernel::TitleRuntime runtime(rex::test::MakeTitleImage(rex::test::kDelayingEntry));
    REQUIRE(runtime.Load());
    REQUIRE(runtime.Launch());

    auto& memory = *StateOf(runtime).GetMemory();
    const rex::u32 block = memory.SystemHeapAlloc(0x2000, 0x1000, rex::kernel::kSystemHeapPhysical);
    REQUIRE(block != 0);
    const rex::u32 ring = memory.GetPhysicalAddress(block);
    const rex::u32 writeback = ring + 0x1000;
    const rex::u32 packets[] = {
        rex::xenos::BuildPacketType3(rex::xenos::PM4_XE_SWAP, 4),
        rex::xenos::kSwapSignature,
        0,
        1280,
        720,
    };
    for (rex::u32 i = 0; i < std::size(packets); ++i) {
      *memory.TranslatePhysical<be_u32*>(ring + i * 4) = packets[i];
    }
    StateOf(runtime).InitializeGPURingBuffer(ring, 9);
    StateOf(runtime).EnableGPUReadPointerWriteBack(writeback, 6);
    rex::ppc::MMIOStore32(
        rex::ppc::MMIOBlockBase(rex::ppc::MMIOBlock::kGPU) + kWritePointerRegister * 4,
        std::size(packets));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto done = [&] {
      return graphics.GetFrameStats().frame_count >= 1 &&
             memory.TranslatePhysical<const be_u32*>(writeback)->Load(std::memory_order_acquire) ==
                 std::size(packets);
    };
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(graphics.GetFrameStats().frame_count >= 1);
    CHECK(memory.TranslatePhysical<const be_u32*>(writeback)->Load(std::memory_order_acquire) ==
          std::size(packets));
    rex::test::UnloadWithin(runtime, std::chrono::seconds(5));
  };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}

REX_TEST_CASE_METHOD(Fixture, "an app that needs xenos refuses gpu.driver none",
                     "[gpu][headless]") {
  HeadlessApp app("none");
  const auto started = app.Start();
  REQUIRE_FALSE(started);
  CHECK(started.error().message.find("xenos family") != std::string::npos);
  app.Finish();
}

REX_TEST_CASE_METHOD(Fixture, "required graphics missing stops the app with its name",
                     "[gpu][headless]") {
  HeadlessApp app("nosuch");
  const auto started = app.Start();
  REQUIRE_FALSE(started);
  CHECK(started.error().message.find("rexgpu-xenos-nosuch") != std::string::npos);
  app.Finish();
}
