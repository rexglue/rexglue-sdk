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
#include <deque>
#include <memory>

#include <rex/assert.h>
#include "gpu/xenos/primitive_processor.h"
#include "gpu/xenos/host/d3d12/d3d12_api.h"
#include "gpu/xenos/host/d3d12/d3d12_upload_buffer_pool.h"

namespace rex::gpu::d3d12 {

class D3D12CommandProcessor;

class D3D12PrimitiveProcessor final : public PrimitiveProcessor {
 public:
  D3D12PrimitiveProcessor(const xenos::RegisterFile& register_file, const XenosSettings& settings,
                          IPhysicalMemory& memory, IPhysicalWriteWatch& watch,
                          SharedMemory& shared_memory, D3D12CommandProcessor& command_processor)
      : PrimitiveProcessor(register_file, settings, memory, watch, shared_memory),
        command_processor_(command_processor) {}
  ~D3D12PrimitiveProcessor();

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() { frame_index_buffer_pool_->ClearCache(); }

  void CompletedSubmissionUpdated();
  /**
   * Copies the builtin index buffer upload in the first submission. No barriers are submitted
   * first, as the destination is in its initial state and the upload buffer is GENERIC_READ.
   */
  void BeginSubmission();
  void BeginFrame();
  void EndFrame();

  D3D12_GPU_VIRTUAL_ADDRESS GetBuiltinIndexBufferGPUAddress(size_t handle) const {
    REX_ASSERT(builtin_index_buffer_ != nullptr);
    return static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(builtin_index_buffer_gpu_address_ +
                                                  GetBuiltinIndexBufferOffsetBytes(handle));
  }
  D3D12_GPU_VIRTUAL_ADDRESS GetConvertedIndexBufferGPUAddress(size_t handle) const {
    return frame_index_buffers_[handle];
  }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;

  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format, u32 index_count,
                                                       bool coalign_for_simd,
                                                       u32 coalignment_original_address,
                                                       size_t& driver_handle_out) override;

 private:
  D3D12CommandProcessor& command_processor_;

  Microsoft::WRL::ComPtr<ID3D12Resource> builtin_index_buffer_;
  D3D12_GPU_VIRTUAL_ADDRESS builtin_index_buffer_gpu_address_ = 0;
  /** Temporary buffer copied to builtin_index_buffer_ at the start of the first submission. */
  Microsoft::WRL::ComPtr<ID3D12Resource> builtin_index_buffer_upload_;
  /** UINT64_MAX when the upload buffer, if it exists, awaits the first submission. */
  u64 builtin_index_buffer_upload_submission_ = UINT64_MAX;

  std::unique_ptr<gpu::d3d12::D3D12UploadBufferPool> frame_index_buffer_pool_;
  /** Indexed by driver handle. */
  std::deque<D3D12_GPU_VIRTUAL_ADDRESS> frame_index_buffers_;
};

}  // namespace rex::gpu::d3d12
