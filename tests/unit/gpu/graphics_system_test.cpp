/**
 * @file        tests/unit/gpu/graphics_system_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "object_app.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/cli.h>
#include <rex/gpu/graphics_driver.h>
#include <rex/gpu/xenos_graphics_driver.h>
#include <rex/gpu/graphics_system.h>
#include "gpu/graphics_control.h"
#include <rex/logging/logger.h>
#include <rex/ppc/mmio.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>
#include <rex/app/app_info.h>
#include <rex/kernel/title_runtime.h>
#include <rex/setting.h>
#include <rex/title.h>

#include "codegen/tool_runtime.h"
#include "kernel/title_state.h"
#include "kernel/title_fixture.h"
#include "temp_folder.h"
#include "test_memory.h"
#include "ui/ui_harness.h"

namespace {

using namespace std::chrono_literals;

struct FakeData {
  std::atomic<bool> every_wait_is_new_frame{true};
  std::atomic<rex::u32> new_frames{0};
  std::atomic<rex::u32> presents{0};
  std::atomic<rex::u32> ring_calls{0};
  std::atomic<rex::u32> write_back_calls{0};
  std::atomic<rex::u32> last_write_index{0};
  std::atomic<rex::u32> last_write_value{0};
  std::mutex mutex;
  std::vector<std::string> events;

  void Log(std::string event) {
    std::lock_guard lock(mutex);
    events.push_back(std::move(event));
  }

  std::vector<std::string> Events() {
    std::lock_guard lock(mutex);
    return events;
  }

  void Reset() {
    {
      std::lock_guard lock(mutex);
      events.clear();
    }
    every_wait_is_new_frame = true;
    new_frames = 0;
    presents = 0;
    ring_calls = 0;
    write_back_calls = 0;
    last_write_index = 0;
    last_write_value = 0;
  }
};

FakeData g_fake;

class PlainDriver final : public rex::gpu::IGraphicsDriver {
 public:
  std::string_view GetName() const override { return "plain"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override { return {}; }
  void Shutdown() override {}
  rex::gpu::FrameStatus WaitForFrame(rex::u32) override {
    std::this_thread::sleep_for(1ms);
    return {};
  }
  void ResizeSurface(rex::u32, rex::u32) override {}
  bool BeginPresent() override { return false; }
  void EndPresent() override {}
  void RenderOverlays(ImDrawData&) override {}
};

class FakeDriver final : public rex::gpu::IXenosGraphicsDriver {
 public:
  std::string_view GetName() const override { return "fake"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override { return {}; }
  void Shutdown() override {}

  rex::gpu::FrameStatus WaitForFrame(rex::u32) override {
    std::this_thread::sleep_for(1ms);
    g_fake.Log("wait");
    const bool fresh = g_fake.every_wait_is_new_frame.load();
    if (fresh) {
      ++g_fake.new_frames;
    }
    return {fresh};
  }
  void ResizeSurface(rex::u32 width, rex::u32 height) override {
    g_fake.Log("resize " + std::to_string(width) + " " + std::to_string(height));
  }
  bool BeginPresent() override {
    g_fake.Log("begin");
    return true;
  }
  void RenderOverlays(ImDrawData&) override { g_fake.Log("overlays"); }
  void EndPresent() override {
    g_fake.Log("end");
    ++g_fake.presents;
  }

  void InitializeRingBuffer(rex::u32, rex::u32) override { ++g_fake.ring_calls; }
  void EnableReadPointerWriteBack(rex::u32, rex::u32) override { ++g_fake.write_back_calls; }
  rex::u32 ReadRegister(rex::u32) override { return 7; }
  void WriteRegister(rex::u32 index, rex::u32 value) override {
    g_fake.last_write_index = index;
    g_fake.last_write_value = value;
  }
};

std::unique_ptr<rex::IDriver> CreateFake() {
  return std::make_unique<FakeDriver>();
}

std::unique_ptr<rex::IDriver> CreatePlain() {
  return std::make_unique<PlainDriver>();
}

class FakeGraphicsProvider final : public rex::IModule, public rex::IDriverModule {
 public:
  std::string_view GetName() const override { return "fake_graphics"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }

 private:
  static constexpr std::array<rex::DriverRegistration, 2> kDrivers = {
      rex::DriverRegistration{rex::DriverType::kGraphics, "fake", rex::GraphicsFamily::kXenos,
                              &CreateFake},
      rex::DriverRegistration{rex::DriverType::kGraphics, "plain", rex::GraphicsFamily::kXenos,
                              &CreatePlain}};
};

class GraphicsApp final : public rex::test::ObjectApp<rex::codegen::ToolApp> {
 public:
  explicit GraphicsApp(std::string driver, bool with_ui = false, std::string frame_limit = {},
                       rex::GraphicsFamily family = rex::GraphicsFamily::kXenos)
      : ObjectApp("graphics_test"),
        driver_(std::move(driver)),
        with_ui_(with_ui),
        frame_limit_(std::move(frame_limit)),
        family_(family) {
    g_fake.Reset();
    rex::test::ReleaseTestMemory();
  }

  rex::Result<void> Start() { return Init(rex::CLIArgList::Parse(std::vector<std::string>{})); }

  rex::Result<rex::codegen::ToolRuntime> OpenTitle() { return Open(folder_.path); }

  rex::gpu::IGraphicsControl* GetGraphicsControl() {
    return GetControlFactory().Get<rex::gpu::IGraphicsControl>();
  }

 protected:
  rex::AppSysRequirements GetRequirements() const override {
    rex::AppSysRequirements requirements;
    requirements.graphics_family = family_;
    return requirements;
  }

  void OnRegisterModules(std::vector<rex::IModule*>& providers) override {
    ObjectApp::OnRegisterModules(providers);
    providers.push_back(&graphics_provider_);
    providers.push_back(&ui_provider_);
  }

  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    REX_TRY_VOID(ToolApp::OnConfigure(settings));
    (void)root.SetText(rex::SettingLayer::kTitle, folder_.path.string());
    (void)GetGraphics().GetSettings().driver.SetText(rex::SettingLayer::kTitle, driver_);
    if (with_ui_) {
      (void)GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "fake");
    }
    if (!frame_limit_.empty()) {
      (void)GetGraphics().GetSettings().frame_limit.SetText(rex::SettingLayer::kTitle,
                                                            frame_limit_);
    }
    return rex::Ok();
  }

 private:
  rex::test::TempFolder folder_{"rex_graphics_"};
  FakeGraphicsProvider graphics_provider_;
  rex::test_ui::FakeUIProvider ui_provider_;
  std::string driver_;
  bool with_ui_;
  std::string frame_limit_;
  rex::GraphicsFamily family_;
};

struct WarningSink final : rex::log::ILogSink {
  void Write(const rex::log::Message& message) override {
    if (message.text.find("has no Xenos graphics driver") != std::string_view::npos) {
      ++count;
    }
  }
  void Flush() override {}
  std::atomic<int> count{0};
};

bool WaitFor(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return done();
}

constexpr rex::u32 kRegisterBase = rex::ppc::MMIOBlockBase(rex::ppc::MMIOBlock::kGPU);

}  // namespace

REX_TEST_CASE("present thread order", "[gpu]") {
  GraphicsApp app("fake");
  REQUIRE(app.Start());
  REQUIRE(WaitFor([] { return g_fake.presents.load() >= 3; }));
  app.Shutdown();

  const std::vector<std::string> events = g_fake.Events();
  const std::vector<std::string> pattern = {"wait", "begin", "end"};
  REQUIRE(events.size() >= 9);
  for (size_t i = 0; i < events.size(); ++i) {
    CAPTURE(i);
    CHECK(events[i] == pattern[i % pattern.size()]);
  }
}

REX_TEST_CASE("resize before begin", "[gpu]") {
  GraphicsApp app("fake", true);
  REQUIRE(app.Start());
  const std::string resize = "resize " + std::to_string(rex::test_ui::g_ui.width) + " " +
                             std::to_string(rex::test_ui::g_ui.height);
  REQUIRE(WaitFor([&] {
    for (const std::string& event : g_fake.Events()) {
      if (event == resize) {
        return true;
      }
    }
    return false;
  }));
  app.Shutdown();

  const std::vector<std::string> events = g_fake.Events();
  bool seen = false;
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i] == resize) {
      seen = true;
      REQUIRE(i + 1 < events.size());
      CHECK(events[i + 1] == "begin");
    }
  }
  CHECK(seen);
}

REX_TEST_CASE("frame limit paces presents", "[gpu]") {
  GraphicsApp app("fake", false, "50");
  REQUIRE(app.Start());
  std::this_thread::sleep_for(2s);
  app.Shutdown();
  CHECK(g_fake.presents.load() >= 50);
}

REX_TEST_CASE("API serves the graphics system only while the app runs", "[gpu]") {
  GraphicsApp app("fake");
  CHECK(REX_INTERFACE(rex::gpu::IGraphicsSystem) == nullptr);
  REQUIRE(app.Start());
  CHECK(REX_INTERFACE(rex::gpu::IGraphicsSystem) == &app.GetGraphics());
  app.Shutdown();
  CHECK(REX_INTERFACE(rex::gpu::IGraphicsSystem) == nullptr);
}

REX_TEST_CASE("frame stats count new frames", "[gpu]") {
  GraphicsApp app("fake");
  REQUIRE(app.Start());
  rex::gpu::IGraphicsSystem& graphics = app.GetGraphics();
  g_fake.every_wait_is_new_frame = false;
  const rex::u32 first = g_fake.presents.load();
  REQUIRE(WaitFor([&] { return g_fake.presents.load() >= first + 5; }));
  const rex::u32 started_with = g_fake.new_frames.load();
  const rex::u64 counted = graphics.GetFrameStats().frame_count;
  CHECK(counted <= started_with);
  g_fake.every_wait_is_new_frame = true;
  const rex::u32 before = g_fake.new_frames.load();
  REQUIRE(WaitFor([&] { return g_fake.new_frames.load() >= before + 10; }));
  g_fake.every_wait_is_new_frame = false;
  const rex::u32 settled = g_fake.presents.load();
  REQUIRE(WaitFor([&] { return g_fake.presents.load() >= settled + 3; }));

  const rex::gpu::FrameStats stats = graphics.GetFrameStats();
  CHECK(stats.frame_count == g_fake.new_frames.load());
  CHECK(stats.fps >= 0.0);

  graphics.ReportFrame();
  CHECK(graphics.GetFrameStats().frame_count == stats.frame_count + 1);
  app.Shutdown();
}

REX_TEST_CASE("an app that needs xenos refuses a graphics driver that is not xenos", "[gpu]") {
  GraphicsApp app("plain");
  const auto started = app.Start();
  REQUIRE_FALSE(started);
  CHECK(started.error().message.find("not a Xenos driver") != std::string::npos);
}

REX_TEST_CASE("vd calls with no driver at all do not warn", "[gpu]") {
  GraphicsApp app("none", false, {}, rex::GraphicsFamily::kNone);
  WarningSink sink;
  auto tool = app.OpenTitle();
  REQUIRE(tool);
  auto sink_guard = REX_LOGGER().AddSink(sink);
  rex::gpu::IGraphicsSystem& graphics = app.GetGraphics();
  CHECK_FALSE(graphics.HasDriver());
  auto& state = StateOf(tool->GetRuntime());
  REQUIRE(state.AttachSystems(app.GetGraphicsControl(), nullptr, nullptr, nullptr, nullptr,
                              rex::VideoMode{}));
  state.InitializeGPURingBuffer(0x1000, 9);
  state.EnableGPUReadPointerWriteBack(0x2000, 6);
  CHECK(sink.count.load() == 0);
  sink_guard.Reset();
}

REX_TEST_CASE("vd calls and registers reach a xenos driver", "[gpu]") {
  GraphicsApp app("fake");
  auto tool = app.OpenTitle();
  REQUIRE(tool);
  rex::gpu::IGraphicsSystem& graphics = app.GetGraphics();
  CHECK(graphics.HasDriver());
  auto& state = StateOf(tool->GetRuntime());
  REQUIRE(state.AttachSystems(app.GetGraphicsControl(), nullptr, nullptr, nullptr, nullptr,
                              rex::VideoMode{}));
  state.InitializeGPURingBuffer(0x1000, 9);
  state.EnableGPUReadPointerWriteBack(0x2000, 6);
  rex::ppc::MMIOStore32(kRegisterBase + 0x20 * 4, 0x55);
  CHECK(rex::ppc::MMIOLoad32(kRegisterBase + 0x20 * 4) == 7);
  CHECK(g_fake.ring_calls.load() == 1);
  CHECK(g_fake.write_back_calls.load() == 1);
  CHECK(g_fake.last_write_index.load() == 0x20);
  CHECK(g_fake.last_write_value.load() == 0x55);
}

REX_TEST_CASE("GetProcessRuntime is the loaded runtime and null after Unload", "[gpu][runtime]") {
  rex::test::LoadedTitle loaded;
  CHECK(REX_RUNTIME() == &loaded.runtime);
  loaded.runtime.Unload();
  CHECK(REX_RUNTIME() == nullptr);
}
