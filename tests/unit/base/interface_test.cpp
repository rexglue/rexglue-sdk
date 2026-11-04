/**
 * @file        tests/unit/base/interface_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>
#include <type_traits>

#include <rex/filesystem/file_system.h>
#include <rex/utility.h>
#include <rex/xam/achievement_manager.h>
#include <rex/xam/content_manager.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/module.h>
#include <rex/kernel/object/notification_broadcaster.h>
#include <rex/kernel/object/object_table.h>

namespace {

class IPlain : public rex::NonCopyable {
 public:
  virtual ~IPlain() = default;

  virtual int Plain() const = 0;
};

template <typename T>
constexpr bool kIsInterface = std::is_base_of_v<rex::NonCopyable, T> && std::is_abstract_v<T> &&
                              !std::is_copy_constructible_v<T> && std::has_virtual_destructor_v<T>;

static_assert(kIsInterface<IPlain>);
static_assert(kIsInterface<rex::filesystem::IFileSystem>);
static_assert(kIsInterface<rex::xam::IAchievementManager>);
static_assert(kIsInterface<rex::xam::IContentManager>);
static_assert(kIsInterface<rex::kernel::IDPCQueue>);
static_assert(kIsInterface<rex::kernel::IOverlappedDispatcher>);
static_assert(kIsInterface<rex::kernel::IModuleRegistry>);
static_assert(kIsInterface<rex::kernel::INotificationBroadcaster>);
static_assert(kIsInterface<rex::kernel::IObjectTable>);
static_assert(kIsInterface<rex::kernel::IThreadRegistry>);

class FakePlain final : public IPlain {
 public:
  int Plain() const override { return 3; }
};

}  // namespace

REX_TEST_CASE("an interface is reached through its base and destroyed through it",
              "[base][interface]") {
  std::unique_ptr<IPlain> plain = std::make_unique<FakePlain>();
  CHECK(plain->Plain() == 3);
}
