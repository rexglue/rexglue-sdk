#include <rex/runtime/workspace_heap.h>

#include <algorithm>
#include <cstring>

#include <o1heap.h>

#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/runtime/error.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

namespace {

constexpr u32 kDefaultSize = 384 * 1024;
constexpr u32 kMinimumSize = 256 * 1024;
constexpr u32 kSizeStep = 16 * 1024;
constexpr u32 kPageSize = 4096;

}  // namespace

WorkspaceHeap::WorkspaceHeap(Key, Memory& memory, u32 base, u32 size, O1HeapInstance* heap)
    : memory_(memory), base_(base), size_(size), heap_(heap) {}

WorkspaceHeap::~WorkspaceHeap() {
  memory_.LookupHeap(base_)->Release(base_);
}

Result<std::unique_ptr<WorkspaceHeap>> WorkspaceHeap::Create(Memory& memory, u32 size) {
  BaseHeap* heap = memory.LookupHeapByType(false, kPageSize);
  u32 base = 0;
  if (!heap ||
      !heap->Alloc(size, kPageSize,
                   kMemoryAllocationReserve | kMemoryAllocationCommit | kMemoryAllocationTitle,
                   kMemoryProtectRead | kMemoryProtectWrite, true, &base)) {
    return Err<std::unique_ptr<WorkspaceHeap>>(StatusError(X_STATUS_NO_MEMORY));
  }
  O1HeapInstance* instance = o1heapInit(memory.TranslateVirtual(base), size);
  if (!instance) {
    heap->Release(base);
    return Err<std::unique_ptr<WorkspaceHeap>>(StatusError(X_STATUS_NO_MEMORY));
  }
  return std::make_unique<WorkspaceHeap>(Key{}, memory, base, size, instance);
}

u32 WorkspaceHeap::ResolveSize(u32 header_size) {
  if (!header_size) {
    return kDefaultSize;
  }
  return rex::round_up(std::max(header_size, kMinimumSize), kSizeStep);
}

u32 WorkspaceHeap::GetSize() const {
  return size_;
}

Result<u32> WorkspaceHeap::Alloc(u32 size) {
  size = std::max(size, 1u);
  void* block = nullptr;
  {
    std::lock_guard lock(mutex_);
    block = o1heapAllocate(heap_, size);
  }
  if (!block) {
    return Err<u32>(StatusError(X_STATUS_NO_MEMORY));
  }
  std::memset(block, 0, size);
  return memory_.HostToGuestVirtual(block);
}

void WorkspaceHeap::Free(u32 address) {
  if (!address) {
    return;
  }
  if (address < base_ || address >= base_ + size_) {
    REXLOG_WARN(system, "Workspace free of {:#010x} is outside {:#010x}+{:#x}, ignored", address,
                base_, size_);
    return;
  }
  std::lock_guard lock(mutex_);
  o1heapFree(heap_, memory_.TranslateVirtual(address));
}

}  // namespace rex::runtime
