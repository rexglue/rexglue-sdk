/**
 * @file        tests/unit/base/init_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include <rex/gpu/graphics_system.h>
#include <rex/init.h>
#include <rex/module.h>

#include "base/settings/settings_control.h"
#include "modules/test_service.h"

namespace rex::test::init_test {

REX_DECLARE_INTERFACE(IInitExample, 1)
  virtual int GetValue() const = 0;
};

}  // namespace rex::test::init_test

namespace {

using rex::test::ITestService;
using rex::test::init_test::IInitExample;

using Events = std::vector<std::string>;

class InitModule final : public rex::IModule, public IInitExample {
 public:
  InitModule(std::string name, Events& events, bool serves)
      : name_(std::move(name)), events_(events), serves_(serves) {}

  std::string_view GetName() const override { return name_; }
  std::span<const rex::InterfaceType> GetRequiredInterfaces() const override { return required; }
  rex::Result<void> Connect(rex::IFactory& factory) override {
    events_.push_back("connect " + name_);
    kept = &factory;
    return {};
  }
  void Disconnect() override { events_.push_back("disconnect " + name_); }
  rex::Result<void> Init() override {
    events_.push_back("init " + name_);
    service_at_init = kept ? kept->Get<ITestService>() : nullptr;
    return {};
  }
  void Shutdown() override { events_.push_back("shutdown " + name_); }
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    if (serves_ && name == IInitExample::kInterface.name &&
        version == IInitExample::kInterface.version) {
      return static_cast<IInitExample*>(this);
    }
    return nullptr;
  }
  int GetValue() const override { return 1; }

  std::vector<rex::InterfaceType> required;
  rex::IFactory* kept = nullptr;
  ITestService* service_at_init = nullptr;

 private:
  std::string name_;
  Events& events_;
  bool serves_;
};

}  // namespace

REX_TEST_CASE("rex::Init loads exactly the listed modules in order", "[base][init]") {
  Events events;
  InitModule a("a", events, false);
  InitModule b("b", events, false);
  rex::Config config;
  config.code_modules = {&a, &b};
  REQUIRE(rex::Init(config));
  CHECK(REX_INTERFACE(rex::gpu::IGraphicsSystem) == nullptr);
  rex::Shutdown();
  const Events expected = {"connect a",  "connect b",  "init a",       "init b",
                           "shutdown b", "shutdown a", "disconnect b", "disconnect a"};
  CHECK(events == expected);
}

REX_TEST_CASE("rex::Init refuses a module listed before the module it requires", "[base][init]") {
  Events events;
  InitModule user("user", events, false);
  InitModule server("server", events, true);
  user.required = {IInitExample::kInterface};
  rex::Config config;
  config.code_modules = {&user, &server};
  auto started = rex::Init(config);
  REQUIRE_FALSE(started);
  CHECK(started.error().message.find("'user'") != std::string::npos);
  CHECK(started.error().message.find(IInitExample::kInterface.name) != std::string::npos);
  CHECK(std::ranges::find(events, "init user") == events.end());
  rex::Shutdown();
}

REX_TEST_CASE("rex::Init accepts the same modules listed in the order they require",
              "[base][init]") {
  Events events;
  InitModule user("user", events, false);
  InitModule server("server", events, true);
  user.required = {IInitExample::kInterface};
  rex::Config config;
  config.code_modules = {&server, &user};
  REQUIRE(rex::Init(config));
  rex::Shutdown();
}

REX_TEST_CASE("rex::Shutdown leaves no module loaded", "[base][init]") {
  Events events;
  InitModule a("a", events, true);
  rex::Config config;
  config.code_modules = {&a};
  REQUIRE(rex::Init(config));
  CHECK(REX_INTERFACE(IInitExample) != nullptr);
  rex::Shutdown();
  CHECK(REX_INTERFACE(IInitExample) == nullptr);
  CHECK(events.back() == "disconnect a");
  rex::Shutdown();
  REQUIRE(rex::Init(config));
  rex::Shutdown();
}

REX_TEST_CASE("a program that lists no rexgpu never loads it", "[base][init]") {
  bool graphics_seen = true;
  rex::Config config;
  config.select_plugins = [&](rex::IFactory& factory,
                              rex::IFactory&) -> rex::Result<std::vector<std::string>> {
    graphics_seen = factory.Get<rex::gpu::IGraphicsSystem>() != nullptr;
    return std::vector<std::string>{};
  };
  REQUIRE(rex::Init(config));
  CHECK_FALSE(graphics_seen);
  CHECK(REX_INTERFACE(rex::gpu::IGraphicsSystem) == nullptr);
  rex::Shutdown();
}

REX_TEST_CASE("rex::Init loads the plugins the callback names before any module inits",
              "[base][init]") {
  Events events;
  InitModule a("a", events, false);
  ITestService* at_callback = nullptr;
  rex::Config config;
  config.code_modules = {&a};
  config.select_plugins = [&](rex::IFactory& factory,
                              rex::IFactory&) -> rex::Result<std::vector<std::string>> {
    at_callback = factory.Get<ITestService>();
    events.push_back("callback");
    return std::vector<std::string>{"rexmodule-test-good"};
  };
  REQUIRE(rex::Init(config));
  CHECK(at_callback == nullptr);
  CHECK(a.service_at_init != nullptr);
  CHECK(REX_INTERFACE(ITestService) != nullptr);
  const Events expected = {"connect a", "callback", "init a"};
  CHECK(events == expected);
  rex::Shutdown();
}

REX_TEST_CASE("a callback error stops rex::Init before any module inits", "[base][init]") {
  Events events;
  InitModule a("a", events, false);
  rex::Config config;
  config.code_modules = {&a};
  config.select_plugins = [](rex::IFactory&,
                             rex::IFactory&) -> rex::Result<std::vector<std::string>> {
    return rex::Err<std::vector<std::string>>(rex::ErrorCategory::kConfig, "refused");
  };
  auto started = rex::Init(config);
  REQUIRE_FALSE(started);
  CHECK(started.error().message == "refused");
  const Events expected = {"connect a", "disconnect a"};
  CHECK(events == expected);
  rex::Shutdown();
}

REX_TEST_CASE("API::Get never returns a private interface", "[base][init]") {
  Events events;
  InitModule a("a", events, true);
  rex::Config config;
  config.code_modules = {&a};
  REQUIRE(rex::Init(config));
  CHECK(REX_INTERFACE(rex::ISettingsControl) == nullptr);
  CHECK(REX_INTERFACE(IInitExample) != nullptr);
  rex::Shutdown();
}

REX_TEST_CASE("the private factory of Init finds the interfaces the public one hides",
              "[base][init]") {
  Events events;
  InitModule a("a", events, true);
  rex::Config config;
  config.code_modules = {&a};
  rex::ISettingsControl* public_found = nullptr;
  bool selected = false;
  rex::ISettingsControl* private_found = nullptr;
  config.select_plugins =
      [&](rex::IFactory& factory,
          rex::IFactory& private_factory) -> rex::Result<std::vector<std::string>> {
    selected = true;
    public_found = factory.Get<rex::ISettingsControl>();
    private_found = private_factory.Get<rex::ISettingsControl>();
    return std::vector<std::string>{};
  };
  REQUIRE(rex::Init(config));
  CHECK(selected);
  CHECK(public_found == nullptr);
  CHECK(private_found != nullptr);
  rex::Shutdown();
}

REX_TEST_CASE("rex::Stop leaves API::Get null and the modules disconnected", "[base][init]") {
  Events events;
  InitModule a("a", events, true);
  rex::Config config;
  config.code_modules = {&a};
  rex::Stop();
  CHECK(events.empty());
  REQUIRE(rex::Init(config));
  CHECK(REX_INTERFACE(IInitExample) != nullptr);
  rex::Stop();
  CHECK(REX_INTERFACE(IInitExample) == nullptr);
  const Events stopped = {"connect a", "init a", "shutdown a", "disconnect a"};
  CHECK(events == stopped);
  rex::Stop();
  rex::Shutdown();
  CHECK(events == stopped);
}
