/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <utility>

#include "gpu/xenos/host/d3d12/d3d12_provider.h"

namespace rex::gpu::d3d12 {
namespace objects {

using DescriptorCPUGPUHandlePair =
    std::pair<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE>;

inline constexpr D3D12_HEAP_PROPERTIES kHeapPropertiesDefault = {
    D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
inline constexpr D3D12_HEAP_PROPERTIES kHeapPropertiesUpload = {
    D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
inline constexpr D3D12_HEAP_PROPERTIES kHeapPropertiesReadback = {
    D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};

/**
 * Builds a graphics pipeline state description with every field named, the enum fields valid.
 *
 * @return The description, zero for the fields without an enum type.
 */
inline D3D12_GRAPHICS_PIPELINE_STATE_DESC BuildGraphicsPipelineStateDesc() {
  constexpr D3D12_RENDER_TARGET_BLEND_DESC kBlend = {.BlendEnable = FALSE,
                                                     .LogicOpEnable = FALSE,
                                                     .SrcBlend = D3D12_BLEND_ONE,
                                                     .DestBlend = D3D12_BLEND_ZERO,
                                                     .BlendOp = D3D12_BLEND_OP_ADD,
                                                     .SrcBlendAlpha = D3D12_BLEND_ONE,
                                                     .DestBlendAlpha = D3D12_BLEND_ZERO,
                                                     .BlendOpAlpha = D3D12_BLEND_OP_ADD,
                                                     .LogicOp = D3D12_LOGIC_OP_CLEAR,
                                                     .RenderTargetWriteMask = 0};
  constexpr D3D12_DEPTH_STENCILOP_DESC kStencil = {.StencilFailOp = D3D12_STENCIL_OP_KEEP,
                                                   .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
                                                   .StencilPassOp = D3D12_STENCIL_OP_KEEP,
                                                   .StencilFunc = D3D12_COMPARISON_FUNC_NONE};
  return D3D12_GRAPHICS_PIPELINE_STATE_DESC{
      .pRootSignature = nullptr,
      .VS = {},
      .PS = {},
      .DS = {},
      .HS = {},
      .GS = {},
      .StreamOutput = {},
      .BlendState = {.AlphaToCoverageEnable = FALSE,
                     .IndependentBlendEnable = FALSE,
                     .RenderTarget = {kBlend, kBlend, kBlend, kBlend, kBlend, kBlend, kBlend,
                                      kBlend}},
      .SampleMask = 0,
      .RasterizerState = {.FillMode = D3D12_FILL_MODE_SOLID,
                          .CullMode = D3D12_CULL_MODE_BACK,
                          .FrontCounterClockwise = FALSE,
                          .DepthBias = 0,
                          .DepthBiasClamp = 0.0f,
                          .SlopeScaledDepthBias = 0.0f,
                          .DepthClipEnable = FALSE,
                          .MultisampleEnable = FALSE,
                          .AntialiasedLineEnable = FALSE,
                          .ForcedSampleCount = 0,
                          .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF},
      .DepthStencilState = {.DepthEnable = FALSE,
                            .DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO,
                            .DepthFunc = D3D12_COMPARISON_FUNC_NONE,
                            .StencilEnable = FALSE,
                            .StencilReadMask = 0,
                            .StencilWriteMask = 0,
                            .FrontFace = kStencil,
                            .BackFace = kStencil},
      .InputLayout = {},
      .IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED,
      .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED,
      .NumRenderTargets = 0,
      .RTVFormats = {},
      .DSVFormat = DXGI_FORMAT_UNKNOWN,
      .SampleDesc = {.Count = 0, .Quality = 0},
      .NodeMask = 0,
      .CachedPSO = {},
      .Flags = D3D12_PIPELINE_STATE_FLAG_NONE};
}

/**
 * Builds a default type heap description with every field named.
 *
 * @param size_in_bytes The heap size.
 * @param flags The heap flags.
 * @return The description.
 */
inline D3D12_HEAP_DESC BuildDefaultHeapDesc(u64 size_in_bytes, D3D12_HEAP_FLAGS flags) {
  return D3D12_HEAP_DESC{.SizeInBytes = size_in_bytes,
                         .Properties = kHeapPropertiesDefault,
                         .Alignment = 0,
                         .Flags = flags};
}

template <typename T>
bool ReleaseAndNull(T& object) {
  if (object != nullptr) {
    object->Release();
    object = nullptr;
    return true;
  }
  return false;
};

ID3D12RootSignature* CreateRootSignature(const D3D12Provider& provider,
                                         const D3D12_ROOT_SIGNATURE_DESC& desc);

ID3D12PipelineState* CreateComputePipeline(ID3D12Device* device, const void* shader,
                                           size_t shader_size, ID3D12RootSignature* root_signature);

constexpr DXGI_FORMAT GetUintPow2DXGIFormat(u32 element_size_bytes_log2) {
  switch (element_size_bytes_log2) {
    case 0:
      return DXGI_FORMAT_R8_UINT;
    case 1:
      return DXGI_FORMAT_R16_UINT;
    case 2:
      return DXGI_FORMAT_R32_UINT;
    case 3:
      return DXGI_FORMAT_R32G32_UINT;
    case 4:
      return DXGI_FORMAT_R32G32B32A32_UINT;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

inline void FillBufferResourceDesc(D3D12_RESOURCE_DESC& desc, UINT64 size,
                                   D3D12_RESOURCE_FLAGS flags) {
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Alignment = 0;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
}

void CreateBufferRawSRV(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE handle,
                        ID3D12Resource* buffer, u32 size, u64 offset = 0);
void CreateBufferRawUAV(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE handle,
                        ID3D12Resource* buffer, u32 size, u64 offset = 0);
void CreateBufferTypedSRV(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE handle,
                          ID3D12Resource* buffer, DXGI_FORMAT format, u32 num_elements,
                          u64 first_element = 0);
void CreateBufferTypedUAV(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE handle,
                          ID3D12Resource* buffer, DXGI_FORMAT format, u32 num_elements,
                          u64 first_element = 0);

}  // namespace objects
}  // namespace rex::gpu::d3d12
