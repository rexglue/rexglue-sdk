/**
 * @file        tests/unit/base/module_served_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/module.h>

#include "apu/audio_module.h"
#include "filesystem/filesystem_module.h"
#include "hid/input_module.h"
#include "kernel/rexkernel_module.h"
#include "ui/ui_module.h"
#include "xam/xam_module.h"

namespace {

void CheckServed(rex::IModule& module) {
  INFO(module.GetName());
  const std::span<const rex::InterfaceType> served = module.GetServedInterfaces();
  CHECK_FALSE(served.empty());
  for (const rex::InterfaceType& type : served) {
    INFO(type.name);
    CHECK(module.QueryInterface(type.name, type.version) != nullptr);
  }
}

}  // namespace

REX_TEST_CASE("a module answers each interface its served list names", "[base][module]") {
  rex::apu::AudioModule audio;
  rex::filesystem::FileSystemModule filesystem;
  rex::hid::InputModule input;
  rex::kernel::KernelModule kernel;
  rex::ui::UIModule ui;
  rex::xam::XAMModule xam;
  CheckServed(audio);
  CheckServed(filesystem);
  CheckServed(input);
  CheckServed(kernel);
  CheckServed(ui);
  CheckServed(xam);
}
