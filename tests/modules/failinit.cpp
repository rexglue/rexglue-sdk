/**
 * @file        tests/modules/failinit.cpp
 * @brief       Test module whose Init fails
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
#include <rex/module.h>

#include "test_module_common.h"

REX_TEST_MODULE_SINK()

namespace {

constexpr const char* kName = "rexmod-test-failinit";

class FailInitModule final : public rex::IModule {
 public:
  static void* GetModule();
  static constexpr std::array<rex::InterfaceEntry, 1> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
  }};

  FailInitModule() { Record(kName, "constructor"); }

  const char* GetName() const override { return kName; }
  std::span<const char* const> GetRequiredInterfaces() const override { return {}; }
  [[nodiscard]] rex::Result<void> Connect(rex::IInterfaceFactory*, const rex::Config&) override {
    Record(kName, "connect");
    return rex::Ok();
  }
  void Disconnect() override { Record(kName, "disconnect"); }
  [[nodiscard]] rex::Result<void> Init() override {
    Record(kName, "init");
    return rex::Err(rex::ErrorCategory::kValidation, "failinit refuses");
  }
  void Shutdown() override { Record(kName, "shutdown"); }

 private:
  static FailInitModule& GetInstance() {
    static FailInitModule instance;
    return instance;
  }
};

void* FailInitModule::GetModule() {
  return static_cast<rex::IModule*>(&GetInstance());
}

}  // namespace

REX_DECLARE_MODULE(FailInitModule)
