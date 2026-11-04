/**
 * @file        kernel/workspace_heap.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/kernel/heap.h>
#include <rex/kernel/guest_allocator.h>

#include <algorithm>
#include <cstring>

#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/kernel/types/status.h>

namespace rex::kernel {

namespace {

constexpr u32 kDefaultSize = 384 * 1024;
constexpr u32 kMinimumSize = 256 * 1024;
constexpr u32 kSizeStep = 16 * 1024;

}  // namespace

WorkspaceHeap::WorkspaceHeap(Key, Memory& memory, std::unique_ptr<GuestAllocator> allocator)
    : memory_(memory), allocator_(std::move(allocator)) {}

Result<std::unique_ptr<WorkspaceHeap>> WorkspaceHeap::Create(Memory& memory, u32 size) {
  auto allocator = GuestAllocator::Create(memory, size, kPageSize4KiB, false, kPageSize4KiB,
                                          kMemoryAllocationTitle, GuestPool::kWorkspace);
  if (!allocator) {
    return Err<std::unique_ptr<WorkspaceHeap>>(allocator.error());
  }
  return std::make_unique<WorkspaceHeap>(Key{}, memory, std::move(*allocator));
}

u32 WorkspaceHeap::ResolveSize(u32 header_size) {
  if (!header_size) {
    return kDefaultSize;
  }
  return rex::RoundUpNonZero(std::max(header_size, kMinimumSize), kSizeStep);
}

u32 WorkspaceHeap::GetSize() const {
  return allocator_->GetSize();
}

Result<u32> WorkspaceHeap::Allocate(u32 size) {
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
    REXSYS_WARN("Workspace free of {:#010x} is outside {:#010x}+{:#x}, ignored", address,
                allocator_->GetBase(), allocator_->GetSize());
    return;
  }
  allocator_->Free(memory_.TranslateVirtual(address));
}

}  // namespace rex::kernel
