#include <rex/kernel/guest_allocator.h>
#include <rex/kernel/guest_memory.h>

#include <algorithm>
#include <cstring>

#include <tlsf.h>

#include <rex/platform/perf.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/heap.h>

namespace rex::kernel {

namespace {

[[maybe_unused]] constexpr const char* kPoolNames[] = {"Guest heap", "Guest workspace", "Title"};
constexpr const char* kPoolPlotNames[] = {"Guest heap in use", "Guest workspace in use", "Title"};

}  // namespace

GuestAllocator::GuestAllocator(Key, GuestAddressSpace& memory, u32 base, u32 size, void* state,
                               GuestPool pool)
    : memory_(memory),
      base_(base),
      size_(size),
      state_(state),
      pool_(pool),
      capacity_(size - tlsf_size() - tlsf_pool_overhead()) {
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
  const size_t overhead = tlsf_size() + tlsf_pool_overhead();
  tlsf_t state = nullptr;
  if (base % tlsf_align_size() == 0 && size >= overhead + tlsf_block_size_min() &&
      size - overhead <= tlsf_block_size_max()) {
    state = tlsf_create_with_pool(memory.TranslateVirtual(base), size);
  }
  if (!state) {
    heap->Release(base);
    return Err<std::unique_ptr<GuestAllocator>>(StatusError(X_STATUS_NO_MEMORY));
  }
  return std::make_unique<GuestAllocator>(Key{}, memory, base, size, state, pool);
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
  peak_request_size_ = std::max<u64>(peak_request_size_, size);
  void* block = tlsf_memalign(state_, kBlockAlignment, size);
  if (!block) {
    ++oom_count_;
    return nullptr;
  }
  allocated_ += tlsf_block_size(block);
  peak_allocated_ = std::max(peak_allocated_, allocated_);
  REX_PROFILE_ALLOC_NAMED(block, size, kPoolNames[static_cast<size_t>(pool_)]);
  return block;
}

void* GuestAllocator::Reallocate(void* block, u32 size) {
  if (!block) {
    return Allocate(size);
  }
  std::lock_guard lock(mutex_);
  peak_request_size_ = std::max<u64>(peak_request_size_, size);
  const size_t old_size = tlsf_block_size(block);
  if (size <= old_size) {
    return block;
  }
  void* resized = tlsf_memalign(state_, kBlockAlignment, size);
  if (!resized) {
    ++oom_count_;
    return nullptr;
  }
  std::memcpy(resized, block, old_size);
  REX_PROFILE_FREE_NAMED(block, kPoolNames[static_cast<size_t>(pool_)]);
  allocated_ += tlsf_block_size(resized) - old_size;
  tlsf_free(state_, block);
  peak_allocated_ = std::max(peak_allocated_, allocated_);
  REX_PROFILE_ALLOC_NAMED(resized, size, kPoolNames[static_cast<size_t>(pool_)]);
  return resized;
}

void GuestAllocator::Free(void* block) {
  if (!block) {
    return;
  }
  std::lock_guard lock(mutex_);
  REX_PROFILE_FREE_NAMED(block, kPoolNames[static_cast<size_t>(pool_)]);
  allocated_ -= tlsf_block_size(block);
  tlsf_free(state_, block);
}

GuestAllocator::AllocatorStats GuestAllocator::GetStats() const {
  std::lock_guard lock(mutex_);
  return {capacity_, allocated_, peak_allocated_, peak_request_size_, oom_count_};
}

bool GuestAllocator::IsConsistent() const {
  std::lock_guard lock(mutex_);
  return tlsf_check(state_) == 0;
}

}  // namespace rex::kernel
