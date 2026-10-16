/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/memory/virtual_memory.h>
#include <rex/ppc/func.h>
#include <rex/platform/lock_order.h>
#include <rex/types.h>

namespace rex::runtime::detail {

/**
 * Returns the host page offset of a guest address. The 0xE0 view maps physical 0 on every platform,
 * so a guest address from 0xE0000000 up adds the 1 page that the hardware adds.
 *
 * @param guest_addr The guest virtual address.
 * @return 0x1000 from 0xE0000000 up, else 0.
 */
constexpr u32 PhysicalHostOffset(u32 guest_addr) noexcept {
  return (guest_addr >= 0xE0000000u) ? 0x1000u : 0u;
}

/**
 * Inverts PhysicalHostOffset for a host offset from the virtual base.
 *
 * @param host_offset The host address minus the virtual base.
 * @return The guest virtual address.
 */
constexpr u32 RemovePhysicalHostOffset(std::size_t host_offset) noexcept {
  constexpr std::size_t kOffset = PhysicalHostOffset(0xE0000000u);
  const bool shifted =
      host_offset >= 0xE0000000u + kOffset && host_offset < 0x100000000ull + kOffset;
  return static_cast<u32>(shifted ? host_offset - kOffset : host_offset);
}

}  // namespace rex::runtime::detail

namespace rex::runtime {

/**
 * Translates a guest address to a host pointer from the memory base, with the same arithmetic as
 * recompiled code and without a Memory or heap lookup.
 *
 * @tparam T The pointer type to return.
 * @param base The host base of guest memory.
 * @param guest_address The guest address.
 * @return The host pointer.
 */
template <typename T = u8*>
inline T GuestPtr(u8* base, u32 guest_address) noexcept {
  return reinterpret_cast<T>(base + guest_address + detail::PhysicalHostOffset(guest_address));
}

class Memory;

/** Selects the heap that SystemHeapAlloc takes memory from. */
enum SystemHeapFlag : u8 {
  kSystemHeapVirtual = 1 << 0,  /**< Virtual heap. */
  kSystemHeapPhysical = 1 << 1, /**< Physical heap. */

  kSystemHeapDefault = kSystemHeapVirtual, /**< Virtual heap. */
};

/** Address range that a heap manages. */
enum class HeapType : u8 {
  kGuestVirtual,  /**< Guest virtual memory. */
  kGuestXEX,      /**< Memory that executable images are mapped into. */
  kGuestPhysical, /**< Physical memory views. */
};

/** Bits of an allocation request. */
enum MemoryAllocationFlag : u8 {
  kMemoryAllocationReserve = 1 << 0, /**< Reserve address space. */
  kMemoryAllocationCommit = 1 << 1,  /**< Commit pages. */
  /** Counts the newly committed pages against the title memory budget. */
  kMemoryAllocationTitle = 1 << 2,
};

/** Bits of a page protection. */
enum MemoryProtectFlag : u8 {
  kMemoryProtectRead = 1 << 0,         /**< Read access. */
  kMemoryProtectWrite = 1 << 1,        /**< Write access. */
  kMemoryProtectNoCache = 1 << 2,      /**< Uncached access. */
  kMemoryProtectWriteCombine = 1 << 3, /**< Write combined access. */

  kMemoryProtectNoAccess = 0, /**< No access. */
};

/** Equivalent to the Win32 MEMORY_BASIC_INFORMATION struct. */
struct HeapAllocationInfo {
  u32 base_address; /**< Base address of the queried pages. */
  /** Base address of the allocation range that contains base_address. */
  u32 allocation_base;
  /** Protection when the region was first allocated. */
  u32 allocation_protect;
  /** Size in bytes when the region was first allocated. */
  u32 allocation_size;
  /** Size in bytes from base_address of the pages with identical attributes. */
  u32 region_size;
  /** Commit, free or reserve state of the pages. */
  u32 state;
  u32 protect; /**< Current protection, MemoryProtectFlag bits. */
};

/** Describes a single page in the page table. */
union PageEntry {
  u64 qword; /**< The whole entry read or written as 1 64 bit value. */
  struct {
    /** Base page of the allocated region in 4 KB pages. */
    u32 base_address : 20;
    /** Page count of the allocated region in 4 KB pages. */
    u32 region_page_count : 20;
    /** MemoryProtectFlag bits given at allocation. */
    u32 allocation_protect : 4;
    /** MemoryProtectFlag bits as of the last Protect. */
    u32 current_protect : 4;
    /** MemoryAllocationFlag bit mask of the page state. */
    u32 state : 2;
    /** Set on a committed page that counts against the title memory budget. */
    u32 title : 1;
    u32 reserved : 13; /**< Unused. */
  };
};

/** Heap abstraction for page based allocation. */
class REX_RUNTIME_API BaseHeap {
 public:
  /** Destroys the heap without releasing committed pages, which Dispose does. */
  virtual ~BaseHeap();

  /**
   * Returns the heap offset relative to membase, without the host_address_offset adjustment.
   *
   * @return The offset in bytes.
   */
  u32 GetHeapBase() const { return heap_base_; }

  /**
   * Returns the length of the heap range.
   *
   * @return The length in bytes.
   */
  u32 GetHeapSize() const { return heap_size_; }

  /**
   * Returns the size of each page in the heap.
   *
   * @return The size in bytes.
   */
  u32 GetPageSize() const { return page_size_; }

  HeapType GetHeapType() const { return heap_type_; }

  /**
   * Returns the offset added to guest virtual addresses to convert them to host addresses, not
   * including membase.
   *
   * @return The offset.
   */
  u32 GetHostAddressOffset() const { return host_address_offset_; }

  /**
   * Translates an offset from the heap base to a host pointer.
   *
   * @tparam T The pointer type to return.
   * @param relative_address Offset in bytes from the heap base.
   * @return The host pointer.
   */
  template <typename T = u8*>
  T TranslateRelative(size_t relative_address) const {
    return reinterpret_cast<T>(membase_ + heap_base_ + host_address_offset_ + relative_address);
  }

  /** Disposes and decommits all memory and clears the page table. */
  virtual void Dispose();

  u32 GetTotalPageCount() const { return static_cast<u32>(page_table_.size()); }
  u32 GetUnreservedPageCount() const { return unreserved_page_count_; }
  u32 GetReservedPageCount() const { return GetTotalPageCount() - GetUnreservedPageCount(); }

  /**
   * Allocates pages with the given properties and allocation strategy. Can reserve and commit the
   * pages and set protection modes. Fails when not enough contiguous pages are found.
   *
   * @param size Size in bytes.
   * @param alignment Alignment in bytes.
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @param top_down True to allocate from the top of the heap.
   * @param out_address Receives the allocated guest address.
   * @return True on success.
   */
  virtual bool Alloc(u32 size, u32 alignment, u32 allocation_type, u32 protect, bool top_down,
                     u32* out_address);

  /**
   * Allocates pages at the given address. Can reserve and commit the pages and set protection
   * modes. Fails when the pages are allocated.
   *
   * @param base_address Guest address to allocate at.
   * @param size Size in bytes.
   * @param alignment Alignment in bytes.
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @return True on success.
   */
  virtual bool AllocFixed(u32 base_address, u32 size, u32 alignment, u32 allocation_type,
                          u32 protect);

  /**
   * Allocates pages at an address within the given address range. Can reserve and commit the pages
   * and set protection modes. Fails when not enough contiguous pages are found.
   *
   * @param low_address Lowest acceptable guest address.
   * @param high_address Highest acceptable guest address.
   * @param size Size in bytes.
   * @param alignment Alignment in bytes.
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @param top_down True to allocate from the top of the range.
   * @param out_address Receives the allocated guest address.
   * @return True on success.
   */
  virtual bool AllocRange(u32 low_address, u32 high_address, u32 size, u32 alignment,
                          u32 allocation_type, u32 protect, bool top_down, u32* out_address);

  /**
   * Allocates from the system portion of the heap, the top of virtual heaps. Physical heaps
   * delegate to Alloc as they have no split.
   *
   * @param size Size in bytes.
   * @param alignment Alignment in bytes.
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @param top_down True to allocate from the top of the heap.
   * @param out_address Receives the allocated guest address.
   * @return True on success.
   */
  virtual bool AllocSystemHeap(u32 size, u32 alignment, u32 allocation_type, u32 protect,
                               bool top_down, u32* out_address);

  /**
   * Decommits pages in the given range. Partially overlapped pages are decommitted.
   *
   * @param address Guest address of the range.
   * @param size Size in bytes.
   * @return True on success.
   */
  virtual bool Decommit(u32 address, u32 size);

  /**
   * Decommits and releases pages in the given range. Partially overlapped pages are released.
   *
   * @param address Guest address in the region.
   * @param out_region_size Receives the size of the released region.
   * @return True on success.
   */
  virtual bool Release(u32 address, u32* out_region_size = nullptr);

  /**
   * Modifies the protection mode of pages within the given range.
   *
   * @param address Guest address of the range.
   * @param size Size in bytes.
   * @param protect MemoryProtectFlag bits.
   * @param old_protect Receives the previous protection.
   * @return True on success.
   */
  virtual bool Protect(u32 address, u32 size, u32 protect, u32* old_protect = nullptr);

  /**
   * Returns information about the given region of pages.
   *
   * @param base_address Guest address in the region.
   * @param out_info Receives the region information.
   * @return True on success.
   */
  bool QueryRegionInfo(u32 base_address, HeapAllocationInfo* out_info);

  /**
   * Returns the size of the region containing the given address.
   *
   * @param address Guest address in the region.
   * @param out_size Receives the size in bytes.
   * @return True on success.
   */
  bool QuerySize(u32 address, u32* out_size);

  /**
   * Returns the current protection mode of the region containing the given address.
   *
   * @param address Guest address in the region.
   * @param out_protect Receives the MemoryProtectFlag bits.
   * @return True on success.
   */
  bool QueryProtect(u32 address, u32* out_protect);

  /**
   * Returns the strictest readability and writability for the entire range.
   *
   * @param low_address Lowest guest address of the range.
   * @param high_address Highest guest address of the range.
   * @return The page access.
   */
  rex::memory::PageAccess QueryRangeAccess(u32 low_address, u32 high_address);

 protected:
  friend class PhysicalHeap;

  /** Constructs an uninitialized heap. */
  BaseHeap();

  /**
   * Reconciles host page access with guest page protection. The caller holds the memory watch lock
   * when guest pages are smaller than host pages.
   *
   * @param start_page_number First guest page of the range.
   * @param end_page_number Last guest page of the range.
   * @return True when the host protection was updated.
   */
  bool SyncHostPageAccess(u32 start_page_number, u32 end_page_number);

  /**
   * Takes the memory watch lock for operations that reconcile host page access.
   *
   * @return The held lock, or an empty lock when guest pages are at least host page sized.
   */
  virtual std::unique_lock<rex::platform::RankedMutex> AcquireHostPageReconcileLock() const;

  /**
   * Allocates at a fixed address with the reconcile lock already held.
   *
   * @param base_address Guest address to allocate at.
   * @param size Size in bytes.
   * @param alignment Alignment in bytes.
   * @param allocation_type Memory allocation flags.
   * @param protect Memory protect flags.
   * @return True on success.
   */
  bool AllocFixedLocked(u32 base_address, u32 size, u32 alignment, u32 allocation_type,
                        u32 protect);

  /**
   * Allocates within an address range with the reconcile lock already held.
   *
   * @param low_address Lowest acceptable guest address.
   * @param high_address Highest acceptable guest address.
   * @param size Size in bytes.
   * @param alignment Alignment in bytes.
   * @param allocation_type Memory allocation flags.
   * @param protect Memory protect flags.
   * @param top_down True to allocate from the top of the range.
   * @param out_address Receives the allocated guest address.
   * @return True on success.
   */
  bool AllocRangeLocked(u32 low_address, u32 high_address, u32 size, u32 alignment,
                        u32 allocation_type, u32 protect, bool top_down, u32* out_address);

  /**
   * Decommits with the reconcile lock already held.
   *
   * @param address Guest address of the range.
   * @param size Size in bytes.
   * @return True on success.
   */
  bool DecommitLocked(u32 address, u32 size);

  /**
   * Releases with the reconcile lock already held.
   *
   * @param address Guest base address of the region.
   * @param out_region_size Receives the size of the released region.
   * @return True on success.
   */
  bool ReleaseLocked(u32 address, u32* out_region_size);

  /**
   * Changes protection with the reconcile lock already held.
   *
   * @param address Guest address of the range.
   * @param size Size in bytes.
   * @param protect Memory protect flags.
   * @param old_protect Receives the previous protection.
   * @return True on success.
   */
  bool ProtectLocked(u32 address, u32 size, u32 protect, u32* old_protect);

  /**
   * Reports whether a host page is write watched for guest invalidation.
   *
   * SyncHostPageAccess must not restore write access to such a page. The watch is stored in a flag
   * that EnableAccessCallbacks checks before re-applying protection, so unprotecting here would
   * disable the watch permanently. Only PhysicalHeap tracks this, every other heap has no watch
   * state.
   *
   * @param host_page_number The host page number.
   * @return True when the page is watched.
   */
  virtual bool IsHostPageWriteWatched([[maybe_unused]] u32 host_page_number) const { return false; }

  /**
   * Initializes the heap properties and allocates the page table.
   *
   * @param memory The owning memory system.
   * @param membase The host base of guest memory.
   * @param heap_type The heap type.
   * @param heap_base The heap offset relative to membase.
   * @param heap_size The heap length in bytes.
   * @param page_size The page size in bytes.
   * @param host_address_offset The offset added to guest addresses to give host addresses.
   */
  void Initialize(runtime::Memory* memory, u8* membase, HeapType heap_type, u32 heap_base,
                  u32 heap_size, u32 page_size, u32 host_address_offset = 0);

  runtime::Memory* memory_; /**< Memory system that owns the heap, source of the host page size. */
  u8* membase_;             /**< Host address of the start of guest memory. */
  HeapType heap_type_;      /**< Heap type given to Initialize. */
  u32 heap_base_;           /**< Guest address of the first byte of the heap. */
  u32 heap_size_;           /**< Length of the heap in bytes. */
  u32 page_size_;           /**< Size of 1 heap page in bytes. */
  /** Base 2 logarithm of page_size_, shifts a heap offset into a page number. */
  u32 page_size_shift_ = 0;
  /** Offset added to guest addresses to give host addresses, given to Initialize. */
  u32 host_address_offset_;
  /** Number of pages not reserved, falls when pages are reserved and rises when released. */
  u32 unreserved_page_count_ = 0;
  std::recursive_mutex heap_mutex_;   /**< Guards the page table and the counters of the heap. */
  std::vector<PageEntry> page_table_; /**< 1 entry for each heap page. */
};

/** Normal heap allowing allocations from guest virtual address ranges. */
class REX_RUNTIME_API VirtualHeap : public BaseHeap {
 public:
  /** Constructs an uninitialized heap. */
  VirtualHeap();
  /** Destroys the heap. */
  ~VirtualHeap() override;

  /**
   * Initializes the heap properties and allocates the page table.
   *
   * @param memory The owning memory system.
   * @param membase The host base of guest memory.
   * @param heap_type The heap type.
   * @param heap_base The heap offset relative to membase.
   * @param heap_size The heap length in bytes.
   * @param page_size The page size in bytes.
   */
  void Initialize(runtime::Memory* memory, u8* membase, HeapType heap_type, u32 heap_base,
                  u32 heap_size, u32 page_size);
};

/**
 * A heap for ranges of memory mapped to physical ranges, used by the audio and graphics subsystems
 * for hardware wired to memory.
 *
 * Sharing pages with virtual pages works through a parent heap that performs allocation in the
 * guest virtual address space 1:1 with the physical address space.
 */
class REX_RUNTIME_API PhysicalHeap : public BaseHeap {
 public:
  /** Constructs an uninitialized heap. */
  PhysicalHeap();
  /** Destroys the heap. */
  ~PhysicalHeap() override;

  /**
   * Initializes the heap properties and allocates the page table.
   *
   * @param memory The owning memory system.
   * @param membase The host base of guest memory.
   * @param heap_type The heap type.
   * @param heap_base The heap offset relative to membase.
   * @param heap_size The heap length in bytes.
   * @param page_size The page size in bytes.
   * @param parent_heap The heap that performs allocation in the guest virtual address space.
   */
  void Initialize(runtime::Memory* memory, u8* membase, HeapType heap_type, u32 heap_base,
                  u32 heap_size, u32 page_size, VirtualHeap* parent_heap);

  /**
   * Allocates pages in the parent heap and maps them at the matching address of this heap.
   *
   * @param size Size in bytes.
   * @param alignment Alignment in bytes.
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @param top_down Ignored, allocation always runs from the top of the heap.
   * @param out_address Receives the allocated guest address.
   * @return True on success.
   */
  bool Alloc(u32 size, u32 alignment, u32 allocation_type, u32 protect, bool top_down,
             u32* out_address) override;
  /**
   * Allocates pages at the given address in the parent heap and in this heap.
   *
   * @param base_address Guest address to allocate at.
   * @param size Size in bytes.
   * @param alignment Alignment in bytes.
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @return True on success.
   */
  bool AllocFixed(u32 base_address, u32 size, u32 alignment, u32 allocation_type,
                  u32 protect) override;
  /**
   * Allocates pages within an address range in the parent heap and in this heap.
   *
   * @param low_address Lowest acceptable guest address.
   * @param high_address Highest acceptable guest address.
   * @param size Size in bytes.
   * @param alignment Alignment in bytes.
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @param top_down True to allocate from the top of the range.
   * @param out_address Receives the allocated guest address.
   * @return True on success.
   */
  bool AllocRange(u32 low_address, u32 high_address, u32 size, u32 alignment, u32 allocation_type,
                  u32 protect, bool top_down, u32* out_address) override;
  /**
   * Allocates with Alloc, as physical heaps have no system portion.
   *
   * @param size Size in bytes.
   * @param alignment Alignment in bytes.
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @param top_down Ignored, allocation always runs from the top of the heap.
   * @param out_address Receives the allocated guest address.
   * @return True on success.
   */
  bool AllocSystemHeap(u32 size, u32 alignment, u32 allocation_type, u32 protect, bool top_down,
                       u32* out_address) override;
  /**
   * Decommits pages in the parent heap and in this heap, invalidating watched pages.
   *
   * @param address Guest address of the range.
   * @param size Size in bytes.
   * @return True on success.
   */
  bool Decommit(u32 address, u32 size) override;
  /**
   * Releases the region in the parent heap and in this heap, invalidating watched pages.
   *
   * @param base_address Guest address in the region.
   * @param out_region_size Receives the size of the released region.
   * @return True on success.
   */
  bool Release(u32 base_address, u32* out_region_size = nullptr) override;
  /**
   * Changes protection in the parent heap and in this heap.
   * Watched pages are invalidated when the range becomes writable.
   *
   * @param address Guest address of the range.
   * @param size Size in bytes.
   * @param protect MemoryProtectFlag bits.
   * @param old_protect Receives the previous protection.
   * @return True on success.
   */
  bool Protect(u32 address, u32 size, u32 protect, u32* old_protect = nullptr) override;

  /**
   * Enables write invalidation notifications for the system pages of a physical range.
   *
   * @param physical_address Start of the physical range.
   * @param length Size of the range in bytes.
   * @param enable_invalidation_notifications True to notify when the title writes the range.
   * @param enable_data_providers True to enable data providers, which are unsupported.
   */
  void EnableAccessCallbacks(u32 physical_address, u32 length,
                             bool enable_invalidation_notifications, bool enable_data_providers);
  /**
   * Triggers the physical memory access callbacks of the watched pages in a range.
   *
   * @param virtual_address Guest address of the range.
   * @param length Length in bytes.
   * @param is_write True for a write access.
   * @param unwatch_exact_range True to unwatch only the requested range.
   * @param unprotect True to restore host write access.
   * @return True when any page in the range was watched.
   */
  bool TriggerCallbacks(u32 virtual_address, u32 length, bool is_write, bool unwatch_exact_range,
                        bool unprotect = true);

  /**
   * TriggerCallbacks with the memory watch lock already held.
   *
   * @param virtual_address Guest address of the range.
   * @param length Length in bytes.
   * @param is_write True for a write access.
   * @param unwatch_exact_range True to unwatch only the requested range.
   * @param unprotect True to restore host write access.
   * @return True when any page in the range was watched.
   */
  bool TriggerCallbacksLocked(u32 virtual_address, u32 length, bool is_write,
                              bool unwatch_exact_range, bool unprotect = true);

  /**
   * Translates a guest address in this heap to its physical address.
   *
   * @param address Guest address in this heap.
   * @return The physical address, offset by 0x1000 in the 0xE0000000 view.
   */
  u32 GetPhysicalAddress(u32 address) const;

 protected:
  /**
   * Reports whether a host page is write watched for guest invalidation.
   *
   * @param host_page_number The host page number.
   * @return True when the page is watched.
   */
  bool IsHostPageWriteWatched(u32 host_page_number) const override;

  /**
   * Takes the memory watch lock when this heap or its parent has
   * guest pages smaller than host pages.
   *
   * @return The held lock, or an empty lock when neither does.
   */
  std::unique_lock<rex::platform::RankedMutex> AcquireHostPageReconcileLock() const override;

  VirtualHeap* parent_heap_; /**< Virtual heap whose pages back this physical heap. */

  u32 system_page_size_; /**< Host page size in bytes. */
  /** Number of host pages that cover the heap including host_address_offset_. */
  u32 system_page_count_;

  /** Watch flags for 64 system pages. */
  struct SystemPageFlagsBlock {
    /** Set bits mark the pages whose writes trigger invalidation callbacks. */
    u64 notify_on_invalidation;
  };
  /**
   * Guarded by Memory::watch_mutex(). Flags for each 64 system pages, interleaved as blocks for bit
   * scans.
   */
  std::vector<SystemPageFlagsBlock> system_page_flags_;
};

/** Outcome of a host access violation inside guest memory. */
enum class AccessViolationResult : u8 {
  /** The write watch resolved the fault, so the access runs again. */
  kResumed,
  /** The access reached a register block that generated code did not route. */
  kMMIOMiss,
  /** Memory makes no claim on the fault. */
  kUnhandled,
};

/**
 * Models the guest memory system. Exposes virtual and physical memory with a TLB and a page table
 * for allocation, mapping and protection.
 *
 * The memory is backed by a memory mapped file at a stable fixed address in the host address space,
 * which gives cheap guest to host address translation and sharing across subsystems.
 *
 * The guest address space is split into ranges with their own page sizes, caching strategies,
 * protections and overlaps. Each range is a BaseHeap, either a VirtualHeap or a PhysicalHeap by
 * type, which models the page table and handles reserving and committing pages.
 */
class REX_RUNTIME_API Memory {
 public:
  /** Constructs the memory system and registers it as the active instance. */
  Memory();
  /** Disposes the heaps, unmaps the views and unregisters the memory system. */
  ~Memory();

  /**
   * Initializes the memory system.
   *
   * @return False when the host address space cannot be reserved or the file mapping fails.
   */
  bool Initialize();

  /**
   * Returns the file name and path of the memory mapped file backing all memory.
   *
   * @return The path.
   */
  const std::filesystem::path& GetFileName() const { return file_name_; }

  /**
   * Returns the base address of virtual memory in the host address space.
   *
   * @return The host base.
   */
  u8* GetVirtualMembase() const { return virtual_membase_; }

  /**
   * Translates a guest virtual address to a host address, with the arithmetic recompiled code
   * uses. The contents at the host address are big endian.
   *
   * @tparam T The pointer type to return.
   * @param guest_address The guest virtual address.
   * @return The host address.
   */
  template <typename T = u8*>
  T TranslateVirtual(u32 guest_address) const {
    assert(!LookupHeap(guest_address) || LookupHeap(guest_address)->GetHostAddressOffset() ==
                                             detail::PhysicalHostOffset(guest_address));
    return GuestPtr<T>(virtual_membase_, guest_address);
  }

  /**
   * Returns the base address of physical memory in the host address space.
   *
   * @return The host base.
   */
  u8* GetPhysicalMembase() const { return physical_membase_; }

  /**
   * Translates a guest physical address to a host address that can be accessed as a normal pointer.
   * The contents at the host address are big endian.
   *
   * @tparam T The pointer type to return.
   * @param guest_address The guest physical address.
   * @return The host address.
   */
  template <typename T = u8*>
  T TranslatePhysical(u32 guest_address) const {
    return reinterpret_cast<T>(physical_membase_ + (guest_address & 0x1FFFFFFF));
  }

  /**
   * Translates a host address to a guest virtual address. The contents at the host address are big
   * endian.
   *
   * @param host_address The host address.
   * @return The guest virtual address.
   */
  u32 HostToGuestVirtual(const void* host_address) const;

  /**
   * Returns the guest physical address for a guest virtual address.
   *
   * @param address The guest virtual address.
   * @return The guest physical address, or UINT32_MAX when it cannot be obtained.
   */
  u32 GetPhysicalAddress(u32 address) const;

  /**
   * Zeros a range of memory at the given guest address.
   *
   * @param address The guest address.
   * @param size Size in bytes.
   */
  void Zero(u32 address, u32 size) const;

  /**
   * Fills a range of guest memory with a byte value.
   *
   * @param address The guest address.
   * @param size Size in bytes.
   * @param value The byte value.
   */
  void Fill(u32 address, u32 size, u8 value) const;

  /**
   * Copies a non overlapping range of guest memory, like memcpy.
   *
   * @param dest The destination guest address.
   * @param src The source guest address.
   * @param size Size in bytes.
   */
  void Copy(u32 dest, u32 src, u32 size) const;

  /**
   * Resolves a host access violation inside guest memory.
   *
   * @param host_address The host address the access faulted on.
   * @param is_write True when the access was a write.
   * @return MMIOMiss for a register block, Resumed when the write watch resolved the fault,
   *         otherwise Unhandled.
   */
  AccessViolationResult HandleAccessViolation(void* host_address, bool is_write);

  /**
   * Callback for physical memory invalidation notifications.
   *
   * Physical memory access callbacks use per system page protection without reference counting or
   * stored ranges. An access to a watched page triggers all callbacks for it. Callbacks can only be
   * removed by triggering them, and may fire spuriously or more than once, so handlers ignore pages
   * they do not track. After a trigger the page is unwatched, in 1 guest view of physical memory
   * only, until requested again. Only accesses through the guest virtual views of physical memory
   * trigger callbacks, so data providers write through the host physical heap or their threads
   * may wait on themselves.
   *
   * Invalidation notifications are one shot callbacks for invalidating physical memory caches such
   * as the GPU copy. They fire for a single page on a write access violation or when data providers
   * synchronize, and for many pages when memory is released or when host code triggers them,
   * as when accessing a file. The granularity is 1 page, so a handler invalidates all
   * data stored in the touched pages. Large ranges such as framebuffers are costly to fault on, so
   * a write fault unprotects the shortest common range returned by all invalidation callbacks,
   * clamped to a sane size and excluding pages with provider callbacks.
   *
   * @param context_ptr The context given at registration.
   * @param physical_address_start The start of the watched region.
   * @param length The length of the watched region in bytes.
   * @param exact_range True when only the exact range is being unwatched.
   * @return The start and length of the smallest physical memory region around the watched region
   * that can be safely unwatched, or (0, UINT32_MAX) when it does not matter.
   */
  using PhysicalMemoryInvalidationCallback = std::pair<u32, u32> (*)(void* context_ptr,
                                                                     u32 physical_address_start,
                                                                     u32 length, bool exact_range);
  /**
   * Registers a physical memory invalidation callback.
   *
   * @param callback The callback.
   * @param callback_context The context passed to the callback.
   * @return A handle for unregistering.
   */
  void* RegisterPhysicalMemoryInvalidationCallback(PhysicalMemoryInvalidationCallback callback,
                                                   void* callback_context);
  /**
   * Unregisters a callback added with RegisterPhysicalMemoryInvalidationCallback.
   *
   * @param callback_handle The handle returned at registration.
   */
  void UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle);

  /**
   * Enables physical memory access callbacks for a memory range, snapped to system page boundaries.
   *
   * @param physical_address The guest physical address of the range.
   * @param length Length in bytes.
   * @param enable_invalidation_notifications True to enable invalidation notifications.
   * @param enable_data_providers True to enable data providers.
   */
  void EnablePhysicalMemoryAccessCallbacks(u32 physical_address, u32 length,
                                           bool enable_invalidation_notifications,
                                           bool enable_data_providers);

  /**
   * Returns the lock that guards the physical memory watch state of the heaps and of every
   * registered invalidation callback client. It is a Leaf lock held while callbacks run, so
   * callbacks take no other ranked lock.
   *
   * @return The watch lock.
   */
  rex::platform::RankedMutex& GetWatchMutex() { return watch_mutex_; }

  /**
   * Allocates virtual memory from the system heap. System memory is kept separate from title memory
   * but is reachable with normal guest virtual addresses. Kernel structures and other internal
   * allocations come from this heap when possible.
   *
   * @param size Size in bytes.
   * @param alignment Alignment in bytes.
   * @param system_heap_flags SystemHeapFlag bits.
   * @return The guest address of the allocation.
   */
  u32 SystemHeapAlloc(u32 size, u32 alignment = 0x20, u32 system_heap_flags = kSystemHeapDefault);

  /**
   * Frees memory allocated with SystemHeapAlloc.
   *
   * @param address Guest address of the allocation.
   * @param out_region_size Receives the size of the freed region.
   */
  void SystemHeapFree(u32 address, u32* out_region_size = nullptr);

  /**
   * Returns the heap for the address space containing the given address.
   *
   * @param address The guest address.
   * @return The heap.
   */
  const BaseHeap* LookupHeap(u32 address) const;

  /**
   * Returns the heap for the address space containing the given address.
   *
   * @param address The guest address.
   * @return The heap.
   */
  BaseHeap* LookupHeap(u32 address) {
    return const_cast<BaseHeap*>(const_cast<const runtime::Memory*>(this)->LookupHeap(address));
  }

  /**
   * Returns the heap with the given properties.
   *
   * @param physical True for a physical heap.
   * @param page_size The page size in bytes.
   * @return The heap.
   */
  BaseHeap* LookupHeapByType(bool physical, u32 page_size);

  /**
   * Aggregates page statistics across the given heaps.
   *
   * @param provided_heaps The heaps.
   * @param heaps_count The number of heaps.
   * @param unreserved_pages Receives the unreserved page count.
   * @param reserved_pages Receives the reserved page count.
   * @param used_pages Receives the used page count.
   * @param reserved_bytes Receives the reserved size in bytes.
   */
  void GetHeapsPageStatsSummary(const BaseHeap* const* provided_heaps, size_t heaps_count,
                                u32& unreserved_pages, u32& reserved_pages, u32& used_pages,
                                u32& reserved_bytes);

  /**
   * Returns the physical base heap.
   *
   * @return The heap.
   */
  VirtualHeap* GetPhysicalHeap();

  /**
   * Sets the title memory budget.
   *
   * @param megabytes Budget in MiB, 0 for no cap.
   */
  void SetBudget(u32 megabytes);

  /**
   * Returns the title pages counted against the budget.
   *
   * @return The count in 4 KB units.
   */
  [[nodiscard]] u32 GetTitlePages() const;

  /**
   * Returns the title memory budget.
   *
   * @return The budget in 4 KB units, 0 when uncapped.
   */
  [[nodiscard]] u32 GetBudgetPages() const;

  /**
   * Initializes the per module function dispatch table at IMAGE_BASE + IMAGE_SIZE, indexed by
   * (guest_addr - code_base) * 2. The function table calls are internally locked and may be made
   * from any thread.
   *
   * @param code_base The first guest address of the code.
   * @param code_size The code size in bytes.
   * @param image_base The guest address of the image.
   * @param image_size The image size in bytes.
   * @return True on success.
   */
  bool InitializeFunctionTable(u32 code_base, u32 code_size, u32 image_base, u32 image_size);
  /**
   * Removes the function table of a module and releases its pages.
   *
   * @param code_base The first guest address of the code, as given to InitializeFunctionTable.
   * @return False when no table is registered for code_base.
   */
  bool DestroyFunctionTable(u32 code_base);
  /**
   * Sets the host function for a guest address.
   *
   * @param guest_address The guest address.
   * @param host_function The host function.
   * @return False when guest_address is outside every registered module range.
   */
  bool SetFunction(u32 guest_address, PPCFunc* host_function);

 private:
#if REX_PLATFORM_MAC
  int MapViewsMac();
#endif
  int MapViews(u8* mapping_base);
  void UnmapViews();
  bool ReserveMMIOBlocks() const;

  bool AccessViolationCallback(void* host_address, bool is_write);

  /**
   * Counts pages against the budget.
   *
   * @param pages Count in 4 KB units.
   * @return True when the budget had room.
   */
  bool AllocateTitlePage(u32 pages);

  /**
   * Returns pages to the budget.
   *
   * @param pages Count in 4 KB units.
   */
  void ReleaseTitlePage(u32 pages);

  std::filesystem::path file_name_;
  u32 system_page_size_ = 0;
  u32 system_allocation_granularity_ = 0;
  u8* virtual_membase_ = nullptr;
  u8* physical_membase_ = nullptr;

  struct FunctionTableEntry {
    u32 table_base;
    u32 code_base;
    u32 code_size;
    u32 thunk_reserve;
  };
  std::vector<FunctionTableEntry> function_tables_;
  mutable std::mutex function_tables_mutex_;

  rex::memory::FileMappingHandle mapping_ = rex::memory::kFileMappingHandleInvalid;
  u8* mapping_base_ = nullptr;
  union {
    struct {
      u8* v00000000;
      u8* v40000000;
      u8* v7f000000;
      u8* v80000000;
      u8* v90000000;
      u8* va0000000;
      u8* vc0000000;
      u8* ve0000000;
      u8* physical;
    };
    u8* all_views[9];
  } views_ = {{0, 0, 0, 0, 0, 0, 0, 0, 0}};

  struct {
    VirtualHeap v00000000;
    VirtualHeap v40000000;
    VirtualHeap v80000000;
    VirtualHeap v90000000;

    VirtualHeap physical;
    PhysicalHeap va0000000;
    PhysicalHeap vc0000000;
    PhysicalHeap ve0000000;
  } heaps_;

  friend class BaseHeap;

  friend class PhysicalHeap;
  rex::platform::RankedMutex watch_mutex_{rex::platform::LockRank::kLeaf};
  std::vector<std::unique_ptr<std::pair<PhysicalMemoryInvalidationCallback, void*>>>
      physical_memory_invalidation_callbacks_;
  std::atomic<u32> title_pages_{0};
  std::atomic<u32> budget_pages_{0};
};

}  // namespace rex::runtime
