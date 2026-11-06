/**
 * @file        tests/unit/kernel/title_runtime_test.cpp
 * @brief       Title load and unload
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <rex/filesystem/file_open.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_paths.h>
#include <rex/kernel/runtime_settings.h>
#include <rex/kernel/kernel_settings.h>

#include "kernel/settings_access.h"
#include "kernel/title_fixture.h"
#include "kernel/title_image.h"
#include "temp_folder.h"
#include "test_memory.h"
#include "kernel/title_state.h"
#include "xam/xam_state.h"
#include <chrono>
#include <memory>
#include <rex/settings_registry.h>
#include <filesystem>
#include "kernel/apu.h"
#include <algorithm>
#include <string_view>
#include <rex/logging/log_category.h>
#include <rex/ppc/image.h>
#include <rex/logging/logger.h>
#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/kernel/thread_state.h>
#include <array>
#include <span>
#include <rex/filesystem/mount.h>
#include <rex/filesystem/file_system.h>
#include "kernel/shim/heap.h"
#include "object_app.h"
#include <rex/app/app.h>
#include <rex/cli.h>
#include <rex/filesystem/file_system_factory.h>
#include <rex/module.h>
#include <rex/ui/ui_system.h>
#include "kernel/kernel_control.h"
#include "codegen/tool_runtime.h"
#include "rexglue/log_categories.h"

namespace {

struct Title {
  rex::test::TempFolder folder{"rex_title_runtime_"};
  rex::test::KernelHost host{folder.path};

  Title() {
    rex::test::ReleaseTestMemory();
    std::filesystem::create_directories(folder.path / "game");
  }
  ~Title() { (void)rex::kernel::GetTitlePaths().game_root.Reset(); }

  void WriteImage() { rex::test::WriteTitle(folder.path / "game"); }

  void Start() { REQUIRE(rex::kernel::GetTitlePaths().game_root.Set(folder.path / "game")); }
};

rex::ppc::PPCImage MakeImage() {
  return rex::test::MakeTitleImage();
}

}  // namespace

REX_TEST_CASE_METHOD(Title, "load reads the image without running", "[kernel][title]") {
  WriteImage();
  Start();
  rex::kernel::TitleRuntime title(MakeImage());
  REQUIRE(title.Load());
  CHECK(title.IsLoaded());
  CHECK_FALSE(title.IsRunning());
  CHECK(title.GetTitleID() == rex::test::kTitleId);
  CHECK_FALSE(StateOf(title).IsTitleLaunched());
  CHECK(StateOf(title).GetAPU() == nullptr);
}

REX_TEST_CASE_METHOD(Title, "load without an entrypoint leaves nothing loaded", "[kernel][title]") {
  Start();
  rex::kernel::TitleRuntime title(MakeImage());
  CHECK_FALSE(title.Load());
  CHECK_FALSE(title.IsLoaded());
  CHECK(StateOf(title).GetAddressSpace() == nullptr);
  CHECK(title.GetTitleID() == 0);
}

REX_TEST_CASE_METHOD(Title, "unload leaves nothing and the title loads again", "[kernel][title]") {
  WriteImage();
  Start();
  rex::kernel::TitleRuntime title(MakeImage());
  REQUIRE(title.Load());
  rex::xam::CurrentXAM().SetOnlineSchemaAddress(0x1234);
  title.Unload();
  CHECK_FALSE(title.IsLoaded());
  CHECK(StateOf(title).GetAddressSpace() == nullptr);
  CHECK(title.GetTitleID() == 0);

  REQUIRE(title.Load());
  CHECK(title.IsLoaded());
  CHECK(rex::xam::CurrentXAM().GetOnlineSchemaAddress() == 0);
  CHECK(title.GetTitleID() == rex::test::kTitleId);
}

REX_TEST_CASE_METHOD(Title, "a loaded title refuses a second load", "[kernel][title]") {
  WriteImage();
  Start();
  rex::kernel::TitleRuntime title(MakeImage());
  REQUIRE(title.Load());
  CHECK_FALSE(title.Load());
  CHECK(title.IsLoaded());
}

REX_TEST_CASE_METHOD(Title, "a second runtime cannot load while the first is loaded",
                     "[kernel][title][guard]") {
  WriteImage();
  Start();
  rex::kernel::TitleRuntime first(MakeImage());
  rex::kernel::TitleRuntime second(MakeImage());
  REQUIRE(first.Load());
  CHECK(REX_RUNTIME() == &first);

  auto refused = second.Load();
  REQUIRE_FALSE(refused);
  CHECK(refused.error().message.find("another runtime is active") != std::string::npos);
  CHECK_FALSE(second.IsLoaded());
  CHECK(first.IsLoaded());
  CHECK(REX_RUNTIME() == &first);

  first.Unload();
  CHECK(REX_RUNTIME() == nullptr);
  REQUIRE(second.Load());
  CHECK(REX_RUNTIME() == &second);
}

REX_TEST_CASE_METHOD(Title, "a failed load leaves nothing behind", "[kernel][title]") {
  WriteImage();
  Start();
  rex::kernel::TitleRuntime title(MakeImage());
  REQUIRE(rex::kernel::GetTitlePaths().game_root.Set(folder.path / "missing"));

  auto failed = title.Load();
  REQUIRE_FALSE(failed);
  CHECK(failed.error().message.find("is not a folder") != std::string::npos);
  CHECK_FALSE(title.IsLoaded());
  CHECK(title.GetTitleID() == 0);
  CHECK(StateOf(title).GetAddressSpace() == nullptr);
  CHECK(StateOf(title).GetFileSystem() == nullptr);
  CHECK(REX_RUNTIME() == nullptr);

  REQUIRE(rex::kernel::GetTitlePaths().game_root.Set(folder.path / "game"));
  REQUIRE(title.Load());
  CHECK(title.IsLoaded());
  CHECK(title.GetTitleID() == rex::test::kTitleId);
}

namespace {

bool CanCreateOnGame(rex::kernel::TitleRuntime& runtime, const std::filesystem::path& game_root,
                     std::string_view name) {
  using namespace rex::filesystem;
  auto opened = StateOf(runtime).GetFileSystem()->Open(
      std::string("game:\\") + std::string(name),
      {.disposition = Disposition::kCreate, .access = Access::kWriteAccess});
  return opened.has_value() && std::filesystem::exists(game_root / name);
}

}  // namespace

REX_TEST_CASE("game: is read only by default", "[kernel][filesystem]") {
  rex::test::TempFolder temp("rex_game_writable_");
  const auto game_root = temp.path / "game";
  std::filesystem::create_directories(game_root);
  rex::test::LoadedTitle loaded(
      {}, false, [&] { REQUIRE(rex::kernel::GetTitlePaths().game_root.Set(game_root)); });
  CHECK_FALSE(CanCreateOnGame(loaded.runtime, game_root, "a.txt"));
}

REX_TEST_CASE("title.game_writable mounts game: read write", "[kernel][filesystem]") {
  rex::test::TempFolder temp("rex_game_writable_");
  const auto game_root = temp.path / "game";
  std::filesystem::create_directories(game_root);
  rex::test::LoadedTitle loaded({}, false, [&] {
    REQUIRE(rex::kernel::GetTitlePaths().game_root.Set(game_root));
    REQUIRE(rex::kernel::GetTitlePaths().game_writable.Set(true));
  });
  CHECK(CanCreateOnGame(loaded.runtime, game_root, "a.txt"));
}

REX_TEST_CASE("title.game_writable changes the mount only at the next Load",
              "[kernel][filesystem]") {
  rex::test::TempFolder temp("rex_game_writable_");
  const auto game_root = temp.path / "game";
  std::filesystem::create_directories(game_root);
  rex::test::LoadedTitle loaded({}, false, [&] {
    REQUIRE(rex::kernel::GetTitlePaths().game_root.Set(game_root));
    REQUIRE(rex::kernel::GetTitlePaths().game_writable.Set(true));
  });
  REQUIRE(rex::kernel::GetTitlePaths().game_writable.Set(false));
  CHECK(CanCreateOnGame(loaded.runtime, game_root, "a.txt"));
  loaded.runtime.Unload();
  REQUIRE(loaded.runtime.Load());
  CHECK_FALSE(CanCreateOnGame(loaded.runtime, game_root, "b.txt"));
}

REX_TEST_CASE("the settings are registered while the kernel module is connected",
              "[kernel][settings]") {
  CHECK_FALSE(REX_SETTINGS().Find("title.game_root"));
  {
    rex::test::TempFolder folder("rex_runtime_settings_");
    rex::test::KernelHost host(folder.path);
    CHECK(REX_SETTINGS().Find("title.game_root"));
    CHECK(REX_SETTINGS().Find("runtime.heap_size_mb"));
    CHECK(REX_SETTINGS().Find("kernel.headless"));
  }
  CHECK_FALSE(REX_SETTINGS().Find("title.game_root"));
  CHECK_FALSE(REX_SETTINGS().Find("runtime.heap_size_mb"));
  CHECK_FALSE(REX_SETTINGS().Find("kernel.headless"));
}

namespace host_service_modules_tests {

namespace {

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "Tool mode builds no APU", "[kernel][modules]") {
  rex::test::LoadedTitle loaded;
  rex::kernel::TitleRuntime& runtime = loaded.runtime;
  CHECK(StateOf(runtime).GetAPU() == nullptr);
  runtime.SetAudioPaused(true);
  CHECK_FALSE(runtime.IsAudioPaused());
}

}  // namespace host_service_modules_tests

namespace call_usage_lookup_tests {

extern "C" void __imp__LookupNarrow(PPCContext& ctx, u8*) {
  ctx.r3.u64 = ctx.r4.u64;
}

REX_DETOUR(LookupNarrow, u32, (u32 first)) {
  return original(first);
}

extern "C" void __imp__LookupCovered(PPCContext& ctx, u8*) {
  ctx.r3.u64 = ctx.r4.u64;
}

REX_DETOUR(LookupCovered, u32, (u32 first, u32 second)) {
  return original(first, second);
}

namespace {

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
};

void MappedUnlisted(PPCContext&, rex::u8*) {}

void MappedListed(PPCContext&, rex::u8*) {}

void NotMapped(PPCContext&, rex::u8*) {}

const PPCFuncMapping kMappings[] = {
    {0x82000100, MappedUnlisted},
    {0x82000200, MappedListed},
    {0x82000300, LookupNarrow},
    {0x82000400, LookupCovered},
    {0, nullptr},
};

const rex::ppc::PPCCallUsage kUsage[] = {
    {0x82000200, 0x5, 0x2},
    {0x82000300, 0x2, 0},
    {0x82000400, 0x2, 0},
    {0, 0, 0},
};

size_t CountMessages(const std::vector<rex::log::CapturedMessage>& messages,
                     std::string_view text) {
  return static_cast<size_t>(std::ranges::count_if(
      messages,
      [&](const rex::log::CapturedMessage& message) { return message.text.contains(text); }));
}

}  // namespace

REX_TEST_CASE_METHOD(Fixture,
                     "call usage: the runtime looks a mapped function up by its host function",
                     "[kernel][call_usage]") {
  rex::test::TitleBoot boot(true);
  rex::ppc::PPCImage image = rex::test::MakeTitleImage(kMappings);
  image.code_base = 0x82000000;
  image.call_usage = kUsage;
  rex::kernel::TitleRuntime runtime(image);
  REQUIRE(runtime.Load());

  const auto usage = StateOf(runtime).GetCallUsage(MappedListed);
  REQUIRE(usage.has_value());
  const rex::ppc::PPCCallUsage found = usage.value_or(rex::ppc::PPCCallUsage{});
  CHECK(found.address == 0x82000200);
  CHECK(found.gpr_mask == 0x5);
  CHECK(found.fpr_mask == 0x2);
  CHECK_FALSE(StateOf(runtime).GetCallUsage(MappedUnlisted).has_value());
  CHECK_FALSE(StateOf(runtime).GetCallUsage(NotMapped).has_value());
  CHECK_FALSE(StateOf(runtime).GetCallUsage(nullptr).has_value());
  runtime.Unload();
}

REX_TEST_CASE_METHOD(Fixture, "call usage: a hook is found by its symbol and checked on first call",
                     "[kernel][call_usage]") {
  rex::test::TitleBoot boot(true);
  rex::Logger* logger = &REX_LOGGER();
  REQUIRE(logger);
  rex::ppc::PPCImage image = rex::test::MakeTitleImage(kMappings);
  image.code_base = 0x82000000;
  image.call_usage = kUsage;
  rex::kernel::TitleRuntime runtime(image);
  REQUIRE(runtime.Load());

  const auto narrow = StateOf(runtime).GetCallUsage(LookupNarrow);
  REQUIRE(narrow.has_value());
  CHECK(narrow.value_or(rex::ppc::PPCCallUsage{}).address == 0x82000300);

  alignas(64) static u8 memory[0x10000] = {};
  rex::kernel::ThreadState state(1, 0x8000, 0, StateOf(runtime).GetAddressSpace(), &runtime);
  rex::kernel::ThreadState* previous = rex::kernel::ThreadState::Get();
  rex::kernel::ThreadState::Bind(&state);
  rex::log::LogCaptureSink sink;
  auto sink_guard = logger->AddSink(sink);

  PPCContext& ctx = *state.GetContext();
  ctx.r3.u64 = 1;
  ctx.r4.u64 = 2;
  LookupNarrow(ctx, memory);
  LookupNarrow(ctx, memory);
  LookupCovered(ctx, memory);
  LookupCovered(ctx, memory);

  std::vector<rex::log::CapturedMessage> messages;
  sink.CopySince(0, messages);
  sink_guard.Reset();
  rex::kernel::ThreadState::Bind(previous);
  runtime.Unload();

#ifndef NDEBUG
  CHECK(CountMessages(messages, "LookupNarrow does not declare r4") == 1);
#else
  CHECK(CountMessages(messages, "LookupNarrow does not declare") == 0);
#endif
  CHECK(CountMessages(messages, "LookupCovered does not declare") == 0);
}

}  // namespace call_usage_lookup_tests

namespace title_public_access_tests {

namespace {

using rex::filesystem::Access;
using rex::filesystem::Disposition;
using rex::filesystem::MountEntry;
using rex::filesystem::MountFlags;
using rex::filesystem::OpenOptions;
using rex::filesystem::ProviderType;

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
};

void Registered(PPCContext&, rex::u8*) {}

const PPCFuncMapping kMappings[] = {
    {0x82000100, Registered},
    {0, nullptr},
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "FindFunction returns the function registered at an address",
                     "[kernel][title]") {
  rex::test::TitleBoot boot(true);
  rex::ppc::PPCImage image = rex::test::MakeTitleImage(kMappings);
  image.code_base = 0x82000000;
  rex::kernel::TitleRuntime runtime(image);
  REQUIRE(runtime.Load());

  const auto found = runtime.FindFunction(0x82000100);
  REQUIRE(found);
  CHECK(*found == Registered);
  runtime.Unload();
}

REX_TEST_CASE_METHOD(Fixture, "FindFunction fails for an address with no function",
                     "[kernel][title]") {
  rex::test::TitleBoot boot(true);
  rex::ppc::PPCImage image = rex::test::MakeTitleImage(kMappings);
  image.code_base = 0x82000000;
  rex::kernel::TitleRuntime runtime(image);
  REQUIRE(runtime.Load());

  const auto missing = runtime.FindFunction(0x82000200);
  REQUIRE_FALSE(missing);
  CHECK(missing.error().category == rex::ErrorCategory::kNotFound);
  runtime.Unload();
}

REX_TEST_CASE_METHOD(Fixture, "GetFileSystem mounts a writable host folder and links a path",
                     "[kernel][title]") {
  rex::test::TitleBoot boot(true);
  rex::kernel::TitleRuntime runtime(rex::test::MakeTitleImage());
  REQUIRE(runtime.Load());
  rex::filesystem::IFileSystem& files = runtime.GetFileSystem();

  const std::filesystem::path save = boot.folder.path / "save";
  std::filesystem::create_directories(save);
  REQUIRE(files.Mount(MountEntry{.path = "\\Device\\Save",
                                 .type = ProviderType::kHostDirectory,
                                 .source = save,
                                 .flags = MountFlags::kWritable}));
  REQUIRE(files.Link("save:", "\\Device\\Save"));
  CHECK(files.FindLink("save:\\slot0.bin").value_or("") == "\\Device\\Save");

  {
    auto opened = files.Open("save:\\slot0.bin", OpenOptions{.disposition = Disposition::kCreate,
                                                             .access = Access::kWriteData});
    REQUIRE(opened);
    const std::array<rex::u8, 4> bytes = {1, 2, 3, 4};
    REQUIRE(opened->file->Write(std::span<const rex::u8>(bytes), 0));
  }
  CHECK(std::filesystem::file_size(save / "slot0.bin") == 4);

  REQUIRE(files.Unlink("save:"));
  CHECK_FALSE(files.FindLink("save:\\slot0.bin").has_value());
  CHECK_FALSE(files.Unlink("save:"));
  runtime.Unload();
}

}  // namespace title_public_access_tests

namespace kernel_state_tests {

namespace {

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "unload resets the kernel state", "[kernel][kernel_state]") {
  rex::test::LoadedTitle loaded;
  rex::kernel::TitleRuntime& runtime = loaded.runtime;

  rex::kernel::TitleState& state = StateOf(runtime);
  REQUIRE(rex::kernel::shim::InitHeap(8, StateOf(runtime).GetAddressSpace()));
  CHECK(state.heap.Alloc(64, true) != 0);
  state.fiber_count = 7;
  state.audio_submit_logs = 10;

  runtime.Unload();

  CHECK(state.fiber_count == 0);
  CHECK(state.audio_submit_logs == 0);
}

REX_TEST_CASE_METHOD(Fixture, "two runtimes in turn start from the same kernel state",
                     "[kernel][kernel_state]") {
  rex::test::TitleBoot boot;
  {
    rex::kernel::TitleRuntime first{rex::ppc::PPCImage{}};
    REQUIRE(first.Load());
    StateOf(first).fiber_count = 9;
    first.Unload();
  }
  rex::kernel::TitleRuntime second{rex::ppc::PPCImage{}};
  REQUIRE(second.Load());
  CHECK(StateOf(second).fiber_count == 0);
}

REX_TEST_CASE_METHOD(Fixture, "launch closes the until launch settings and unload opens them",
                     "[kernel][settings]") {
  rex::test::LoadedTitle loaded;
  rex::kernel::TitleRuntime& runtime = loaded.runtime;

  rex::kernel::RuntimeSettings& settings = rex::kernel::GetRuntimeSettings();
  CHECK(settings.title_memory_mb.Set(100));
  CHECK(settings.ignore_thread_priorities.Set(false));

  StateOf(runtime).ReportTitleLaunched();
  CHECK_FALSE(settings.title_memory_mb.Set(200));
  CHECK(settings.title_memory_mb.Get() == 100);
  CHECK(settings.ignore_thread_priorities.Set(true));

  runtime.Unload();
  CHECK(settings.title_memory_mb.Set(300));
  CHECK(settings.title_memory_mb.Get() == 300);
}

}  // namespace kernel_state_tests

namespace kernel_module_tests {

namespace {

class ListApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  explicit ListApp(std::string dropped)
      : ObjectApp("kernel_module_test"), dropped_(std::move(dropped)) {}

  rex::Result<void> Start() { return Init(rex::CLIArgList::Parse(std::span<const std::string>{})); }

 protected:
  void OnModules(std::vector<std::string>& modules) override {
    std::erase(modules, dropped_);
    ObjectApp::OnModules(modules);
  }

 private:
  std::string dropped_;
};

}  // namespace

REX_TEST_CASE("the kernel refuses to start without IUISystem", "[kernel][module]") {
  ListApp app("rexui");
  const auto started = app.Start();
  REQUIRE_FALSE(started);
  CHECK(started.error().message.find(rex::ui::IUISystem::kInterface.name) != std::string::npos);
  CHECK(app.Shutdown() == 1);
}

REX_TEST_CASE("the kernel loads no module of its own", "[kernel][module]") {
  ListApp app("rexfilesystem");
  const auto started = app.Start();
  REQUIRE_FALSE(started);
  CHECK(started.error().message.find(rex::filesystem::IFileSystemFactory::kInterface.name) !=
        std::string::npos);
  CHECK(app.Shutdown() == 1);
}

REX_TEST_CASE("the kernel module serves IKernelControl only through the private factory",
              "[kernel][module]") {
  rex::test::TempFolder folder("rex_kernel_module_");
  rex::test::KernelHost host(folder.path);
  CHECK(host.app.GetPrivateFactory().Get<rex::kernel::IKernelControl>() != nullptr);
  CHECK(REX_INTERFACE(rex::kernel::IKernelControl) == nullptr);
}

}  // namespace kernel_module_tests

namespace tool_runtime_tests {

REX_TEST_CASE("The tool runtime opens under the log categories the rexglue tool registers",
              "[codegen][tool]") {
  const rex::test::TempFolder folder("rex_tool_runtime_log_test_");
  const rex::test::TempFolder app_root("rex_tool_runtime_log_app_");
  const std::filesystem::path& root = folder.path;
  rex::test::ReleaseTestMemory();
  rex::Logger& logger = REX_LOGGER();
  REQUIRE(logger.debugger.Set(false));
  REQUIRE(logger.AddCategories(rexglue::kLogCategories));
  {
    rex::test::ObjectApp<rex::codegen::ToolApp> app("tool_runtime_test", app_root.path);
    auto tool = app.Open(root);
    INFO((tool ? std::string() : tool.error().message));
    CHECK(tool.has_value());
  }
  logger.RemoveCategories(rexglue::kLogCategories);
}

namespace {

class CrashProbe final : public rex::test::ObjectApp<rex::codegen::ToolApp> {
 public:
  CrashProbe() : ObjectApp("crash_probe") {}
  using ToolApp::GetCrashOptions;
};

class PlainApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  PlainApp() : ObjectApp("plain_app") {}
  using App::GetCrashOptions;
};

}  // namespace

REX_TEST_CASE("console app crash options show no dialog", "[codegen][tool]") {
  CHECK_FALSE(CrashProbe().GetCrashOptions().show_dialog);
  CHECK_FALSE(PlainApp().GetCrashOptions().show_dialog);
}

REX_TEST_CASE("codegen loads without launching", "[codegen][tool]") {
  const rex::test::TempFolder folder("rex_tool_runtime_load_test_");
  const rex::test::TempFolder app_root("rex_tool_runtime_load_app_");
  rex::test::ReleaseTestMemory();
  rex::test::ObjectApp<rex::codegen::ToolApp> app("tool_runtime_test", app_root.path);
  auto tool = app.Open(folder.path);
  REQUIRE(tool.has_value());
  rex::kernel::TitleRuntime& runtime = tool->GetRuntime();
  CHECK(runtime.IsLoaded());
  CHECK_FALSE(runtime.IsRunning());
  CHECK(StateOf(runtime).GetFileSystem() != nullptr);
}

}  // namespace tool_runtime_tests
