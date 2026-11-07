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

#include <rex/kernel/module.h>
#include <rex/kernel/object/notification_broadcaster.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/module.h>

#include "apu/audio_module.h"
#include "filesystem/filesystem_module.h"
#include "hid/input_module.h"
#include "kernel/rexkernel_module.h"
#include "kernel/kernel_fixture.h"
#include "ui/ui_module.h"
#include "xam/xam_module.h"

namespace {

/** The interfaces the kernel module serves for a title, which answer only while one is serving. */
bool IsPerTitle(const rex::InterfaceType& type) {
  static const rex::InterfaceType kPerTitle[] = {
      rex::kernel::IObjectTable::kInterface, rex::kernel::IThreadRegistry::kInterface,
      rex::kernel::IModuleRegistry::kInterface, rex::kernel::INotificationBroadcaster::kInterface,
      rex::kernel::IOverlappedDispatcher::kInterface};
  for (const rex::InterfaceType& per_title : kPerTitle) {
    if (per_title.version == type.version && std::string_view(per_title.name) == type.name) {
      return true;
    }
  }
  return false;
}

void CheckServed(rex::IModule& module, bool title_serving) {
  INFO(module.GetName());
  const std::span<const rex::InterfaceType> served = module.GetServedInterfaces();
  CHECK_FALSE(served.empty());
  for (const rex::InterfaceType& type : served) {
    INFO(type.name);
    const bool answers = title_serving || !IsPerTitle(type);
    CHECK((module.QueryInterface(type.name, type.version) != nullptr) == answers);
  }
}

}  // namespace

REX_TEST_CASE("a module answers each interface its served list names outside a title",
              "[base][module]") {
  rex::apu::AudioModule audio;
  rex::filesystem::FileSystemModule filesystem;
  rex::hid::InputModule input;
  rex::kernel::KernelModule kernel;
  rex::ui::UIModule ui;
  rex::xam::XAMModule xam;
  CheckServed(audio, false);
  CheckServed(filesystem, false);
  CheckServed(input, false);
  CheckServed(kernel, false);
  CheckServed(ui, false);
  CheckServed(xam, false);
}

REX_TEST_CASE("the kernel module answers each interface its served list names while a title serves",
              "[base][module]") {
  rex::test::KernelFixture fixture;
  rex::kernel::KernelModule kernel;
  CheckServed(kernel, true);
}
