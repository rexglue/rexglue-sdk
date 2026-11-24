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

#include <cstring>

#include <rex/logging/log_category.h>
#include <rex/math.h>
#include "gpu/xenos/host/d3d12/d3d12_upload_buffer_pool.h"
#include "gpu/xenos/host/d3d12/d3d12_objects.h"

namespace rex::gpu::d3d12 {

namespace {
// Mapped memory after the part of each page that is handed out.
//
// Constant buffer pool pages are bound as root CBVs, which have no size in
// Direct3D 12, so reads from them are not bounds-checked. On vkd3d-proton
// (RADV), root CBVs are raw GPU addresses, and a read that goes even slightly
// past data ending exactly at the end of a page hits unmapped memory - a GPU
// page fault and a lost device. On Windows, such reads usually land in other
// mapped memory and go unnoticed. Padding every pool keeps this simple.
//
// Committed buffers are backed by D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT
// (64 KiB) granules anyway, so this costs one granule per page.
constexpr size_t kPageTailPadding = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
}  // namespace

D3D12UploadBufferPool::D3D12UploadBufferPool(const D3D12Provider& provider, size_t page_size)
    : GraphicsUploadBufferPool(rex::RoundUpPow2(
          page_size, static_cast<size_t>(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT))),
      provider_(provider) {}

u8* D3D12UploadBufferPool::Request(u64 submission_index, size_t size, size_t alignment,
                                   ID3D12Resource** buffer_out, size_t* offset_out,
                                   D3D12_GPU_VIRTUAL_ADDRESS* gpu_address_out) {
  size_t offset;
  const D3D12Page* page = static_cast<const D3D12Page*>(
      GraphicsUploadBufferPool::Request(submission_index, size, alignment, offset));
  if (!page) {
    return nullptr;
  }
  if (buffer_out) {
    *buffer_out = page->buffer.Get();
  }
  if (offset_out) {
    *offset_out = offset;
  }
  if (gpu_address_out) {
    *gpu_address_out = page->gpu_address + offset;
  }
  return reinterpret_cast<u8*>(page->mapping) + offset;
}

u8* D3D12UploadBufferPool::RequestPartial(u64 submission_index, size_t size, size_t alignment,
                                          ID3D12Resource** buffer_out, size_t* offset_out,
                                          size_t* size_out,
                                          D3D12_GPU_VIRTUAL_ADDRESS* gpu_address_out) {
  size_t offset, size_obtained;
  const D3D12Page* page = static_cast<const D3D12Page*>(GraphicsUploadBufferPool::RequestPartial(
      submission_index, size, alignment, offset, size_obtained));
  if (!page) {
    return nullptr;
  }
  if (buffer_out) {
    *buffer_out = page->buffer.Get();
  }
  if (offset_out) {
    *offset_out = offset;
  }
  if (size_out) {
    *size_out = size_obtained;
  }
  if (gpu_address_out) {
    *gpu_address_out = page->gpu_address + offset;
  }
  return reinterpret_cast<u8*>(page->mapping) + offset;
}

std::unique_ptr<GraphicsUploadBufferPool::Page> D3D12UploadBufferPool::CreatePageImplementation() {
  D3D12_RESOURCE_DESC buffer_desc;
  objects::FillBufferResourceDesc(buffer_desc, page_size_ + kPageTailPadding,
                                  D3D12_RESOURCE_FLAG_NONE);
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  if (FAILED(provider_.GetDevice()->CreateCommittedResource(
          &objects::kHeapPropertiesUpload, provider_.GetHeapFlagCreateNotZeroed(), &buffer_desc,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)))) {
    REXUI_FAIL(nullptr, "Failed to create a D3D upload buffer with {} bytes", page_size_);
  }
  D3D12_RANGE read_range;
  read_range.Begin = 0;
  read_range.End = 0;
  void* mapping;
  if (FAILED(buffer->Map(0, &read_range, &mapping))) {
    REXUI_ERROR("Failed to map a D3D upload buffer with {} bytes", page_size_);
    buffer->Release();
    return nullptr;
  }
  // Deterministic contents for reads that go past the end of the page.
  std::memset(static_cast<u8*>(mapping) + page_size_, 0, kPageTailPadding);
  return std::make_unique<D3D12Page>(buffer.Get(), mapping);
}

D3D12UploadBufferPool::D3D12Page::D3D12Page(ID3D12Resource* buffer, void* mapping)
    : buffer(buffer), mapping(mapping) {
  gpu_address = buffer->GetGPUVirtualAddress();
}

}  // namespace rex::gpu::d3d12
