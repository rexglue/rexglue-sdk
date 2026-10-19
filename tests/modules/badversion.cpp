/**
 * @file        tests/modules/badversion.cpp
 * @brief       Test module that serves the module interface at version 2 only
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

#include "test_module_sink.h"

REX_TEST_MODULE_SINK()

namespace {

constexpr const char* kName = "rexmod-test-badversion";

class BadVersionModule final : public rex::IModule {
 public:
  static void* GetModule();
  static constexpr std::array<rex::InterfaceEntry, 1> kInterfaces = {{
      {rex::IModule::kInterfaceName, 2, &GetModule},
  }};

  BadVersionModule() { ReportEvent(kName, "constructor"); }

  const char* GetName() const override { return kName; }
  std::span<const char* const> GetRequiredInterfaces() const override { return {}; }
  [[nodiscard]] rex::Result<void> Connect(rex::IInterfaceFactory*, const rex::Config&) override {
    ReportEvent(kName, "connect");
    return rex::Ok();
  }
  void Disconnect() override { ReportEvent(kName, "disconnect"); }
  [[nodiscard]] rex::Result<void> Init() override {
    ReportEvent(kName, "init");
    return rex::Ok();
  }
  void Shutdown() override { ReportEvent(kName, "shutdown"); }

 private:
  static BadVersionModule& GetInstance() {
    static BadVersionModule instance;
    return instance;
  }
};

void* BadVersionModule::GetModule() {
  return static_cast<rex::IModule*>(&GetInstance());
}

}  // namespace

REX_DECLARE_MODULE(BadVersionModule)
