/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/heap.h
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
#include <type_traits>
#include <utility>
#include <vector>

#include <rex/platform.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/memory/virtual_memory.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/platform/lock_order.h>
#include <rex/types.h>

namespace rex::kernel {

inline constexpr u32 kPageSize4KiB = 4 * 1024;

/** The large page size and title allocation granularity, in bytes. */
inline constexpr u32 kPageSize64KiB = 64 * 1024;

inline constexpr u32 kPageSize16MiB = 16 * 1024 * 1024;

}  // namespace rex::kernel

namespace rex::kernel::detail {

/**
 * The alignment a guest pointer to T must have, 1 for void and function types.
 *
 * @tparam IsOpaque True when T has no alignment.
 */
template <typename T, bool IsOpaque = std::is_void_v<T> || std::is_function_v<T>>
struct PointeeAlignment {
  static constexpr u32 kValue = 1;
};

/** The alignment a guest pointer to T must have. */
template <typename T>
struct PointeeAlignment<T, false> {
  static constexpr u32 kValue = static_cast<u32>(alignof(T));
};

}  // namespace rex::kernel::detail

namespace rex::kernel {

class GuestAllocator;
class GuestAddressSpace;

/** Selects the heap that SystemHeapAlloc takes memory from. */
enum SystemHeapFlag : u8 {
  kSystemHeapVirtual = 1 << 0,
  kSystemHeapPhysical = 1 << 1,

  kSystemHeapDefault = kSystemHeapVirtual,
};

/** The system heap interface XAM allocates from. */
class ISystemHeap {
 public:
  virtual ~ISystemHeap() = default;

  /**
   * Allocates memory for the system.
   *
   * @param system_heap_flags SystemHeapFlag bits.
   */
  virtual u32 SystemHeapAlloc(u32 size, u32 alignment = 0x20,
                              u32 system_heap_flags = kSystemHeapDefault) = 0;

  /**
   * Frees memory allocated with SystemHeapAlloc.
   *
   * @param out_region_size Receives the size of the freed region.
   */
  virtual void SystemHeapFree(u32 address, u32* out_region_size = nullptr) = 0;

  [[nodiscard]] virtual bool IsAddressMapped(u32 address) const = 0;
};

enum class HeapType : u8 {
  kGuestVirtual,
  kGuestXEX, /**< Memory that executable images are mapped into. */
  kGuestPhysical,
};

enum MemoryAllocationFlag : u8 {
  kMemoryAllocationReserve = 1 << 0,
  kMemoryAllocationCommit = 1 << 1,
  /** Counts the newly committed pages against the title memory budget. */
  kMemoryAllocationTitle = 1 << 2,
};

enum MemoryProtectFlag : u8 {
  kMemoryProtectRead = 1 << 0,
  kMemoryProtectWrite = 1 << 1,
  kMemoryProtectNoCache = 1 << 2,
  kMemoryProtectWriteCombine = 1 << 3,

  kMemoryProtectNoAccess = 0,
};

/** Equivalent to the Win32 MEMORY_BASIC_INFORMATION struct. */
struct HeapAllocation {
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

union PageEntry {
  u64 qword; /**< The whole entry read or written as one 64 bit value. */
  struct {
    /** Base page of the allocated region in pages of the heap's page size. */
    u32 base_address : 20;
    /** Page count of the allocated region in pages of the heap's page size. */
    u32 region_page_count : 20;
    /** MemoryProtectFlag bits given at allocation. */
    u32 allocation_protect : 4;
    /** MemoryProtectFlag bits as of the last Protect. */
    u32 current_protect : 4;
    /** MemoryAllocationFlag bit mask of the page state. */
    u32 state : 2;
    /** Set on a committed page that counts against the title memory budget. */
    u32 title : 1;
    u32 reserved : 13;
  };
};

class REX_KERNEL_API BaseHeap {
 public:
  /** Does not release committed pages, which Dispose does. */
  virtual ~BaseHeap();

  /** Returns the heap offset relative to membase, without the host_address_offset adjustment. */
  u32 GetHeapBase() const { return heap_base_; }

  u32 GetHeapSize() const { return heap_size_; }

  u32 GetPageSize() const { return page_size_; }

  HeapType GetHeapType() const { return heap_type_; }

  /**
   * Returns the offset added to guest addresses to get host addresses, excluding membase.
   */
  u32 GetHostAddressOffset() const { return host_address_offset_; }

  /** Translates an offset from the heap base to a host pointer. */
  template <typename T = u8*>
  T TranslateRelative(size_t relative_address) const {
    return reinterpret_cast<T>(membase_ + heap_base_ + host_address_offset_ + relative_address);
  }

  /** Disposes and decommits all memory and clears the page table. */
  virtual void Dispose();

  u32 GetTotalPageCount() const { return static_cast<u32>(page_table_.size()); }

  u32 GetUnreservedPageCount() const { return unreserved_page_count_; }

  u32 GetReservedPageCount() const { return GetTotalPageCount() - GetUnreservedPageCount(); }

  u32 GetCommittedPages() const { return committed_pages_.load(std::memory_order_relaxed); }

  /**
   * Plots the committed bytes on the profiler graph. Does nothing for a heap that views another.
   */
  void PlotProfile() const;

  /**
   * Allocates pages anywhere in the heap. Fails when not enough contiguous pages are found.
   *
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @param top_down True to allocate from the top of the heap.
   * @param out_address Receives the allocated guest address.
   */
  virtual bool Alloc(u32 size, u32 alignment, u32 allocation_type, u32 protect, bool top_down,
                     u32* out_address);

  /**
   * Allocates pages at an address. Fails when the pages are allocated.
   *
   * @param base_address Guest address to allocate at.
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   */
  virtual bool AllocFixed(u32 base_address, u32 size, u32 alignment, u32 allocation_type,
                          u32 protect);

  /**
   * Allocates pages within an address range. Fails when not enough contiguous pages are found.
   *
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @param top_down True to allocate from the top of the range.
   * @param out_address Receives the allocated guest address.
   */
  virtual bool AllocRange(u32 low_address, u32 high_address, u32 size, u32 alignment,
                          u32 allocation_type, u32 protect, bool top_down, u32* out_address);

  /**
   * Allocates from the system portion, the top of a virtual heap. A physical heap uses Alloc.
   *
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @param top_down True to allocate from the top of the heap.
   * @param out_address Receives the allocated guest address.
   */
  virtual bool AllocSystemHeap(u32 size, u32 alignment, u32 allocation_type, u32 protect,
                               bool top_down, u32* out_address);

  /** Decommits pages in the given range. Partially overlapped pages are decommitted. */
  virtual bool Decommit(u32 address, u32 size);

  /**
   * Decommits and releases pages in the given range. Partially overlapped pages are released.
   *
   * @param out_region_size Receives the size of the released region.
   */
  virtual bool Release(u32 address, u32* out_region_size = nullptr);

  /**
   * Releases the pages of a range inside one region, trimming or splitting it. Partially
   * overlapped pages are released. Only for a heap whose regions need no parent heap.
   *
   * @param size Size of the range in bytes, not 0.
   * @param out_address Receives the guest address of the first released page.
   * @param out_size Receives the size of the released pages in bytes.
   * @return True on success, false when the range is not all inside one allocated region.
   */
  bool ReleaseRange(u32 address, u32 size, u32* out_address, u32* out_size);

  /**
   * Modifies the protection mode of pages within the given range.
   *
   * @param protect MemoryProtectFlag bits.
   * @param old_protect Receives the previous protection.
   */
  virtual bool Protect(u32 address, u32 size, u32 protect, u32* old_protect = nullptr);

  /**
   * Returns information about the given region of pages.
   *
   * @param out_info Receives the region information.
   */
  bool QueryRegionInfo(u32 base_address, HeapAllocation* out_info);

  /**
   * Returns the size of the region containing the given address.
   *
   * @param out_size Receives the size in bytes.
   */
  bool QuerySize(u32 address, u32* out_size);

  /**
   * Returns the current protection mode of the region containing the given address.
   *
   * @param out_protect Receives the MemoryProtectFlag bits.
   */
  bool QueryProtect(u32 address, u32* out_protect);

  /** Returns the strictest readability and writability for the range. */
  rex::memory::PageAccess QueryRangeAccess(u32 low_address, u32 high_address);

  /**
   * Returns the host access for a host page, the union of the committed title protection of its
   * guest pages. A write watched page stays read only. The caller holds the heap mutex and, when
   * guest pages are smaller than host pages, the memory watch lock.
   *
   * @param host_page_number Host page number from the heap host base, with GetHostAddressOffset().
   */
  rex::memory::PageAccess GetHostPageAccess(u32 host_page_number) const;

  /**
   * Reapplies the host access the heap grants to a faulting host page, under the heap mutex. The
   * caller holds the memory watch lock.
   *
   * @param is_write True for a write, false for a read.
   * @return True when the heap allows the access and the host page now permits it.
   */
  bool ReapplyHostPageAccess(void* host_address, bool is_write);

 protected:
  friend class PhysicalHeap;

  BaseHeap();

  /**
   * Returns the union of the committed guest protection of the guest pages in a host page, ignoring
   * write watches. The caller holds the same locks as for GetHostPageAccess.
   *
   * @param host_page_number Host page number from the heap host base, with GetHostAddressOffset().
   * @return The host access the guest protection allows.
   */
  rex::memory::PageAccess GetHostPageGuestAccess(u32 host_page_number) const;

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
   * Returns the first address of the range reserved for system allocations, the end of the heap for
   * a heap without a reserved range. Title allocations end one byte below it.
   */
  u32 GetSystemRangeBase() const;

  bool AllocFixedLocked(u32 base_address, u32 size, u32 alignment, u32 allocation_type,
                        u32 protect);

  bool AllocRangeLocked(u32 low_address, u32 high_address, u32 size, u32 alignment,
                        u32 allocation_type, u32 protect, bool top_down, u32* out_address);

  bool DecommitLocked(u32 address, u32 size);

  bool ReleaseLocked(u32 address, u32* out_region_size);

  bool ReleaseRangeLocked(u32 address, u32 size, u32* out_address, u32* out_size);

  bool ProtectLocked(u32 address, u32 size, u32 protect, u32* old_protect);

  /**
   * Checks that a range can be protected: it is not empty, lies inside the heap, stays in one
   * region and is committed. The caller holds heap_mutex_.
   *
   * @param address The first address of the range.
   * @param size The size of the range in bytes.
   * @param out_start_page The page number holding the first byte.
   * @param out_end_page The page number holding the last byte.
   * @return True when the range can be protected.
   */
  bool CheckProtectRangeLocked(u32 address, u32 size, u32* out_start_page, u32* out_end_page);

  /**
   * Reports whether a host page is write watched, so SyncHostPageAccess must not restore write
   * access to it. Only PhysicalHeap tracks this.
   *
   * @return True when the page is watched.
   */
  virtual bool IsHostPageWriteWatched([[maybe_unused]] u32 host_page_number) const { return false; }

  /**
   * Counts a page as committed and reports it to the profiler memory pool of the heap.
   */
  void OnPageCommitted(u32 page_number);

  /**
   * Counts a page as decommitted and reports it to the profiler memory pool of the heap.
   */
  void OnPageDecommitted(u32 page_number);

  /**
   * Initializes the heap properties and allocates the page table.
   *
   * @param heap_base The heap offset relative to membase.
   * @param host_address_offset The offset added to guest addresses to give host addresses.
   */
  void Initialize(kernel::GuestAddressSpace* memory, u8* membase, HeapType heap_type, u32 heap_base,
                  u32 heap_size, u32 page_size, u32 host_address_offset = 0);

  kernel::GuestAddressSpace*
      memory_; /**< Guest address space that owns the heap, source of the host page size. */
  u8* membase_;
  HeapType heap_type_;
  u32 heap_base_;
  u32 heap_size_;
  u32 page_size_;
  /** Base 2 logarithm of page_size_, shifts a heap offset into a page number. */
  u32 page_size_shift_ = 0;
  /** Offset added to guest addresses to give host addresses, given to Initialize. */
  u32 host_address_offset_;
  u32 unreserved_page_count_ = 0;
  /** Number of committed pages, changed with the heap mutex held. */
  std::atomic<u32> committed_pages_{0};
  /** Profiler memory pool of the heap, null for a heap that views the memory of another heap. */
  const char* profile_pool_ = nullptr;
  const char* profile_plot_ = nullptr;
  std::recursive_mutex heap_mutex_; /**< Guards the page table and the counters of the heap. */
  std::vector<PageEntry> page_table_;
};

class REX_KERNEL_API VirtualHeap : public BaseHeap {
 public:
  VirtualHeap();
  ~VirtualHeap() override;

  /**
   * Initializes the heap properties and allocates the page table.
   *
   * @param heap_base The heap offset relative to membase.
   */
  void Initialize(kernel::GuestAddressSpace* memory, u8* membase, HeapType heap_type, u32 heap_base,
                  u32 heap_size, u32 page_size);
};

/**
 * A heap for memory mapped to physical ranges, used by hardware wired to memory. It allocates
 * through a parent heap in the guest virtual address space, 1:1 with the physical address space.
 */
class REX_KERNEL_API PhysicalHeap : public BaseHeap {
 public:
  PhysicalHeap();
  ~PhysicalHeap() override;

  /**
   * Initializes the heap properties and allocates the page table.
   *
   * @param heap_base The heap offset relative to membase.
   * @param parent_heap The heap that performs allocation in the guest virtual address space.
   */
  void Initialize(kernel::GuestAddressSpace* memory, u8* membase, HeapType heap_type, u32 heap_base,
                  u32 heap_size, u32 page_size, VirtualHeap* parent_heap);

  /**
   * Allocates pages in the parent heap and maps them at the matching address of this heap.
   *
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @param top_down Ignored, allocation always runs from the top of the heap.
   * @param out_address Receives the allocated guest address.
   */
  bool Alloc(u32 size, u32 alignment, u32 allocation_type, u32 protect, bool top_down,
             u32* out_address) override;
  /**
   * Allocates pages at the given address in the parent heap and in this heap.
   *
   * @param base_address Guest address to allocate at.
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   */
  bool AllocFixed(u32 base_address, u32 size, u32 alignment, u32 allocation_type,
                  u32 protect) override;
  /**
   * Allocates pages within an address range in the parent heap and in this heap.
   *
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @param top_down True to allocate from the top of the range.
   * @param out_address Receives the allocated guest address.
   */
  bool AllocRange(u32 low_address, u32 high_address, u32 size, u32 alignment, u32 allocation_type,
                  u32 protect, bool top_down, u32* out_address) override;
  /**
   * Allocates with Alloc, as physical heaps have no system portion.
   *
   * @param allocation_type MemoryAllocationFlag bits.
   * @param protect MemoryProtectFlag bits.
   * @param top_down Ignored, allocation always runs from the top of the heap.
   * @param out_address Receives the allocated guest address.
   */
  bool AllocSystemHeap(u32 size, u32 alignment, u32 allocation_type, u32 protect, bool top_down,
                       u32* out_address) override;
  /** Decommits pages in the parent heap and in this heap, invalidating watched pages. */
  bool Decommit(u32 address, u32 size) override;
  /**
   * Releases the region in the parent heap and in this heap, invalidating watched pages.
   *
   * @param out_region_size Receives the size of the released region.
   */
  bool Release(u32 base_address, u32* out_region_size = nullptr) override;
  /**
   * Changes protection in the parent heap and in this heap, invalidating watched pages when the
   * range becomes writable.
   *
   * @param protect MemoryProtectFlag bits.
   * @param old_protect Receives the previous protection.
   */
  bool Protect(u32 address, u32 size, u32 protect, u32* old_protect = nullptr) override;

  /**
   * Enables write invalidation notifications for the system pages of a physical range.
   *
   * @param enable_invalidation_notifications True to notify when the title writes the range.
   * @param enable_data_providers True to enable data providers, which are unsupported.
   */
  void EnableAccessCallbacks(u32 physical_address, u32 length,
                             bool enable_invalidation_notifications, bool enable_data_providers);
  /**
   * Triggers the physical memory access callbacks of the watched pages in a range.
   *
   * @param unwatch_exact_range True to unwatch only the requested range.
   * @param unprotect True to restore host write access.
   * @return True when any page in the range was watched.
   */
  bool TriggerCallbacks(u32 virtual_address, u32 length, bool is_write, bool unwatch_exact_range,
                        bool unprotect = true);

  bool TriggerCallbacksLocked(u32 virtual_address, u32 length, bool is_write,
                              bool unwatch_exact_range, bool unprotect = true);

  /**
   * Translates a guest address in this heap to its physical address.
   *
   * @return The physical address, offset by 0x1000 in the 0xE0000000 view.
   */
  u32 GetPhysicalAddress(u32 address) const;

 protected:
  bool IsHostPageWriteWatched(u32 host_page_number) const override;

  /**
   * Takes the memory watch lock when this heap or its parent has guest pages smaller than host
   * pages.
   *
   * @return The held lock, or an empty lock when neither does.
   */
  std::unique_lock<rex::platform::RankedMutex> AcquireHostPageReconcileLock() const override;

  VirtualHeap* parent_heap_; /**< Virtual heap whose pages back this physical heap. */

  u32 system_page_size_;
  /** Number of host pages that cover the heap including host_address_offset_. */
  u32 system_page_count_;

  /** Watch flags for 64 system pages. */
  struct SystemPageFlagsBlock {
    /** Set bits mark the pages whose writes trigger invalidation callbacks. */
    u64 notify_on_invalidation;
  };
  /** Guarded by GuestAddressSpace::LockWatches(). */
  std::vector<SystemPageFlagsBlock> system_page_flags_;
};

enum class AccessViolationResult : u8 {
  /** The write watch resolved the fault, so the access runs again. */
  kResumed,
  /** The access reached a register block that generated code did not route. */
  kMMIOMiss,
  /** GuestAddressSpace makes no claim on the fault. */
  kUnhandled,
};

/**
 * The guest address space, backed by a memory mapped file at a fixed host address. It is split into
 * ranges, each a BaseHeap that is a VirtualHeap or a PhysicalHeap.
 */
class REX_KERNEL_API GuestAddressSpace : public ISystemHeap {
 public:
  /** Constructs the memory system and registers it as the active instance. */
  GuestAddressSpace();
  ~GuestAddressSpace();

  /**
   * Initializes the memory system.
   *
   * @return False when the host address space cannot be reserved or the file mapping fails.
   */
  bool Initialize();

  /** Returns the path of the memory mapped file that backs all memory. */
  const std::filesystem::path& GetFileName() const { return file_name_; }

  u8* GetVirtualMembase() const { return virtual_membase_; }

  /** Returns the translator for guest virtual addresses, which does not depend on this object. */
  rex::memory::GuestMembase GetMembase() const {
    return rex::memory::GuestMembase(virtual_membase_);
  }

  /**
   * Translates a guest virtual address to a host address, with the arithmetic recompiled code
   * uses. The contents at the host address are big endian.
   */
  template <typename T = u8*>
  T TranslateVirtual(u32 guest_address) const {
    assert(!LookupHeap(guest_address) ||
           LookupHeap(guest_address)->GetHostAddressOffset() ==
               rex::memory::detail::PhysicalHostOffset(guest_address));
    return rex::memory::GuestPtr<T>(virtual_membase_, guest_address);
  }

  /**
   * Translates a guest virtual address to a host pointer only when the title could access it.
   *
   * @param required The kMemoryProtect bits the page must grant, the read bit by default.
   * @return The host pointer, null when the address is 0, misaligned, in no heap, or on a page that
   * lacks a required bit.
   */
  template <typename T = u8*>
  T TryTranslateVirtual(u32 guest_address, u32 required = kMemoryProtectRead) const {
    static_assert(std::is_pointer_v<T>, "TryTranslateVirtual returns a pointer type");
    constexpr u32 kAlignment = detail::PointeeAlignment<std::remove_pointer_t<T>>::kValue;
    if (guest_address == 0 || guest_address % kAlignment != 0) {
      return nullptr;
    }
    const BaseHeap* heap = LookupHeap(guest_address);
    u32 protect = 0;
    if (heap == nullptr || !const_cast<BaseHeap*>(heap)->QueryProtect(guest_address, &protect) ||
        (protect & required) != required) {
      return nullptr;
    }
    return rex::memory::GuestPtr<T>(virtual_membase_, guest_address);
  }

  u8* GetPhysicalMembase() const { return physical_membase_; }

  /** Translates a guest physical address to a host pointer. The contents are big endian. */
  template <typename T = u8*>
  T TranslatePhysical(u32 guest_address) const {
    return reinterpret_cast<T>(physical_membase_ + (guest_address & 0x1FFFFFFF));
  }

  /**
   * Translates a host address to a guest virtual address.
   */
  u32 HostToGuestVirtual(const void* host_address) const;

  /**
   * Returns the guest physical address for a guest virtual address, or UINT32_MAX when it cannot be
   * obtained.
   */
  u32 GetPhysicalAddress(u32 address) const;

  void Zero(u32 address, u32 size) const;

  void Fill(u32 address, u32 size, u8 value) const;

  /** Copies a non overlapping range of guest memory, like memcpy. */
  void Copy(u32 dest, u32 src, u32 size) const;

  /**
   * Resolves a host access violation inside guest memory.
   *
   * @param host_address The host address the access faulted on.
   * @param is_write True when the access was a write.
   * @return kMMIOMiss for a register block, kResumed when the write watch resolved the fault,
   *         otherwise kUnhandled.
   */
  AccessViolationResult HandleAccessViolation(void* host_address, bool is_write);

  /**
   * Callback for physical memory invalidation notifications. Callbacks are one shot, may fire
   * spuriously or more than once, and fire only for accesses through the guest virtual views of
   * physical memory, so a handler ignores pages it does not track and data providers write through
   * the host physical heap. A triggered page is unwatched in one guest view until requested again.
   * The granularity is 1 page, so a handler invalidates all data in every page from the one holding
   * the first written byte to the one holding the last. A write fault unprotects the shortest range
   * the callbacks return.
   *
   * @param context_ptr The context given at registration.
   * @param length The length of the watched region in bytes.
   * @param exact_range True when only the exact range is being unwatched.
   * @return The start and length of the smallest physical memory region around the watched region
   * that can be unwatched, or (0, UINT32_MAX) when it does not matter.
   */
  using PhysicalMemoryInvalidationCallback = std::pair<u32, u32> (*)(void* context_ptr,
                                                                     u32 physical_address_start,
                                                                     u32 length, bool exact_range);
  /**
   * Registers a physical memory invalidation callback.
   *
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
   */
  void EnablePhysicalMemoryAccessCallbacks(u32 physical_address, u32 length,
                                           bool enable_invalidation_notifications,
                                           bool enable_data_providers);

  /**
   * Allocates virtual memory from the system heap, kept apart from title memory but reachable with
   * normal guest virtual addresses.
   *
   * @param system_heap_flags SystemHeapFlag bits.
   */
  u32 SystemHeapAlloc(u32 size, u32 alignment = 0x20,
                      u32 system_heap_flags = kSystemHeapDefault) override;

  void SystemHeapFree(u32 address, u32* out_region_size = nullptr) override;

  bool IsAddressMapped(u32 address) const override { return LookupHeap(address) != nullptr; }

  const BaseHeap* LookupHeap(u32 address) const;

  BaseHeap* LookupHeap(u32 address) {
    return const_cast<BaseHeap*>(
        const_cast<const kernel::GuestAddressSpace*>(this)->LookupHeap(address));
  }

  BaseHeap* LookupHeapByType(bool physical, u32 page_size);

  /**
   * @param unreserved_pages Receives the unreserved page count.
   * @param reserved_pages Receives the reserved page count.
   * @param used_pages Receives the used page count.
   * @param reserved_bytes Receives the reserved size in bytes.
   */
  void GetHeapsPageStats(const BaseHeap* const* provided_heaps, size_t heaps_count,
                         u32& unreserved_pages, u32& reserved_pages, u32& used_pages,
                         u32& reserved_bytes);

  VirtualHeap* GetPhysicalHeap();

  /**
   * Sets the title memory budget.
   *
   * @param megabytes Budget in MiB, 0 for no cap.
   */
  void SetBudget(u32 megabytes);

  /** Returns the title pages counted against the budget. The count is in 4 KB units. */
  [[nodiscard]] u32 GetTitlePages() const;

  /**
   * @return The budget in 4 KB units, 0 when uncapped.
   */
  [[nodiscard]] u32 GetBudgetPages() const;

  /**
   * Plots heap, allocator pool and title memory use on the profiler graphs. Call it once per frame.
   */
  void PlotProfile();

  /**
   * Initializes the per module function dispatch table at IMAGE_BASE + IMAGE_SIZE, indexed by
   * (guest_addr - code_base) * 2. The function table calls are internally locked and may be made
   * from any thread.
   *
   * @param code_base The first guest address of the code.
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
   * @return False when guest_address is outside every registered module range.
   */
  bool SetFunction(u32 guest_address, PPCFunc* host_function);

  /**
   * Takes the lock that guards the physical memory watch state of the heaps and of every
   * registered invalidation callback client. It is a Leaf lock held while callbacks run, so
   * callbacks take no other ranked lock.
   */
  [[nodiscard]] std::unique_lock<rex::platform::RankedMutex> LockWatches() {
    return std::unique_lock(watch_mutex_);
  }

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
  bool ReserveBudgetPages(u32 pages);

  /**
   * @param pages Count in 4 KB units.
   */
  void ReleaseBudgetPages(u32 pages);

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
  friend class GuestAllocator;

  friend class PhysicalHeap;
  rex::platform::RankedMutex watch_mutex_{rex::platform::LockRank::kLeaf};
  std::vector<std::unique_ptr<std::pair<PhysicalMemoryInvalidationCallback, void*>>>
      physical_memory_invalidation_callbacks_;
  std::atomic<u32> title_pages_{0};
  std::atomic<u32> budget_pages_{0};
  std::mutex allocators_mutex_;
  std::vector<GuestAllocator*> allocators_; /**< Every live guest allocator, for the plots. */
};

}  // namespace rex::kernel
