/**
 * @file        tests/modules/dyninit_module.cpp
 * @brief       Module with a dynamic initializer that the init audit refuses
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/module.h>

namespace {

const int kMode = []() noexcept {
  volatile int seed = 2;
  return seed % 2 + 1;
}();

class DynInitModule final : public rex::IModule {
 public:
  std::string_view GetName() const override { return kMode == 1 ? "dyninit" : "dyninit-other"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
};

}  // namespace

REX_DECLARE_MODULE(DynInitModule)
