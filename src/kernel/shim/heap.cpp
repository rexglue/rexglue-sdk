/**
 * @file        kernel/shim/heap.cpp
 *
 * @brief       ReXHeap implementation using o1heap with size header pattern.
 *              Hooks RtlAllocateHeap, RtlFreeHeap, RtlSizeHeap, RtlReAllocateHeap.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <rex/kernel/shim/heap.h>

#include <algorithm>
#include <cstring>
#include <o1heap.h>

#include <rex/cvar.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/kernel/shim/define.h>
#include <rex/runtime/xmemory.h>
#include <rex/logging.h>

// ---------------------------------------------------------------------------
// Size header: prepended to every allocation so we can answer RtlSizeHeap
// without o1heap exposing per-allocation usable size.
// ---------------------------------------------------------------------------
namespace {

struct SizeHeader {
  u64 requested_size;
  u64 reserved;  // padding to O1HEAP_ALIGNMENT
};
static_assert(sizeof(SizeHeader) == O1HEAP_ALIGNMENT,
              "SizeHeader must be exactly one O1HEAP_ALIGNMENT unit");

constexpr u32 kHeaderSize = static_cast<u32>(O1HEAP_ALIGNMENT);
constexpr u32 kMinSegmentSize = 4u * 1024u * 1024u;
constexpr u32 kDefaultGrowthSegmentSize = 64u * 1024u * 1024u;

#ifndef HEAP_ZERO_MEMORY
constexpr u32 HEAP_ZERO_MEMORY = 0x00000008;
#endif

}  // namespace

// ---------------------------------------------------------------------------
// ReXHeap implementation
// ---------------------------------------------------------------------------

namespace rex::kernel::shim {

void* ReXHeap::GuestToHost(u32 guest_addr) const {
  return rex::runtime::GuestPtr<void*>(membase_, guest_addr);
}

u32 ReXHeap::HostToGuest(void* host_ptr) const {
  return rex::runtime::detail::RemovePhysicalHostOffset(
      static_cast<std::size_t>(static_cast<u8*>(host_ptr) - membase_));
}

bool ReXHeap::Init(u32 heap_size_bytes, rex::runtime::Memory* memory) {
  memory_ = memory;
  auto* mem = memory_;
  if (!mem) {
    REX_FAIL(kernel, false, "shim_heap: memory is null");
  }

  membase_ = mem->virtual_membase();
  if (!membase_) {
    REX_FAIL(kernel, false, "shim_heap: virtual_membase is null");
  }

  initial_segment_size_ =
      rex::align<u32>(std::max(heap_size_bytes, kMinSegmentSize), O1HEAP_ALIGNMENT);

  std::lock_guard lock(mutex_);
  segments_.clear();
  if (!AllocateSegmentLocked(initial_segment_size_)) {
    return false;
  }

  const auto diagnostics = GetDiagnosticsLocked();
  REXLOG_DEBUG(kernel, "shim_heap: initialized with {} segment(s), capacity={}MB", segments_.size(),
               diagnostics.capacity / (1024 * 1024));
  return true;
}

u32 ReXHeap::Alloc(u32 size, bool zero) {
  std::lock_guard lock(mutex_);
  return AllocLocked(size, zero);
}

void ReXHeap::Free(u32 guest_addr) {
  if (!guest_addr)
    return;
  std::lock_guard lock(mutex_);
  auto* segment = FindSegmentByGuestLocked(guest_addr);
  if (!segment || guest_addr < segment->guest_base + kHeaderSize) {
    REXLOG_WARN(kernel, "rexshim_RtlFreeHeap: skipping OOB ptr 0x{:08X}", guest_addr);
    return;
  }

  void* real_host = GuestToHost(guest_addr - kHeaderSize);
  o1heapFree(segment->heap, real_host);
}

u32 ReXHeap::Size(u32 guest_addr) {
  if (!guest_addr)
    return ~0u;

  std::lock_guard lock(mutex_);
  auto* segment = FindSegmentByGuestLocked(guest_addr);
  if (!segment || guest_addr < segment->guest_base + kHeaderSize) {
    return ~0u;
  }

  auto* hdr = static_cast<SizeHeader*>(GuestToHost(guest_addr - kHeaderSize));
  return static_cast<u32>(hdr->requested_size);
}

u32 ReXHeap::Realloc(u32 guest_addr, u32 new_size, bool zero_new) {
  std::lock_guard lock(mutex_);
  if (!guest_addr) {
    return AllocLocked(new_size, zero_new);
  }
  if (new_size == 0)
    new_size = 1;

  auto* segment = FindSegmentByGuestLocked(guest_addr);
  if (!segment || guest_addr < segment->guest_base + kHeaderSize) {
    // Pre-hook allocation outside our heap -- treat as fresh alloc.
    REXLOG_WARN(kernel, "rexshim_RtlReAllocateHeap: OOB ptr 0x{:08X}, treating as new alloc({})",
                guest_addr, new_size);
    return AllocLocked(new_size, zero_new);
  }

  void* real_host = GuestToHost(guest_addr - kHeaderSize);

  auto* old_hdr = static_cast<SizeHeader*>(real_host);
  u32 old_size = static_cast<u32>(old_hdr->requested_size);
  void* new_ptr = o1heapReallocate(segment->heap, real_host, new_size + kHeaderSize);
  if (!new_ptr) {
    // Cross-segment fallback: allocate a fresh block, copy, then free old.
    u32 new_guest = AllocLocked(new_size, false);
    if (!new_guest) {
      REXLOG_WARN(kernel, "rexshim_RtlReAllocateHeap: o1heapReallocate({}) failed", new_size);
      return 0;
    }

    void* new_user_ptr = GuestToHost(new_guest);
    void* old_user_ptr = GuestToHost(guest_addr);
    u32 copy_size = std::min(old_size, new_size);
    std::memcpy(new_user_ptr, old_user_ptr, copy_size);
    if (zero_new && new_size > old_size) {
      std::memset(static_cast<u8*>(new_user_ptr) + old_size, 0, new_size - old_size);
    }

    o1heapFree(segment->heap, real_host);
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

bool ReXHeap::AllocateSegmentLocked(u32 segment_size_bytes) {
  auto* mem = memory_;
  if (!mem) {
    REX_FAIL(kernel, false, "shim_heap: kernel memory is null during segment allocation");
  }

  segment_size_bytes =
      rex::align<u32>(std::max(segment_size_bytes, kMinSegmentSize), O1HEAP_ALIGNMENT);

  u32 guest_base = 0;

  // Allocate from the regular virtual heap (top-down) instead of the system
  // heap. The system heap range is shared with kernel bookkeeping allocations
  // (KernelGuestGlobals, thread PCR/TLS and module headers) and cannot
  // accommodate a large contiguous shim heap segment alongside them.
  auto* vheap = mem->LookupHeapByType(false, 4096);
  if (!vheap ||
      !vheap->Alloc(segment_size_bytes, O1HEAP_ALIGNMENT,
                    rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
                    rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, true,
                    &guest_base)) {
    REX_FAIL(kernel, false, "shim_heap: virtual heap allocation of {} bytes failed",
             segment_size_bytes);
  }

  u8* host_base = mem->TranslateVirtual<u8*>(guest_base);
  if (!host_base) {
    REX_FAIL(kernel, false, "shim_heap: TranslateVirtual failed for guest base 0x{:08X}",
             guest_base);
  }

  O1HeapInstance* heap = o1heapInit(host_base, segment_size_bytes);
  if (!heap) {
    REX_FAIL(kernel, false, "shim_heap: o1heapInit failed for segment 0x{:08X} size {}", guest_base,
             segment_size_bytes);
  }

  segments_.push_back(
      HeapSegment{heap, guest_base, static_cast<u32>(guest_base + segment_size_bytes)});
  REXLOG_DEBUG(kernel, "shim_heap: added segment guest=0x{:08X}-0x{:08X} size={}MB", guest_base,
               guest_base + segment_size_bytes, segment_size_bytes / (1024 * 1024));
  return true;
}

u32 ReXHeap::AllocLocked(u32 size, bool zero) {
  if (size == 0) {
    size = 1;
  }

  const u32 alloc_size = size + kHeaderSize;
  auto try_allocate_in_segment = [&](HeapSegment& segment) -> u32 {
    void* ptr = o1heapAllocate(segment.heap, alloc_size);
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
    if (u32 addr = try_allocate_in_segment(segment)) {
      return addr;
    }
  }

  const u32 min_required_segment = rex::align<u32>(alloc_size + kHeaderSize, O1HEAP_ALIGNMENT);
  const u32 growth_segment_size =
      std::max({initial_segment_size_, kDefaultGrowthSegmentSize, min_required_segment});
  if (!AllocateSegmentLocked(growth_segment_size)) {
    const auto diagnostics = GetDiagnosticsLocked();
    bool invariants_ok = true;
    for (const auto& segment : segments_) {
      invariants_ok = invariants_ok && o1heapDoInvariantsHold(segment.heap);
    }
    const u64 free_bytes = diagnostics.capacity - diagnostics.allocated;
    REXLOG_WARN(kernel,
                "rexshim_RtlAllocateHeap: o1heapAllocate({}) failed (segments={}, capacity={}MB, "
                "allocated={}MB, free={}MB, oom_count={}, invariants_ok={})",
                size, segments_.size(), diagnostics.capacity / (1024 * 1024),
                diagnostics.allocated / (1024 * 1024), free_bytes / (1024 * 1024),
                diagnostics.oom_count, invariants_ok);
    return 0;
  }

  if (u32 addr = try_allocate_in_segment(segments_.back())) {
    return addr;
  }

  const auto diagnostics = GetDiagnosticsLocked();
  const u64 free_bytes = diagnostics.capacity - diagnostics.allocated;
  REXLOG_WARN(kernel,
              "rexshim_RtlAllocateHeap: allocation still failed after growth for {} bytes "
              "(segments={}, capacity={}MB, allocated={}MB, free={}MB, oom_count={})",
              size, segments_.size(), diagnostics.capacity / (1024 * 1024),
              diagnostics.allocated / (1024 * 1024), free_bytes / (1024 * 1024),
              diagnostics.oom_count);
  return 0;
}

ReXHeap::HeapSegment* ReXHeap::FindSegmentByGuestLocked(u32 guest_addr) {
  auto it = std::find_if(segments_.begin(), segments_.end(), [&](const HeapSegment& segment) {
    return guest_addr >= segment.guest_base && guest_addr < segment.guest_end;
  });
  return it != segments_.end() ? &(*it) : nullptr;
}

HeapDiagnostics ReXHeap::GetDiagnosticsLocked() const {
  HeapDiagnostics diagnostics{};
  for (const auto& segment : segments_) {
    const auto d = o1heapGetDiagnostics(segment.heap);
    diagnostics.capacity += d.capacity;
    diagnostics.allocated += d.allocated;
    diagnostics.peak_allocated += d.peak_allocated;
    diagnostics.peak_request_size =
        std::max(diagnostics.peak_request_size, static_cast<u64>(d.peak_request_size));
    diagnostics.oom_count += d.oom_count;
  }
  return diagnostics;
}

// ---------------------------------------------------------------------------
// Global instance + RTL hooks
// ---------------------------------------------------------------------------

ReXHeap g_heap;

REX_SHIM_FN(RtlAllocateHeap, u32, u32 hHeap, u32 dwFlags, u32 dwBytes) {
  return g_heap.Alloc(dwBytes, dwFlags & HEAP_ZERO_MEMORY);
}

REX_SHIM_FN(RtlFreeHeap, u32, u32 hHeap, u32 dwFlags, u32 ptr) {
  g_heap.Free(static_cast<u32>(ptr));
  return 1;
}

REX_SHIM_FN(RtlSizeHeap, u32, u32 hHeap, u32 dwFlags, u32 ptr) {
  return g_heap.Size(static_cast<u32>(ptr));
}

REX_SHIM_FN(RtlReAllocateHeap, u32, u32 hHeap, u32 dwFlags, u32 ptr, u32 dwBytes) {
  return g_heap.Realloc(static_cast<u32>(ptr), dwBytes, dwFlags & HEAP_ZERO_MEMORY);
}

bool InitHeap(u32 heap_size_mb, rex::runtime::Memory* memory) {
  return g_heap.Init(heap_size_mb * 1024u * 1024u, memory);
}

}  // namespace rex::kernel::shim
