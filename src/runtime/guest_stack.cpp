#include "runtime/guest_stack.h"

#include <algorithm>

#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/runtime/error.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

u32 ResolveGuestStackSize(u32 requested, u32 guest_default, u32 minimum) {
  const u32 fallback = guest_default ? guest_default : kDefaultGuestStackSize;
  const u32 size = requested ? requested : fallback;
  return std::max(rex::RoundUpNonZero(size, 4096u), minimum);
}

u32 ResolveTLSSlotCount(u32 xex_slot_count) {
  constexpr u32 kMinimumSlots = 32;
  constexpr u32 kMaximumSlots = 256;
  if (xex_slot_count > kMaximumSlots) {
    REXLOG_ERROR(system, "TLS slot count {} exceeds the {} slot process bitmap", xex_slot_count,
                 kMaximumSlots);
  }
  return std::clamp(xex_slot_count, kMinimumSlots, kMaximumSlots);
}

Result<GuestStack> AllocateGuestStack(Memory& memory, u32 size, u32 tls_size, bool title_memory) {
  BaseHeap* heap = memory.LookupHeap(KernelThread::kStackAddressRangeBegin);
  size = rex::RoundUpNonZero(size, 4096u);
  tls_size = rex::RoundUp(tls_size, 16u);
  const u32 committed_size = rex::RoundUpNonZero(size + tls_size, kGuestStackPageSize);
  const u32 alloc_size = committed_size + kGuestStackPageSize;
  u32 alloc_base = 0;
  if (!heap->AllocRange(KernelThread::kStackAddressRangeBegin, KernelThread::kStackAddressRangeEnd,
                        alloc_size, kGuestStackPageSize, kMemoryAllocationReserve,
                        kMemoryProtectNoAccess, false, &alloc_base)) {
    return Err<GuestStack>(StatusError(X_STATUS_NO_MEMORY));
  }
  const u32 commit =
      kMemoryAllocationCommit | (title_memory ? static_cast<u32>(kMemoryAllocationTitle) : 0u);
  const u32 read_write = kMemoryProtectRead | kMemoryProtectWrite;
  if (!heap->AllocFixed(alloc_base + kGuestStackPageSize, committed_size, kGuestStackPageSize,
                        commit, read_write) ||
      !heap->AllocFixed(alloc_base, kGuestStackPageSize, kGuestStackPageSize,
                        kMemoryAllocationCommit, read_write) ||
      !heap->Protect(alloc_base, kGuestStackPageSize, kMemoryProtectNoAccess)) {
    heap->Release(alloc_base);
    return Err<GuestStack>(StatusError(X_STATUS_NO_MEMORY));
  }
  const u32 top = alloc_base + alloc_size;
  memory.Fill(alloc_base + kGuestStackPageSize, committed_size - tls_size, 0xBE);
  return GuestStack{alloc_base, top - tls_size - size, top - tls_size};
}

void FreeGuestStack(Memory& memory, u32 alloc_base) {
  if (alloc_base) {
    memory.LookupHeap(alloc_base)->Release(alloc_base);
  }
}

}  // namespace rex::runtime
