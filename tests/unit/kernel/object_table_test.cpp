/**
 * Unit tests for kernel object system (XObject and ObjectTable)
 *
 * Tests handle management, reference counting, and name mapping.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/logging.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xobject.h>

using namespace rex;
using namespace rex::system;
using namespace rex::system::util;

// =============================================================================
// Test Fixtures and Helpers
// =============================================================================

// Minimal XObject subclass for testing
class TestObject : public XObject {
 public:
  static constexpr Type kObjectType = Type::Undefined;

  TestObject() : XObject(Type::Undefined) {}

  // Track destructor calls for leak detection
  static int destructor_count;
  ~TestObject() override { destructor_count++; }
};

int TestObject::destructor_count = 0;

// Reset destructor count before each test
struct DestructorCountReset {
  DestructorCountReset() { TestObject::destructor_count = 0; }
};

// Initialize logging once
void InitTestLogging() {
  static bool initialized = false;
  if (!initialized) {
    rex::InitLogging();
    initialized = true;
  }
}

// =============================================================================
// object_ref Smart Pointer Tests
// =============================================================================

TEST_CASE("object_ref default construction is null", "[kernel][object_ref]") {
  InitTestLogging();
  object_ref<TestObject> ref;

  CHECK(ref.get() == nullptr);
  CHECK(!ref);
  CHECK(ref == nullptr);
}

TEST_CASE("object_ref construction from raw pointer", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();
  // Object starts with ref count of 1 from constructor

  {
    object_ref<TestObject> ref(obj);
    CHECK(ref.get() == obj);
    CHECK(ref);
    CHECK(ref != nullptr);
  }
  // Destructor should release, destroying object
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref copy construction retains", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();

  {
    object_ref<TestObject> ref1(obj);
    {
      object_ref<TestObject> ref2(ref1);  // Copy - should retain
      CHECK(ref1.get() == ref2.get());
      CHECK(TestObject::destructor_count == 0);  // Still alive
    }
    // ref2 destroyed, but ref1 still holds
    CHECK(TestObject::destructor_count == 0);
  }
  // Both refs destroyed
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref move construction transfers ownership", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();

  {
    object_ref<TestObject> ref1(obj);
    object_ref<TestObject> ref2(std::move(ref1));

    CHECK(ref1.get() == nullptr);  // Moved from
    CHECK(ref2.get() == obj);
    CHECK(TestObject::destructor_count == 0);
  }
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref copy assignment retains", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();

  {
    object_ref<TestObject> ref1(obj);
    object_ref<TestObject> ref2;

    ref2 = ref1;  // Copy assign
    CHECK(ref1.get() == ref2.get());
    CHECK(TestObject::destructor_count == 0);
  }
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref move assignment transfers ownership", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();

  {
    object_ref<TestObject> ref1(obj);
    object_ref<TestObject> ref2;

    ref2 = std::move(ref1);
    CHECK(ref1.get() == nullptr);
    CHECK(ref2.get() == obj);
  }
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref reset releases reference", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();

  object_ref<TestObject> ref(obj);
  CHECK(TestObject::destructor_count == 0);

  ref.reset();
  CHECK(ref.get() == nullptr);
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref release returns pointer without destroying", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();

  object_ref<TestObject> ref(obj);
  auto* released = ref.release();

  CHECK(released == obj);
  CHECK(ref.get() == nullptr);
  CHECK(TestObject::destructor_count == 0);  // Not destroyed

  // Clean up manually
  released->Release();
  CHECK(TestObject::destructor_count == 1);
}

TEST_CASE("object_ref arrow operator works", "[kernel][object_ref]") {
  InitTestLogging();

  auto* obj = new TestObject();
  object_ref<TestObject> ref(obj);

  CHECK(ref->type() == XObject::Type::Undefined);
}

TEST_CASE("object_ref dereference operator works", "[kernel][object_ref]") {
  InitTestLogging();

  auto* obj = new TestObject();
  object_ref<TestObject> ref(obj);

  TestObject& deref = *ref;
  CHECK(&deref == obj);
}

TEST_CASE("retain_object helper retains and wraps", "[kernel][object_ref]") {
  InitTestLogging();
  DestructorCountReset reset;

  auto* obj = new TestObject();
  // obj has ref count 1

  {
    auto ref = retain_object(obj);  // Should retain, ref count now 2
    CHECK(ref.get() == obj);
  }
  // ref destroyed, ref count back to 1
  CHECK(TestObject::destructor_count == 0);

  obj->Release();  // Final release
  CHECK(TestObject::destructor_count == 1);
}

// =============================================================================
// ObjectTable Handle Allocation Tests
// =============================================================================

TEST_CASE("ObjectTable AddHandle allocates valid handle", "[kernel][object_table]") {
  InitTestLogging();
  DestructorCountReset reset;

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  X_STATUS status = table.AddHandle(obj, &handle);

  CHECK(status == X_STATUS_SUCCESS);
  CHECK(handle != 0);
  CHECK(handle >= XObject::kHandleBase);  // 0xF8000000

  // Handle should be in object's handle list
  CHECK(obj->handles().size() == 1);
  CHECK(obj->handles()[0] == handle);

  table.Reset();
}

TEST_CASE("ObjectTable first handle is slot 1 not 0", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);

  // First handle should be slot 1: kHandleBase + (1 << 2) = 0xF8000004
  CHECK(handle == XObject::kHandleBase + 4);

  table.Reset();
}

TEST_CASE("ObjectTable multiple objects get unique handles", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj1 = new TestObject();
  auto* obj2 = new TestObject();
  auto* obj3 = new TestObject();

  X_HANDLE h1 = 0, h2 = 0, h3 = 0;
  table.AddHandle(obj1, &h1);
  table.AddHandle(obj2, &h2);
  table.AddHandle(obj3, &h3);

  CHECK(h1 != h2);
  CHECK(h2 != h3);
  CHECK(h1 != h3);

  // Handles increment by 4 (slot << 2)
  CHECK(h2 == h1 + 4);
  CHECK(h3 == h2 + 4);

  table.Reset();
}

// =============================================================================
// ObjectTable Handle Lookup Tests
// =============================================================================

TEST_CASE("ObjectTable LookupObject finds object by handle", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);

  auto found = table.LookupObject<XObject>(handle);
  CHECK(found.get() == obj);

  table.Reset();
}

TEST_CASE("ObjectTable LookupObject returns null for invalid handle", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;

  auto found = table.LookupObject<XObject>(0xF8001234);
  CHECK(found.get() == nullptr);
}

TEST_CASE("ObjectTable LookupObject returns null for handle 0", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;

  auto found = table.LookupObject<XObject>(0);
  CHECK(found.get() == nullptr);
}

// =============================================================================
// ObjectTable Handle Reference Counting Tests
// =============================================================================

TEST_CASE("ObjectTable RetainHandle increments ref count", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);

  X_STATUS status = table.RetainHandle(handle);
  CHECK(status == X_STATUS_SUCCESS);

  // Need two releases to remove
  table.ReleaseHandle(handle);
  auto still_there = table.LookupObject<XObject>(handle);
  CHECK(still_there.get() == obj);

  table.ReleaseHandle(handle);  // This should remove it

  table.Reset();
}

TEST_CASE("ObjectTable ReleaseHandle removes at zero refs", "[kernel][object_table]") {
  InitTestLogging();
  DestructorCountReset reset;

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);

  X_STATUS status = table.ReleaseHandle(handle);
  CHECK(status == X_STATUS_SUCCESS);

  // Handle should be gone
  auto found = table.LookupObject<XObject>(handle);
  CHECK(found.get() == nullptr);

  table.Reset();
}

TEST_CASE("ObjectTable ReleaseHandle on invalid handle returns error", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;

  X_STATUS status = table.ReleaseHandle(0xF8001234);
  CHECK(status == X_STATUS_INVALID_HANDLE);
}

TEST_CASE("ObjectTable DuplicateHandle creates new handle for same object",
          "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE h1 = 0, h2 = 0;
  table.AddHandle(obj, &h1);
  table.DuplicateHandle(h1, &h2);

  CHECK(h1 != h2);

  // Both handles refer to same object
  auto found1 = table.LookupObject<XObject>(h1);
  auto found2 = table.LookupObject<XObject>(h2);
  CHECK(found1.get() == obj);
  CHECK(found2.get() == obj);

  // Object should have both handles
  CHECK(obj->handles().size() == 2);

  table.Reset();
}

// =============================================================================
// ObjectTable Handle Release Tests
// =============================================================================

TEST_CASE("ObjectTable ReleaseHandle removes handle from table", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);

  // ReleaseHandle decrements ref count; at 0 it removes the handle
  X_STATUS status = table.ReleaseHandle(handle);
  CHECK(status == X_STATUS_SUCCESS);

  auto found = table.LookupObject<XObject>(handle);
  CHECK(found.get() == nullptr);

  table.Reset();
}

TEST_CASE("ObjectTable ReleaseHandle updates object's handle list", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();
  obj->Retain();  // Keep alive after table releases

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);
  CHECK(obj->handles().size() == 1);

  table.ReleaseHandle(handle);
  CHECK(obj->handles().empty());

  obj->Release();
  table.Reset();
}

// =============================================================================
// ObjectTable Name Mapping Tests
// =============================================================================

TEST_CASE("ObjectTable AddNameMapping registers name", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);

  X_STATUS status = table.AddNameMapping("TestName", handle);
  CHECK(status == X_STATUS_SUCCESS);

  table.Reset();
}

TEST_CASE("ObjectTable duplicate name returns collision", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj1 = new TestObject();
  auto* obj2 = new TestObject();

  X_HANDLE h1 = 0, h2 = 0;
  table.AddHandle(obj1, &h1);
  table.AddHandle(obj2, &h2);

  CHECK(table.AddNameMapping("SharedName", h1) == X_STATUS_SUCCESS);
  CHECK(table.AddNameMapping("SharedName", h2) == X_STATUS_OBJECT_NAME_COLLISION);

  table.Reset();
}

TEST_CASE("ObjectTable name collision is case-insensitive", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj1 = new TestObject();
  auto* obj2 = new TestObject();

  X_HANDLE h1 = 0, h2 = 0;
  table.AddHandle(obj1, &h1);
  table.AddHandle(obj2, &h2);

  CHECK(table.AddNameMapping("TestName", h1) == X_STATUS_SUCCESS);
  // Different case should still collide
  CHECK(table.AddNameMapping("testname", h2) == X_STATUS_OBJECT_NAME_COLLISION);
  CHECK(table.AddNameMapping("TESTNAME", h2) == X_STATUS_OBJECT_NAME_COLLISION);

  table.Reset();
}

TEST_CASE("ObjectTable GetObjectByName returns not found for missing name",
          "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;

  X_HANDLE found = 0;
  X_STATUS status = table.GetObjectByName("DoesNotExist", &found);
  CHECK(status == X_STATUS_OBJECT_NAME_NOT_FOUND);
  CHECK(found == X_INVALID_HANDLE_VALUE);
}

TEST_CASE("ObjectTable RemoveNameMapping clears name", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj = new TestObject();

  X_HANDLE handle = 0;
  table.AddHandle(obj, &handle);
  table.AddNameMapping("TempName", handle);

  table.RemoveNameMapping("TempName");

  X_HANDLE found = 0;
  CHECK(table.GetObjectByName("TempName", &found) == X_STATUS_OBJECT_NAME_NOT_FOUND);

  table.Reset();
}

// =============================================================================
// ObjectTable Reset and Bulk Operations
// =============================================================================

TEST_CASE("ObjectTable Reset releases all objects", "[kernel][object_table]") {
  InitTestLogging();
  DestructorCountReset reset;

  ObjectTable table;
  auto* obj1 = new TestObject();
  auto* obj2 = new TestObject();
  auto* obj3 = new TestObject();

  table.AddHandle(obj1, nullptr);
  table.AddHandle(obj2, nullptr);
  table.AddHandle(obj3, nullptr);

  // Objects have ref count 2: 1 from new, 1 from AddHandle's Retain
  // Release our initial ref so table owns them exclusively
  obj1->Release();
  obj2->Release();
  obj3->Release();

  CHECK(TestObject::destructor_count == 0);

  table.Reset();

  // Now Reset's Release brings ref count from 1 to 0, destroying them
  CHECK(TestObject::destructor_count == 3);
}

TEST_CASE("ObjectTable GetAllObjects returns all objects", "[kernel][object_table]") {
  InitTestLogging();

  ObjectTable table;
  auto* obj1 = new TestObject();
  auto* obj2 = new TestObject();

  table.AddHandle(obj1, nullptr);
  table.AddHandle(obj2, nullptr);

  auto all = table.GetAllObjects();
  CHECK(all.size() == 2);

  table.Reset();
}

// =============================================================================
// TODO: Tests requiring kernel integration
// =============================================================================

// TODO: Test handle 0xFFFFFFFF (CurrentProcess) - requires full KernelState
// TODO: Test handle 0xFFFFFFFE (CurrentThread) - requires XThread integration
// TODO: Test GetObjectsByType<T>() with real typed objects
// TODO: Test GetObjectByName with existing object - requires kernel_state for RetainHandle
// TODO: Test case-insensitive name lookup via GetObjectByName
