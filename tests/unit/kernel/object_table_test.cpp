/**
 * Unit tests for the kernel object table
 *
 * Tests handle management, pinning, and name mapping.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <thread>

#include <rex/system/object_data.h>
#include <rex/system/util/object_table.h>

using rex::X_HANDLE;
using rex::X_STATUS;
using rex::system::AddOptions;
using rex::system::EventObject;
using rex::system::ObjectKind;
using rex::system::SemaphoreObject;
using rex::system::util::ObjectTable;

namespace {

X_HANDLE AddEvent(ObjectTable& table, std::string_view name = {}) {
  X_HANDLE handle = 0;
  REQUIRE(table.Emplace<EventObject>(&handle, AddOptions{name, {}}, true, false) ==
          X_STATUS_SUCCESS);
  return handle;
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
  X_HANDLE copy = 0;
  REQUIRE(table.Duplicate(handle, &copy) == X_STATUS_SUCCESS);
  CHECK(copy != handle);
  CHECK(&*table.Get<EventObject>(handle) == &*table.Get<EventObject>(copy));
  CHECK(table.Duplicate(0xF8FFFFF0, &copy) == X_STATUS_INVALID_HANDLE);
}

TEST_CASE("ObjectTable Close removes at zero and reports bad handles", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  CHECK(table.Close(handle) == X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
  CHECK(table.Close(handle) == X_STATUS_INVALID_HANDLE);
  CHECK(table.Close(0) == X_STATUS_INVALID_HANDLE);
  CHECK(table.Close(0xFFFFFFFF) == X_STATUS_INVALID_HANDLE);
}

TEST_CASE("ObjectTable Retain needs a matching Close", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  REQUIRE(table.Retain(handle) == X_STATUS_SUCCESS);
  CHECK(table.Close(handle) == X_STATUS_SUCCESS);
  CHECK(table.Get(handle));
  CHECK(table.Close(handle) == X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
  CHECK(table.Retain(handle) == X_STATUS_INVALID_HANDLE);
}

TEST_CASE("ObjectTable RetainPrimary needs a matching Close", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  {
    auto pin = table.Get(handle);
    REQUIRE(table.RetainPrimary(pin) == X_STATUS_SUCCESS);
  }
  CHECK(table.Close(handle) == X_STATUS_SUCCESS);
  CHECK(table.Get(handle));
  CHECK(table.Close(handle) == X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
}

TEST_CASE("ObjectTable PrimaryHandle returns the first handle", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  X_HANDLE copy = 0;
  REQUIRE(table.Duplicate(handle, &copy) == X_STATUS_SUCCESS);
  auto pin = table.Get(copy);
  CHECK(table.PrimaryHandle(pin) == handle);
}

TEST_CASE("ObjectTable ReleasePrimary closes the first handle", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  {
    auto pin = table.Get(handle);
    CHECK(table.ReleasePrimary(pin) == X_STATUS_SUCCESS);
  }
  CHECK_FALSE(table.Get(handle));
}

TEST_CASE("ObjectTable Remove ignores the count", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  {
    auto pin = table.Get(handle);
    REQUIRE(table.RetainPrimary(pin) == X_STATUS_SUCCESS);
  }
  CHECK(table.Remove(handle) == X_STATUS_SUCCESS);
  CHECK_FALSE(table.Get(handle));
}

TEST_CASE("ObjectTable keeps a pinned object alive across Close", "[kernel][object_table][pin]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table);
  auto pin = table.Get<EventObject>(handle);
  REQUIRE(pin);
  CHECK(table.Close(handle) == X_STATUS_SUCCESS);
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
    REQUIRE(table.Close(handle) == X_STATUS_SUCCESS);
    closed = true;
    holder.join();
  }
}

TEST_CASE("ObjectTable names are case insensitive and collide", "[kernel][object_table][names]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table, "Alpha");
  X_HANDLE found = 0;
  CHECK(table.OpenByName("ALPHA", &found) == X_STATUS_SUCCESS);
  CHECK(found == handle);
  CHECK(table.AddName("alpha", 0xF8000008) == X_STATUS_OBJECT_NAME_COLLISION);
  CHECK(table.OpenByName("Missing", &found) == X_STATUS_OBJECT_NAME_NOT_FOUND);
  CHECK(found == X_INVALID_HANDLE_VALUE);
}

TEST_CASE("ObjectTable OpenByName takes a handle reference", "[kernel][object_table][names]") {
  ObjectTable table(nullptr);
  const X_HANDLE handle = AddEvent(table, "Beta");
  X_HANDLE found = 0;
  REQUIRE(table.OpenByName("Beta", &found) == X_STATUS_SUCCESS);
  CHECK(table.Close(handle) == X_STATUS_SUCCESS);
  CHECK(table.Get(handle));
  CHECK(table.Close(handle) == X_STATUS_SUCCESS);
  CHECK(table.OpenByName("Beta", &found) == X_STATUS_OBJECT_NAME_NOT_FOUND);
}

TEST_CASE("ObjectTable GetAll returns one pin per object of the type", "[kernel][object_table]") {
  ObjectTable table(nullptr);
  const X_HANDLE a = AddEvent(table);
  AddEvent(table);
  X_HANDLE copy = 0;
  REQUIRE(table.Duplicate(a, &copy) == X_STATUS_SUCCESS);
  X_HANDLE semaphore = 0;
  REQUIRE(table.Emplace<SemaphoreObject>(&semaphore, AddOptions{}, 1, 2) == X_STATUS_SUCCESS);
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
