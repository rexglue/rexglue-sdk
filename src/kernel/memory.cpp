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
#include <atomic>
#include <bit>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <format>

#include <rex/assert.h>
#include <rex/platform/perf.h>
#include <rex/platform/exception_handler.h>
#include <rex/logging/log_category.h>
#include <rex/math.h>
#include <rex/bit.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/guest_allocator.h>
#include <rex/ppc/mmio.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/platform/process.h>
#include <rex/platform/thread.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/runtime_settings.h>

#include <rex/kernel/types/status.h>
#include <rex/utility.h>
#include "kernel/settings_access.h"

#if REX_PLATFORM_MAC
#include <sys/mman.h>
#endif

namespace rex::kernel {

u32 GetPageCount(u32 value, u32 page_size, u32 page_size_shift) {
  return rex::RoundUpNonZero(value, page_size) >> page_size_shift;
}

static std::atomic<kernel::GuestAddressSpace*> g_active_memory{nullptr};

namespace {

constexpr std::string_view kFileMappingPrefix = "xenia_memory_";

enum class GuestFaultType : u8 { kMMIO, kUnhandled, kPhysicalWrite };

struct GuestFault {
  GuestFaultType type = GuestFaultType::kUnhandled;
  u32 address = 0;
  bool is_write = false;
  u32 physical_address = 0;
  u32 guest_protect = 0;
  u32 physical_protect = 0;
};

thread_local std::optional<GuestFault> t_guest_fault;

class PendingScribble : public NonCopyable {
 public:
  PendingScribble(u32 allocation_type, u32 protect)
      : enabled_((allocation_type & ~static_cast<u32>(kMemoryAllocationTitle)) !=
                     kMemoryAllocationReserve &&
                 GetRuntimeSettings().scribble_heap.Get() && (protect & kMemoryProtectWrite)) {}
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
    range = {heap.TranslateRelative(address - heap.GetHeapBase()),
             rex::RoundUpNonZero(size, heap.GetPageSize())};
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
    switch (fault->type) {
      case GuestFaultType::kMMIO:
        append("{} register {} 0x{:08X}",
               rex::ppc::MMIOBlockOf(fault->address) == rex::ppc::MMIOBlock::kGPU ? "GPU" : "APU",
               access, fault->address);
        break;
      case GuestFaultType::kUnhandled:
        append("unhandled title {} of 0x{:08X}", access, fault->address);
        break;
      case GuestFaultType::kPhysicalWrite:
        append(
            "unhandled guest physical write of 0x{:08X}, physical 0x{:08X}, title protect "
            "0x{:08X}, physical protect 0x{:08X}",
            fault->address, fault->physical_address, fault->guest_protect, fault->physical_protect);
        break;
    }
  }
  const GuestAddressSpace* memory = g_active_memory.load(std::memory_order_acquire);
  if (memory && host_address) {
    const u64 base = reinterpret_cast<u64>(memory->GetVirtualMembase());
    if (host_address >= base && host_address - base < kGuestAddressSpace) {
      append("{}guest address 0x{:08X}", cursor == out.data() ? "" : ", ",
             memory->HostToGuestVirtual(std::bit_cast<const void*>(host_address)));
    }
  }
  if (const KernelThread* thread =
          KernelThread::IsInThread() ? KernelThread::GetCallingThread() : nullptr) {
    append("{}guest thread {:08X} {}", cursor == out.data() ? "" : ", ", thread->GetThreadID(),
           thread->GetCrashName());
  }
  return static_cast<u32>(cursor - out.data());
}

bool OnAccessViolation(rex::platform::Exception* ex, void* data) {
  if (ex->GetCode() != rex::platform::Exception::Code::kAccessViolation) {
    return false;
  }
  const auto operation = ex->GetAccessViolationOperation();
  if (operation != rex::platform::Exception::AccessViolationOperation::kRead &&
      operation != rex::platform::Exception::AccessViolationOperation::kWrite) {
    return false;
  }
  auto* memory = static_cast<GuestAddressSpace*>(data);
  const bool is_write = operation == rex::platform::Exception::AccessViolationOperation::kWrite;
  auto* host_address = std::bit_cast<void*>(static_cast<uintptr_t>(ex->GetFaultAddress()));
  switch (memory->HandleAccessViolation(host_address, is_write)) {
    case AccessViolationResult::kResumed:
      return true;
    case AccessViolationResult::kMMIOMiss:
      t_guest_fault = GuestFault{.type = GuestFaultType::kMMIO,
                                 .address = memory->HostToGuestVirtual(host_address),
                                 .is_write = is_write};
      return false;
    case AccessViolationResult::kUnhandled:
      return false;
  }
  return false;
}

PageEntry LoadPageEntry(PageEntry& slot) {
  PageEntry entry;
  entry.qword = std::atomic_ref<u64>(slot.qword).load(std::memory_order_acquire);
  return entry;
}

void StorePageEntry(PageEntry& slot, PageEntry entry) {
  std::atomic_ref<u64>(slot.qword).store(entry.qword, std::memory_order_release);
}

}  // namespace

GuestAddressSpace::GuestAddressSpace() {
  system_page_size_ = static_cast<u32>(rex::memory::GetPageSize());
  system_allocation_granularity_ = static_cast<u32>(rex::memory::GetAllocationGranularity());
  REX_ASSERT(g_active_memory.load() == nullptr);
  g_active_memory.store(this, std::memory_order_release);
  platform::SetCrashAddressDescriber(DescribeCrashAddress);
}

GuestAddressSpace::~GuestAddressSpace() {
  REX_ASSERT(g_active_memory.load() == this);
  platform::SetCrashAddressDescriber(nullptr);
  g_active_memory.store(nullptr, std::memory_order_release);
  if (const u32 count = g_recovered_protect_count.exchange(0, std::memory_order_relaxed)) {
    REXSYS_WARN("Recovered stale physical page protection {} times, last at title {:08X}", count,
                g_recovered_protect_address.load(std::memory_order_relaxed));
  }

  rex::platform::ExceptionHandler::Uninstall(OnAccessViolation, this);

  physical_memory_invalidation_callbacks_.clear();

  heaps_.v00000000.Dispose();
  heaps_.v40000000.Dispose();
  heaps_.v80000000.Dispose();
  heaps_.v90000000.Dispose();
  heaps_.va0000000.Dispose();
  heaps_.vc0000000.Dispose();
  heaps_.ve0000000.Dispose();
  heaps_.physical.Dispose();

  if (mapping_ != rex::memory::kFileMappingHandleInvalid) {
    UnmapViews();
    rex::memory::CloseFileMappingHandle(mapping_, file_name_);
    mapping_base_ = nullptr;
    mapping_ = rex::memory::kFileMappingHandleInvalid;
  }

  virtual_membase_ = nullptr;
  physical_membase_ = nullptr;
}

bool GuestAddressSpace::Initialize() {
  if ((system_allocation_granularity_ > 0x1000) != rex::memory::detail::kPhysicalOffsetInCode) {
    REXSYS_ERROR(
        "The host allocation granularity is 0x{:X}, which does not match how the 0xE0000000 view "
        "offset is applied on this platform.",
        system_allocation_granularity_);
    return false;
  }
  rex::memory::RemoveStaleFileMappings(kFileMappingPrefix);
  file_name_ = rex::memory::CreateFileMappingName(kFileMappingPrefix);

  const size_t mapping_size =
      rex::RoundUpNonZero(static_cast<size_t>(0x120000000ull) + system_allocation_granularity_,
                          static_cast<size_t>(system_allocation_granularity_));
  mapping_ = rex::memory::CreateFileMappingHandle(file_name_, mapping_size,
                                                  rex::memory::PageAccess::kReadWrite, false);
  if (mapping_ == rex::memory::kFileMappingHandleInvalid) {
    REXSYS_ERROR("Unable to reserve the 4gb guest address space.");
    REX_ASSERT_ALWAYS();
    return false;
  }

#if REX_PLATFORM_MAC
  if (MapViewsMac()) {
    REXSYS_ERROR("Unable to find a continuous block in the 64bit address space.");
    REX_ASSERT_ALWAYS();
    return false;
  }
  mapping_base_ = views_.all_views[0];
#else
  mapping_base_ = 0;
  for (size_t n = 32; n < 64; n++) {
    auto mapping_base = std::bit_cast<u8*>(uintptr_t{1} << n);
    if (!MapViews(mapping_base)) {
      mapping_base_ = mapping_base;
      break;
    }
  }
  if (!mapping_base_) {
    REXSYS_ERROR("Unable to find a continuous block in the 64bit address space.");
    REX_ASSERT_ALWAYS();
    return false;
  }
#endif
  virtual_membase_ = mapping_base_;
  physical_membase_ = mapping_base_ + 0x100000000ull;

  REXSYS_INFO("Guest memory mapped at virtual base 0x{:016X}, physical base 0x{:016X}",
              reinterpret_cast<uintptr_t>(virtual_membase_),
              reinterpret_cast<uintptr_t>(physical_membase_));

  heaps_.v00000000.Initialize(this, virtual_membase_, kernel::HeapType::kGuestVirtual, 0x00000000,
                              0x40000000, kPageSize4KiB);
  heaps_.v40000000.Initialize(this, virtual_membase_, kernel::HeapType::kGuestVirtual, 0x40000000,
                              0x40000000 - 0x01000000, kPageSize64KiB);
  heaps_.v80000000.Initialize(this, virtual_membase_, kernel::HeapType::kGuestXEX, 0x80000000,
                              0x10000000, kPageSize64KiB);
  heaps_.v90000000.Initialize(this, virtual_membase_, kernel::HeapType::kGuestXEX, 0x90000000,
                              0x10000000, kPageSize4KiB);

  heaps_.physical.Initialize(this, physical_membase_, kernel::HeapType::kGuestPhysical, 0x00000000,
                             0x20000000, kPageSize4KiB);
  heaps_.va0000000.Initialize(this, virtual_membase_, kernel::HeapType::kGuestPhysical, 0xA0000000,
                              0x20000000, kPageSize64KiB, &heaps_.physical);
  heaps_.vc0000000.Initialize(this, virtual_membase_, kernel::HeapType::kGuestPhysical, 0xC0000000,
                              0x20000000, kPageSize16MiB, &heaps_.physical);
  heaps_.ve0000000.Initialize(this, virtual_membase_, kernel::HeapType::kGuestPhysical, 0xE0000000,
                              0x1FD00000, kPageSize4KiB, &heaps_.physical);

  heaps_.v00000000.AllocFixed(0x00000000, 0x10000, 0x10000,
                              kernel::kMemoryAllocationReserve | kernel::kMemoryAllocationCommit,
                              !GetRuntimeSettings().protect_zero.Get()
                                  ? kernel::kMemoryProtectRead | kernel::kMemoryProtectWrite
                                  : kernel::kMemoryProtectNoAccess);
  heaps_.physical.AllocFixed(0x1FFF0000, 0x10000, 0x10000, kernel::kMemoryAllocationReserve,
                             kernel::kMemoryProtectNoAccess);

  heaps_.vc0000000.AllocFixed(0xC0000000, 0x01000000, 32,
                              kernel::kMemoryAllocationReserve | kernel::kMemoryAllocationCommit,
                              kernel::kMemoryProtectRead | kernel::kMemoryProtectWrite);

  // Precommit the physical range so the GPU never page faults on it.
  rex::memory::AllocFixed(heaps_.physical.TranslateRelative(0), heaps_.physical.GetHeapSize(),
                          rex::memory::AllocationType::kCommit,
                          rex::memory::PageAccess::kReadWrite);

  if (!ReserveMMIOBlocks()) {
    REXSYS_ERROR("Unable to reserve the MMIO register blocks");
    REX_ASSERT_ALWAYS();
    return false;
  }
  rex::platform::ExceptionHandler::Install(OnAccessViolation, this);

  u32 reserved_physical_address;
  heaps_.va0000000.Alloc(0x340000, kPageSize64KiB, kernel::kMemoryAllocationReserve,
                         kernel::kMemoryProtectNoAccess, true, &reserved_physical_address);

  // A title reads a constant at 0x8000001C in the XEX range.
  u32 xex_constant_address;
  heaps_.v80000000.Alloc(0x40000, kPageSize4KiB, kernel::kMemoryAllocationCommit,
                         kernel::kMemoryProtectRead | kernel::kMemoryProtectWrite, false,
                         &xex_constant_address);

  *TranslateVirtual<be_u32*>(0x80000000 + 0x1C) = 0x2a6e3f38;

  return true;
}

/**
 * Maps each guest address range to a view of the backing mapping.
 *
 * - 0x00000000..0x3FFFFFFF virtual 4KB pages
 * - 0x40000000..0x7EFFFFFF virtual 64KB pages
 * - 0x7F000000..0x7FFFFFFF GPU writeback
 * - 0x80000000..0x8FFFFFFF XEX 64KB pages, the top 64MB from 0x8C000000 is encrypted
 * - 0x90000000..0x9FFFFFFF XEX 4KB pages
 * - 0xA0000000..0xBFFFFFFF physical 64KB pages
 * - 0xC0000000..0xDFFFFFFF physical 16MB pages
 * - 0xE0000000..0xFFFFFFFF physical 4KB pages
 * - 0x100000000..0x11FFFFFFF physical raw
 *
 * The physical ranges map onto the lower 512MB, so 0xA0000000 is physical 0x00000000.
 */
static const struct {
  u64 virtual_address_start;
  u64 virtual_address_end;
  u64 target_address;
} kViewRanges[] = {
    {
        0x00000000,
        0x3FFFFFFF,
        0x0000000000000000ull,
    },
    {
        0x40000000,
        0x7EFFFFFF,
        0x0000000040000000ull,
    },
    {
        0x7F000000,
        0x7FFFFFFF,
        0x0000000100000000ull,
    },
    {
        0x80000000,
        0x8FFFFFFF,
        0x0000000080000000ull,
    },
    {
        0x90000000,
        0x9FFFFFFF,
        0x0000000080000000ull,
    },
    {
        0xA0000000,
        0xBFFFFFFF,
        0x0000000100000000ull,
    },
    {
        0xC0000000,
        0xDFFFFFFF,
        0x0000000100000000ull,
    },
    {
        0xE0000000,
        0xFFFFFFFF,
        0x0000000100001000ull,
    },
    {
        0x100000000,
        0x11FFFFFFF,
        0x0000000100000000ull,
    },
};
#if REX_PLATFORM_MAC
int GuestAddressSpace::MapViewsMac() {
  REX_ASSERT(rex::CountOf(kViewRanges) == rex::CountOf(views_.all_views));

  // mmap without MAP_FIXED can ignore the requested address, so reserve one range first.
  const size_t total_size =
      static_cast<size_t>(kViewRanges[rex::CountOf(kViewRanges) - 1].virtual_address_end -
                          kViewRanges[0].virtual_address_start + 1);

  void* reserved_base = mmap(nullptr, total_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reserved_base == MAP_FAILED) {
    REXSYS_FAIL(1, "MapViewsMac: reserve failed: {}", std::strerror(errno));
  }

  u8* mapping_base = reinterpret_cast<u8*>(reserved_base);
  u64 granularity_mask = ~static_cast<u64>(system_allocation_granularity_ - 1);

  for (size_t n = 0; n < rex::CountOf(kViewRanges); n++) {
    size_t view_size = static_cast<size_t>(kViewRanges[n].virtual_address_end -
                                           kViewRanges[n].virtual_address_start + 1);
    size_t file_offset = static_cast<size_t>(kViewRanges[n].target_address & granularity_mask);
    void* target_address = mapping_base + kViewRanges[n].virtual_address_start;
    void* result = mmap(target_address, view_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                        mapping_, file_offset);
    if (result == MAP_FAILED || result != target_address) {
      int err = errno;
      REXSYS_ERROR(
          "MapViewsMac: map failed view {} addr 0x{:016X} size 0x{:X} offset 0x{:X} err {} ({})", n,
          reinterpret_cast<uintptr_t>(target_address), view_size, file_offset, err,
          std::strerror(err));
      munmap(reserved_base, total_size);
      for (auto& view : views_.all_views) {
        view = nullptr;
      }
      return 1;
    }
    views_.all_views[n] = reinterpret_cast<u8*>(result);
  }

  return 0;
}
#endif  // REX_PLATFORM_MAC

int GuestAddressSpace::MapViews(u8* mapping_base) {
  REX_ASSERT(rex::CountOf(kViewRanges) == rex::CountOf(views_.all_views));
  u64 granularity_mask = ~static_cast<u64>(system_allocation_granularity_ - 1);
  for (size_t n = 0; n < rex::CountOf(kViewRanges); n++) {
    views_.all_views[n] = reinterpret_cast<u8*>(rex::memory::MapFileView(
        mapping_, mapping_base + kViewRanges[n].virtual_address_start,
        kViewRanges[n].virtual_address_end - kViewRanges[n].virtual_address_start + 1,
        rex::memory::PageAccess::kReadWrite, kViewRanges[n].target_address & granularity_mask));
    if (!views_.all_views[n]) {
      UnmapViews();
      return 1;
    }
  }
  return 0;
}

void GuestAddressSpace::UnmapViews() {
  for (size_t n = 0; n < rex::CountOf(views_.all_views); n++) {
    if (views_.all_views[n]) {
      size_t length = kViewRanges[n].virtual_address_end - kViewRanges[n].virtual_address_start + 1;
      rex::memory::UnmapFileView(mapping_, views_.all_views[n], length);
    }
  }
}

const BaseHeap* GuestAddressSpace::LookupHeap(u32 address) const {
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
    return &heaps_.va0000000;
  }
  if (address < 0xE0000000) {
    return &heaps_.vc0000000;
  }
  return &heaps_.ve0000000;
}

BaseHeap* GuestAddressSpace::LookupHeapByType(bool physical, u32 page_size) {
  if (physical) {
    if (page_size <= 4096) {
      return &heaps_.ve0000000;
    } else if (page_size <= 64 * 1024) {
      return &heaps_.va0000000;
    } else {
      return &heaps_.vc0000000;
    }
  } else {
    if (page_size <= 4096) {
      return &heaps_.v00000000;
    } else {
      return &heaps_.v40000000;
    }
  }
}

VirtualHeap* GuestAddressSpace::GetPhysicalHeap() {
  return &heaps_.physical;
}

u32 GuestAddressSpace::HostToGuestVirtual(const void* host_address) const {
  return rex::memory::HostPtrToGuest(virtual_membase_, host_address);
}

u32 GuestAddressSpace::GetPhysicalAddress(u32 address) const {
  const BaseHeap* heap = LookupHeap(address);
  if (!heap || heap->GetHeapType() != kernel::HeapType::kGuestPhysical) {
    return UINT32_MAX;
  }
  return static_cast<const PhysicalHeap*>(heap)->GetPhysicalAddress(address);
}

void GuestAddressSpace::Zero(u32 address, u32 size) const {
  std::memset(TranslateVirtual(address), 0, size);
}

void GuestAddressSpace::Fill(u32 address, u32 size, u8 value) const {
  std::memset(TranslateVirtual(address), value, size);
}

void GuestAddressSpace::Copy(u32 dest, u32 src, u32 size) const {
  u8* pdest = TranslateVirtual(dest);
  const u8* psrc = TranslateVirtual(src);
  std::memcpy(pdest, psrc, size);
}

bool GuestAddressSpace::ReserveMMIOBlocks() const {
  const size_t host_page_size = rex::memory::GetPageSize();
  return std::ranges::all_of(
      std::array{rex::ppc::MMIOBlock::kGPU, rex::ppc::MMIOBlock::kAPU},
      [&](rex::ppc::MMIOBlock block) {
        u8* const start = TranslateVirtual(rex::ppc::MMIOBlockBase(block));
        u8* const aligned_start =
            start - (reinterpret_cast<uintptr_t>(start) & (host_page_size - 1));
        const size_t aligned_length = rex::RoundUpNonZero(
            static_cast<size_t>(start - aligned_start) + rex::ppc::kMMIOBlockSize, host_page_size);
        return rex::memory::AllocFixed(aligned_start, aligned_length,
                                       rex::memory::AllocationType::kCommit,
                                       rex::memory::PageAccess::kNoAccess) != nullptr;
      });
}

AccessViolationResult GuestAddressSpace::HandleAccessViolation(void* host_address, bool is_write) {
  const auto host = reinterpret_cast<uintptr_t>(host_address);
  const auto virtual_base = reinterpret_cast<uintptr_t>(virtual_membase_);
  const auto physical_base = reinterpret_cast<uintptr_t>(physical_membase_);
  if (host < virtual_base || host >= physical_base + 0x20000000) {
    return AccessViolationResult::kUnhandled;
  }
  if (host < physical_base && rex::ppc::IsMMIOBlockAddress(static_cast<u32>(host - virtual_base))) {
    return AccessViolationResult::kMMIOMiss;
  }
  return AccessViolationCallback(host_address, is_write) ? AccessViolationResult::kResumed
                                                         : AccessViolationResult::kUnhandled;
}

bool GuestAddressSpace::AccessViolationCallback(void* host_address, bool is_write) {
  // Accesses through physical_membase_ bypass callbacks, which fire only on virtual regions.
  if (reinterpret_cast<size_t>(host_address) < reinterpret_cast<size_t>(virtual_membase_) ||
      reinterpret_cast<size_t>(host_address) >= reinterpret_cast<size_t>(physical_membase_)) {
    return false;
  }
  std::unique_lock lock(watch_mutex_);
  u32 virtual_address = HostToGuestVirtual(host_address);
  BaseHeap* heap = LookupHeap(virtual_address);
  if (!heap) {
    t_guest_fault = GuestFault{
        .type = GuestFaultType::kUnhandled, .address = virtual_address, .is_write = is_write};
    return false;
  }

  // The heap decides the host access. A page it allows has stale host protection.
  if (heap->ReapplyHostPageAccess(host_address, is_write)) {
    g_recovered_protect_count.fetch_add(1, std::memory_order_relaxed);
    g_recovered_protect_address.store(virtual_address, std::memory_order_relaxed);
    return true;
  }

  if (heap->GetHeapType() != kernel::HeapType::kGuestPhysical) {
    t_guest_fault = GuestFault{
        .type = GuestFaultType::kUnhandled, .address = virtual_address, .is_write = is_write};
    return false;
  }

  // A length of 1 never crosses a page boundary.
  auto physical_heap = static_cast<PhysicalHeap*>(heap);
  if (physical_heap->TriggerCallbacksLocked(virtual_address, 1, is_write, false)) {
    return true;
  }
  lock.unlock();

  if (is_write) {
    u32 current_guest_protect = 0;
    heap->QueryProtect(virtual_address, &current_guest_protect);
    u32 physical_address = GetPhysicalAddress(virtual_address);
    u32 physical_protect = 0;
    if (physical_address != UINT32_MAX) {
      heaps_.physical.QueryProtect(physical_address, &physical_protect);
    }
    t_guest_fault = GuestFault{.type = GuestFaultType::kPhysicalWrite,
                               .address = virtual_address,
                               .is_write = true,
                               .physical_address = physical_address,
                               .guest_protect = current_guest_protect,
                               .physical_protect = physical_protect};
  }

  return false;
}

void* GuestAddressSpace::RegisterPhysicalMemoryInvalidationCallback(
    PhysicalMemoryInvalidationCallback callback, void* callback_context) {
  auto entry = std::make_unique<std::pair<PhysicalMemoryInvalidationCallback, void*>>(
      callback, callback_context);
  void* handle = entry.get();
  std::lock_guard lock(watch_mutex_);
  physical_memory_invalidation_callbacks_.push_back(std::move(entry));
  return handle;
}

void GuestAddressSpace::UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle) {
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

void GuestAddressSpace::EnablePhysicalMemoryAccessCallbacks(u32 physical_address, u32 length,
                                                            bool enable_invalidation_notifications,
                                                            bool enable_data_providers) {
  heaps_.va0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications, enable_data_providers);
  heaps_.vc0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications, enable_data_providers);
  heaps_.ve0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications, enable_data_providers);
}

u32 GuestAddressSpace::SystemHeapAlloc(u32 size, u32 alignment, u32 system_heap_flags) {
  bool is_physical = !!(system_heap_flags & kernel::kSystemHeapPhysical);
  auto heap = LookupHeapByType(is_physical, 4096);
  u32 address;
  if (!heap->AllocSystemHeap(
          size, alignment, kernel::kMemoryAllocationReserve | kernel::kMemoryAllocationCommit,
          kernel::kMemoryProtectRead | kernel::kMemoryProtectWrite, false, &address)) {
    return 0;
  }
  Zero(address, size);
  return address;
}

void GuestAddressSpace::SystemHeapFree(u32 address, u32* out_region_size) {
  if (!address) {
    return;
  }
  auto heap = LookupHeap(address);
  heap->Release(address, out_region_size);
}

void GuestAddressSpace::GetHeapsPageStats(const BaseHeap* const* provided_heaps, size_t heaps_count,
                                          u32& unreserved_pages, u32& reserved_pages,
                                          u32& used_pages, u32& reserved_bytes) {
  // The heap array is fixed after init and page counts are approximate.
  for (size_t i = 0; i < heaps_count; i++) {
    const BaseHeap* heap = provided_heaps[i];
    u32 heap_unreserved = heap->GetUnreservedPageCount();
    u32 heap_reserved = heap->GetReservedPageCount();

    unreserved_pages += heap_unreserved;
    reserved_pages += heap_reserved;
    used_pages += ((heap->GetTotalPageCount() - heap_unreserved) * heap->GetPageSize()) / 4096;
    reserved_bytes += heap_reserved * heap->GetPageSize();
  }
}

void GuestAddressSpace::SetBudget(u32 megabytes) {
  budget_pages_.store(static_cast<u32>(std::min<u64>(u64{megabytes} * 256, UINT32_MAX)),
                      std::memory_order_relaxed);
}

u32 GuestAddressSpace::GetTitlePages() const {
  return title_pages_.load(std::memory_order_relaxed);
}

u32 GuestAddressSpace::GetBudgetPages() const {
  return budget_pages_.load(std::memory_order_relaxed);
}

bool GuestAddressSpace::ReserveBudgetPages(u32 pages) {
  const u32 budget = budget_pages_.load(std::memory_order_relaxed);
  u32 used = title_pages_.load(std::memory_order_relaxed);
  do {
    if (budget && used + pages > budget) {
      return false;
    }
  } while (!title_pages_.compare_exchange_weak(used, used + pages, std::memory_order_relaxed));
  return true;
}

void GuestAddressSpace::ReleaseBudgetPages(u32 pages) {
  title_pages_.fetch_sub(pages, std::memory_order_relaxed);
}

void GuestAddressSpace::PlotProfile() {
#ifdef REXGLUE_ENABLE_PROFILING
  if (!TracyIsStarted) {
    return;
  }
  heaps_.v00000000.PlotProfile();
  heaps_.v40000000.PlotProfile();
  heaps_.v80000000.PlotProfile();
  heaps_.v90000000.PlotProfile();
  heaps_.physical.PlotProfile();

  std::array<u64, static_cast<size_t>(GuestPool::kCount)> in_use{};
  {
    std::lock_guard lock(allocators_mutex_);
    for (const GuestAllocator* allocator : allocators_) {
      in_use[static_cast<size_t>(allocator->GetPool())] += allocator->GetStats().allocated;
    }
  }
  for (size_t i = 0; i < in_use.size(); ++i) {
    REX_PROFILE_PLOT(GuestAllocator::GetPoolPlotName(static_cast<GuestPool>(i)), in_use[i]);
  }

  REX_PROFILE_PLOT("Title memory in use", u64{GetTitlePages()} * 4096);
  REX_PROFILE_PLOT("Title memory budget", u64{GetBudgetPages()} * 4096);
#endif
}

bool GuestAddressSpace::InitializeFunctionTable(u32 code_base, u32 code_size, u32 image_base,
                                                u32 image_size) {
  constexpr u32 kThunkReserveSize = kernel::FunctionDispatcher::kThunkReserveSize;
  u32 table_base = image_base + image_size;
  u32 table_size = (code_size + kThunkReserveSize) * 2;

  REXSYS_DEBUG(
      "Initializing function table at {:08X}, size {:08X} for code {:08X}-{:08X} "
      "(+{:08X} thunk reserve)",
      table_base, table_size, code_base, code_base + code_size, kThunkReserveSize);

  if (!heaps_.v80000000.AllocFixed(
          table_base, table_size, 0x10000,
          kernel::kMemoryAllocationReserve | kernel::kMemoryAllocationCommit,
          kernel::kMemoryProtectRead | kernel::kMemoryProtectWrite)) {
    REXSYS_FAIL(false, "Failed to allocate function table at {:08X}", table_base);
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

bool GuestAddressSpace::DestroyFunctionTable(u32 code_base) {
  u32 table_base = 0;
  u32 table_size = 0;
  u32 logged_code_base = 0;
  u32 logged_code_size = 0;
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

  REXSYS_DEBUG("Destroying function table at {:08X}, size {:08X} for code {:08X}-{:08X}",
               table_base, table_size, logged_code_base, logged_code_base + logged_code_size);

  heaps_.v80000000.Release(table_base);
  return true;
}

bool GuestAddressSpace::SetFunction(u32 guest_address, PPCFunc* host_function) {
  u32 table_address = 0;
  {
    std::lock_guard lock(function_tables_mutex_);
    for (const auto& entry : function_tables_) {
      u32 range_end = entry.code_base + entry.code_size + entry.thunk_reserve;
      if (guest_address >= entry.code_base && guest_address < range_end) {
        u64 offset = (static_cast<u64>(guest_address) - entry.code_base) * 2;
        table_address = entry.table_base + static_cast<u32>(offset);
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

rex::memory::PageAccess ToPageAccess(u32 protect) {
  bool is_writable =
      (protect & kernel::kMemoryProtectWrite) || (protect & kernel::kMemoryProtectWriteCombine);

  if ((protect & kernel::kMemoryProtectRead) && !is_writable) {
    return rex::memory::PageAccess::kReadOnly;
  } else if ((protect & kernel::kMemoryProtectRead) && is_writable) {
    return rex::memory::PageAccess::kReadWrite;
  } else {
    return rex::memory::PageAccess::kNoAccess;
  }
}

u32 FromPageAccess(rex::memory::PageAccess protect) {
  switch (protect) {
    case memory::PageAccess::kNoAccess:
      return kernel::kMemoryProtectNoAccess;
    case memory::PageAccess::kReadOnly:
      return kernel::kMemoryProtectRead;
    case memory::PageAccess::kReadWrite:
      return kernel::kMemoryProtectRead | kernel::kMemoryProtectWrite;
    case memory::PageAccess::kExecuteReadOnly:
      REX_ASSERT_ALWAYS();
      return kernel::kMemoryProtectRead;
    case memory::PageAccess::kExecuteReadWrite:
      REX_ASSERT_ALWAYS();
      return kernel::kMemoryProtectRead | kernel::kMemoryProtectWrite;
  }

  return kernel::kMemoryProtectNoAccess;
}

BaseHeap::BaseHeap() : membase_(nullptr), heap_base_(0), heap_size_(0), page_size_(0) {}

BaseHeap::~BaseHeap() = default;

std::unique_lock<rex::platform::RankedMutex> BaseHeap::AcquireHostPageReconcileLock() const {
  if (page_size_ >= memory_->system_page_size_) {
    return {};
  }
  return std::unique_lock(memory_->watch_mutex_);
}

rex::memory::PageAccess BaseHeap::GetHostPageAccess(u32 host_page_number) const {
  const u32 host_page_size = memory_->system_page_size_;
  // A host page spans several guest pages, so it gets the union of their committed access.
  const u64 host_page_start = static_cast<u64>(host_page_number) * host_page_size;
  const u64 host_page_end = host_page_start + host_page_size;
  const u64 heap_host_start = host_address_offset_;
  const u64 heap_host_end = heap_host_start + heap_size_;
  const u64 overlap_start = std::max(host_page_start, heap_host_start);
  const u64 overlap_end = std::min(host_page_end, heap_host_end);
  if (overlap_start >= overlap_end) {
    return rex::memory::PageAccess::kNoAccess;
  }

  const u32 guest_page_start =
      static_cast<u32>(overlap_start - heap_host_start) >> page_size_shift_;
  const u32 guest_page_end =
      static_cast<u32>(overlap_end - heap_host_start - 1) >> page_size_shift_;

  bool has_read = false;
  bool has_write = false;
  bool has_committed = false;
  for (u32 page_number = guest_page_start; page_number <= guest_page_end; ++page_number) {
    const auto& page_entry = page_table_[page_number];
    if (!(page_entry.state & kernel::kMemoryAllocationCommit)) {
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
  // A released physical alias stays accessible unless protect_on_release is set, for the GPU.
  if (!has_committed && heap_type_ == kernel::HeapType::kGuestPhysical &&
      !GetRuntimeSettings().protect_on_release.Get()) {
    access = rex::memory::PageAccess::kReadWrite;
  } else if (!has_read) {
    access = rex::memory::PageAccess::kNoAccess;
  } else {
    access = has_write ? rex::memory::PageAccess::kReadWrite : rex::memory::PageAccess::kReadOnly;
  }

  // A write watched page stays read only so the watch can fire.
  if (access == rex::memory::PageAccess::kReadWrite && IsHostPageWriteWatched(host_page_number)) {
    access = rex::memory::PageAccess::kReadOnly;
  }
  return access;
}

bool BaseHeap::ReapplyHostPageAccess(void* host_address, bool is_write) {
  const u32 host_page_size = memory_->system_page_size_;
  u8* const page = static_cast<u8*>(host_address) -
                   (reinterpret_cast<uintptr_t>(host_address) & (host_page_size - 1));
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  const u8* const host_origin = TranslateRelative(0) - host_address_offset_;
  const rex::memory::PageAccess expected =
      GetHostPageAccess(static_cast<u32>((page - host_origin) / host_page_size));
  const bool allowed = is_write ? expected == rex::memory::PageAccess::kReadWrite
                                : expected != rex::memory::PageAccess::kNoAccess;
  return allowed && rex::memory::Protect(page, host_page_size, expected, nullptr);
}

bool BaseHeap::SyncHostPageAccess(u32 start_page_number, u32 end_page_number) {
  const u32 host_page_size = memory_->system_page_size_;
  if (page_size_ >= host_page_size) {
    return true;
  }

  const u64 guest_byte_start = static_cast<u64>(start_page_number) << page_size_shift_;
  const u64 guest_byte_end = ((static_cast<u64>(end_page_number) + 1) << page_size_shift_) - 1;
  const u32 host_page_first =
      static_cast<u32>((guest_byte_start + host_address_offset_) / host_page_size);
  const u32 host_page_last =
      static_cast<u32>((guest_byte_end + host_address_offset_) / host_page_size);

  // Coalesce host pages with equal access into one Protect call.
  u8* const heap_host_base = membase_ + heap_base_;
  u32 run_first_page = host_page_first;
  rex::memory::PageAccess run_access = GetHostPageAccess(host_page_first);
  const auto flush_run = [&](u32 run_last_page) {
    const size_t run_length =
        (static_cast<size_t>(run_last_page - run_first_page) + 1) * host_page_size;
    u8* const run_pointer = heap_host_base + static_cast<size_t>(run_first_page) * host_page_size;
    if (!rex::memory::Protect(run_pointer, run_length, run_access, nullptr)) {
      REXSYS_FAIL(false, "BaseHeap::SyncHostPageAccess failed for heap {:08X} host pages {}-{}",
                  heap_base_, run_first_page, run_last_page);
    }
    return true;
  };

  for (u32 host_page = host_page_first + 1; host_page <= host_page_last; ++host_page) {
    const rex::memory::PageAccess access = GetHostPageAccess(host_page);
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

void BaseHeap::Initialize(kernel::GuestAddressSpace* memory, u8* membase, HeapType heap_type,
                          u32 heap_base, u32 heap_size, u32 page_size, u32 host_address_offset) {
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
  unreserved_page_count_ = static_cast<u32>(page_table_.size());
  switch (heap_base) {
    case 0x00000000:
      profile_pool_ = heap_type == HeapType::kGuestPhysical ? "Guest physical" : "Guest v00000000";
      profile_plot_ = heap_type == HeapType::kGuestPhysical ? "Guest physical committed"
                                                            : "Guest v00000000 committed";
      break;
    case 0x40000000:
      profile_pool_ = "Guest v40000000";
      profile_plot_ = "Guest v40000000 committed";
      break;
    case 0x80000000:
      profile_pool_ = "Guest v80000000";
      profile_plot_ = "Guest v80000000 committed";
      break;
    case 0x90000000:
      profile_pool_ = "Guest v90000000";
      profile_plot_ = "Guest v90000000 committed";
      break;
    default:
      break;
  }
}

void BaseHeap::OnPageCommitted([[maybe_unused]] u32 page_number) {
  committed_pages_.fetch_add(1, std::memory_order_relaxed);
  if (profile_pool_) {
    REX_PROFILE_ALLOC_NAMED(TranslateRelative(page_number << page_size_shift_), page_size_,
                            profile_pool_);
  }
}

void BaseHeap::OnPageDecommitted([[maybe_unused]] u32 page_number) {
  committed_pages_.fetch_sub(1, std::memory_order_relaxed);
  if (profile_pool_) {
    REX_PROFILE_FREE_NAMED(TranslateRelative(page_number << page_size_shift_), profile_pool_);
  }
}

void BaseHeap::PlotProfile() const {
  if (profile_plot_) {
    REX_PROFILE_PLOT(profile_plot_, u64{GetCommittedPages()} * page_size_);
  }
}

void BaseHeap::Dispose() {
  for (u32 page_number = 0; page_number < page_table_.size(); ++page_number) {
    auto& page_entry = page_table_[page_number];
    if (page_entry.state) {
      rex::memory::DeallocFixed(TranslateRelative(page_number << page_size_shift_), 0,
                                rex::memory::DeallocationType::kRelease);
      page_number += page_entry.region_page_count;
    }
  }
}

u32 BaseHeap::GetSystemRangeBase() const {
  u32 reserved = 0;
  if (heap_type_ == kernel::HeapType::kGuestVirtual) {
    reserved = page_size_ == 0x10000 ? 0x0F000000 : 0x10000000;
  }
  return heap_base_ + heap_size_ - reserved;
}

bool BaseHeap::Alloc(u32 size, u32 alignment, u32 allocation_type, u32 protect, bool top_down,
                     u32* out_address) {
  *out_address = 0;
  size = rex::RoundUpNonZero(size, page_size_);
  alignment = rex::RoundUpNonZero(alignment, page_size_);

  u32 low_address = heap_base_;
  u32 high_address = GetSystemRangeBase() - 1;
  return AllocRange(low_address, high_address, size, alignment, allocation_type, protect, top_down,
                    out_address);
}

bool BaseHeap::AllocSystemHeap(u32 size, u32 alignment, u32 allocation_type, u32 protect,
                               bool top_down, u32* out_address) {
  *out_address = 0;
  size = rex::RoundUpNonZero(size, page_size_);
  alignment = rex::RoundUpNonZero(alignment, page_size_);

  u32 low_address = heap_base_;
  if (heap_type_ == kernel::HeapType::kGuestVirtual) {
    low_address = GetSystemRangeBase();
  }
  u32 high_address = heap_base_ + (heap_size_ - 1);
  return AllocRange(low_address, high_address, size, alignment, allocation_type, protect, top_down,
                    out_address);
}

bool BaseHeap::AllocFixed(u32 base_address, u32 size, u32 alignment, u32 allocation_type,
                          u32 protect) {
  PendingScribble scribble(allocation_type, protect);
  auto reconcile_lock = AcquireHostPageReconcileLock();
  if (!AllocFixedLocked(base_address, size, alignment, allocation_type, protect)) {
    return false;
  }
  scribble.Add(*this, base_address, size);
  return true;
}

bool BaseHeap::AllocFixedLocked(u32 base_address, u32 size, u32 alignment, u32 allocation_type,
                                u32 protect) {
  const bool title_memory = (allocation_type & kernel::kMemoryAllocationTitle) != 0;
  allocation_type &= ~static_cast<u32>(kernel::kMemoryAllocationTitle);
  alignment = rex::RoundUpNonZero(alignment, page_size_);
  if (base_address % alignment != 0) {
    if (base_address % page_size_ != 0) {
      REXSYS_FAIL(false,
                  "BaseHeap::AllocFixed invalid base alignment: base={:08X} page_size={:08X} "
                  "requested_alignment={:08X} heap={:08X}-{:08X}",
                  base_address, page_size_, alignment, heap_base_, heap_base_ + (heap_size_ - 1));
    }
    // A fixed address guarantees page alignment only, so a stricter alignment falls back.
    REXSYS_WARN(
        "BaseHeap::AllocFixed clamping alignment from {:08X} to page size {:08X} for "
        "base={:08X}",
        alignment, page_size_, base_address);
    alignment = page_size_;
  }
  size = rex::RoundUpPow2(size, alignment);
  u32 page_count = GetPageCount(size, page_size_, page_size_shift_);
  u32 start_page_number = (base_address - heap_base_) >> page_size_shift_;
  u32 end_page_number = start_page_number + page_count - 1;
  if (start_page_number >= page_table_.size() || end_page_number >= page_table_.size()) {
    REXSYS_FAIL(false, "BaseHeap::AllocFixed passed out of range address range");
  }

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // A reserve needs the whole range unreserved, a commit may overlap committed pages.
  for (u32 page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    u32 state = page_table_[page_number].state;
    if ((allocation_type == kernel::kMemoryAllocationReserve) && state) {
      REXSYS_FAIL(false,
                  "BaseHeap::AllocFixed attempting to reserve an already reserved "
                  "range");
    }
    if ((allocation_type == kernel::kMemoryAllocationCommit) &&
        !(state & kernel::kMemoryAllocationReserve)) {
      REXSYS_WARN("BaseHeap::AllocFixed attempting commit on unreserved page");
      allocation_type |= kernel::kMemoryAllocationReserve;
      break;
    }
  }

  u32 title_pages = 0;
  if (title_memory && (allocation_type & kernel::kMemoryAllocationCommit)) {
    const auto range = std::span(page_table_).subspan(start_page_number, page_count);
    const auto uncommitted = std::ranges::count_if(range, [](const PageEntry& page_entry) {
      return (page_entry.state & kernel::kMemoryAllocationCommit) == 0;
    });
    title_pages = static_cast<u32>(uncommitted) * (page_size_ >> 12);
    if (title_pages && !memory_->ReserveBudgetPages(title_pages)) {
      REXSYS_WARN("BaseHeap::AllocFixed over the title memory budget");
      return false;
    }
  }

  if (allocation_type == kernel::kMemoryAllocationReserve) {
  } else if (page_size_ >= memory_->system_page_size_) {
    auto alloc_type = (allocation_type & kernel::kMemoryAllocationCommit)
                          ? rex::memory::AllocationType::kCommit
                          : rex::memory::AllocationType::kReserve;
    void* result =
        rex::memory::AllocFixed(TranslateRelative(start_page_number << page_size_shift_),
                                page_count << page_size_shift_, alloc_type, ToPageAccess(protect));
    if (!result) {
      memory_->ReleaseBudgetPages(title_pages);
      REXSYS_FAIL(false, "BaseHeap::AllocFixed failed to alloc range from host");
    }
  }

  for (u32 page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    PageEntry page_entry = page_table_[page_number];
    if (title_pages && (page_entry.state & kernel::kMemoryAllocationCommit) == 0) {
      page_entry.title = 1;
    }
    if ((allocation_type & kernel::kMemoryAllocationCommit) &&
        !(page_entry.state & kernel::kMemoryAllocationCommit)) {
      OnPageCommitted(page_number);
    }
    if (allocation_type & kernel::kMemoryAllocationReserve) {
      if (!page_entry.state) {
        unreserved_page_count_--;
      }
      page_entry.base_address = start_page_number;
      page_entry.region_page_count = page_count;
    }
    page_entry.allocation_protect = protect;
    page_entry.current_protect = protect;
    page_entry.state = kernel::kMemoryAllocationReserve | allocation_type;
    StorePageEntry(page_table_[page_number], page_entry);
  }

  if (!SyncHostPageAccess(start_page_number, end_page_number)) {
    return false;
  }

  return true;
}

bool BaseHeap::AllocRange(u32 low_address, u32 high_address, u32 size, u32 alignment,
                          u32 allocation_type, u32 protect, bool top_down, u32* out_address) {
  PendingScribble scribble(allocation_type, protect);
  auto reconcile_lock = AcquireHostPageReconcileLock();
  if (!AllocRangeLocked(low_address, high_address, size, alignment, allocation_type, protect,
                        top_down, out_address)) {
    return false;
  }
  scribble.Add(*this, *out_address, size);
  return true;
}

bool BaseHeap::AllocRangeLocked(u32 low_address, u32 high_address, u32 size, u32 alignment,
                                u32 allocation_type, u32 protect, bool top_down, u32* out_address) {
  *out_address = 0;
  const bool title_memory = (allocation_type & kernel::kMemoryAllocationTitle) != 0;
  allocation_type &= ~static_cast<u32>(kernel::kMemoryAllocationTitle);

  alignment = rex::RoundUpNonZero(alignment, page_size_);
  u32 page_count = GetPageCount(size, page_size_, page_size_shift_);
  low_address = std::max(heap_base_, rex::RoundUpPow2(low_address, alignment));
  high_address = std::min(heap_base_ + (heap_size_ - 1), rex::RoundUpPow2(high_address, alignment));
  if (high_address < low_address) {
    REXSYS_FAIL(false, "BaseHeap::Alloc invalid requested range");
  }
  u32 low_page_number = (low_address - heap_base_) >> page_size_shift_;
  u32 high_page_number = (high_address - heap_base_) >> page_size_shift_;
  low_page_number = std::min(static_cast<u32>(page_table_.size()) - 1, low_page_number);
  high_page_number = std::min(static_cast<u32>(page_table_.size()) - 1, high_page_number);
  if (high_page_number < low_page_number) {
    REXSYS_FAIL(false, "BaseHeap::Alloc invalid requested page range");
  }

  u32 available_page_count = high_page_number - low_page_number + 1;
  if (!page_count || page_count > available_page_count) {
    REXSYS_FAIL(false, "BaseHeap::Alloc page count too big for requested range");
  }

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  u32 start_page_number = UINT_MAX;
  u32 end_page_number = UINT_MAX;
  u32 page_scan_stride = alignment >> page_size_shift_;
  u32 max_base_page_number = high_page_number + 1 - page_count;
  if (top_down) {
    max_base_page_number -= max_base_page_number % page_scan_stride;
    for (i64 base_page_number = max_base_page_number; base_page_number >= low_page_number;
         base_page_number -= page_scan_stride) {
      if (page_table_[base_page_number].state != 0) {
        continue;
      }
      start_page_number = static_cast<u32>(base_page_number);
      end_page_number = static_cast<u32>(base_page_number) + page_count - 1;
      REX_ASSERT(end_page_number < page_table_.size());
      bool any_taken = false;
      for (u32 page_number = static_cast<u32>(base_page_number);
           !any_taken && page_number <= end_page_number; ++page_number) {
        bool is_free = page_table_[page_number].state == 0;
        if (!is_free) {
          any_taken = true;
          if (page_count > page_number) {
            base_page_number = -1;
          } else {
            base_page_number = page_number - page_count;
            base_page_number -= base_page_number % page_scan_stride;
            base_page_number += page_scan_stride;
          }
          break;
        }
      }
      if (!any_taken) {
        break;
      }
      start_page_number = end_page_number = UINT_MAX;
    }
  } else {
    for (u32 base_page_number = low_page_number; base_page_number <= max_base_page_number;
         base_page_number += page_scan_stride) {
      if (page_table_[base_page_number].state != 0) {
        continue;
      }
      start_page_number = base_page_number;
      end_page_number = base_page_number + page_count - 1;
      bool any_taken = false;
      for (u32 page_number = base_page_number; !any_taken && page_number <= end_page_number;
           ++page_number) {
        bool is_free = page_table_[page_number].state == 0;
        if (!is_free) {
          any_taken = true;
          base_page_number = rex::RoundUpNonZero(page_number + 1, page_scan_stride);
          base_page_number -= page_scan_stride;
          break;
        }
      }
      if (!any_taken) {
        break;
      }
      start_page_number = end_page_number = UINT_MAX;
    }
  }
  if (start_page_number == UINT_MAX || end_page_number == UINT_MAX) {
    REXSYS_WARN("BaseHeap::Alloc failed to find contiguous range");
    return false;
  }
  const u32 title_pages = title_memory && (allocation_type & kernel::kMemoryAllocationCommit)
                              ? page_count * (page_size_ >> 12)
                              : 0;
  if (title_pages && !memory_->ReserveBudgetPages(title_pages)) {
    REXSYS_WARN("BaseHeap::Alloc over the title memory budget");
    return false;
  }

  if (allocation_type == kernel::kMemoryAllocationReserve) {
  } else if (page_size_ >= memory_->system_page_size_) {
    auto alloc_type = (allocation_type & kernel::kMemoryAllocationCommit)
                          ? rex::memory::AllocationType::kCommit
                          : rex::memory::AllocationType::kReserve;
    void* result =
        rex::memory::AllocFixed(TranslateRelative(start_page_number << page_size_shift_),
                                page_count << page_size_shift_, alloc_type, ToPageAccess(protect));
    if (!result) {
      memory_->ReleaseBudgetPages(title_pages);
      REXSYS_FAIL(false, "BaseHeap::Alloc failed to alloc range from host");
    }
  }

  for (u32 page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    PageEntry page_entry = page_table_[page_number];
    if (!page_entry.state) {
      unreserved_page_count_--;
    }
    if ((allocation_type & kernel::kMemoryAllocationCommit) &&
        !(page_entry.state & kernel::kMemoryAllocationCommit)) {
      OnPageCommitted(page_number);
    }
    page_entry.base_address = start_page_number;
    page_entry.region_page_count = page_count;
    page_entry.allocation_protect = protect;
    page_entry.current_protect = protect;
    page_entry.state = kernel::kMemoryAllocationReserve | allocation_type;
    page_entry.title = title_pages != 0;
    StorePageEntry(page_table_[page_number], page_entry);
  }

  if (!SyncHostPageAccess(start_page_number, end_page_number)) {
    return false;
  }

  *out_address = heap_base_ + (start_page_number << page_size_shift_);
  return true;
}

bool BaseHeap::Decommit(u32 address, u32 size) {
  auto reconcile_lock = AcquireHostPageReconcileLock();
  return DecommitLocked(address, size);
}

bool BaseHeap::DecommitLocked(u32 address, u32 size) {
  u32 page_count = GetPageCount(size, page_size_, page_size_shift_);
  u32 start_page_number = (address - heap_base_) >> page_size_shift_;
  u32 end_page_number = start_page_number + page_count - 1;
  start_page_number = std::min(static_cast<u32>(page_table_.size()) - 1, start_page_number);
  end_page_number = std::min(static_cast<u32>(page_table_.size()) - 1, end_page_number);

  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // Mapped memory cannot be decommitted.

  u32 released = 0;
  for (u32 page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    PageEntry page_entry = page_table_[page_number];
    released += page_entry.title;
    page_entry.title = 0;
    if (page_entry.state & kernel::kMemoryAllocationCommit) {
      OnPageDecommitted(page_number);
    }
    page_entry.state &= ~kernel::kMemoryAllocationCommit;
    StorePageEntry(page_table_[page_number], page_entry);
  }
  memory_->ReleaseBudgetPages(released * (page_size_ >> 12));

  if (!SyncHostPageAccess(start_page_number, end_page_number)) {
    return false;
  }

  return true;
}

bool BaseHeap::Release(u32 base_address, u32* out_region_size) {
  auto reconcile_lock = AcquireHostPageReconcileLock();
  return ReleaseLocked(base_address, out_region_size);
}

bool BaseHeap::ReleaseLocked(u32 base_address, u32* out_region_size) {
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  u32 base_page_number = (base_address - heap_base_) >> page_size_shift_;
  auto base_page_entry = page_table_[base_page_number];
  if (!base_page_entry.state || base_page_entry.base_address != base_page_number) {
    REXSYS_FAIL(false, "BaseHeap::Release failed because address is not a region start");
  }

  if (heap_base_ == 0x00000000 && base_page_number == 0) {
    REXSYS_FAIL(false, "BaseHeap::Release: Attempt to free 0!");
  }

  if (out_region_size) {
    *out_region_size = (base_page_entry.region_page_count << page_size_shift_);
  }

  // Releasing from the host is not needed because the mapping reserves the range.
  if (page_size_ == rex::memory::GetPageSize() ||
      ((base_page_entry.region_page_count << page_size_shift_) % rex::memory::GetPageSize() == 0 &&
       ((base_page_number << page_size_shift_) % rex::memory::GetPageSize() == 0))) {
    if (GetRuntimeSettings().protect_on_release.Get()) {
      if (!rex::memory::Protect(TranslateRelative(base_page_number << page_size_shift_),
                                base_page_entry.region_page_count << page_size_shift_,
                                rex::memory::PageAccess::kNoAccess, nullptr)) {
        REXSYS_WARN("BaseHeap::Release failed due to host VirtualProtect failure");
      }
    }
  }

  u32 end_page_number = base_page_number + base_page_entry.region_page_count - 1;
  u32 released = 0;
  for (u32 page_number = base_page_number; page_number <= end_page_number; ++page_number) {
    const PageEntry page_entry = page_table_[page_number];
    released += page_entry.title;
    if (page_entry.state & kernel::kMemoryAllocationCommit) {
      OnPageDecommitted(page_number);
    }
    StorePageEntry(page_table_[page_number], PageEntry{});
    unreserved_page_count_++;
  }
  memory_->ReleaseBudgetPages(released * (page_size_ >> 12));

  if (!SyncHostPageAccess(base_page_number, end_page_number)) {
    return false;
  }

  return true;
}

bool BaseHeap::ReleaseRange(u32 address, u32 size, u32* out_address, u32* out_size) {
  auto reconcile_lock = AcquireHostPageReconcileLock();
  return ReleaseRangeLocked(address, size, out_address, out_size);
}

bool BaseHeap::ReleaseRangeLocked(u32 address, u32 size, u32* out_address, u32* out_size) {
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  if (!size || address < heap_base_) {
    return false;
  }
  const u32 first_page = (address - heap_base_) >> page_size_shift_;
  const u64 last_byte = u64{address} + size - 1 - heap_base_;
  const u64 last_page_wide = last_byte >> page_size_shift_;
  if (last_page_wide >= page_table_.size()) {
    return false;
  }
  const u32 last_page = static_cast<u32>(last_page_wide);
  const PageEntry first_entry = page_table_[first_page];
  if (!first_entry.state) {
    return false;
  }
  const u32 region_first = first_entry.base_address;
  const u32 region_last = region_first + first_entry.region_page_count - 1;
  if (last_page > region_last) {
    return false;
  }
  for (u32 page_number = first_page; page_number <= last_page; ++page_number) {
    if (!page_table_[page_number].state) {
      return false;
    }
  }
  if (heap_base_ == 0x00000000 && region_first == 0 && first_page == 0) {
    REXSYS_FAIL(false, "BaseHeap::ReleaseRange: Attempt to free 0!");
  }

  const u32 range_bytes = (last_page - first_page + 1) << page_size_shift_;
  if (page_size_ == rex::memory::GetPageSize() ||
      (range_bytes % rex::memory::GetPageSize() == 0 &&
       ((first_page << page_size_shift_) % rex::memory::GetPageSize() == 0))) {
    if (GetRuntimeSettings().protect_on_release.Get()) {
      if (!rex::memory::Protect(TranslateRelative(first_page << page_size_shift_), range_bytes,
                                rex::memory::PageAccess::kNoAccess, nullptr)) {
        REXSYS_WARN("BaseHeap::ReleaseRange failed due to host VirtualProtect failure");
      }
    }
  }

  u32 released = 0;
  for (u32 page_number = first_page; page_number <= last_page; ++page_number) {
    const PageEntry page_entry = page_table_[page_number];
    released += page_entry.title;
    if (page_entry.state & kernel::kMemoryAllocationCommit) {
      OnPageDecommitted(page_number);
    }
    StorePageEntry(page_table_[page_number], PageEntry{});
    unreserved_page_count_++;
  }
  memory_->ReleaseBudgetPages(released * (page_size_ >> 12));

  if (first_page > region_first) {
    for (u32 page_number = region_first; page_number < first_page; ++page_number) {
      PageEntry page_entry = page_table_[page_number];
      page_entry.region_page_count = first_page - region_first;
      StorePageEntry(page_table_[page_number], page_entry);
    }
  }
  if (last_page < region_last) {
    const u32 new_first = last_page + 1;
    for (u32 page_number = new_first; page_number <= region_last; ++page_number) {
      PageEntry page_entry = page_table_[page_number];
      page_entry.base_address = new_first;
      page_entry.region_page_count = region_last - last_page;
      StorePageEntry(page_table_[page_number], page_entry);
    }
  }

  if (out_address) {
    *out_address = heap_base_ + (first_page << page_size_shift_);
  }
  if (out_size) {
    *out_size = range_bytes;
  }
  return SyncHostPageAccess(first_page, last_page);
}

bool BaseHeap::Protect(u32 address, u32 size, u32 protect, u32* old_protect) {
  auto reconcile_lock = AcquireHostPageReconcileLock();
  return ProtectLocked(address, size, protect, old_protect);
}

bool BaseHeap::CheckProtectRangeLocked(u32 address, u32 size, u32* out_start_page,
                                       u32* out_end_page) {
  if (!size) {
    REXSYS_FAIL(false, "BaseHeap::Protect failed due to zero size");
  }

  // Protect covers every page from the one holding the first byte to the one holding the last, and
  // needs all of them committed.

  u32 start_page_number = (address - heap_base_) >> page_size_shift_;
  if (start_page_number >= page_table_.size()) {
    REXSYS_FAIL(false, "BaseHeap::Protect failed due to out-of-bounds base address {:08X}",
                address);
  }
  u32 end_page_number =
      static_cast<u32>((static_cast<u64>(address) + size - 1 - heap_base_) >> page_size_shift_);
  if (end_page_number >= page_table_.size()) {
    REXSYS_FAIL(false,
                "BaseHeap::Protect failed due to out-of-bounds range ({:08X} bytes "
                "from {:08x})",
                size, address);
  }

  u32 first_base_address = UINT_MAX;
  for (u32 page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    auto page_entry = page_table_[page_number];
    if (first_base_address == UINT_MAX) {
      first_base_address = page_entry.base_address;
    } else if (first_base_address != page_entry.base_address) {
      REXSYS_FAIL(false, "BaseHeap::Protect failed due to request spanning regions");
    }
    if (!(page_entry.state & kernel::kMemoryAllocationCommit)) {
      REXSYS_FAIL(false, "BaseHeap::Protect failed due to uncommitted page");
    }
  }

  *out_start_page = start_page_number;
  *out_end_page = end_page_number;
  return true;
}

bool BaseHeap::ProtectLocked(u32 address, u32 size, u32 protect, u32* old_protect) {
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  u32 start_page_number;
  u32 end_page_number;
  if (!CheckProtectRangeLocked(address, size, &start_page_number, &end_page_number)) {
    return false;
  }

  // Guest pages smaller than host pages are reconciled after the page table update.
  if (page_size_ >= memory_->system_page_size_) {
    const u32 page_count = end_page_number - start_page_number + 1;
    memory::PageAccess old_protect_access;
    if (!rex::memory::Protect(TranslateRelative(start_page_number << page_size_shift_),
                              page_count << page_size_shift_, ToPageAccess(protect),
                              old_protect ? &old_protect_access : nullptr)) {
      REXSYS_FAIL(false, "BaseHeap::Protect failed due to host VirtualProtect failure");
    }

    if (old_protect) {
      *old_protect = FromPageAccess(old_protect_access);
    }
  } else {
    if (old_protect) {
      *old_protect = page_table_[start_page_number].current_protect;
    }
  }

  for (u32 page_number = start_page_number; page_number <= end_page_number; ++page_number) {
    PageEntry page_entry = page_table_[page_number];
    page_entry.current_protect = protect;
    StorePageEntry(page_table_[page_number], page_entry);
  }

  if (!SyncHostPageAccess(start_page_number, end_page_number)) {
    return false;
  }

  return true;
}

bool BaseHeap::QueryRegionInfo(u32 base_address, HeapAllocation* out_info) {
  u32 start_page_number = (base_address - heap_base_) >> page_size_shift_;
  if (start_page_number >= page_table_.size()) {
    REXSYS_FAIL(false, "BaseHeap::QueryRegionInfo base page out of range");
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
    out_info->allocation_base = heap_base_ + (start_page_entry.base_address << page_size_shift_);
    out_info->allocation_protect = start_page_entry.allocation_protect;
    out_info->allocation_size = start_page_entry.region_page_count << page_size_shift_;
    out_info->state = start_page_entry.state;
    out_info->protect = start_page_entry.current_protect;

    for (u32 page_number = start_page_number;
         page_number < start_page_entry.base_address + start_page_entry.region_page_count;
         ++page_number) {
      auto page_entry = page_table_[page_number];
      if (page_entry.base_address != start_page_entry.base_address ||
          page_entry.state != start_page_entry.state ||
          page_entry.current_protect != start_page_entry.current_protect) {
        break;
      }
      out_info->region_size += page_size_;
    }
  } else {
    for (u32 page_number = start_page_number; page_number < page_table_.size(); ++page_number) {
      auto page_entry = page_table_[page_number];
      if (page_entry.state) {
        break;
      }
      out_info->region_size += page_size_;
    }
  }
  return true;
}

bool BaseHeap::QuerySize(u32 address, u32* out_size) {
  u32 page_number = (address - heap_base_) >> page_size_shift_;
  if (page_number >= page_table_.size()) {
    REXSYS_ERROR("BaseHeap::QuerySize base page out of range");
    *out_size = 0;
    return false;
  }
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
  auto page_entry = page_table_[page_number];
  *out_size = (page_entry.region_page_count << page_size_shift_);
  return true;
}

bool BaseHeap::QueryProtect(u32 address, u32* out_protect) {
  u32 page_number = (address - heap_base_) >> page_size_shift_;
  if (page_number >= page_table_.size()) {
    REXSYS_ERROR("BaseHeap::QueryProtect base page out of range");
    *out_protect = 0;
    return false;
  }
  const PageEntry page_entry = LoadPageEntry(page_table_[page_number]);
  *out_protect = page_entry.current_protect;
  return true;
}

rex::memory::PageAccess BaseHeap::QueryRangeAccess(u32 low_address, u32 high_address) {
  if (low_address > high_address || low_address < heap_base_ ||
      (high_address - heap_base_) >= heap_size_) {
    return rex::memory::PageAccess::kNoAccess;
  }
  u32 low_page_number = (low_address - heap_base_) >> page_size_shift_;
  u32 high_page_number = (high_address - heap_base_) >> page_size_shift_;
  bool all_readable = true;
  bool all_writable = true;
  {
    std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
    for (u32 i = low_page_number; i <= high_page_number; ++i) {
      u32 page_protect = page_table_[i].current_protect;
      if (!(page_protect & kernel::kMemoryProtectRead)) {
        all_readable = false;
      }
      if (!(page_protect & kernel::kMemoryProtectWrite) &&
          !(page_protect & kernel::kMemoryProtectWriteCombine)) {
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

void VirtualHeap::Initialize(kernel::GuestAddressSpace* memory, u8* membase, HeapType heap_type,
                             u32 heap_base, u32 heap_size, u32 page_size) {
  BaseHeap::Initialize(memory, membase, heap_type, heap_base, heap_size, page_size);
}

PhysicalHeap::PhysicalHeap() : parent_heap_(nullptr) {}

PhysicalHeap::~PhysicalHeap() = default;

void PhysicalHeap::Initialize(kernel::GuestAddressSpace* memory, u8* membase, HeapType heap_type,
                              u32 heap_base, u32 heap_size, u32 page_size,
                              VirtualHeap* parent_heap) {
  const u32 host_address_offset = rex::memory::detail::PhysicalHostOffset(heap_base);

  BaseHeap::Initialize(memory, membase, heap_type, heap_base, heap_size, page_size,
                       host_address_offset);
  profile_pool_ = nullptr;
  profile_plot_ = nullptr;
  parent_heap_ = parent_heap;
  system_page_size_ = static_cast<u32>(rex::memory::GetPageSize());

  system_page_count_ =
      (static_cast<size_t>(heap_size_) + host_address_offset + (system_page_size_ - 1)) /
      system_page_size_;
  system_page_flags_.resize((system_page_count_ + 63) / 64);
}

bool PhysicalHeap::Alloc(u32 size, u32 alignment, u32 allocation_type, u32 protect, bool top_down,
                         u32* out_address) {
  *out_address = 0;

  // Always top down. The parent heap is bottom up, so this avoids collisions.
  top_down = true;

  size = rex::RoundUpNonZero(size, page_size_);
  alignment = rex::RoundUpNonZero(alignment, page_size_);

  PendingScribble scribble(allocation_type, protect);
  auto reconcile_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // The parent heap allocation gives a physical address in the first 512MB.
  u32 parent_heap_start = GetPhysicalAddress(heap_base_);
  u32 parent_heap_end = GetPhysicalAddress(heap_base_ + (heap_size_ - 1));
  u32 parent_address;
  if (!parent_heap_->AllocRangeLocked(parent_heap_start, parent_heap_end, size, alignment,
                                      allocation_type, protect, top_down, &parent_address)) {
    REXSYS_FAIL(false, "PhysicalHeap::Alloc unable to alloc physical memory in parent heap");
  }

  u32 address = heap_base_ + parent_address - parent_heap_start;
  if (!BaseHeap::AllocFixedLocked(
          address, size, alignment,
          allocation_type & ~static_cast<u32>(kernel::kMemoryAllocationTitle), protect)) {
    REXSYS_ERROR("PhysicalHeap::Alloc unable to pin physical memory in physical heap");
    parent_heap_->ReleaseLocked(parent_address, nullptr);
    return false;
  }
  scribble.Add(*parent_heap_, parent_address, size);
  scribble.Add(*this, address, size);
  *out_address = address;
  return true;
}

bool PhysicalHeap::AllocFixed(u32 base_address, u32 size, u32 alignment, u32 allocation_type,
                              u32 protect) {
  size = rex::RoundUpNonZero(size, page_size_);
  alignment = rex::RoundUpNonZero(alignment, page_size_);

  PendingScribble scribble(allocation_type, protect);
  auto reconcile_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // The parent heap allocation gives a physical address in the first 512MB.
  u32 parent_base_address = GetPhysicalAddress(base_address);
  std::lock_guard<std::recursive_mutex> parent_lock(parent_heap_->heap_mutex_);
  const u32 parent_size =
      parent_base_address % alignment == 0 ? rex::RoundUpPow2(size, alignment) : size;
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
    REXSYS_FAIL(false, "PhysicalHeap::AllocFixed range at {:08X} is partly in use", base_address);
  }
  if (!parent_heap_->AllocFixedLocked(parent_base_address, size, alignment, allocation_type,
                                      protect)) {
    REXSYS_FAIL(false, "PhysicalHeap::AllocFixed unable to alloc physical memory in parent heap");
  }

  u32 address = heap_base_ + parent_base_address - GetPhysicalAddress(heap_base_);
  if (!BaseHeap::AllocFixedLocked(
          address, size, page_size_,
          allocation_type & ~static_cast<u32>(kernel::kMemoryAllocationTitle), protect)) {
    REXSYS_ERROR("PhysicalHeap::AllocFixed unable to pin physical memory in physical heap");
    if (parent_was_free) {
      parent_heap_->ReleaseLocked(parent_base_address, nullptr);
    }
    return false;
  }
  scribble.Add(*parent_heap_, parent_base_address, size);
  scribble.Add(*this, address, size);

  return true;
}

bool PhysicalHeap::AllocRange(u32 low_address, u32 high_address, u32 size, u32 alignment,
                              u32 allocation_type, u32 protect, bool top_down, u32* out_address) {
  *out_address = 0;

  size = rex::RoundUpNonZero(size, page_size_);
  alignment = rex::RoundUpNonZero(alignment, page_size_);

  PendingScribble scribble(allocation_type, protect);
  auto reconcile_lock = AcquireHostPageReconcileLock();
  std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);

  // The parent heap allocation gives a physical address in the first 512MB.
  low_address = std::max(heap_base_, low_address);
  high_address = std::min(heap_base_ + (heap_size_ - 1), high_address);
  u32 parent_low_address = GetPhysicalAddress(low_address);
  u32 parent_high_address = GetPhysicalAddress(high_address);
  u32 parent_address;
  if (!parent_heap_->AllocRangeLocked(parent_low_address, parent_high_address, size, alignment,
                                      allocation_type, protect, top_down, &parent_address)) {
    REXSYS_FAIL(false, "PhysicalHeap::AllocRange unable to alloc physical memory in parent heap");
  }

  u32 address = heap_base_ + parent_address - GetPhysicalAddress(heap_base_);
  if (!BaseHeap::AllocFixedLocked(
          address, size, page_size_,
          allocation_type & ~static_cast<u32>(kernel::kMemoryAllocationTitle), protect)) {
    REXSYS_ERROR("PhysicalHeap::AllocRange unable to pin physical memory in physical heap");
    parent_heap_->ReleaseLocked(parent_address, nullptr);
    return false;
  }
  scribble.Add(*parent_heap_, parent_address, size);
  scribble.Add(*this, address, size);
  *out_address = address;
  return true;
}

bool PhysicalHeap::AllocSystemHeap(u32 size, u32 alignment, u32 allocation_type, u32 protect,
                                   bool top_down, u32* out_address) {
  return Alloc(size, alignment, allocation_type, protect, top_down, out_address);
}

bool PhysicalHeap::Decommit(u32 address, u32 size) {
  std::lock_guard lock(memory_->watch_mutex_);

  u32 parent_address = GetPhysicalAddress(address);
  if (!parent_heap_->DecommitLocked(parent_address, size)) {
    REXSYS_FAIL(false, "PhysicalHeap::Decommit failed due to parent heap failure");
  }

  TriggerCallbacksLocked(address, size, true, true);

  return BaseHeap::DecommitLocked(address, size);
}

bool PhysicalHeap::Release(u32 base_address, u32* out_region_size) {
  std::lock_guard lock(memory_->watch_mutex_);

  u32 parent_base_address = GetPhysicalAddress(base_address);
  if (!parent_heap_->ReleaseLocked(parent_base_address, out_region_size)) {
    REXSYS_FAIL(false, "PhysicalHeap::Release failed due to parent heap failure");
  }

  // Invalidate here because Alloc cannot see callbacks enabled in other heaps mapping the memory.
  u32 region_size;
  if (QuerySize(base_address, &region_size)) {
    TriggerCallbacksLocked(base_address, region_size, true, true);
  }

  return BaseHeap::ReleaseLocked(base_address, out_region_size);
}

bool PhysicalHeap::Protect(u32 address, u32 size, u32 protect, u32* old_protect) {
  std::lock_guard lock(memory_->watch_mutex_);

  u32 start_page_number;
  u32 end_page_number;
  {
    std::lock_guard<std::recursive_mutex> parent_lock(parent_heap_->heap_mutex_);
    if (!parent_heap_->CheckProtectRangeLocked(GetPhysicalAddress(address), size,
                                               &start_page_number, &end_page_number)) {
      return false;
    }
  }
  {
    std::lock_guard<std::recursive_mutex> heap_lock(heap_mutex_);
    if (!CheckProtectRangeLocked(address, size, &start_page_number, &end_page_number)) {
      return false;
    }
  }

  // Only invalidate when making writable again, not when marking a range immutable.
  if (protect & kernel::kMemoryProtectWrite) {
    TriggerCallbacksLocked(address, size, true, true, false);
  }

  if (!parent_heap_->ProtectLocked(GetPhysicalAddress(address), size, protect, old_protect)) {
    REXSYS_FAIL(false, "PhysicalHeap::Protect failed due to parent heap failure");
  }

  return BaseHeap::ProtectLocked(address, size, protect, nullptr);
}

/**
 * Enables write invalidation notifications for system pages.
 *
 * A page the title cannot write is left alone because access violations must reach the title there.
 *
 * @param physical_address Start of the physical range.
 * @param length Size of the range in bytes.
 * @param enable_invalidation_notifications True to notify when the title writes the range.
 * @param enable_data_providers True to enable data providers, unsupported.
 */
void PhysicalHeap::EnableAccessCallbacks(u32 physical_address, u32 length,
                                         bool enable_invalidation_notifications,
                                         bool enable_data_providers) {
  REX_ASSERT(!enable_data_providers);
  if (!enable_invalidation_notifications && !enable_data_providers) {
    return;
  }
  u32 physical_address_offset = GetPhysicalAddress(heap_base_);
  if (physical_address < physical_address_offset) {
    if (physical_address_offset - physical_address >= length) {
      return;
    }
    length -= physical_address_offset - physical_address;
    physical_address = physical_address_offset;
  }
  u32 heap_relative_address = physical_address - physical_address_offset;
  if (heap_relative_address >= heap_size_) {
    return;
  }
  length = std::min(length, heap_size_ - heap_relative_address);
  if (length == 0) {
    return;
  }

  u32 system_page_first = (heap_relative_address + GetHostAddressOffset()) / system_page_size_;
  u32 system_page_last =
      (heap_relative_address + length - 1 + GetHostAddressOffset()) / system_page_size_;
  system_page_last = std::min(system_page_last, system_page_count_ - 1);
  REX_ASSERT(system_page_first <= system_page_last);

  rex::memory::PageAccess protect_access = enable_data_providers
                                               ? rex::memory::PageAccess::kNoAccess
                                               : rex::memory::PageAccess::kReadOnly;
  u8* protect_base = membase_ + heap_base_;
  u32 protect_system_page_first = UINT32_MAX;
  std::lock_guard lock(memory_->watch_mutex_);
  for (u32 i = system_page_first; i <= system_page_last; ++i) {
    SystemPageFlagsBlock& page_flags_block = system_page_flags_[i >> 6];
    u64 page_flags_bit = static_cast<u64>(1) << (i & 63);
    u32 guest_page_number =
        rex::SatSub(i * system_page_size_, GetHostAddressOffset()) >> page_size_shift_;
    if (guest_page_number >= page_table_.size()) {
      REXSYS_ERROR("Access callback page OOB: system_page={} guest_page={} offset=0x{:X}", i,
                   guest_page_number, GetHostAddressOffset());
      REX_ASSERT_ALWAYS();
      continue;
    }
    // A host page larger than a guest page takes the access of every guest page in it.
    rex::memory::PageAccess current_page_access =
        page_size_ < system_page_size_
            ? GetHostPageAccess(i)
            : ToPageAccess(page_table_[guest_page_number].current_protect);
    bool protect_system_page = false;
    if (current_page_access != rex::memory::PageAccess::kNoAccess) {
      if (enable_invalidation_notifications) {
        if (current_page_access != rex::memory::PageAccess::kReadOnly &&
            (page_flags_block.notify_on_invalidation & page_flags_bit) == 0) {
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

bool PhysicalHeap::TriggerCallbacks(u32 virtual_address, u32 length, bool is_write,
                                    bool unwatch_exact_range, bool unprotect) {
  std::lock_guard lock(memory_->watch_mutex_);
  return TriggerCallbacksLocked(virtual_address, length, is_write, unwatch_exact_range, unprotect);
}

bool PhysicalHeap::TriggerCallbacksLocked(u32 virtual_address, u32 length, bool is_write,
                                          bool unwatch_exact_range, bool unprotect) {
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
  u32 heap_relative_address = virtual_address - heap_base_;
  if (heap_relative_address >= heap_size_) {
    return false;
  }
  length = std::min(length, heap_size_ - heap_relative_address);
  if (length == 0) {
    return false;
  }

  u32 system_page_first = (heap_relative_address + GetHostAddressOffset()) / system_page_size_;
  u32 system_page_last =
      (heap_relative_address + length - 1 + GetHostAddressOffset()) / system_page_size_;
  system_page_last = std::min(system_page_last, system_page_count_ - 1);
  REX_ASSERT(system_page_first <= system_page_last);
  u32 block_index_first = system_page_first >> 6;
  u32 block_index_last = system_page_last >> 6;

  bool any_watched = false;
  for (u32 i = block_index_first; i <= block_index_last; ++i) {
    u64 block = system_page_flags_[i].notify_on_invalidation;
    if (i == block_index_first) {
      block &= ~((static_cast<u64>(1) << (system_page_first & 63)) - 1);
    }
    if (i == block_index_last && (system_page_last & 63) != 63) {
      block &= (static_cast<u64>(1) << ((system_page_last & 63) + 1)) - 1;
    }
    if (block) {
      any_watched = true;
      break;
    }
  }
  if (!any_watched) {
    return false;
  }

  if (!unprotect) {
    unwatch_exact_range = true;
  }
  u32 physical_address_offset = GetPhysicalAddress(heap_base_);
  u32 physical_address_start =
      rex::SatSub(system_page_first * system_page_size_, GetHostAddressOffset()) +
      physical_address_offset;
  u32 physical_length =
      std::min(rex::SatSub(system_page_last * system_page_size_ + system_page_size_,
                           GetHostAddressOffset()) +
                   physical_address_offset - physical_address_start,
               heap_size_ - (physical_address_start - physical_address_offset));
  u32 unwatch_first = 0;
  u32 unwatch_last = UINT32_MAX;
  for (const auto& invalidation_callback : memory_->physical_memory_invalidation_callbacks_) {
    std::pair<u32, u32> callback_unwatch_range =
        invalidation_callback->first(invalidation_callback->second, physical_address_start,
                                     physical_length, unwatch_exact_range);
    if (!unwatch_exact_range) {
      unwatch_first = std::max(unwatch_first, callback_unwatch_range.first);
      unwatch_last =
          std::min(unwatch_last,
                   rex::SatAdd(callback_unwatch_range.first,
                               std::max(callback_unwatch_range.second, static_cast<u32>(1)) - 1));
    }
  }
  if (!unwatch_exact_range) {
    unwatch_first = std::min(unwatch_first, physical_address_start);
    unwatch_last = std::max(unwatch_last, physical_address_start + physical_length - 1);
    // Unprotect at most 4MB, which caps the page loop at 1024 iterations.
    constexpr u32 kMaxUnwatchExcess = 4 * 1024 * 1024;
    unwatch_first = std::max(unwatch_first, physical_address_start & ~(kMaxUnwatchExcess - 1));
    unwatch_last = std::min(
        unwatch_last, (physical_address_start + physical_length - 1) | (kMaxUnwatchExcess - 1));
    unwatch_first = rex::SatSub(unwatch_first, physical_address_offset);
    unwatch_last = rex::SatSub(unwatch_last, physical_address_offset);
    unwatch_first = std::min(unwatch_first, heap_size_ - 1);
    unwatch_last = std::min(unwatch_last, heap_size_ - 1);
    unwatch_first += GetHostAddressOffset();
    unwatch_last += GetHostAddressOffset();
    REX_ASSERT(unwatch_first <= unwatch_last);
    system_page_first = unwatch_first / system_page_size_;
    system_page_last = unwatch_last / system_page_size_;
    block_index_first = system_page_first >> 6;
    block_index_last = system_page_last >> 6;
  }

  if (unprotect) {
    u8* protect_base = membase_ + heap_base_;
    u32 unprotect_system_page_first = UINT32_MAX;
    for (u32 i = system_page_first; i <= system_page_last; ++i) {
      bool unprotect_page = (system_page_flags_[i >> 6].notify_on_invalidation &
                             (static_cast<u64>(1) << (i & 63))) != 0;
      if (unprotect_page) {
        u32 guest_page_number =
            rex::SatSub(i * system_page_size_, GetHostAddressOffset()) >> page_size_shift_;
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

  for (u32 i = block_index_first; i <= block_index_last; ++i) {
    u64 mask = 0;
    if (i == block_index_first) {
      mask |= (static_cast<u64>(1) << (system_page_first & 63)) - 1;
    }
    if (i == block_index_last && (system_page_last & 63) != 63) {
      mask |= ~((static_cast<u64>(1) << ((system_page_last & 63) + 1)) - 1);
    }
    system_page_flags_[i].notify_on_invalidation &= mask;
  }

  return true;
}

std::unique_lock<rex::platform::RankedMutex> PhysicalHeap::AcquireHostPageReconcileLock() const {
  auto lock = BaseHeap::AcquireHostPageReconcileLock();
  if (!lock.owns_lock() && parent_heap_ &&
      parent_heap_->GetPageSize() < memory_->system_page_size_) {
    lock = std::unique_lock(memory_->watch_mutex_);
  }
  return lock;
}

bool PhysicalHeap::IsHostPageWriteWatched(u32 host_page_number) const {
  // Indices count host pages from membase_ + heap_base_, including GetHostAddressOffset().
  if (host_page_number >= system_page_count_) {
    return false;
  }
  return (system_page_flags_[host_page_number >> 6].notify_on_invalidation &
          (static_cast<u64>(1) << (host_page_number & 63))) != 0;
}

u32 PhysicalHeap::GetPhysicalAddress(u32 address) const {
  REX_ASSERT(address >= heap_base_);
  address -= heap_base_;
  REX_ASSERT(address < heap_size_);
  if (heap_base_ >= 0xE0000000) {
    address += 0x1000;
  }
  return address;
}

}  // namespace rex::kernel
