#include "kernel/shim/heap.h"

#include <algorithm>
#include <cstring>

#include <rex/math.h>
#include <rex/ppc/hook.h>
#include "kernel/shim/function.h"
#include <rex/kernel/guest_allocator.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/title_runtime.h>
#include <rex/logging/log_category.h>
#include "kernel/categories.h"

#include "kernel/title_state.h"
#include "kernel/kernel.h"

namespace {

struct SizeHeader {
  u64 requested_size;
  u64 reserved;
};
static_assert(sizeof(SizeHeader) == rex::kernel::GuestAllocator::kBlockAlignment,
              "SizeHeader must be exactly 1 block alignment unit");

constexpr u32 kHeaderSize = rex::kernel::GuestAllocator::kBlockAlignment;
constexpr u32 kMinSegmentSize = 4u * 1024u * 1024u;
constexpr u32 kDefaultGrowthSegmentSize = 64u * 1024u * 1024u;

constexpr u32 kHeapZeroMemory = 0x00000008;

}  // namespace

namespace rex::kernel::shim {

void* GuestHeap::GuestToHost(u32 guest_addr) const {
  return rex::memory::GuestPtr<void*>(membase_, guest_addr);
}

u32 GuestHeap::HostToGuest(void* host_ptr) const {
  return rex::memory::HostPtrToGuest(membase_, host_ptr);
}

bool GuestHeap::Init(u32 heap_size_bytes, rex::kernel::GuestAddressSpace* memory) {
  memory_ = memory;
  auto* mem = memory_;
  if (!mem) {
    REXKRNL_FAIL(false, "shim_heap: memory is null");
  }

  membase_ = mem->GetVirtualMembase();
  if (!membase_) {
    REXKRNL_FAIL(false, "shim_heap: virtual_membase is null");
  }

  initial_segment_size_ =
      rex::RoundUpPow2<u32>(std::max(heap_size_bytes, kMinSegmentSize), kHeaderSize);

  std::lock_guard lock(mutex_);
  segments_.clear();
  if (!AllocateSegmentLocked(initial_segment_size_)) {
    return false;
  }

  const auto diagnostics = GetStatsLocked();
  REXKRNL_DEBUG("shim_heap: initialized with {} segment(s), capacity={}MB", segments_.size(),
                diagnostics.capacity >> 20);
  return true;
}

void GuestHeap::Shutdown() {
  std::lock_guard lock(mutex_);
  segments_.clear();
}

u32 GuestHeap::Alloc(u32 size, bool zero) {
  std::lock_guard lock(mutex_);
  return AllocLocked(size, zero);
}

void GuestHeap::Free(u32 guest_addr) {
  if (!guest_addr)
    return;
  std::lock_guard lock(mutex_);
  auto* segment = FindSegmentByGuestLocked(guest_addr);
  if (!segment || guest_addr < segment->GetBase() + kHeaderSize) {
    REXKRNL_WARN("rexshim_RtlFreeHeap: skipping OOB ptr 0x{:08X}", guest_addr);
    return;
  }

  segment->Free(GuestToHost(guest_addr - kHeaderSize));
}

u32 GuestHeap::GetSize(u32 guest_addr) {
  if (!guest_addr)
    return ~0u;

  std::lock_guard lock(mutex_);
  auto* segment = FindSegmentByGuestLocked(guest_addr);
  if (!segment || guest_addr < segment->GetBase() + kHeaderSize) {
    return ~0u;
  }

  auto* hdr = static_cast<SizeHeader*>(GuestToHost(guest_addr - kHeaderSize));
  return static_cast<u32>(hdr->requested_size);
}

u32 GuestHeap::Realloc(u32 guest_addr, u32 new_size, bool zero_new) {
  std::lock_guard lock(mutex_);
  if (!guest_addr) {
    return AllocLocked(new_size, zero_new);
  }
  if (new_size == 0)
    new_size = 1;

  auto* segment = FindSegmentByGuestLocked(guest_addr);
  if (!segment || guest_addr < segment->GetBase() + kHeaderSize) {
    REXKRNL_WARN("rexshim_RtlReAllocateHeap: OOB ptr 0x{:08X}, treating as new alloc({})",
                 guest_addr, new_size);
    return AllocLocked(new_size, zero_new);
  }

  void* real_host = GuestToHost(guest_addr - kHeaderSize);

  auto* old_hdr = static_cast<SizeHeader*>(real_host);
  u32 old_size = static_cast<u32>(old_hdr->requested_size);
  void* new_ptr = segment->Reallocate(real_host, new_size + kHeaderSize);
  if (!new_ptr) {
    u32 new_guest = AllocLocked(new_size, false);
    if (!new_guest) {
      REXKRNL_WARN("rexshim_RtlReAllocateHeap: Reallocate({}) failed", new_size);
      return 0;
    }

    void* new_user_ptr = GuestToHost(new_guest);
    void* old_user_ptr = GuestToHost(guest_addr);
    u32 copy_size = std::min(old_size, new_size);
    std::memcpy(new_user_ptr, old_user_ptr, copy_size);
    if (zero_new && new_size > old_size) {
      std::memset(static_cast<u8*>(new_user_ptr) + old_size, 0, new_size - old_size);
    }

    segment->Free(real_host);
    return new_guest;
  }

  auto* new_hdr = static_cast<SizeHeader*>(new_ptr);
  new_hdr->requested_size = new_size;

  void* user_ptr = static_cast<u8*>(new_ptr) + kHeaderSize;
  if (zero_new && new_size > old_size) {
    std::memset(static_cast<u8*>(user_ptr) + old_size, 0, new_size - old_size);
  }

  return HostToGuest(user_ptr);
}

bool GuestHeap::AllocateSegmentLocked(u32 segment_size_bytes) {
  auto* mem = memory_;
  if (!mem) {
    REXKRNL_FAIL(false, "shim_heap: kernel memory is null during segment allocation");
  }

  segment_size_bytes =
      rex::RoundUpPow2<u32>(std::max(segment_size_bytes, kMinSegmentSize), kHeaderSize);

  // The system heap range is shared with kernel bookkeeping allocations and cannot hold a segment.
  auto segment = kernel::GuestAllocator::Create(*mem, segment_size_bytes, kHeaderSize, false,
                                                kernel::kPageSize4KiB, 0, kernel::GuestPool::kHeap);
  if (!segment) {
    REXKRNL_FAIL(false, "shim_heap: segment allocation of {} bytes failed", segment_size_bytes);
  }

  const u32 guest_base = (*segment)->GetBase();
  REXKRNL_DEBUG("shim_heap: added segment guest=0x{:08X}-0x{:08X} size={}MB", guest_base,
                guest_base + segment_size_bytes, segment_size_bytes / (1024 * 1024));
  segments_.push_back(std::move(*segment));
  return true;
}

u32 GuestHeap::AllocLocked(u32 size, bool zero) {
  if (size == 0) {
    size = 1;
  }

  const u32 alloc_size = size + kHeaderSize;
  auto try_allocate_in_segment = [&](kernel::GuestAllocator& segment) -> u32 {
    void* ptr = segment.Allocate(alloc_size);
    if (!ptr) {
      return 0;
    }

    auto* hdr = static_cast<SizeHeader*>(ptr);
    hdr->requested_size = size;
    hdr->reserved = 0;

    void* user_ptr = static_cast<u8*>(ptr) + kHeaderSize;
    if (zero) {
      std::memset(user_ptr, 0, size);
    }
    return HostToGuest(user_ptr);
  };

  for (auto& segment : segments_) {
    if (u32 addr = try_allocate_in_segment(*segment)) {
      return addr;
    }
  }

  const u32 min_required_segment = rex::RoundUpPow2<u32>(alloc_size + kHeaderSize, kHeaderSize);
  const u32 growth_segment_size =
      std::max({initial_segment_size_, kDefaultGrowthSegmentSize, min_required_segment});
  if (!AllocateSegmentLocked(growth_segment_size)) {
    const auto diagnostics = GetStatsLocked();
    bool invariants_ok = true;
    for (const auto& segment : segments_) {
      invariants_ok = invariants_ok && segment->IsConsistent();
    }
    const u64 free_bytes = diagnostics.capacity - diagnostics.allocated;
    REXKRNL_WARN(
        "rexshim_RtlAllocateHeap: Allocate({}) failed (segments={}, capacity={}MB, "
        "allocated={}MB, free={}MB, oom_count={}, invariants_ok={})",
        size, segments_.size(), diagnostics.capacity >> 20, diagnostics.allocated >> 20,
        free_bytes >> 20, diagnostics.oom_count, invariants_ok);
    return 0;
  }

  if (u32 addr = try_allocate_in_segment(*segments_.back())) {
    return addr;
  }

  const auto diagnostics = GetStatsLocked();
  const u64 free_bytes = diagnostics.capacity - diagnostics.allocated;
  REXKRNL_WARN(
      "rexshim_RtlAllocateHeap: allocation failed after growth for {} bytes "
      "(segments={}, capacity={}MB, allocated={}MB, free={}MB, oom_count={})",
      size, segments_.size(), diagnostics.capacity >> 20, diagnostics.allocated >> 20,
      free_bytes >> 20, diagnostics.oom_count);
  return 0;
}

kernel::GuestAllocator* GuestHeap::FindSegmentByGuestLocked(u32 guest_addr) {
  auto it = std::ranges::find_if(
      segments_, [&](const auto& segment) { return segment->Contains(guest_addr); });
  return it != segments_.end() ? it->get() : nullptr;
}

kernel::GuestAllocator::AllocatorStats GuestHeap::GetStatsLocked() const {
  kernel::GuestAllocator::AllocatorStats diagnostics{};
  for (const auto& segment : segments_) {
    const auto d = segment->GetStats();
    diagnostics.capacity += d.capacity;
    diagnostics.allocated += d.allocated;
    diagnostics.peak_allocated += d.peak_allocated;
    diagnostics.peak_request_size = std::max(diagnostics.peak_request_size, d.peak_request_size);
    diagnostics.oom_count += d.oom_count;
  }
  return diagnostics;
}

REX_SHIM_FN(RtlAllocateHeap, u32, [[maybe_unused]] u32 hHeap, u32 dwFlags, u32 dwBytes) {
  return kernel::GetCurrentState().heap.Alloc(dwBytes, dwFlags & kHeapZeroMemory);
}

REX_SHIM_FN(RtlFreeHeap, u32, [[maybe_unused]] u32 hHeap, [[maybe_unused]] u32 dwFlags, u32 ptr) {
  kernel::GetCurrentState().heap.Free(ptr);
  return 1;
}

REX_SHIM_FN(RtlSizeHeap, u32, [[maybe_unused]] u32 hHeap, [[maybe_unused]] u32 dwFlags, u32 ptr) {
  return kernel::GetCurrentState().heap.GetSize(ptr);
}

REX_SHIM_FN(RtlReAllocateHeap, u32, [[maybe_unused]] u32 hHeap, u32 dwFlags, u32 ptr, u32 dwBytes) {
  return kernel::GetCurrentState().heap.Realloc(ptr, dwBytes, dwFlags & kHeapZeroMemory);
}

bool InitHeap(u32 heap_size_mb, rex::kernel::GuestAddressSpace* memory) {
  TitleRuntime* runtime = REX_RUNTIME();
  if (!runtime) {
    REXKRNL_FAIL(false, "shim_heap: no runtime is set up");
  }
  return StateOf(*runtime).heap.Init(heap_size_mb * 1024u * 1024u, memory);
}

void ShutdownHeap() {
  if (TitleRuntime* runtime = REX_RUNTIME()) {
    StateOf(*runtime).heap.Shutdown();
  }
}

}  // namespace rex::kernel::shim
