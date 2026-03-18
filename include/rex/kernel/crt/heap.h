/**
 * @file        kernel/crt/heap.h
 * @brief       ReXHeap -- O(1) native heap for guest memory using o1heap.
 *              Provides rexcrt hooks for RtlAllocateHeap, RtlFreeHeap, etc.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, rexcrt_heap_enable);
REXCVAR_DECLARE(uint32_t, rexcrt_heap_size_mb);

struct O1HeapInstance;

namespace rex::memory {
class Memory;
}

namespace rex::kernel::crt {

/// Mirrors O1HeapDiagnostics without requiring o1heap.h in consumer headers.
struct HeapDiagnostics {
  uint64_t capacity;
  uint64_t allocated;
  uint64_t peak_allocated;
  uint64_t peak_request_size;
  uint64_t oom_count;
};

class ReXHeap {
 public:
  bool Init(uint32_t heap_size_bytes, rex::memory::Memory* memory);

  uint32_t Alloc(uint32_t size, bool zero = false);
  void Free(uint32_t guest_addr);
  uint32_t Size(uint32_t guest_addr);
  uint32_t Realloc(uint32_t guest_addr, uint32_t new_size, bool zero_new = false);

  bool InHeap(uint32_t guest_addr) const;
  HeapDiagnostics GetDiagnostics() const;

 private:
  struct HeapSegment {
    O1HeapInstance* heap = nullptr;
    uint32_t guest_base = 0;
    uint32_t guest_end = 0;
  };

  mutable std::mutex mutex_;
  std::vector<HeapSegment> segments_;
  uint8_t* membase_ = nullptr;
  uint32_t initial_segment_size_ = 0;
  rex::memory::Memory* memory_ = nullptr;

  void* GuestToHost(uint32_t guest_addr) const;
  uint32_t HostToGuest(void* host_ptr) const;
  bool AllocateSegmentLocked(uint32_t segment_size_bytes);
  uint32_t AllocLocked(uint32_t size, bool zero = false);
  HeapSegment* FindSegmentByGuestLocked(uint32_t guest_addr);
  const HeapSegment* FindSegmentByGuestLocked(uint32_t guest_addr) const;
  HeapDiagnostics GetDiagnosticsLocked() const;
};

/// Initialize the global rexcrt heap. Called by Runtime::Setup() when enabled.
bool InitHeap(uint32_t heap_size_mb, rex::memory::Memory* memory);

/// Access the global heap instance (valid after InitHeap).
ReXHeap& GetHeap();

}  // namespace rex::kernel::crt
