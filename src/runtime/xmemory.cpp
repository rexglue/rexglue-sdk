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

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <span>
#include <utility>

#if REX_PLATFORM_MAC
#include <sys/mman.h>
#endif

#include <format>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/mmio.h>
#include <rex/runtime/xmemory.h>
#include <rex/platform/thread.h>
#include "runtime/cvar_table.h"

// TODO(benvanik): move xbox.h out
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

uint32_t get_page_count(uint32_t value, uint32_t page_size, uint32_t page_size_shift) {
  return rex::round_up(value, page_size) >> page_size_shift;
}

/**
 * Memory map:
 * 0x00000000 - 0x3FFFFFFF (1024mb) - virtual 4k pages
 * 0x40000000 - 0x7FFFFFFF (1024mb) - virtual 64k pages
 * 0x80000000 - 0x8BFFFFFF ( 192mb) - xex 64k pages
 * 0x8C000000 - 0x8FFFFFFF (  64mb) - xex 64k pages (encrypted)
 * 0x90000000 - 0x9FFFFFFF ( 256mb) - xex 4k pages
 * 0xA0000000 - 0xBFFFFFFF ( 512mb) - physical 64k pages
 * 0xC0000000 - 0xDFFFFFFF          - physical 16mb pages
 * 0xE0000000 - 0xFFFFFFFF          - physical 4k pages
 *
 * We use the host OS to create an entire addressable range for this. That way
 * we don't have to emulate a TLB. It'd be cool to pass through page
 * sizes or use madvice to let the OS know what to expect.
 *
 * We create our own heap of committed memory that resides at
 * memory_HEAP_LOW to memory_HEAP_HIGH - all normal user allocations
 * come from there. Since the console has no paging, we know that the size of
 * this heap will never need to be larger than ~512MB (realistically, smaller
 * than that). We place it far away from the XEX data and keep the memory
 * around it uncommitted so that we have some warning if things go astray.
 *
 * For XEX and GPU data we allow placement allocations (base_address != 0) and
 * commit the requested memory as needed. This bypasses the standard heap, but
 * XEXs should never be overwriting anything so that's fine. We can also query
 * for previous commits and assert that we aren't committing twice.
 *
 * GPU memory is mapped onto the lower 512mb of the virtual 4k range (0).
 * So 0xA0000000 = 0x00000000. A more sophisticated allocator could handle
 * this.
 */

static runtime::Memory* active_memory_ = nullptr;

namespace {

enum class GuestFaultKind : u8 { kMMIO, kUnhandled, kPhysicalWrite };

struct GuestFault {
  GuestFaultKind kind = GuestFaultKind::kUnhandled;
  u32 address = 0;
  bool is_write = false;
  u32 physical_address = 0;
  u32 guest_protect = 0;
  u32 physical_protect = 0;
};

thread_local std::optional<GuestFault> t_guest_fault;

class PendingScribble {
 public:
  PendingScribble(u32 allocation_type, u32 protect)
      : enabled_((allocation_type & ~static_cast<u32>(kMemoryAllocationTitle)) !=
                     kMemoryAllocationReserve &&
                 REXCVAR_GET(::rex::runtime::cvars::scribble_heap) &&
                 (protect & kMemoryProtectWrite)) {}
  PendingScribble(const PendingScribble&) = delete;
  PendingScribble& operator=(const PendingScribble&) = delete;
  ~PendingScribble() {
    for (const auto& [host, size] : ranges_) {
      if (host) {
        std::memset(host, 0xCD, size);
      }
    }
  }

  void Add(const BaseHeap& heap, u32 address, u32 size) {
    if (!enabled_) {
      return;
    }
    auto& range = ranges_[ranges_[0].first ? 1 : 0];
    range = {heap.TranslateRelative(address - heap.heap_base()),
             rex::round_up(size, heap.page_size())};
  }

 private:
  bool enabled_;
  std::array<std::pair<u8*, u32>, 2> ranges_{};
};
std::atomic<u32> g_recovered_protect_count{0};
std::atomic<u32> g_recovered_protect_address{0};

u32 DescribeCrashAddress(u64 host_address, std::span<char> out) {
  constexpr u64 kGuestAddressSpace = u64{1} << 32;
  char* cursor = out.data();
  char* const end = out.data() + out.size();
  const auto append = [&]<typename... Args>(std::format_string<Args...> format, Args&&... args) {
    cursor = std::format_to_n(cursor, end - cursor, format, std::forward<Args>(args)...).out;
  };
  if (const auto& fault = t_guest_fault) {
    const std::string_view access = fault->is_write ? "write" : "read";
    switch (fault->kind) {
      case GuestFaultKind::kMMIO:
        append("{} register {} 0x{:08X}",
               MMIOBlockOf(fault->address) == MMIOBlock::GPU ? "GPU" : "APU", access,
               fault->address);
        break;
      case GuestFaultKind::kUnhandled:
        append("unhandled guest {} of 0x{:08X}", access, fault->address);
        break;
      case GuestFaultKind::kPhysicalWrite:
        append(
            "unhandled guest physical write of 0x{:08X}, physical 0x{:08X}, guest protect "
            "0x{:08X}, physical protect 0x{:08X}",
            fault->address, fault->physical_address, fault->guest_protect, fault->physical_protect);
        break;
    }
  }
  const Memory* memory = active_memory_.load(std::memory_order_acquire);
  if (memory && host_address) {
    const u64 base = reinterpret_cast<u64>(memory->virtual_membase());
    if (host_address >= base && host_address - base < kGuestAddressSpace) {
      append("{}guest address 0x{:08X}", cursor == out.data() ? "" : ", ",
             memory->HostToGuestVirtual(std::bit_cast<const void*>(host_address)));
    }
  }
  if (const ThreadRecord* thread =
          ThreadRecord::IsInThread() ? ThreadRecord::GetCurrentThread() : nullptr) {
    append("{}guest thread {:08X} {}", cursor == out.data() ? "" : ", ", thread->thread_id(),
           thread->crash_name());
  }
  return static_cast<u32>(cursor - out.data());
}

bool OnAccessViolation(rex::platform::Exception* ex, void* data) {
  if (ex->code() != rex::platform::Exception::Code::kAccessViolation) {
    return false;
  }
  const auto operation = ex->access_violation_operation();
  if (operation != rex::platform::Exception::AccessViolationOperation::kRead &&
      operation != rex::platform::Exception::AccessViolationOperation::kWrite) {
    return false;
  }
  auto* memory = static_cast<Memory*>(data);
  const bool is_write = operation == rex::platform::Exception::AccessViolationOperation::kWrite;
  auto* host_address = std::bit_cast<void*>(static_cast<uintptr_t>(ex->fault_address()));
  switch (memory->HandleAccessViolation(host_address, is_write)) {
    case AccessViolationResult::Resumed:
      return true;
    case AccessViolationResult::MMIOMiss: {
      const u32 address = memory->HostToGuestVirtual(host_address);
      REXLOG_ERROR(system, "{} register {} 0x{:08X} at host PC 0x{:016X}",
                   MMIOBlockOf(address) == MMIOBlock::GPU ? "GPU" : "APU",
                   is_write ? "write" : "read", address, ex->pc());
      return false;
    }
    case AccessViolationResult::Unhandled:
      return false;
  }
  return false;
}

}  // namespace

Memory::Memory() {
  system_page_size_ = static_cast<uint32_t>(rex::memory::page_size());
  system_allocation_granularity_ = static_cast<uint32_t>(rex::memory::allocation_granularity());
  REX_ASSERT(active_memory_ == 0);
  active_memory_ = this;
}

Memory::~Memory() {
  REX_ASSERT(active_memory_ == this);
  active_memory_ = nullptr;

  rex::platform::ExceptionHandler::Uninstall(OnAccessViolation, this);

  physical_memory_invalidation_callbacks_.clear();

  heaps_.v00000000.Dispose();
  heaps_.v40000000.Dispose();
  heaps_.v80000000.Dispose();
  heaps_.v90000000.Dispose();
  heaps_.vA0000000.Dispose();
  heaps_.vC0000000.Dispose();
  heaps_.vE0000000.Dispose();
  heaps_.physical.Dispose();

  // Unmap all views and close mapping.
  if (mapping_ != rex::memory::kFileMappingHandleInvalid) {
    UnmapViews();
    rex::memory::CloseFileMappingHandle(mapping_, file_name_);
    mapping_base_ = nullptr;
    mapping_ = rex::memory::kFileMappingHandleInvalid;
  }

  virtual_membase_ = nullptr;
  physical_membase_ = nullptr;
}

bool Memory::Initialize() {
  file_name_ = std::format("xenia_memory_{}", Clock::QueryHostTickCount());

  // Create main page file-backed mapping. This is all reserved but
  // uncommitted (so it shouldn't expand page file).
  // Round up to allocation granularity to accommodate platforms with large pages.
  const size_t mapping_size =
      rex::round_up(static_cast<size_t>(0x120000000ull) + system_allocation_granularity_,
                    static_cast<size_t>(system_allocation_granularity_));
  mapping_ = rex::memory::CreateFileMappingHandle(file_name_, mapping_size,
                                                  rex::memory::PageAccess::kReadWrite, false);
  if (mapping_ == rex::memory::kFileMappingHandleInvalid) {
    REXLOG_ERROR(system, "Unable to reserve the 4gb guest address space.");
    REX_ASSERT_ALWAYS();
    return false;
  }

#if REX_PLATFORM_MAC
  // On macOS, reserve a contiguous host range first, then carve guest views
  // into it with MAP_SHARED|MAP_FIXED so all views share the same backing fd.
  if (MapViewsMac()) {
    REXLOG_ERROR(system, "Unable to find a continuous block in the 64bit address space.");
    REX_ASSERT_ALWAYS();
    return false;
  }
  mapping_base_ = views_.all_views[0];
#else
  // Attempt to create our views. This may fail at the first address
  // we pick, so try a few times.
  mapping_base_ = 0;
  for (size_t n = 32; n < 64; n++) {
    auto mapping_base = std::bit_cast<uint8_t*>(uintptr_t{1} << n);
    if (!MapViews(mapping_base)) {
      mapping_base_ = mapping_base;
      break;
    }
  }
  if (!mapping_base_) {
    REXLOG_ERROR(system, "Unable to find a continuous block in the 64bit address space.");
    REX_ASSERT_ALWAYS();
    return false;
  }
#endif
  virtual_membase_ = mapping_base_;
  physical_membase_ = mapping_base_ + 0x100000000ull;

  REXLOG_INFO(system, "guest memory mapped at virtual base 0x{:016X}, physical base 0x{:016X}",
              reinterpret_cast<uintptr_t>(virtual_membase_),
              reinterpret_cast<uintptr_t>(physical_membase_));

  // Prepare virtual heaps.
  heaps_.v00000000.Initialize(this, virtual_membase_, runtime::HeapType::kGuestVirtual, 0x00000000,
                              0x40000000, 4096);
  heaps_.v40000000.Initialize(this, virtual_membase_, runtime::HeapType::kGuestVirtual, 0x40000000,
                              0x40000000 - 0x01000000, 64 * 1024);
  heaps_.v80000000.Initialize(this, virtual_membase_, runtime::HeapType::kGuestXex, 0x80000000,
                              0x10000000, 64 * 1024);
  heaps_.v90000000.Initialize(this, virtual_membase_, runtime::HeapType::kGuestXex, 0x90000000,
                              0x10000000, 4096);

  // Prepare physical heaps.
  heaps_.physical.Initialize(this, physical_membase_, runtime::HeapType::kGuestPhysical, 0x00000000,
                             0x20000000, 4096);
  heaps_.vA0000000.Initialize(this, virtual_membase_, runtime::HeapType::kGuestPhysical, 0xA0000000,
                              0x20000000, 64 * 1024, &heaps_.physical);
  heaps_.vC0000000.Initialize(this, virtual_membase_, runtime::HeapType::kGuestPhysical, 0xC0000000,
                              0x20000000, 16 * 1024 * 1024, &heaps_.physical);
  heaps_.vE0000000.Initialize(this, virtual_membase_, runtime::HeapType::kGuestPhysical, 0xE0000000,
                              0x1FD00000, 4096, &heaps_.physical);

  // Protect the first and last 64kb of memory.
  heaps_.v00000000.AllocFixed(0x00000000, 0x10000, 0x10000,
                              runtime::kMemoryAllocationReserve | runtime::kMemoryAllocationCommit,
                              !REXCVAR_GET(::rex::runtime::cvars::protect_zero)
                                  ? runtime::kMemoryProtectRead | runtime::kMemoryProtectWrite
                                  : runtime::kMemoryProtectNoAccess);
  heaps_.physical.AllocFixed(0x1FFF0000, 0x10000, 0x10000, runtime::kMemoryAllocationReserve,
                             runtime::kMemoryProtectNoAccess);

  // GPU writeback.
  // 0xC... is physical, 0x7F... is virtual. We may need to overlay these.
  heaps_.vC0000000.AllocFixed(0xC0000000, 0x01000000, 32,
                              runtime::kMemoryAllocationReserve | runtime::kMemoryAllocationCommit,
                              runtime::kMemoryProtectRead | runtime::kMemoryProtectWrite);

  // Pre-commit the physical memory range so the GPU can access it
  // without page faults. Reference: xenia-canary 5f5be0668.
  rex::memory::AllocFixed(heaps_.physical.TranslateRelative(0), heaps_.physical.heap_size(),
                          rex::memory::AllocationType::kCommit,
                          rex::memory::PageAccess::kReadWrite);

  if (!ReserveMMIOBlocks()) {
    REXLOG_ERROR(system, "Unable to reserve the MMIO register blocks");
    REX_ASSERT_ALWAYS();
    return false;
  }
  rex::platform::ExceptionHandler::Install(OnAccessViolation, this);

  // ?
  uint32_t unk_phys_alloc;
  heaps_.vA0000000.Alloc(0x340000, 64 * 1024, runtime::kMemoryAllocationReserve,
                         runtime::kMemoryProtectNoAccess, true, &unk_phys_alloc);

  // Allocate region at start of XEX range. Title 544307D5 explicitly
  // accesses 0x8000001C and expects a specific constant value.
  // Reference: xenia-canary 78f97f8ff.
  uint32_t unknown_xex_range;
  heaps_.v80000000.Alloc(0x40000, 4 * 1024, runtime::kMemoryAllocationCommit,
                         runtime::kMemoryProtectRead | runtime::kMemoryProtectWrite, false,
                         &unknown_xex_range);

  *TranslateVirtual<be_u32*>(0x80000000 + 0x1C) = 0x2a6e3f38;

  return true;
}

static const struct {
  uint64_t virtual_address_start;
  uint64_t virtual_address_end;
  uint64_t target_address;
} map_info[] = {
    // (1024mb) - virtual 4k pages
    {
        0x00000000,
        0x3FFFFFFF,
        0x0000000000000000ull,
    },
    // (1024mb) - virtual 64k pages (cont)
    {
        0x40000000,
        0x7EFFFFFF,
        0x0000000040000000ull,
    },
    //   (16mb) - GPU writeback + 15mb of XPS?
    {
        0x7F000000,
        0x7FFFFFFF,
        0x0000000100000000ull,
    },
    //  (256mb) - xex 64k pages
    {
        0x80000000,
        0x8FFFFFFF,
        0x0000000080000000ull,
    },
    //  (256mb) - xex 4k pages
    {
        0x90000000,
        0x9FFFFFFF,
        0x0000000080000000ull,
    },
    //  (512mb) - physical 64k pages
    {
        0xA0000000,
        0xBFFFFFFF,
        0x0000000100000000ull,
    },
    //          - physical 16mb pages
    {
        0xC0000000,
        0xDFFFFFFF,
        0x0000000100000000ull,
    },
    //          - physical 4k pages
    {
        0xE0000000,
        0xFFFFFFFF,
        0x0000000100000000ull,
    },
    //          - physical raw
    {
        0x100000000,
        0x11FFFFFFF,
        0x0000000100000000ull,
    },
};
#if REX_PLATFORM_MAC
int Memory::MapViewsMac() {
  REX_ASSERT(rex::countof(map_info) == rex::countof(views_.all_views));

  // macOS does not guarantee that a non-MAP_FIXED mmap will honor a requested
  // address. Reserve a contiguous range first, then MAP_FIXED each view within
  // it so the guest layout is stable across runs.
  const size_t total_size =
      static_cast<size_t>(map_info[rex::countof(map_info) - 1].virtual_address_end -
                          map_info[0].virtual_address_start + 1);

  void* reserved_base = mmap(nullptr, total_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reserved_base == MAP_FAILED) {
    REX_FAIL(system, 1, "MapViewsMac: reserve failed: {}", std::strerror(errno));
  }

  uint8_t* mapping_base = reinterpret_cast<uint8_t*>(reserved_base);
  uint64_t granularity_mask = ~static_cast<uint64_t>(system_allocation_granularity_ - 1);

  for (size_t n = 0; n < rex::countof(map_info); n++) {
    size_t view_size = static_cast<size_t>(map_info[n].virtual_address_end -
                                           map_info[n].virtual_address_start + 1);
    size_t file_offset = static_cast<size_t>(map_info[n].target_address & granularity_mask);
    void* target_address = mapping_base + map_info[n].virtual_address_start;
    void* result = mmap(target_address, view_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                        mapping_, file_offset);
    if (result == MAP_FAILED || result != target_address) {
      int err = errno;
      REXLOG_ERROR(
          system,
          "MapViewsMac: map failed view {} addr 0x{:016X} size 0x{:X} offset 0x{:X} err {} ({})", n,
          reinterpret_cast<uintptr_t>(target_address), view_size, file_offset, err,
          std::strerror(err));
      munmap(reserved_base, total_size);
      for (auto& view : views_.all_views) {
        view = nullptr;
      }
      return 1;
    }
    views_.all_views[n] = reinterpret_cast<uint8_t*>(result);
  }

  return 0;
}
#endif  // REX_PLATFORM_MAC

int Memory::MapViews(uint8_t* mapping_base) {
  REX_ASSERT(rex::countof(map_info) == rex::countof(views_.all_views));
  uint64_t granularity_mask = ~static_cast<uint64_t>(system_allocation_granularity_ - 1);
  for (size_t n = 0; n < rex::countof(map_info); n++) {
    views_.all_views[n] = reinterpret_cast<uint8_t*>(rex::memory::MapFileView(
        mapping_, mapping_base + map_info[n].virtual_address_start,
        map_info[n].virtual_address_end - map_info[n].virtual_address_start + 1,
        rex::memory::PageAccess::kReadWrite, map_info[n].target_address & granularity_mask));
    if (!views_.all_views[n]) {
      // Failed, so bail and try again.
      UnmapViews();
      return 1;
    }
  }
  return 0;
}

void Memory::UnmapViews() {
  for (size_t n = 0; n < rex::countof(views_.all_views); n++) {
    if (views_.all_views[n]) {
      size_t length = map_info[n].virtual_address_end - map_info[n].virtual_address_start + 1;
      rex::memory::UnmapFileView(mapping_, views_.all_views[n], length);
    }
  }
}

const BaseHeap* Memory::LookupHeap(uint32_t address) const {
  if (address < 0x40000000) {
    return &heaps_.v00000000;
  }
  if (address < 0x7F000000) {
    return &heaps_.v40000000;
  }
  if (address < 0x80000000 || address >= 0xFFD00000) {
    return nullptr;
  }
  if (address < 0x90000000) {
    return &heaps_.v80000000;
  }
  if (address < 0xA0000000) {
    return &heaps_.v90000000;
  }
  if (address < 0xC0000000) {
    return &heaps_.vA0000000;
  }
  if (address < 0xE0000000) {
    return &heaps_.vC0000000;
  }
  return &heaps_.vE0000000;
}

BaseHeap* Memory::LookupHeapByType(bool physical, uint32_t page_size) {
  if (physical) {
    if (page_size <= 4096) {
      return &heaps_.vE0000000;
    } else if (page_size <= 64 * 1024) {
      return &heaps_.vA0000000;
    } else {
      return &heaps_.vC0000000;
    }
  } else {
    if (page_size <= 4096) {
      return &heaps_.v00000000;
    } else {
      return &heaps_.v40000000;
    }
  }
}

VirtualHeap* Memory::GetPhysicalHeap() {
  return &heaps_.physical;
}

uint32_t Memory::HostToGuestVirtual(const void* host_address) const {
  return detail::RemovePhysicalHostOffset(reinterpret_cast<size_t>(host_address) -
                                          reinterpret_cast<size_t>(virtual_membase_));
}

uint32_t Memory::GetPhysicalAddress(uint32_t address) const {
  const BaseHeap* heap = LookupHeap(address);
  if (!heap || heap->heap_type() != runtime::HeapType::kGuestPhysical) {
    return UINT32_MAX;
  }
  return static_cast<const PhysicalHeap*>(heap)->GetPhysicalAddress(address);
}

void Memory::Zero(uint32_t address, uint32_t size) const {
  std::memset(TranslateVirtual(address), 0, size);
}

void Memory::Fill(uint32_t address, uint32_t size, uint8_t value) const {
  std::memset(TranslateVirtual(address), value, size);
}

void Memory::Copy(uint32_t dest, uint32_t src, uint32_t size) const {
  uint8_t* pdest = TranslateVirtual(dest);
  const uint8_t* psrc = TranslateVirtual(src);
  std::memcpy(pdest, psrc, size);
}

bool Memory::ReserveMMIOBlocks() const {
  const size_t host_page_size = rex::memory::page_size();
  return std::ranges::all_of(std::array{MMIOBlock::GPU, MMIOBlock::APU}, [&](MMIOBlock block) {
    u8* const start = TranslateVirtual(MMIOBlockBase(block));
    u8* const aligned_start = start - (reinterpret_cast<uintptr_t>(start) & (host_page_size - 1));
    const size_t aligned_length =
        rex::round_up(static_cast<size_t>(start - aligned_start) + kMMIOBlockSize, host_page_size);
    return rex::memory::AllocFixed(aligned_start, aligned_length,
                                   rex::memory::AllocationType::kCommit,
                                   rex::memory::PageAccess::kNoAccess) != nullptr;
  });
}

AccessViolationResult Memory::HandleAccessViolation(void* host_address, bool is_write) {
  const auto host = reinterpret_cast<uintptr_t>(host_address);
  const auto virtual_base = reinterpret_cast<uintptr_t>(virtual_membase_);
  const auto physical_base = reinterpret_cast<uintptr_t>(physical_membase_);
  if (host < virtual_base || host >= physical_base + 0x20000000) {
    return AccessViolationResult::Unhandled;
  }
  if (host < physical_base && IsMMIOBlockAddress(static_cast<u32>(host - virtual_base))) {
    return AccessViolationResult::MMIOMiss;
  }
  return AccessViolationCallback(host_address, is_write) ? AccessViolationResult::Resumed
                                                         : AccessViolationResult::Unhandled;
}

bool Memory::AccessViolationCallback(void* host_address, bool is_write) {
  std::unique_lock lock(watch_mutex_);
  {
    rex::memory::PageAccess cur_access;
    size_t page_length = rex::memory::page_size();
    rex::memory::QueryProtect(host_address, page_length, cur_access);
    if (cur_access != rex::memory::PageAccess::kNoAccess &&
        (!is_write || cur_access != rex::memory::PageAccess::kReadOnly)) {
      return true;
    }
  }
  // Access via physical_membase_ is special, when need to bypass everything
  // (for instance, for a data provider to write the data) so only
  // triggering callbacks on virtual memory regions.
  if (reinterpret_cast<size_t>(host_address) < reinterpret_cast<size_t>(virtual_membase_) ||
      reinterpret_cast<size_t>(host_address) >= reinterpret_cast<size_t>(physical_membase_)) {
    return false;
  }
  uint32_t virtual_address = HostToGuestVirtual(host_address);
  BaseHeap* heap = LookupHeap(virtual_address);
  if (!heap || heap->heap_type() != runtime::HeapType::kGuestPhysical) {
    REX_FAIL(
        system, false,
        "Unhandled guest access violation: {} of guest 0x{:08X} (host 0x{:016X}) on thread 0x{:X}",
        is_write ? "write" : "read", virtual_address, reinterpret_cast<uintptr_t>(host_address),
        rex::platform::current_thread_id());
  }

  // Will be rounded to physical page boundaries internally, so pass 1 as
  // the length - guranteed not to cross page boundaries also.
  auto physical_heap = static_cast<PhysicalHeap*>(heap);
  if (physical_heap->TriggerCallbacksLocked(virtual_address, 1, is_write, false)) {
    return true;
  }
  lock.unlock();

  // Recovery path for stale host protection state in physical memory:
  // if guest metadata says the page is writable but host protection is still
  // read-only / no-access, restore write access and resume execution.
  if (is_write) {
    constexpr uint32_t kWriteProtectMask =
        runtime::kMemoryProtectWrite | runtime::kMemoryProtectWriteCombine;
    uint32_t guest_protect = 0;
    bool allow_write = false;
    if (heap->QueryProtect(virtual_address, &guest_protect) &&
        (guest_protect & kWriteProtectMask)) {
      allow_write = true;
    } else {
      // If write-watch left current protect stale, trust committed allocation
      // metadata first for this alias.
      HeapAllocationInfo guest_info{};
      if (heap->QueryRegionInfo(virtual_address, &guest_info) &&
          (guest_info.state & runtime::kMemoryAllocationCommit) &&
          (guest_info.allocation_protect & kWriteProtectMask)) {
        guest_protect = guest_info.protect;
        allow_write = true;
      } else {
        // Alias-aware fallback: consult canonical 0x00000000 physical heap
        // metadata when this alias has stale tracking.
        uint32_t physical_address = GetPhysicalAddress(virtual_address);
        if (physical_address != UINT32_MAX) {
          uint32_t physical_protect = 0;
          if (heaps_.physical.QueryProtect(physical_address, &physical_protect) &&
              (physical_protect & kWriteProtectMask)) {
            guest_protect = physical_protect;
            allow_write = true;
          } else {
            HeapAllocationInfo physical_info{};
            if (heaps_.physical.QueryRegionInfo(physical_address, &physical_info) &&
                (physical_info.state & runtime::kMemoryAllocationCommit) &&
                (physical_info.allocation_protect & kWriteProtectMask)) {
              guest_protect = physical_info.protect;
              allow_write = true;
            }
          }
        }
      }
    }
    if (allow_write) {
      const size_t host_page_size = rex::memory::page_size();
      u8* const page = static_cast<u8*>(host_address) -
                       (reinterpret_cast<uintptr_t>(host_address) & (host_page_size - 1));
      if (rex::memory::Protect(page, host_page_size, rex::memory::PageAccess::kReadWrite,
                               nullptr)) {
        REXLOG_WARN(system,
                    "Recovered stale physical page protection for guest {:08X} (host {:016X}, "
                    "guest_protect {:08X})",
                    virtual_address, reinterpret_cast<uintptr_t>(page), guest_protect);
        return true;
      }
    }

    uint32_t current_guest_protect = 0;
    heap->QueryProtect(virtual_address, &current_guest_protect);
    uint32_t physical_address = GetPhysicalAddress(virtual_address);
    uint32_t physical_protect = 0;
    if (physical_address != UINT32_MAX) {
      heaps_.physical.QueryProtect(physical_address, &physical_protect);
    }
    REXLOG_ERROR(system,
                 "Unhandled guest physical write fault: guest={:08X} host={:016X} phys={:08X} "
                 "guest_protect={:08X} physical_protect={:08X}",
                 virtual_address, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(host_address)),
                 physical_address, current_guest_protect, physical_protect);
  }

  return false;
}

void* Memory::RegisterPhysicalMemoryInvalidationCallback(
    PhysicalMemoryInvalidationCallback callback, void* callback_context) {
  auto entry = std::make_unique<std::pair<PhysicalMemoryInvalidationCallback, void*>>(
      callback, callback_context);
  void* handle = entry.get();
  std::lock_guard lock(watch_mutex_);
  physical_memory_invalidation_callbacks_.push_back(std::move(entry));
  return handle;
}

void Memory::UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle) {
  std::unique_ptr<std::pair<PhysicalMemoryInvalidationCallback, void*>> entry;
  {
    std::lock_guard lock(watch_mutex_);
    auto it = std::find_if(
        physical_memory_invalidation_callbacks_.begin(),
        physical_memory_invalidation_callbacks_.end(),
        [callback_handle](const auto& candidate) { return candidate.get() == callback_handle; });
    REX_ASSERT(it != physical_memory_invalidation_callbacks_.end());
    if (it != physical_memory_invalidation_callbacks_.end()) {
      entry = std::move(*it);
      physical_memory_invalidation_callbacks_.erase(it);
    }
  }
}

void Memory::EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                                 bool enable_invalidation_notifications,
                                                 bool enable_data_providers) {
  heaps_.vA0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications, enable_data_providers);
  heaps_.vC0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications, enable_data_providers);
  heaps_.vE0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications, enable_data_providers);
}

uint32_t Memory::SystemHeapAlloc(uint32_t size, uint32_t alignment, uint32_t system_heap_flags) {
  // TODO(benvanik): lightweight pool.
  bool is_physical = !!(system_heap_flags & runtime::kSystemHeapPhysical);
  auto heap = LookupHeapByType(is_physical, 4096);
  uint32_t address;
  if (!heap->AllocSystemHeap(
          size, alignment, runtime::kMemoryAllocationReserve | runtime::kMemoryAllocationCommit,
          runtime::kMemoryProtectRead | runtime::kMemoryProtectWrite, false, &address)) {
    return 0;
  }
  Zero(address, size);
  return address;
}

void Memory::SystemHeapFree(uint32_t address, uint32_t* out_region_size) {
  if (!address) {
    return;
  }
  // TODO(benvanik): lightweight pool.
  auto heap = LookupHeap(address);
  heap->Release(address, out_region_size);
}

void Memory::GetHeapsPageStatsSummary(const BaseHeap* const* provided_heaps, size_t heaps_count,
                                      uint32_t& unreserved_pages, uint32_t& reserved_pages,
                                      uint32_t& used_pages, uint32_t& reserved_bytes) {
  // Heap array is fixed after init; page counts are approximate statistics.
  for (size_t i = 0; i < heaps_count; i++) {
    const BaseHeap* heap = provided_heaps[i];
    uint32_t heap_unreserved = heap->unreserved_page_count();
    uint32_t heap_reserved = heap->reserved_page_count();

    unreserved_pages += heap_unreserved;
    reserved_pages += heap_reserved;
    used_pages += ((heap->total_page_count() - heap_unreserved) * heap->page_size()) / 4096;
    reserved_bytes += heap_reserved * heap->page_size();
  }
}

void Memory::SetBudget(u32 megabytes) {
  budget_pages_.store(static_cast<u32>(std::min<u64>(u64{megabytes} * 256, UINT32_MAX)),
                      std::memory_order_relaxed);
}

u32 Memory::title_pages() const {
  return title_pages_.load(std::memory_order_relaxed);
}

u32 Memory::budget_pages() const {
  return budget_pages_.load(std::memory_order_relaxed);
}

bool Memory::AllocateTitlePage(u32 pages) {
  const u32 budget = budget_pages_.load(std::memory_order_relaxed);
  u32 used = title_pages_.load(std::memory_order_relaxed);
  do {
    if (budget && used + pages > budget) {
      return false;
    }
  } while (!title_pages_.compare_exchange_weak(used, used + pages, std::memory_order_relaxed));
  return true;
}

void Memory::ReleaseTitlePage(u32 pages) {
  title_pages_.fetch_sub(pages, std::memory_order_relaxed);
}

//=============================================================================
// Recompiled Code Function Table
//=============================================================================

bool Memory::InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,
                                     uint32_t image_size) {
  constexpr uint32_t kThunkReserveSize = runtime::FunctionDispatcher::kThunkReserveSize;
  uint32_t table_base = image_base + image_size;
  uint32_t table_size = (code_size + kThunkReserveSize) * 2;

  REXLOG_DEBUG(system,
               "Initializing function table at {:08X}, size {:08X} for code {:08X}-{:08X} "
               "(+{:08X} thunk reserve)",
               table_base, table_size, code_base, code_base + code_size, kThunkReserveSize);

  if (!heaps_.v80000000.AllocFixed(
          table_base, table_size, 0x10000,
          runtime::kMemoryAllocationReserve | runtime::kMemoryAllocationCommit,
          runtime::kMemoryProtectRead | runtime::kMemoryProtectWrite)) {
    REX_FAIL(system, false, "Failed to allocate function table at {:08X}", table_base);
  }

  Zero(table_base, table_size);

  std::lock_guard lock(function_tables_mutex_);
  function_tables_.push_back({
      .table_base = table_base,
      .code_base = code_base,
      .code_size = code_size,
      .thunk_reserve = kThunkReserveSize,
  });

  return true;
}

bool Memory::DestroyFunctionTable(uint32_t code_base) {
  uint32_t table_base = 0;
  uint32_t table_size = 0;
  uint32_t logged_code_base = 0;
  uint32_t logged_code_size = 0;
  {
    std::lock_guard lock(function_tables_mutex_);
    auto it = std::find_if(
        function_tables_.begin(), function_tables_.end(),
        [code_base](const FunctionTableEntry& entry) { return entry.code_base == code_base; });
    if (it == function_tables_.end()) {
      return false;
    }
    table_base = it->table_base;
    table_size = (it->code_size + it->thunk_reserve) * 2;
    logged_code_base = it->code_base;
    logged_code_size = it->code_size;
    function_tables_.erase(it);
  }

  REXLOG_DEBUG(system, "Destroying function table at {:08X}, size {:08X} for code {:08X}-{:08X}",
               table_base, table_size, logged_code_base, logged_code_base + logged_code_size);

  heaps_.v80000000.Release(table_base);
  return true;
}

bool Memory::SetFunction(uint32_t guest_address, PPCFunc* host_function) {
  uint32_t table_address = 0;
  {
    std::lock_guard lock(function_tables_mutex_);
    for (const auto& entry : function_tables_) {
      uint32_t range_end = entry.code_base + entry.code_size + entry.thunk_reserve;
      if (guest_address >= entry.code_base && guest_address < range_end) {
        uint64_t offset = (static_cast<uint64_t>(guest_address) - entry.code_base) * 2;
        table_address = entry.table_base + static_cast<uint32_t>(offset);
        break;
      }
    }
  }
  if (!table_address) {
    return false;
  }
  auto* slot = TranslateVirtual<PPCFunc**>(table_address);
  *slot = host_function;
  return true;
}

rex::memory::PageAccess ToPageAccess(uint32_t protect) {
  bool is_writable =
      (protect & runtime::kMemoryProtectWrite) || (protect & runtime::kMemoryProtectWriteCombine);

  if ((protect & runtime::kMemoryProtectRead) && !is_writable) {
    return rex::memory::PageAccess::kReadOnly;
  } else if ((protect & runtime::kMemoryProtectRead) && is_writable) {
    return rex::memory::PageAccess::kReadWrite;
  } else {
    return rex::memory::PageAccess::kNoAccess;
  }
}

uint32_t FromPageAccess(rex::memory::PageAccess protect) {
  switch (protect) {
    case memory::PageAccess::kNoAccess:
      return runtime::kMemoryProtectNoAccess;
    case memory::PageAccess::kReadOnly:
      return runtime::kMemoryProtectRead;
    case memory::PageAccess::kReadWrite:
      return runtime::kMemoryProtectRead | runtime::kMemoryProtectWrite;
    case memory::PageAccess::kExecuteReadOnly:
      // Guest memory cannot be executable - this should never happen :)
      REX_ASSERT_ALWAYS();
      return runtime::kMemoryProtectRead;
    case memory::PageAccess::kExecuteReadWrite:
      // Guest memory cannot be executable - this should never happen :)
      REX_ASSERT_ALWAYS();
      return runtime::kMemoryProtectRead | runtime::kMemoryProtectWrite;
  }

  return runtime::kMemoryProtectNoAccess;
}

BaseHeap::BaseHeap() : membase_(nullptr), heap_base_(0), heap_size_(0), page_size_(0) {}

BaseHeap::~BaseHeap() = default;

std::unique_lock<rex::platform::RankedMutex> BaseHeap::AcquireHostPageReconcileLock() const {
  if (page_size_ >= memory_->system_page_size_) {
    return {};
  }
  return std::unique_lock(memory_->watch_mutex_);
}

bool BaseHeap::SyncHostPageAccess(uint32_t start_page_number, uint32_t end_page_number) {
  const uint32_t host_page_size = memory_->system_page_size_;
  if (page_size_ >= host_page_size) {
    return true;
  }

  // Multiple guest pages may share one host page. Keep the guest page table as
  // the source of truth, then grant the host page the union of the access needed
  // by its committed guest pages. This can't enforce stricter access for one
  // guest page while a neighbor needs broader access, but it keeps neighboring
  // committed pages usable and allows guest protection metadata to stay exact.
  const auto get_host_page_access = [&](uint32_t host_page_number) {
    const uint64_t host_page_start = static_cast<uint64_t>(host_page_number) * host_page_size;
    const uint64_t host_page_end = host_page_start + host_page_size;
    const uint64_t heap_host_start = host_address_offset_;
    const uint64_t heap_host_end = heap_host_start + heap_size_;
    const uint64_t overlap_start = std::max(host_page_start, heap_host_start);
    const uint64_t overlap_end = std::min(host_page_end, heap_host_end);
    if (overlap_start >= overlap_end) {
      return rex::memory::PageAccess::kNoAccess;
    }

    const uint32_t guest_page_start =
        static_cast<uint32_t>(overlap_start - heap_host_start) >> page_size_shift_;
    const uint32_t guest_page_end =
        static_cast<uint32_t>(overlap_end - heap_host_start - 1) >> page_size_shift_;

    bool has_read = false;
    bool has_write = false;
    bool has_committed = false;
    for (uint32_t page_number = guest_page_start; page_number <= guest_page_end; ++page_number) {
      const auto& page_entry = page_table_[page_number];
      if (!(page_entry.state & runtime::kMemoryAllocationCommit)) {
        continue;
      }
      has_committed = true;
      const rex::memory::PageAccess page_access = ToPageAccess(page_entry.current_protect);
      if (page_access == rex::memory::PageAccess::kReadWrite) {
        has_read = true;
        has_write = true;
        break;
      }
      if (page_access == rex::memory::PageAccess::kReadOnly) {
        has_read = true;
      }
    }

    rex::memory::PageAccess access;
    // Physical aliases are intentionally left accessible after release unless
    // protect_on_release is enabled because GPU users may still reference them.
    if (!has_committed && heap_type_ == runtime::HeapType::kGuestPhysical &&
        !REXCVAR_GET(::rex::runtime::cvars::protect_on_release)) {
      access = rex::memory::PageAccess::kReadWrite;
    } else if (!has_read) {
      access = rex::memory::PageAccess::kNoAccess;
    } else {
      access = has_write ? rex::memory::PageAccess::kReadWrite : rex::memory::PageAccess::kReadOnly;
    }

    // The guest page table is not the whole truth: a physical heap page may
    // also be write-watched for GPU invalidation. EnableAccessCallbacks stores
    // that in notify_on_invalidation and skips any page whose bit is already
    // set, so handing write access back here would stop the watch firing
    // permanently. One host page covers several guest pages, so an unrelated
    // operation on a neighbour reaches this code constantly - clamp instead.
    // Leaving the page read-only can only cause a spurious invalidation, which
    // is safe; restoring write access loses invalidations, which is not.
    if (access == rex::memory::PageAccess::kReadWrite && IsHostPageWriteWatched(host_page_number)) {
      access = rex::memory::PageAccess::kReadOnly;
    }
    return access;
  };

  const uint64_t guest_byte_start = static_cast<uint64_t>(start_page_number) << page_size_shift_;
  const uint64_t guest_byte_end =
      ((static_cast<uint64_t>(end_page_number) + 1) << page_size_shift_) - 1;
  const uint32_t host_page_first =
      static_cast<uint32_t>((guest_byte_start + host_address_offset_) / host_page_size);
  const uint32_t host_page_last =
      static_cast<uint32_t>((guest_byte_end + host_address_offset_) / host_page_size);

  // Coalesce neighbouring host pages that resolve to the same access into one
  // Protect call. A bulk commit spans thousands of host pages and almost always
  // wants a single uniform access for all of them, so protecting page by page
  // would cost thousands of syscalls where one suffices.
  uint8_t* const heap_host_base = membase_ + heap_base_;
  uint32_t run_first_page = host_page_first;
  rex::memory::PageAccess run_access = get_host_page_access(host_page_first);
  const auto flush_run = [&](uint32_t run_last_page) {
    const size_t run_length =
        (static_cast<size_t>(run_last_page - run_first_page) + 1) * host_page_size;
    uint8_t* const run_pointer =
        heap_host_base + static_cast<size_t>(run_first_page) * host_page_size;
    if (!rex::memory::Protect(run_pointer, run_length, run_access, nullptr)) {
      REX_FAIL(system, false,
               "BaseHeap::SyncHostPageAccess failed for heap {:08X} host pages {}-{}", heap_base_,
               run_first_page, run_last_page);
    }
    return true;
  };

  for (uint32_t host_page = host_page_first + 1; host_page <= host_page_last; ++host_page) {
    const rex::memory::PageAccess access = get_host_page_access(host_page);
    if (access == run_access) {
      continue;
    }
    if (!flush_run(host_page - 1)) {
      return false;
    }
    run_first_page = host_page;
    run_access = access;
  }
  return flush_run(host_page_last);
}

void BaseHeap::Initialize(runtime::Memory* memory, uint8_t* membase, HeapType heap_type,
                          uint32_t heap_base, uint32_t heap_size, uint32_t page_size,
                          uint32_t host_address_offset) {
  memory_ = memory;
  membase_ = membase;
  heap_type_ = heap_type;
  heap_base_ = heap_base;
  heap_size_ = heap_size;
  page_size_ = page_size;
  REX_ASSERT(rex::is_pow2(page_size_));
  page_size_shift_ = rex::log2_floor(page_size_);
  host_address_offset_ = host_address_offset;
  page_table_.resize(heap_size / page_size);
  unreserved_page_count_ = static_cast<uint32_t>(page_table_.size());
}

void BaseHeap::Dispose() {
  // Walk table and release all regions.
  for (uint32_t page_number = 0; page_number < page_table_.size(); ++page_number) {
    auto& page_entry = page_table_[page_number];
    if (page_entry.state) {
      rex::memory::DeallocFixed(TranslateRelative(page_number << page_size_shift_), 0,
                                rex::memory::DeallocationType::kRelease);
      page_number += page_entry.region_page_count;
    }
  }
}

bool BaseHeap::Alloc(uint32_t size, uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                     bool top_down, uint32_t* out_address) {
  *out_address = 0;
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  // Reserve address space at the top for thread stacks.
  // 0x3XXXXXXX is for system threads, 0x7XXXXXXX is for title threads.
  uint32_t heap_virtual_guest_offset = 0;
  if (heap_type_ == runtime::HeapType::kGuestVirtual) {
    heap_virtual_guest_offset = 0x10000000;
    if (page_size_ == 0x10000) {
      heap_virtual_guest_offset = 0x0F000000;
    }
  }

  uint32_t low_address = heap_base_;
  uint32_t high_address = heap_base_ + (heap_size_ - 1) - heap_virtual_guest_offset;
  return AllocRange(low_address, high_address, size, alignment, allocation_type, protect, top_down,
                    out_address);
}

bool BaseHeap::AllocSystemHeap(uint32_t size, uint32_t alignment, uint32_t allocation_type,
                               uint32_t protect, bool top_down, uint32_t* out_address) {
  *out_address = 0;
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  uint32_t low_address = heap_base_;
  if (heap_type_ == runtime::HeapType::kGuestVirtual) {
    low_address = heap_base_ + heap_size_ - 0x10000000;
  }
  uint32_t high_address = heap_base_ + (heap_size_ - 1);
  return AllocRange(low_address, high_address, size, alignment, allocation_type, protect, top_down,
                    out_address);
}

bool BaseHeap::AllocFixed(uint32_t base_address, uint32_t size, uint32_t alignment,
                          uint32_t allocation_type, uint32_t protect) {
  PendingScribble scribble(allocation_type, protect);
  auto reconcile_lock = AcquireHostPageReconcileLock();
  if (!AllocFixedLocked(base_address, size, alignment, allocation_type, protect)) {
    return false;
  }
  scribble.Add(*this, base_address, size);
  return true;
}

bool BaseHeap::AllocFixedLocked(uint32_t base_address, uint32_t size, uint32_t alignment,
                                uint32_t allocation_type, uint32_t protect) {
  const bool title_memory = (allocation_type & runtime::kMemoryAllocationTitle) != 0;
  allocation_type &= ~static_cast<u32>(runtime::kMemoryAllocationTitle);
  alignment = rex::round_up(alignment, page_size_);
  if (base_address % alignment != 0) {
    if (base_address % page_size_ != 0) {
      REX_FAIL(system, false,
               "BaseHeap::AllocFixed invalid base alignment: base={:08X} page_size={:08X} "
               "requested_alignment={:08X} heap={:08X}-{:08X}",
               base_address, page_size_, alignment, heap_base_, heap_base_ + (heap_size_ - 1));
    }
    // Fixed allocations can only be guaranteed page-aligned. If callers provide
    // a stricter alignment for an already-fixed address, fall back to page size.
    REXLOG_WARN(system,
                "BaseHeap::AllocFixed clamping alignment from {:08X} to page size {:08X} for "
                "base={:08X}",
                alignment, page_size_, base_address);
    alignment = page_size_;
  }
  size = rex::align(size, alignment);
  uint32_t page_count = get_page_count(size, page_size_, page_size_shift_);
  uint32_t start_page_number = (base_address - heap_base_) >> page_size_shift_;
  uint32_t end_page_number = start_page_number + page_count - 1;
  if (start_page_number >= page_table_.size() || end_page_number >= page_table_.size()) {
    REX_FAIL(system, false, "BaseHeap::AllocFixed passed out of range address range");
  }

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // - If we are reserving the entire range requested must not be already
  //   reserved.
  // - If we are committing it's ok for pages within the range to already be
  //   committed.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    uint32_t state = page_table_[page_number].state;
    if ((allocation_type == runtime::kMemoryAllocationReserve) && state) {
      // Already reserved.
      REX_FAIL(system, false,
               "BaseHeap::AllocFixed attempting to reserve an already reserved "
               "range");
    }
    if ((allocation_type == runtime::kMemoryAllocationCommit) &&
        !(state & runtime::kMemoryAllocationReserve)) {
      // Attempting a commit-only op on an unreserved page.
      // This may be OK.
      REXLOG_WARN(system, "BaseHeap::AllocFixed attempting commit on unreserved page");
      allocation_type |= runtime::kMemoryAllocationReserve;
      break;
    }
  }

  u32 title_pages = 0;
  if (title_memory && (allocation_type & runtime::kMemoryAllocationCommit)) {
    const auto range = std::span(page_table_).subspan(start_page_number, page_count);
    const auto uncommitted = std::ranges::count_if(range, [](const PageEntry& page_entry) {
      return (page_entry.state & runtime::kMemoryAllocationCommit) == 0;
    });
    title_pages = static_cast<u32>(uncommitted) * (page_size_ >> 12);
    if (title_pages && !memory_->AllocateTitlePage(title_pages)) {
      REXLOG_WARN(system, "BaseHeap::AllocFixed over the title memory budget");
      return false;
    }
  }

  // Allocate from host.
  if (allocation_type == runtime::kMemoryAllocationReserve) {
    // Reserve is not needed, as we are mapped already.
  } else if (page_size_ >= memory_->system_page_size_) {
    auto alloc_type = (allocation_type & runtime::kMemoryAllocationCommit)
                          ? rex::memory::AllocationType::kCommit
                          : rex::memory::AllocationType::kReserve;
    void* result =
        rex::memory::AllocFixed(TranslateRelative(start_page_number << page_size_shift_),
                                page_count << page_size_shift_, alloc_type, ToPageAccess(protect));
    if (!result) {
      memory_->ReleaseTitlePage(title_pages);
      REX_FAIL(system, false, "BaseHeap::AllocFixed failed to alloc range from host");
    }
  }

  // Set page state.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    if (title_pages && (page_entry.state & runtime::kMemoryAllocationCommit) == 0) {
      page_entry.title = 1;
    }
    if (allocation_type & runtime::kMemoryAllocationReserve) {
      if (!page_entry.state) {
        unreserved_page_count_--;
      }
      // Region is based on reservation.
      page_entry.base_address = start_page_number;
      page_entry.region_page_count = page_count;
    }
    page_entry.allocation_protect = protect;
    page_entry.current_protect = protect;
    page_entry.state = runtime::kMemoryAllocationReserve | allocation_type;
  }

  if (!SyncHostPageAccess(start_page_number, end_page_number)) {
    return false;
  }

  return true;
}

bool BaseHeap::AllocRange(uint32_t low_address, uint32_t high_address, uint32_t size,
                          uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                          bool top_down, uint32_t* out_address) {
  PendingScribble scribble(allocation_type, protect);
  auto reconcile_lock = AcquireHostPageReconcileLock();
  if (!AllocRangeLocked(low_address, high_address, size, alignment, allocation_type, protect,
                        top_down, out_address)) {
    return false;
  }
  scribble.Add(*this, *out_address, size);
  return true;
}

bool BaseHeap::AllocRangeLocked(uint32_t low_address, uint32_t high_address, uint32_t size,
                                uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                                bool top_down, uint32_t* out_address) {
  *out_address = 0;
  const bool title_memory = (allocation_type & runtime::kMemoryAllocationTitle) != 0;
  allocation_type &= ~static_cast<u32>(runtime::kMemoryAllocationTitle);

  alignment = rex::round_up(alignment, page_size_);
  uint32_t page_count = get_page_count(size, page_size_, page_size_shift_);
  low_address = std::max(heap_base_, rex::align(low_address, alignment));
  high_address = std::min(heap_base_ + (heap_size_ - 1), rex::align(high_address, alignment));
  if (high_address < low_address) {
    REX_FAIL(system, false, "BaseHeap::Alloc invalid requested range");
  }
  uint32_t low_page_number = (low_address - heap_base_) >> page_size_shift_;
  uint32_t high_page_number = (high_address - heap_base_) >> page_size_shift_;
  low_page_number = std::min(static_cast<uint32_t>(page_table_.size()) - 1, low_page_number);
  high_page_number = std::min(static_cast<uint32_t>(page_table_.size()) - 1, high_page_number);
  if (high_page_number < low_page_number) {
    REX_FAIL(system, false, "BaseHeap::Alloc invalid requested page range");
  }

  uint32_t available_page_count = high_page_number - low_page_number + 1;
  if (!page_count || page_count > available_page_count) {
    REX_FAIL(system, false, "BaseHeap::Alloc page count too big for requested range");
  }

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Find a free page range.
  // The base page must match the requested alignment, so we first scan for
  // a free aligned page and only then check for continuous free pages.
  // TODO(benvanik): optimized searching (free list buckets or a bitmap).
  uint32_t start_page_number = UINT_MAX;
  uint32_t end_page_number = UINT_MAX;
  uint32_t page_scan_stride = alignment >> page_size_shift_;
  uint32_t max_base_page_number = high_page_number + 1 - page_count;
  if (top_down) {
    max_base_page_number -= max_base_page_number % page_scan_stride;
    for (int64_t base_page_number = max_base_page_number; base_page_number >= low_page_number;
         base_page_number -= page_scan_stride) {
      if (page_table_[base_page_number].state != 0) {
        // Base page not free, skip to next usable page.
        continue;
      }
      // Check requested range to ensure free.
      start_page_number = static_cast<uint32_t>(base_page_number);
      end_page_number = static_cast<uint32_t>(base_page_number) + page_count - 1;
      REX_ASSERT(end_page_number < page_table_.size());
      bool any_taken = false;
      for (uint32_t page_number = static_cast<uint32_t>(base_page_number);
           !any_taken && page_number <= end_page_number; ++page_number) {
        bool is_free = page_table_[page_number].state == 0;
        if (!is_free) {
          // At least one page in the range is used, skip to next.
          // We know we'll be starting at least before this page.
          any_taken = true;
          if (page_count > page_number) {
            // Not enough space left to fit entire page range. Breaks outer
            // loop.
            base_page_number = -1;
          } else {
            base_page_number = page_number - page_count;
            base_page_number -= base_page_number % page_scan_stride;
            base_page_number += page_scan_stride;  // cancel out loop logic
          }
          break;
        }
      }
      if (!any_taken) {
        // Found our place.
        break;
      }
      // Retry.
      start_page_number = end_page_number = UINT_MAX;
    }
  } else {
    for (uint32_t base_page_number = low_page_number; base_page_number <= max_base_page_number;
         base_page_number += page_scan_stride) {
      if (page_table_[base_page_number].state != 0) {
        // Base page not free, skip to next usable page.
        continue;
      }
      // Check requested range to ensure free.
      start_page_number = base_page_number;
      end_page_number = base_page_number + page_count - 1;
      bool any_taken = false;
      for (uint32_t page_number = base_page_number; !any_taken && page_number <= end_page_number;
           ++page_number) {
        bool is_free = page_table_[page_number].state == 0;
        if (!is_free) {
          // At least one page in the range is used, skip to next.
          // We know we'll be starting at least after this page.
          any_taken = true;
          base_page_number = rex::round_up(page_number + 1, page_scan_stride);
          base_page_number -= page_scan_stride;  // cancel out loop logic
          break;
        }
      }
      if (!any_taken) {
        // Found our place.
        break;
      }
      // Retry.
      start_page_number = end_page_number = UINT_MAX;
    }
  }
  if (start_page_number == UINT_MAX || end_page_number == UINT_MAX) {
    REXLOG_WARN(system, "BaseHeap::Alloc failed to find contiguous range");
    return false;
  }
  const u32 title_pages = title_memory && (allocation_type & runtime::kMemoryAllocationCommit)
                              ? page_count * (page_size_ >> 12)
                              : 0;
  if (title_pages && !memory_->AllocateTitlePage(title_pages)) {
    REXLOG_WARN(system, "BaseHeap::Alloc over the title memory budget");
    return false;
  }

  // Allocate from host.
  if (allocation_type == runtime::kMemoryAllocationReserve) {
    // Reserve is not needed, as we are mapped already.
  } else if (page_size_ >= memory_->system_page_size_) {
    auto alloc_type = (allocation_type & runtime::kMemoryAllocationCommit)
                          ? rex::memory::AllocationType::kCommit
                          : rex::memory::AllocationType::kReserve;
    void* result =
        rex::memory::AllocFixed(TranslateRelative(start_page_number << page_size_shift_),
                                page_count << page_size_shift_, alloc_type, ToPageAccess(protect));
    if (!result) {
      memory_->ReleaseTitlePage(title_pages);
      REX_FAIL(system, false, "BaseHeap::Alloc failed to alloc range from host");
    }
  }

  // Set page state.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    if (!page_entry.state) {
      unreserved_page_count_--;
    }
    page_entry.base_address = start_page_number;
    page_entry.region_page_count = page_count;
    page_entry.allocation_protect = protect;
    page_entry.current_protect = protect;
    page_entry.state = runtime::kMemoryAllocationReserve | allocation_type;
    page_entry.title = title_pages != 0;
  }

  if (!SyncHostPageAccess(start_page_number, end_page_number)) {
    return false;
  }

  *out_address = heap_base_ + (start_page_number << page_size_shift_);
  return true;
}

bool BaseHeap::Decommit(uint32_t address, uint32_t size) {
  auto reconcile_lock = AcquireHostPageReconcileLock();
  return DecommitLocked(address, size);
}

bool BaseHeap::DecommitLocked(uint32_t address, uint32_t size) {
  uint32_t page_count = get_page_count(size, page_size_, page_size_shift_);
  uint32_t start_page_number = (address - heap_base_) >> page_size_shift_;
  uint32_t end_page_number = start_page_number + page_count - 1;
  start_page_number = std::min(static_cast<uint32_t>(page_table_.size()) - 1, start_page_number);
  end_page_number = std::min(static_cast<uint32_t>(page_table_.size()) - 1, end_page_number);

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Release from host.
  // TODO(benvanik): find a way to decommit memory;
  //     mapped memory cannot be decommitted.
  /*BOOL result =
      VirtualFree(TranslateRelative(start_page_number << page_size_shift_),
                  page_count << page_size_shift_, MEM_DECOMMIT);
  if (!result) {
    PLOGW("BaseHeap::Decommit failed due to host VirtualFree failure");
    return false;
  }*/

  u32 released = 0;
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    released += page_entry.title;
    page_entry.title = 0;
    page_entry.state &= ~runtime::kMemoryAllocationCommit;
  }
  memory_->ReleaseTitlePage(released * (page_size_ >> 12));

  if (!SyncHostPageAccess(start_page_number, end_page_number)) {
    return false;
  }

  return true;
}

bool BaseHeap::Release(uint32_t base_address, uint32_t* out_region_size) {
  auto reconcile_lock = AcquireHostPageReconcileLock();
  return ReleaseLocked(base_address, out_region_size);
}

bool BaseHeap::ReleaseLocked(uint32_t base_address, uint32_t* out_region_size) {
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Given address must be a region base address.
  uint32_t base_page_number = (base_address - heap_base_) >> page_size_shift_;
  auto base_page_entry = page_table_[base_page_number];
  if (!base_page_entry.state || base_page_entry.base_address != base_page_number) {
    REX_FAIL(system, false, "BaseHeap::Release failed because address is not a region start");
  }

  if (heap_base_ == 0x00000000 && base_page_number == 0) {
    REX_FAIL(system, false, "BaseHeap::Release: Attempt to free 0!");
  }

  if (out_region_size) {
    *out_region_size = (base_page_entry.region_page_count << page_size_shift_);
  }

  // Release from host not needed as mapping reserves the range for us.
  // TODO(benvanik): protect with NOACCESS?
  /*BOOL result = VirtualFree(
      TranslateRelative(base_page_number << page_size_shift_), 0, MEM_RELEASE);
  if (!result) {
    PLOGE("BaseHeap::Release failed due to host VirtualFree failure");
    return false;
  }*/
  // Instead, we protect it, if we can.
  if (page_size_ == rex::memory::page_size() ||
      ((base_page_entry.region_page_count << page_size_shift_) % rex::memory::page_size() == 0 &&
       ((base_page_number << page_size_shift_) % rex::memory::page_size() == 0))) {
    // TODO(benvanik): figure out why games are using memory after releasing
    // it. It's possible this is some virtual/physical stuff where the GPU
    // still can access it.
    if (REXCVAR_GET(::rex::runtime::cvars::protect_on_release)) {
      if (!rex::memory::Protect(TranslateRelative(base_page_number << page_size_shift_),
                                base_page_entry.region_page_count << page_size_shift_,
                                rex::memory::PageAccess::kNoAccess, nullptr)) {
        REXLOG_WARN(system, "BaseHeap::Release failed due to host VirtualProtect failure");
      }
    }
  }

  uint32_t end_page_number = base_page_number + base_page_entry.region_page_count - 1;
  u32 released = 0;
  for (uint32_t page_number = base_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    released += page_entry.title;
    page_entry.qword = 0;
    unreserved_page_count_++;
  }
  memory_->ReleaseTitlePage(released * (page_size_ >> 12));

  if (!SyncHostPageAccess(base_page_number, end_page_number)) {
    return false;
  }

  return true;
}

bool BaseHeap::Protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t* old_protect) {
  auto reconcile_lock = AcquireHostPageReconcileLock();
  return ProtectLocked(address, size, protect, old_protect);
}

bool BaseHeap::ProtectLocked(uint32_t address, uint32_t size, uint32_t protect,
                             uint32_t* old_protect) {
  if (!size) {
    REX_FAIL(system, false, "BaseHeap::Protect failed due to zero size");
  }

  // From the VirtualProtect MSDN page:
  //
  // "The region of affected pages includes all pages containing one or more
  //  bytes in the range from the lpAddress parameter to (lpAddress+dwSize).
  //  This means that a 2-byte range straddling a page boundary causes the
  //  protection attributes of both pages to be changed."
  //
  // "The access protection value can be set only on committed pages. If the
  //  state of any page in the specified region is not committed, the function
  //  fails and returns without modifying the access protection of any pages in
  //  the specified region."

  uint32_t start_page_number = (address - heap_base_) >> page_size_shift_;
  if (start_page_number >= page_table_.size()) {
    REX_FAIL(system, false, "BaseHeap::Protect failed due to out-of-bounds base address {:08X}",
             address);
  }
  uint32_t end_page_number = static_cast<uint32_t>(
      (static_cast<uint64_t>(address) + size - 1 - heap_base_) >> page_size_shift_);
  if (end_page_number >= page_table_.size()) {
    REX_FAIL(system, false,
             "BaseHeap::Protect failed due to out-of-bounds range ({:08X} bytes "
             "from {:08x})",
             size, address);
  }

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Ensure all pages are in the same reserved region and all are committed.
  uint32_t first_base_address = UINT_MAX;
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto page_entry = page_table_[page_number];
    if (first_base_address == UINT_MAX) {
      first_base_address = page_entry.base_address;
    } else if (first_base_address != page_entry.base_address) {
      REX_FAIL(system, false, "BaseHeap::Protect failed due to request spanning regions");
    }
    if (!(page_entry.state & runtime::kMemoryAllocationCommit)) {
      REX_FAIL(system, false, "BaseHeap::Protect failed due to uncommitted page");
    }
  }

  // Change host protection directly when guest pages are at least as large as
  // host pages. Smaller guest pages are reconciled after the page table update.
  if (page_size_ >= memory_->system_page_size_) {
    const uint32_t page_count = end_page_number - start_page_number + 1;
    memory::PageAccess old_protect_access;
    if (!rex::memory::Protect(TranslateRelative(start_page_number << page_size_shift_),
                              page_count << page_size_shift_, ToPageAccess(protect),
                              old_protect ? &old_protect_access : nullptr)) {
      REX_FAIL(system, false, "BaseHeap::Protect failed due to host VirtualProtect failure");
    }

    if (old_protect) {
      *old_protect = FromPageAccess(old_protect_access);
    }
  } else {
    if (old_protect) {
      *old_protect = page_table_[start_page_number].current_protect;
    }
  }

  // Perform table change.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto& page_entry = page_table_[page_number];
    page_entry.current_protect = protect;
  }

  if (!SyncHostPageAccess(start_page_number, end_page_number)) {
    return false;
  }

  return true;
}

bool BaseHeap::QueryRegionInfo(uint32_t base_address, HeapAllocationInfo* out_info) {
  uint32_t start_page_number = (base_address - heap_base_) >> page_size_shift_;
  if (start_page_number > page_table_.size()) {
    REX_FAIL(system, false, "BaseHeap::QueryRegionInfo base page out of range");
  }

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  auto start_page_entry = page_table_[start_page_number];
  out_info->base_address = base_address;
  out_info->allocation_base = 0;
  out_info->allocation_protect = 0;
  out_info->region_size = 0;
  out_info->state = 0;
  out_info->protect = 0;
  if (start_page_entry.state) {
    // Committed/reserved region.
    out_info->allocation_base = heap_base_ + (start_page_entry.base_address << page_size_shift_);
    out_info->allocation_protect = start_page_entry.allocation_protect;
    out_info->allocation_size = start_page_entry.region_page_count << page_size_shift_;
    out_info->state = start_page_entry.state;
    out_info->protect = start_page_entry.current_protect;

    // Scan forward and report the size of the region matching the initial
    // base address's attributes.
    for (uint32_t page_number = start_page_number;
         page_number < start_page_entry.base_address + start_page_entry.region_page_count;
         ++page_number) {
      auto page_entry = page_table_[page_number];
      if (page_entry.base_address != start_page_entry.base_address ||
          page_entry.state != start_page_entry.state ||
          page_entry.current_protect != start_page_entry.current_protect) {
        // Different region or different properties within the region; done.
        break;
      }
      out_info->region_size += page_size_;
    }
  } else {
    // Free region.
    for (uint32_t page_number = start_page_number; page_number < page_table_.size();
         ++page_number) {
      auto page_entry = page_table_[page_number];
      if (page_entry.state) {
        // First non-free page; done with region.
        break;
      }
      out_info->region_size += page_size_;
    }
  }
  return true;
}

bool BaseHeap::QuerySize(uint32_t address, uint32_t* out_size) {
  uint32_t page_number = (address - heap_base_) >> page_size_shift_;
  if (page_number > page_table_.size()) {
    REXLOG_ERROR(system, "BaseHeap::QuerySize base page out of range");
    *out_size = 0;
    return false;
  }
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  auto page_entry = page_table_[page_number];
  *out_size = (page_entry.region_page_count << page_size_shift_);
  return true;
}

bool BaseHeap::QueryProtect(uint32_t address, uint32_t* out_protect) {
  uint32_t page_number = (address - heap_base_) >> page_size_shift_;
  if (page_number > page_table_.size()) {
    REXLOG_ERROR(system, "BaseHeap::QueryProtect base page out of range");
    *out_protect = 0;
    return false;
  }
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  auto page_entry = page_table_[page_number];
  *out_protect = page_entry.current_protect;
  return true;
}

rex::memory::PageAccess BaseHeap::QueryRangeAccess(uint32_t low_address, uint32_t high_address) {
  if (low_address > high_address || low_address < heap_base_ ||
      (high_address - heap_base_) >= heap_size_) {
    return rex::memory::PageAccess::kNoAccess;
  }
  uint32_t low_page_number = (low_address - heap_base_) >> page_size_shift_;
  uint32_t high_page_number = (high_address - heap_base_) >> page_size_shift_;
  bool all_readable = true;
  bool all_writable = true;
  {
    std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
    for (uint32_t i = low_page_number; i <= high_page_number; ++i) {
      uint32_t page_protect = page_table_[i].current_protect;
      if (!(page_protect & runtime::kMemoryProtectRead)) {
        all_readable = false;
      }
      if (!(page_protect & runtime::kMemoryProtectWrite) &&
          !(page_protect & runtime::kMemoryProtectWriteCombine)) {
        all_writable = false;
      }
    }
  }
  if (all_readable && all_writable) {
    return rex::memory::PageAccess::kReadWrite;
  } else if (all_readable) {
    return rex::memory::PageAccess::kReadOnly;
  } else {
    return rex::memory::PageAccess::kNoAccess;
  }
}

VirtualHeap::VirtualHeap() = default;

VirtualHeap::~VirtualHeap() = default;

void VirtualHeap::Initialize(runtime::Memory* memory, uint8_t* membase, HeapType heap_type,
                             uint32_t heap_base, uint32_t heap_size, uint32_t page_size) {
  BaseHeap::Initialize(memory, membase, heap_type, heap_base, heap_size, page_size);
}

PhysicalHeap::PhysicalHeap() : parent_heap_(nullptr) {}

PhysicalHeap::~PhysicalHeap() = default;

void PhysicalHeap::Initialize(runtime::Memory* memory, uint8_t* membase, HeapType heap_type,
                              uint32_t heap_base, uint32_t heap_size, uint32_t page_size,
                              VirtualHeap* parent_heap) {
  const uint32_t host_address_offset = detail::PhysicalHostOffset(heap_base);

  BaseHeap::Initialize(memory, membase, heap_type, heap_base, heap_size, page_size,
                       host_address_offset);
  parent_heap_ = parent_heap;
  system_page_size_ = static_cast<uint32_t>(rex::memory::page_size());

  system_page_count_ =
      (static_cast<size_t>(heap_size_) + host_address_offset + (system_page_size_ - 1)) /
      system_page_size_;
  system_page_flags_.resize((system_page_count_ + 63) / 64);
}

bool PhysicalHeap::Alloc(uint32_t size, uint32_t alignment, uint32_t allocation_type,
                         uint32_t protect, bool top_down, uint32_t* out_address) {
  *out_address = 0;

  // Default top-down. Since parent heap is bottom-up this prevents
  // collisions.
  top_down = true;

  // Adjust alignment size our page size differs from the parent.
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  PendingScribble scribble(allocation_type, protect);
  auto reconcile_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Allocate from parent heap (gets our physical address in 0-512mb).
  uint32_t parent_heap_start = GetPhysicalAddress(heap_base_);
  uint32_t parent_heap_end = GetPhysicalAddress(heap_base_ + (heap_size_ - 1));
  uint32_t parent_address;
  if (!parent_heap_->AllocRangeLocked(parent_heap_start, parent_heap_end, size, alignment,
                                      allocation_type, protect, top_down, &parent_address)) {
    REX_FAIL(system, false, "PhysicalHeap::Alloc unable to alloc physical memory in parent heap");
  }

  // Given the address we've reserved in the parent heap, pin that here.
  // Shouldn't be possible for it to be allocated already.
  uint32_t address = heap_base_ + parent_address - parent_heap_start;
  if (!BaseHeap::AllocFixedLocked(
          address, size, alignment,
          allocation_type & ~static_cast<u32>(runtime::kMemoryAllocationTitle), protect)) {
    REXLOG_ERROR(system, "PhysicalHeap::Alloc unable to pin physical memory in physical heap");
    parent_heap_->ReleaseLocked(parent_address, nullptr);
    return false;
  }
  scribble.Add(*parent_heap_, parent_address, size);
  scribble.Add(*this, address, size);
  *out_address = address;
  return true;
}

bool PhysicalHeap::AllocFixed(uint32_t base_address, uint32_t size, uint32_t alignment,
                              uint32_t allocation_type, uint32_t protect) {
  // Adjust alignment size our page size differs from the parent.
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  PendingScribble scribble(allocation_type, protect);
  auto reconcile_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Allocate from parent heap (gets our physical address in 0-512mb).
  uint32_t parent_base_address = GetPhysicalAddress(base_address);
  std::lock_guard<std::recursive_mutex> parent_lock(parent_heap_->heap_mutex_);
  const u32 parent_size = parent_base_address % alignment == 0 ? rex::align(size, alignment) : size;
  const u32 parent_first =
      (parent_base_address - parent_heap_->heap_base_) >> parent_heap_->page_size_shift_;
  const u32 parent_count = parent_size >> parent_heap_->page_size_shift_;
  const bool parent_in_range = parent_first + parent_count <= parent_heap_->page_table_.size();
  const auto parent_pages =
      parent_in_range ? std::span(parent_heap_->page_table_).subspan(parent_first, parent_count)
                      : std::span<PageEntry>();
  const bool parent_was_free =
      parent_in_range && std::ranges::all_of(parent_pages, [](const PageEntry& page_entry) {
        return page_entry.state == 0;
      });
  if (parent_in_range && !parent_was_free &&
      std::ranges::any_of(parent_pages,
                          [](const PageEntry& page_entry) { return page_entry.state == 0; })) {
    REX_FAIL(system, false, "PhysicalHeap::AllocFixed range at {:08X} is partly in use",
             base_address);
  }
  if (!parent_heap_->AllocFixedLocked(parent_base_address, size, alignment, allocation_type,
                                      protect)) {
    REX_FAIL(system, false, "PhysicalHeap::Alloc unable to alloc physical memory in parent heap");
  }

  // Given the address we've reserved in the parent heap, pin that here.
  // Shouldn't be possible for it to be allocated already.
  uint32_t address = heap_base_ + parent_base_address - GetPhysicalAddress(heap_base_);
  if (!BaseHeap::AllocFixedLocked(
          address, size, page_size_,
          allocation_type & ~static_cast<u32>(runtime::kMemoryAllocationTitle), protect)) {
    REXLOG_ERROR(system, "PhysicalHeap::Alloc unable to pin physical memory in physical heap");
    if (parent_was_free) {
      parent_heap_->ReleaseLocked(parent_base_address, nullptr);
    }
    return false;
  }
  scribble.Add(*parent_heap_, parent_base_address, size);
  scribble.Add(*this, address, size);

  return true;
}

bool PhysicalHeap::AllocRange(uint32_t low_address, uint32_t high_address, uint32_t size,
                              uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                              bool top_down, uint32_t* out_address) {
  *out_address = 0;

  // Adjust alignment size our page size differs from the parent.
  size = rex::round_up(size, page_size_);
  alignment = rex::round_up(alignment, page_size_);

  PendingScribble scribble(allocation_type, protect);
  auto reconcile_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Allocate from parent heap (gets our physical address in 0-512mb).
  low_address = std::max(heap_base_, low_address);
  high_address = std::min(heap_base_ + (heap_size_ - 1), high_address);
  uint32_t parent_low_address = GetPhysicalAddress(low_address);
  uint32_t parent_high_address = GetPhysicalAddress(high_address);
  uint32_t parent_address;
  if (!parent_heap_->AllocRangeLocked(parent_low_address, parent_high_address, size, alignment,
                                      allocation_type, protect, top_down, &parent_address)) {
    REX_FAIL(system, false, "PhysicalHeap::Alloc unable to alloc physical memory in parent heap");
  }

  // Given the address we've reserved in the parent heap, pin that here.
  // Shouldn't be possible for it to be allocated already.
  uint32_t address = heap_base_ + parent_address - GetPhysicalAddress(heap_base_);
  if (!BaseHeap::AllocFixedLocked(
          address, size, page_size_,
          allocation_type & ~static_cast<u32>(runtime::kMemoryAllocationTitle), protect)) {
    REXLOG_ERROR(system, "PhysicalHeap::Alloc unable to pin physical memory in physical heap");
    parent_heap_->ReleaseLocked(parent_address, nullptr);
    return false;
  }
  scribble.Add(*parent_heap_, parent_address, size);
  scribble.Add(*this, address, size);
  *out_address = address;
  return true;
}

bool PhysicalHeap::AllocSystemHeap(uint32_t size, uint32_t alignment, uint32_t allocation_type,
                                   uint32_t protect, bool top_down, uint32_t* out_address) {
  return Alloc(size, alignment, allocation_type, protect, top_down, out_address);
}

bool PhysicalHeap::Decommit(uint32_t address, uint32_t size) {
  std::lock_guard lock(memory_->watch_mutex_);

  uint32_t parent_address = GetPhysicalAddress(address);
  if (!parent_heap_->DecommitLocked(parent_address, size)) {
    REX_FAIL(system, false, "PhysicalHeap::Decommit failed due to parent heap failure");
  }

  // Not caring about the contents anymore.
  TriggerCallbacksLocked(address, size, true, true);

  return BaseHeap::DecommitLocked(address, size);
}

bool PhysicalHeap::Release(uint32_t base_address, uint32_t* out_region_size) {
  std::lock_guard lock(memory_->watch_mutex_);

  uint32_t parent_base_address = GetPhysicalAddress(base_address);
  if (!parent_heap_->ReleaseLocked(parent_base_address, out_region_size)) {
    REX_FAIL(system, false, "PhysicalHeap::Release failed due to parent heap failure");
  }

  // Must invalidate here because the range being released may be reused in
  // another mapping of physical memory - but callback flags are set in each
  // heap separately (https://github.com/xenia-project/xenia/issues/1559 -
  // dynamic vertices in 4D5307F2 start screen and menu allocated in 0xA0000000
  // at addresses that overlap intro video textures in 0xE0000000, with the
  // state of the allocator as of February 24th, 2020). If memory is invalidated
  // in Alloc instead, Alloc won't be aware of callbacks enabled in other heaps,
  // thus callback handlers will keep considering this range valid forever.
  uint32_t region_size;
  if (QuerySize(base_address, &region_size)) {
    TriggerCallbacksLocked(base_address, region_size, true, true);
  }

  return BaseHeap::ReleaseLocked(base_address, out_region_size);
}

bool PhysicalHeap::Protect(uint32_t address, uint32_t size, uint32_t protect,
                           uint32_t* old_protect) {
  std::lock_guard lock(memory_->watch_mutex_);

  // Only invalidate if making writable again, for simplicity - not when simply
  // marking some range as immutable, for instance.
  if (protect & runtime::kMemoryProtectWrite) {
    TriggerCallbacksLocked(address, size, true, true, false);
  }

  if (!parent_heap_->ProtectLocked(GetPhysicalAddress(address), size, protect, old_protect)) {
    REX_FAIL(system, false, "PhysicalHeap::Protect failed due to parent heap failure");
  }

  return BaseHeap::ProtectLocked(address, size, protect, nullptr);
}

void PhysicalHeap::EnableAccessCallbacks(uint32_t physical_address, uint32_t length,
                                         bool enable_invalidation_notifications,
                                         bool enable_data_providers) {
  // TODO(Triang3l): Implement data providers.
  REX_ASSERT(!enable_data_providers);
  if (!enable_invalidation_notifications && !enable_data_providers) {
    return;
  }
  uint32_t physical_address_offset = GetPhysicalAddress(heap_base_);
  if (physical_address < physical_address_offset) {
    if (physical_address_offset - physical_address >= length) {
      return;
    }
    length -= physical_address_offset - physical_address;
    physical_address = physical_address_offset;
  }
  uint32_t heap_relative_address = physical_address - physical_address_offset;
  if (heap_relative_address >= heap_size_) {
    return;
  }
  length = std::min(length, heap_size_ - heap_relative_address);
  if (length == 0) {
    return;
  }

  uint32_t system_page_first = (heap_relative_address + host_address_offset()) / system_page_size_;
  uint32_t system_page_last =
      (heap_relative_address + length - 1 + host_address_offset()) / system_page_size_;
  system_page_last = std::min(system_page_last, system_page_count_ - 1);
  REX_ASSERT(system_page_first <= system_page_last);

  // Update callback flags for system pages and make their protection stricter
  // if needed.
  rex::memory::PageAccess protect_access = enable_data_providers
                                               ? rex::memory::PageAccess::kNoAccess
                                               : rex::memory::PageAccess::kReadOnly;
  uint8_t* protect_base = membase_ + heap_base_;
  uint32_t protect_system_page_first = UINT32_MAX;
  std::lock_guard lock(memory_->watch_mutex_);
  for (uint32_t i = system_page_first; i <= system_page_last; ++i) {
    // Check if need to enable callbacks for the page and raise its protection.
    //
    // If enabling invalidation notifications:
    // - Page writable and not watched for changes yet - protect and enable
    //   invalidation notifications.
    // - Page seen as writable by the guest, but only needs data providers -
    //   set the bits to enable invalidation notifications (already has
    //   even stricter protection than needed).
    // - Page not writable as requested by the game - don't do anything (need
    //   real access violations here).
    // If enabling data providers:
    // - Page accessible (either read/write or read-only) and didn't need data
    //   providers initially - protect and enable data providers.
    // - Otherwise - do nothing.
    //
    // It's safe not to await data provider completion here before protecting as
    // this never makes protection lighter, so it can't interfere with page
    // faults that await data providers.
    //
    // Enabling data providers doesn't need to be deferred - providers will be
    // polled for the last time without releasing the lock.
    SystemPageFlagsBlock& page_flags_block = system_page_flags_[i >> 6];
    uint64_t page_flags_bit = static_cast<uint64_t>(1) << (i & 63);
    uint32_t guest_page_number =
        rex::sat_sub(i * system_page_size_, host_address_offset()) >> page_size_shift_;
    if (guest_page_number >= page_table_.size()) {
      REXLOG_ERROR(system, "Access callback page OOB: system_page={} guest_page={} offset=0x{:X}",
                   i, guest_page_number, host_address_offset());
      REX_ASSERT_ALWAYS();
      continue;
    }
    rex::memory::PageAccess current_page_access =
        ToPageAccess(page_table_[guest_page_number].current_protect);
    bool protect_system_page = false;
    // Don't do anything with inaccessible pages - don't protect, don't enable
    // callbacks - because real access violations are needed there. And don't
    // enable invalidation notifications for read-only pages for the same
    // reason.
    if (current_page_access != rex::memory::PageAccess::kNoAccess) {
      // TODO(Triang3l): Enable data providers.
      if (enable_invalidation_notifications) {
        if (current_page_access != rex::memory::PageAccess::kReadOnly &&
            (page_flags_block.notify_on_invalidation & page_flags_bit) == 0) {
          // TODO(Triang3l): Check if data providers are already enabled.
          // If data providers are already enabled for the page, it has even
          // stricter protection.
          protect_system_page = true;
          page_flags_block.notify_on_invalidation |= page_flags_bit;
        }
      }
    }
    if (protect_system_page) {
      if (protect_system_page_first == UINT32_MAX) {
        protect_system_page_first = i;
      }
    } else {
      if (protect_system_page_first != UINT32_MAX) {
        rex::memory::Protect(
            protect_base + static_cast<size_t>(protect_system_page_first) * system_page_size_,
            static_cast<size_t>(i - protect_system_page_first) * system_page_size_, protect_access);
        protect_system_page_first = UINT32_MAX;
      }
    }
  }
  if (protect_system_page_first != UINT32_MAX) {
    rex::memory::Protect(
        protect_base + static_cast<size_t>(protect_system_page_first) * system_page_size_,
        static_cast<size_t>(system_page_last + 1 - protect_system_page_first) * system_page_size_,
        protect_access);
  }
}

bool PhysicalHeap::TriggerCallbacks(uint32_t virtual_address, uint32_t length, bool is_write,
                                    bool unwatch_exact_range, bool unprotect) {
  std::lock_guard lock(memory_->watch_mutex_);
  return TriggerCallbacksLocked(virtual_address, length, is_write, unwatch_exact_range, unprotect);
}

bool PhysicalHeap::TriggerCallbacksLocked(uint32_t virtual_address, uint32_t length, bool is_write,
                                          bool unwatch_exact_range, bool unprotect) {
  // TODO(Triang3l): Support read watches.
  REX_ASSERT(is_write);
  if (!is_write) {
    return false;
  }

  if (virtual_address < heap_base_) {
    if (heap_base_ - virtual_address >= length) {
      return false;
    }
    length -= heap_base_ - virtual_address;
    virtual_address = heap_base_;
  }
  uint32_t heap_relative_address = virtual_address - heap_base_;
  if (heap_relative_address >= heap_size_) {
    return false;
  }
  length = std::min(length, heap_size_ - heap_relative_address);
  if (length == 0) {
    return false;
  }

  uint32_t system_page_first = (heap_relative_address + host_address_offset()) / system_page_size_;
  uint32_t system_page_last =
      (heap_relative_address + length - 1 + host_address_offset()) / system_page_size_;
  system_page_last = std::min(system_page_last, system_page_count_ - 1);
  REX_ASSERT(system_page_first <= system_page_last);
  uint32_t block_index_first = system_page_first >> 6;
  uint32_t block_index_last = system_page_last >> 6;

  // Check if watching any page, whether need to call the callback at all.
  bool any_watched = false;
  for (uint32_t i = block_index_first; i <= block_index_last; ++i) {
    uint64_t block = system_page_flags_[i].notify_on_invalidation;
    if (i == block_index_first) {
      block &= ~((static_cast<uint64_t>(1) << (system_page_first & 63)) - 1);
    }
    if (i == block_index_last && (system_page_last & 63) != 63) {
      block &= (static_cast<uint64_t>(1) << ((system_page_last & 63) + 1)) - 1;
    }
    if (block) {
      any_watched = true;
      break;
    }
  }
  if (!any_watched) {
    return false;
  }

  // Trigger callbacks.
  if (!unprotect) {
    // If not doing anything with protection, no point in unwatching excess
    // pages.
    unwatch_exact_range = true;
  }
  uint32_t physical_address_offset = GetPhysicalAddress(heap_base_);
  uint32_t physical_address_start =
      rex::sat_sub(system_page_first * system_page_size_, host_address_offset()) +
      physical_address_offset;
  uint32_t physical_length =
      std::min(rex::sat_sub(system_page_last * system_page_size_ + system_page_size_,
                            host_address_offset()) +
                   physical_address_offset - physical_address_start,
               heap_size_ - (physical_address_start - physical_address_offset));
  uint32_t unwatch_first = 0;
  uint32_t unwatch_last = UINT32_MAX;
  for (const auto& invalidation_callback : memory_->physical_memory_invalidation_callbacks_) {
    std::pair<uint32_t, uint32_t> callback_unwatch_range =
        invalidation_callback->first(invalidation_callback->second, physical_address_start,
                                     physical_length, unwatch_exact_range);
    if (!unwatch_exact_range) {
      unwatch_first = std::max(unwatch_first, callback_unwatch_range.first);
      unwatch_last = std::min(
          unwatch_last,
          rex::sat_add(callback_unwatch_range.first,
                       std::max(callback_unwatch_range.second, static_cast<uint32_t>(1)) - 1));
    }
  }
  if (!unwatch_exact_range) {
    // Always unwatch at least the requested pages.
    unwatch_first = std::min(unwatch_first, physical_address_start);
    unwatch_last = std::max(unwatch_last, physical_address_start + physical_length - 1);
    // Don't unprotect too much if not caring much about the region (limit to
    // 4 MB - arbitrary, but max 1024 iterations of the page loop).
    const uint32_t kMaxUnwatchExcess = 4 * 1024 * 1024;
    unwatch_first = std::max(unwatch_first, physical_address_start & ~(kMaxUnwatchExcess - 1));
    unwatch_last = std::min(
        unwatch_last, (physical_address_start + physical_length - 1) | (kMaxUnwatchExcess - 1));
    // Convert to heap-relative addresses.
    unwatch_first = rex::sat_sub(unwatch_first, physical_address_offset);
    unwatch_last = rex::sat_sub(unwatch_last, physical_address_offset);
    // Clamp to the heap upper bound.
    unwatch_first = std::min(unwatch_first, heap_size_ - 1);
    unwatch_last = std::min(unwatch_last, heap_size_ - 1);
    // Convert to system pages and update the range.
    unwatch_first += host_address_offset();
    unwatch_last += host_address_offset();
    REX_ASSERT(unwatch_first <= unwatch_last);
    system_page_first = unwatch_first / system_page_size_;
    system_page_last = unwatch_last / system_page_size_;
    block_index_first = system_page_first >> 6;
    block_index_last = system_page_last >> 6;
  }

  // Unprotect ranges that need unprotection.
  if (unprotect) {
    uint8_t* protect_base = membase_ + heap_base_;
    uint32_t unprotect_system_page_first = UINT32_MAX;
    for (uint32_t i = system_page_first; i <= system_page_last; ++i) {
      // Check if need to allow writing to this page.
      bool unprotect_page = (system_page_flags_[i >> 6].notify_on_invalidation &
                             (static_cast<uint64_t>(1) << (i & 63))) != 0;
      if (unprotect_page) {
        uint32_t guest_page_number =
            rex::sat_sub(i * system_page_size_, host_address_offset()) >> page_size_shift_;
        if (ToPageAccess(page_table_[guest_page_number].current_protect) !=
            rex::memory::PageAccess::kReadWrite) {
          unprotect_page = false;
        }
      }
      if (unprotect_page) {
        if (unprotect_system_page_first == UINT32_MAX) {
          unprotect_system_page_first = i;
        }
      } else {
        if (unprotect_system_page_first != UINT32_MAX) {
          rex::memory::Protect(
              protect_base + static_cast<size_t>(unprotect_system_page_first) * system_page_size_,
              static_cast<size_t>(i - unprotect_system_page_first) * system_page_size_,
              rex::memory::PageAccess::kReadWrite);
          unprotect_system_page_first = UINT32_MAX;
        }
      }
    }
    if (unprotect_system_page_first != UINT32_MAX) {
      rex::memory::Protect(
          protect_base + static_cast<size_t>(unprotect_system_page_first) * system_page_size_,
          static_cast<size_t>(system_page_last + 1 - unprotect_system_page_first) *
              system_page_size_,
          rex::memory::PageAccess::kReadWrite);
    }
  }

  // Mark pages as not write-watched.
  for (uint32_t i = block_index_first; i <= block_index_last; ++i) {
    uint64_t mask = 0;
    if (i == block_index_first) {
      mask |= (static_cast<uint64_t>(1) << (system_page_first & 63)) - 1;
    }
    if (i == block_index_last && (system_page_last & 63) != 63) {
      mask |= ~((static_cast<uint64_t>(1) << ((system_page_last & 63) + 1)) - 1);
    }
    system_page_flags_[i].notify_on_invalidation &= mask;
  }

  return true;
}

std::unique_lock<rex::platform::RankedMutex> PhysicalHeap::AcquireHostPageReconcileLock() const {
  auto lock = BaseHeap::AcquireHostPageReconcileLock();
  if (!lock.owns_lock() && parent_heap_ && parent_heap_->page_size() < memory_->system_page_size_) {
    lock = std::unique_lock(memory_->watch_mutex_);
  }
  return lock;
}

bool PhysicalHeap::IsHostPageWriteWatched(uint32_t host_page_number) const {
  // Indices match EnableAccessCallbacks: both count host pages from
  // membase_ + heap_base_, including host_address_offset().
  if (host_page_number >= system_page_count_) {
    return false;
  }
  return (system_page_flags_[host_page_number >> 6].notify_on_invalidation &
          (static_cast<uint64_t>(1) << (host_page_number & 63))) != 0;
}

uint32_t PhysicalHeap::GetPhysicalAddress(uint32_t address) const {
  REX_ASSERT(address >= heap_base_);
  address -= heap_base_;
  REX_ASSERT(address < heap_size_);
  if (heap_base_ >= 0xE0000000) {
    address += 0x1000;
  }
  return address;
}

}  // namespace rex::runtime
