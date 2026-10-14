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

#include "gpu/xenos/host/d3d12/d3d12_provider.h"
#include "gpu/xenos/host/graphics_upload_buffer_pool.h"

namespace rex::gpu::d3d12 {

class D3D12UploadBufferPool : public GraphicsUploadBufferPool {
 public:
  /**
   * Creates the pool with the page size aligned to D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT.
   *
   * The heap backing a buffer is aligned to that value anyway, so the alignment wastes no space.
   *
   * @param provider Provider owning the device.
   * @param page_size Requested page size in bytes.
   */
  D3D12UploadBufferPool(const D3D12Provider& provider, size_t page_size = kDefaultPageSize);

  u8* Request(u64 submission_index, size_t size, size_t alignment, ID3D12Resource** buffer_out,
              size_t* offset_out, D3D12_GPU_VIRTUAL_ADDRESS* gpu_address_out);
  u8* RequestPartial(u64 submission_index, size_t size, size_t alignment,
                     ID3D12Resource** buffer_out, size_t* offset_out, size_t* size_out,
                     D3D12_GPU_VIRTUAL_ADDRESS* gpu_address_out);

 protected:
  std::unique_ptr<Page> CreatePageImplementation() override;

 private:
  struct D3D12Page : public Page {
    /**
     * Creates a reference to the buffer, which must stay mapped until this page is deleted.
     *
     * @param buffer Upload buffer backing the page.
     * @param mapping CPU mapping of the buffer.
     */
    D3D12Page(ID3D12Resource* buffer, void* mapping);
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    void* mapping;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address;
  };

  const D3D12Provider& provider_;
};

}  // namespace rex::gpu::d3d12
