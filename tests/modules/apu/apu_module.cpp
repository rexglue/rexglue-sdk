/**
 * @file        tests/modules/apu/apu_module.cpp
 * @brief       apu backend test module serving a null audio output
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <optional>
#include <span>

#include <rex/api.h>
#include <rex/apu.h>
#include <rex/module.h>

namespace {

class APUModule final : public rex::IModule {
 public:
  static void* GetModule() { return static_cast<rex::IModule*>(&Instance()); }
  static void* GetBackend() {
    auto& backend = Instance().backend_;
    if (!backend) {
      backend.emplace();
    }
    return static_cast<rex::apu::IAPUBackend*>(&*backend);
  }
  static constexpr std::array<rex::InterfaceEntry, 2> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
      {rex::apu::IAPUBackend::kInterfaceName, rex::apu::IAPUBackend::kInterfaceVersion,
       &GetBackend},
  }};

  const char* Name() const override { return "rexmod-test-apu"; }
  std::span<const char* const> RequiredInterfaces() const override { return {}; }
  [[nodiscard]] rex::Result<void> Connect(rex::IInterfaceFactory*, const rex::Config&) override {
    return rex::Ok();
  }
  void Disconnect() override { backend_.reset(); }
  [[nodiscard]] rex::Result<void> Init() override { return rex::Ok(); }
  void Shutdown() override {}

 private:
  static APUModule& Instance() {
    static APUModule instance;
    return instance;
  }
  std::optional<rex::apu::NullAPUBackend> backend_;
};

}  // namespace

REX_DECLARE_MODULE(APUModule)
