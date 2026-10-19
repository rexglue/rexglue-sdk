/**
 * @file        tests/modules/duplicate-echo.cpp
 * @brief       Test module that serves the echo interface
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

#include "echo.h"
#include "test_module_sink.h"

REX_TEST_MODULE_SINK()

namespace {

constexpr const char* kName = "rexmod-test-duplicate-echo";

class DuplicateEchoModule final : public rex::IModule, public rex::test::IEcho {
 public:
  static void* GetModule();
  static void* GetEcho();
  static constexpr std::array<rex::InterfaceEntry, 2> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
      {rex::test::IEcho::kInterfaceName, rex::test::IEcho::kInterfaceVersion, &GetEcho},
  }};

  DuplicateEchoModule() { ReportEvent(kName, "constructor"); }

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
  int Echo(int value) override { return value * 2; }
  int GetValue() override { return 0; }

 private:
  static DuplicateEchoModule& GetInstance() {
    static DuplicateEchoModule instance;
    return instance;
  }
};

void* DuplicateEchoModule::GetModule() {
  return static_cast<rex::IModule*>(&GetInstance());
}

void* DuplicateEchoModule::GetEcho() {
  return static_cast<rex::test::IEcho*>(&GetInstance());
}

}  // namespace

REX_DECLARE_MODULE(DuplicateEchoModule)
