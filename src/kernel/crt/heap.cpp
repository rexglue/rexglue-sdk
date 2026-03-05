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

#include <cstring>
#include <o1heap.h>

#include <rex/cvar.h>
#include <rex/ppc/function.h>
#include <rex/system/kernel_state.h>
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
  return guest_addr >= heap_base_ && guest_addr < heap_end_;
}

bool ReXHeap::Init(uint32_t heap_size_bytes) {
  auto* mem = rex::system::kernel_state()->memory();
  if (!mem) {
    REXKRNL_ERROR("rexcrt_heap: kernel_memory() is null");
    return false;
  }

  membase_ = mem->virtual_membase();

  uint32_t guest_base = mem->SystemHeapAlloc(heap_size_bytes);
  if (!guest_base) {
    REXKRNL_ERROR("rexcrt_heap: SystemHeapAlloc({}) failed", heap_size_bytes);
    return false;
  }

  uint8_t* host_base = mem->TranslateVirtual<uint8_t*>(guest_base);
  heap_base_ = guest_base;
  heap_end_ = guest_base + heap_size_bytes;

  heap_ = o1heapInit(host_base, heap_size_bytes);
  if (!heap_) {
    REXKRNL_ERROR("rexcrt_heap: o1heapInit failed");
    mem->SystemHeapFree(guest_base);
    return false;
  }

  REXKRNL_INFO("rexcrt_heap: guest=0x{:08X}-0x{:08X} host={} size={}MB", heap_base_, heap_end_,
               (void*)host_base, heap_size_bytes / (1024 * 1024));
  return true;
}

uint32_t ReXHeap::Alloc(uint32_t size, bool zero) {
  if (size == 0)
    size = 1;

  std::lock_guard lock(mutex_);
  void* ptr = o1heapAllocate(heap_, size + kHeaderSize);
  if (!ptr) {
    REXKRNL_WARN("rexcrt_RtlAllocateHeap: o1heapAllocate({}) failed", size);
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
}

void ReXHeap::Free(uint32_t guest_addr) {
  if (!guest_addr)
    return;
  if (!InHeap(guest_addr)) {
    REXKRNL_WARN("rexcrt_RtlFreeHeap: skipping OOB ptr 0x{:08X}", guest_addr);
    return;
  }

  void* real_host = GuestToHost(guest_addr - kHeaderSize);
  std::lock_guard lock(mutex_);
  o1heapFree(heap_, real_host);
}

uint32_t ReXHeap::Size(uint32_t guest_addr) {
  if (!guest_addr || !InHeap(guest_addr))
    return ~0u;

  auto* hdr = static_cast<SizeHeader*>(GuestToHost(guest_addr - kHeaderSize));
  return static_cast<uint32_t>(hdr->requested_size);
}

uint32_t ReXHeap::Realloc(uint32_t guest_addr, uint32_t new_size, bool zero_new) {
  if (!guest_addr)
    return Alloc(new_size, zero_new);
  if (new_size == 0)
    new_size = 1;

  // Pre-hook allocation outside our heap -- treat as fresh alloc
  if (!InHeap(guest_addr)) {
    REXKRNL_WARN("rexcrt_RtlReAllocateHeap: OOB ptr 0x{:08X}, treating as new alloc({})",
                 guest_addr, new_size);
    return Alloc(new_size, zero_new);
  }

  void* real_host = GuestToHost(guest_addr - kHeaderSize);

  std::lock_guard lock(mutex_);
  auto* old_hdr = static_cast<SizeHeader*>(real_host);
  uint32_t old_size = static_cast<uint32_t>(old_hdr->requested_size);
  void* new_ptr = o1heapReallocate(heap_, real_host, new_size + kHeaderSize);
  if (!new_ptr) {
    REXKRNL_WARN("rexcrt_RtlReAllocateHeap: o1heapReallocate({}) failed", new_size);
    return 0;
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
  std::lock_guard lock(const_cast<std::mutex&>(mutex_));
  auto d = o1heapGetDiagnostics(heap_);
  return {d.capacity, d.allocated, d.peak_allocated, d.peak_request_size, d.oom_count};
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

bool InitHeap(uint32_t heap_size_mb) {
  return g_heap.Init(heap_size_mb * 1024u * 1024u);
}

ReXHeap& GetHeap() {
  return g_heap;
}

}  // namespace rex::kernel::crt

REXCRT_EXPORT(rexcrt_RtlAllocateHeap, rex::kernel::crt::RtlAllocateHeap_entry)
REXCRT_EXPORT(rexcrt_RtlFreeHeap, rex::kernel::crt::RtlFreeHeap_entry)
REXCRT_EXPORT(rexcrt_RtlSizeHeap, rex::kernel::crt::RtlSizeHeap_entry)
REXCRT_EXPORT(rexcrt_RtlReAllocateHeap, rex::kernel::crt::RtlReAllocateHeap_entry)
