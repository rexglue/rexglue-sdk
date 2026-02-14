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

 // Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <cstring>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/kernel/kernel_state.h>
#include <rex/runtime/guest/function.h>
#include <rex/runtime/guest/types.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/kernel/xtypes.h>

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime::guest;

uint32_t ToXdkProtectFlags(uint32_t protect) {
  uint32_t result = 0;
  if (!(protect & memory::kMemoryProtectRead) && !(protect & memory::kMemoryProtectWrite)) {
    result = X_PAGE_NOACCESS;
  } else if ((protect & memory::kMemoryProtectRead) &&
             !(protect & memory::kMemoryProtectWrite)) {
    result = X_PAGE_READONLY;
  } else {
    result = X_PAGE_READWRITE;
  }
  if (protect & memory::kMemoryProtectNoCache) {
    result |= X_PAGE_NOCACHE;
  }
  if (protect & memory::kMemoryProtectWriteCombine) {
    result |= X_PAGE_WRITECOMBINE;
  }
  return result;
}

uint32_t FromXdkProtectFlags(uint32_t protect) {
  uint32_t result = 0;
  if ((protect & X_PAGE_READONLY) | (protect & X_PAGE_EXECUTE_READ)) {
    result = memory::kMemoryProtectRead;
  } else if ((protect & X_PAGE_READWRITE) |
             (protect & X_PAGE_EXECUTE_READWRITE)) {
    result = memory::kMemoryProtectRead | memory::kMemoryProtectWrite;
  }
  if (protect & X_PAGE_NOCACHE) {
    result |= memory::kMemoryProtectNoCache;
  }
  if (protect & X_PAGE_WRITECOMBINE) {
    result |= memory::kMemoryProtectWriteCombine;
  }
  return result;
}

dword_result_t NtAllocateVirtualMemory_entry(lpdword_t base_addr_ptr,
                                             lpdword_t region_size_ptr,
                                             dword_t alloc_type,
                                             dword_t protect_bits,
                                             dword_t debug_memory) {
  uint32_t input_base = base_addr_ptr ? static_cast<uint32_t>(*base_addr_ptr) : 0;
  uint32_t input_size = region_size_ptr ? static_cast<uint32_t>(*region_size_ptr) : 0;
  REXKRNL_IMPORT_TRACE("NtAllocateVirtualMemory", "base={:#x} size={:#x} type={:#x} protect={:#x} debug={}",
         input_base, input_size, (uint32_t)alloc_type, (uint32_t)protect_bits, (uint32_t)debug_memory);

  // NTSTATUS
  // _Inout_  PVOID *BaseAddress,
  // _Inout_  PSIZE_T RegionSize,
  // _In_     ULONG AllocationType,
  // _In_     ULONG Protect
  // _In_     BOOLEAN DebugMemory

  assert_not_null(base_addr_ptr);
  assert_not_null(region_size_ptr);

  // Set to TRUE when allocation is from devkit memory area.
  // assert_true(debug_memory == 0);
  // just warn tf am i gunna do about it
  if((uint32_t)debug_memory != 0)
    REXKRNL_WARN("attmpted allocation to devkit memory area (debug_memory={})", (uint32_t)debug_memory);

  // This allocates memory from the kernel heap, which is initialized on startup
  // and shared by both the kernel implementation and user code.
  // The xe_memory_ref object is used to actually get the memory, and although
  // it's simple today we could extend it to do better things in the future.

  // Must request a size.
  if (!base_addr_ptr || !region_size_ptr || !*region_size_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // Check allocation type.
  if (!(alloc_type & (X_MEM_COMMIT | X_MEM_RESET | X_MEM_RESERVE))) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // If MEM_RESET is set only MEM_RESET can be set.
  if (alloc_type & X_MEM_RESET && (alloc_type & ~X_MEM_RESET)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // Don't allow games to set execute bits.
  if (protect_bits & (X_PAGE_EXECUTE | X_PAGE_EXECUTE_READ |
                      X_PAGE_EXECUTE_READWRITE | X_PAGE_EXECUTE_WRITECOPY)) {
    REXKRNL_WARN("Game setting EXECUTE bit on allocation");
  }

  uint32_t page_size;
  if (*base_addr_ptr != 0) {
    // ignore specified page size when base address is specified.
    auto heap = kernel_memory()->LookupHeap(*base_addr_ptr);
    if (heap->heap_type() != memory::HeapType::kGuestVirtual) {
      return X_STATUS_INVALID_PARAMETER;
    }
    page_size = heap->page_size();
  } else {
    // Adjust size.
    page_size = 4 * 1024;
    if (alloc_type & X_MEM_LARGE_PAGES) {
      page_size = 64 * 1024;
    }
  }

  // Round the base address down to the nearest page boundary.
  uint32_t adjusted_base = *base_addr_ptr - (*base_addr_ptr % page_size);
  // For some reason, some games pass in negative sizes.
  uint32_t adjusted_size = int32_t(*region_size_ptr) < 0
                               ? -int32_t(region_size_ptr.value())
                               : region_size_ptr.value();
  adjusted_size = rex::round_up(adjusted_size, page_size);

  // Allocate.
  uint32_t allocation_type = 0;
  if (alloc_type & X_MEM_RESERVE) {
    allocation_type |= memory::kMemoryAllocationReserve;
  }
  if (alloc_type & X_MEM_COMMIT) {
    allocation_type |= memory::kMemoryAllocationCommit;
  }
  if (alloc_type & X_MEM_RESET) {
    REXKRNL_ERROR("X_MEM_RESET not implemented");
    assert_always();
  }
  uint32_t protect = FromXdkProtectFlags(protect_bits);
  uint32_t address = 0;
  memory::BaseHeap* heap;
  memory::HeapAllocationInfo prev_alloc_info = {};
  bool was_commited = false;

  if (adjusted_base != 0) {
    heap = kernel_memory()->LookupHeap(adjusted_base);
    if (heap->page_size() != page_size) {
      // Specified the wrong page size for the wrong heap.
      return X_STATUS_ACCESS_DENIED;
    }
    was_commited = heap->QueryRegionInfo(adjusted_base, &prev_alloc_info) &&
                   (prev_alloc_info.state & memory::kMemoryAllocationCommit) != 0;

    // Log was_commited status
    REXKRNL_DEBUG("[NTALLOC] QueryRegion: base=0x{:08X} prev_state=0x{:X} was_commited={}",
           adjusted_base, prev_alloc_info.state, was_commited);

    if (heap->AllocFixed(adjusted_base, adjusted_size, page_size,
                         allocation_type, protect)) {
      address = adjusted_base;
    }
  } else {
    bool top_down = !!(alloc_type & X_MEM_TOP_DOWN);
    heap = kernel_memory()->LookupHeapByType(false, page_size);
    heap->Alloc(adjusted_size, page_size, allocation_type, protect, top_down,
                &address);
  }
  if (!address) {
    // Failed - assume no memory available.
    return X_STATUS_NO_MEMORY;
  }

  // Zero memory, if needed.
  if (address && !(alloc_type & X_MEM_NOZERO)) {
    if (alloc_type & X_MEM_COMMIT) {
      if (!(protect & memory::kMemoryProtectWrite)) {
        heap->Protect(address, adjusted_size,
                      memory::kMemoryProtectRead | memory::kMemoryProtectWrite);
      }
      if (!was_commited) {
        kernel_memory()->Zero(address, adjusted_size);
      }
      if (!(protect & memory::kMemoryProtectWrite)) {
        heap->Protect(address, adjusted_size, protect);
      }
    }
  }

  REXKRNL_DEBUG("NtAllocateVirtualMemory = {:08X}", address);

  // Stash back.
  // Maybe set X_STATUS_ALREADY_COMMITTED if MEM_COMMIT?
  *base_addr_ptr = address;
  *region_size_ptr = adjusted_size;
  REXKRNL_IMPORT_RESULT("NtAllocateVirtualMemory", "0x0 addr={:#x} size={:#x}",
         address, adjusted_size);
  return X_STATUS_SUCCESS;
}

dword_result_t NtProtectVirtualMemory_entry(lpdword_t base_addr_ptr,
                                            lpdword_t region_size_ptr,
                                            dword_t protect_bits,
                                            lpdword_t old_protect,
                                            dword_t debug_memory) {
  // Set to TRUE when this memory refers to devkit memory area.
  assert_true(debug_memory == 0);

  // Must request a size.
  if (!base_addr_ptr || !region_size_ptr || !*region_size_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  // Don't allow games to set execute bits.
  if (protect_bits & (X_PAGE_EXECUTE | X_PAGE_EXECUTE_READ |
                      X_PAGE_EXECUTE_READWRITE | X_PAGE_EXECUTE_WRITECOPY)) {
    REXKRNL_WARN("Game setting EXECUTE bit on protect");
    return X_STATUS_ACCESS_DENIED;
  }

  auto heap = kernel_memory()->LookupHeap(*base_addr_ptr);
  if (heap->heap_type() != memory::HeapType::kGuestVirtual) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // Adjust the base downwards to the nearest page boundary.
  uint32_t adjusted_base =
      *base_addr_ptr - (*base_addr_ptr % heap->page_size());
  uint32_t adjusted_size = rex::round_up(*region_size_ptr, heap->page_size());
  uint32_t protect = FromXdkProtectFlags(protect_bits);

  uint32_t tmp_old_protect = 0;

  // FIXME: I think it's valid for NtProtectVirtualMemory to span regions, but
  // as of now our implementation will fail in this case. Need to verify.
  if (!heap->Protect(adjusted_base, adjusted_size, protect, &tmp_old_protect)) {
    return X_STATUS_ACCESS_DENIED;
  }

  // Write back output variables.
  *base_addr_ptr = adjusted_base;
  *region_size_ptr = adjusted_size;

  if (old_protect) {
    *old_protect = tmp_old_protect;
  }

  return X_STATUS_SUCCESS;
}

dword_result_t NtFreeVirtualMemory_entry(lpdword_t base_addr_ptr,
                                         lpdword_t region_size_ptr,
                                         dword_t free_type,
                                         dword_t debug_memory) {
  uint32_t base_addr_value = *base_addr_ptr;
  uint32_t region_size_value = *region_size_ptr;
  REXKRNL_IMPORT_TRACE("NtFreeVirtualMemory", "base={:#x} size={:#x} type={:#x} debug={}",
         base_addr_value, region_size_value, (uint32_t)free_type, (uint32_t)debug_memory);
  // X_MEM_DECOMMIT | X_MEM_RELEASE

  // NTSTATUS
  // _Inout_  PVOID *BaseAddress,
  // _Inout_  PSIZE_T RegionSize,
  // _In_     ULONG FreeType
  // _In_     BOOLEAN DebugMemory

  // Set to TRUE when freeing external devkit memory.
  assert_true(debug_memory == 0);

  if (!base_addr_value) {
    return X_STATUS_MEMORY_NOT_ALLOCATED;
  }

  auto heap = kernel_state()->memory()->LookupHeap(base_addr_value);
  if (heap->heap_type() != memory::HeapType::kGuestVirtual) {
    return X_STATUS_INVALID_PARAMETER;
  }
  bool result = false;
  if (free_type == X_MEM_DECOMMIT) {
    // If zero, we may need to query size (free whole region).
    assert_not_zero(region_size_value);

    region_size_value = rex::round_up(region_size_value, heap->page_size());
    result = heap->Decommit(base_addr_value, region_size_value);
  } else {
    result = heap->Release(base_addr_value, &region_size_value);
  }
  if (!result) {
    return X_STATUS_UNSUCCESSFUL;
  }

  *base_addr_ptr = base_addr_value;
  *region_size_ptr = region_size_value;
  REXKRNL_IMPORT_RESULT("NtFreeVirtualMemory", "0x0");
  return X_STATUS_SUCCESS;
}

struct X_MEMORY_BASIC_INFORMATION {
  be<uint32_t> base_address;
  be<uint32_t> allocation_base;
  be<uint32_t> allocation_protect;
  be<uint32_t> region_size;
  be<uint32_t> state;
  be<uint32_t> protect;
  be<uint32_t> type;
};

dword_result_t NtQueryVirtualMemory_entry(
    dword_t base_address,
    pointer_t<X_MEMORY_BASIC_INFORMATION> memory_basic_information_ptr) {
  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  memory::HeapAllocationInfo alloc_info;
  if (heap == nullptr || !heap->QueryRegionInfo(base_address, &alloc_info)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  memory_basic_information_ptr->base_address = alloc_info.base_address;
  memory_basic_information_ptr->allocation_base = alloc_info.allocation_base;
  memory_basic_information_ptr->allocation_protect =
      ToXdkProtectFlags(alloc_info.allocation_protect);
  memory_basic_information_ptr->region_size = alloc_info.region_size;
  // https://docs.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-memory_basic_information
  // State: ... This member can be one of the following values: MEM_COMMIT,
  // MEM_FREE, MEM_RESERVE.
  // State queried by Beautiful Katamari before displaying the loading screen.
  uint32_t x_state;
  if (alloc_info.state & memory::kMemoryAllocationCommit) {
    assert_not_zero(alloc_info.state & memory::kMemoryAllocationReserve);
    x_state = X_MEM_COMMIT;
  } else if (alloc_info.state & memory::kMemoryAllocationReserve) {
    x_state = X_MEM_RESERVE;
  } else {
    x_state = X_MEM_FREE;
  }
  memory_basic_information_ptr->state = x_state;
  memory_basic_information_ptr->protect = ToXdkProtectFlags(alloc_info.protect);
  memory_basic_information_ptr->type = X_MEM_PRIVATE;

  return X_STATUS_SUCCESS;
}

dword_result_t MmAllocatePhysicalMemoryEx_entry(
    dword_t flags, dword_t region_size, dword_t protect_bits,
    dword_t min_addr_range, dword_t max_addr_range, dword_t alignment) {
  REXKRNL_IMPORT_TRACE("MmAllocatePhysicalMemoryEx", "flags={:#x} size={:#x} protect={:#x} min={:#x} max={:#x} align={:#x}",
         (uint32_t)flags, (uint32_t)region_size, (uint32_t)protect_bits,
         (uint32_t)min_addr_range, (uint32_t)max_addr_range, (uint32_t)alignment);

  // Check protection bits.
  if (!(protect_bits & (X_PAGE_READONLY | X_PAGE_READWRITE))) {
    REXKRNL_ERROR("MmAllocatePhysicalMemoryEx: bad protection bits");
    return 0;
  }

  // Either may be OR'ed into protect_bits:
  // X_PAGE_NOCACHE
  // X_PAGE_WRITECOMBINE
  // We could use this to detect what's likely GPU-synchronized memory
  // and let the GPU know we're messing with it (or even allocate from
  // the GPU). At least the D3D command buffer is X_PAGE_WRITECOMBINE.

  // Calculate page size.
  // Default            = 4KB
  // X_MEM_LARGE_PAGES  = 64KB
  // X_MEM_16MB_PAGES   = 16MB
  uint32_t page_size = 4 * 1024;
  if (protect_bits & X_MEM_LARGE_PAGES) {
    page_size = 64 * 1024;
  } else if (protect_bits & X_MEM_16MB_PAGES) {
    page_size = 16 * 1024 * 1024;
  }

  // Round up the region size and alignment to the next page.
  uint32_t adjusted_size = rex::round_up(region_size, page_size);
  uint32_t adjusted_alignment = rex::round_up(alignment, page_size);

  uint32_t allocation_type = memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit;
  uint32_t protect = FromXdkProtectFlags(protect_bits);
  bool top_down = true;
  auto heap = static_cast<memory::PhysicalHeap*>(
      kernel_memory()->LookupHeapByType(true, page_size));
  // min_addr_range/max_addr_range are bounds in physical memory, not virtual.
  uint32_t heap_base = heap->heap_base();
  uint32_t heap_physical_address_offset = heap->GetPhysicalAddress(heap_base);
  uint32_t heap_min_addr =
      rex::sat_sub(min_addr_range.value(), heap_physical_address_offset);
  uint32_t heap_max_addr =
      rex::sat_sub(max_addr_range.value(), heap_physical_address_offset);
  uint32_t heap_size = heap->heap_size();
  heap_min_addr = heap_base + std::min(heap_min_addr, heap_size - 1);
  heap_max_addr = heap_base + std::min(heap_max_addr, heap_size - 1);
  uint32_t base_address;
  if (!heap->AllocRange(heap_min_addr, heap_max_addr, adjusted_size,
                        adjusted_alignment, allocation_type, protect, top_down,
                        &base_address)) {
    // Failed - assume no memory available.
    return 0;
  }
  REXKRNL_IMPORT_RESULT("MmAllocatePhysicalMemoryEx", "addr={:#x}", base_address);

  return base_address;
}

dword_result_t MmAllocatePhysicalMemory_entry(dword_t flags,
                                              dword_t region_size,
                                              dword_t protect_bits) {
  return MmAllocatePhysicalMemoryEx_entry(flags, region_size, protect_bits, 0,
                                          0xFFFFFFFFu, 0);
}

void MmFreePhysicalMemory_entry(dword_t type, dword_t base_address) {
  REXKRNL_IMPORT_TRACE("MmFreePhysicalMemory", "type={:#x} addr={:#x}",
         (uint32_t)type, (uint32_t)base_address);

  assert_true((base_address & 0x1F) == 0);

  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  heap->Release(base_address);
}

dword_result_t MmQueryAddressProtect_entry(dword_t base_address) {
  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  uint32_t access;
  if (!heap->QueryProtect(base_address, &access)) {
    access = 0;
  }
  access = !access ? 0 : ToXdkProtectFlags(access);

  return access;
}

void MmSetAddressProtect_entry(lpvoid_t base_address, dword_t region_size,
                               dword_t protect_bits) {
  if (!protect_bits) {
    REXKRNL_ERROR("MmSetAddressProtect: Failed due to incorrect protect_bits");
    return;
  }

  uint32_t protect = FromXdkProtectFlags(protect_bits);
  auto heap = kernel_memory()->LookupHeap(base_address);
  heap->Protect(base_address.guest_address(), region_size, protect);
}

dword_result_t MmQueryAllocationSize_entry(lpvoid_t base_address) {
  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  uint32_t size;
  if (!heap->QuerySize(base_address, &size)) {
    size = 0;
  }

  return size;
}

// https://code.google.com/p/vdash/source/browse/trunk/vdash/include/kernel.h
struct X_MM_QUERY_STATISTICS_SECTION {
  rex::be<uint32_t> available_pages;
  rex::be<uint32_t> total_virtual_memory_bytes;
  rex::be<uint32_t> reserved_virtual_memory_bytes;
  rex::be<uint32_t> physical_pages;
  rex::be<uint32_t> pool_pages;
  rex::be<uint32_t> stack_pages;
  rex::be<uint32_t> image_pages;
  rex::be<uint32_t> heap_pages;
  rex::be<uint32_t> virtual_pages;
  rex::be<uint32_t> page_table_pages;
  rex::be<uint32_t> cache_pages;
};

struct X_MM_QUERY_STATISTICS_RESULT {
  rex::be<uint32_t> size;
  rex::be<uint32_t> total_physical_pages;
  rex::be<uint32_t> kernel_pages;
  X_MM_QUERY_STATISTICS_SECTION title;
  X_MM_QUERY_STATISTICS_SECTION system;
  rex::be<uint32_t> highest_physical_page;
};
static_assert_size(X_MM_QUERY_STATISTICS_RESULT, 104);

dword_result_t MmQueryStatistics_entry(
    pointer_t<X_MM_QUERY_STATISTICS_RESULT> stats_ptr) {
  if (!stats_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  const uint32_t size = sizeof(X_MM_QUERY_STATISTICS_RESULT);

  if (stats_ptr->size != size) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }

  // Zero out the struct.
  stats_ptr.Zero();

  // Set the constants the game is likely asking for.
  // These numbers are mostly guessed. If the game is just checking for
  // memory, this should satisfy it. If it's actually verifying things
  // this won't work :/
  stats_ptr->size = size;

  stats_ptr->total_physical_pages = 0x00020000;  // 512mb / 4kb pages
  stats_ptr->kernel_pages = 0x00000300;

  // TODO(gibbed): maybe use LookupHeapByType instead?
  auto heap_a = kernel_memory()->LookupHeap(0xA0000000);
  auto heap_c = kernel_memory()->LookupHeap(0xC0000000);
  auto heap_e = kernel_memory()->LookupHeap(0xE0000000);

  assert_not_null(heap_a);
  assert_not_null(heap_c);
  assert_not_null(heap_e);

#define GET_USED_PAGE_COUNT(x) \
  (x->GetTotalPageCount() - x->GetUnreservedPageCount())
#define GET_USED_PAGE_SIZE(x) ((GET_USED_PAGE_COUNT(x) * x->page_size()) / 4096)
  uint32_t used_pages = 0;
  used_pages += GET_USED_PAGE_SIZE(heap_a);
  used_pages += GET_USED_PAGE_SIZE(heap_c);
  used_pages += GET_USED_PAGE_SIZE(heap_e);
#undef GET_USED_PAGE_SIZE
#undef GET_USED_PAGE_COUNT

  assert_true(used_pages < stats_ptr->total_physical_pages);

  stats_ptr->title.available_pages =
      stats_ptr->total_physical_pages - used_pages;
  stats_ptr->title.total_virtual_memory_bytes =
      0x2FFF0000;  // TODO(gibbed): FIXME
  stats_ptr->title.reserved_virtual_memory_bytes =
      0x00160000;                                // TODO(gibbed): FIXME
  stats_ptr->title.physical_pages = 0x00001000;  // TODO(gibbed): FIXME
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

// https://msdn.microsoft.com/en-us/library/windows/hardware/ff554547(v=vs.85).aspx
dword_result_t MmGetPhysicalAddress_entry(dword_t base_address) {
  // base_address = result of MmAllocatePhysicalMemory.
  uint32_t physical_address = kernel_memory()->GetPhysicalAddress(base_address);
  assert_true(physical_address != UINT32_MAX);
  if (physical_address == UINT32_MAX) {
    physical_address = 0;
  }
  REXKRNL_IMPORT_RESULT("MmGetPhysicalAddress", "addr={:#x} -> {:#x}",
         (uint32_t)base_address, physical_address);
  return physical_address;
}

dword_result_t MmMapIoSpace_entry(dword_t unk0, lpvoid_t src_address,
                                  dword_t size, dword_t flags) {
  // I've only seen this used to map XMA audio contexts.
  // The code seems fine with taking the src address, so this just returns that.
  // If others start using it there could be problems.
  assert_true(unk0 == 2);
  assert_true(size == 0x40);
  assert_true(flags == 0x404);

  return src_address.guest_address();
}

dword_result_t ExAllocatePoolTypeWithTag_entry(dword_t size, dword_t tag,
                                               dword_t zero) {
  uint32_t alignment = 8;
  uint32_t adjusted_size = size;
  if (adjusted_size < 4 * 1024) {
    adjusted_size = rex::round_up(adjusted_size, 4 * 1024);
  } else {
    alignment = 4 * 1024;
  }

  uint32_t addr =
      kernel_state()->memory()->SystemHeapAlloc(adjusted_size, alignment);

  return addr;
}

dword_result_t ExAllocatePool_entry(dword_t size) {
  const uint32_t none = 0x656E6F4E;  // 'None'
  return ExAllocatePoolTypeWithTag_entry(size, none, 0);
}

void ExFreePool_entry(lpvoid_t base_address) {
  kernel_state()->memory()->SystemHeapFree(base_address);
}

dword_result_t KeGetImagePageTableEntry_entry(lpvoid_t address) {
  // Unknown
  return 1;
}

dword_result_t KeLockL2_entry() {
  // TODO
  return 0;
}

void KeUnlockL2_entry() {}

dword_result_t MmCreateKernelStack_entry(dword_t stack_size, dword_t r4) {
  assert_zero(r4);  // Unknown argument.

  auto stack_size_aligned = (stack_size + 0xFFF) & 0xFFFFF000;
  uint32_t stack_alignment = (stack_size & 0xF000) ? 0x1000 : 0x10000;

  uint32_t stack_address;
  kernel_memory()
      ->LookupHeap(0x70000000)
      ->AllocRange(0x70000000, 0x7F000000, stack_size_aligned, stack_alignment,
                   memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                   memory::kMemoryProtectRead | memory::kMemoryProtectWrite, false,
                   &stack_address);
  return stack_address + stack_size;
}

dword_result_t MmDeleteKernelStack_entry(lpvoid_t stack_base,
                                         lpvoid_t stack_end) {
  // Release the stack (where stack_end is the low address)
  if (kernel_memory()->LookupHeap(0x70000000)->Release(stack_end)) {
    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

}  // namespace rex::kernel::xboxkrnl

GUEST_FUNCTION_HOOK(__imp__NtAllocateVirtualMemory, rex::kernel::xboxkrnl::NtAllocateVirtualMemory_entry)
GUEST_FUNCTION_HOOK(__imp__NtProtectVirtualMemory, rex::kernel::xboxkrnl::NtProtectVirtualMemory_entry)
GUEST_FUNCTION_HOOK(__imp__NtFreeVirtualMemory, rex::kernel::xboxkrnl::NtFreeVirtualMemory_entry)
GUEST_FUNCTION_HOOK(__imp__NtQueryVirtualMemory, rex::kernel::xboxkrnl::NtQueryVirtualMemory_entry)
GUEST_FUNCTION_HOOK(__imp__MmAllocatePhysicalMemoryEx, rex::kernel::xboxkrnl::MmAllocatePhysicalMemoryEx_entry)
GUEST_FUNCTION_HOOK(__imp__MmAllocatePhysicalMemory, rex::kernel::xboxkrnl::MmAllocatePhysicalMemory_entry)
GUEST_FUNCTION_HOOK(__imp__MmFreePhysicalMemory, rex::kernel::xboxkrnl::MmFreePhysicalMemory_entry)
GUEST_FUNCTION_HOOK(__imp__MmQueryAddressProtect, rex::kernel::xboxkrnl::MmQueryAddressProtect_entry)
GUEST_FUNCTION_HOOK(__imp__MmSetAddressProtect, rex::kernel::xboxkrnl::MmSetAddressProtect_entry)
GUEST_FUNCTION_HOOK(__imp__MmQueryAllocationSize, rex::kernel::xboxkrnl::MmQueryAllocationSize_entry)
GUEST_FUNCTION_HOOK(__imp__MmQueryStatistics, rex::kernel::xboxkrnl::MmQueryStatistics_entry)
GUEST_FUNCTION_HOOK(__imp__MmGetPhysicalAddress, rex::kernel::xboxkrnl::MmGetPhysicalAddress_entry)
GUEST_FUNCTION_HOOK(__imp__MmMapIoSpace, rex::kernel::xboxkrnl::MmMapIoSpace_entry)
GUEST_FUNCTION_HOOK(__imp__ExAllocatePoolTypeWithTag, rex::kernel::xboxkrnl::ExAllocatePoolTypeWithTag_entry)
GUEST_FUNCTION_HOOK(__imp__ExAllocatePool, rex::kernel::xboxkrnl::ExAllocatePool_entry)
GUEST_FUNCTION_HOOK(__imp__ExFreePool, rex::kernel::xboxkrnl::ExFreePool_entry)
GUEST_FUNCTION_HOOK(__imp__KeGetImagePageTableEntry, rex::kernel::xboxkrnl::KeGetImagePageTableEntry_entry)
GUEST_FUNCTION_HOOK(__imp__KeLockL2, rex::kernel::xboxkrnl::KeLockL2_entry)
GUEST_FUNCTION_HOOK(__imp__KeUnlockL2, rex::kernel::xboxkrnl::KeUnlockL2_entry)
GUEST_FUNCTION_HOOK(__imp__MmCreateKernelStack, rex::kernel::xboxkrnl::MmCreateKernelStack_entry)
GUEST_FUNCTION_HOOK(__imp__MmDeleteKernelStack, rex::kernel::xboxkrnl::MmDeleteKernelStack_entry)
