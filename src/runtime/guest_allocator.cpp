#include <rex/runtime/guest_allocator.h>

#include <o1heap.h>

#include <rex/runtime/error.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

namespace {

[[maybe_unused]] constexpr const char* kPoolNames[] = {"Guest heap", "Guest workspace", "Title"};
constexpr const char* kPoolPlotNames[] = {"Guest heap in use", "Guest workspace in use", "Title"};

}  // namespace

GuestAllocator::GuestAllocator(Key, Memory& memory, u32 base, u32 size, O1HeapInstance* heap,
                               GuestPool pool)
    : memory_(memory), base_(base), size_(size), heap_(heap), pool_(pool) {
  std::lock_guard lock(memory_.allocators_mutex_);
  memory_.allocators_.push_back(this);
}

GuestAllocator::~GuestAllocator() {
  memory_.LookupHeap(base_)->Release(base_);
}

Result<std::unique_ptr<GuestAllocator>> GuestAllocator::Create(Memory& memory, u32 size,
                                                               u32 alignment, bool physical,
                                                               u32 page_size, u32 flags) {
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
  return std::make_unique<GuestAllocator>(Key{}, memory, base, size, instance);
}

u32 GuestAllocator::GetBase() const {
  return base_;
}

u32 GuestAllocator::GetSize() const {
  return size_;
}

bool GuestAllocator::Contains(u32 address) const {
  return address >= base_ && address - base_ < size_;
}

void* GuestAllocator::Allocate(u32 size) {
  std::lock_guard lock(mutex_);
  return o1heapAllocate(heap_, size);
}

void* GuestAllocator::Reallocate(void* block, u32 size) {
  std::lock_guard lock(mutex_);
  return o1heapReallocate(heap_, block, size);
}

void GuestAllocator::Free(void* block) {
  std::lock_guard lock(mutex_);
  o1heapFree(heap_, block);
}

GuestAllocator::Diagnostics GuestAllocator::GetDiagnostics() const {
  std::lock_guard lock(mutex_);
  const O1HeapDiagnostics d = o1heapGetDiagnostics(heap_);
  return {d.capacity, d.allocated, d.peak_allocated, d.peak_request_size, d.oom_count};
}

bool GuestAllocator::InvariantsHold() const {
  std::lock_guard lock(mutex_);
  return o1heapDoInvariantsHold(heap_);
}

}  // namespace rex::runtime
