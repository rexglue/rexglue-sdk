#include <rex/kernel/guest_allocator.h>
#include <rex/kernel/guest_memory.h>

#include <o1heap.h>

#include <rex/platform/perf.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/heap.h>

namespace rex::kernel {

namespace {

[[maybe_unused]] constexpr const char* kPoolNames[] = {"Guest heap", "Guest workspace", "Title"};
constexpr const char* kPoolPlotNames[] = {"Guest heap in use", "Guest workspace in use", "Title"};

}  // namespace

GuestAllocator::GuestAllocator(Key, GuestAddressSpace& memory, u32 base, u32 size,
                               O1HeapInstance* heap, GuestPool pool)
    : memory_(memory), base_(base), size_(size), heap_(heap), pool_(pool) {
  std::lock_guard lock(memory_.allocators_mutex_);
  memory_.allocators_.push_back(this);
}

GuestAllocator::~GuestAllocator() {
  {
    std::lock_guard lock(memory_.allocators_mutex_);
    std::erase(memory_.allocators_, this);
  }
  memory_.LookupHeap(base_)->Release(base_);
}

const char* GuestAllocator::GetPoolPlotName(GuestPool pool) {
  return kPoolPlotNames[static_cast<size_t>(pool)];
}

Result<std::unique_ptr<GuestAllocator>> GuestAllocator::Create(rex::kernel::GuestMemory& memory,
                                                               u32 size, u32 alignment,
                                                               bool physical, u32 page_size,
                                                               u32 flags, GuestPool pool) {
  return Create(memory.GetAddressSpace(), size, alignment, physical, page_size, flags, pool);
}

Result<std::unique_ptr<GuestAllocator>> GuestAllocator::Create(GuestAddressSpace& memory, u32 size,
                                                               u32 alignment, bool physical,
                                                               u32 page_size, u32 flags,
                                                               GuestPool pool) {
  BaseHeap* heap = memory.LookupHeapByType(physical, page_size);
  u32 base = 0;
  if (!heap ||
      !heap->Alloc(size, alignment, kMemoryAllocationReserve | kMemoryAllocationCommit | flags,
                   kMemoryProtectRead | kMemoryProtectWrite, true, &base)) {
    return Err<std::unique_ptr<GuestAllocator>>(StatusError(X_STATUS_NO_MEMORY));
  }
  O1HeapInstance* instance = o1heapInit(memory.TranslateVirtual(base), size);
  if (!instance) {
    heap->Release(base);
    return Err<std::unique_ptr<GuestAllocator>>(StatusError(X_STATUS_NO_MEMORY));
  }
  return std::make_unique<GuestAllocator>(Key{}, memory, base, size, instance, pool);
}

u32 GuestAllocator::GetBase() const {
  return base_;
}

u32 GuestAllocator::GetSize() const {
  return size_;
}

GuestPool GuestAllocator::GetPool() const {
  return pool_;
}

bool GuestAllocator::Contains(u32 address) const {
  return address >= base_ && address - base_ < size_;
}

void* GuestAllocator::Allocate(u32 size) {
  std::lock_guard lock(mutex_);
  void* block = o1heapAllocate(heap_, size);
  if (block) {
    REX_PROFILE_ALLOC_NAMED(block, size, kPoolNames[static_cast<size_t>(pool_)]);
  }
  return block;
}

void* GuestAllocator::Reallocate(void* block, u32 size) {
  std::lock_guard lock(mutex_);
  void* resized = o1heapReallocate(heap_, block, size);
  if (resized) {
    if (block) {
      REX_PROFILE_FREE_NAMED(block, kPoolNames[static_cast<size_t>(pool_)]);
    }
    REX_PROFILE_ALLOC_NAMED(resized, size, kPoolNames[static_cast<size_t>(pool_)]);
  }
  return resized;
}

void GuestAllocator::Free(void* block) {
  std::lock_guard lock(mutex_);
  if (block) {
    REX_PROFILE_FREE_NAMED(block, kPoolNames[static_cast<size_t>(pool_)]);
  }
  o1heapFree(heap_, block);
}

GuestAllocator::AllocatorStats GuestAllocator::GetStats() const {
  std::lock_guard lock(mutex_);
  const O1HeapDiagnostics d = o1heapGetDiagnostics(heap_);
  return {d.capacity, d.allocated, d.peak_allocated, d.peak_request_size, d.oom_count};
}

bool GuestAllocator::IsConsistent() const {
  std::lock_guard lock(mutex_);
  return o1heapDoInvariantsHold(heap_);
}

}  // namespace rex::kernel
