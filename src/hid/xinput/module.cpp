/**
 * @file        hid/xinput/module.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <memory>

#include "hid/common/module_base.h"
#include "hid/xinput/xinput_driver.h"

namespace rex::hid::xinput {

class XInputModule final : public HIDModuleBase {
 public:
  static void* GetModule() { return static_cast<IModule*>(&GetInstance()); }
  static void* GetBackend() { return static_cast<hid::IHIDBackend*>(&GetInstance().GetHub()); }

  static constexpr std::array<InterfaceEntry, 2> kInterfaces = {{
      {IModule::kInterfaceName, IModule::kInterfaceVersion, &GetModule},
      {hid::IHIDBackend::kInterfaceName, hid::IHIDBackend::kInterfaceVersion, &GetBackend},
  }};

  const char* GetName() const override { return "rex-hid-xinput"; }

 protected:
  [[nodiscard]] Result<std::unique_ptr<Driver>> CreatePadDriver(const HIDConfig&) override {
    auto driver = std::make_unique<XInputDriver>();
    REX_TRY_VOID(driver->Setup());
    return std::unique_ptr<Driver>(std::move(driver));
  }

 private:
  static XInputModule& GetInstance() {
    static XInputModule instance;
    return instance;
  }
};

}  // namespace rex::hid::xinput

REX_DECLARE_MODULE(rex::hid::xinput::XInputModule)
