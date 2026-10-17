#include <rex/runtime/workspace_heap.h>

#include <algorithm>
#include <cstring>

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

}  // namespace

WorkspaceHeap::WorkspaceHeap(Key, Memory& memory, std::unique_ptr<GuestAllocator> allocator)
    : memory_(memory), allocator_(std::move(allocator)) {}

Result<std::unique_ptr<WorkspaceHeap>> WorkspaceHeap::Create(Memory& memory, u32 size) {
  auto allocator = GuestAllocator::Create(memory, size, kPageSize4KiB, false, kPageSize4KiB,
                                          kMemoryAllocationTitle);
  if (!allocator) {
    return Err<std::unique_ptr<WorkspaceHeap>>(allocator.error());
  }
  return std::make_unique<WorkspaceHeap>(Key{}, memory, std::move(*allocator));
}

u32 WorkspaceHeap::ResolveSize(u32 header_size) {
  if (!header_size) {
    return kDefaultSize;
  }
  return rex::round_up_nonzero(std::max(header_size, kMinimumSize), kSizeStep);
}

u32 WorkspaceHeap::GetSize() const {
  return allocator_->GetSize();
}

Result<u32> WorkspaceHeap::Alloc(u32 size) {
  size = std::max(size, 1u);
  void* block = allocator_->Allocate(size);
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
  if (!allocator_->Contains(address)) {
    REXLOG_WARN(system, "Workspace free of {:#010x} is outside {:#010x}+{:#x}, ignored", address,
                allocator_->GetBase(), allocator_->GetSize());
    return;
  }
  allocator_->Free(memory_.TranslateVirtual(address));
}

}  // namespace rex::runtime
