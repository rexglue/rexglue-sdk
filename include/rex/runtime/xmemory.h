#pragma once
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

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/macros.h>
#include <rex/memory/utils.h>
#include <rex/ppc/func.h>  // PPCFunc type
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

/// Lightweight guest-to-host pointer translation using the memory base.
/// For hooks and kernel code operating with the base pointer directly.
/// Same raw arithmetic as recompiled code; no Memory* or heap lookup needed.
template <typename T = u8*>
inline T GuestPtr(u8* base, u32 guest_address) noexcept {
  return reinterpret_cast<T>(base + guest_address + detail::PhysicalHostOffset(guest_address));
}

class Memory;

enum SystemHeapFlag : uint32_t {
  kSystemHeapVirtual = 1 << 0,
  kSystemHeapPhysical = 1 << 1,

  kSystemHeapDefault = kSystemHeapVirtual,
};

enum class HeapType : uint8_t {
  kGuestVirtual,
  kGuestXex,
  kGuestPhysical,
};

enum MemoryAllocationFlag : uint32_t {
  kMemoryAllocationReserve = 1 << 0,
  kMemoryAllocationCommit = 1 << 1,
  /** Counts the newly committed pages against the title memory budget. */
  kMemoryAllocationTitle = 1 << 2,
};

enum MemoryProtectFlag : uint32_t {
  kMemoryProtectRead = 1 << 0,
  kMemoryProtectWrite = 1 << 1,
  kMemoryProtectNoCache = 1 << 2,
  kMemoryProtectWriteCombine = 1 << 3,

  kMemoryProtectNoAccess = 0,
};

// Equivalent to the Win32 MEMORY_BASIC_INFORMATION struct.
struct HeapAllocationInfo {
  // A pointer to the base address of the region of pages.
  uint32_t base_address;
  // A pointer to the base address of a range of pages allocated by the
  // VirtualAlloc function. The page pointed to by the BaseAddress member is
  // contained within this allocation range.
  uint32_t allocation_base;
  // The memory protection option when the region was initially allocated.
  uint32_t allocation_protect;
  // The size specified when the region was initially allocated, in bytes.
  uint32_t allocation_size;
  // The size of the region beginning at the base address in which all pages
  // have identical attributes, in bytes.
  uint32_t region_size;
  // The state of the pages in the region (commit/free/reserve).
  uint32_t state;
  // The access protection of the pages in the region.
  uint32_t protect;
};

// Describes a single page in the page table.
union PageEntry {
  uint64_t qword;
  struct {
    // Base address of the allocated region in 4k pages.
    uint32_t base_address : 20;
    // Total number of pages in the allocated region in 4k pages.
    uint32_t region_page_count : 20;
    // Protection bits specified during region allocation.
    // Composed of bits from MemoryProtectFlag.
    uint32_t allocation_protect : 4;
    // Current protection bits as of the last Protect.
    // Composed of bits from MemoryProtectFlag.
    uint32_t current_protect : 4;
    // Allocation state of the page as a MemoryAllocationFlag bit mask.
    uint32_t state : 2;
    /** Set on a committed page that counts against the title memory budget. */
    uint32_t title : 1;
    uint32_t reserved : 13;
  };
};

// Heap abstraction for page-based allocation.
class REX_RUNTIME_API BaseHeap {
 public:
  virtual ~BaseHeap();

  // Offset of the heap in relative to membase, without host_address_offset
  // adjustment.
  uint32_t heap_base() const { return heap_base_; }

  // Length of the heap range.
  uint32_t heap_size() const { return heap_size_; }

  // Size of each page within the heap range in bytes.
  uint32_t page_size() const { return page_size_; }

  // Type of specified heap
  HeapType heap_type() const { return heap_type_; }

  // Offset added to the virtual addresses to convert them to host addresses
  // (not including membase).
  uint32_t host_address_offset() const { return host_address_offset_; }

  template <typename T = uint8_t*>
  inline T TranslateRelative(size_t relative_address) const {
    return reinterpret_cast<T>(membase_ + heap_base_ + host_address_offset_ + relative_address);
  }

  // Disposes and decommits all memory and clears the page table.
  virtual void Dispose();

  uint32_t total_page_count() const { return static_cast<uint32_t>(page_table_.size()); }
  uint32_t unreserved_page_count() const { return unreserved_page_count_; }
  uint32_t reserved_page_count() const { return total_page_count() - unreserved_page_count(); }

  // Allocates pages with the given properties and allocation strategy.
  // This can reserve and commit the pages as well as set protection modes.
  // This will fail if not enough contiguous pages can be found.
  virtual bool Alloc(uint32_t size, uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                     bool top_down, uint32_t* out_address);

  // Allocates pages at the given address.
  // This can reserve and commit the pages as well as set protection modes.
  // This will fail if the pages are already allocated.
  virtual bool AllocFixed(uint32_t base_address, uint32_t size, uint32_t alignment,
                          uint32_t allocation_type, uint32_t protect);

  // Allocates pages at an address within the given address range.
  // This can reserve and commit the pages as well as set protection modes.
  // This will fail if not enough contiguous pages can be found.
  virtual bool AllocRange(uint32_t low_address, uint32_t high_address, uint32_t size,
                          uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                          bool top_down, uint32_t* out_address);

  // Allocates from the system portion of the heap (top of virtual heaps).
  // Physical heaps delegate to regular Alloc since there is no split.
  virtual bool AllocSystemHeap(uint32_t size, uint32_t alignment, uint32_t allocation_type,
                               uint32_t protect, bool top_down, uint32_t* out_address);

  // Decommits pages in the given range.
  // Partial overlapping pages will also be decommitted.
  virtual bool Decommit(uint32_t address, uint32_t size);

  // Decommits and releases pages in the given range.
  // Partial overlapping pages will also be released.
  virtual bool Release(uint32_t address, uint32_t* out_region_size = nullptr);

  // Modifies the protection mode of pages within the given range.
  virtual bool Protect(uint32_t address, uint32_t size, uint32_t protect,
                       uint32_t* old_protect = nullptr);

  // Queries information about the given region of pages.
  bool QueryRegionInfo(uint32_t base_address, HeapAllocationInfo* out_info);

  // Queries the size of the region containing the given address.
  bool QuerySize(uint32_t address, uint32_t* out_size);

  // Queries the current protection mode of the region containing the given
  // address.
  bool QueryProtect(uint32_t address, uint32_t* out_protect);

  // Queries the strictest readability and writability for the entire
  // range.
  rex::memory::PageAccess QueryRangeAccess(uint32_t low_address, uint32_t high_address);

 protected:
  friend class PhysicalHeap;

  BaseHeap();

  /**
   * Reconciles host page access with guest page protection. The caller holds the memory watch lock
   * when guest pages are smaller than host pages.
   *
   * @param start_page_number First guest page of the range.
   * @param end_page_number Last guest page of the range.
   * @return True when the host protection was updated.
   */
  bool SyncHostPageAccess(uint32_t start_page_number, uint32_t end_page_number);

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
  bool AllocFixedLocked(uint32_t base_address, uint32_t size, uint32_t alignment,
                        uint32_t allocation_type, uint32_t protect);

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
  bool AllocRangeLocked(uint32_t low_address, uint32_t high_address, uint32_t size,
                        uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                        bool top_down, uint32_t* out_address);

  /**
   * Decommits with the reconcile lock already held.
   *
   * @param address Guest address of the range.
   * @param size Size in bytes.
   * @return True on success.
   */
  bool DecommitLocked(uint32_t address, uint32_t size);

  /**
   * Releases with the reconcile lock already held.
   *
   * @param address Guest base address of the region.
   * @param out_region_size Receives the size of the released region.
   * @return True on success.
   */
  bool ReleaseLocked(uint32_t address, uint32_t* out_region_size);

  /**
   * Changes protection with the reconcile lock already held.
   *
   * @param address Guest address of the range.
   * @param size Size in bytes.
   * @param protect Memory protect flags.
   * @param old_protect Receives the previous protection.
   * @return True on success.
   */
  bool ProtectLocked(uint32_t address, uint32_t size, uint32_t protect, uint32_t* old_protect);

  // Whether a host page is write-watched for guest invalidation.
  // SyncHostPageAccess must not restore write access to such a page: the watch
  // is stored in a flag that EnableAccessCallbacks checks before re-applying
  // protection, so silently unprotecting here would disable the watch forever.
  // Only PhysicalHeap tracks this; every other heap has no watch state.
  virtual bool IsHostPageWriteWatched(uint32_t host_page_number) const { return false; }

  void Initialize(runtime::Memory* memory, uint8_t* membase, HeapType heap_type, uint32_t heap_base,
                  uint32_t heap_size, uint32_t page_size, uint32_t host_address_offset = 0);

  runtime::Memory* memory_;
  uint8_t* membase_;
  HeapType heap_type_;
  uint32_t heap_base_;
  uint32_t heap_size_;
  uint32_t page_size_;
  uint32_t page_size_shift_ = 0;
  uint32_t host_address_offset_;
  uint32_t unreserved_page_count_ = 0;
  std::recursive_mutex heap_mutex_;
  std::vector<PageEntry> page_table_;
};

// Normal heap allowing allocations from guest virtual address ranges.
class REX_RUNTIME_API VirtualHeap : public BaseHeap {
 public:
  VirtualHeap();
  ~VirtualHeap() override;

  // Initializes the heap properties and allocates the page table.
  void Initialize(runtime::Memory* memory, uint8_t* membase, HeapType heap_type, uint32_t heap_base,
                  uint32_t heap_size, uint32_t page_size);
};

// A heap for ranges of memory that are mapped to physical ranges.
// Physical ranges are used by the audio and graphics subsystems representing
// hardware wired directly to memory in the console.
//
// The physical heap and the behavior of sharing pages with virtual pages is
// implemented by having a 'parent' heap that is used to perform allocation in
// the guest virtual address space 1:1 with the physical address space.
class REX_RUNTIME_API PhysicalHeap : public BaseHeap {
 public:
  PhysicalHeap();
  ~PhysicalHeap() override;

  // Initializes the heap properties and allocates the page table.
  void Initialize(runtime::Memory* memory, uint8_t* membase, HeapType heap_type, uint32_t heap_base,
                  uint32_t heap_size, uint32_t page_size, VirtualHeap* parent_heap);

  bool Alloc(uint32_t size, uint32_t alignment, uint32_t allocation_type, uint32_t protect,
             bool top_down, uint32_t* out_address) override;
  bool AllocFixed(uint32_t base_address, uint32_t size, uint32_t alignment,
                  uint32_t allocation_type, uint32_t protect) override;
  bool AllocRange(uint32_t low_address, uint32_t high_address, uint32_t size, uint32_t alignment,
                  uint32_t allocation_type, uint32_t protect, bool top_down,
                  uint32_t* out_address) override;
  bool AllocSystemHeap(uint32_t size, uint32_t alignment, uint32_t allocation_type,
                       uint32_t protect, bool top_down, uint32_t* out_address) override;
  bool Decommit(uint32_t address, uint32_t size) override;
  bool Release(uint32_t base_address, uint32_t* out_region_size = nullptr) override;
  bool Protect(uint32_t address, uint32_t size, uint32_t protect,
               uint32_t* old_protect = nullptr) override;

  void EnableAccessCallbacks(uint32_t physical_address, uint32_t length,
                             bool enable_invalidation_notifications, bool enable_data_providers);
  // Returns true if any page in the range was watched.
  bool TriggerCallbacks(uint32_t virtual_address, uint32_t length, bool is_write,
                        bool unwatch_exact_range, bool unprotect = true);

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
  bool TriggerCallbacksLocked(uint32_t virtual_address, uint32_t length, bool is_write,
                              bool unwatch_exact_range, bool unprotect = true);

  uint32_t GetPhysicalAddress(uint32_t address) const;

 protected:
  bool IsHostPageWriteWatched(uint32_t host_page_number) const override;

  std::unique_lock<rex::platform::RankedMutex> AcquireHostPageReconcileLock() const override;

  VirtualHeap* parent_heap_;

  uint32_t system_page_size_;
  uint32_t system_page_count_;

  struct SystemPageFlagsBlock {
    // Whether writing to each page should result trigger invalidation
    // callbacks.
    uint64_t notify_on_invalidation;
  };
  // Protected by Memory::watch_mutex(). Flags for each 64 system pages,
  // interleaved as blocks, so bit scan can be used to quickly extract ranges.
  std::vector<SystemPageFlagsBlock> system_page_flags_;
};

/** Outcome of a host access violation inside guest memory. */
enum class AccessViolationResult : u8 {
  /** The write watch resolved the fault, so the access runs again. */
  Resumed,
  /** The access reached a register block that generated code did not route. */
  MMIOMiss,
  /** Memory makes no claim on the fault. */
  Unhandled,
};

// Models the entire guest memory system on the console.
// This exposes interfaces to both virtual and physical memory and a TLB and
// page table for allocation, mapping, and protection.
//
// The memory is backed by a memory mapped file and is placed at a stable
// fixed address in the host address space (like 0x100000000). This allows
// efficient guest<->host address translations as well as easy sharing of the
// memory across subsystems.
//
// The guest memory address space is split into several ranges that have varying
// properties such as page sizes, caching strategies, protections, and
// overlap with other ranges. Each range is represented by a BaseHeap of either
// VirtualHeap or PhysicalHeap depending on type. Heaps model the page tables
// and can handle reservation and committing of requested pages.
class REX_RUNTIME_API Memory {
 public:
  Memory();
  ~Memory();

  // Initializes the memory system.
  // This may fail if the host address space could not be reserved or the
  // mapping to the file system fails.
  bool Initialize();

  // Full file name and path of the memory-mapped file backing all memory.
  const std::filesystem::path& file_name() const { return file_name_; }

  // Base address of virtual memory in the host address space.
  // This is often something like 0x100000000.
  inline uint8_t* virtual_membase() const { return virtual_membase_; }

  /**
   * Translates a guest virtual address to a host address, with the arithmetic recompiled code
   * uses. The contents at the host address are big endian.
   *
   * @tparam T The pointer type to return.
   * @param guest_address The guest virtual address.
   * @return The host address.
   */
  template <typename T = uint8_t*>
  inline T TranslateVirtual(uint32_t guest_address) const {
    assert(!LookupHeap(guest_address) || LookupHeap(guest_address)->host_address_offset() ==
                                             detail::PhysicalHostOffset(guest_address));
    return GuestPtr<T>(virtual_membase_, guest_address);
  }

  // Base address of physical memory in the host address space.
  // This is often something like 0x200000000.
  inline uint8_t* physical_membase() const { return physical_membase_; }

  // Translates a guest physical address to a host address that can be accessed
  // as a normal pointer.
  // Note that the contents at the specified host address are big-endian.
  template <typename T = uint8_t*>
  inline T TranslatePhysical(uint32_t guest_address) const {
    return reinterpret_cast<T>(physical_membase_ + (guest_address & 0x1FFFFFFF));
  }

  // Translates a host address to a guest virtual address.
  // Note that the contents at the returned host address are big-endian.
  uint32_t HostToGuestVirtual(const void* host_address) const;

  // Returns the guest physical address for the guest virtual address, or
  // UINT32_MAX if it can't be obtained.
  uint32_t GetPhysicalAddress(uint32_t address) const;

  // Zeros out a range of memory at the given guest address.
  void Zero(uint32_t address, uint32_t size) const;

  // Fills a range of guest memory with the given byte value.
  void Fill(uint32_t address, uint32_t size, uint8_t value) const;

  // Copies a non-overlapping range of guest memory (like a memcpy).
  void Copy(uint32_t dest, uint32_t src, uint32_t size) const;

  /**
   * Resolves a host access violation inside guest memory.
   *
   * @param host_address The host address the access faulted on.
   * @param is_write True when the access was a write.
   * @return MMIOMiss for a register block, Resumed when the write watch resolved the fault,
   *         otherwise Unhandled.
   */
  AccessViolationResult HandleAccessViolation(void* host_address, bool is_write);

  // Physical memory access callbacks, two types of them.
  //
  // This is simple per-system-page protection without reference counting or
  // stored ranges. Whenever a watched page is accessed, all callbacks for it
  // are triggered. Also the only way to remove callbacks is to trigger them
  // somehow. Since there are no references from pages to individual callbacks,
  // there's no way to disable only a specific callback for a page. Also
  // callbacks may be triggered spuriously, and handlers should properly ignore
  // pages they don't care about.
  //
  // Once callbacks are triggered for a page, the page is not watched anymore
  // until requested again later. It is, however, unwatched only in one guest
  // view of physical memory (because different views may have different
  // protection for the same memory) - but it's rare when the same memory is
  // used with different guest page sizes, and it's okay to fire a callback more
  // than once.
  //
  // Only accessing the guest virtual memory views of physical memory triggers
  // callbacks - data providers, for instance, must write to the host physical
  // heap directly, otherwise their threads may infinitely await themselves.
  //
  // - Invalidation notifications:
  //
  // Protecting from writing. One-shot callbacks for invalidation of
  // kinds of physical memory caches (such as the GPU copy of the memory).
  //
  // May be triggered for a single page (in case of a write access violation or
  // when need to synchronize data given by data providers) or for multiple
  // pages (like when memory is released, or explicitly to trigger callbacks
  // when host-side code can't rely on regular access violations, like when
  // accessing a file).
  //
  // Since granularity of callbacks is one single page, an invalidation
  // notification handler must invalidate the all the data stored in the touched
  // pages.
  //
  // Because large ranges (like whole framebuffers) may be written to and
  // exceptions are expensive, it's better to unprotect multiple pages as a
  // result of a write access violation, so the shortest common range returned
  // by all the invalidation callbacks (clamped to a sane range and also not to
  // touch pages with provider callbacks) is unprotected.
  //
  // - Data providers:
  //
  // TODO(Triang3l): Implement data providers - more complicated because they
  // will need to be able to release the global lock.

  // Returns start and length of the smallest physical memory region surrounding
  // the watched region that can be safely unwatched, if it doesn't matter,
  // return (0, UINT32_MAX).
  using PhysicalMemoryInvalidationCallback = std::pair<uint32_t, uint32_t> (*)(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);
  // Returns a handle for unregistering or for skipping one notification handler
  // while triggering data providers.
  void* RegisterPhysicalMemoryInvalidationCallback(PhysicalMemoryInvalidationCallback callback,
                                                   void* callback_context);
  // Unregisters a physical memory invalidation callback previously added with
  // RegisterPhysicalMemoryInvalidationCallback.
  void UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle);

  // Enables physical memory access callbacks for the specified memory range,
  // snapped to system page boundaries.
  void EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                           bool enable_invalidation_notifications,
                                           bool enable_data_providers);

  /**
   * Returns the lock that guards the physical memory watch state of the heaps and of every
   * registered invalidation callback client. It is a Leaf lock held while callbacks run, so
   * callbacks take no other ranked lock.
   *
   * @return The watch lock.
   */
  rex::platform::RankedMutex& watch_mutex() { return watch_mutex_; }

  // Allocates virtual memory from the 'system' heap.
  // System memory is kept separate from game memory but is still accessible
  // using normal guest virtual addresses. Kernel structures and other internal
  // 'system' allocations should come from this heap when possible.
  uint32_t SystemHeapAlloc(uint32_t size, uint32_t alignment = 0x20,
                           uint32_t system_heap_flags = kSystemHeapDefault);

  // Frees memory allocated with SystemHeapAlloc.
  void SystemHeapFree(uint32_t address, uint32_t* out_region_size = nullptr);

  // Gets the heap for the address space containing the given address.
  const BaseHeap* LookupHeap(uint32_t address) const;

  inline BaseHeap* LookupHeap(uint32_t address) {
    return const_cast<BaseHeap*>(const_cast<const runtime::Memory*>(this)->LookupHeap(address));
  }

  // Gets the heap with the given properties.
  BaseHeap* LookupHeapByType(bool physical, uint32_t page_size);

  // Aggregates page statistics across the given heaps.
  void GetHeapsPageStatsSummary(const BaseHeap* const* provided_heaps, size_t heaps_count,
                                uint32_t& unreserved_pages, uint32_t& reserved_pages,
                                uint32_t& used_pages, uint32_t& reserved_bytes);

  // Gets the physical base heap.
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
  [[nodiscard]] u32 title_pages() const;

  /**
   * Returns the title memory budget.
   *
   * @return The budget in 4 KB units, 0 when uncapped.
   */
  [[nodiscard]] u32 budget_pages() const;

  //==========================================================================
  // Recompiled Code Function Table API
  //==========================================================================
  // Per-module function dispatch table at IMAGE_BASE + IMAGE_SIZE, indexed by
  // (guest_addr - code_base) * 2. Internally locked: callers may invoke from
  // any thread.
  bool InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,
                               uint32_t image_size);
  bool DestroyFunctionTable(uint32_t code_base);
  // Returns false if guest_address is outside every registered module range.
  bool SetFunction(uint32_t guest_address, PPCFunc* host_function);

 private:
#if REX_PLATFORM_MAC
  int MapViewsMac();
#endif
  int MapViews(uint8_t* mapping_base);
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
  uint32_t system_page_size_ = 0;
  uint32_t system_allocation_granularity_ = 0;
  uint8_t* virtual_membase_ = nullptr;
  uint8_t* physical_membase_ = nullptr;

  struct FunctionTableEntry {
    uint32_t table_base;
    uint32_t code_base;
    uint32_t code_size;
    uint32_t thunk_reserve;
  };
  std::vector<FunctionTableEntry> function_tables_;
  mutable std::mutex function_tables_mutex_;

  rex::memory::FileMappingHandle mapping_ = rex::memory::kFileMappingHandleInvalid;
  uint8_t* mapping_base_ = nullptr;
  union {
    struct {
      uint8_t* v00000000;
      uint8_t* v40000000;
      uint8_t* v7F000000;
      uint8_t* v80000000;
      uint8_t* v90000000;
      uint8_t* vA0000000;
      uint8_t* vC0000000;
      uint8_t* vE0000000;
      uint8_t* physical;
    };
    uint8_t* all_views[9];
  } views_ = {{0, 0, 0, 0, 0, 0, 0, 0, 0}};

  struct {
    VirtualHeap v00000000;
    VirtualHeap v40000000;
    VirtualHeap v80000000;
    VirtualHeap v90000000;

    VirtualHeap physical;
    PhysicalHeap vA0000000;
    PhysicalHeap vC0000000;
    PhysicalHeap vE0000000;
  } heaps_;

  friend class BaseHeap;

  friend class PhysicalHeap;
  rex::platform::RankedMutex watch_mutex_{rex::platform::LockRank::Leaf};
  std::vector<std::unique_ptr<std::pair<PhysicalMemoryInvalidationCallback, void*>>>
      physical_memory_invalidation_callbacks_;
  std::atomic<u32> title_pages_{0};
  std::atomic<u32> budget_pages_{0};
};

}  // namespace rex::runtime
