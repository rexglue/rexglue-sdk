/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <bit>
#include <cstring>

#include <rex/assert.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/types/thread.h>
#include <rex/logging/log_category.h>
#include "kernel/categories.h"
#include <rex/math.h>
#include <rex/bit.h>
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/heap.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/types/memory.h>
#include "kernel/argument_check.h"
#include "kernel/guest_stack.h"
#include "kernel/title_state.h"
#include "kernel/kernel.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::kernel;

namespace {

constexpr u32 kMaxMemoryRegionType = 2;
constexpr u32 kInvalidAllocationTypeMask = 0x9F67CFFF;
constexpr u32 kInvalidProtectionMask = 0xFFFFCE98;
constexpr u32 kUserProtectionBits = kXPageGuard | 0x1000 | 0x2000;
constexpr u32 kBaseProtectionBits =
    kXPageNoAccess | kXPageReadOnly | kXPageReadWrite | kXPageExecuteRead | kXPageExecuteReadWrite;
constexpr u32 kAllocationOperations = kXMemCommit | kXMemReserve | kXMemReset;

bool IsValidMemoryRegionType(u32 region_type) {
  return region_type <= kMaxMemoryRegionType;
}

bool IsValidAllocationType(u32 alloc_type) {
  if (alloc_type & kInvalidAllocationTypeMask) {
    return false;
  }
  if (!(alloc_type & kAllocationOperations)) {
    return false;
  }
  return !(alloc_type & kXMemReset) || !(alloc_type & ~u32{kXMemReset});
}

bool IsValidProtection(u32 protect) {
  if (protect & kInvalidProtectionMask) {
    return false;
  }
  if ((protect & kXPageNoAccess) && (protect & kUserProtectionBits)) {
    return false;
  }
  return std::popcount(protect & kBaseProtectionBits) == 1;
}

bool IsValidFreeType(u32 free_type) {
  return free_type == kXMemDecommit || free_type == kXMemRelease;
}

}  // namespace

u32 ToXdkProtectFlags(u32 protect) {
  u32 result = 0;
  if (!(protect & kernel::kMemoryProtectRead) && !(protect & kernel::kMemoryProtectWrite)) {
    result = kXPageNoAccess;
  } else if ((protect & kernel::kMemoryProtectRead) && !(protect & kernel::kMemoryProtectWrite)) {
    result = kXPageReadOnly;
  } else {
    result = kXPageReadWrite;
  }
  if (protect & kernel::kMemoryProtectNoCache) {
    result |= kXPageNoCache;
  }
  if (protect & kernel::kMemoryProtectWriteCombine) {
    result |= kXPageWriteCombine;
  }
  return result;
}

u32 FromXdkProtectFlags(u32 protect) {
  u32 result = 0;
  if ((protect & kXPageReadOnly) | (protect & kXPageExecuteRead)) {
    result = kernel::kMemoryProtectRead;
  } else if ((protect & kXPageReadWrite) | (protect & kXPageExecuteReadWrite)) {
    result = kernel::kMemoryProtectRead | kernel::kMemoryProtectWrite;
  }
  if (protect & kXPageNoCache) {
    result |= kernel::kMemoryProtectNoCache;
  }
  if (protect & kXPageWriteCombine) {
    result |= kernel::kMemoryProtectWriteCombine;
  }
  return result;
}

/**
 * Reserves, commits or resets virtual memory. Region types 0 to 2 all use the guest heap.
 *
 * @param base_addr_ptr The base address, or 0 to choose one. Receives the allocated base.
 * @param region_size_ptr The size in bytes. Receives the rounded size.
 * @param alloc_type The X_MEM_ allocation flags.
 * @param protect_bits The X_PAGE_ protection flags.
 * @param region_type Range 0 to 2.
 * @return The status.
 */
u32 NtAllocateVirtualMemory_entry(MappedU32 base_addr_ptr, MappedU32 region_size_ptr,
                                  u32 alloc_type, u32 protect_bits, u32 region_type) {
  u32 input_base = base_addr_ptr ? static_cast<u32>(*base_addr_ptr) : 0;
  u32 input_size = region_size_ptr ? static_cast<u32>(*region_size_ptr) : 0;
  REXKRNL_TRACE(
      "NtAllocateVirtualMemory "
      "base={:#x} size={:#x} type={:#x} protect={:#x} region={}",
      input_base, input_size, static_cast<u32>(alloc_type), static_cast<u32>(protect_bits),
      static_cast<u32>(region_type));

  if (!IsValidMemoryRegionType(region_type)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  REX_ASSERT(base_addr_ptr != nullptr);
  REX_ASSERT(region_size_ptr != nullptr);

  if (!base_addr_ptr || !region_size_ptr || !*region_size_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (!IsValidAllocationType(alloc_type)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (!IsValidProtection(protect_bits)) {
    return X_STATUS_INVALID_PAGE_PROTECTION;
  }

  u32 page_size;
  if (*base_addr_ptr != 0) {
    // The specified page size is ignored when a base address is given.
    auto heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeap(*base_addr_ptr);
    if (!heap || heap->GetHeapType() != kernel::HeapType::kGuestVirtual) {
      return X_STATUS_INVALID_PARAMETER;
    }
    page_size = heap->GetPageSize();
  } else {
    page_size = kernel::kPageSize4KiB;
    if (alloc_type & kXMemLargePages) {
      page_size = kernel::kPageSize64KiB;
    }
  }

  u32 adjusted_base = *base_addr_ptr - (*base_addr_ptr % page_size);
  // Titles can pass negative sizes.
  u32 adjusted_size = static_cast<i32>(*region_size_ptr) < 0
                          ? -static_cast<i32>(region_size_ptr.GetValue())
                          : region_size_ptr.GetValue();
  // Without a base address the allocation granularity is 64 KB, otherwise the heap page size.
  adjusted_size =
      rex::RoundUpNonZero(adjusted_size, adjusted_base ? page_size : kernel::kPageSize64KiB);

  if (alloc_type & kXMemReset) {
    *base_addr_ptr = adjusted_base;
    *region_size_ptr = adjusted_size;
    return X_STATUS_SUCCESS;
  }

  u32 allocation_type = 0;
  if (alloc_type & kXMemReserve) {
    allocation_type |= kernel::kMemoryAllocationReserve;
  }
  if (alloc_type & kXMemCommit) {
    allocation_type |= kernel::kMemoryAllocationCommit | kernel::kMemoryAllocationTitle;
  }
  u32 protect = FromXdkProtectFlags(protect_bits);
  u32 address = 0;
  kernel::BaseHeap* heap;
  kernel::HeapAllocation prev_alloc_info = {};
  bool was_committed = false;

  if (adjusted_base != 0) {
    heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeap(adjusted_base);
    REX_KERNEL_REQUIRE_NONNULL(heap);
    if (heap->GetPageSize() != page_size) {
      return X_STATUS_ACCESS_DENIED;
    }
    was_committed = heap->QueryRegionInfo(adjusted_base, &prev_alloc_info) &&
                    (prev_alloc_info.state & kernel::kMemoryAllocationCommit) != 0;

    if (heap->AllocFixed(adjusted_base, adjusted_size, page_size, allocation_type, protect)) {
      address = adjusted_base;
    }
  } else {
    bool top_down = !!(alloc_type & kXMemTopDown);
    heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeapByType(false, page_size);
    heap->Alloc(adjusted_size, page_size, allocation_type, protect, top_down, &address);
  }
  if (!address) {
    return X_STATUS_NO_MEMORY;
  }

  if (address && !(alloc_type & kXMemNoZero)) {
    if (alloc_type & kXMemCommit) {
      if (!(protect & kernel::kMemoryProtectWrite)) {
        heap->Protect(address, adjusted_size,
                      kernel::kMemoryProtectRead | kernel::kMemoryProtectWrite);
      }
      if (!was_committed) {
        kernel::GetCurrentState().GetAddressSpace()->Zero(address, adjusted_size);
      }
      if (!(protect & kernel::kMemoryProtectWrite)) {
        heap->Protect(address, adjusted_size, protect);
      }
    }
  }
  *base_addr_ptr = address;
  *region_size_ptr = adjusted_size;
  REXKRNL_TRACE(
      "NtAllocateVirtualMemory -> "
      "0x0 addr={:#x} size={:#x}",
      address, adjusted_size);
  return X_STATUS_SUCCESS;
}

u32 NtProtectVirtualMemory_entry(MappedU32 base_addr_ptr, MappedU32 region_size_ptr,
                                 u32 protect_bits, MappedU32 old_protect, u32 region_type) {
  if (!IsValidMemoryRegionType(region_type)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (!base_addr_ptr || !region_size_ptr || !*region_size_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (!IsValidProtection(protect_bits)) {
    return X_STATUS_INVALID_PAGE_PROTECTION;
  }

  auto heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeap(*base_addr_ptr);
  if (!heap || heap->GetHeapType() != kernel::HeapType::kGuestVirtual) {
    return X_STATUS_INVALID_PARAMETER;
  }
  u32 adjusted_base = *base_addr_ptr - (*base_addr_ptr % heap->GetPageSize());
  u32 adjusted_size = rex::RoundUpNonZero(*region_size_ptr, heap->GetPageSize());
  u32 protect = FromXdkProtectFlags(protect_bits);

  u32 tmp_old_protect = 0;

  if (!heap->Protect(adjusted_base, adjusted_size, protect, &tmp_old_protect)) {
    return X_STATUS_ACCESS_DENIED;
  }

  *base_addr_ptr = adjusted_base;
  *region_size_ptr = adjusted_size;

  if (old_protect) {
    *old_protect = tmp_old_protect;
  }

  return X_STATUS_SUCCESS;
}

u32 NtFreeVirtualMemory_entry(MappedU32 base_addr_ptr, MappedU32 region_size_ptr, u32 free_type,
                              u32 region_type) {
  if (!IsValidMemoryRegionType(region_type) || !IsValidFreeType(free_type)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  u32 base_addr_value = *base_addr_ptr;
  u32 region_size_value = *region_size_ptr;
  REXKRNL_TRACE(
      "NtFreeVirtualMemory "
      "base={:#x} size={:#x} type={:#x} region={}",
      base_addr_value, region_size_value, static_cast<u32>(free_type),
      static_cast<u32>(region_type));

  if (!base_addr_value) {
    return X_STATUS_MEMORY_NOT_ALLOCATED;
  }

  auto heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeap(base_addr_value);
  if (!heap) {
    return X_STATUS_MEMORY_NOT_ALLOCATED;
  }
  if (heap->GetHeapType() != kernel::HeapType::kGuestVirtual) {
    return X_STATUS_INVALID_PARAMETER;
  }
  bool result = false;
  if (free_type == kXMemDecommit) {
    const u32 page_size = heap->GetPageSize();
    if (region_size_value == 0) {
      return X_STATUS_SUCCESS;
    }
    const u64 end = rex::RoundUpNonZero(u64{base_addr_value} + region_size_value, page_size);
    base_addr_value -= base_addr_value % page_size;
    region_size_value = static_cast<u32>(end - base_addr_value);
    result = heap->Decommit(base_addr_value, region_size_value);
  } else {
    if (region_size_value != 0) {
      u32 freed_base = 0;
      u32 freed_size = 0;
      result = heap->ReleaseRange(base_addr_value, region_size_value, &freed_base, &freed_size);
      base_addr_value = freed_base;
      region_size_value = freed_size;
    } else {
      result = heap->Release(base_addr_value, &region_size_value);
    }
  }
  if (!result) {
    return X_STATUS_UNSUCCESSFUL;
  }

  *base_addr_ptr = base_addr_value;
  *region_size_ptr = region_size_value;
  REXKRNL_TRACE(
      "NtFreeVirtualMemory -> "
      "0x0");
  return X_STATUS_SUCCESS;
}

struct X_MEMORY_BASIC_INFORMATION {
  be_u32 base_address;
  be_u32 allocation_base;
  be_u32 allocation_protect;
  be_u32 region_size;
  be_u32 state;
  be_u32 protect;
  be_u32 type;
};

u32 NtQueryVirtualMemory_entry(u32 base_address,
                               GuestPtrT<X_MEMORY_BASIC_INFORMATION> memory_basic_information_ptr,
                               u32 region_type) {
  switch (region_type) {
    case 0:
    case 1:
    case 2:
      break;
    default:
      return X_STATUS_INVALID_PARAMETER;
  }

  auto heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeap(base_address);
  kernel::HeapAllocation alloc_info;
  if (heap == nullptr || !heap->QueryRegionInfo(base_address, &alloc_info)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  memory_basic_information_ptr->base_address = alloc_info.base_address;
  memory_basic_information_ptr->allocation_base = alloc_info.allocation_base;
  memory_basic_information_ptr->allocation_protect =
      ToXdkProtectFlags(alloc_info.allocation_protect);
  memory_basic_information_ptr->region_size = alloc_info.region_size;
  u32 x_state;
  if (alloc_info.state & kernel::kMemoryAllocationCommit) {
    REX_ASSERT((alloc_info.state & kernel::kMemoryAllocationReserve) != 0);
    x_state = kXMemCommit;
  } else if (alloc_info.state & kernel::kMemoryAllocationReserve) {
    x_state = kXMemReserve;
  } else {
    x_state = kXMemFree;
  }
  memory_basic_information_ptr->state = x_state;
  memory_basic_information_ptr->protect = ToXdkProtectFlags(alloc_info.protect);
  memory_basic_information_ptr->type = kXMemPrivate;

  return X_STATUS_SUCCESS;
}

/**
 * Allocates contiguous physical memory. kXMemLargePages selects 64 KB pages and kXMem16MBPages
 * selects 16 MB pages, otherwise pages are 4 KB.
 *
 * @param flags Unused.
 * @param region_size The size in bytes, rounded up to the page size.
 * @param protect_bits The X_PAGE_ protection and X_MEM_ page size flags.
 * @param min_addr_range The lowest acceptable physical address.
 * @param max_addr_range The highest acceptable physical address.
 * @param alignment The alignment, rounded up to the page size.
 * @return The base address, or 0 when no memory is available.
 */
u32 MmAllocatePhysicalMemoryEx_entry(u32 flags, u32 region_size, u32 protect_bits,
                                     u32 min_addr_range, u32 max_addr_range, u32 alignment) {
  REXKRNL_TRACE(
      "MmAllocatePhysicalMemoryEx "
      "flags={:#x} size={:#x} protect={:#x} min={:#x} max={:#x} align={:#x}",
      static_cast<u32>(flags), static_cast<u32>(region_size), static_cast<u32>(protect_bits),
      static_cast<u32>(min_addr_range), static_cast<u32>(max_addr_range),
      static_cast<u32>(alignment));

  if (!(protect_bits & (kXPageReadOnly | kXPageReadWrite))) {
    REXKRNL_FAIL(0, "MmAllocatePhysicalMemoryEx: bad protection bits");
  }

  u32 page_size = kernel::kPageSize4KiB;
  if (protect_bits & kXMemLargePages) {
    page_size = kernel::kPageSize64KiB;
  } else if (protect_bits & kXMem16MBPages) {
    page_size = kernel::kPageSize16MiB;
  }

  const u32 base_address = kernel::GetCurrentState().GetAddressSpace()->AllocPhysical(
      region_size, page_size, FromXdkProtectFlags(protect_bits), min_addr_range, max_addr_range,
      alignment);
  if (!base_address) {
    return 0;
  }
  REXKRNL_TRACE(
      "MmAllocatePhysicalMemoryEx -> "
      "addr={:#x}",
      base_address);

  return base_address;
}

u32 MmAllocatePhysicalMemory_entry(u32 flags, u32 region_size, u32 protect_bits) {
  return MmAllocatePhysicalMemoryEx_entry(flags, region_size, protect_bits, 0, 0xFFFFFFFFu, 0);
}

void MmFreePhysicalMemory_entry(u32 type, u32 base_address) {
  REXKRNL_TRACE(
      "MmFreePhysicalMemory "
      "type={:#x} addr={:#x}",
      static_cast<u32>(type), static_cast<u32>(base_address));

  REX_ASSERT((base_address & 0x1F) == 0);

  auto heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeap(base_address);
  if (heap) {
    heap->Release(base_address);
  }
}

u32 MmQueryAddressProtect_entry(u32 base_address) {
  auto heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeap(base_address);
  u32 access;
  if (!heap || !heap->QueryProtect(base_address, &access)) {
    access = 0;
  }
  access = !access ? 0 : ToXdkProtectFlags(access);

  return access;
}

void MmSetAddressProtect_entry(MappedVoid base_address, u32 region_size, u32 protect_bits) {
  constexpr u32 kRequiredProtectBits = kXPageNoAccess | kXPageReadOnly | kXPageReadWrite |
                                       kXPageExecuteRead | kXPageExecuteReadWrite;

  if (rex::BitCount(protect_bits & kRequiredProtectBits) != 1) {
    REX_ASSERT(!(rex::BitCount(static_cast<u32>(protect_bits) & kRequiredProtectBits) > 1));
    return;
  }

  u32 protect = FromXdkProtectFlags(protect_bits);
  auto heap =
      kernel::GetCurrentState().GetAddressSpace()->LookupHeap(base_address.GetGuestAddress());
  if (!heap) {
    return;
  }
  if (heap->GetHeapType() == kernel::HeapType::kGuestXEX) {
    return;
  }

  heap->Protect(base_address.GetGuestAddress(), region_size, protect);
}

u32 MmQueryAllocationSize_entry(MappedVoid base_address) {
  auto heap =
      kernel::GetCurrentState().GetAddressSpace()->LookupHeap(base_address.GetGuestAddress());
  u32 size;
  if (!heap || !heap->QuerySize(base_address.GetGuestAddress(), &size)) {
    size = 0;
  }

  return size;
}

struct X_MM_QUERY_STATISTICS_SECTION {
  be_u32 available_pages;
  be_u32 total_virtual_memory_bytes;
  be_u32 reserved_virtual_memory_bytes;
  be_u32 physical_pages;
  be_u32 pool_pages;
  be_u32 stack_pages;
  be_u32 image_pages;
  be_u32 heap_pages;
  be_u32 virtual_pages;
  be_u32 page_table_pages;
  be_u32 cache_pages;
};

struct X_MM_QUERY_STATISTICS_RESULT {
  be_u32 size;
  be_u32 total_physical_pages;
  be_u32 kernel_pages;
  X_MM_QUERY_STATISTICS_SECTION title;
  X_MM_QUERY_STATISTICS_SECTION system;
  be_u32 highest_physical_page;
};
REX_STATIC_ASSERT_SIZE(X_MM_QUERY_STATISTICS_RESULT, 104);

u32 MmQueryStatistics_entry(GuestPtrT<X_MM_QUERY_STATISTICS_RESULT> stats_ptr) {
  REX_KERNEL_REQUIRE_NONNULL(stats_ptr);

  const u32 size = sizeof(X_MM_QUERY_STATISTICS_RESULT);

  const u32 requested_size = stats_ptr->size;
  if (requested_size < size) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }

  stats_ptr.Zero();

  stats_ptr->size = requested_size;

  // 512 MB of 4 KB pages.
  stats_ptr->total_physical_pages = 0x00020000;
  stats_ptr->kernel_pages = 0x00002000;

  u32 reserved_pages = 0;
  u32 unreserved_pages = 0;
  u32 used_pages = 0;
  u32 reserved_pages_bytes = 0;
  const kernel::BaseHeap* physical_heaps[3] = {
      kernel::GetCurrentState().GetAddressSpace()->LookupHeapByType(true, 0x1000),
      kernel::GetCurrentState().GetAddressSpace()->LookupHeapByType(true, 0x10000),
      kernel::GetCurrentState().GetAddressSpace()->LookupHeapByType(true, 0x1000000)};

  kernel::GetCurrentState().GetAddressSpace()->GetHeapsPageStats(
      physical_heaps, std::size(physical_heaps), unreserved_pages, reserved_pages, used_pages,
      reserved_pages_bytes);

  REX_ASSERT(used_pages < stats_ptr->total_physical_pages);

  auto* memory = kernel::GetCurrentState().GetAddressSpace();
  const u32 title_pages = stats_ptr->total_physical_pages - stats_ptr->kernel_pages;
  const u32 budget = memory->GetBudgetPages() ? memory->GetBudgetPages() : title_pages;
  const u32 used = memory->GetBudgetPages() ? memory->GetTitlePages() : used_pages;
  stats_ptr->title.available_pages = budget > used ? budget - used : 0;
  stats_ptr->title.total_virtual_memory_bytes = 0x2FFE0000;
  stats_ptr->title.reserved_virtual_memory_bytes = reserved_pages_bytes;
  stats_ptr->title.physical_pages = 0x00001000;
  stats_ptr->title.pool_pages = 0x00000010;
  stats_ptr->title.stack_pages = 0x00000100;
  stats_ptr->title.image_pages = 0x00000100;
  stats_ptr->title.heap_pages = 0x00000100;
  stats_ptr->title.virtual_pages = 0x00000100;
  stats_ptr->title.page_table_pages = 0x00000100;
  stats_ptr->title.cache_pages = 0x00000100;

  stats_ptr->system.available_pages = 0x00000000;
  stats_ptr->system.total_virtual_memory_bytes = 0x00000000;
  stats_ptr->system.reserved_virtual_memory_bytes = 0x00000000;
  stats_ptr->system.physical_pages = 0x00000000;
  stats_ptr->system.pool_pages = 0x00000000;
  stats_ptr->system.stack_pages = 0x00000000;
  stats_ptr->system.image_pages = 0x00000000;
  stats_ptr->system.heap_pages = 0x00000000;
  stats_ptr->system.virtual_pages = 0x00000000;
  stats_ptr->system.page_table_pages = 0x00000000;
  stats_ptr->system.cache_pages = 0x00000000;

  stats_ptr->highest_physical_page = 0x0001FFFF;

  return X_STATUS_SUCCESS;
}

/** Returns the physical address of a guest address, or 0 when base_address is not physical. */
u32 MmGetPhysicalAddress_entry(u32 base_address) {
  u32 physical_address =
      kernel::GetCurrentState().GetAddressSpace()->GetPhysicalAddress(base_address);
  REX_ASSERT(physical_address != UINT32_MAX);
  if (physical_address == UINT32_MAX) {
    physical_address = 0;
  }
  REXKRNL_TRACE(
      "MmGetPhysicalAddress -> "
      "addr={:#x} -> {:#x}",
      static_cast<u32>(base_address), physical_address);
  return physical_address;
}

/**
 * Returns the source address, which suffices for XMA audio contexts. Other uses are unsupported.
 *
 * @param unk0 Must be 2.
 * @param src_address The address to map.
 * @param size The size in bytes, which must be 0x40.
 * @param flags Must be 0x404.
 */
u32 MmMapIoSpace_entry(u32 unk0, MappedVoid src_address, u32 size, u32 flags) {
  REX_ASSERT(unk0 == 2);
  REX_ASSERT(size == 0x40);
  REX_ASSERT(flags == 0x404);

  return src_address.GetGuestAddress();
}

struct X_POOL_ALLOC_HEADER {
  u8 unk_0;
  u8 unk_1;
  u8 unk_2;
  u8 unk_3;
  be_u32 tag;
};
REX_STATIC_ASSERT_SIZE(X_POOL_ALLOC_HEADER, 8);

u32 ExAllocatePoolTypeWithTag_entry(u32 size, u32 tag, [[maybe_unused]] u32 zero) {
  if (size <= 0xFD8) {
    u32 adjusted_size = size + sizeof(X_POOL_ALLOC_HEADER);
    u32 addr = kernel::GetCurrentState().GetAddressSpace()->SystemHeapAlloc(adjusted_size, 64);
    if (!addr)
      return 0;
    auto header =
        kernel::GetCurrentState().GetAddressSpace()->TranslateVirtual<X_POOL_ALLOC_HEADER*>(addr);
    header->unk_2 = 170;
    header->tag = tag;
    return addr + sizeof(X_POOL_ALLOC_HEADER);
  } else {
    return kernel::GetCurrentState().GetAddressSpace()->SystemHeapAlloc(size,
                                                                        kernel::kPageSize4KiB);
  }
}

u32 ExAllocatePool_entry(u32 size) {
  const u32 none = 0x656E6F4E;
  return ExAllocatePoolTypeWithTag_entry(size, none, 0);
}

void ExFreePool_entry(MappedVoid base_address) {
  u32 addr = base_address.GetGuestAddress();
  if ((addr & (kernel::kPageSize4KiB - 1)) == 0) {
    // A page aligned address is a large allocation without a pool header.
    kernel::GetCurrentState().GetAddressSpace()->SystemHeapFree(addr);
  } else {
    kernel::GetCurrentState().GetAddressSpace()->SystemHeapFree(addr - sizeof(X_POOL_ALLOC_HEADER));
  }
}

u32 KeGetImagePageTableEntry_entry(MappedVoid address) {
  auto heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeap(address.GetGuestAddress());
  if (!heap || heap->GetHeapType() != kernel::HeapType::kGuestXEX) {
    return 0;
  }
  u32 result = address.GetGuestAddress() - heap->GetHeapBase();
  result /= heap->GetPageSize();
  if (heap->GetPageSize() < 65536) {
    result |= 0x40000000;
  }
  return result & 0x400FFFFF;
}

u32 KeLockL2_entry() {
  return 0;
}

void KeUnlockL2_entry() {}

u32 MmCreateKernelStack_entry(u32 stack_size, u32 r4) {
  REX_ASSERT(r4 == 0);
  auto& runtime = kernel::GetCurrent();
  const auto* process = StateOf(runtime).GetAddressSpace()->TranslateVirtual<const X_KPROCESS*>(
      StateOf(runtime).GetTitleProcess());
  const u32 size =
      ResolveGuestStackSize(stack_size, process->kernel_stack_size, kMinimumThreadStackSize);
  Result<GuestStack> stack = AllocateGuestStack(*StateOf(runtime).GetAddressSpace(), size, 0, true);
  return stack ? stack->base : 0;
}

u32 MmDeleteKernelStack_entry(MappedVoid stack_base, MappedVoid stack_end) {
  auto* memory = kernel::GetCurrentState().GetAddressSpace();
  const u32 base = stack_base.GetGuestAddress();
  if (base <= KernelThread::kStackAddressRangeBegin || base > KernelThread::kStackAddressRangeEnd ||
      stack_end.GetGuestAddress() >= base) {
    return X_STATUS_INVALID_PARAMETER;
  }
  kernel::HeapAllocation info{};
  if (!memory->LookupHeap(base - 1)->QueryRegionInfo(base - 1, &info) || !info.state ||
      info.allocation_base + info.allocation_size != base ||
      stack_end.GetGuestAddress() < info.allocation_base + kGuestStackPageSize) {
    return X_STATUS_INVALID_PARAMETER;
  }
  FreeGuestStack(*memory, info.allocation_base);
  return X_STATUS_SUCCESS;
}

u32 ExAllocatePoolWithTag_entry(u32 numbytes, u32 tag, u32 zero) {
  return ExAllocatePoolTypeWithTag_entry(numbytes, tag, zero);
}

u32 MmIsAddressValid_entry(u32 address) {
  auto heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeap(address);
  if (!heap) {
    return 0;
  }
  return heap->QueryRangeAccess(address, address) != rex::memory::PageAccess::kNoAccess;
}

u32 NtAllocateEncryptedMemory_entry([[maybe_unused]] u32 unk, u32 region_size,
                                    MappedU32 base_addr_ptr) {
  if (!region_size) {
    return X_STATUS_INVALID_PARAMETER;
  }

  const u32 region_size_adjusted = rex::RoundUpNonZero(region_size, kernel::kPageSize64KiB);
  if (region_size_adjusted > 16 * 1024 * 1024) {
    return X_STATUS_INVALID_PARAMETER;
  }

  u32 out_address = 0;
  auto heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeap(0x8C000000);
  if (!heap) {
    return X_STATUS_UNSUCCESSFUL;
  }
  if (!heap->AllocRange(0x8C000000, 0x8FFFFFFF, region_size_adjusted, kernel::kPageSize64KiB,
                        kernel::kMemoryAllocationCommit | kernel::kMemoryAllocationTitle,
                        kernel::kMemoryProtectRead | kernel::kMemoryProtectWrite, false,
                        &out_address)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  REXKRNL_TRACE(
      "NtAllocateEncryptedMemory -> "
      "addr={:#x}",
      out_address);
  *base_addr_ptr = out_address;
  return X_STATUS_SUCCESS;
}

u32 NtFreeEncryptedMemory_entry([[maybe_unused]] u32 region_type, MappedU32 base_address_ptr) {
  REX_KERNEL_REQUIRE_NONNULL(base_address_ptr);

  auto heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeap(0x80000000);
  REX_KERNEL_REQUIRE_NONNULL(heap);
  const u32 encrypt_address = heap->GetHeapBase() + heap->GetPageSize() * (*base_address_ptr);

  auto encrypt_heap = kernel::GetCurrentState().GetAddressSpace()->LookupHeap(encrypt_address);
  if (!encrypt_heap || encrypt_heap->GetHeapType() != kernel::HeapType::kGuestXEX) {
    return X_STATUS_INVALID_PARAMETER;
  }

  kernel::GetCurrentState().GetAddressSpace()->SystemHeapFree(encrypt_address);
  return X_STATUS_SUCCESS;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__NtAllocateVirtualMemory, rex::kernel::xboxkrnl::NtAllocateVirtualMemory_entry)
REX_EXPORT(__imp__NtProtectVirtualMemory, rex::kernel::xboxkrnl::NtProtectVirtualMemory_entry)
REX_EXPORT(__imp__NtFreeVirtualMemory, rex::kernel::xboxkrnl::NtFreeVirtualMemory_entry)
REX_EXPORT(__imp__NtQueryVirtualMemory, rex::kernel::xboxkrnl::NtQueryVirtualMemory_entry)
REX_EXPORT(__imp__MmAllocatePhysicalMemoryEx,
           rex::kernel::xboxkrnl::MmAllocatePhysicalMemoryEx_entry)
REX_EXPORT(__imp__MmAllocatePhysicalMemory, rex::kernel::xboxkrnl::MmAllocatePhysicalMemory_entry)
REX_EXPORT(__imp__MmFreePhysicalMemory, rex::kernel::xboxkrnl::MmFreePhysicalMemory_entry)
REX_EXPORT(__imp__MmQueryAddressProtect, rex::kernel::xboxkrnl::MmQueryAddressProtect_entry)
REX_EXPORT(__imp__MmSetAddressProtect, rex::kernel::xboxkrnl::MmSetAddressProtect_entry)
REX_EXPORT(__imp__MmQueryAllocationSize, rex::kernel::xboxkrnl::MmQueryAllocationSize_entry)
REX_EXPORT(__imp__MmQueryStatistics, rex::kernel::xboxkrnl::MmQueryStatistics_entry)
REX_EXPORT(__imp__MmGetPhysicalAddress, rex::kernel::xboxkrnl::MmGetPhysicalAddress_entry)
REX_EXPORT(__imp__MmMapIoSpace, rex::kernel::xboxkrnl::MmMapIoSpace_entry)
REX_EXPORT(__imp__ExAllocatePoolTypeWithTag, rex::kernel::xboxkrnl::ExAllocatePoolTypeWithTag_entry)
REX_EXPORT(__imp__ExAllocatePool, rex::kernel::xboxkrnl::ExAllocatePool_entry)
REX_EXPORT(__imp__ExFreePool, rex::kernel::xboxkrnl::ExFreePool_entry)
REX_EXPORT(__imp__KeGetImagePageTableEntry, rex::kernel::xboxkrnl::KeGetImagePageTableEntry_entry)
REX_EXPORT(__imp__KeLockL2, rex::kernel::xboxkrnl::KeLockL2_entry)
REX_EXPORT(__imp__KeUnlockL2, rex::kernel::xboxkrnl::KeUnlockL2_entry)
REX_EXPORT(__imp__MmCreateKernelStack, rex::kernel::xboxkrnl::MmCreateKernelStack_entry)
REX_EXPORT(__imp__MmDeleteKernelStack, rex::kernel::xboxkrnl::MmDeleteKernelStack_entry)

REX_EXPORT(__imp__ExAllocatePoolWithTag, rex::kernel::xboxkrnl::ExAllocatePoolWithTag_entry)
REX_EXPORT_STUB(__imp__ExQueryPoolBlockSize);
REX_EXPORT_STUB(__imp__MmDoubleMapMemory);
REX_EXPORT_STUB(__imp__MmUnmapMemory);
REX_EXPORT(__imp__MmIsAddressValid, rex::kernel::xboxkrnl::MmIsAddressValid_entry)
REX_EXPORT_STUB(__imp__MmLockAndMapSegmentArray);
REX_EXPORT_STUB(__imp__MmLockUnlockBufferPages);
REX_EXPORT_STUB(__imp__MmPersistPhysicalMemoryAllocation);
REX_EXPORT_STUB(__imp__MmSplitPhysicalMemoryAllocation);
REX_EXPORT_STUB(__imp__MmUnlockAndUnmapSegmentArray);
REX_EXPORT_STUB(__imp__MmUnmapIoSpace);
REX_EXPORT(__imp__NtAllocateEncryptedMemory, rex::kernel::xboxkrnl::NtAllocateEncryptedMemory_entry)
REX_EXPORT(__imp__NtFreeEncryptedMemory, rex::kernel::xboxkrnl::NtFreeEncryptedMemory_entry)
REX_EXPORT_STUB(__imp__ExDebugMonitorService);
REX_EXPORT_STUB(__imp__MmDbgReadCheck);
REX_EXPORT_STUB(__imp__MmDbgReleaseAddress);
REX_EXPORT_STUB(__imp__MmDbgWriteCheck);
REX_EXPORT_STUB(__imp__MmGetPoolPagesType);
REX_EXPORT_STUB(__imp__ExExpansionInstall);
REX_EXPORT_STUB(__imp__ExExpansionCall);
REX_EXPORT_STUB(__imp__MmResetLowestAvailablePages);
