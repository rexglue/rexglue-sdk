/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

#include "gpu/xenos/host/d3d12/d3d12_provider.h"

namespace rex::gpu::d3d12 {

/**
 * Pool of shader visible descriptor heap pages.
 *
 * A submission index is the fence value or a value derived from it.
 */
class D3D12DescriptorHeapPool {
 public:
  static constexpr uint64_t kHeapIndexInvalid = UINT64_MAX;

  D3D12DescriptorHeapPool(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                          uint32_t page_size);
  ~D3D12DescriptorHeapPool();

  void Reclaim(uint64_t completed_submission_index);
  void ChangeSubmissionTimeline();
  void ClearCache();

  /**
   * Allocates descriptor space in the current heap.
   *
   * All descriptors of a draw call must share a heap, so a full update is needed when the current
   * heap lacks space for a partial update or the heap holding the unchanged part is outdated.
   * Callers doing partial updates pass the previous return value for their descriptor set. A
   * result equal to previous_heap_index means a partial update with count_for_partial_update
   * descriptors allocated. Any other result means all descriptors are rewritten with
   * count_for_full_update allocated. Must be called even when a partial update writes nothing
   * (count_for_partial_update 0), because a full update may be required.
   *
   * @param submission_index Fence value or a value derived from it.
   * @param previous_heap_index Value returned by the previous call for this descriptor set.
   * @param count_for_partial_update Descriptors to allocate for a partial update.
   * @param count_for_full_update Descriptors to allocate for a full update.
   * @param index_out Receives the first allocated descriptor index within the heap.
   * @return The heap reset index, or kHeapIndexInvalid on error.
   */
  uint64_t Request(uint64_t submission_index, uint64_t previous_heap_index,
                   uint32_t count_for_partial_update, uint32_t count_for_full_update,
                   uint32_t& index_out);

  /**
   * Returns the current heap for binding and writing.
   *
   * Valid only after a successful Request because the heap may not exist before.
   *
   * @return The current heap.
   */
  ID3D12DescriptorHeap* GetLastRequestHeap() const { return writable_first_->heap.Get(); }
  D3D12_CPU_DESCRIPTOR_HANDLE GetLastRequestHeapCPUStart() const {
    return writable_first_->cpu_start;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE GetLastRequestHeapGPUStart() const {
    return writable_first_->gpu_start;
  }

 private:
  ID3D12Device* device_;
  D3D12_DESCRIPTOR_HEAP_TYPE type_;
  uint32_t page_size_;

  struct Page {
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start;
    uint64_t last_submission_index;
    Page* next;
  };

  Page* writable_first_ = nullptr;
  Page* writable_last_ = nullptr;
  Page* submitted_first_ = nullptr;
  Page* submitted_last_ = nullptr;
  uint64_t current_heap_index_ = 0;
  uint32_t current_page_used_ = 0;
};

}  // namespace rex::gpu::d3d12
