/**
 * @file        tests/unit/gpu/no_driver_test.cpp
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
#include <functional>
#include <vector>

#include <rex/app/app.h>
#include <rex/cli.h>
#include <rex/gpu/graphics_system.h>
#include <rex/xenos/register.h>
#include <rex/ppc/mmio.h>
#include <rex/ppc/mmio.h>
#include <rex/kernel/title_runtime.h>
#include <rex/setting.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/title.h>

#include "kernel/title_entries.h"
#include "kernel/title_fakes.h"
#include "kernel/title_image.h"
#include "temp_folder.h"
#include "test_memory.h"
#include "kernel/title_state.h"

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
    (void)settings.SetText(rex::SettingLayer::kTitle, "title.game_root",
                           (folder_.path / "game").string());
    (void)root.SetText(rex::SettingLayer::kTitle, folder_.path.string());
    (void)GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "none");
    (void)GetInput().GetSettings().drivers.SetText(rex::SettingLayer::kTitle, "none");
    (void)GetAudio().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "none");
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
