/**
 * @file        tests/unit/base/module_group_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <span>
#include <string>
#include <vector>

#include <rex/logging/logger.h>
#include <rex/module.h>
#include <rex/platform/dynlib.h>
#include <rex/platform/file.h>
#include <rex/settings.h>

#include "base/library_name.h"
#include "base/module_group.h"
#include "modules/test_service.h"

namespace rex::test::module_group_test {

REX_DECLARE_INTERFACE(IExample, 3)
  virtual int GetValue() const = 0;
};

REX_DECLARE_PRIVATE_INTERFACE(IHidden, 1)
  virtual int GetValue() const = 0;
};

}  // namespace rex::test::module_group_test

namespace {

using rex::test::ITestService;
using rex::test::module_group_test::IExample;
using rex::test::module_group_test::IHidden;

static_assert(std::string_view(IHidden::kInterface.name) ==
              "rex::test::module_group_test::IHidden");
static_assert(IHidden::kInterface.is_private);

class FakeHidden final : public IHidden {
 public:
  int GetValue() const override { return 9; }
};

class FakeModule final : public rex::IModule, public IExample {
 public:
  FakeModule(std::string name, std::vector<std::string>& events, int value = 0)
      : name_(std::move(name)), events_(events), value_(value) {}

  std::string_view GetName() const override { return name_; }
  std::span<const rex::InterfaceType> GetRequiredInterfaces() const override { return required; }
  rex::Result<void> Connect(rex::IFactory& factory) override {
    events_.push_back("connect " + name_);
    seen = factory.Get<IExample>();
    kept = &factory;
    if (fail_connect) {
      return rex::Err(rex::ErrorCategory::kModule, "refused connect");
    }
    return {};
  }
  void Disconnect() override { events_.push_back("disconnect " + name_); }
  rex::Result<void> Init() override {
    events_.push_back("init " + name_);
    seen_at_init = kept ? kept->Get<IExample>() : nullptr;
    if (fail_init) {
      return rex::Err(rex::ErrorCategory::kModule, "refused init");
    }
    return {};
  }
  void Shutdown() override { events_.push_back("shutdown " + name_); }
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    if (serves && name == IExample::kInterface.name && version == IExample::kInterface.version) {
      return static_cast<IExample*>(this);
    }
    return nullptr;
  }
  int GetValue() const override { return value_; }

  bool fail_connect = false;
  bool fail_init = false;
  bool serves = false;
  std::vector<rex::InterfaceType> required;
  IExample* seen = nullptr;
  IExample* seen_at_init = nullptr;
  rex::IFactory* kept = nullptr;

 private:
  std::string name_;
  std::vector<std::string>& events_;
  int value_;
};

class BaseServicesModule final : public rex::IModule {
 public:
  std::string_view GetName() const override { return "base_services"; }
  std::span<const rex::InterfaceType> GetRequiredInterfaces() const override {
    static constexpr rex::InterfaceType kRequired[] = {rex::ISettings::kInterface,
                                                       rex::ILogger::kInterface};
    return kRequired;
  }
  rex::Result<void> Connect(rex::IFactory& factory) override {
    settings = factory.Get<rex::ISettings>();
    logger = factory.Get<rex::ILogger>();
    return {};
  }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view, rex::u32) override { return nullptr; }

  rex::ISettings* settings = nullptr;
  rex::ILogger* logger = nullptr;
};

using Events = std::vector<std::string>;

}  // namespace

REX_TEST_CASE("connect and init run in list order and the reverse undoes them", "[base][module]") {
  Events events;
  FakeModule a("a", events);
  FakeModule b("b", events);
  FakeModule c("c", events);
  {
    rex::ModuleGroup group;
    REQUIRE(group.Add(a));
    REQUIRE(group.Add(b));
    REQUIRE(group.Add(c));
    REQUIRE(group.Connect());
    REQUIRE(group.Init());
    group.Shutdown();
    group.Disconnect();
  }
  const Events expected = {"connect a",  "connect b",    "connect c",    "init a",
                           "init b",     "init c",       "shutdown c",   "shutdown b",
                           "shutdown a", "disconnect c", "disconnect b", "disconnect a"};
  CHECK(events == expected);
}

REX_TEST_CASE("a failing connect unwinds the earlier modules only", "[base][module]") {
  Events events;
  FakeModule a("a", events);
  FakeModule b("b", events);
  FakeModule c("c", events);
  b.fail_connect = true;
  rex::ModuleGroup group;
  REQUIRE(group.Add(a));
  REQUIRE(group.Add(b));
  REQUIRE(group.Add(c));
  auto connected = group.Connect();
  REQUIRE_FALSE(connected);
  CHECK(connected.error().message.find("'b'") != std::string::npos);
  const Events expected = {"connect a", "connect b", "disconnect a"};
  CHECK(events == expected);
}

REX_TEST_CASE("a failing init shuts down the earlier modules and disconnects all",
              "[base][module]") {
  Events events;
  FakeModule a("a", events);
  FakeModule b("b", events);
  FakeModule c("c", events);
  b.fail_init = true;
  rex::ModuleGroup group;
  REQUIRE(group.Add(a));
  REQUIRE(group.Add(b));
  REQUIRE(group.Add(c));
  REQUIRE(group.Connect());
  auto started = group.Init();
  REQUIRE_FALSE(started);
  CHECK(started.error().message.find("'b'") != std::string::npos);
  const Events expected = {"connect a",  "connect b",    "connect c",    "init a",      "init b",
                           "shutdown a", "disconnect c", "disconnect b", "disconnect a"};
  CHECK(events == expected);
}

REX_TEST_CASE("a missing required interface fails connect naming it", "[base][module]") {
  Events events;
  FakeModule user("user", events);
  user.required.push_back(IExample::kInterface);
  rex::ModuleGroup group;
  REQUIRE(group.Add(user));
  auto connected = group.Connect();
  REQUIRE_FALSE(connected);
  CHECK(connected.error().message.find(IExample::kInterface.name) != std::string::npos);
  CHECK(connected.error().message.find("version 3") != std::string::npos);
  CHECK(events.empty());
}

REX_TEST_CASE("a required interface served by an earlier module connects", "[base][module]") {
  Events events;
  FakeModule provider("provider", events, 9);
  provider.serves = true;
  FakeModule user("user", events);
  user.required.push_back(IExample::kInterface);
  rex::ModuleGroup group;
  REQUIRE(group.Add(provider));
  REQUIRE(group.Add(user));
  REQUIRE(group.Connect());
  REQUIRE(user.seen != nullptr);
  CHECK(user.seen->GetValue() == 9);
}

REX_TEST_CASE("a required interface served only by a later module fails connect",
              "[base][module]") {
  Events events;
  FakeModule user("user", events);
  user.required.push_back(IExample::kInterface);
  FakeModule provider("provider", events);
  provider.serves = true;
  rex::ModuleGroup group;
  REQUIRE(group.Add(user));
  REQUIRE(group.Add(provider));
  CHECK_FALSE(group.Connect());
}

REX_TEST_CASE("find matches the version exactly", "[base][module]") {
  Events events;
  FakeModule provider("provider", events, 5);
  provider.serves = true;
  rex::ModuleGroup group;
  REQUIRE(group.Add(provider));
  CHECK(group.Find(IExample::kInterface.name, IExample::kInterface.version) != nullptr);
  CHECK(group.Find(IExample::kInterface.name, IExample::kInterface.version + 1) == nullptr);
  CHECK(group.Find("rex::test::module_group_test::IMissing", IExample::kInterface.version) ==
        nullptr);
}

REX_TEST_CASE("host served interfaces win over module interfaces", "[base][module]") {
  Events events;
  FakeModule host("host", events, 1);
  FakeModule provider("provider", events, 2);
  provider.serves = true;
  rex::ModuleGroup group;
  group.Serve(static_cast<IExample&>(host), IExample::kInterface);
  REQUIRE(group.Add(provider));
  auto* found = group.Get<IExample>();
  REQUIRE(found != nullptr);
  CHECK(found->GetValue() == 1);
}

REX_TEST_CASE("find all lists the serving modules in list order", "[base][module]") {
  Events events;
  FakeModule first("first", events, 1);
  first.serves = true;
  FakeModule skipped("skipped", events, 2);
  FakeModule last("last", events, 3);
  last.serves = true;
  rex::ModuleGroup group;
  REQUIRE(group.Add(first));
  REQUIRE(group.Add(skipped));
  REQUIRE(group.Add(last));
  const auto all = group.FindAll<IExample>();
  REQUIRE(all.size() == 2);
  CHECK(all[0]->GetValue() == 1);
  CHECK(all[1]->GetValue() == 3);
}

REX_TEST_CASE("a module added after connect and init is connected and inited", "[base][module]") {
  Events events;
  FakeModule a("a", events);
  FakeModule late("late", events);
  rex::ModuleGroup group;
  REQUIRE(group.Add(a));
  REQUIRE(group.Connect());
  REQUIRE(group.Init());
  REQUIRE(group.Add(late));
  const Events expected = {"connect a", "init a", "connect late", "init late"};
  CHECK(events == expected);
}

REX_TEST_CASE("a late module that fails to connect is removed", "[base][module]") {
  Events events;
  FakeModule a("a", events);
  FakeModule late("late", events);
  late.fail_connect = true;
  rex::ModuleGroup group;
  REQUIRE(group.Add(a));
  REQUIRE(group.Connect());
  CHECK_FALSE(group.Add(late));
  group.Disconnect();
  const Events expected = {"connect a", "connect late", "disconnect a"};
  CHECK(events == expected);
}

REX_TEST_CASE("the destructor unwinds once", "[base][module]") {
  Events events;
  FakeModule a("a", events);
  FakeModule b("b", events);
  {
    rex::ModuleGroup group;
    REQUIRE(group.Add(a));
    REQUIRE(group.Add(b));
    REQUIRE(group.Connect());
    REQUIRE(group.Init());
    group.Shutdown();
  }
  const Events expected = {"connect a",  "connect b",  "init a",       "init b",
                           "shutdown b", "shutdown a", "disconnect b", "disconnect a"};
  CHECK(events == expected);
}

REX_TEST_CASE("load serves the interfaces of a module by name", "[base][module]") {
  rex::ModuleGroup group;
  REQUIRE(group.Load("rexmodule-test-good"));
  REQUIRE(group.Connect());
  REQUIRE(group.Init());
  auto* service = group.Get<ITestService>();
  REQUIRE(service != nullptr);
  CHECK(service->GetValue() == 42);
  CHECK(group.FindAll<ITestService>().size() == 1);
}

REX_TEST_CASE("load keeps a module loaded twice once", "[base][module]") {
  rex::ModuleGroup group;
  REQUIRE(group.Load("rexmodule-test-good"));
  REQUIRE(group.Load("rexmodule-test-good"));
  CHECK(group.FindAll<ITestService>().size() == 1);
}

REX_TEST_CASE("load refuses another build identity before calling the module", "[base][module]") {
  const auto file = rex::platform::GetExecutableFolder() /
                    rex::detail::ModuleFileName("rexmodule-test-otheridentity");
  rex::platform::DynamicLibrary probe;
  REQUIRE(probe.Load(file, rex::platform::SymbolResolution::kImmediate));
  auto* create_calls = probe.GetSymbol<int*>("ReXTestCreateCalls");
  auto* enumerate_calls = probe.GetSymbol<int*>("ReXTestEnumerateCalls");
  REQUIRE(create_calls != nullptr);
  REQUIRE(enumerate_calls != nullptr);
  rex::ModuleGroup group;
  auto loaded = group.Load("rexmodule-test-otheridentity");
  REQUIRE_FALSE(loaded);
  CHECK(loaded.error().message.find("rexmodule-test-otheridentity") != std::string::npos);
  CHECK(loaded.error().message.find("another release") != std::string::npos);
  CHECK(*create_calls == 0);
  CHECK(*enumerate_calls == 0);
}

REX_TEST_CASE("load compares the identity exactly", "[base][module]") {
  rex::ModuleGroup other("another-release");
  CHECK_FALSE(other.Load("rexmodule-test-good"));
  rex::ModuleGroup matching("0.0.0-000000000000");
  auto loaded = matching.Load("rexmodule-test-otheridentity");
  REQUIRE_FALSE(loaded);
  CHECK(loaded.error().message.find("does not serve") != std::string::npos);
}

REX_TEST_CASE("load refuses a module that exports no identity", "[base][module]") {
  rex::ModuleGroup group;
  auto loaded = group.Load("rexmod-test-noidentity");
  REQUIRE_FALSE(loaded);
  CHECK(loaded.error().message.find(rex::kBuildIdentitySymbol) != std::string::npos);
}

REX_TEST_CASE("load reports a module that is not there", "[base][module]") {
  rex::ModuleGroup group;
  auto loaded = group.Load("rexmodule-test-missing");
  REQUIRE_FALSE(loaded);
  CHECK(loaded.error().message.find("rexmodule-test-missing") != std::string::npos);
}

REX_TEST_CASE("a batch connects all in order, then inits all, and Init finds later modules",
              "[base][module]") {
  Events events;
  FakeModule a("a", events);
  FakeModule b("b", events, 7);
  b.serves = true;
  rex::ModuleGroup group;
  REQUIRE(group.Connect());
  REQUIRE(group.Init());
  rex::IModule* const batch[] = {&a, &b};
  REQUIRE(group.Add(batch));
  const Events expected = {"connect a", "connect b", "init a", "init b"};
  CHECK(events == expected);
  CHECK(a.seen == nullptr);
  REQUIRE(a.seen_at_init != nullptr);
  CHECK(a.seen_at_init->GetValue() == 7);
}

REX_TEST_CASE("a failing batch shuts down and removes the whole batch", "[base][module]") {
  Events events;
  FakeModule keep("keep", events);
  FakeModule a("a", events);
  FakeModule b("b", events);
  b.fail_init = true;
  rex::ModuleGroup group;
  REQUIRE(group.Add(keep));
  REQUIRE(group.Connect());
  REQUIRE(group.Init());
  events.clear();
  const size_t mark = group.Mark();
  rex::IModule* const batch[] = {&a, &b};
  auto added = group.Add(batch);
  REQUIRE_FALSE(added);
  CHECK(added.error().message.find("'b'") != std::string::npos);
  CHECK(group.Mark() == mark);
  const Events expected = {"connect a",  "connect b",    "init a",      "init b",
                           "shutdown a", "disconnect b", "disconnect a"};
  CHECK(events == expected);
}

REX_TEST_CASE("ShutdownAfter stops only the modules added after the mark", "[base][module]") {
  Events events;
  FakeModule a("a", events);
  FakeModule b("b", events);
  FakeModule c("c", events);
  rex::ModuleGroup group;
  REQUIRE(group.Connect());
  REQUIRE(group.Init());
  REQUIRE(group.Add(a));
  const size_t mark = group.Mark();
  REQUIRE(group.Add(b));
  REQUIRE(group.Add(c));
  events.clear();
  group.ShutdownAfter(mark);
  const Events after = {"shutdown c", "shutdown b"};
  CHECK(events == after);
  events.clear();
  group.Shutdown();
  const Events rest = {"shutdown a"};
  CHECK(events == rest);
}

REX_TEST_CASE("a module finds ISettings and ILogger at Connect", "[base][module]") {
  BaseServicesModule module;
  rex::ModuleGroup group;
  REQUIRE(group.Add(module));
  REQUIRE(group.Connect());
  CHECK(module.settings == static_cast<rex::ISettings*>(&REX_SETTINGS()));
  CHECK(module.logger == static_cast<rex::ILogger*>(&REX_LOGGER()));
  group.Disconnect();
}

class HiddenModule final : public rex::IModule, public IHidden {
 public:
  REX_MODULE_SERVES(IHidden)

  std::string_view GetName() const override { return "hidden"; }
  rex::Result<void> Connect(rex::IFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<IHidden>(*this, name, version);
  }
  int GetValue() const override { return 9; }
};

REX_TEST_CASE("a code module's private interface is not found by the public factory",
              "[base][module]") {
  HiddenModule module;
  rex::ModuleGroup group;
  REQUIRE(group.Add(module));
  REQUIRE(group.Connect());
  CHECK(group.Get<IHidden>() == nullptr);
  CHECK(group.FindAll(IHidden::kInterface.name, IHidden::kInterface.version).empty());
  IHidden* found = group.GetPrivateFactory().Get<IHidden>();
  REQUIRE(found != nullptr);
  CHECK(found->GetValue() == 9);
  group.Disconnect();
}

REX_TEST_CASE("the private factory finds a private interface a module serves", "[base][module]") {
  FakeHidden hidden;
  rex::ModuleGroup group;
  group.Serve(hidden, IHidden::kInterface);
  CHECK(group.Get<IHidden>() == nullptr);
  CHECK(group.FindAll(IHidden::kInterface.name, IHidden::kInterface.version).empty());
  IHidden* found = group.GetPrivateFactory().Get<IHidden>();
  REQUIRE(found != nullptr);
  CHECK(found->GetValue() == 9);
}
