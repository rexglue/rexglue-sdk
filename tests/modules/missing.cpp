/**
 * @file        tests/modules/missing.cpp
 * @brief       Test module that requires an interface nobody serves
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

REXMOD_TEST_SINK()

namespace {

constexpr const char* kName = "rexmod-test-missing";

class MissingModule final : public rex::IModule {
 public:
  static void* GetModule();
  static constexpr std::array<rex::InterfaceEntry, 1> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
  }};

  MissingModule() { Record(kName, "constructor"); }

  const char* Name() const override { return kName; }
  std::span<const char* const> RequiredInterfaces() const override {
    static constexpr const char* kRequired[] = {"rex.test.absent@1"};
    return kRequired;
  }
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

 private:
  static MissingModule& Instance() {
    static MissingModule instance;
    return instance;
  }
};

void* MissingModule::GetModule() {
  return static_cast<rex::IModule*>(&Instance());
}

}  // namespace

REX_DECLARE_MODULE(MissingModule)
