/**
 * @file        tests/unit/core/interface_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <type_traits>

#include <rex/apu.h>
#include <rex/cvar/registry.h>
#include <rex/filesystem/file_system.h>
#include <rex/gpu.h>
#include <rex/hid.h>
#include <rex/logging/logger.h>
#include <rex/module.h>
#include <rex/runtime/achievement_manager.h>
#include <rex/runtime/content_manager.h>
#include <rex/runtime/dispatcher.h>
#include <rex/runtime/module_registry.h>
#include <rex/runtime/notifier.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/thread_registry.h>
#include <rex/ui/user_interface.h>

#include "modules/echo.h"

namespace {

REX_INTERFACE(IPlain)
 public:
  virtual int Plain() const = 0;
};

REX_INTERFACE(INamed, "rex.test.named", 7)
 public:
  virtual int Named() const = 0;
};

class Both final : public INamed, public rex::test::IEcho {
 public:
  int Named() const override { return 1; }
  int Echo(int value) override { return value; }
  int GetValue() override { return 2; }
};

template <typename T>
constexpr bool kIsServedInterface =
    std::is_base_of_v<rex::IInterface, T> && std::is_abstract_v<T> &&
    !std::is_copy_constructible_v<T> && std::has_virtual_destructor_v<T> &&
    std::string_view(T::kInterfaceName).starts_with("rex.") && T::kInterfaceVersion == 1;

static_assert(std::is_base_of_v<rex::IInterface, IPlain>);
static_assert(std::is_abstract_v<IPlain>);
static_assert(std::has_virtual_destructor_v<IPlain>);
static_assert(!std::is_copy_constructible_v<IPlain>);
static_assert(std::string_view(INamed::kInterfaceName) == "rex.test.named");
static_assert(INamed::kInterfaceVersion == 7);
static_assert(std::is_base_of_v<rex::Interface<"rex.test.named", 7>, INamed>);

static_assert(kIsServedInterface<rex::IModule>);
static_assert(kIsServedInterface<rex::ILogger>);
static_assert(kIsServedInterface<rex::apu::IAPUBackend>);
static_assert(kIsServedInterface<rex::cvar::ICvarRegistry>);
static_assert(kIsServedInterface<rex::filesystem::IFileSystem>);
static_assert(kIsServedInterface<rex::gpu::IGPUBackend>);
static_assert(kIsServedInterface<rex::hid::IHIDBackend>);
static_assert(kIsServedInterface<rex::runtime::IAchievementManager>);
static_assert(kIsServedInterface<rex::runtime::IContentManager>);
static_assert(kIsServedInterface<rex::runtime::IDispatcher>);
static_assert(kIsServedInterface<rex::runtime::IModuleRegistry>);
static_assert(kIsServedInterface<rex::runtime::INotifier>);
static_assert(kIsServedInterface<rex::runtime::IObjectTable>);
static_assert(kIsServedInterface<rex::runtime::IThreadRegistry>);
static_assert(kIsServedInterface<rex::ui::IUserInterface>);
static_assert(kIsServedInterface<rex::test::IEcho>);

}  // namespace

REX_TEST_CASE("served interface names are unique", "[core][module][interface]") {
  std::array<std::string_view, 16> names = {
      rex::IModule::kInterfaceName,
      rex::ILogger::kInterfaceName,
      rex::apu::IAPUBackend::kInterfaceName,
      rex::cvar::ICvarRegistry::kInterfaceName,
      rex::filesystem::IFileSystem::kInterfaceName,
      rex::gpu::IGPUBackend::kInterfaceName,
      rex::hid::IHIDBackend::kInterfaceName,
      rex::runtime::IAchievementManager::kInterfaceName,
      rex::runtime::IContentManager::kInterfaceName,
      rex::runtime::IDispatcher::kInterfaceName,
      rex::runtime::IModuleRegistry::kInterfaceName,
      rex::runtime::INotifier::kInterfaceName,
      rex::runtime::IObjectTable::kInterfaceName,
      rex::runtime::IThreadRegistry::kInterfaceName,
      rex::ui::IUserInterface::kInterfaceName,
      rex::test::IEcho::kInterfaceName,
  };
  std::ranges::sort(names);
  CHECK(std::ranges::adjacent_find(names) == names.end());
}

REX_TEST_CASE("a class of 2 interfaces converts to each of them", "[core][module][interface]") {
  Both both;
  INamed* named = &both;
  rex::test::IEcho* echo = &both;
  CHECK(named->Named() == 1);
  CHECK(echo->Echo(5) == 5);
  CHECK(echo->GetValue() == 2);
}
