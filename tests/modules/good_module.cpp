/**
 * @file        tests/modules/good_module.cpp
 * @brief       Module with no dynamic initializer that serves ITestService
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "test_service.h"

namespace {

using rex::test::ITestService;

class GoodModule final : public rex::IModule, public ITestService {
 public:
  REX_MODULE_SERVES(ITestService)

  std::string_view GetName() const override { return "good"; }
  rex::Result<void> Connect(rex::IFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<ITestService>(*this, name, version);
  }
  int GetValue() const override { return 42; }
};

}  // namespace

REX_DECLARE_MODULE(GoodModule)
