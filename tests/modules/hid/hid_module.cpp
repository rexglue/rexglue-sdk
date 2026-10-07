/**
 * @file        tests/modules/hid/hid_module.cpp
 * @brief       hid backend test module serving the null HID backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <span>

#include <rex/api.h>
#include <rex/hid.h>
#include <rex/module.h>

namespace {

class HIDModule final : public rex::IModule {
 public:
  static void* GetModule() { return static_cast<rex::IModule*>(&Instance()); }
  static void* GetBackend() { return static_cast<rex::hid::IBackend*>(&Instance().backend_); }
  static constexpr std::array<rex::InterfaceEntry, 2> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
      {rex::hid::IBackend::kInterfaceName, rex::hid::IBackend::kInterfaceVersion, &GetBackend},
  }};

  const char* Name() const override { return "rexmod-test-hid"; }
  std::span<const char* const> RequiredInterfaces() const override { return {}; }
  [[nodiscard]] rex::Result<void> Connect(rex::IInterfaceFactory*, const rex::Config&) override {
    return rex::Ok();
  }
  void Disconnect() override {}
  [[nodiscard]] rex::Result<void> Init() override { return rex::Ok(); }
  void Shutdown() override {}

 private:
  static HIDModule& Instance() {
    static HIDModule instance;
    return instance;
  }
  rex::hid::NullBackend backend_;
};

}  // namespace

REX_DECLARE_MODULE(HIDModule)
