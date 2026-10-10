/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/types.h>
#include "gpu/xenos/host/d3d12/d3d12_provider.h"

namespace rex::gpu::d3d12 {

/**
 * Single descriptor pool with reference counting and unique ownership of allocations.
 *
 * Safe when the release order of the descriptor heap and its descriptors is undefined.
 */
class D3D12CpuDescriptorPool : public std::enable_shared_from_this<D3D12CpuDescriptorPool>,
                               public rex::NonCopyable {
 public:
  class Descriptor : public rex::MoveOnly {
   public:
    Descriptor() = default;
    Descriptor(std::shared_ptr<D3D12CpuDescriptorPool> pool, size_t index)
        : pool_(std::move(pool)), index_(index) {}
    Descriptor(Descriptor&& descriptor) noexcept {
      pool_ = std::move(descriptor.pool_);
      index_ = descriptor.index_;
    }
    Descriptor& operator=(Descriptor&& descriptor) noexcept {
      if (IsValid() && pool_ == descriptor.pool_ && index_ == descriptor.index_) {
        return *this;
      }
      Free();
      pool_ = std::move(descriptor.pool_);
      index_ = descriptor.index_;
      return *this;
    }
    ~Descriptor() { Free(); }
    void Free() {
      if (!pool_) {
        return;
      }
      pool_->FreeDescriptor(index_);
      pool_.reset();
    }
    bool IsValid() const { return pool_ != nullptr; }
    operator bool() const { return IsValid(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetHandle() const {
      REX_ASSERT(IsValid());
      return pool_->GetHandle(index_);
    }

   private:
    std::shared_ptr<D3D12CpuDescriptorPool> pool_;
    size_t index_ = 0;
  };

  D3D12CpuDescriptorPool(const gpu::d3d12::D3D12Provider& provider, D3D12_DESCRIPTOR_HEAP_TYPE type,
                         u32 heap_size_log2)
      : provider_(provider), type_(type), heap_size_log2_(heap_size_log2) {
    REX_ASSERT(heap_size_log2 <= 31);
  }
  ~D3D12CpuDescriptorPool() {
    for (ID3D12DescriptorHeap* heap : heaps_) {
      heap->Release();
    }
  }

  Descriptor AllocateDescriptor();

 private:
  void FreeDescriptor(size_t index) noexcept {
    next_free_[index] = free_head_;
    free_head_ = index;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE GetHandle(size_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE heap_start =
        heaps_[index >> heap_size_log2_]->GetCPUDescriptorHandleForHeapStart();
    u32 heap_local_index = static_cast<u32>(index & ((static_cast<u32>(1) << heap_size_log2_) - 1));
    return provider_.OffsetDescriptor(type_, heap_start, heap_local_index);
  }

  const gpu::d3d12::D3D12Provider& provider_;
  D3D12_DESCRIPTOR_HEAP_TYPE type_;
  u32 heap_size_log2_;
  std::vector<ID3D12DescriptorHeap*> heaps_;
  std::vector<size_t> next_free_;
  size_t free_head_ = SIZE_MAX;
  u32 last_heap_allocated_ = 0;
};

}  // namespace rex::gpu::d3d12
