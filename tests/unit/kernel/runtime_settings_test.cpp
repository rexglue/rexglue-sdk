/**
 * @file        tests/unit/kernel/runtime_settings_test.cpp
 * @brief       Runtime and kernel setting registration
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <chrono>
#include <memory>
#include <string>

#include <rex/kernel/title_runtime.h>
#include <rex/settings_registry.h>

#include "kernel/settings_access.h"
#include "kernel/title_fixture.h"
#include "temp_folder.h"

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
