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
#include <vector>
#include <rex/kernel/function_dispatcher.h>
#include <rex/title.h>

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

  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    REX_TRY_VOID(settings.SetText(rex::SettingLayer::kTitle, "title.game_root",
                                  (folder_.path / "game").string()));
    REX_TRY_VOID(root.SetText(rex::SettingLayer::kTitle, folder_.path.string()));
    REX_TRY_VOID(GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "none"));
    REX_TRY_VOID(GetInput().GetSettings().drivers.SetText(rex::SettingLayer::kTitle, "none"));
    REX_TRY_VOID(GetAudio().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "none"));
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

    auto& memory = *StateOf(runtime).GetAddressSpace();
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

namespace no_driver_tests {

namespace {

constexpr rex::u32 kWritePointerRegister = 0x01C5;

class NoDriverApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  NoDriverApp() : ObjectApp("no_driver") { rex::test::WriteTitle(folder_.path / "game"); }

  std::function<void(NoDriverApp&, rex::kernel::TitleRuntime&)> body;
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
  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    REX_TRY_VOID(settings.SetText(rex::SettingLayer::kTitle, "title.game_root",
                                  (folder_.path / "game").string()));
    REX_TRY_VOID(root.SetText(rex::SettingLayer::kTitle, folder_.path.string()));
    REX_TRY_VOID(GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "none"));
    REX_TRY_VOID(GetInput().GetSettings().drivers.SetText(rex::SettingLayer::kTitle, "none"));
    REX_TRY_VOID(GetAudio().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "none"));
    return rex::Ok();
  }

  rex::Result<void> OnInit() override {
    try {
      rex::kernel::TitleRuntime runtime(rex::test::MakeTitleImage(rex::test::kDelayingEntry));
      REQUIRE(runtime.Load());
      REQUIRE(runtime.Launch());
      body(*this, runtime);
      rex::test::UnloadWithin(runtime, std::chrono::seconds(5));
    } catch (...) {
      failure = std::current_exception();
      return rex::Err(rex::ErrorCategory::kRuntime, "the test body threw");
    }
    return rex::Ok();
  }

 private:
  rex::test::TempFolder folder_{"rex_no_driver_"};
};

struct Fixture {
  Fixture() {
    rex::test::ReleaseTestMemory();
    rex::test::g_entry.Reset();
  }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "a title launches and unloads with no graphics driver", "[gpu]") {
  NoDriverApp app;
  app.body = [](NoDriverApp& host, rex::kernel::TitleRuntime&) {
    CHECK_FALSE(host.GetGraphics().HasDriver());
  };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}

REX_TEST_CASE_METHOD(Fixture, "a title with no audio plugin runs with the null audio driver",
                     "[gpu]") {
  NoDriverApp app;
  app.body = [](NoDriverApp&, rex::kernel::TitleRuntime& runtime) {
    CHECK(StateOf(runtime).IsTitleLaunched());
  };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}

REX_TEST_CASE_METHOD(Fixture, "no graphics driver writes the write pointer back", "[gpu]") {
  NoDriverApp app;
  app.body = [](NoDriverApp&, rex::kernel::TitleRuntime& runtime) {
    auto* memory = StateOf(runtime).GetAddressSpace();
    const rex::u32 ring = memory->SystemHeapAlloc(0x1000, 0x1000, rex::kernel::kSystemHeapPhysical);
    REQUIRE(ring != 0);
    const rex::u32 physical = memory->GetPhysicalAddress(ring);
    const rex::u32 writeback = physical + 0x800;
    StateOf(runtime).InitializeGPURingBuffer(physical, 9);
    StateOf(runtime).EnableGPUReadPointerWriteBack(writeback, 6);
    const rex::u32 register_address =
        rex::ppc::MMIOBlockBase(rex::ppc::MMIOBlock::kGPU) + kWritePointerRegister * 4;
    rex::ppc::MMIOStore32(register_address, 0x20);
    CHECK(*memory->TranslatePhysical<const be_u32*>(writeback) == 0x20);
    CHECK(rex::ppc::MMIOLoad32(register_address) == 0x20);
  };
  REQUIRE(app.Start());
  CHECK(app.Finish() == 0);
}

REX_TEST_CASE("The fixed registers report the display mode and canned values", "[gpu]") {
  rex::VideoMode mode;
  mode.width = 1920;
  mode.height = 5000;
  using rex::xenos::ReadFixedRegister;
  using rex::xenos::Register;
  CHECK(ReadFixedRegister(static_cast<rex::u32>(Register::RB_EDRAM_TIMING), mode) == 0x08100748u);
  CHECK(ReadFixedRegister(static_cast<rex::u32>(Register::RB_BC_CONTROL), mode) == 0x0000200Eu);
  CHECK(ReadFixedRegister(static_cast<rex::u32>(Register::D1MODE_V_COUNTER), mode) == 0x0FFFu);
  CHECK(ReadFixedRegister(static_cast<rex::u32>(Register::D1MODE_VBLANK_VLINE_STATUS), mode) == 1u);
  CHECK(ReadFixedRegister(static_cast<rex::u32>(Register::AVIVO_D1MODE_VIEWPORT_SIZE), mode) ==
        ((1920u << 16) | 0x0FFFu));
  CHECK_FALSE(ReadFixedRegister(static_cast<rex::u32>(Register::CP_RB_WPTR), mode));
}

}  // namespace no_driver_tests
