/**
 * @file        tests/unit/ui/hid_attach_test.cpp
 * @brief       The window hands the pointer to the HID backend with or without a presenter
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"

#include <memory>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_mouse.h>

#include <rex/api.h>
#include <rex/cvar/registry.h>
#include <rex/gpu.h>
#include <rex/hid.h>
#include <rex/module.h>
#include <rex/ui/user_interface.h>

#include "core/cvar_registry.h"
#include "hid/hub.h"

extern "C" void* ReXCreateInterface(const char* name, rex::u32 version, rex::i32* out_status);

namespace {

class HubFactory final : public rex::IInterfaceFactory {
 public:
  [[nodiscard]] rex::Result<void*> Get(std::string_view name, rex::u32 version) override {
    (void)version;
    if (name == rex::gpu::IGPUBackend::kInterfaceName) {
      ++gpu_requests;
    }
    if (name == rex::cvar::ICvarRegistry::kInterfaceName) {
      return static_cast<void*>(registry);
    }
    if (name == rex::hid::IHIDBackend::kInterfaceName) {
      return static_cast<void*>(static_cast<rex::hid::IHIDBackend*>(hub));
    }
    return rex::Err<void*>(rex::ErrorCategory::kModule, "not served");
  }

  rex::cvar::ICvarRegistry* registry = nullptr;
  rex::hid::Hub* hub = nullptr;
  int gpu_requests = 0;
};

}  // namespace

REX_TEST_CASE("An open window applies the pointer mode without a presenter", "[ui][hid]") {
  rex::test::TempFolder user_root{"rex_hid_attach_test_"};
  auto registry = rex::detail::CreateCvarRegistry();
  rex::hid::Hub hub;
  HubFactory factory;
  factory.registry = registry.get();
  factory.hub = &hub;
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  rex::Config config;
  config.ui.video_driver = "dummy";
  config.paths.user_data_root = user_root.path;
  rex::i32 status = 0;
  auto* module = static_cast<rex::IModule*>(ReXCreateInterface("rex.module", 1, &status));
  auto* ui = static_cast<rex::ui::IUserInterface*>(ReXCreateInterface("rex.ui", 1, &status));
  REQUIRE(module != nullptr);
  REQUIRE(ui != nullptr);
  REQUIRE(module->Connect(&factory, config));
  REQUIRE(module->Init());
  auto opened = ui->OpenWindow(config.ui, [](const rex::ui::UIEvent&) { return false; });
  if (!opened) {
    module->Shutdown();
    module->Disconnect();
    SKIP("SDL dummy video driver unavailable");
  }
  hub.SetPointerMode(rex::hid::PointerMode::kHidden);
  for (rex::i32 frame = 0; frame < 100 && SDL_CursorVisible(); ++frame) {
    ui->PumpFrame();
  }
  CHECK_FALSE(SDL_CursorVisible());
  CHECK(hub.GetPointer().window_width > 0);
  ui->CloseWindow();
  module->Shutdown();
  module->Disconnect();
}
REX_TEST_CASE("The UI module requests the GPU backend in Init", "[ui][gpu]") {
  rex::test::TempFolder user_root{"rex_gpu_fetch_test_"};
  auto registry = rex::detail::CreateCvarRegistry();
  rex::hid::Hub hub;
  HubFactory factory;
  factory.registry = registry.get();
  factory.hub = &hub;
  rex::Config config;
  config.paths.user_data_root = user_root.path;
  rex::i32 status = 0;
  auto* module = static_cast<rex::IModule*>(ReXCreateInterface("rex.module", 1, &status));
  REQUIRE(module != nullptr);
  REQUIRE(module->Connect(&factory, config));
  CHECK(factory.gpu_requests == 0);
  REQUIRE(module->Init());
  CHECK(factory.gpu_requests == 1);
  module->Shutdown();
  module->Disconnect();
}
