/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "gpu/xenos/shared_memory.h"
#include <rex/memory.h>
#include "gpu/xenos/host/d3d12/d3d12_api.h"
#include "gpu/xenos/host/d3d12/d3d12_upload_buffer_pool.h"

namespace rex::gpu::d3d12 {

class D3D12CommandProcessor;

class D3D12SharedMemory : public SharedMemory {
 public:
  D3D12SharedMemory(D3D12CommandProcessor& command_processor, runtime::Memory& memory);
  ~D3D12SharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  ID3D12Resource* GetBuffer() const { return buffer_; }
  D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const { return buffer_gpu_address_; }

  void CompletedSubmissionUpdated();
  void BeginSubmission();

  // RequestRange can leave the buffer in copy destination state, so it precedes UseFor*.

  /** Makes the buffer usable for vertices, indices and texture untiling. */
  void UseForReading() {
    // Vertex fetch is also allowed in pixel shaders.
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_INDEX_BUFFER |
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  /** Makes the buffer usable for texture tiling after a resolve. */
  void UseForWriting() {
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  /** Makes the buffer usable as a source for copy commands. */
  void UseAsCopySource() { CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE); }
  /**
   * Marks UAV writes to the buffer by draws or dispatches, so the next UseForWriting issues a
   * UAV barrier ordering overlapping UAV writes and reads.
   */
  void MarkUAVWritesCommitNeeded() {
    if (buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      buffer_uav_writes_commit_needed_ = true;
    }
  }

  void WriteRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void WriteRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  /**
   * Writes a typed SRV descriptor. D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP makes the
   * smallest supported formats 32 bit.
   *
   * @param handle Destination CPU descriptor.
   * @param element_size_bytes_pow2 Log2 of the element size in bytes, 2 to 4.
   */
  void WriteUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                  uint32_t element_size_bytes_pow2);
  void WriteUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                  uint32_t element_size_bytes_pow2);

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;

  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  D3D12CommandProcessor& command_processor_;

  /** The 512 MB tiled buffer. */
  ID3D12Resource* buffer_ = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS buffer_gpu_address_ = 0;
  std::vector<ID3D12Heap*> buffer_tiled_heaps_;
  D3D12_RESOURCE_STATES buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  bool buffer_uav_writes_commit_needed_ = false;
  void CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state);

  /** Views in the non shader visible descriptor heap, copied instead of created for binding. */
  enum class BufferDescriptorIndex : uint32_t {
    kRawSRV,
    kR32UintSRV,
    kR32G32UintSRV,
    kR32G32B32A32UintSRV,
    kRawUAV,
    kR32UintUAV,
    kR32G32UintUAV,
    kR32G32B32A32UintUAV,

    kCount,
  };
  ID3D12DescriptorHeap* buffer_descriptor_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE buffer_descriptor_heap_start_;

  std::unique_ptr<gpu::d3d12::D3D12UploadBufferPool> upload_buffer_pool_;
};

}  // namespace rex::gpu::d3d12
