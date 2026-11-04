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

#include <rex/assert.h>
#include <rex/logging/log.h>
#include "gpu/xenos/host/d3d12/d3d12_descriptor_heap_pool.h"
#include "gpu/xenos/host/page_list.h"

namespace rex::gpu::d3d12 {

D3D12DescriptorHeapPool::D3D12DescriptorHeapPool(ID3D12Device* device,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE type, u32 page_size)
    : device_(device), type_(type), page_size_(page_size) {}

D3D12DescriptorHeapPool::~D3D12DescriptorHeapPool() {
  ClearCache();
}

void D3D12DescriptorHeapPool::Reclaim(u64 completed_submission_index) {
  ReclaimPages(submitted_first_, submitted_last_, writable_first_, writable_last_,
               completed_submission_index);
}

void D3D12DescriptorHeapPool::ChangeSubmissionTimeline() {
  ResetPageTimeline(submitted_first_, submitted_last_, writable_first_, writable_last_);
}

void D3D12DescriptorHeapPool::ClearCache() {
  ++current_heap_index_;
  current_page_used_ = 0;
  while (submitted_first_) {
    auto next = submitted_first_->next;
    delete submitted_first_;
    submitted_first_ = next;
  }
  submitted_last_ = nullptr;
  while (writable_first_) {
    auto next = writable_first_->next;
    delete writable_first_;
    writable_first_ = next;
  }
  writable_last_ = nullptr;
}

u64 D3D12DescriptorHeapPool::Request(u64 submission_index, u64 previous_heap_index,
                                     u32 count_for_partial_update, u32 count_for_full_update,
                                     u32& index_out) {
  REX_ASSERT(count_for_partial_update <= count_for_full_update);
  REX_ASSERT(count_for_full_update <= page_size_);
  if (count_for_partial_update > count_for_full_update || count_for_full_update > page_size_) {
    return kHeapIndexInvalid;
  }
  REX_ASSERT(!current_page_used_ || submission_index >= writable_first_->last_submission_index);
  REX_ASSERT(!submitted_last_ || submission_index >= submitted_last_->last_submission_index);
  u32 count =
      previous_heap_index == current_heap_index_ ? count_for_partial_update : count_for_full_update;
  if (page_size_ - current_page_used_ < count) {
    if (submitted_last_) {
      submitted_last_->next = writable_first_;
    } else {
      submitted_first_ = writable_first_;
    }
    submitted_last_ = writable_first_;
    writable_first_ = writable_first_->next;
    submitted_last_->next = nullptr;
    if (!writable_first_) {
      writable_last_ = nullptr;
    }
    ++current_heap_index_;
    current_page_used_ = 0;
    count = count_for_full_update;
  }
  if (!writable_first_) {
    D3D12_DESCRIPTOR_HEAP_DESC new_heap_desc;
    new_heap_desc.Type = type_;
    new_heap_desc.NumDescriptors = page_size_;
    new_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    new_heap_desc.NodeMask = 0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> new_heap;
    if (FAILED(device_->CreateDescriptorHeap(&new_heap_desc, IID_PPV_ARGS(&new_heap)))) {
      REXUI_FAIL(kHeapIndexInvalid, "Failed to create a heap for {} shader visible descriptors",
                 page_size_);
    }
    writable_first_ = new Page;
    writable_first_->heap = new_heap;
    writable_first_->cpu_start = new_heap->GetCPUDescriptorHandleForHeapStart();
    writable_first_->gpu_start = new_heap->GetGPUDescriptorHandleForHeapStart();
    writable_first_->last_submission_index = submission_index;
    writable_first_->next = nullptr;
    writable_last_ = writable_first_;
  }
  writable_first_->last_submission_index = submission_index;
  index_out = current_page_used_;
  current_page_used_ += count;
  return current_heap_index_;
}

}  // namespace rex::gpu::d3d12
