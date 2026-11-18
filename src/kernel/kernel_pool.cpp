/**
 * @file        kernel/kernel_pool.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "kernel/kernel_pool.h"

#include <algorithm>

#include <rex/kernel/heap.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/types/thread.h>
#include <rex/logging/log_category.h>

namespace rex::kernel {

namespace {

constexpr u8 kStateFree = 0x55;
constexpr u8 kStateUsed = 0xAA;
constexpr u8 kStateLookaside = 0x5A;
constexpr u32 kLargeThreshold = 0xFD8;
constexpr u32 kPageSize = 0x1000;
constexpr size_t kPageCount = size_t{1} << 20;
constexpr u32 kHeaderSize = sizeof(PoolBlockHeader);
constexpr u32 kTitleDescriptor = 0;
constexpr u32 kSystemDescriptor = 1;

/**
 * Marks a free list link, the console sets bit 0 of every link. The guest sees the link values
 * block | 1, with 1 at both ends of a list. The console links through the link field address and
 * the descriptor list head, which changes no count and no block address.
 */
constexpr u32 kLinkTag = 1;

bool IsTitleThreadCalling(GuestAddressSpace& memory) {
  const ThreadState* state = ThreadState::Get();
  if (state == nullptr || state->GetAddressSpace() != &memory) {
    return false;
  }
  const u32 pcr_address = static_cast<u32>(state->GetContext()->r13.u64);
  if (pcr_address == 0) {
    return false;
  }
  const auto* pcr = memory.TranslateVirtual<const X_KPCR*>(pcr_address);
  if (pcr->prcb_data.dpc_active) {
    return pcr->processtype_value_in_dpc == kXProcTypeUser;
  }
  const u32 thread_address = static_cast<u32>(pcr->prcb_data.current_thread);
  if (thread_address == 0) {
    return false;
  }
  return memory.TranslateVirtual<const X_KTHREAD*>(thread_address)->process_type == kXProcTypeUser;
}

}  // namespace

// calloc gives lazily zeroed pages, so the 1 MiB table costs nothing until a page is used, and it
// does not go through HostAlloc.
KernelPool::KernelPool(GuestAddressSpace& memory)
    : memory_(memory), small_pages_(static_cast<u8*>(std::calloc(kPageCount, 1)), &std::free) {
  descriptors_[kTitleDescriptor].title = true;
}

PoolBlockHeader* KernelPool::Header(u32 block) const {
  return memory_.TranslateVirtual<PoolBlockHeader*>(block);
}

void KernelPool::ZeroLinks(u32 block) const {
  auto* links = memory_.TranslateVirtual<be_u32*>(block + kHeaderSize);
  links[0] = 0;
  links[1] = 0;
}

u32 KernelPool::NextBlock(u32 block) const {
  return block + Header(block)->units * kUnitSize;
}

void KernelPool::Insert(Descriptor& descriptor, u32 block, bool at_tail) {
  const u32 units = Header(block)->units;
  auto* links = memory_.TranslateVirtual<be_u32*>(block + kHeaderSize);
  u32& head = descriptor.head[units];
  u32& tail = descriptor.tail[units];
  if (head == 0) {
    links[0] = kLinkTag;
    links[1] = kLinkTag;
    head = tail = block;
  } else if (at_tail) {
    memory_.TranslateVirtual<be_u32*>(tail + kHeaderSize)[0] = block | kLinkTag;
    links[0] = kLinkTag;
    links[1] = tail | kLinkTag;
    tail = block;
  } else {
    memory_.TranslateVirtual<be_u32*>(head + kHeaderSize)[1] = block | kLinkTag;
    links[0] = head | kLinkTag;
    links[1] = kLinkTag;
    head = block;
  }
}

void KernelPool::Remove(Descriptor& descriptor, u32 block) {
  const u32 units = Header(block)->units;
  auto* links = memory_.TranslateVirtual<be_u32*>(block + kHeaderSize);
  const u32 next = static_cast<u32>(links[0]) & ~kLinkTag;
  const u32 previous = static_cast<u32>(links[1]) & ~kLinkTag;
  if (previous != 0) {
    memory_.TranslateVirtual<be_u32*>(previous + kHeaderSize)[0] = links[0];
  } else {
    descriptor.head[units] = next;
  }
  if (next != 0) {
    memory_.TranslateVirtual<be_u32*>(next + kHeaderSize)[1] = links[1];
  } else {
    descriptor.tail[units] = previous;
  }
}

u32 KernelPool::Allocate(u32 size, u32 tag, u32 pool_type) {
  u32 index = 0;
  switch (pool_type) {
    case 0:
      index = IsTitleThreadCalling(memory_) ? kTitleDescriptor : kSystemDescriptor;
      break;
    case 1:
      index = kTitleDescriptor;
      break;
    case 2:
      index = kSystemDescriptor;
      break;
    default:
      return 0;
  }
  std::lock_guard lock(mutex_);
  Descriptor& descriptor = descriptors_[index];
  if (size > kLargeThreshold) {
    return AllocateLarge(descriptor, index, size);
  }
  return AllocateSmall(descriptor, index, (size + 39) >> 5, tag);
}

u32 KernelPool::AllocateLarge(Descriptor& descriptor, u32 index, u32 size) {
  const u32 flags = kSystemHeapDefault | (descriptor.title ? kSystemHeapTitle : 0);
  const u32 address = memory_.SystemHeapAlloc(size, kPageSize, flags);
  if (address == 0) {
    return 0;
  }
  large_blocks_[address] = LargeBlock{static_cast<u8>(index), size};
  return address;
}

u32 KernelPool::AllocateSmall(Descriptor& descriptor, u32 index, u32 units, u32 tag) {
  u32 block = 0;
  if (units <= kMaxLookasideUnits && descriptor.lookaside[units].count != 0) {
    Lookaside& cached = descriptor.lookaside[units];
    block = cached.blocks[--cached.count];
    PoolBlockHeader* header = Header(block);
    header->state = kStateUsed;
    header->tag = tag;
    ZeroLinks(block);
    return block + kHeaderSize;
  }
  for (int attempt = 0; attempt < 2 && block == 0; ++attempt) {
    for (u32 size = units + 1; size <= kPageUnits; ++size) {
      if (descriptor.head[size] != 0) {
        block = descriptor.head[size];
        break;
      }
    }
    if (block != 0) {
      break;
    }
    const u32 flags = kSystemHeapDefault | (descriptor.title ? kSystemHeapTitle : 0);
    const u32 page = memory_.SystemHeapAlloc(kPageSize, kPageSize, flags);
    if (page == 0) {
      return 0;
    }
    small_pages_[page / kPageSize] = static_cast<u8>(index + 1);
    PoolBlockHeader* header = Header(page);
    header->previous_units = 0;
    header->units = kPageUnits;
    header->state = kStateFree;
    Insert(descriptor, page, false);
  }
  if (block == 0) {
    return 0;
  }
  Remove(descriptor, block);
  PoolBlockHeader* found = Header(block);
  const u32 found_units = found->units;
  const u32 page = block & ~(kPageSize - 1);
  const u32 remainder_units = found_units - units;
  u32 allocated;
  u32 remainder;
  if (found->previous_units == 0) {
    allocated = block;
    remainder = block + units * kUnitSize;
    Header(remainder)->previous_units = static_cast<u8>(units);
    Header(allocated)->units = static_cast<u8>(units);
  } else {
    remainder = block;
    allocated = block + remainder_units * kUnitSize;
    Header(allocated)->previous_units = static_cast<u8>(remainder_units);
    Header(allocated)->units = static_cast<u8>(units);
  }
  PoolBlockHeader* remainder_header = Header(remainder);
  remainder_header->units = static_cast<u8>(remainder_units);
  remainder_header->state = kStateFree;
  Insert(descriptor, remainder, true);
  const u32 after = NextBlock(std::max(allocated, remainder));
  if (after < page + kPageSize) {
    Header(after)->previous_units =
        static_cast<u8>(allocated > remainder ? units : remainder_units);
  }
  PoolBlockHeader* header = Header(allocated);
  header->state = kStateUsed;
  header->tag = tag;
  ZeroLinks(allocated);
  return allocated + kHeaderSize;
}

bool KernelPool::Owns(u32 address) {
  std::lock_guard lock(mutex_);
  if ((address & (kPageSize - 1)) == 0) {
    return large_blocks_.contains(address);
  }
  const u32 page = address & ~(kPageSize - 1);
  return small_pages_[page / kPageSize] != 0 && address >= page + kHeaderSize &&
         Header(address - kHeaderSize)->state == kStateUsed;
}

bool KernelPool::Free(u32 address) {
  std::lock_guard lock(mutex_);
  if ((address & (kPageSize - 1)) == 0) {
    const auto large = large_blocks_.find(address);
    if (large == large_blocks_.end()) {
      return false;
    }
    memory_.SystemHeapFree(address);
    large_blocks_.erase(large);
    return true;
  }
  const u32 page = address & ~(kPageSize - 1);
  const u8 owner = small_pages_[page / kPageSize];
  if (owner == 0 || address < page + kHeaderSize) {
    return false;
  }
  const u32 block = address - kHeaderSize;
  if (Header(block)->state != kStateUsed) {
    REXSYS_WARN("Pool free of {:08X} that is not an allocated block", address);
    return true;
  }
  FreeSmall(descriptors_[owner - 1], block, page);
  return true;
}

void KernelPool::FreeSmall(Descriptor& descriptor, u32 block, u32 page) {
  PoolBlockHeader* header = Header(block);
  u32 units = header->units;
  if (units <= kMaxLookasideUnits && descriptor.lookaside[units].count < kLookasideDepth) {
    Lookaside& cached = descriptor.lookaside[units];
    header->state = kStateLookaside;
    cached.blocks[cached.count++] = block;
    return;
  }
  bool coalesced = false;
  const u32 next = block + units * kUnitSize;
  if (next < page + kPageSize && Header(next)->state == kStateFree) {
    Remove(descriptor, next);
    units += Header(next)->units;
    coalesced = true;
  }
  u32 start = block;
  if (header->previous_units != 0) {
    const u32 previous = block - header->previous_units * kUnitSize;
    if (Header(previous)->state == kStateFree) {
      Remove(descriptor, previous);
      units += Header(previous)->units;
      start = previous;
      coalesced = true;
    }
  }
  if (start == page && units == kPageUnits) {
    memory_.SystemHeapFree(page);
    small_pages_[page / kPageSize] = 0;
    return;
  }
  PoolBlockHeader* merged = Header(start);
  merged->units = static_cast<u8>(units);
  merged->state = kStateFree;
  const u32 after = start + units * kUnitSize;
  if (coalesced && after < page + kPageSize) {
    Header(after)->previous_units = static_cast<u8>(units);
  }
  Insert(descriptor, start, coalesced);
}

}  // namespace rex::kernel
