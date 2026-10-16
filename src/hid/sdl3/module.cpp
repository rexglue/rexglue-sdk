/**
 * @file        hid/sdl3/module.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <memory>

#include <SDL3/SDL_hints.h>

#include "hid/common/module_base.h"
#include "hid/sdl3/sdl3_driver.h"

namespace rex::hid::sdl3 {

class SDL3Module final : public HIDModuleBase {
 public:
  static void* GetModule() { return static_cast<IModule*>(&GetInstance()); }
  static void* GetBackend() { return static_cast<hid::IHIDBackend*>(&GetInstance().GetHub()); }

  static constexpr std::array<InterfaceEntry, 2> kInterfaces = {{
      {IModule::kInterfaceName, IModule::kInterfaceVersion, &GetModule},
      {hid::IHIDBackend::kInterfaceName, hid::IHIDBackend::kInterfaceVersion, &GetBackend},
  }};

  const char* GetName() const override { return "rexhid-sdl3"; }

  [[nodiscard]] Result<void> Connect(IInterfaceFactory* factory, const Config& config) override {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    return HIDModuleBase::Connect(factory, config);
  }

 protected:
  [[nodiscard]] Result<std::unique_ptr<Driver>> CreatePadDriver(const HIDConfig& config) override {
    auto driver = std::make_unique<SDL3Driver>(config.mappings_file);
    REX_TRY_VOID(driver->Setup());
    return std::unique_ptr<Driver>(std::move(driver));
  }

 private:
  static SDL3Module& GetInstance() {
    static SDL3Module instance;
    return instance;
  }
};

}  // namespace rex::hid::sdl3

REX_DECLARE_MODULE(rex::hid::sdl3::SDL3Module)
