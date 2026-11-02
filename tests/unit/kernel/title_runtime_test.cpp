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
  CHECK(StateOf(title).GetMemory() == nullptr);
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
  CHECK(StateOf(title).GetMemory() == nullptr);
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

REX_TEST_CASE_METHOD(Title, "the settings come from the title runtime", "[kernel][title]") {
  rex::kernel::TitleRuntime title(MakeImage());
  CHECK(&title.GetRuntimeSettings() == &rex::kernel::GetRuntimeSettings());
  CHECK(REX_SETTINGS().Find("kernel.headless").value() == &title.GetKernelSettings().headless);
  CHECK(&title.GetTitlePaths() == &rex::kernel::GetTitlePaths());
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
  CHECK(StateOf(title).GetMemory() == nullptr);
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
