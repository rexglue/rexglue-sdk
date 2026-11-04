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

#include <cstdint>
#include <memory>

#include <rex/logging/log_category.h>
#include "gpu/xenos/host/d3d12/d3d12_cpu_descriptor_pool.h"

namespace rex::gpu::d3d12 {

D3D12CPUDescriptorPool::Descriptor D3D12CPUDescriptorPool::AllocateDescriptor() {
  if (free_head_ != SIZE_MAX) {
    const size_t index = free_head_;
    free_head_ = next_free_[index];
    return Descriptor(shared_from_this(), index);
  }
  u32 heap_size = static_cast<u32>(1) << heap_size_log2_;
  if (!heaps_.empty() && last_heap_allocated_count_ < heap_size) {
    return Descriptor(shared_from_this(),
                      (heaps_.size() - 1) * heap_size + last_heap_allocated_count_++);
  }
  heaps_.reserve(heaps_.size() + 1);
  next_free_.resize((heaps_.size() + 1) * size_t{heap_size});
  D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc;
  descriptor_heap_desc.Type = type_;
  descriptor_heap_desc.NumDescriptors = heap_size;
  descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  descriptor_heap_desc.NodeMask = 0;
  ID3D12DescriptorHeap* descriptor_heap;
  if (FAILED(provider_.GetDevice()->CreateDescriptorHeap(&descriptor_heap_desc,
                                                         IID_PPV_ARGS(&descriptor_heap)))) {
    REXUI_FAIL(Descriptor(),
               "Failed to create a non shader visible descriptor heap for {} "
               "descriptors",
               heap_size);
  }
  heaps_.push_back(descriptor_heap);
  last_heap_allocated_count_ = 1;
  return Descriptor(shared_from_this(), (heaps_.size() - 1) * heap_size);
}

}  // namespace rex::gpu::d3d12
