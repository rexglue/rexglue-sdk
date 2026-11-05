/**
 * @file        tests/unit/kernel/object_table_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <memory>
#include <optional>
#include <thread>

#include <rex/kernel/types/status.h>
#include <rex/kernel/object/xam_object.h>
#include <rex/kernel/object/object_table.h>

#include "kernel/object_table.h"

using rex::kernel::AddOptions;
using rex::kernel::EventObject;
using rex::kernel::ObjectTable;
using rex::kernel::ObjectType;
using rex::kernel::SemaphoreObject;
using rex::kernel::ToXStatus;
using rex::kernel::X_HANDLE;
using rex::kernel::X_STATUS;

namespace {

class CountedXAMObject final : public rex::kernel::IXAMObject {
 public:
  static constexpr rex::u32 kTag = 0x434E5431;

  explicit CountedXAMObject(int& destroyed) : destroyed_(destroyed) {}
  ~CountedXAMObject() override { ++destroyed_; }

  [[nodiscard]] rex::u32 GetTag() const override { return kTag; }

 private:
  int& destroyed_;
};

class OtherXAMObject final : public rex::kernel::IXAMObject {
 public:
  static constexpr rex::u32 kTag = 0x4F544852;

  [[nodiscard]] rex::u32 GetTag() const override { return kTag; }
};

rex::kernel::X_HANDLE AddEvent(ObjectTable& table, std::string_view name = {}) {
  auto added = table.Emplace<EventObject>(AddOptions{name, {}}, true, false);
  REQUIRE(added.has_value());
  return *added;
}

}  // namespace

REX_TEST_CASE("ObjectTable first handle is slot 1 not 0", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  CHECK(AddEvent(table) == 0xF8000004);
}

REX_TEST_CASE("ObjectTable gives each object a unique 4 aligned handle", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE a = AddEvent(table);
  const rex::kernel::X_HANDLE b = AddEvent(table);
  CHECK(a != b);
  CHECK((a & 3) == 0);
  CHECK((b & 3) == 0);
  CHECK(b == a + 4);
}

REX_TEST_CASE("ObjectTable Get is type checked", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table);
  CHECK(table.Get<EventObject>(handle));
  CHECK_FALSE(table.Get<SemaphoreObject>(handle));
  CHECK(table.Get(handle).GetType() == ObjectType::kEvent);
}

REX_TEST_CASE("ObjectTable Get checks the tag of an XAM object", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  int destroyed = 0;
  auto added = table.Emplace<CountedXAMObject>(AddOptions{}, destroyed);
  REQUIRE(added.has_value());
  CHECK(table.Get<CountedXAMObject>(*added));
  CHECK_FALSE(table.Get<OtherXAMObject>(*added));
  CHECK_FALSE(table.Get<EventObject>(*added));
  CHECK(table.Get(*added).GetType() == ObjectType::kXAM);
}

REX_TEST_CASE("ObjectTable destroys an XAM object when its last handle closes",
              "[kernel][object_table]") {
  ObjectTable table(nullptr);
  int destroyed = 0;
  auto added = table.Emplace<CountedXAMObject>(AddOptions{}, destroyed);
  REQUIRE(added.has_value());
  CHECK(destroyed == 0);
  REQUIRE(table.Close(*added).has_value());
  CHECK(destroyed == 1);
}

REX_TEST_CASE("ObjectTable Get rejects 0 pseudo and unknown handles", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  AddEvent(table);
  CHECK_FALSE(table.Get(0));
  CHECK_FALSE(table.Get(0xFFFFFFFF));
  CHECK_FALSE(table.Get(0xFFFFFFFE));
  CHECK_FALSE(table.Get(0xF8FFFFF0));
  CHECK_FALSE(table.Get(0x12345678));
}

REX_TEST_CASE("ObjectTable Duplicate gives a new handle to the same object",
              "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table);
  auto duplicated = table.Duplicate(handle);
  REQUIRE(duplicated.has_value());
  const rex::kernel::X_HANDLE copy = *duplicated;
  CHECK(copy != handle);
  CHECK(&*table.Get<EventObject>(handle) == &*table.Get<EventObject>(copy));
  CHECK(ToXStatus(table.Duplicate(0xF8FFFFF0)) == rex::kernel::X_STATUS_INVALID_HANDLE);
}

REX_TEST_CASE("ObjectTable Close removes at zero and reports bad handles",
              "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table);
  CHECK(ToXStatus(table.Close(handle)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
  CHECK(ToXStatus(table.Close(handle)) == rex::kernel::X_STATUS_INVALID_HANDLE);
  CHECK(ToXStatus(table.Close(0)) == rex::kernel::X_STATUS_INVALID_HANDLE);
  CHECK(ToXStatus(table.Close(0xFFFFFFFF)) == rex::kernel::X_STATUS_INVALID_HANDLE);
}

REX_TEST_CASE("ObjectTable Open gives each call its own handle", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table);
  auto first = table.Open(table.Get(handle));
  auto second = table.Open(table.Get(handle));
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(*first != handle);
  CHECK(*second != *first);
  CHECK(ToXStatus(table.Close(*first)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK(ToXStatus(table.Close(*first)) == rex::kernel::X_STATUS_INVALID_HANDLE);
  CHECK(table.Get(handle));
  CHECK(table.Get(*second));
}

REX_TEST_CASE("ObjectTable Reference keeps the object after its last handle closes",
              "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table);
  {
    auto pin = table.Get(handle);
    REQUIRE(ToXStatus(table.Reference(pin)) == rex::kernel::X_STATUS_SUCCESS);
  }
  CHECK(ToXStatus(table.Close(handle)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
  CHECK(ToXStatus(table.Close(handle)) == rex::kernel::X_STATUS_INVALID_HANDLE);
  CHECK(table.GetAll().size() == 1);
}

REX_TEST_CASE("ObjectTable Dereference destroys the object once no handle is left",
              "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table);
  auto pin = table.Get(handle);
  REQUIRE(ToXStatus(table.Reference(pin)) == rex::kernel::X_STATUS_SUCCESS);
  REQUIRE(ToXStatus(table.Close(handle)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK(ToXStatus(table.Dereference(pin)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK(table.GetAll().empty());
  CHECK(ToXStatus(table.Reference(pin)) == rex::kernel::X_STATUS_INVALID_HANDLE);
}

REX_TEST_CASE("ObjectTable Dereference leaves an object that still has a handle",
              "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table);
  auto pin = table.Get(handle);
  REQUIRE(ToXStatus(table.Reference(pin)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK(ToXStatus(table.Dereference(pin)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK(table.Get(handle));
  CHECK(ToXStatus(table.Dereference(pin)) == rex::kernel::X_STATUS_INVALID_PARAMETER);
}

REX_TEST_CASE("ObjectTable GetByAddress finds an object that has no handle of the title",
              "[kernel][object_table]") {
  ObjectTable table(nullptr);
  auto added =
      table.Emplace<EventObject>(AddOptions{{}, rex::kernel::ObjectBlock{0x2000}}, true, false);
  REQUIRE(added.has_value());
  CHECK(table.GetByAddress(0x2000));
  CHECK_FALSE(table.GetByAddress(0x2004));
  {
    auto pin = table.Get(*added);
    REQUIRE(ToXStatus(table.Reference(pin)) == rex::kernel::X_STATUS_SUCCESS);
  }
  REQUIRE(ToXStatus(table.Close(*added)) == rex::kernel::X_STATUS_SUCCESS);
  auto found = table.GetByAddress(0x2000);
  REQUIRE(found);
  CHECK(ToXStatus(table.Dereference(found)) == rex::kernel::X_STATUS_SUCCESS);
  found.Reset();
  CHECK_FALSE(table.GetByAddress(0x2000));
}

REX_TEST_CASE("ObjectTable keeps a pinned object alive across Close",
              "[kernel][object_table][pin]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table);
  auto pin = table.Get<EventObject>(handle);
  REQUIRE(pin);
  CHECK(ToXStatus(table.Close(handle)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
  CHECK(pin->event != nullptr);
}

REX_TEST_CASE("ObjectTable destroys on the last pin when another thread closed first",
              "[kernel][object_table][pin]") {
  for (int round = 0; round < 200; ++round) {
    ObjectTable table(nullptr);
    const rex::kernel::X_HANDLE handle = AddEvent(table);
    std::atomic<bool> pinned{false};
    std::atomic<bool> closed{false};
    std::thread holder([&] {
      auto pin = table.Get<EventObject>(handle);
      pinned = true;
      while (!closed) {
        std::this_thread::yield();
      }
      pin->event->Set();
    });
    while (!pinned) {
      std::this_thread::yield();
    }
    REQUIRE(ToXStatus(table.Close(handle)) == rex::kernel::X_STATUS_SUCCESS);
    closed = true;
    holder.join();
  }
}

REX_TEST_CASE("ObjectTable names are case insensitive and collide",
              "[kernel][object_table][names]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table, "Alpha");
  auto found = table.OpenByName("ALPHA");
  REQUIRE(found.has_value());
  CHECK(*found != handle);
  CHECK(&*table.Get<EventObject>(*found) == &*table.Get<EventObject>(handle));
  CHECK(ToXStatus(table.AddName("alpha", 0xF8000008)) ==
        rex::kernel::X_STATUS_OBJECT_NAME_COLLISION);
  auto missing = table.OpenByName("Missing");
  CHECK(ToXStatus(missing) == rex::kernel::X_STATUS_OBJECT_NAME_NOT_FOUND);
  CHECK(missing.value_or(rex::kernel::kXInvalidHandleValue) == rex::kernel::kXInvalidHandleValue);
}

REX_TEST_CASE("ObjectTable OpenByName gives each open its own handle",
              "[kernel][object_table][names]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table, "Beta");
  auto opened = table.OpenByName("Beta");
  REQUIRE(opened.has_value());
  CHECK(ToXStatus(table.Close(handle)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
  CHECK(table.Get(*opened));
  auto again = table.OpenByName("Beta");
  REQUIRE(again.has_value());
  CHECK(ToXStatus(table.Close(*opened)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK(ToXStatus(table.Close(*again)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK(ToXStatus(table.OpenByName("Beta")) == rex::kernel::X_STATUS_OBJECT_NAME_NOT_FOUND);
}

REX_TEST_CASE("ObjectTable keeps a name while any handle is open",
              "[kernel][object_table][names]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table, "Gamma");
  auto duplicated = table.Duplicate(handle);
  REQUIRE(duplicated.has_value());
  REQUIRE(ToXStatus(table.Close(handle)) == rex::kernel::X_STATUS_SUCCESS);
  auto found = table.OpenByName("Gamma");
  REQUIRE(found.has_value());
  CHECK(ToXStatus(table.AddName("Gamma", 0xF8000010)) ==
        rex::kernel::X_STATUS_OBJECT_NAME_COLLISION);
  REQUIRE(ToXStatus(table.Close(*duplicated)) == rex::kernel::X_STATUS_SUCCESS);
  REQUIRE(ToXStatus(table.Close(*found)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK(ToXStatus(table.OpenByName("Gamma")) == rex::kernel::X_STATUS_OBJECT_NAME_NOT_FOUND);
}

REX_TEST_CASE("ObjectTable frees a name when the last handle closes under a reference",
              "[kernel][object_table][names]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table, "Delta");
  {
    auto pin = table.Get(handle);
    REQUIRE(ToXStatus(table.Reference(pin)) == rex::kernel::X_STATUS_SUCCESS);
  }
  REQUIRE(ToXStatus(table.Close(handle)) == rex::kernel::X_STATUS_SUCCESS);
  CHECK(ToXStatus(table.OpenByName("Delta")) == rex::kernel::X_STATUS_OBJECT_NAME_NOT_FOUND);
}

REX_TEST_CASE("ObjectTable All returns one pin per object of the type", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE a = AddEvent(table);
  AddEvent(table);
  REQUIRE(table.Duplicate(a).has_value());
  REQUIRE(table.Emplace<SemaphoreObject>(AddOptions{}, 1, 2).has_value());
  CHECK(table.All<EventObject>().size() == 2);
  CHECK(table.All<SemaphoreObject>().size() == 1);
  CHECK(table.GetAll().size() == 3);
}

REX_TEST_CASE("ObjectTable Reset closes everything", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const rex::kernel::X_HANDLE handle = AddEvent(table);
  table.Reset();
  CHECK_FALSE(table.Get(handle));
}
