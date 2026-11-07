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

#include <rex/logging/log_category.h>
#include <rex/math.h>
#include <rex/platform/perf.h>
#include <rex/kernel/types/status.h>
#include "kernel/best_fit_range_allocator.h"

namespace rex::kernel {

namespace {

constexpr u32 kDefaultSize = 384 * 1024;
constexpr u32 kMinimumSize = 256 * 1024;
constexpr u32 kSizeStep = 16 * 1024;
[[maybe_unused]] constexpr const char* kPoolName = "Guest workspace";

}  // namespace

WorkspaceHeap::WorkspaceHeap(Key, GuestAddressSpace& memory, u32 base, u32 size)
    : memory_(memory),
      base_(base),
      size_(size),
      ranges_(std::make_unique<BestFitRangeAllocator>(size)) {
  std::lock_guard lock(memory_.allocators_mutex_);
  memory_.workspaces_.push_back(this);
}

WorkspaceHeap::~WorkspaceHeap() {
  {
    std::lock_guard lock(memory_.allocators_mutex_);
    std::erase(memory_.workspaces_, this);
  }
  memory_.LookupHeap(base_)->Release(base_);
}

Result<std::unique_ptr<WorkspaceHeap>> WorkspaceHeap::Create(GuestAddressSpace& memory, u32 size) {
  BaseHeap* heap = memory.LookupHeapByType(false, kPageSize4KiB);
  u32 base = 0;
  if (!heap ||
      !heap->Alloc(size, kPageSize4KiB,
                   kMemoryAllocationReserve | kMemoryAllocationCommit | kMemoryAllocationTitle,
                   kMemoryProtectRead | kMemoryProtectWrite, true, &base)) {
    return Err<std::unique_ptr<WorkspaceHeap>>(StatusError(X_STATUS_NO_MEMORY));
  }
  return std::make_unique<WorkspaceHeap>(Key{}, memory, base, size);
}

u32 WorkspaceHeap::ResolveSize(u32 header_size) {
  if (!header_size) {
    return kDefaultSize;
  }
  return rex::RoundUpNonZero(std::max(header_size, kMinimumSize), kSizeStep);
}

u32 WorkspaceHeap::GetSize() const {
  return size_;
}

u32 WorkspaceHeap::GetAllocated() const {
  std::lock_guard lock(mutex_);
  return ranges_->GetAllocated();
}

Result<u32> WorkspaceHeap::Allocate(u32 size, bool zero) {
  size = std::max(size, 1u);
  std::lock_guard lock(mutex_);
  const std::optional<u32> offset = ranges_->Allocate(size);
  if (!offset) {
    return Err<u32>(StatusError(X_STATUS_NO_MEMORY));
  }
  const u32 address = base_ + *offset;
  void* block = memory_.TranslateVirtual(address);
  if (zero) {
    std::memset(block, 0, size);
  }
  REX_PROFILE_ALLOC_NAMED(block, size, kPoolName);
  return address;
}

void WorkspaceHeap::Free(u32 address) {
  if (!address) {
    return;
  }
  if (address < base_ || address - base_ >= size_) {
    REXSYS_WARN("Workspace free of {:#010x} is outside {:#010x}+{:#x}, ignored", address, base_,
                size_);
    return;
  }
  std::lock_guard lock(mutex_);
  if (!ranges_->Free(address - base_)) {
    REXSYS_WARN("Workspace free of {:#010x} is not a live block, ignored", address);
    return;
  }
  REX_PROFILE_FREE_NAMED(memory_.TranslateVirtual(address), kPoolName);
}

}  // namespace rex::kernel
