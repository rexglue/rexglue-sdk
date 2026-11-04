/**
 * @file        tests/unit/base/module_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string_view>
#include <vector>

#include <rex/kernel/object/object_table.h>
#include <rex/module.h>

namespace rex::test::module_test {

REX_DECLARE_INTERFACE(IExample, 3)
  virtual int GetValue() const = 0;
};

REX_DECLARE_INTERFACE(IOther, 1)};

}  // namespace rex::test::module_test

namespace {

using rex::test::module_test::IExample;
using rex::test::module_test::IOther;

class FakeExample final : public IExample {
 public:
  int GetValue() const override { return 7; }
};

class FakeFactory final : public rex::IInterfaceFactory {
 public:
  rex::IInterface* Find(std::string_view name, rex::u32 version) override {
    if (name == IExample::kInterface.name && version == IExample::kInterface.version) {
      return &example_;
    }
    return nullptr;
  }
  std::vector<rex::IInterface*> FindAll(std::string_view name, rex::u32 version) override {
    rex::IInterface* found = Find(name, version);
    return found ? std::vector<rex::IInterface*>{found} : std::vector<rex::IInterface*>{};
  }

 private:
  FakeExample example_;
};

}  // namespace

static_assert(std::string_view(rex::IModule::kInterface.name) == "rex::IModule");
static_assert(std::string_view(rex::kernel::IObjectTable::kInterface.name) ==
              "rex::kernel::IObjectTable");
static_assert(std::string_view(IExample::kInterface.name) == "rex::test::module_test::IExample");
static_assert(!IExample::kInterface.is_private);

REX_TEST_CASE("the module interface is rex::IModule version 1", "[base][module]") {
  CHECK(std::string_view(rex::IModule::kInterface.name) == "rex::IModule");
  CHECK(rex::IModule::kInterface.version == 1);
}

REX_TEST_CASE("a declared interface carries its qualified type name and version",
              "[base][module]") {
  CHECK(std::string_view(IExample::kInterface.name) == "rex::test::module_test::IExample");
  CHECK(IExample::kInterface.version == 3);
}

REX_TEST_CASE("the factory finds an interface by its qualified type name", "[base][module]") {
  FakeFactory factory;
  CHECK(factory.Find("rex::test::module_test::IExample", 3) != nullptr);
  CHECK(factory.Find("IExample", 3) == nullptr);
}

REX_TEST_CASE("the factory returns the served interface for the exact version", "[base][module]") {
  FakeFactory factory;
  IExample* example = factory.Get<IExample>();
  REQUIRE(example != nullptr);
  CHECK(example->GetValue() == 7);
}

REX_TEST_CASE("the factory returns null for another name", "[base][module]") {
  FakeFactory factory;
  CHECK(factory.Get<IOther>() == nullptr);
}

REX_TEST_CASE("the factory returns null for another version", "[base][module]") {
  FakeFactory factory;
  CHECK(factory.Find(IExample::kInterface.name, IExample::kInterface.version + 1) == nullptr);
  CHECK(factory.Find(IExample::kInterface.name, IExample::kInterface.version - 1) == nullptr);
}
