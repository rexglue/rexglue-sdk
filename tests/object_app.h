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
#include <memory>
#include <string>
#include <vector>

#include <rex/module.h>

namespace rex::test {

#ifdef REX_TEST_REAL_MODULES

/** The base app, with the modules supplied by the module libraries. */
template <typename Base>
class ObjectApp : public Base {
 public:
  using Base::Base;
};

#else

/**
 * The module instances an ObjectApp registers, built before the app base and destroyed after.
 * Defined in object_app.cpp, so the module headers stay out of the tests that include this one.
 */
class ObjectModules {
 public:
  ObjectModules();
  ~ObjectModules();

  /** Returns every module instance. */
  std::vector<IModule*> GetAll();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
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
    for (IModule* module : GetAll()) {
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
