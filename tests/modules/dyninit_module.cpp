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

#include <cstdlib>

#include <rex/module.h>

namespace {

const int g_mode = std::rand() % 2 + 1;

class DynInitModule final : public rex::IModule {
 public:
  std::string_view GetName() const override { return g_mode == 1 ? "dyninit" : "dyninit-other"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
};

}  // namespace

REX_DECLARE_MODULE(DynInitModule)
