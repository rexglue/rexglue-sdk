/**
 * @file        tests/modules/good.cpp
 * @brief       Well formed test module serving the module and echo interfaces
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
#include <rex/cvar.h>
#include <rex/module.h>

#include "echo.h"
#include "test_module_sink.h"

REX_TEST_MODULE_SINK()

namespace {

constexpr const char* kName = "rexmod-test-good";

REXCVAR_INT32(echo, value, 0, "Value the good module echoes");
REXCVAR_BOOL(echo, ready, false, "Second row of the table");
REXCVAR_TABLE(kEchoCvars, value, ready);

class GoodModule final : public rex::IModule, public rex::test::IEcho {
 public:
  static void* GetModule();
  static void* GetEcho();
  static constexpr std::array<rex::InterfaceEntry, 2> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
      {rex::test::IEcho::kInterfaceName, rex::test::IEcho::kInterfaceVersion, &GetEcho},
  }};

  GoodModule() { Record(kName, "constructor"); }

  const char* GetName() const override { return kName; }
  std::span<const char* const> GetRequiredInterfaces() const override {
    static constexpr std::array<const char*, 1> kRequired = {"rex.cvar@1"};
    return kRequired;
  }
  [[nodiscard]] rex::Result<void> Connect(rex::IInterfaceFactory* factory,
                                          const rex::Config&) override {
    Record(kName, "connect");
    REX_TRY(cvars_, factory->Get<rex::cvar::ICvarRegistry>());
    return cvars_->Register("echo", kEchoCvars);
  }
  void Disconnect() override {
    Record(kName, "disconnect");
    if (cvars_)
      cvars_->Unregister(kEchoCvars);
    cvars_ = nullptr;
  }
  [[nodiscard]] rex::Result<void> Init() override {
    Record(kName, "init");
    return rex::Ok();
  }
  void Shutdown() override { Record(kName, "shutdown"); }
  int Echo(int input) override { return input; }
  int Value() override { return value.Get(); }

 private:
  rex::cvar::ICvarRegistry* cvars_ = nullptr;
  static GoodModule& GetInstance() {
    static GoodModule instance;
    return instance;
  }
};

void* GoodModule::GetModule() {
  return static_cast<rex::IModule*>(&GetInstance());
}

void* GoodModule::GetEcho() {
  return static_cast<rex::test::IEcho*>(&GetInstance());
}

}  // namespace

REX_DECLARE_MODULE(GoodModule)
