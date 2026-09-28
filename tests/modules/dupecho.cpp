/**
 * @file        tests/modules/dupecho.cpp
 * @brief       Test module that also serves rex.test.echo@1
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
#include "test_module_common.h"

REXMOD_TEST_SINK()

namespace {

constexpr const char* kName = "rexmod-test-dupecho";

class DupEchoModule final : public rex::IModule, public rex::test::IEcho {
 public:
  static void* GetModule();
  static void* GetEcho();
  static constexpr std::array<rex::InterfaceEntry, 2> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
      {rex::test::IEcho::kInterfaceName, rex::test::IEcho::kInterfaceVersion, &GetEcho},
  }};

  DupEchoModule() { Record(kName, "constructor"); }

  const char* Name() const override { return kName; }
  std::span<const char* const> RequiredInterfaces() const override { return {}; }
  rex::Status Connect(rex::IInterfaceFactory*, const rex::Config&) override {
    Record(kName, "connect");
    return rex::Ok();
  }
  void Disconnect() override { Record(kName, "disconnect"); }
  rex::Status Init() override {
    Record(kName, "init");
    return rex::Ok();
  }
  void Shutdown() override { Record(kName, "shutdown"); }
  int Echo(int value) override { return value * 2; }

 private:
  static DupEchoModule& Instance() {
    static DupEchoModule instance;
    return instance;
  }
};

void* DupEchoModule::GetModule() {
  return static_cast<rex::IModule*>(&Instance());
}

void* DupEchoModule::GetEcho() {
  return static_cast<rex::test::IEcho*>(&Instance());
}

}  // namespace

REX_DECLARE_MODULE(DupEchoModule)
