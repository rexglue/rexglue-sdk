/**
 * @file        tests/unit/kernel/object_block_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/kernel/object/object_block.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/module.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>

#include "kernel/kernel_fixture.h"
#include "kernel/title_state.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

constexpr u16 kOrdinalThreadType = 0x001B;
constexpr u16 kOrdinalEventType = 0x000E;
constexpr u16 kOrdinalSemaphoreType = 0x0017;

u32 HeaderType(rex::kernel::GuestAddressSpace* memory, u32 body) {
  return *memory->TranslateVirtual<const be_u32*>(body - 8);
}

u32 ExportedType(Caller& c, u16 ordinal) {
  auto& runtime = c.GetRuntime();
  const auto kernel_module = runtime.GetModuleRegistry().FindKernelModule("xboxkrnl.exe");
  REQUIRE(kernel_module);
  auto module = runtime.GetObjectTable().Get<rex::kernel::ModuleObject>(*kernel_module);
  REQUIRE(module);
  return module->GetVariable(ordinal);
}

}  // namespace

REX_TEST_CASE("An object block keeps its type 8 bytes before the body", "[kernel][object]") {
  KernelFixture fixture;
  auto* memory = StateOf(fixture.GetRuntime()).GetAddressSpace();
  const u32 type_address =
      StateOf(fixture.GetRuntime()).GetObjectTypeAddress(rex::kernel::ObjectType::kEvent);
  REQUIRE(type_address != 0);
  const auto block = rex::kernel::AllocObjectBlock(memory, 0x10, type_address);
  REQUIRE(block.address != 0);
  const u32 header_address = block.address - static_cast<u32>(sizeof(rex::kernel::X_OBJECT_HEADER));
  const auto* header =
      memory->TranslateVirtual<const rex::kernel::X_OBJECT_HEADER*>(header_address);
  const u32 type = HeaderType(memory, block.address);
  CHECK(type == header->object_type_ptr);
  CHECK(type == type_address);
  CHECK(header->pointer_count == 0);
  CHECK(header->handle_count == 0);
  CHECK(header->flags == 0);
  CHECK(header->hash_index == 0);
  rex::kernel::BaseHeap* heap = memory->LookupHeap(type);
  REQUIRE(heap != nullptr);
  rex::kernel::HeapAllocation info{};
  REQUIRE(heap->QueryRegionInfo(type, &info));
  CHECK((info.state & rex::kernel::kMemoryAllocationCommit) != 0);
  CHECK(info.region_size >= sizeof(rex::kernel::X_OBJECT_TYPE));
  rex::kernel::FreeObjectBlock(memory, block.address);
  REQUIRE(heap->QueryRegionInfo(type, &info));
  CHECK((info.state & rex::kernel::kMemoryAllocationCommit) != 0);
}

REX_TEST_CASE("An object block with no exported type points at 0", "[kernel][object]") {
  KernelFixture fixture;
  auto* memory = StateOf(fixture.GetRuntime()).GetAddressSpace();
  const auto block = rex::kernel::AllocObjectBlock(memory, 0x10, 0);
  REQUIRE(block.address != 0);
  CHECK(HeaderType(memory, block.address) == 0);
  rex::kernel::FreeObjectBlock(memory, block.address);
}

REX_TEST_CASE("The header of a new event holds the address of ExEventObjectType",
              "[kernel][object]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* memory = StateOf(c.GetRuntime()).GetAddressSpace();
    const u32 handle_ptr = c.Alloc(4);
    const u32 object_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0);
    const u32 handle = c.Load32(handle_ptr);
    const u32 event_type = ExportedType(c, kOrdinalEventType);
    REQUIRE(event_type != 0);
    REQUIRE(c.Kernel("ObReferenceObjectByHandle", {handle, event_type, object_ptr}) == 0);
    const u32 body = c.Load32(object_ptr);
    CHECK(HeaderType(memory, body) == event_type);
    CHECK(c.Kernel("ObDereferenceObject", {body}) == 0);
    CHECK(c.Kernel("ObReferenceObjectByHandle",
                   {handle, ExportedType(c, kOrdinalSemaphoreType), object_ptr}) ==
          rex::kernel::X_STATUS_OBJECT_TYPE_MISMATCH);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("The header of a new semaphore holds the address of ExSemaphoreObjectType",
              "[kernel][object]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* memory = StateOf(c.GetRuntime()).GetAddressSpace();
    const u32 handle_ptr = c.Alloc(4);
    const u32 object_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateSemaphore", {handle_ptr, 0, 1, 2}) == 0);
    const u32 handle = c.Load32(handle_ptr);
    REQUIRE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, object_ptr}) == 0);
    const u32 body = c.Load32(object_ptr);
    CHECK(HeaderType(memory, body) == ExportedType(c, kOrdinalSemaphoreType));
    CHECK(c.Kernel("ObDereferenceObject", {body}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("The header of a new thread holds the address of ExThreadObjectType",
              "[kernel][object]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* memory = StateOf(c.GetRuntime()).GetAddressSpace();
    const u32 handle = c.Spawn([](Caller&) {});
    const u32 object_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, object_ptr}) == 0);
    const u32 body = c.Load32(object_ptr);
    CHECK(HeaderType(memory, body) == ExportedType(c, kOrdinalThreadType));
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.AllocQuad(-50000000)}) == 0);
    CHECK(c.Kernel("ObDereferenceObject", {body}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}
