/**
 * @file        kernel/crt/heap.cpp
 *
 * @brief       ReXHeap implementation using o1heap with size header pattern.
 *              Hooks RtlAllocateHeap, RtlFreeHeap, RtlSizeHeap, RtlReAllocateHeap.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <rex/kernel/crt/heap.h>

#include <algorithm>
#include <cstring>
#include <o1heap.h>

#include <rex/cvar.h>
#include <rex/math.h>
#include <rex/platform.h>
#include <rex/ppc/function.h>
#include <rex/system/xmemory.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(rexcrt_heap_enable, false, "crt",
                    "Enable o1heap-backed CRT heap for RtlAllocateHeap/Free/Size/ReAlloc")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_UINT32(rexcrt_heap_size_mb, 256, "crt", "Heap size in megabytes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(1, 2048);

using namespace rex::ppc;

// ---------------------------------------------------------------------------
// Size header: prepended to every allocation so we can answer RtlSizeHeap
// without o1heap exposing per-allocation usable size.
// ---------------------------------------------------------------------------
namespace {

struct SizeHeader {
  uint64_t requested_size;
  uint64_t reserved;  // padding to O1HEAP_ALIGNMENT
};
static_assert(sizeof(SizeHeader) == O1HEAP_ALIGNMENT,
              "SizeHeader must be exactly one O1HEAP_ALIGNMENT unit");

constexpr uint32_t kHeaderSize = static_cast<uint32_t>(O1HEAP_ALIGNMENT);
constexpr uint32_t kMinSegmentSize = 4u * 1024u * 1024u;
constexpr uint32_t kDefaultGrowthSegmentSize = 64u * 1024u * 1024u;

#ifndef HEAP_ZERO_MEMORY
constexpr uint32_t HEAP_ZERO_MEMORY = 0x00000008;
#endif

}  // namespace

// ---------------------------------------------------------------------------
// ReXHeap implementation
// ---------------------------------------------------------------------------

namespace rex::kernel::crt {

void* ReXHeap::GuestToHost(uint32_t guest_addr) const {
  return membase_ + guest_addr;
}

uint32_t ReXHeap::HostToGuest(void* host_ptr) const {
  return static_cast<uint32_t>(static_cast<uint8_t*>(host_ptr) - membase_);
}

bool ReXHeap::InHeap(uint32_t guest_addr) const {
  std::lock_guard lock(mutex_);
  return FindSegmentByGuestLocked(guest_addr) != nullptr;
}

bool ReXHeap::Init(uint32_t heap_size_bytes, rex::memory::Memory* memory) {
  memory_ = memory;
  auto* mem = memory_;
  if (!mem) {
    REXKRNL_ERROR("rexcrt_heap: memory is null");
    return false;
  }

  membase_ = mem->virtual_membase();
  if (!membase_) {
    REXKRNL_ERROR("rexcrt_heap: virtual_membase is null");
    return false;
  }

  initial_segment_size_ =
      rex::align<uint32_t>(std::max(heap_size_bytes, kMinSegmentSize), O1HEAP_ALIGNMENT);

  std::lock_guard lock(mutex_);
  segments_.clear();
  if (!AllocateSegmentLocked(initial_segment_size_)) {
    return false;
  }

  const auto diagnostics = GetDiagnosticsLocked();
  REXKRNL_INFO("rexcrt_heap: initialized with {} segment(s), capacity={}MB", segments_.size(),
               diagnostics.capacity / (1024 * 1024));
  return true;
}

uint32_t ReXHeap::Alloc(uint32_t size, bool zero) {
  std::lock_guard lock(mutex_);
  return AllocLocked(size, zero);
}

void ReXHeap::Free(uint32_t guest_addr) {
  if (!guest_addr)
    return;
  std::lock_guard lock(mutex_);
  auto* segment = FindSegmentByGuestLocked(guest_addr);
  if (!segment || guest_addr < segment->guest_base + kHeaderSize) {
    REXKRNL_WARN("rexcrt_RtlFreeHeap: skipping OOB ptr 0x{:08X}", guest_addr);
    return;
  }

  void* real_host = GuestToHost(guest_addr - kHeaderSize);
  o1heapFree(segment->heap, real_host);
}

uint32_t ReXHeap::Size(uint32_t guest_addr) {
  if (!guest_addr)
    return ~0u;

  std::lock_guard lock(mutex_);
  auto* segment = FindSegmentByGuestLocked(guest_addr);
  if (!segment || guest_addr < segment->guest_base + kHeaderSize) {
    return ~0u;
  }

  auto* hdr = static_cast<SizeHeader*>(GuestToHost(guest_addr - kHeaderSize));
  return static_cast<uint32_t>(hdr->requested_size);
}

uint32_t ReXHeap::Realloc(uint32_t guest_addr, uint32_t new_size, bool zero_new) {
  std::lock_guard lock(mutex_);
  if (!guest_addr) {
    return AllocLocked(new_size, zero_new);
  }
  if (new_size == 0)
    new_size = 1;

  auto* segment = FindSegmentByGuestLocked(guest_addr);
  if (!segment || guest_addr < segment->guest_base + kHeaderSize) {
    // Pre-hook allocation outside our heap -- treat as fresh alloc.
    REXKRNL_WARN("rexcrt_RtlReAllocateHeap: OOB ptr 0x{:08X}, treating as new alloc({})",
                 guest_addr, new_size);
    return AllocLocked(new_size, zero_new);
  }

  void* real_host = GuestToHost(guest_addr - kHeaderSize);

  auto* old_hdr = static_cast<SizeHeader*>(real_host);
  uint32_t old_size = static_cast<uint32_t>(old_hdr->requested_size);
  void* new_ptr = o1heapReallocate(segment->heap, real_host, new_size + kHeaderSize);
  if (!new_ptr) {
    // Cross-segment fallback: allocate a fresh block, copy, then free old.
    uint32_t new_guest = AllocLocked(new_size, false);
    if (!new_guest) {
      REXKRNL_WARN("rexcrt_RtlReAllocateHeap: o1heapReallocate({}) failed", new_size);
      return 0;
    }

    void* new_user_ptr = GuestToHost(new_guest);
    void* old_user_ptr = GuestToHost(guest_addr);
    uint32_t copy_size = std::min(old_size, new_size);
    std::memcpy(new_user_ptr, old_user_ptr, copy_size);
    if (zero_new && new_size > old_size) {
      std::memset(static_cast<uint8_t*>(new_user_ptr) + old_size, 0, new_size - old_size);
    }

    o1heapFree(segment->heap, real_host);
    return new_guest;
  }

  auto* new_hdr = static_cast<SizeHeader*>(new_ptr);
  new_hdr->requested_size = new_size;

  void* user_ptr = static_cast<uint8_t*>(new_ptr) + kHeaderSize;
  if (zero_new && new_size > old_size) {
    std::memset(static_cast<uint8_t*>(user_ptr) + old_size, 0, new_size - old_size);
  }

  return HostToGuest(user_ptr);
}

HeapDiagnostics ReXHeap::GetDiagnostics() const {
  std::lock_guard lock(mutex_);
  return GetDiagnosticsLocked();
}

bool ReXHeap::AllocateSegmentLocked(uint32_t segment_size_bytes) {
  auto* mem = memory_;
  if (!mem) {
    REXKRNL_ERROR("rexcrt_heap: kernel memory is null during segment allocation");
    return false;
  }

  segment_size_bytes =
      rex::align<uint32_t>(std::max(segment_size_bytes, kMinSegmentSize), O1HEAP_ALIGNMENT);

  uint32_t guest_base = 0;
  bool used_system_heap = false;

  auto alloc_regular_virtual_heap = [&]() -> bool {
    auto* heap = mem->LookupHeapByType(false, 4096);
    if (!heap ||
        !heap->Alloc(segment_size_bytes, O1HEAP_ALIGNMENT,
                     rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                     rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, true,
                     &guest_base)) {
      return false;
    }
    return true;
  };

#if REX_PLATFORM_LINUX
  if (!alloc_regular_virtual_heap()) {
    REXKRNL_ERROR("rexcrt_heap: regular virtual heap allocation of {} bytes failed",
                  segment_size_bytes);
    return false;
  }
#else
  guest_base = mem->SystemHeapAlloc(segment_size_bytes);
  used_system_heap = guest_base != 0;
  if (!guest_base && !alloc_regular_virtual_heap()) {
    REXKRNL_ERROR("rexcrt_heap: failed to allocate {} bytes from both system and regular heaps",
                  segment_size_bytes);
    return false;
  }
#endif

  uint8_t* host_base = mem->TranslateVirtual<uint8_t*>(guest_base);
  if (!host_base) {
    REXKRNL_ERROR("rexcrt_heap: TranslateVirtual failed for guest base 0x{:08X}", guest_base);
    if (used_system_heap) {
      mem->SystemHeapFree(guest_base);
    }
    return false;
  }

  O1HeapInstance* heap = o1heapInit(host_base, segment_size_bytes);
  if (!heap) {
    REXKRNL_ERROR("rexcrt_heap: o1heapInit failed for segment 0x{:08X} size {}", guest_base,
                  segment_size_bytes);
    if (used_system_heap) {
      mem->SystemHeapFree(guest_base);
    }
    return false;
  }

  segments_.push_back(
      HeapSegment{heap, guest_base, static_cast<uint32_t>(guest_base + segment_size_bytes)});
  REXKRNL_INFO("rexcrt_heap: added segment guest=0x{:08X}-0x{:08X} size={}MB", guest_base,
               guest_base + segment_size_bytes, segment_size_bytes / (1024 * 1024));
  return true;
}

uint32_t ReXHeap::AllocLocked(uint32_t size, bool zero) {
  if (size == 0) {
    size = 1;
  }

  const uint32_t alloc_size = size + kHeaderSize;
  auto try_allocate_in_segment = [&](HeapSegment& segment) -> uint32_t {
    void* ptr = o1heapAllocate(segment.heap, alloc_size);
    if (!ptr) {
      return 0;
    }

    auto* hdr = static_cast<SizeHeader*>(ptr);
    hdr->requested_size = size;
    hdr->reserved = 0;

    void* user_ptr = static_cast<uint8_t*>(ptr) + kHeaderSize;
    if (zero) {
      std::memset(user_ptr, 0, size);
    }
    return HostToGuest(user_ptr);
  };

  for (auto& segment : segments_) {
    if (uint32_t guest = try_allocate_in_segment(segment)) {
      return guest;
    }
  }

  const uint32_t min_required_segment =
      rex::align<uint32_t>(alloc_size + kHeaderSize, O1HEAP_ALIGNMENT);
  const uint32_t growth_segment_size =
      std::max({initial_segment_size_, kDefaultGrowthSegmentSize, min_required_segment});
  if (!AllocateSegmentLocked(growth_segment_size)) {
    const auto diagnostics = GetDiagnosticsLocked();
    bool invariants_ok = true;
    for (const auto& segment : segments_) {
      invariants_ok = invariants_ok && o1heapDoInvariantsHold(segment.heap);
    }
    const uint64_t free_bytes = diagnostics.capacity - diagnostics.allocated;
    REXKRNL_WARN(
        "rexcrt_RtlAllocateHeap: o1heapAllocate({}) failed (segments={}, capacity={}MB, "
        "allocated={}MB, free={}MB, oom_count={}, invariants_ok={})",
        size, segments_.size(), diagnostics.capacity / (1024 * 1024),
        diagnostics.allocated / (1024 * 1024), free_bytes / (1024 * 1024), diagnostics.oom_count,
        invariants_ok);
    return 0;
  }

  if (uint32_t guest = try_allocate_in_segment(segments_.back())) {
    return guest;
  }

  const auto diagnostics = GetDiagnosticsLocked();
  const uint64_t free_bytes = diagnostics.capacity - diagnostics.allocated;
  REXKRNL_WARN(
      "rexcrt_RtlAllocateHeap: allocation still failed after growth for {} bytes "
      "(segments={}, capacity={}MB, allocated={}MB, free={}MB, oom_count={})",
      size, segments_.size(), diagnostics.capacity / (1024 * 1024),
      diagnostics.allocated / (1024 * 1024), free_bytes / (1024 * 1024), diagnostics.oom_count);
  return 0;
}

ReXHeap::HeapSegment* ReXHeap::FindSegmentByGuestLocked(uint32_t guest_addr) {
  auto it = std::find_if(segments_.begin(), segments_.end(), [&](const HeapSegment& segment) {
    return guest_addr >= segment.guest_base && guest_addr < segment.guest_end;
  });
  return it != segments_.end() ? &(*it) : nullptr;
}

const ReXHeap::HeapSegment* ReXHeap::FindSegmentByGuestLocked(uint32_t guest_addr) const {
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
    diagnostics.peak_request_size = std::max(diagnostics.peak_request_size, d.peak_request_size);
    diagnostics.oom_count += d.oom_count;
  }
  return diagnostics;
}

// ---------------------------------------------------------------------------
// Global instance + RTL hooks
// ---------------------------------------------------------------------------

ReXHeap g_heap;

ppc_u32_result_t RtlAllocateHeap_entry(ppc_u32_t hHeap, ppc_u32_t dwFlags, ppc_u32_t dwBytes) {
  return g_heap.Alloc(dwBytes, dwFlags & HEAP_ZERO_MEMORY);
}

ppc_u32_result_t RtlFreeHeap_entry(ppc_u32_t hHeap, ppc_u32_t dwFlags, ppc_u32_t ptr) {
  g_heap.Free(static_cast<uint32_t>(ptr));
  return 1;
}

ppc_u32_result_t RtlSizeHeap_entry(ppc_u32_t hHeap, ppc_u32_t dwFlags, ppc_u32_t ptr) {
  return g_heap.Size(static_cast<uint32_t>(ptr));
}

ppc_u32_result_t RtlReAllocateHeap_entry(ppc_u32_t hHeap, ppc_u32_t dwFlags, ppc_u32_t ptr,
                                         ppc_u32_t dwBytes) {
  return g_heap.Realloc(static_cast<uint32_t>(ptr), dwBytes, dwFlags & HEAP_ZERO_MEMORY);
}

bool InitHeap(uint32_t heap_size_mb, rex::memory::Memory* memory) {
  return g_heap.Init(heap_size_mb * 1024u * 1024u, memory);
}

ReXHeap& GetHeap() {
  return g_heap;
}

}  // namespace rex::kernel::crt

REXCRT_EXPORT(rexcrt_RtlAllocateHeap, rex::kernel::crt::RtlAllocateHeap_entry)
REXCRT_EXPORT(rexcrt_RtlFreeHeap, rex::kernel::crt::RtlFreeHeap_entry)
REXCRT_EXPORT(rexcrt_RtlSizeHeap, rex::kernel::crt::RtlSizeHeap_entry)
REXCRT_EXPORT(rexcrt_RtlReAllocateHeap, rex::kernel::crt::RtlReAllocateHeap_entry)
