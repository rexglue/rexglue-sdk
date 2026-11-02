/**
 * @file        tests/unit/kernel/title_fixture.h
 * @brief       TitleRuntime fixtures with the runtime settings registered
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "rex_test.h"
#include "object_app.h"

#include <filesystem>
#include <functional>
#include <span>
#include <string>

#include <rex/cli.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_paths.h>

#include "codegen/tool_runtime.h"
#include "kernel/settings_access.h"
#include "kernel/title_image.h"
#include "temp_folder.h"
#include "test_memory.h"

namespace rex::test {

/** Runs rex::Init with the default modules, so a TitleRuntime finds the kernel module running. */
struct KernelHostApp final : rex::test::ObjectApp<codegen::ToolApp> {
  using ObjectApp::ObjectApp;

  IFactory& GetPrivateFactory() { return GetControlFactory(); }
};

struct KernelHost {
  explicit KernelHost(const std::filesystem::path& root) : app("kernel_host", root) {
    REQUIRE(app.Init(CLIArgList::Parse(std::span<const std::string>{})));
  }

  ~KernelHost() { app.Shutdown(); }

  KernelHostApp app;
};

/** Runs the kernel module and restores the runtime settings when it ends. */
struct TitleBoot {
  /**
   * @param title_on_disk True writes a title the image loads, false loads no title file.
   * @param adjust Sets settings before the first Load.
   */
  explicit TitleBoot(bool title_on_disk = false, const std::function<void()>& adjust = nullptr)
      : folder("rex_title_boot_") {
    ReleaseTestMemory();
    if (title_on_disk) {
      WriteTitle(folder.path / "game");
      REQUIRE(kernel::GetTitlePaths().game_root.Set(folder.path / "game"));
    } else {
      REQUIRE(kernel::GetTitlePaths().entry.Set(""));
    }
    if (adjust) {
      adjust();
    }
  }

  ~TitleBoot() {
    (void)kernel::GetTitlePaths().game_root.Reset();
    (void)kernel::GetTitlePaths().game_writable.Reset();
    (void)kernel::GetTitlePaths().metadata_root.Reset();
    (void)kernel::GetTitlePaths().entry.Reset();
  }

  TempFolder folder;
  KernelHost host{folder.path};
};

/** A loaded runtime over an image, tool mode when the image has no code. */
struct LoadedTitle : TitleBoot {
  explicit LoadedTitle(const ppc::PPCImage& image = {}, bool title_on_disk = false,
                       const std::function<void()>& adjust = nullptr)
      : TitleBoot(title_on_disk, adjust), runtime(image) {
    REQUIRE(runtime.Load());
  }

  ~LoadedTitle() { runtime.Unload(); }

  rex::kernel::TitleRuntime runtime;
};

}  // namespace rex::test
