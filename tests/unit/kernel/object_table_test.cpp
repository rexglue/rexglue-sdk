/**
 * Unit tests for the kernel object table
 *
 * Tests handle management, pinning, and name mapping.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <thread>

#include <rex/runtime/error.h>
#include <rex/runtime/object_data.h>

#include "runtime/object_table.h"

using rex::X_HANDLE;
using rex::X_STATUS;
using rex::runtime::AddOptions;
using rex::runtime::EventObject;
using rex::runtime::ObjectKind;
using rex::runtime::ObjectTable;
using rex::runtime::SemaphoreObject;
using rex::runtime::ToXStatus;

namespace {

X_HANDLE AddEvent(ObjectTable& table, std::string_view name = {}) {
  auto added = table.Emplace<EventObject>(AddOptions{name, {}}, true, false);
  REQUIRE(added.has_value());
  return *added;
}

}  // namespace

TEST_CASE("ObjectTable first handle is slot 1 not 0", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  CHECK(AddEvent(table) == 0xF8000004);
}

TEST_CASE("ObjectTable gives each object a unique 4 aligned handle", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE a = AddEvent(table);
  const X_HANDLE b = AddEvent(table);
  CHECK(a != b);
  CHECK((a & 3) == 0);
  CHECK((b & 3) == 0);
  CHECK(b == a + 4);
}

TEST_CASE("ObjectTable Get is type checked", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  CHECK(table.Get<EventObject>(handle));
  CHECK_FALSE(table.Get<SemaphoreObject>(handle));
  CHECK(table.Get(handle).kind() == ObjectKind::Event);
}

TEST_CASE("ObjectTable Get rejects 0 pseudo and unknown handles", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  AddEvent(table);
  CHECK_FALSE(table.Get(0));
  CHECK_FALSE(table.Get(0xFFFFFFFF));
  CHECK_FALSE(table.Get(0xFFFFFFFE));
  CHECK_FALSE(table.Get(0xF8FFFFF0));
  CHECK_FALSE(table.Get(0x12345678));
}

TEST_CASE("ObjectTable Duplicate gives a new handle to the same object", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  auto duplicated = table.Duplicate(handle);
  REQUIRE(duplicated.has_value());
  const X_HANDLE copy = *duplicated;
  CHECK(copy != handle);
  CHECK(&*table.Get<EventObject>(handle) == &*table.Get<EventObject>(copy));
  CHECK(ToXStatus(table.Duplicate(0xF8FFFFF0)) == rex::X_STATUS_INVALID_HANDLE);
}

TEST_CASE("ObjectTable Close removes at zero and reports bad handles", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  CHECK(ToXStatus(table.Close(handle)) == rex::X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
  CHECK(ToXStatus(table.Close(handle)) == rex::X_STATUS_INVALID_HANDLE);
  CHECK(ToXStatus(table.Close(0)) == rex::X_STATUS_INVALID_HANDLE);
  CHECK(ToXStatus(table.Close(0xFFFFFFFF)) == rex::X_STATUS_INVALID_HANDLE);
}

TEST_CASE("ObjectTable Retain needs a matching Close", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  REQUIRE(ToXStatus(table.Retain(handle)) == rex::X_STATUS_SUCCESS);
  CHECK(ToXStatus(table.Close(handle)) == rex::X_STATUS_SUCCESS);
  CHECK(table.Get(handle));
  CHECK(ToXStatus(table.Close(handle)) == rex::X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
  CHECK(ToXStatus(table.Retain(handle)) == rex::X_STATUS_INVALID_HANDLE);
}

TEST_CASE("ObjectTable RetainPrimary needs a matching Close", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  {
    auto pin = table.Get(handle);
    REQUIRE(ToXStatus(table.RetainPrimary(pin)) == rex::X_STATUS_SUCCESS);
  }
  CHECK(ToXStatus(table.Close(handle)) == rex::X_STATUS_SUCCESS);
  CHECK(table.Get(handle));
  CHECK(ToXStatus(table.Close(handle)) == rex::X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
}

TEST_CASE("ObjectTable PrimaryHandle returns the first handle", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  auto duplicated = table.Duplicate(handle);
  REQUIRE(duplicated.has_value());
  const X_HANDLE copy = *duplicated;
  auto pin = table.Get(copy);
  CHECK(table.PrimaryHandle(pin) == std::optional<X_HANDLE>(handle));
}

TEST_CASE("ObjectTable ReleasePrimary closes the first handle", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  {
    auto pin = table.Get(handle);
    CHECK(ToXStatus(table.ReleasePrimary(pin)) == rex::X_STATUS_SUCCESS);
  }
  CHECK_FALSE(table.Get(handle));
}

TEST_CASE("ObjectTable Remove ignores the count", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  {
    auto pin = table.Get(handle);
    REQUIRE(ToXStatus(table.RetainPrimary(pin)) == rex::X_STATUS_SUCCESS);
  }
  CHECK(ToXStatus(table.Remove(handle)) == rex::X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
}

TEST_CASE("ObjectTable keeps a pinned object alive across Close", "[kernel][object_table][pin]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  auto pin = table.Get<EventObject>(handle);
  REQUIRE(pin);
  CHECK(ToXStatus(table.Close(handle)) == rex::X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
  CHECK(pin->event != nullptr);
}

TEST_CASE("ObjectTable destroys on the last pin when another thread closed first",
          "[kernel][object_table][pin]") {
  for (int round = 0; round < 200; ++round) {
    ObjectTable table(nullptr);
    const X_HANDLE handle = AddEvent(table);
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
    REQUIRE(ToXStatus(table.Close(handle)) == rex::X_STATUS_SUCCESS);
    closed = true;
    holder.join();
  }
}

TEST_CASE("ObjectTable names are case insensitive and collide", "[kernel][object_table][names]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table, "Alpha");
  auto found = table.OpenByName("ALPHA");
  REQUIRE(found.has_value());
  CHECK(*found == handle);
  CHECK(ToXStatus(table.AddName("alpha", 0xF8000008)) == rex::X_STATUS_OBJECT_NAME_COLLISION);
  auto missing = table.OpenByName("Missing");
  CHECK(ToXStatus(missing) == rex::X_STATUS_OBJECT_NAME_NOT_FOUND);
  CHECK(missing.value_or(rex::X_INVALID_HANDLE_VALUE) == rex::X_INVALID_HANDLE_VALUE);
}

TEST_CASE("ObjectTable OpenByName takes a handle reference", "[kernel][object_table][names]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table, "Beta");
  REQUIRE(table.OpenByName("Beta").has_value());
  CHECK(ToXStatus(table.Close(handle)) == rex::X_STATUS_SUCCESS);
  CHECK(table.Get(handle));
  CHECK(ToXStatus(table.Close(handle)) == rex::X_STATUS_SUCCESS);
  CHECK(ToXStatus(table.OpenByName("Beta")) == rex::X_STATUS_OBJECT_NAME_NOT_FOUND);
}

TEST_CASE("ObjectTable keeps a name while any handle is open", "[kernel][object_table][names]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table, "Gamma");
  auto duplicated = table.Duplicate(handle);
  REQUIRE(duplicated.has_value());
  REQUIRE(ToXStatus(table.Close(handle)) == rex::X_STATUS_SUCCESS);
  auto found = table.OpenByName("Gamma");
  REQUIRE(found.has_value());
  CHECK(*found == *duplicated);
  CHECK(ToXStatus(table.AddName("Gamma", 0xF8000010)) == rex::X_STATUS_OBJECT_NAME_COLLISION);
  REQUIRE(ToXStatus(table.Close(*duplicated)) == rex::X_STATUS_SUCCESS);
  REQUIRE(ToXStatus(table.Close(*duplicated)) == rex::X_STATUS_SUCCESS);
  CHECK(ToXStatus(table.OpenByName("Gamma")) == rex::X_STATUS_OBJECT_NAME_NOT_FOUND);
}

TEST_CASE("ObjectTable GetAll returns one pin per object of the type", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE a = AddEvent(table);
  AddEvent(table);
  REQUIRE(table.Duplicate(a).has_value());
  REQUIRE(table.Emplace<SemaphoreObject>(AddOptions{}, 1, 2).has_value());
  CHECK(table.GetAll<EventObject>().size() == 2);
  CHECK(table.GetAll<SemaphoreObject>().size() == 1);
  CHECK(table.GetAll().size() == 3);
}

TEST_CASE("ObjectTable Reset closes everything", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  table.Reset();
  CHECK_FALSE(table.Get(handle));
}
