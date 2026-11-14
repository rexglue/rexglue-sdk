/**
 * @file        tests/modules/versioned_module.cpp
 * @brief       Module that reports another SDK build, no build, or other interface versions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "test_service.h"

#ifndef REX_TEST_BUILD
#define REX_TEST_BUILD REXGLUE_BUILD_IDENTITY
#endif

namespace {

using rex::test::ITestService;

class VersionedModule final : public rex::IModule, public ITestService {
 public:
  REX_MODULE_SERVES(ITestService)

#ifdef REX_TEST_REQUIRES_VERSION
  std::span<const rex::InterfaceType> GetRequiredInterfaces() const override {
    static constexpr rex::InterfaceType kNeeded[] = {
        {ITestService::kInterface.name, REX_TEST_REQUIRES_VERSION}};
    return kNeeded;
  }
#endif
  std::string_view GetName() const override { return "versioned"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<ITestService>(*this, name, version);
  }
  int GetValue() const override { return 7; }
};

VersionedModule& Instance() {
  static VersionedModule instance;
  return instance;
}

}  // namespace

extern "C" {

#ifndef REX_TEST_NO_BUILD
REX_DETAIL_EXPORT const char* ReXBuildIdentity() {
  return REX_TEST_BUILD;
}
#endif

REX_DETAIL_EXPORT rex::IInterface* ReXCreateInterface(const char* name, rex::u32 version) {
  return rex::detail::CreateModuleInterface<VersionedModule>(Instance(), name, version);
}

REX_DETAIL_EXPORT rex::u32 ReXEnumerateInterfaces(rex::InterfaceType* out, rex::u32 capacity) {
  const rex::u32 count = rex::detail::EnumerateModuleInterfaces(Instance(), out, capacity);
#ifdef REX_TEST_MODULE_VERSION
  for (rex::u32 i = 0; out && i < count && i < capacity; ++i) {
    if (std::string_view(out[i].name) == rex::IModule::kInterface.name) {
      out[i].version = REX_TEST_MODULE_VERSION;
    }
  }
#endif
  return count;
}

}  // extern "C"
