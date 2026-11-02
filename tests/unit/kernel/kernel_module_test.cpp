/**
 * @file        tests/unit/kernel/kernel_module_test.cpp
 * @brief       Kernel module dependencies, services and loaded modules
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "object_app.h"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include <rex/app/app.h>
#include <rex/cli.h>
#include <rex/filesystem/file_system_factory.h>
#include <rex/module.h>
#include <rex/ui/ui_system.h>

#include "kernel/kernel_control.h"
#include "kernel/title_fixture.h"

namespace {

class ListApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  explicit ListApp(std::string dropped)
      : ObjectApp("kernel_module_test"), dropped_(std::move(dropped)) {}

  rex::Result<void> Start() { return Init(rex::CLIArgList::Parse(std::span<const std::string>{})); }

 protected:
  void OnModules(std::vector<std::string>& modules) override {
    std::erase(modules, dropped_);
    ObjectApp::OnModules(modules);
  }

 private:
  std::string dropped_;
};

}  // namespace

REX_TEST_CASE("the kernel refuses to start without IUISystem", "[kernel][module]") {
  ListApp app("rexui");
  const auto started = app.Start();
  REQUIRE_FALSE(started);
  CHECK(started.error().message.find(rex::ui::IUISystem::kInterface.name) != std::string::npos);
  CHECK(app.Shutdown() == 1);
}

REX_TEST_CASE("the kernel loads no module of its own", "[kernel][module]") {
  ListApp app("rexfilesystem");
  const auto started = app.Start();
  REQUIRE_FALSE(started);
  CHECK(started.error().message.find(rex::filesystem::IFileSystemFactory::kInterface.name) !=
        std::string::npos);
  CHECK(app.Shutdown() == 1);
}

REX_TEST_CASE("the kernel module serves IKernelControl only through the private factory",
              "[kernel][module]") {
  rex::test::TempFolder folder("rex_kernel_module_");
  rex::test::KernelHost host(folder.path);
  CHECK(host.app.GetPrivateFactory().Get<rex::kernel::IKernelControl>() != nullptr);
  CHECK(REX_INTERFACE(rex::kernel::IKernelControl) == nullptr);
}
