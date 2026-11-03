/**
 * @file        tests/object_app.h
 * @brief       An app base for tests that link the module object libraries
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include <rex/module.h>

#ifndef REX_TEST_REAL_MODULES
#include "apu/audio_module.h"
#include "filesystem/filesystem_module.h"
#include "hid/input_module.h"
#include "kernel/rexkernel_module.h"
#include "xam/xam_module.h"
#include "ui/ui_module.h"
#endif

namespace rex::test {

#ifdef REX_TEST_REAL_MODULES

/** The base app, with the modules supplied by the module libraries. */
template <typename Base>
class ObjectApp : public Base {
 public:
  using Base::Base;
};

#else

/** The module instances an ObjectApp registers, built before the app base and destroyed after. */
struct ObjectModules {
  filesystem::FileSystemModule filesystem_module;
  hid::InputModule input_module;
  apu::AudioModule audio_module;
  ui::UIModule ui_module;
  xam::XAMModule xam_module;
  kernel::KernelModule kernel_module;
};

/**
 * Runs an app over the module objects linked into the test executable and loads only rexgpu from
 * its library. Leaves out a module that the module list no longer names when OnModules ends, so an
 * override that removes a name calls ObjectApp::OnModules last.
 */
template <typename Base>
class ObjectApp : private ObjectModules, public Base {
 public:
  using Base::Base;

 protected:
  void OnModules(std::vector<std::string>& modules) override {
    Base::OnModules(modules);
    registered_.clear();
    for (IModule* module :
         {static_cast<IModule*>(&filesystem_module), static_cast<IModule*>(&input_module),
          static_cast<IModule*>(&audio_module), static_cast<IModule*>(&ui_module),
          static_cast<IModule*>(&xam_module), static_cast<IModule*>(&kernel_module)}) {
      if (std::ranges::find(modules, std::string(module->GetName())) != modules.end()) {
        registered_.push_back(module);
      }
    }
    std::erase_if(modules, [](const std::string& name) { return name != "rexgpu"; });
  }

  void OnRegisterModules(std::vector<IModule*>& modules) override {
    Base::OnRegisterModules(modules);
    modules.insert(modules.end(), registered_.begin(), registered_.end());
  }

 private:
  std::vector<IModule*> registered_;
};

#endif

}  // namespace rex::test
