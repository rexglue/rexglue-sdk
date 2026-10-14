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

#include "thirdparty/dxbc/DXBCChecksum.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include "gpu/xenos/d3d12/command_processor.h"
#include "gpu/xenos/d3d12/deferred_command_list.h"
#include "gpu/xenos/d3d12/render_target_cache.h"
#include "gpu/xenos/d3d12/texture_cache.h"
#include "gpu/xenos/format/dxbc.h"
#include "gpu/xenos/pipeline/shader/dxbc_translator.h"
#include "gpu/xenos/draw_state.h"
#include <rex/xenos/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string.h>
#include "gpu/xenos/host/d3d12/d3d12_provider.h"
#include "gpu/xenos/host/d3d12/d3d12_objects.h"

#include "gpu/xenos/cvar_table.h"

namespace rex::gpu::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/clear_uint2_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fullscreen_cw_vs.h"
#include "../shaders/bytecode/d3d12_5_1/host_depth_store_1xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/host_depth_store_2xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/host_depth_store_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/passthrough_position_xy_vs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_clear_32bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_clear_32bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_clear_64bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_clear_64bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_128bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_128bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_16bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_16bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_32bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_32bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_64bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_64bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_8bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_8bpp_scaled_cs.h"
}  // namespace shaders

const D3D12RenderTargetCache::ResolveCopyShaderCode
    D3D12RenderTargetCache::kResolveCopyShaders[static_cast<size_t>(
        draw_state::ResolveCopyShaderIndex::kCount)] = {
        {shaders::kResolveFast32BPP1x2xMSAACS, sizeof(shaders::kResolveFast32BPP1x2xMSAACS),
         shaders::kResolveFast32BPP1x2xMSAAScaledCS,
         sizeof(shaders::kResolveFast32BPP1x2xMSAAScaledCS)},
        {shaders::kResolveFast32BPP4xMSAACS, sizeof(shaders::kResolveFast32BPP4xMSAACS),
         shaders::kResolveFast32BPP4xMSAAScaledCS,
         sizeof(shaders::kResolveFast32BPP4xMSAAScaledCS)},
        {shaders::kResolveFast64BPP1x2xMSAACS, sizeof(shaders::kResolveFast64BPP1x2xMSAACS),
         shaders::kResolveFast64BPP1x2xMSAAScaledCS,
         sizeof(shaders::kResolveFast64BPP1x2xMSAAScaledCS)},
        {shaders::kResolveFast64BPP4xMSAACS, sizeof(shaders::kResolveFast64BPP4xMSAACS),
         shaders::kResolveFast64BPP4xMSAAScaledCS,
         sizeof(shaders::kResolveFast64BPP4xMSAAScaledCS)},
        {shaders::kResolveFull8BPPCS, sizeof(shaders::kResolveFull8BPPCS),
         shaders::kResolveFull8BPPScaledCS, sizeof(shaders::kResolveFull8BPPScaledCS)},
        {shaders::kResolveFull16BPPCS, sizeof(shaders::kResolveFull16BPPCS),
         shaders::kResolveFull16BPPScaledCS, sizeof(shaders::kResolveFull16BPPScaledCS)},
        {shaders::kResolveFull32BPPCS, sizeof(shaders::kResolveFull32BPPCS),
         shaders::kResolveFull32BPPScaledCS, sizeof(shaders::kResolveFull32BPPScaledCS)},
        {shaders::kResolveFull64BPPCS, sizeof(shaders::kResolveFull64BPPCS),
         shaders::kResolveFull64BPPScaledCS, sizeof(shaders::kResolveFull64BPPScaledCS)},
        {shaders::kResolveFull128BPPCS, sizeof(shaders::kResolveFull128BPPCS),
         shaders::kResolveFull128BPPScaledCS, sizeof(shaders::kResolveFull128BPPScaledCS)},
};

const u32 D3D12RenderTargetCache::kTransferUsedRootParameters[static_cast<size_t>(
    TransferRootSignatureIndex::kCount)] = {
    kTransferUsedRootParameterColorSRVBit | kTransferUsedRootParameterAddressConstantBit,
    kTransferUsedRootParameterDepthSRVBit | kTransferUsedRootParameterAddressConstantBit,
    kTransferUsedRootParameterDepthSRVBit | kTransferUsedRootParameterStencilSRVBit |
        kTransferUsedRootParameterAddressConstantBit,
    kTransferUsedRootParameterStencilMaskConstantBit | kTransferUsedRootParameterColorSRVBit |
        kTransferUsedRootParameterAddressConstantBit,
    kTransferUsedRootParameterStencilMaskConstantBit | kTransferUsedRootParameterStencilSRVBit |
        kTransferUsedRootParameterAddressConstantBit,
    kTransferUsedRootParameterColorSRVBit | kTransferUsedRootParameterAddressConstantBit |
        kTransferUsedRootParameterHostDepthSRVBit |
        kTransferUsedRootParameterHostDepthAddressConstantBit,
    kTransferUsedRootParameterDepthSRVBit | kTransferUsedRootParameterAddressConstantBit |
        kTransferUsedRootParameterHostDepthSRVBit |
        kTransferUsedRootParameterHostDepthAddressConstantBit,
    kTransferUsedRootParameterDepthSRVBit | kTransferUsedRootParameterStencilSRVBit |
        kTransferUsedRootParameterAddressConstantBit | kTransferUsedRootParameterHostDepthSRVBit |
        kTransferUsedRootParameterHostDepthAddressConstantBit,
};

const D3D12RenderTargetCache::TransferModeInfo
    D3D12RenderTargetCache::kTransferModes[static_cast<size_t>(TransferMode::kCount)] = {
        {TransferOutput::kDepth, TransferRootSignatureIndex::kColor,
         TransferRootSignatureIndex::kColor},
        {TransferOutput::kColor, TransferRootSignatureIndex::kColor,
         TransferRootSignatureIndex::kColor},
        {TransferOutput::kDepth, TransferRootSignatureIndex::kDepth,
         TransferRootSignatureIndex::kDepthStencil},
        {TransferOutput::kColor, TransferRootSignatureIndex::kDepthStencil,
         TransferRootSignatureIndex::kDepthStencil},
        {TransferOutput::kStencilBit, TransferRootSignatureIndex::kColorToStencilBit,
         TransferRootSignatureIndex::kColorToStencilBit},
        {TransferOutput::kStencilBit, TransferRootSignatureIndex::kStencilToStencilBit,
         TransferRootSignatureIndex::kStencilToStencilBit},
        {TransferOutput::kDepth, TransferRootSignatureIndex::kColorAndHostDepth,
         TransferRootSignatureIndex::kColorAndHostDepth},
        {TransferOutput::kDepth, TransferRootSignatureIndex::kDepthAndHostDepth,
         TransferRootSignatureIndex::kDepthStencilAndHostDepth},
};

D3D12RenderTargetCache::~D3D12RenderTargetCache() {
  Shutdown(true);
}

bool D3D12RenderTargetCache::Initialize() {
  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  if (REXCVAR_GET(::rex::gpu::cvars::render_target_path_d3d12) == "rtv") {
    path_ = Path::kHostRenderTargets;
  } else if (REXCVAR_GET(::rex::gpu::cvars::render_target_path_d3d12) == "rov") {
    path_ = Path::kPixelShaderInterlock;
  } else {
#if 1
    path_ = provider.GetAdapterVendorID() == gpu::GraphicsProvider::GPUVendorID::kIntel
                ? Path::kPixelShaderInterlock
                : Path::kHostRenderTargets;
#else
    path_ = provider.GetAdapterVendorID() == gpu::GraphicsProvider::GPUVendorID::kAMD
                ? Path::kHostRenderTargets
                : Path::kPixelShaderInterlock;
#endif
  }
  if (path_ == Path::kPixelShaderInterlock && !provider.AreRasterizerOrderedViewsSupported()) {
    path_ = Path::kHostRenderTargets;
  }

  u32 edram_buffer_size =
      xenos::kEDRAMSizeBytes * (GetDrawResolutionScaleX() * GetDrawResolutionScaleY());
  D3D12_RESOURCE_DESC edram_buffer_desc;
  gpu::d3d12::objects::FillBufferResourceDesc(edram_buffer_desc, edram_buffer_size,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  // Depth self comparison with host render targets and ROV drawing use the buffer as a UAV.
  edram_buffer_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // Zeroed for a stable initial value with ROV and to not leak data outside host render targets.
  if (FAILED(device->CreateCommittedResource(
          &gpu::d3d12::objects::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &edram_buffer_desc,
          edram_buffer_state_, nullptr, IID_PPV_ARGS(&edram_buffer_)))) {
    REXLOG_ERROR(gpu, "D3D12RenderTargetCache: Failed to create the EDRAM buffer");
    Shutdown();
    return false;
  }
  edram_buffer_->SetName(L"EDRAM Buffer");
  edram_buffer_modification_status_ = EDRAMBufferModificationStatus::kUnmodified;

  D3D12_DESCRIPTOR_HEAP_DESC edram_buffer_descriptor_heap_desc;
  edram_buffer_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  edram_buffer_descriptor_heap_desc.NumDescriptors =
      std::to_underlying(EDRAMBufferDescriptorIndex::kCount);
  edram_buffer_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  edram_buffer_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&edram_buffer_descriptor_heap_desc,
                                          IID_PPV_ARGS(&edram_buffer_descriptor_heap_)))) {
    REXLOG_ERROR(gpu,
                 "D3D12RenderTargetCache: Failed to create the descriptor heap for "
                 "EDRAM buffer views");
    Shutdown();
    return false;
  }
  edram_buffer_descriptor_heap_start_ =
      edram_buffer_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  gpu::d3d12::objects::CreateBufferRawSRV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    std::to_underlying(EDRAMBufferDescriptorIndex::kRawSRV)),
      edram_buffer_, edram_buffer_size);
  gpu::d3d12::objects::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    std::to_underlying(EDRAMBufferDescriptorIndex::kR32UintSRV)),
      edram_buffer_, DXGI_FORMAT_R32_UINT, edram_buffer_size >> 2);
  gpu::d3d12::objects::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    std::to_underlying(EDRAMBufferDescriptorIndex::kR32G32UintSRV)),
      edram_buffer_, DXGI_FORMAT_R32G32_UINT, edram_buffer_size >> 3);
  gpu::d3d12::objects::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          std::to_underlying(EDRAMBufferDescriptorIndex::kR32G32B32A32UintSRV)),
      edram_buffer_, DXGI_FORMAT_R32G32B32A32_UINT, edram_buffer_size >> 4);
  gpu::d3d12::objects::CreateBufferRawUAV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    std::to_underlying(EDRAMBufferDescriptorIndex::kRawUAV)),
      edram_buffer_, edram_buffer_size);
  gpu::d3d12::objects::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    std::to_underlying(EDRAMBufferDescriptorIndex::kR32UintUAV)),
      edram_buffer_, DXGI_FORMAT_R32_UINT, edram_buffer_size >> 2);
  gpu::d3d12::objects::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    std::to_underlying(EDRAMBufferDescriptorIndex::kR32G32UintUAV)),
      edram_buffer_, DXGI_FORMAT_R32G32_UINT, edram_buffer_size >> 3);
  gpu::d3d12::objects::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          std::to_underlying(EDRAMBufferDescriptorIndex::kR32G32B32A32UintUAV)),
      edram_buffer_, DXGI_FORMAT_R32G32B32A32_UINT, edram_buffer_size >> 4);

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  std::array<D3D12_ROOT_PARAMETER, 3> resolve_copy_root_parameters;
  resolve_copy_root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  resolve_copy_root_parameters[0].Constants.ShaderRegister = 0;
  resolve_copy_root_parameters[0].Constants.RegisterSpace = 0;
  // Shared memory is bound whole at 1x resolution, and in portions with scaled resolution.
  resolve_copy_root_parameters[0].Constants.Num32BitValues =
      (draw_resolution_scaled ? sizeof(draw_state::ResolveCopyShaderConstants::DestRelative)
                              : sizeof(draw_state::ResolveCopyShaderConstants)) /
      sizeof(u32);
  resolve_copy_root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_DESCRIPTOR_RANGE resolve_copy_dest_range;
  resolve_copy_dest_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  resolve_copy_dest_range.NumDescriptors = 1;
  resolve_copy_dest_range.BaseShaderRegister = 0;
  resolve_copy_dest_range.RegisterSpace = 0;
  resolve_copy_dest_range.OffsetInDescriptorsFromTableStart = 0;
  resolve_copy_root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  resolve_copy_root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  resolve_copy_root_parameters[1].DescriptorTable.pDescriptorRanges = &resolve_copy_dest_range;
  resolve_copy_root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_DESCRIPTOR_RANGE resolve_copy_source_range;
  resolve_copy_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  resolve_copy_source_range.NumDescriptors = 1;
  resolve_copy_source_range.BaseShaderRegister = 0;
  resolve_copy_source_range.RegisterSpace = 0;
  resolve_copy_source_range.OffsetInDescriptorsFromTableStart = 0;
  resolve_copy_root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  resolve_copy_root_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
  resolve_copy_root_parameters[2].DescriptorTable.pDescriptorRanges = &resolve_copy_source_range;
  resolve_copy_root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC resolve_copy_root_signature_desc;
  resolve_copy_root_signature_desc.NumParameters = UINT(resolve_copy_root_parameters.size());
  resolve_copy_root_signature_desc.pParameters = resolve_copy_root_parameters.data();
  resolve_copy_root_signature_desc.NumStaticSamplers = 0;
  resolve_copy_root_signature_desc.pStaticSamplers = nullptr;
  resolve_copy_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  resolve_copy_root_signature_ =
      gpu::d3d12::objects::CreateRootSignature(provider, resolve_copy_root_signature_desc);
  if (resolve_copy_root_signature_ == nullptr) {
    REXLOG_ERROR(gpu,
                 "D3D12RenderTargetCache: Failed to create the resolve copy root "
                 "signature");
    Shutdown();
    return false;
  }
  direct_resolve_root_signature_color_ = resolve_copy_root_signature_;
  direct_resolve_root_signature_depth_ = resolve_copy_root_signature_;
  direct_resolve_root_signature_color_->AddRef();
  direct_resolve_root_signature_depth_->AddRef();

  for (size_t i = 0; i < size_t(draw_state::ResolveCopyShaderIndex::kCount); ++i) {
    const draw_state::ResolveCopyShaderInfo& resolve_copy_shader_info =
        draw_state::kResolveCopyShaderInfo[i];
    const ResolveCopyShaderCode& resolve_copy_shader_code = kResolveCopyShaders[i];
    REX_ASSERT(resolve_copy_shader_code.unscaled && resolve_copy_shader_code.unscaled_size &&
               resolve_copy_shader_code.scaled && resolve_copy_shader_code.scaled_size);
    ID3D12PipelineState* resolve_copy_pipeline = gpu::d3d12::objects::CreateComputePipeline(
        device,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled
                               : resolve_copy_shader_code.unscaled,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled_size
                               : resolve_copy_shader_code.unscaled_size,
        resolve_copy_root_signature_);
    if (resolve_copy_pipeline == nullptr) {
      REXLOG_ERROR(gpu, "D3D12RenderTargetCache: Failed to create {} resolve copy pipeline",
                   resolve_copy_shader_info.debug_name);
      Shutdown();
      return false;
    }
    std::u16string resolve_copy_pipeline_name = rex::ToUTF16(resolve_copy_shader_info.debug_name);
    resolve_copy_pipeline->SetName(reinterpret_cast<LPCWSTR>(resolve_copy_pipeline_name.c_str()));
    resolve_copy_pipelines_[i] = resolve_copy_pipeline;
  }

  // Read at initialization so pipelines stay consistent across titles in one instance.
  use_stencil_reference_output_ =
      REXCVAR_GET(::rex::gpu::cvars::native_stencil_value_output) &&
      provider.IsPSSpecifiedStencilReferenceSupported() &&
      (REXCVAR_GET(::rex::gpu::cvars::native_stencil_value_output_d3d12_intel) ||
       provider.GetAdapterVendorID() != gpu::GraphicsProvider::GPUVendorID::kIntel);

  if (path_ == Path::kHostRenderTargets) {
    gamma_render_target_as_unorm16_ =
        REXCVAR_GET(::rex::gpu::cvars::gamma_render_target_as_unorm16);

    depth_float24_round_ = REXCVAR_GET(::rex::gpu::cvars::depth_float24_round);
    depth_float24_convert_in_pixel_shader_ =
        REXCVAR_GET(::rex::gpu::cvars::depth_float24_convert_in_pixel_shader);

    if (REXCVAR_GET(::rex::gpu::cvars::native_2x_msaa)) {
      msaa_2x_supported_ = true;
      static const DXGI_FORMAT kRenderTargetDXGIFormats[] = {
          DXGI_FORMAT_R16G16B16A16_FLOAT,
          DXGI_FORMAT_R16G16B16A16_SNORM,
          DXGI_FORMAT_R32G32_FLOAT,
          DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
          DXGI_FORMAT_R10G10B10A2_UNORM,
          DXGI_FORMAT_R8G8B8A8_UNORM,
          DXGI_FORMAT_R16G16_FLOAT,
          DXGI_FORMAT_R16G16_SNORM,
          DXGI_FORMAT_R32_FLOAT,
          DXGI_FORMAT_D24_UNORM_S8_UINT,
          // For ownership transfer.
          DXGI_FORMAT_R16G16B16A16_UINT,
          DXGI_FORMAT_R32G32_UINT,
          DXGI_FORMAT_R16G16_UINT,
          DXGI_FORMAT_R32_UINT,
      };
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS multisample_quality_levels;
      multisample_quality_levels.SampleCount = 2;
      multisample_quality_levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
      for (DXGI_FORMAT format : kRenderTargetDXGIFormats) {
        multisample_quality_levels.Format = format;
        multisample_quality_levels.NumQualityLevels = 0;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                               &multisample_quality_levels,
                                               sizeof(multisample_quality_levels))) ||
            !multisample_quality_levels.NumQualityLevels) {
          msaa_2x_supported_ = false;
          break;
        }
      }
    } else {
      msaa_2x_supported_ = false;
    }
    if (msaa_2x_supported_ && gamma_render_target_as_unorm16_) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS multisample_quality_levels;
      multisample_quality_levels.SampleCount = 2;
      multisample_quality_levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
      multisample_quality_levels.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
      multisample_quality_levels.NumQualityLevels = 0;
      if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                             &multisample_quality_levels,
                                             sizeof(multisample_quality_levels))) ||
          !multisample_quality_levels.NumQualityLevels) {
        msaa_2x_supported_ = false;
      }
    }
    if (!msaa_2x_supported_) {
      REXLOG_WARN(gpu,
                  "2x MSAA is not supported, emulated via top-left and bottom-right "
                  "samples of 4x MSAA");
    }

    descriptor_pool_color_ = std::make_unique<gpu::d3d12::D3D12CPUDescriptorPool>(
        provider, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 11);
    descriptor_pool_depth_ = std::make_unique<gpu::d3d12::D3D12CPUDescriptorPool>(
        provider, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 11);
    descriptor_pool_srv_ = std::make_unique<gpu::d3d12::D3D12CPUDescriptorPool>(
        provider, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 11);

    null_rtv_descriptor_ss_ = descriptor_pool_color_->AllocateDescriptor();
    null_rtv_descriptor_ms_ = descriptor_pool_color_->AllocateDescriptor();
    if (!null_rtv_descriptor_ss_ || !null_rtv_descriptor_ms_) {
      Shutdown();
      return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC null_rtv_desc;
    // Any format bindable as a render target works, but not DXGI_FORMAT_UNKNOWN.
    null_rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    null_rtv_desc.Texture2D.MipSlice = 0;
    null_rtv_desc.Texture2D.PlaneSlice = 0;
    device->CreateRenderTargetView(nullptr, &null_rtv_desc, null_rtv_descriptor_ss_.GetHandle());
    null_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    device->CreateRenderTargetView(nullptr, &null_rtv_desc, null_rtv_descriptor_ms_.GetHandle());

    D3D12_ROOT_PARAMETER
    host_depth_store_root_parameters[kHostDepthStoreRootParameterCount];
    D3D12_ROOT_PARAMETER& host_depth_store_root_constants =
        host_depth_store_root_parameters[kHostDepthStoreRootParameterConstants];
    host_depth_store_root_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    host_depth_store_root_constants.Constants.ShaderRegister = 0;
    host_depth_store_root_constants.Constants.RegisterSpace = 0;
    host_depth_store_root_constants.Constants.Num32BitValues =
        sizeof(HostDepthStoreConstants) / sizeof(u32);
    host_depth_store_root_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE host_depth_store_root_source_range;
    host_depth_store_root_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    host_depth_store_root_source_range.NumDescriptors = 1;
    host_depth_store_root_source_range.BaseShaderRegister = 0;
    host_depth_store_root_source_range.RegisterSpace = 0;
    host_depth_store_root_source_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER& host_depth_store_root_source =
        host_depth_store_root_parameters[kHostDepthStoreRootParameterSource];
    host_depth_store_root_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    host_depth_store_root_source.DescriptorTable.NumDescriptorRanges = 1;
    host_depth_store_root_source.DescriptorTable.pDescriptorRanges =
        &host_depth_store_root_source_range;
    host_depth_store_root_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE host_depth_store_root_dest_range;
    host_depth_store_root_dest_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    host_depth_store_root_dest_range.NumDescriptors = 1;
    host_depth_store_root_dest_range.BaseShaderRegister = 0;
    host_depth_store_root_dest_range.RegisterSpace = 0;
    host_depth_store_root_dest_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER& host_depth_store_root_dest =
        host_depth_store_root_parameters[kHostDepthStoreRootParameterDest];
    host_depth_store_root_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    host_depth_store_root_dest.DescriptorTable.NumDescriptorRanges = 1;
    host_depth_store_root_dest.DescriptorTable.pDescriptorRanges =
        &host_depth_store_root_dest_range;
    host_depth_store_root_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC host_depth_store_root_desc;
    host_depth_store_root_desc.NumParameters = UINT(rex::CountOf(host_depth_store_root_parameters));
    host_depth_store_root_desc.pParameters = host_depth_store_root_parameters;
    host_depth_store_root_desc.NumStaticSamplers = 0;
    host_depth_store_root_desc.pStaticSamplers = nullptr;
    host_depth_store_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    host_depth_store_root_signature_ =
        gpu::d3d12::objects::CreateRootSignature(provider, host_depth_store_root_desc);
    if (!host_depth_store_root_signature_) {
      REXLOG_ERROR(gpu,
                   "D3D12RenderTargetCache: Failed to create the host depth storing "
                   "root signature");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[std::to_underlying(xenos::MSAASamples::k1X)] =
        gpu::d3d12::objects::CreateComputePipeline(device, shaders::kHostDepthStore1xMSAACS,
                                                   sizeof(shaders::kHostDepthStore1xMSAACS),
                                                   host_depth_store_root_signature_);
    if (!host_depth_store_pipelines_[std::to_underlying(xenos::MSAASamples::k1X)]) {
      REXLOG_ERROR(gpu,
                   "D3D12RenderTargetCache: Failed to create the 1-sample host depth "
                   "storing pipeline");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[std::to_underlying(xenos::MSAASamples::k1X)]->SetName(
        L"Host Depth Store 1xMSAA");
    host_depth_store_pipelines_[std::to_underlying(xenos::MSAASamples::k2X)] =
        gpu::d3d12::objects::CreateComputePipeline(device, shaders::kHostDepthStore2xMSAACS,
                                                   sizeof(shaders::kHostDepthStore2xMSAACS),
                                                   host_depth_store_root_signature_);
    if (!host_depth_store_pipelines_[std::to_underlying(xenos::MSAASamples::k2X)]) {
      REXLOG_ERROR(gpu,
                   "D3D12RenderTargetCache: Failed to create the 2-sample host depth "
                   "storing pipeline");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[std::to_underlying(xenos::MSAASamples::k2X)]->SetName(
        L"Host Depth Store 2xMSAA");
    host_depth_store_pipelines_[std::to_underlying(xenos::MSAASamples::k4X)] =
        gpu::d3d12::objects::CreateComputePipeline(device, shaders::kHostDepthStore4xMSAACS,
                                                   sizeof(shaders::kHostDepthStore4xMSAACS),
                                                   host_depth_store_root_signature_);
    if (!host_depth_store_pipelines_[std::to_underlying(xenos::MSAASamples::k4X)]) {
      REXLOG_ERROR(gpu,
                   "D3D12RenderTargetCache: Failed to create the 4-sample host depth "
                   "storing pipeline");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[std::to_underlying(xenos::MSAASamples::k4X)]->SetName(
        L"Host Depth Store 4xMSAA");

    transfer_vertex_buffer_pool_ = std::make_unique<gpu::d3d12::D3D12UploadBufferPool>(
        provider, std::max(gpu::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                           sizeof(f32) * 2 * 6 * Transfer::kMaxCutoutBorderRectangles *
                               xenos::kEDRAMTileCount));

    D3D12_DESCRIPTOR_RANGE transfer_root_color_srv_range;
    transfer_root_color_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    transfer_root_color_srv_range.NumDescriptors = 1;
    transfer_root_color_srv_range.BaseShaderRegister = kTransferSRVRegisterColor;
    transfer_root_color_srv_range.RegisterSpace = 0;
    transfer_root_color_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE transfer_root_depth_srv_range;
    transfer_root_depth_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    transfer_root_depth_srv_range.NumDescriptors = 1;
    transfer_root_depth_srv_range.BaseShaderRegister = kTransferSRVRegisterDepth;
    transfer_root_depth_srv_range.RegisterSpace = 0;
    transfer_root_depth_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE transfer_root_stencil_srv_range;
    transfer_root_stencil_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    transfer_root_stencil_srv_range.NumDescriptors = 1;
    transfer_root_stencil_srv_range.BaseShaderRegister = kTransferSRVRegisterStencil;
    transfer_root_stencil_srv_range.RegisterSpace = 0;
    transfer_root_stencil_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE transfer_root_host_depth_srv_range;
    transfer_root_host_depth_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    transfer_root_host_depth_srv_range.NumDescriptors = 1;
    transfer_root_host_depth_srv_range.BaseShaderRegister = kTransferSRVRegisterHostDepth;
    transfer_root_host_depth_srv_range.RegisterSpace = 0;
    transfer_root_host_depth_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER
    transfer_root_parameters[kTransferUsedRootParameterCount];
    D3D12_ROOT_SIGNATURE_DESC transfer_root_desc;
    transfer_root_desc.pParameters = transfer_root_parameters;
    transfer_root_desc.NumStaticSamplers = 0;
    transfer_root_desc.pStaticSamplers = nullptr;
    transfer_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    for (size_t i = 0; i < size_t(TransferRootSignatureIndex::kCount); ++i) {
      u32 transfer_root_mask = kTransferUsedRootParameters[i];
      if (transfer_root_mask & kTransferUsedRootParameterStencilMaskConstantBit) {
        D3D12_ROOT_PARAMETER& transfer_root_stencil_mask_constant =
            transfer_root_parameters[rex::BitCount(
                transfer_root_mask & (kTransferUsedRootParameterStencilMaskConstantBit - 1))];
        transfer_root_stencil_mask_constant.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transfer_root_stencil_mask_constant.Constants.ShaderRegister =
            kTransferCBVRegisterStencilMask;
        transfer_root_stencil_mask_constant.Constants.RegisterSpace = 0;
        transfer_root_stencil_mask_constant.Constants.Num32BitValues = 1;
        transfer_root_stencil_mask_constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      if (transfer_root_mask & kTransferUsedRootParameterColorSRVBit) {
        D3D12_ROOT_PARAMETER& transfer_root_color_srv = transfer_root_parameters[rex::BitCount(
            transfer_root_mask & (kTransferUsedRootParameterColorSRVBit - 1))];
        transfer_root_color_srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transfer_root_color_srv.DescriptorTable.NumDescriptorRanges = 1;
        transfer_root_color_srv.DescriptorTable.pDescriptorRanges = &transfer_root_color_srv_range;
        transfer_root_color_srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      if (transfer_root_mask & kTransferUsedRootParameterDepthSRVBit) {
        D3D12_ROOT_PARAMETER& transfer_root_depth_srv = transfer_root_parameters[rex::BitCount(
            transfer_root_mask & (kTransferUsedRootParameterDepthSRVBit - 1))];
        transfer_root_depth_srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transfer_root_depth_srv.DescriptorTable.NumDescriptorRanges = 1;
        transfer_root_depth_srv.DescriptorTable.pDescriptorRanges = &transfer_root_depth_srv_range;
        transfer_root_depth_srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      if (transfer_root_mask & kTransferUsedRootParameterStencilSRVBit) {
        D3D12_ROOT_PARAMETER& transfer_root_stencil_srv = transfer_root_parameters[rex::BitCount(
            transfer_root_mask & (kTransferUsedRootParameterStencilSRVBit - 1))];
        transfer_root_stencil_srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transfer_root_stencil_srv.DescriptorTable.NumDescriptorRanges = 1;
        transfer_root_stencil_srv.DescriptorTable.pDescriptorRanges =
            &transfer_root_stencil_srv_range;
        transfer_root_stencil_srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      if (transfer_root_mask & kTransferUsedRootParameterAddressConstantBit) {
        D3D12_ROOT_PARAMETER& transfer_root_address_constant =
            transfer_root_parameters[rex::BitCount(
                transfer_root_mask & (kTransferUsedRootParameterAddressConstantBit - 1))];
        transfer_root_address_constant.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transfer_root_address_constant.Constants.ShaderRegister = kTransferCBVRegisterAddress;
        transfer_root_address_constant.Constants.RegisterSpace = 0;
        transfer_root_address_constant.Constants.Num32BitValues =
            sizeof(TransferAddressConstant) / sizeof(u32);
        transfer_root_address_constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      if (transfer_root_mask & kTransferUsedRootParameterHostDepthSRVBit) {
        D3D12_ROOT_PARAMETER& transfer_root_host_depth_srv = transfer_root_parameters[rex::BitCount(
            transfer_root_mask & (kTransferUsedRootParameterHostDepthSRVBit - 1))];
        transfer_root_host_depth_srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transfer_root_host_depth_srv.DescriptorTable.NumDescriptorRanges = 1;
        transfer_root_host_depth_srv.DescriptorTable.pDescriptorRanges =
            &transfer_root_host_depth_srv_range;
        transfer_root_host_depth_srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      if (transfer_root_mask & kTransferUsedRootParameterHostDepthAddressConstantBit) {
        D3D12_ROOT_PARAMETER& transfer_root_host_address_constant =
            transfer_root_parameters[rex::BitCount(
                transfer_root_mask & (kTransferUsedRootParameterHostDepthAddressConstantBit - 1))];
        transfer_root_host_address_constant.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transfer_root_host_address_constant.Constants.ShaderRegister =
            kTransferCBVRegisterHostDepthAddress;
        transfer_root_host_address_constant.Constants.RegisterSpace = 0;
        transfer_root_host_address_constant.Constants.Num32BitValues =
            sizeof(TransferAddressConstant) / sizeof(u32);
        transfer_root_host_address_constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      transfer_root_desc.NumParameters = rex::BitCount(transfer_root_mask);
      REX_ASSERT(transfer_root_desc.NumParameters <= kTransferUsedRootParameterCount);
      transfer_root_signatures_[i] =
          gpu::d3d12::objects::CreateRootSignature(provider, transfer_root_desc);
      if (!transfer_root_signatures_[i]) {
        REXLOG_ERROR(gpu,
                     "D3D12RenderTargetCache: Failed to create the render target "
                     "ownership transfer root signature {:X}",
                     transfer_root_mask);
        Shutdown();
        return false;
      }
    }

    D3D12_DESCRIPTOR_RANGE dump_root_source_range;
    dump_root_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    dump_root_source_range.NumDescriptors = 1;
    dump_root_source_range.BaseShaderRegister = 0;
    dump_root_source_range.RegisterSpace = 0;
    dump_root_source_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE dump_root_stencil_range;
    dump_root_stencil_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    dump_root_stencil_range.NumDescriptors = 1;
    dump_root_stencil_range.BaseShaderRegister = 1;
    dump_root_stencil_range.RegisterSpace = 0;
    dump_root_stencil_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE dump_root_edram_range;
    dump_root_edram_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    dump_root_edram_range.NumDescriptors = 1;
    dump_root_edram_range.BaseShaderRegister = 0;
    dump_root_edram_range.RegisterSpace = 0;
    dump_root_edram_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER
    dump_root_color_parameters[kDumpRootParameterColorCount];
    D3D12_ROOT_PARAMETER
    dump_root_depth_parameters[kDumpRootParameterDepthCount];
    for (u32 i = 0; i < 2; ++i) {
      D3D12_ROOT_PARAMETER& dump_root_offsets =
          i ? dump_root_depth_parameters[kDumpRootParameterOffsets]
            : dump_root_color_parameters[kDumpRootParameterOffsets];
      dump_root_offsets.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      dump_root_offsets.Constants.ShaderRegister = kDumpCbufferOffsets;
      dump_root_offsets.Constants.RegisterSpace = 0;
      dump_root_offsets.Constants.Num32BitValues = sizeof(DumpOffsets) / sizeof(u32);
      dump_root_offsets.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_ROOT_PARAMETER& dump_root_source =
          i ? dump_root_depth_parameters[kDumpRootParameterSource]
            : dump_root_color_parameters[kDumpRootParameterSource];
      dump_root_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      dump_root_source.DescriptorTable.NumDescriptorRanges = 1;
      dump_root_source.DescriptorTable.pDescriptorRanges = &dump_root_source_range;
      dump_root_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      if (i) {
        D3D12_ROOT_PARAMETER& dump_root_stencil =
            dump_root_depth_parameters[kDumpRootParameterDepthStencil];
        dump_root_stencil.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        dump_root_stencil.DescriptorTable.NumDescriptorRanges = 1;
        dump_root_stencil.DescriptorTable.pDescriptorRanges = &dump_root_stencil_range;
        dump_root_stencil.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      }
      D3D12_ROOT_PARAMETER& dump_root_pitches =
          i ? dump_root_depth_parameters[kDumpRootParameterDepthPitches]
            : dump_root_color_parameters[kDumpRootParameterColorPitches];
      dump_root_pitches.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      dump_root_pitches.Constants.ShaderRegister = kDumpCbufferPitches;
      dump_root_pitches.Constants.RegisterSpace = 0;
      dump_root_pitches.Constants.Num32BitValues = sizeof(DumpPitches) / sizeof(u32);
      dump_root_pitches.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_ROOT_PARAMETER& dump_root_edram =
          i ? dump_root_depth_parameters[kDumpRootParameterDepthEDRAM]
            : dump_root_color_parameters[kDumpRootParameterColorEDRAM];
      dump_root_edram.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      dump_root_edram.DescriptorTable.NumDescriptorRanges = 1;
      dump_root_edram.DescriptorTable.pDescriptorRanges = &dump_root_edram_range;
      dump_root_edram.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC dump_root_desc;
    dump_root_desc.NumParameters = UINT(rex::CountOf(dump_root_color_parameters));
    dump_root_desc.pParameters = dump_root_color_parameters;
    dump_root_desc.NumStaticSamplers = 0;
    dump_root_desc.pStaticSamplers = nullptr;
    dump_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    dump_root_signature_color_ = gpu::d3d12::objects::CreateRootSignature(provider, dump_root_desc);
    if (!dump_root_signature_color_) {
      REXLOG_ERROR(gpu,
                   "D3D12RenderTargetCache: Failed to create the color render target "
                   "dumping root signature");
      Shutdown();
      return false;
    }
    dump_root_desc.NumParameters = UINT(rex::CountOf(dump_root_depth_parameters));
    dump_root_desc.pParameters = dump_root_depth_parameters;
    dump_root_signature_depth_ = gpu::d3d12::objects::CreateRootSignature(provider, dump_root_desc);
    if (!dump_root_signature_depth_) {
      REXLOG_ERROR(gpu,
                   "D3D12RenderTargetCache: Failed to create the depth render target "
                   "dumping root signature");
      Shutdown();
      return false;
    }

    D3D12_ROOT_PARAMETER uint32_rtv_clear_root_constants;
    uint32_rtv_clear_root_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    uint32_rtv_clear_root_constants.Constants.ShaderRegister = 0;
    uint32_rtv_clear_root_constants.Constants.RegisterSpace = 0;
    uint32_rtv_clear_root_constants.Constants.Num32BitValues = 2;
    uint32_rtv_clear_root_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC uint32_rtv_clear_root_desc;
    uint32_rtv_clear_root_desc.NumParameters = 1;
    uint32_rtv_clear_root_desc.pParameters = &uint32_rtv_clear_root_constants;
    uint32_rtv_clear_root_desc.NumStaticSamplers = 0;
    uint32_rtv_clear_root_desc.pStaticSamplers = nullptr;
    uint32_rtv_clear_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    uint32_rtv_clear_root_signature_ =
        gpu::d3d12::objects::CreateRootSignature(provider, uint32_rtv_clear_root_desc);
    if (!uint32_rtv_clear_root_signature_) {
      REXLOG_ERROR(gpu,
                   "D3D12RenderTargetCache: Failed to create the k_32_FLOAT / "
                   "k_32_32_FLOAT render target clearing root signature");
      Shutdown();
      return false;
    }
    auto uint32_rtv_clear_pipeline_desc = objects::BuildGraphicsPipelineStateDesc();
    uint32_rtv_clear_pipeline_desc.pRootSignature = uint32_rtv_clear_root_signature_;
    uint32_rtv_clear_pipeline_desc.VS.pShaderBytecode = shaders::kFullscreenCWVS;
    uint32_rtv_clear_pipeline_desc.VS.BytecodeLength = sizeof(shaders::kFullscreenCWVS);
    uint32_rtv_clear_pipeline_desc.PS.pShaderBytecode = shaders::kClearUInt2PS;
    uint32_rtv_clear_pipeline_desc.PS.BytecodeLength = sizeof(shaders::kClearUInt2PS);
    uint32_rtv_clear_pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    uint32_rtv_clear_pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    uint32_rtv_clear_pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    uint32_rtv_clear_pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
    uint32_rtv_clear_pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    uint32_rtv_clear_pipeline_desc.NumRenderTargets = 1;
    for (size_t i = 0; i < 2; ++i) {
      uint32_rtv_clear_pipeline_desc.RTVFormats[0] =
          GetColorOwnershipTransferDXGIFormat(i ? xenos::ColorRenderTargetFormat::k_32_32_FLOAT
                                                : xenos::ColorRenderTargetFormat::k_32_FLOAT);
      for (size_t j = static_cast<size_t>(xenos::MSAASamples::k1X);
           j <= static_cast<size_t>(xenos::MSAASamples::k4X); ++j) {
        if (xenos::MSAASamples(j) == xenos::MSAASamples::k2X && !msaa_2x_supported_) {
          // Emulated 2x uses samples 0 and 3 of 4x as its samples 0 and 1.
          uint32_rtv_clear_pipeline_desc.SampleMask = 0b1001;
          uint32_rtv_clear_pipeline_desc.SampleDesc.Count = 4;
        } else {
          uint32_rtv_clear_pipeline_desc.SampleMask = UINT_MAX;
          uint32_rtv_clear_pipeline_desc.SampleDesc.Count = 1 << j;
        }
        ID3D12PipelineState* uint32_rtv_clear_pipeline;
        if (FAILED(device->CreateGraphicsPipelineState(&uint32_rtv_clear_pipeline_desc,
                                                       IID_PPV_ARGS(&uint32_rtv_clear_pipeline)))) {
          REXLOG_ERROR(gpu,
                       "D3D12RenderTargetCache: Failed to create the {} {}-sample "
                       "render target clearing pipeline",
                       i ? "k_32_32_FLOAT" : "k_32_FLOAT", static_cast<u32>(1) << j);
          Shutdown();
          return false;
        }
        uint32_rtv_clear_pipelines_[i][j] = uint32_rtv_clear_pipeline;
        auto uint32_rtv_clear_pipeline_name =
            rex::ToUTF16(std::format("Resolve Clear {} {}xMSAA", i ? "k_32_32_FLOAT" : "k_32_FLOAT",
                                     static_cast<u32>(1) << j));
        uint32_rtv_clear_pipeline->SetName(
            reinterpret_cast<LPCWSTR>(uint32_rtv_clear_pipeline_name.c_str()));
      }
    }

    // The FXC compiled dump shader is about 2 KB, so 1024 words (4 KB) leave headroom.
    built_shader_.reserve(1024);
  } else if (path_ == Path::kPixelShaderInterlock) {
    // Blending is done in linear space directly in shaders.
    gamma_render_target_as_unorm16_ = false;

    // Always true float24 depth rounded to the nearest even.
    depth_float24_round_ = true;
    depth_float24_convert_in_pixel_shader_ = true;

    // Only ForcedSampleCount, which doesn't support 2x.
    msaa_2x_supported_ = false;

    std::array<D3D12_ROOT_PARAMETER, 2> resolve_rov_clear_root_parameters;
    resolve_rov_clear_root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    resolve_rov_clear_root_parameters[0].Constants.ShaderRegister = 0;
    resolve_rov_clear_root_parameters[0].Constants.RegisterSpace = 0;
    resolve_rov_clear_root_parameters[0].Constants.Num32BitValues =
        sizeof(draw_state::ResolveClearShaderConstants) / sizeof(u32);
    resolve_rov_clear_root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE resolve_rov_clear_dest_range;
    resolve_rov_clear_dest_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    resolve_rov_clear_dest_range.NumDescriptors = 1;
    resolve_rov_clear_dest_range.BaseShaderRegister = 0;
    resolve_rov_clear_dest_range.RegisterSpace = 0;
    resolve_rov_clear_dest_range.OffsetInDescriptorsFromTableStart = 0;
    resolve_rov_clear_root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    resolve_rov_clear_root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    resolve_rov_clear_root_parameters[1].DescriptorTable.pDescriptorRanges =
        &resolve_rov_clear_dest_range;
    resolve_rov_clear_root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC resolve_rov_clear_root_signature_desc;
    resolve_rov_clear_root_signature_desc.NumParameters =
        UINT(resolve_rov_clear_root_parameters.size());
    resolve_rov_clear_root_signature_desc.pParameters = resolve_rov_clear_root_parameters.data();
    resolve_rov_clear_root_signature_desc.NumStaticSamplers = 0;
    resolve_rov_clear_root_signature_desc.pStaticSamplers = nullptr;
    resolve_rov_clear_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    resolve_rov_clear_root_signature_ =
        gpu::d3d12::objects::CreateRootSignature(provider, resolve_rov_clear_root_signature_desc);
    if (resolve_rov_clear_root_signature_ == nullptr) {
      REXLOG_ERROR(gpu,
                   "D3D12RenderTargetCache: Failed to create the resolve EDRAM buffer "
                   "clear root signature");
      Shutdown();
      return false;
    }

    resolve_rov_clear_32bpp_pipeline_ = gpu::d3d12::objects::CreateComputePipeline(
        device,
        draw_resolution_scaled ? shaders::kResolveClear32BPPScaledCS
                               : shaders::kResolveClear32BPPCS,
        draw_resolution_scaled ? sizeof(shaders::kResolveClear32BPPScaledCS)
                               : sizeof(shaders::kResolveClear32BPPCS),
        resolve_rov_clear_root_signature_);
    if (resolve_rov_clear_32bpp_pipeline_ == nullptr) {
      REXLOG_ERROR(gpu,
                   "D3D12RenderTargetCache: Failed to create the 32bpp resolve EDRAM "
                   "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_rov_clear_32bpp_pipeline_->SetName(L"Resolve Clear 32bpp");
    resolve_rov_clear_64bpp_pipeline_ = gpu::d3d12::objects::CreateComputePipeline(
        device,
        draw_resolution_scaled ? shaders::kResolveClear64BPPScaledCS
                               : shaders::kResolveClear64BPPCS,
        draw_resolution_scaled ? sizeof(shaders::kResolveClear64BPPScaledCS)
                               : sizeof(shaders::kResolveClear64BPPCS),
        resolve_rov_clear_root_signature_);
    if (resolve_rov_clear_64bpp_pipeline_ == nullptr) {
      REXLOG_ERROR(gpu,
                   "D3D12RenderTargetCache: Failed to create the 64bpp resolve EDRAM "
                   "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_rov_clear_64bpp_pipeline_->SetName(L"Resolve Clear 64bpp");
  } else {
    REX_UNHANDLED_CASE(path_);
    Shutdown();
    return false;
  }

  InitializeCommon();

  return true;
}

void D3D12RenderTargetCache::Shutdown(bool from_destructor) {
  gpu::d3d12::objects::ReleaseAndNull(resolve_rov_clear_64bpp_pipeline_);
  gpu::d3d12::objects::ReleaseAndNull(resolve_rov_clear_32bpp_pipeline_);
  gpu::d3d12::objects::ReleaseAndNull(resolve_rov_clear_root_signature_);

  for (size_t i = 0; i < 2; ++i) {
    for (size_t j = static_cast<size_t>(xenos::MSAASamples::k1X);
         j <= static_cast<size_t>(xenos::MSAASamples::k4X); ++j) {
      gpu::d3d12::objects::ReleaseAndNull(uint32_rtv_clear_pipelines_[i][j]);
    }
  }
  gpu::d3d12::objects::ReleaseAndNull(uint32_rtv_clear_root_signature_);

  for (const auto& dump_pipeline_pair : dump_pipelines_) {
    if (dump_pipeline_pair.second) {
      dump_pipeline_pair.second->Release();
    }
  }
  dump_pipelines_.clear();
  for (const auto& direct_resolve_pipeline_pair : direct_resolve_pipelines_) {
    bool aliased_resolve_copy_pipeline = false;
    for (ID3D12PipelineState* resolve_copy_pipeline : resolve_copy_pipelines_) {
      if (direct_resolve_pipeline_pair.second == resolve_copy_pipeline) {
        aliased_resolve_copy_pipeline = true;
        break;
      }
    }
    if (direct_resolve_pipeline_pair.second && !aliased_resolve_copy_pipeline) {
      direct_resolve_pipeline_pair.second->Release();
    }
  }
  direct_resolve_pipelines_.clear();
  gpu::d3d12::objects::ReleaseAndNull(direct_resolve_root_signature_depth_);
  gpu::d3d12::objects::ReleaseAndNull(direct_resolve_root_signature_color_);
  gpu::d3d12::objects::ReleaseAndNull(dump_root_signature_depth_);
  gpu::d3d12::objects::ReleaseAndNull(dump_root_signature_color_);

  for (const auto& transfer_pipeline_array_pair : transfer_stencil_bit_pipelines_) {
    for (ID3D12PipelineState* transfer_pipeline : transfer_pipeline_array_pair.second) {
      if (transfer_pipeline) {
        transfer_pipeline->Release();
      }
    }
  }
  transfer_stencil_bit_pipelines_.clear();
  for (const auto& transfer_pipeline_pair : transfer_pipelines_) {
    if (transfer_pipeline_pair.second) {
      transfer_pipeline_pair.second->Release();
    }
  }
  transfer_pipelines_.clear();
  for (auto& root_signature : transfer_root_signatures_) {
    gpu::d3d12::objects::ReleaseAndNull(root_signature);
  }

  transfer_vertex_buffer_pool_.reset();

  for (auto& pipeline : host_depth_store_pipelines_) {
    gpu::d3d12::objects::ReleaseAndNull(pipeline);
  }
  gpu::d3d12::objects::ReleaseAndNull(host_depth_store_root_signature_);

  null_rtv_descriptor_ms_.Free();
  null_rtv_descriptor_ss_.Free();
  descriptor_pool_srv_.reset();
  descriptor_pool_depth_.reset();
  descriptor_pool_color_.reset();

  for (auto& pipeline : resolve_copy_pipelines_) {
    gpu::d3d12::objects::ReleaseAndNull(pipeline);
  }
  gpu::d3d12::objects::ReleaseAndNull(resolve_copy_root_signature_);

  gpu::d3d12::objects::ReleaseAndNull(edram_buffer_descriptor_heap_);
  gpu::d3d12::objects::ReleaseAndNull(edram_buffer_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12RenderTargetCache::CompletedSubmissionUpdated() {
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  }
}

void D3D12RenderTargetCache::BeginSubmission() {
  InvalidateCommandListRenderTargets();
  // ExecuteCommandLists is a full UAV barrier.
  if (edram_buffer_modification_status_ != EDRAMBufferModificationStatus::kUnmodified) {
    REX_ASSERT(edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    edram_buffer_modification_status_ = EDRAMBufferModificationStatus::kUnmodified;
    PixelShaderInterlockFullEDRAMBarrierPlaced();
  }
}

bool D3D12RenderTargetCache::Update(bool is_rasterization_done,
                                    xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
                                    u32 normalized_color_mask, const Shader& vertex_shader) {
  if (!RenderTargetCache::Update(is_rasterization_done, normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    return false;
  }
  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      RenderTarget* const* depth_and_color_render_targets = GetLastUpdateAccumulatedRenderTargets();
      PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                       depth_and_color_render_targets, GetLastUpdateTransfers());
      SetCommandListRenderTargets(depth_and_color_render_targets);
    } break;
    case Path::kPixelShaderInterlock: {
      TransitionEDRAMBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      // UAV writes such as clears are not synchronized with ROV accesses, so commit them first.
      CommitEDRAMBufferUAVWrites(EDRAMBufferModificationStatus::kAsUAV);
      MarkEDRAMBufferModified(EDRAMBufferModificationStatus::kAsROV);
    } break;
    default:
      REX_UNHANDLED_CASE(GetPath());
      return false;
  }
  return true;
}

void D3D12RenderTargetCache::WriteEDRAMRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    std::to_underlying(EDRAMBufferDescriptorIndex::kRawSRV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RenderTargetCache::WriteEDRAMRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    std::to_underlying(EDRAMBufferDescriptorIndex::kRawUAV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RenderTargetCache::WriteEDRAMUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                             u32 element_size_bytes_pow2) {
  EDRAMBufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = EDRAMBufferDescriptorIndex::kR32UintSRV;
      break;
    case 3:
      descriptor_index = EDRAMBufferDescriptorIndex::kR32G32UintSRV;
      break;
    case 4:
      descriptor_index = EDRAMBufferDescriptorIndex::kR32G32B32A32UintSRV;
      break;
    default:
      REX_UNHANDLED_CASE(element_size_bytes_pow2);
      return;
  }
  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(1, handle,
                                provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                                              static_cast<u32>(descriptor_index)),
                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RenderTargetCache::WriteEDRAMUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                             u32 element_size_bytes_pow2) {
  EDRAMBufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = EDRAMBufferDescriptorIndex::kR32UintUAV;
      break;
    case 3:
      descriptor_index = EDRAMBufferDescriptorIndex::kR32G32UintUAV;
      break;
    case 4:
      descriptor_index = EDRAMBufferDescriptorIndex::kR32G32B32A32UintUAV;
      break;
    default:
      REX_UNHANDLED_CASE(element_size_bytes_pow2);
      return;
  }
  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(1, handle,
                                provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                                              static_cast<u32>(descriptor_index)),
                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool D3D12RenderTargetCache::Resolve(const runtime::Memory& memory,
                                     D3D12SharedMemory& shared_memory,
                                     D3D12TextureCache& texture_cache, u32& written_address_out,
                                     u32& written_length_out) {
  written_address_out = 0;
  written_length_out = 0;

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  draw_state::ResolveInfo resolve_info;
  bool fixed_16_truncated_to_minus_1_to_1 = IsFixed16TruncatedToMinus1To1();
  if (!draw_state::GetResolveInfo(GetRegisterFile(), memory, GetDrawResolutionScaleX(),
                                  GetDrawResolutionScaleY(), fixed_16_truncated_to_minus_1_to_1,
                                  fixed_16_truncated_to_minus_1_to_1, resolve_info)) {
    return false;
  }

  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }

  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();

  bool copied = false;
  if (resolve_info.copy_dest_extent_length) {
    draw_state::ResolveCopyShaderConstants copy_shader_constants;
    u32 copy_group_count_x, copy_group_count_y;
    draw_state::ResolveCopyShaderIndex copy_shader =
        resolve_info.GetCopyShader(GetDrawResolutionScaleX(), GetDrawResolutionScaleY(),
                                   copy_shader_constants, copy_group_count_x, copy_group_count_y);
    REX_ASSERT(copy_group_count_x && copy_group_count_y);
    if (copy_shader != draw_state::ResolveCopyShaderIndex::kUnknown) {
      const draw_state::ResolveCopyShaderInfo& copy_shader_info =
          draw_state::kResolveCopyShaderInfo[static_cast<size_t>(copy_shader)];
      bool direct_resolved = false;
      if (GetPath() == Path::kHostRenderTargets) {
        if (REXCVAR_GET(::rex::gpu::cvars::direct_host_resolve)) {
          direct_resolved =
              TryResolveCopyDirectly(resolve_info, copy_shader, draw_resolution_scaled);
          if (direct_resolved) {
            ++direct_resolve_success_count_;
          } else {
            ++direct_resolve_fallback_count_;
          }
        }
        if (!direct_resolved) {
          u32 dump_base;
          u32 dump_row_length_used;
          u32 dump_rows;
          u32 dump_pitch;
          resolve_info.GetCopyEDRAMTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
          if (!DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch)) {
            REX_FAIL(gpu, false,
                     "D3D12RenderTargetCache: Failed to dump host render targets for resolve");
          }
        }
      }

      bool copy_dest_committed;
      if (draw_resolution_scaled) {
        // Commits from the extent start, while the bound buffer begins at the base.
        copy_dest_committed =
            texture_cache.EnsureScaledResolveMemoryCommitted(
                resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length) &&
            texture_cache.UpdateScaledResolveRange(resolve_info.copy_dest_base,
                                                   resolve_info.copy_dest_extent_start -
                                                       resolve_info.copy_dest_base +
                                                       resolve_info.copy_dest_extent_length);
      } else {
        copy_dest_committed = shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                                         resolve_info.copy_dest_extent_length);
      }
      if (copy_dest_committed) {
        gpu::d3d12::objects::DescriptorCPUGPUHandlePair descriptor_dest;
        gpu::d3d12::objects::DescriptorCPUGPUHandlePair descriptor_source;
        gpu::d3d12::objects::DescriptorCPUGPUHandlePair descriptors[2];
        if (command_processor_.RequestOneUseSingleViewDescriptors(
                bindless_resources_used_ ? static_cast<u32>(draw_resolution_scaled) : 2,
                descriptors)) {
          if (bindless_resources_used_) {
            if (draw_resolution_scaled) {
              descriptor_dest = descriptors[0];
            } else {
              descriptor_dest = command_processor_.GetSharedMemoryUintPow2BindlessUAVHandlePair(
                  copy_shader_info.dest_bpe_log2);
            }
            if (copy_shader_info.source_is_raw) {
              descriptor_source = command_processor_.GetSystemBindlessViewHandlePair(
                  D3D12CommandProcessor::SystemBindlessView::kEDRAMRawSRV);
            } else {
              descriptor_source = command_processor_.GetEDRAMUintPow2BindlessSRVHandlePair(
                  copy_shader_info.source_bpe_log2);
            }
          } else {
            descriptor_dest = descriptors[0];
            if (!draw_resolution_scaled) {
              shared_memory.WriteUintPow2UAVDescriptor(descriptor_dest.first,
                                                       copy_shader_info.dest_bpe_log2);
            }
            descriptor_source = descriptors[1];
            if (copy_shader_info.source_is_raw) {
              WriteEDRAMRawSRVDescriptor(descriptor_source.first);
            } else {
              WriteEDRAMUintPow2SRVDescriptor(descriptor_source.first,
                                              copy_shader_info.source_bpe_log2);
            }
          }
          if (draw_resolution_scaled) {
            texture_cache.CreateCurrentScaledResolveRangeUintPow2UAV(
                descriptor_dest.first, copy_shader_info.dest_bpe_log2);
            texture_cache.TransitionCurrentScaledResolveRange(
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          } else {
            shared_memory.UseForWriting();
          }
          TransitionEDRAMBuffer(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

          command_list.D3DSetComputeRootSignature(resolve_copy_root_signature_);
          command_list.D3DSetComputeRootDescriptorTable(2, descriptor_source.second);
          command_list.D3DSetComputeRootDescriptorTable(1, descriptor_dest.second);
          if (draw_resolution_scaled) {
            command_list.D3DSetComputeRoot32BitConstants(
                0, sizeof(copy_shader_constants.dest_relative) / sizeof(u32),
                &copy_shader_constants.dest_relative, 0);
          } else {
            command_list.D3DSetComputeRoot32BitConstants(
                0, sizeof(copy_shader_constants) / sizeof(u32), &copy_shader_constants, 0);
          }
          command_processor_.SetExternalPipeline(
              resolve_copy_pipelines_[static_cast<size_t>(copy_shader)]);
          command_processor_.SubmitBarriers();
          command_list.D3DDispatch(copy_group_count_x, copy_group_count_y, 1);

          if (draw_resolution_scaled) {
            texture_cache.MarkCurrentScaledResolveRangeUAVWritesCommitNeeded();
          } else {
            shared_memory.MarkUAVWritesCommitNeeded();
          }

          texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                            resolve_info.copy_dest_extent_length);
          written_address_out = resolve_info.copy_dest_extent_start;
          written_length_out = resolve_info.copy_dest_extent_length;
          copied = true;
        }
      } else {
        REXLOG_ERROR(gpu,
                     "D3D12RenderTargetCache: Failed to obtain the resolve destination "
                     "memory region");
      }
    }
  } else {
    copied = true;
  }

  bool cleared = false;
  bool clear_depth = resolve_info.IsClearingDepth();
  bool clear_color = resolve_info.IsClearingColor();
  if (clear_depth || clear_color) {
    switch (GetPath()) {
      case Path::kHostRenderTargets: {
        Transfer::Rectangle clear_rectangle;
        RenderTarget* clear_render_targets[2];
        // A false result is an empty region (success) or an error, both ignored.
        if (PrepareHostRenderTargetsResolveClear(resolve_info, clear_rectangle,
                                                 clear_render_targets[0], clear_transfers_[0],
                                                 clear_render_targets[1], clear_transfers_[1])) {
          u64 clear_values[2];
          clear_values[0] = resolve_info.rb_depth_clear;
          clear_values[1] = resolve_info.rb_color_clear |
                            (static_cast<u64>(resolve_info.rb_color_clear_lo) << 32);
          PerformTransfersAndResolveClears(2, clear_render_targets, clear_transfers_, clear_values,
                                           &clear_rectangle);
        }
        cleared = true;
      } break;
      case Path::kPixelShaderInterlock: {
        gpu::d3d12::objects::DescriptorCPUGPUHandlePair descriptor_edram;
        bool descriptor_edram_obtained;
        if (bindless_resources_used_) {
          descriptor_edram = command_processor_.GetSystemBindlessViewHandlePair(
              D3D12CommandProcessor::SystemBindlessView ::kEDRAMR32G32B32A32UintUAV);
          descriptor_edram_obtained = true;
        } else {
          descriptor_edram_obtained =
              command_processor_.RequestOneUseSingleViewDescriptors(1, &descriptor_edram);
          if (descriptor_edram_obtained) {
            WriteEDRAMUintPow2UAVDescriptor(descriptor_edram.first, 4);
          }
        }
        if (descriptor_edram_obtained) {
          TransitionEDRAMBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          // The depth and color ranges are assumed not to overlap, so committing once is enough.
          CommitEDRAMBufferUAVWrites();
          command_list.D3DSetComputeRootSignature(resolve_rov_clear_root_signature_);
          command_list.D3DSetComputeRootDescriptorTable(1, descriptor_edram.second);
          std::pair<u32, u32> clear_group_count = resolve_info.GetClearShaderGroupCount(
              GetDrawResolutionScaleX(), GetDrawResolutionScaleY());
          REX_ASSERT(clear_group_count.first && clear_group_count.second);
          if (clear_depth) {
            draw_state::ResolveClearShaderConstants depth_clear_constants;
            resolve_info.GetDepthClearShaderConstants(depth_clear_constants);
            command_list.D3DSetComputeRoot32BitConstants(
                0, sizeof(depth_clear_constants) / sizeof(u32), &depth_clear_constants, 0);
            command_processor_.SetExternalPipeline(resolve_rov_clear_32bpp_pipeline_);
            command_processor_.SubmitBarriers();
            command_list.D3DDispatch(clear_group_count.first, clear_group_count.second, 1);
          }
          if (clear_color) {
            draw_state::ResolveClearShaderConstants color_clear_constants;
            resolve_info.GetColorClearShaderConstants(color_clear_constants);
            if (clear_depth) {
              // The depth clear already set the constants that are not render target specific.
              command_list.D3DSetComputeRoot32BitConstants(
                  0, sizeof(color_clear_constants.rt_specific) / sizeof(u32),
                  &color_clear_constants.rt_specific,
                  offsetof(draw_state::ResolveClearShaderConstants, rt_specific) / sizeof(u32));
            } else {
              command_list.D3DSetComputeRoot32BitConstants(
                  0, sizeof(color_clear_constants) / sizeof(u32), &color_clear_constants, 0);
            }
            command_processor_.SetExternalPipeline(resolve_info.color_edram_info.format_is_64bpp
                                                       ? resolve_rov_clear_64bpp_pipeline_
                                                       : resolve_rov_clear_32bpp_pipeline_);
            command_processor_.SubmitBarriers();
            command_list.D3DDispatch(clear_group_count.first, clear_group_count.second, 1);
          }
          MarkEDRAMBufferModified();
          cleared = true;
        }
      } break;
      default:
        REX_UNHANDLED_CASE(GetPath());
    }
  } else {
    cleared = true;
  }

  return copied && cleared;
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorResourceDXGIFormat(
    xenos::ColorRenderTargetFormat format) const {
  // Typed is preferred over typeless for more compression opportunities.
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return gamma_render_target_as_unorm16_ ? DXGI_FORMAT_R16G16B16A16_UNORM
                                             : DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    // SNORM has two representations of -1.
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    // Floating point formats stay typeless to propagate NaN during unmodified ownership transfers.
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_TYPELESS;
    default:
      REX_UNHANDLED_CASE(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorDrawDXGIFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DXGI_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_FLOAT;
    default:
      return GetColorResourceDXGIFormat(format);
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorOwnershipTransferDXGIFormat(
    xenos::ColorRenderTargetFormat format, bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_UINT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_UINT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_UINT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_UINT;
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorDrawDXGIFormat(format);
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthResourceDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_R24G8_TYPELESS;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
      REX_UNHANDLED_CASE(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthDSVDXGIFormat(xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
      REX_UNHANDLED_CASE(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthSRVDepthDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
      REX_UNHANDLED_CASE(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthSRVStencilDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    default:
      REX_UNHANDLED_CASE(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

std::unique_ptr<RenderTargetCache::RenderTarget> D3D12RenderTargetCache::CreateRenderTarget(
    RenderTargetKey key) {
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();

  D3D12_RESOURCE_DESC resource_desc;
  resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  resource_desc.Alignment = 0;
  resource_desc.Width = u64{key.GetWidth()} * GetDrawResolutionScaleX();
  resource_desc.Height =
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples) * GetDrawResolutionScaleY();
  resource_desc.DepthOrArraySize = 1;
  resource_desc.MipLevels = 1;
  if (key.is_depth) {
    resource_desc.Format = GetDepthResourceDXGIFormat(key.GetDepthFormat());
  } else {
    resource_desc.Format = GetColorResourceDXGIFormat(key.GetColorFormat());
  }
  REX_ASSERT(resource_desc.Format != DXGI_FORMAT_UNKNOWN);
  if (resource_desc.Format == DXGI_FORMAT_UNKNOWN) {
    REX_FAIL(gpu, nullptr, "D3D12RenderTargetCache: Unknown {} render target format {}",
             key.is_depth ? "depth" : "color", static_cast<u32>(key.resource_format));
  }
  if (key.msaa_samples == xenos::MSAASamples::k2X && !GetMSAA2xSupported()) {
    resource_desc.SampleDesc.Count = 4;
  } else {
    resource_desc.SampleDesc.Count = UINT(1) << UINT(key.msaa_samples);
  }
  resource_desc.SampleDesc.Quality = 0;
  resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  resource_desc.Flags = key.is_depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                     : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  // The first access is an ownership transfer into this render target or direct drawing.
  D3D12_RESOURCE_STATES resource_state =
      key.is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  D3D12_CLEAR_VALUE optimized_clear_value;
  if (key.is_depth) {
    optimized_clear_value.Format = GetDepthDSVDXGIFormat(key.GetDepthFormat());
    // Fixed point depth has 1 farthest, while floating point depth has 0 farthest.
    optimized_clear_value.DepthStencil.Depth =
        key.GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24S8 ? 1.0f : 0.0f;
    optimized_clear_value.DepthStencil.Stencil = 0;
  } else {
    optimized_clear_value.Format = GetColorDrawDXGIFormat(key.GetColorFormat());
    optimized_clear_value.Color[0] = 0.0f;
    optimized_clear_value.Color[1] = 0.0f;
    optimized_clear_value.Color[2] = 0.0f;
    optimized_clear_value.Color[3] = 0.0f;
  }
  // Created zeroed for determinism with compression and float24 / float32 depth mirroring.
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  if (FAILED(device->CreateCommittedResource(&gpu::d3d12::objects::kHeapPropertiesDefault,
                                             D3D12_HEAP_FLAG_NONE, &resource_desc, resource_state,
                                             &optimized_clear_value, IID_PPV_ARGS(&resource)))) {
    return nullptr;
  }
  {
    std::u16string resource_name = rex::ToUTF16(key.GetDebugName());
    resource->SetName(reinterpret_cast<LPCWSTR>(resource_name.c_str()));
  }

  gpu::d3d12::D3D12CPUDescriptorPool& descriptor_pool =
      key.is_depth ? *descriptor_pool_depth_ : *descriptor_pool_color_;
  gpu::d3d12::D3D12CPUDescriptorPool::Descriptor descriptor_draw =
      descriptor_pool.AllocateDescriptor();
  gpu::d3d12::D3D12CPUDescriptorPool::Descriptor descriptor_srv =
      descriptor_pool_srv_->AllocateDescriptor();
  if (!descriptor_draw.IsValid() || !descriptor_srv.IsValid()) {
    return nullptr;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE descriptor_draw_handle = descriptor_draw.GetHandle();
  gpu::d3d12::D3D12CPUDescriptorPool::Descriptor descriptor_load_separate;
  gpu::d3d12::D3D12CPUDescriptorPool::Descriptor descriptor_srv_stencil;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
  srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (resource_desc.SampleDesc.Count > 1) {
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Texture2D.PlaneSlice = 0;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
  }
  if (key.is_depth) {
    descriptor_srv_stencil = descriptor_pool_srv_->AllocateDescriptor();
    if (!descriptor_srv_stencil.IsValid()) {
      return nullptr;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
    dsv_desc.Format = optimized_clear_value.Format;
    dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
    D3D12_SHADER_RESOURCE_VIEW_DESC stencil_srv_desc;
    stencil_srv_desc.Format = GetDepthSRVStencilDXGIFormat(key.GetDepthFormat());
    stencil_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (resource_desc.SampleDesc.Count > 1) {
      dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
      stencil_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
      dsv_desc.Texture2D.MipSlice = 0;
      stencil_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      stencil_srv_desc.Texture2D.MostDetailedMip = 0;
      stencil_srv_desc.Texture2D.MipLevels = 1;
      stencil_srv_desc.Texture2D.PlaneSlice = 1;
      stencil_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    }
    device->CreateDepthStencilView(resource.Get(), &dsv_desc, descriptor_draw_handle);
    device->CreateShaderResourceView(resource.Get(), &stencil_srv_desc,
                                     descriptor_srv_stencil.GetHandle());
    srv_desc.Format = GetDepthSRVDepthDXGIFormat(key.GetDepthFormat());
  } else {
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
    rtv_desc.Format = optimized_clear_value.Format;
    if (resource_desc.SampleDesc.Count > 1) {
      rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    } else {
      rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      rtv_desc.Texture2D.MipSlice = 0;
      rtv_desc.Texture2D.PlaneSlice = 0;
    }
    device->CreateRenderTargetView(resource.Get(), &rtv_desc, descriptor_draw_handle);
    DXGI_FORMAT load_format = GetColorOwnershipTransferDXGIFormat(key.GetColorFormat());
    if (rtv_desc.Format != load_format) {
      descriptor_load_separate = descriptor_pool.AllocateDescriptor();
      if (!descriptor_load_separate.IsValid()) {
        return nullptr;
      }
      rtv_desc.Format = load_format;
      device->CreateRenderTargetView(resource.Get(), &rtv_desc,
                                     descriptor_load_separate.GetHandle());
    }
    srv_desc.Format = load_format;
  }
  device->CreateShaderResourceView(resource.Get(), &srv_desc, descriptor_srv.GetHandle());

  return std::make_unique<D3D12RenderTarget>(
      key, resource.Get(), std::move(descriptor_draw), std::move(descriptor_load_separate),
      std::move(descriptor_srv), std::move(descriptor_srv_stencil), resource_state);
}

bool D3D12RenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24FS8) {
    return !depth_float24_convert_in_pixel_shader_;
  }
  return false;
}

bool D3D12RenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

void D3D12RenderTargetCache::RequestPixelShaderInterlockBarrier() {
  CommitEDRAMBufferUAVWrites();
}

void D3D12RenderTargetCache::TransitionEDRAMBuffer(D3D12_RESOURCE_STATES new_state) {
  if (command_processor_.PushTransitionBarrier(edram_buffer_, edram_buffer_state_, new_state)) {
    // Resets only when a barrier is inserted, as barriers for an unchanged state are dropped.
    edram_buffer_modification_status_ = EDRAMBufferModificationStatus::kUnmodified;
  }
  edram_buffer_state_ = new_state;
}

void D3D12RenderTargetCache::MarkEDRAMBufferModified(
    EDRAMBufferModificationStatus modification_status) {
  REX_ASSERT(modification_status != EDRAMBufferModificationStatus::kUnmodified);
  REX_ASSERT(edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (edram_buffer_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    return;
  }
  // Modification as a UAV needs stricter synchronization than as ROV, hence the max.
  edram_buffer_modification_status_ =
      std::max(edram_buffer_modification_status_, modification_status);
}

void D3D12RenderTargetCache::CommitEDRAMBufferUAVWrites(
    EDRAMBufferModificationStatus commit_status) {
  REX_ASSERT(commit_status != EDRAMBufferModificationStatus::kUnmodified);
  if (edram_buffer_modification_status_ < commit_status) {
    return;
  }
  REX_ASSERT(edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    command_processor_.PushUAVBarrier(edram_buffer_);
  }
  edram_buffer_modification_status_ = EDRAMBufferModificationStatus::kUnmodified;
  PixelShaderInterlockFullEDRAMBarrierPlaced();
}

ID3D12PipelineState* const* D3D12RenderTargetCache::GetOrCreateTransferPipelines(
    TransferShaderKey key) {
  const TransferModeInfo& mode = kTransferModes[static_cast<size_t>(key.mode)];
  bool dest_is_stencil_bit = (mode.output == TransferOutput::kStencilBit);

  if (dest_is_stencil_bit) {
    auto pipelines_it = transfer_stencil_bit_pipelines_.find(key);
    if (pipelines_it != transfer_stencil_bit_pipelines_.end()) {
      return pipelines_it->second[0] ? pipelines_it->second.data() : nullptr;
    }
  } else {
    auto pipeline_it = transfer_pipelines_.find(key);
    if (pipeline_it != transfer_pipelines_.end()) {
      return pipeline_it->second ? &pipeline_it->second : nullptr;
    }
  }

  u32 rs = kTransferUsedRootParameters[static_cast<size_t>(
      use_stencil_reference_output_ ? mode.root_signature_with_stencil_ref
                                    : mode.root_signature_no_stencil_ref)];

  bool dest_is_color = (mode.output == TransferOutput::kColor);

  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      xenos::DepthRenderTargetFormat(key.dest_resource_format);
  bool dest_is_64bpp = dest_is_color && xenos::IsColorRenderTargetFormat64bpp(dest_color_format);

  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      xenos::DepthRenderTargetFormat(key.source_resource_format);
  bool source_is_color = (rs & kTransferUsedRootParameterColorSRVBit) != 0;
  bool source_is_64bpp;
  u32 source_color_format_component_count;
  u32 source_color_srv_component_mask;
  bool source_color_is_uint;
  if (source_is_color) {
    REX_ASSERT((rs & kTransferUsedRootParameterDepthSRVBit) == 0);
    REX_ASSERT((rs & kTransferUsedRootParameterStencilSRVBit) == 0);
    source_is_64bpp = xenos::IsColorRenderTargetFormat64bpp(source_color_format);
    source_color_format_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(source_color_format);
    if (dest_is_stencil_bit) {
      if (source_is_64bpp && !dest_is_64bpp) {
        // Needs 1 component, chosen from the two 32bpp halves of the 64bpp sample.
        source_color_srv_component_mask = 0b1 | (0b1 << (source_color_format_component_count >> 1));
      } else {
        // Red is at least 8 bits per component in all formats.
        source_color_srv_component_mask = 0b1;
      }
    } else {
      source_color_srv_component_mask =
          (static_cast<u32>(1) << source_color_format_component_count) - 1;
    }
    GetColorOwnershipTransferDXGIFormat(source_color_format, &source_color_is_uint);
  } else {
    source_is_64bpp = false;
    source_color_format_component_count = 0;
    source_color_srv_component_mask = 0;
    source_color_is_uint = false;
  }

  bool shader_uses_stencil_reference_output =
      mode.output == TransferOutput::kDepth && use_stencil_reference_output_;

  // built_shader_.resize() invalidates pointers and zeroes the new memory.

  built_shader_.clear();

  // RDEF, ISGN, OSGN, SHEX, optionally SFI0, STAT.
  u32 blob_count = 5 + static_cast<u32>(shader_uses_stencil_reference_output);

  built_shader_.resize(sizeof(dxbc::ContainerHeader) / sizeof(u32) + blob_count);
  u32 blob_offset_position_dwords = sizeof(dxbc::ContainerHeader) / sizeof(u32);
  u32 blob_position_dwords = static_cast<u32>(built_shader_.size());
  constexpr u32 kBlobHeaderSizeDwords = sizeof(dxbc::BlobHeader) / sizeof(u32);

  u32 name_ptr;

  built_shader_[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 rdef_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;

  built_shader_.resize(rdef_position_dwords + sizeof(dxbc::RdefHeader) / sizeof(u32));
  dxbc::AppendAlignedString(built_shader_, "ReXGlue");

  // Constant types are uint only, named "dword" when scalar.
  name_ptr = static_cast<u32>((built_shader_.size() - rdef_position_dwords) * sizeof(u32));
  u32 rdef_dword_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "dword");
  u32 rdef_type_uint_position_dwords = static_cast<u32>(built_shader_.size());
  u32 rdef_type_uint_ptr =
      static_cast<u32>((rdef_type_uint_position_dwords - rdef_position_dwords) * sizeof(u32));
  built_shader_.resize(rdef_type_uint_position_dwords + sizeof(dxbc::RdefType) / sizeof(u32));
  {
    auto& rdef_type_uint =
        *reinterpret_cast<dxbc::RdefType*>(built_shader_.data() + rdef_type_uint_position_dwords);
    rdef_type_uint.variable_class = dxbc::RdefVariableClass::kScalar;
    rdef_type_uint.variable_type = dxbc::RdefVariableType::kUInt;
    rdef_type_uint.row_count = 1;
    rdef_type_uint.column_count = 1;
    rdef_type_uint.name_ptr = rdef_dword_name_ptr;
  }

  u32 rdef_constant_count = 0;
  u32 rdef_constant_index_stencil_mask =
      (rs & kTransferUsedRootParameterStencilMaskConstantBit) ? rdef_constant_count++ : UINT32_MAX;
  REX_ASSERT(!(dest_is_stencil_bit && rdef_constant_index_stencil_mask == UINT32_MAX));
  u32 rdef_constant_index_address =
      (rs & kTransferUsedRootParameterAddressConstantBit) ? rdef_constant_count++ : UINT32_MAX;
  REX_ASSERT(rdef_constant_index_address != UINT32_MAX);
  u32 rdef_constant_index_host_depth_address =
      (rs & kTransferUsedRootParameterHostDepthAddressConstantBit) ? rdef_constant_count++
                                                                   : UINT32_MAX;
  name_ptr = static_cast<u32>((built_shader_.size() - rdef_position_dwords) * sizeof(u32));
  u32 rdef_xe_transfer_stencil_mask_name_ptr = name_ptr;
  if (rdef_constant_index_stencil_mask != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_stencil_mask");
  }
  u32 rdef_xe_transfer_address_name_ptr = name_ptr;
  if (rdef_constant_index_address != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_address");
  }
  u32 rdef_xe_transfer_host_depth_address_name_ptr = name_ptr;
  if (rdef_constant_index_host_depth_address != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_host_depth_address");
  }
  u32 rdef_constants_position_dwords = static_cast<u32>(built_shader_.size());
  u32 rdef_constants_ptr =
      static_cast<u32>((rdef_constants_position_dwords - rdef_position_dwords) * sizeof(u32));
  built_shader_.resize(rdef_constants_position_dwords +
                       sizeof(dxbc::RdefVariable) / sizeof(u32) * rdef_constant_count);
  {
    auto rdef_constants = reinterpret_cast<dxbc::RdefVariable*>(built_shader_.data() +
                                                                rdef_constants_position_dwords);
    if (rdef_constant_index_stencil_mask != UINT32_MAX) {
      dxbc::RdefVariable& rdef_constant_stencil_mask =
          rdef_constants[rdef_constant_index_stencil_mask];
      rdef_constant_stencil_mask.name_ptr = rdef_xe_transfer_stencil_mask_name_ptr;
      rdef_constant_stencil_mask.size_bytes = sizeof(u32);
      rdef_constant_stencil_mask.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_stencil_mask.type_ptr = rdef_type_uint_ptr;
      rdef_constant_stencil_mask.start_texture = UINT32_MAX;
      rdef_constant_stencil_mask.start_sampler = UINT32_MAX;
    }
    if (rdef_constant_index_address != UINT32_MAX) {
      dxbc::RdefVariable& rdef_constant_address = rdef_constants[rdef_constant_index_address];
      rdef_constant_address.name_ptr = rdef_xe_transfer_address_name_ptr;
      rdef_constant_address.size_bytes = sizeof(u32);
      rdef_constant_address.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_address.type_ptr = rdef_type_uint_ptr;
      rdef_constant_address.start_texture = UINT32_MAX;
      rdef_constant_address.start_sampler = UINT32_MAX;
    }
    if (rdef_constant_index_host_depth_address != UINT32_MAX) {
      dxbc::RdefVariable& rdef_constant_host_depth_address =
          rdef_constants[rdef_constant_index_host_depth_address];
      rdef_constant_host_depth_address.name_ptr = rdef_xe_transfer_host_depth_address_name_ptr;
      rdef_constant_host_depth_address.size_bytes = sizeof(u32);
      rdef_constant_host_depth_address.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_host_depth_address.type_ptr = rdef_type_uint_ptr;
      rdef_constant_host_depth_address.start_texture = UINT32_MAX;
      rdef_constant_host_depth_address.start_sampler = UINT32_MAX;
    }
  }

  // Constant buffers reuse the constant names.
  u32 rdef_cbuffer_count = 0;
  u32 cbuffer_index_stencil_mask =
      rdef_constant_index_stencil_mask != UINT32_MAX ? rdef_cbuffer_count++ : UINT32_MAX;
  u32 cbuffer_index_address =
      rdef_constant_index_address != UINT32_MAX ? rdef_cbuffer_count++ : UINT32_MAX;
  u32 cbuffer_index_host_depth_address =
      rdef_constant_index_host_depth_address != UINT32_MAX ? rdef_cbuffer_count++ : UINT32_MAX;
  u32 rdef_cbuffer_position_dwords = static_cast<u32>(built_shader_.size());
  built_shader_.resize(rdef_cbuffer_position_dwords +
                       sizeof(dxbc::RdefCbuffer) / sizeof(u32) * rdef_cbuffer_count);
  {
    auto rdef_cbuffers =
        reinterpret_cast<dxbc::RdefCbuffer*>(built_shader_.data() + rdef_cbuffer_position_dwords);
    if (cbuffer_index_stencil_mask != UINT32_MAX) {
      dxbc::RdefCbuffer& rdef_cbuffer_stencil_mask = rdef_cbuffers[cbuffer_index_stencil_mask];
      rdef_cbuffer_stencil_mask.name_ptr = rdef_xe_transfer_stencil_mask_name_ptr;
      rdef_cbuffer_stencil_mask.variable_count = 1;
      rdef_cbuffer_stencil_mask.variables_ptr = static_cast<u32>(
          rdef_constants_ptr + sizeof(dxbc::RdefVariable) * rdef_constant_index_stencil_mask);
      rdef_cbuffer_stencil_mask.size_vector_aligned_bytes = sizeof(u32) * 4;
    }
    if (cbuffer_index_address != UINT32_MAX) {
      dxbc::RdefCbuffer& rdef_cbuffer_address = rdef_cbuffers[cbuffer_index_address];
      rdef_cbuffer_address.name_ptr = rdef_xe_transfer_address_name_ptr;
      rdef_cbuffer_address.variable_count = 1;
      rdef_cbuffer_address.variables_ptr = static_cast<u32>(
          rdef_constants_ptr + sizeof(dxbc::RdefVariable) * rdef_constant_index_address);
      rdef_cbuffer_address.size_vector_aligned_bytes = sizeof(u32) * 4;
    }
    if (cbuffer_index_host_depth_address != UINT32_MAX) {
      dxbc::RdefCbuffer& rdef_cbuffer_host_depth_address =
          rdef_cbuffers[cbuffer_index_host_depth_address];
      rdef_cbuffer_host_depth_address.name_ptr = rdef_xe_transfer_host_depth_address_name_ptr;
      rdef_cbuffer_host_depth_address.variable_count = 1;
      rdef_cbuffer_host_depth_address.variables_ptr = static_cast<u32>(
          rdef_constants_ptr + sizeof(dxbc::RdefVariable) * rdef_constant_index_host_depth_address);
      rdef_cbuffer_host_depth_address.size_vector_aligned_bytes = sizeof(u32) * 4;
    }
  }

  u32 rdef_srv_count = 0;
  u32 srv_index_color =
      (rs & kTransferUsedRootParameterColorSRVBit) ? rdef_srv_count++ : UINT32_MAX;
  u32 srv_index_depth =
      (rs & kTransferUsedRootParameterDepthSRVBit) ? rdef_srv_count++ : UINT32_MAX;
  u32 srv_index_stencil =
      (rs & kTransferUsedRootParameterStencilSRVBit) ? rdef_srv_count++ : UINT32_MAX;
  u32 srv_index_host_depth =
      (rs & kTransferUsedRootParameterHostDepthSRVBit) ? rdef_srv_count++ : UINT32_MAX;
  u32 rdef_binding_count = rdef_srv_count + rdef_cbuffer_count;
  name_ptr = static_cast<u32>((built_shader_.size() - rdef_position_dwords) * sizeof(u32));
  u32 rdef_xe_transfer_color_name_ptr = name_ptr;
  if (srv_index_color != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_color");
  }
  u32 rdef_xe_transfer_depth_name_ptr = name_ptr;
  if (srv_index_depth != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_depth");
  }
  u32 rdef_xe_transfer_stencil_name_ptr = name_ptr;
  if (srv_index_stencil != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_stencil");
  }
  u32 rdef_xe_transfer_host_depth_name_ptr = name_ptr;
  if (srv_index_host_depth != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_transfer_host_depth");
  }
  u32 rdef_binding_position_dwords = static_cast<u32>(built_shader_.size());
  built_shader_.resize(rdef_binding_position_dwords +
                       sizeof(dxbc::RdefInputBind) / sizeof(u32) * rdef_binding_count);
  {
    auto rdef_bindings =
        reinterpret_cast<dxbc::RdefInputBind*>(built_shader_.data() + rdef_binding_position_dwords);
    u32 rdef_binding_index = 0;
    if (srv_index_color != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_color = rdef_bindings[rdef_binding_index++];
      rdef_binding_color.name_ptr = rdef_xe_transfer_color_name_ptr;
      rdef_binding_color.type = dxbc::RdefInputType::kTexture;
      rdef_binding_color.return_type =
          source_color_is_uint ? dxbc::ResourceReturnType::kUInt : dxbc::ResourceReturnType::kFloat;
      if (key.source_msaa_samples != xenos::MSAASamples::k1X) {
        rdef_binding_color.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
      } else {
        rdef_binding_color.dimension = dxbc::RdefDimension::kSRVTexture2D;
        rdef_binding_color.sample_count = UINT32_MAX;
      }
      rdef_binding_color.bind_point = kTransferSRVRegisterColor;
      rdef_binding_color.bind_count = 1;
      REX_ASSERT(source_color_srv_component_mask != 0);
      rdef_binding_color.flags = (32 - rex::lzcnt(source_color_srv_component_mask) - 1)
                                 << dxbc::kRdefInputFlagsComponentsShift;
      rdef_binding_color.id = srv_index_color;
    }
    if (srv_index_depth != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_depth = rdef_bindings[rdef_binding_index++];
      rdef_binding_depth.name_ptr = rdef_xe_transfer_depth_name_ptr;
      rdef_binding_depth.type = dxbc::RdefInputType::kTexture;
      rdef_binding_depth.return_type = dxbc::ResourceReturnType::kFloat;
      if (key.source_msaa_samples != xenos::MSAASamples::k1X) {
        rdef_binding_depth.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
      } else {
        rdef_binding_depth.dimension = dxbc::RdefDimension::kSRVTexture2D;
        rdef_binding_depth.sample_count = UINT32_MAX;
      }
      rdef_binding_depth.bind_point = kTransferSRVRegisterDepth;
      rdef_binding_depth.bind_count = 1;
      rdef_binding_depth.id = srv_index_depth;
    }
    if (srv_index_stencil != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_stencil = rdef_bindings[rdef_binding_index++];
      rdef_binding_stencil.name_ptr = rdef_xe_transfer_stencil_name_ptr;
      rdef_binding_stencil.type = dxbc::RdefInputType::kTexture;
      rdef_binding_stencil.return_type = dxbc::ResourceReturnType::kUInt;
      if (key.source_msaa_samples != xenos::MSAASamples::k1X) {
        rdef_binding_stencil.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
      } else {
        rdef_binding_stencil.dimension = dxbc::RdefDimension::kSRVTexture2D;
        rdef_binding_stencil.sample_count = UINT32_MAX;
      }
      rdef_binding_stencil.bind_point = kTransferSRVRegisterStencil;
      rdef_binding_stencil.bind_count = 1;
      rdef_binding_stencil.flags = dxbc::kRdefInputFlags2Component;
      rdef_binding_stencil.id = srv_index_stencil;
    }
    if (srv_index_host_depth != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_host_depth = rdef_bindings[rdef_binding_index++];
      rdef_binding_host_depth.name_ptr = rdef_xe_transfer_host_depth_name_ptr;
      rdef_binding_host_depth.type = dxbc::RdefInputType::kTexture;
      if (key.host_depth_source_is_copy) {
        // Float as uint.
        rdef_binding_host_depth.return_type = dxbc::ResourceReturnType::kUInt;
        rdef_binding_host_depth.dimension = dxbc::RdefDimension::kSRVBuffer;
        rdef_binding_host_depth.sample_count = UINT32_MAX;
      } else {
        rdef_binding_host_depth.return_type = dxbc::ResourceReturnType::kFloat;
        if (key.host_depth_source_msaa_samples != xenos::MSAASamples::k1X) {
          rdef_binding_host_depth.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
        } else {
          rdef_binding_host_depth.dimension = dxbc::RdefDimension::kSRVTexture2D;
          rdef_binding_host_depth.sample_count = UINT32_MAX;
        }
      }
      rdef_binding_host_depth.bind_point = kTransferSRVRegisterHostDepth;
      rdef_binding_host_depth.bind_count = 1;
      rdef_binding_host_depth.id = srv_index_host_depth;
    }
    if (cbuffer_index_stencil_mask != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_stencil_mask = rdef_bindings[rdef_binding_index++];
      rdef_binding_stencil_mask.name_ptr = rdef_xe_transfer_stencil_mask_name_ptr;
      rdef_binding_stencil_mask.type = dxbc::RdefInputType::kCbuffer;
      rdef_binding_stencil_mask.bind_point = kTransferCBVRegisterStencilMask;
      rdef_binding_stencil_mask.bind_count = 1;
      rdef_binding_stencil_mask.flags = dxbc::kRdefInputFlagUserPacked;
      rdef_binding_stencil_mask.id = cbuffer_index_stencil_mask;
    }
    if (cbuffer_index_address != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_address = rdef_bindings[rdef_binding_index++];
      rdef_binding_address.name_ptr = rdef_xe_transfer_address_name_ptr;
      rdef_binding_address.type = dxbc::RdefInputType::kCbuffer;
      rdef_binding_address.bind_point = kTransferCBVRegisterAddress;
      rdef_binding_address.bind_count = 1;
      rdef_binding_address.flags = dxbc::kRdefInputFlagUserPacked;
      rdef_binding_address.id = cbuffer_index_address;
    }
    if (cbuffer_index_host_depth_address != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_host_depth_address = rdef_bindings[rdef_binding_index++];
      rdef_binding_host_depth_address.name_ptr = rdef_xe_transfer_host_depth_address_name_ptr;
      rdef_binding_host_depth_address.type = dxbc::RdefInputType::kCbuffer;
      rdef_binding_host_depth_address.bind_point = kTransferCBVRegisterHostDepthAddress;
      rdef_binding_host_depth_address.bind_count = 1;
      rdef_binding_host_depth_address.flags = dxbc::kRdefInputFlagUserPacked;
      rdef_binding_host_depth_address.id = cbuffer_index_host_depth_address;
    }
  }

  {
    auto& rdef_header =
        *reinterpret_cast<dxbc::RdefHeader*>(built_shader_.data() + rdef_position_dwords);
    rdef_header.cbuffer_count = rdef_cbuffer_count;
    rdef_header.cbuffers_ptr =
        static_cast<u32>((rdef_cbuffer_position_dwords - rdef_position_dwords) * sizeof(u32));
    rdef_header.input_bind_count = rdef_binding_count;
    rdef_header.input_binds_ptr =
        static_cast<u32>((rdef_binding_position_dwords - rdef_position_dwords) * sizeof(u32));
    rdef_header.shader_model = dxbc::RdefShaderModel::kPixelShader51;
    rdef_header.compile_flags =
        dxbc::kCompileFlagNoPreshader | dxbc::kCompileFlagPreferFlowControl |
        dxbc::kCompileFlagIeeeStrictness | dxbc::kCompileFlagAllResourcesBound;
    rdef_header.generator_name_ptr = sizeof(dxbc::RdefHeader);
    rdef_header.fourcc = dxbc::RdefHeader::FourCC::k51;
    rdef_header.InitializeSizes();
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = static_cast<u32>(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             built_shader_[blob_offset_position_dwords++];
  }

  /** Registers for accessing in the shader code. Multiple inputs may be packed into one. */
  enum InputRegister : u8 {
    kInputRegisterPosition,
    kInputRegisterSampleIndex,
    kInputRegisterCount,
  };

  u32 isgn_parameter_count = 1 + static_cast<u32>(key.dest_msaa_samples != xenos::MSAASamples::k1X);

  built_shader_[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 isgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(isgn_position_dwords + sizeof(dxbc::Signature) / sizeof(u32) +
                       sizeof(dxbc::SignatureParameter) / sizeof(u32) * isgn_parameter_count);

  name_ptr = static_cast<u32>((built_shader_.size() - isgn_position_dwords) * sizeof(u32));
  u32 isgn_sv_position_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "SV_Position");
  u32 isgn_sv_sample_index_name_ptr = name_ptr;
  if (key.dest_msaa_samples != xenos::MSAASamples::k1X) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "SV_SampleIndex");
  }

  {
    auto& isgn_header =
        *reinterpret_cast<dxbc::Signature*>(built_shader_.data() + isgn_position_dwords);
    isgn_header.parameter_count = isgn_parameter_count;
    isgn_header.parameter_info_ptr = sizeof(dxbc::Signature);
    auto isgn_parameters = reinterpret_cast<dxbc::SignatureParameter*>(
        built_shader_.data() + isgn_position_dwords + sizeof(dxbc::Signature) / sizeof(u32));
    u32 isgn_parameter_index = 0;
    dxbc::SignatureParameter& isgn_sv_position = isgn_parameters[isgn_parameter_index++];
    isgn_sv_position.semantic_name_ptr = isgn_sv_position_name_ptr;
    isgn_sv_position.system_value = dxbc::Name::kPosition;
    isgn_sv_position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
    isgn_sv_position.register_index = kInputRegisterPosition;
    isgn_sv_position.mask = 0b1111;
    isgn_sv_position.always_reads_mask = 0b0011;
    if (key.dest_msaa_samples != xenos::MSAASamples::k1X) {
      dxbc::SignatureParameter& isgn_sv_sample_index = isgn_parameters[isgn_parameter_index++];
      isgn_sv_sample_index.semantic_name_ptr = isgn_sv_sample_index_name_ptr;
      isgn_sv_sample_index.system_value = dxbc::Name::kSampleIndex;
      isgn_sv_sample_index.component_type = dxbc::SignatureRegisterComponentType::kUInt32;
      isgn_sv_sample_index.register_index = kInputRegisterSampleIndex;
      isgn_sv_sample_index.mask = 0b0001;
      isgn_sv_sample_index.always_reads_mask = 0b0001;
    }
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kInputSignature;
    blob_position_dwords = static_cast<u32>(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             built_shader_[blob_offset_position_dwords++];
  }

  u32 osgn_parameter_count = 0;
  u32 osgn_parameter_index_sv_target =
      mode.output == TransferOutput::kColor ? osgn_parameter_count++ : UINT32_MAX;
  u32 osgn_parameter_index_sv_depth =
      mode.output == TransferOutput::kDepth ? osgn_parameter_count++ : UINT32_MAX;
  u32 osgn_parameter_index_sv_stencil_ref =
      shader_uses_stencil_reference_output ? osgn_parameter_count++ : UINT32_MAX;

  built_shader_[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 osgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(osgn_position_dwords + sizeof(dxbc::Signature) / sizeof(u32) +
                       sizeof(dxbc::SignatureParameter) / sizeof(u32) * osgn_parameter_count);

  name_ptr = static_cast<u32>((built_shader_.size() - osgn_position_dwords) * sizeof(u32));
  u32 osgn_sv_target_name_ptr = name_ptr;
  if (osgn_parameter_index_sv_target != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "SV_Target");
  }
  u32 osgn_sv_depth_name_ptr = name_ptr;
  if (osgn_parameter_index_sv_depth != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "SV_Depth");
  }
  u32 osgn_sv_stencil_ref_name_ptr = name_ptr;
  if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "SV_StencilRef");
  }

  bool dest_color_is_uint;
  if (mode.output == TransferOutput::kColor) {
    GetColorOwnershipTransferDXGIFormat(dest_color_format, &dest_color_is_uint);
  } else {
    dest_color_is_uint = false;
  }

  {
    auto& osgn_header =
        *reinterpret_cast<dxbc::Signature*>(built_shader_.data() + osgn_position_dwords);
    osgn_header.parameter_count = osgn_parameter_count;
    osgn_header.parameter_info_ptr = sizeof(dxbc::Signature);
    auto osgn_parameters = reinterpret_cast<dxbc::SignatureParameter*>(
        built_shader_.data() + osgn_position_dwords + sizeof(dxbc::Signature) / sizeof(u32));
    if (osgn_parameter_index_sv_target != UINT32_MAX) {
      dxbc::SignatureParameter& osgn_sv_target = osgn_parameters[osgn_parameter_index_sv_target];
      osgn_sv_target.semantic_name_ptr = osgn_sv_target_name_ptr;
      osgn_sv_target.component_type = dest_color_is_uint
                                          ? dxbc::SignatureRegisterComponentType::kUInt32
                                          : dxbc::SignatureRegisterComponentType::kFloat32;
      osgn_sv_target.register_index = 0;
      osgn_sv_target.mask = 0b1111;
    }
    if (osgn_parameter_index_sv_depth != UINT32_MAX) {
      dxbc::SignatureParameter& osgn_sv_depth = osgn_parameters[osgn_parameter_index_sv_depth];
      osgn_sv_depth.semantic_name_ptr = osgn_sv_depth_name_ptr;
      osgn_sv_depth.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      osgn_sv_depth.register_index = UINT32_MAX;
      osgn_sv_depth.mask = 0b0001;
      osgn_sv_depth.never_writes_mask = 0b1110;
    }
    if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
      dxbc::SignatureParameter& osgn_sv_stencil_ref =
          osgn_parameters[osgn_parameter_index_sv_stencil_ref];
      osgn_sv_stencil_ref.semantic_name_ptr = osgn_sv_stencil_ref_name_ptr;
      // SV_StencilRef is uint in DXC and recent FXC, but older FXC wrongly expects float.
      osgn_sv_stencil_ref.component_type = dxbc::SignatureRegisterComponentType::kUInt32;
      osgn_sv_stencil_ref.register_index = UINT32_MAX;
      osgn_sv_stencil_ref.mask = 0b0001;
      osgn_sv_stencil_ref.never_writes_mask = 0b1110;
    }
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kOutputSignature;
    blob_position_dwords = static_cast<u32>(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             built_shader_[blob_offset_position_dwords++];
  }

  built_shader_[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 shex_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(shex_position_dwords);

  built_shader_.push_back(dxbc::VersionToken(dxbc::ProgramType::kPixelShader, 5, 1));
  built_shader_.push_back(0);

  dxbc::Statistics stat{};
  dxbc::Assembler a(built_shader_, stat);

  a.OpDclGlobalFlags(dxbc::kGlobalFlagAllResourcesBound);
  if (cbuffer_index_stencil_mask != UINT32_MAX) {
    a.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::kDcl, cbuffer_index_stencil_mask, kTransferCBVRegisterStencilMask,
                      kTransferCBVRegisterStencilMask),
        1);
  }
  if (cbuffer_index_address != UINT32_MAX) {
    a.OpDclConstantBuffer(dxbc::Src::CB(dxbc::Src::kDcl, cbuffer_index_address,
                                        kTransferCBVRegisterAddress, kTransferCBVRegisterAddress),
                          1);
  }
  if (cbuffer_index_host_depth_address != UINT32_MAX) {
    a.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::kDcl, cbuffer_index_host_depth_address,
                      kTransferCBVRegisterHostDepthAddress, kTransferCBVRegisterHostDepthAddress),
        1);
  }
  if (srv_index_color != UINT32_MAX) {
    a.OpDclResource(
        key.source_msaa_samples != xenos::MSAASamples::k1X ? dxbc::ResourceDimension::kTexture2DMS
                                                           : dxbc::ResourceDimension::kTexture2D,
        dxbc::ResourceReturnTypeX4Token(source_color_is_uint ? dxbc::ResourceReturnType::kUInt
                                                             : dxbc::ResourceReturnType::kFloat),
        dxbc::Src::T(dxbc::Src::kDcl, srv_index_color, kTransferSRVRegisterColor,
                     kTransferSRVRegisterColor));
  }
  if (srv_index_depth != UINT32_MAX) {
    a.OpDclResource(key.source_msaa_samples != xenos::MSAASamples::k1X
                        ? dxbc::ResourceDimension::kTexture2DMS
                        : dxbc::ResourceDimension::kTexture2D,
                    dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kFloat),
                    dxbc::Src::T(dxbc::Src::kDcl, srv_index_depth, kTransferSRVRegisterDepth,
                                 kTransferSRVRegisterDepth));
  }
  if (srv_index_stencil != UINT32_MAX) {
    a.OpDclResource(key.source_msaa_samples != xenos::MSAASamples::k1X
                        ? dxbc::ResourceDimension::kTexture2DMS
                        : dxbc::ResourceDimension::kTexture2D,
                    dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kUInt),
                    dxbc::Src::T(dxbc::Src::kDcl, srv_index_stencil, kTransferSRVRegisterStencil,
                                 kTransferSRVRegisterStencil));
  }
  if (srv_index_host_depth != UINT32_MAX) {
    a.OpDclResource(key.host_depth_source_is_copy
                        ? dxbc::ResourceDimension::kBuffer
                        : (key.host_depth_source_msaa_samples != xenos::MSAASamples::k1X
                               ? dxbc::ResourceDimension::kTexture2DMS
                               : dxbc::ResourceDimension::kTexture2D),
                    dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kFloat),
                    dxbc::Src::T(dxbc::Src::kDcl, srv_index_host_depth,
                                 kTransferSRVRegisterHostDepth, kTransferSRVRegisterHostDepth));
  }
  a.OpDclInputPSSIV(dxbc::InterpolationMode::kLinearNoPerspective,
                    dxbc::Dest::V1D(kInputRegisterPosition, 0b0011), dxbc::Name::kPosition);
  if (key.dest_msaa_samples != xenos::MSAASamples::k1X) {
    a.OpDclInputPSSGV(dxbc::Dest::V1D(kInputRegisterSampleIndex, 0b0001), dxbc::Name::kSampleIndex);
  }
  if (osgn_parameter_index_sv_target != UINT32_MAX) {
    a.OpDclOutput(dxbc::Dest::O(0));
  }
  if (osgn_parameter_index_sv_depth != UINT32_MAX) {
    a.OpDclOutput(dxbc::Dest::ODepth());
  }
  if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
    a.OpDclOutput(dxbc::Dest::OStencilRef());
  }
  // 3 temps suffice, as texture loads can overwrite addressing temps after using them.
  a.OpDclTemps(3);

  u32 draw_resolution_scale_x = this->GetDrawResolutionScaleX();
  u32 draw_resolution_scale_y = this->GetDrawResolutionScaleY();

  u32 tile_width_samples = xenos::kEDRAMTileWidthSamples * draw_resolution_scale_x;
  u32 tile_height_samples = xenos::kEDRAMTileHeightSamples * draw_resolution_scale_y;

  a.OpFToU(dxbc::Dest::R(0, 0b0011), dxbc::Src::V1D(kInputRegisterPosition));
  u32 dest_tile_width_pixels =
      tile_width_samples >> (static_cast<u32>(dest_is_64bpp) +
                             static_cast<u32>(key.dest_msaa_samples >= xenos::MSAASamples::k4X));
  u32 dest_tile_height_pixels =
      tile_height_samples >> static_cast<u32>(key.dest_msaa_samples >= xenos::MSAASamples::k2X);
  // After the divide, r0.xy = pixel XY within the 32bpp tile and r0.zw = 32bpp tile XY.
  a.OpUDiv(dxbc::Dest::R(0, 0b1100), dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0, 0b01000100),
           dxbc::Src::LU(dest_tile_width_pixels, dest_tile_height_pixels, dest_tile_width_pixels,
                         dest_tile_height_pixels));

  a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEDRAMPitchTilesBits), dxbc::Src::LU(0),
           dxbc::Src::CB(cbuffer_index_address, kTransferCBVRegisterAddress, 0, dxbc::Src::kXXXX));
  // r0.z = 32bpp tile index relative to the destination base, r0.w and r1.x are free.
  a.OpUMAd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(1, dxbc::Src::kXXXX),
           dxbc::Src::R(0, dxbc::Src::kWWWW), dxbc::Src::R(0, dxbc::Src::kZZZZ));

  // Source is 64bpp or 32bpp, depth if only depth is available and color otherwise.

  // The source loads to r1, or low to r0 and high to r1 for 64bpp color (the address is in r1).

  // Source XY in r1.xy and sample in r1.z, unless unmodified in r0.xy / v#. r0.w = 64bpp half.

  dxbc::Src dest_sample(dxbc::Src::V1D(kInputRegisterSampleIndex, dxbc::Src::kXXXX));
  dxbc::Src source_sample(dest_sample);
  u32 source_tile_pixel_x_reg = 0;
  u32 source_tile_pixel_y_reg = 0;

  if (!source_is_64bpp && dest_is_64bpp) {
    if (key.source_msaa_samples >= xenos::MSAASamples::k4X) {
      if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
        a.OpAnd(dxbc::Dest::R(1, 0b0100), dest_sample, dxbc::Src::LU(0b10));
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(31), dxbc::Src::LU(1),
                dxbc::Src::R(0, dxbc::Src::kXXXX),
                dxbc::Src::V1D(kInputRegisterSampleIndex, dxbc::Src::kXXXX));
        source_tile_pixel_x_reg = 1;
      } else if (key.dest_msaa_samples == xenos::MSAASamples::k2X) {
        // Sample 1 (native 2x) or 0 (2x as 4x) becomes samples 01, sample 0 or 3 becomes 23.
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        if (msaa_2x_supported_) {
          a.OpXOr(dxbc::Dest::R(1, 0b0100), dest_sample, dxbc::Src::LU(1));
          a.OpIShL(dxbc::Dest::R(1, 0b0100), source_sample, dxbc::Src::LU(1));
        } else {
          a.OpAnd(dxbc::Dest::R(1, 0b0100), dest_sample, dxbc::Src::LU(0b10));
        }
      } else {
        // Horizontal pixel index is reused, and vertical pixel 1 becomes sample 2.
        a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1),
                dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(0));
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        a.OpUShR(dxbc::Dest::R(1, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(1));
        source_tile_pixel_y_reg = 1;
      }
    } else {
      if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
        a.OpIShL(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(2));
        a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(1), dxbc::Src::LU(1),
                dxbc::Src::V1D(kInputRegisterSampleIndex, dxbc::Src::kXXXX),
                dxbc::Src::R(1, dxbc::Src::kXXXX));
        source_tile_pixel_x_reg = 1;
        // Y is handled by common code.
      } else {
        a.OpIShL(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
        source_tile_pixel_x_reg = 1;
        // Y is handled by common code.
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
      // The needed half is in the destination horizontal sample index.
      if (key.source_msaa_samples >= xenos::MSAASamples::k4X) {
        a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(0),
                dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        // 2 destination horizontal pixels = 1 source horizontal pixel.
        a.OpUShR(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
        source_tile_pixel_x_reg = 1;
      } else {
        // The horizontal pixel index is reused and Y is handled by common code.
      }
      a.OpAnd(dxbc::Dest::R(0, 0b1000), dest_sample, dxbc::Src::LU(1));
    } else {
      // The needed half is in the destination horizontal pixel index.
      if (key.source_msaa_samples >= xenos::MSAASamples::k4X) {
        // (Destination horizontal pixel >> 1) & 1 = source horizontal sample (first bit).
        a.OpUBFE(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1),
                 dxbc::Src::R(0, dxbc::Src::kXXXX));
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        if (key.dest_msaa_samples == xenos::MSAASamples::k2X) {
          // Vertical samples (native 2x bit 0 1/0, 2x as 4x bit 1 0/1) = source bit 1 samples.
          if (msaa_2x_supported_) {
            a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1), dest_sample,
                    source_sample);
            a.OpXOr(dxbc::Dest::R(1, 0b0100), source_sample, dxbc::Src::LU(1 << 1));
          } else {
            a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(0), source_sample,
                    dest_sample);
          }
        } else {
          // 1 destination vertical pixel = 1 source vertical sample.
          a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1),
                  dxbc::Src::R(0, dxbc::Src::kYYYY), source_sample);
          a.OpUShR(dxbc::Dest::R(1, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(1));
          source_tile_pixel_y_reg = 1;
        }
        // 4 destination horizontal pixels = 1 source horizontal pixel.
        a.OpUShR(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(2));
        source_tile_pixel_x_reg = 1;
      } else {
        // 2 destination horizontal pixels = 1 source pixel.
        a.OpUShR(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
        source_tile_pixel_x_reg = 1;
        // Y is handled by common code.
      }
      a.OpAnd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
    }
  } else {
    if (key.source_msaa_samples != key.dest_msaa_samples) {
      if (key.source_msaa_samples >= xenos::MSAASamples::k4X) {
        if (key.dest_msaa_samples == xenos::MSAASamples::k2X) {
          // Horizontal pixels to samples, vertical sample (1/0 native, 0/1 as 4x) to second bit.
          source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
          if (msaa_2x_supported_) {
            a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(31), dxbc::Src::LU(1), dest_sample,
                    dxbc::Src::R(0, dxbc::Src::kXXXX));
            a.OpXOr(dxbc::Dest::R(1, 0b0100), source_sample, dxbc::Src::LU(1 << 1));
          } else {
            a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(0),
                    dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
          }
          a.OpUShR(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
          source_tile_pixel_x_reg = 1;
        } else {
          a.OpAnd(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
          source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
          a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1),
                  dxbc::Src::R(0, dxbc::Src::kYYYY), source_sample);
          a.OpUShR(dxbc::Dest::R(1, 0b0011), dxbc::Src::R(0), dxbc::Src::LU(1));
          source_tile_pixel_x_reg = 1;
          source_tile_pixel_y_reg = 1;
        }
      } else {
        // Only X is handled here, and Y by common code.
        if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(31), dxbc::Src::LU(1),
                  dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
          source_tile_pixel_x_reg = 1;
        }
      }
    }
  }
  // Common source Y and sample index for 1x and 2x sources, independent of bits per sample.
  if (key.source_msaa_samples < xenos::MSAASamples::k4X &&
      key.source_msaa_samples != key.dest_msaa_samples) {
    if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
      if (key.source_msaa_samples == xenos::MSAASamples::k2X) {
        // Destination vertical samples (second bit) to source samples (1, 0 native, 0, 3 as 4x).
        a.OpUShR(dxbc::Dest::R(1, 0b0100), dest_sample, dxbc::Src::LU(1));
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        if (msaa_2x_supported_) {
          a.OpXOr(dxbc::Dest::R(1, 0b0100), source_sample, dxbc::Src::LU(1));
        } else {
          a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1), source_sample,
                  source_sample);
        }
      } else {
        // Vertical samples (second bit) to Y pixels.
        a.OpUShR(dxbc::Dest::R(1, 0b0010), dest_sample, dxbc::Src::LU(1));
        a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(1, dxbc::Src::kYYYY));
        source_tile_pixel_y_reg = 1;
      }
    } else {
      if (key.source_msaa_samples == xenos::MSAASamples::k2X) {
        // Vertical pixels to vertical samples (1, 0 native 2x, 0, 3 2x as 4x).
        a.OpAnd(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(1));
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        if (msaa_2x_supported_) {
          a.OpXOr(dxbc::Dest::R(1, 0b0100), source_sample, dxbc::Src::LU(1));
        } else {
          a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1), source_sample,
                  source_sample);
        }
        a.OpUShR(dxbc::Dest::R(1, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(1));
        source_tile_pixel_y_reg = 1;
      } else {
        // Vertical samples (native 2x first bit 1/0, 2x as 4x second bit 0/1) to vertical pixels.
        if (msaa_2x_supported_) {
          a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                  dxbc::Src::R(0, dxbc::Src::kYYYY), dest_sample);
          a.OpXOr(dxbc::Dest::R(1, 0b0010), dxbc::Src::R(1, dxbc::Src::kYYYY), dxbc::Src::LU(1));
        } else {
          a.OpUShR(dxbc::Dest::R(1, 0b0010), dest_sample, dxbc::Src::LU(1));
          a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                  dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(1, dxbc::Src::kYYYY));
        }
        source_tile_pixel_y_reg = 1;
      }
    }
  }

  u32 source_pixel_width_dwords_log2 =
      static_cast<u32>(key.source_msaa_samples >= xenos::MSAASamples::k4X) +
      static_cast<u32>(source_is_64bpp);

  if (source_is_color != dest_is_color) {
    // Color and depth / stencil swap 40 sample columns in the source tile, with r1.w as a temp.
    u32 source_32bpp_tile_half_pixels = tile_width_samples >> (1 + source_pixel_width_dwords_log2);
    a.OpULT(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(source_tile_pixel_x_reg, dxbc::Src::kXXXX),
            dxbc::Src::LU(source_32bpp_tile_half_pixels));
    a.OpMovC(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
             dxbc::Src::LI(static_cast<i32>(source_32bpp_tile_half_pixels)),
             dxbc::Src::LI(-static_cast<i32>(source_32bpp_tile_half_pixels)));
    a.OpIAdd(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(source_tile_pixel_x_reg, dxbc::Src::kXXXX),
             dxbc::Src::R(1, dxbc::Src::kWWWW));
    source_tile_pixel_x_reg = 1;
  }

  a.OpIBFE(dxbc::Dest::R(1, 0b1000), dxbc::Src::LU(xenos::kEDRAMBaseTilesBits + 1),
           dxbc::Src::LU(xenos::kEDRAMPitchTilesBits * 2),
           dxbc::Src::CB(cbuffer_index_address, kTransferCBVRegisterAddress, 0, dxbc::Src::kXXXX));
  // A negative sum is a transfer across EDRAM addressing wrapping, fixed by the mask below.
  a.OpIAdd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(0, dxbc::Src::kZZZZ),
           dxbc::Src::R(1, dxbc::Src::kWWWW));
  a.OpAnd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
          dxbc::Src::LU(xenos::kEDRAMTileCount - 1));
  a.OpUBFE(dxbc::Dest::R(2, 0b0001), dxbc::Src::LU(xenos::kEDRAMPitchTilesBits),
           dxbc::Src::LU(xenos::kEDRAMPitchTilesBits),
           dxbc::Src::CB(cbuffer_index_address, kTransferCBVRegisterAddress, 0, dxbc::Src::kXXXX));
  a.OpUDiv(dxbc::Dest::R(1, 0b1000), dxbc::Dest::R(2, 0b0001), dxbc::Src::R(1, dxbc::Src::kWWWW),
           dxbc::Src::R(2, dxbc::Src::kXXXX));
  a.OpUMAd(
      dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(tile_width_samples >> source_pixel_width_dwords_log2),
      dxbc::Src::R(2, dxbc::Src::kXXXX), dxbc::Src::R(source_tile_pixel_x_reg, dxbc::Src::kXXXX));
  a.OpUMAd(dxbc::Dest::R(1, 0b0010),
           dxbc::Src::LU(tile_height_samples >>
                         static_cast<u32>(key.source_msaa_samples >= xenos::MSAASamples::k2X)),
           dxbc::Src::R(1, dxbc::Src::kWWWW),
           dxbc::Src::R(source_tile_pixel_y_reg, dxbc::Src::kYYYY));

  bool source_load_is_two_dwords = !source_is_64bpp && dest_is_64bpp;
  if (key.source_msaa_samples != xenos::MSAASamples::k1X) {
    for (u32 i = 0; i <= static_cast<u32>(source_load_is_two_dwords); ++i) {
      u32 source_load_register = source_load_is_two_dwords ? i : 1;
      if (srv_index_depth != UINT32_MAX) {
        a.OpLdMS(dxbc::Dest::R(source_load_register, 0b1000), dxbc::Src::R(1), 0b0011,
                 dxbc::Src::T(srv_index_depth, kTransferSRVRegisterDepth, dxbc::Src::kXXXX),
                 source_sample);
      }
      if (srv_index_stencil != UINT32_MAX) {
        a.OpLdMS(dxbc::Dest::R(source_load_register, 0b0001), dxbc::Src::R(1), 0b0011,
                 dxbc::Src::T(srv_index_stencil, kTransferSRVRegisterStencil, dxbc::Src::kYYYY),
                 source_sample);
      } else if (srv_index_color != UINT32_MAX) {
        a.OpLdMS(dxbc::Dest::R(source_load_register, source_color_srv_component_mask),
                 dxbc::Src::R(1), 0b0011, dxbc::Src::T(srv_index_color, kTransferSRVRegisterColor),
                 source_sample);
      }
      if (source_load_is_two_dwords && !i) {
        if (key.source_msaa_samples >= xenos::MSAASamples::k4X) {
          a.OpOr(dxbc::Dest::R(1, 0b0100), source_sample, dxbc::Src::LU(1));
          source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        } else {
          a.OpOr(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::LU(1));
        }
      }
    }
  } else {
    a.OpMov(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(0));
    dxbc::Src source_coordinates(dxbc::Src::R(1, 0b10000100));
    for (u32 i = 0; i <= static_cast<u32>(source_load_is_two_dwords); ++i) {
      u32 source_load_register = source_load_is_two_dwords ? i : 1;
      if (srv_index_depth != UINT32_MAX) {
        a.OpLd(dxbc::Dest::R(source_load_register, 0b1000), source_coordinates, 0b1011,
               dxbc::Src::T(srv_index_depth, kTransferSRVRegisterDepth, dxbc::Src::kXXXX));
      }
      if (srv_index_stencil != UINT32_MAX) {
        a.OpLd(dxbc::Dest::R(source_load_register, 0b0001), source_coordinates, 0b1011,
               dxbc::Src::T(srv_index_stencil, kTransferSRVRegisterStencil, dxbc::Src::kYYYY));
      } else if (srv_index_color != UINT32_MAX) {
        a.OpLd(dxbc::Dest::R(source_load_register, source_color_srv_component_mask),
               source_coordinates, 0b1011,
               dxbc::Src::T(srv_index_color, kTransferSRVRegisterColor));
      }
      if (source_load_is_two_dwords && !i) {
        a.OpOr(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::LU(1));
      }
    }
  }
  if (source_is_64bpp && !dest_is_64bpp) {
    u32 source_color_half_component_count = source_color_format_component_count >> 1;
    if (dest_is_stencil_bit) {
      a.OpMovC(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kWWWW),
               dxbc::Src::R(1).Select(source_color_half_component_count),
               dxbc::Src::R(1, dxbc::Src::kXXXX));
    } else {
      u32 color_high_dword_swizzle =
          (source_color_half_component_count * 0b01010101) &
          ~((static_cast<u32>(1) << (source_color_half_component_count * 2)) - 1);
      for (u32 i = 0; i < source_color_half_component_count; ++i) {
        color_high_dword_swizzle |= (source_color_half_component_count + i) << (i * 2);
      }
      a.OpMovC(dxbc::Dest::R(1, (1 << source_color_half_component_count) - 1),
               dxbc::Src::R(0, dxbc::Src::kWWWW), dxbc::Src::R(1, color_high_dword_swizzle),
               dxbc::Src::R(1));
    }
  }

  if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX && srv_index_stencil != UINT32_MAX) {
    // Depth to depth writes the stencil loaded to r1.x directly to the output.
    REX_ASSERT(mode.output == TransferOutput::kDepth);
    a.OpMov(dxbc::Dest::OStencilRef(), dxbc::Src::R(1, dxbc::Src::kXXXX));
  }

  if (dest_is_64bpp) {
    // color_packed_in_r0x_and_r1x combines raw 32 bit samples from r0.x and r1.x generically.
    bool color_packed_in_r0x_and_r1x = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          // 8_8_8_8_GAMMA is stored as linear in R16G16B16A16_UNORM.
          for (u32 i = 0; i < 2; ++i) {
            for (u32 j = 0; j < 3; ++j) {
              DXBCShaderTranslator::PreSaturatedLinearToPWLGamma(a, i, j, i, j, 2, 0, 2, 1);
            }
          }
        }
          [[fallthrough]];
        case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
          color_packed_in_r0x_and_r1x = true;
          for (u32 i = 0; i < 2; ++i) {
            a.OpMAd(dxbc::Dest::R(i), dxbc::Src::R(i), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(i), dxbc::Src::R(i));
            for (u32 j = 1; j < 4; ++j) {
              a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(j * 8),
                      dxbc::Src::R(i).Select(j), dxbc::Src::R(i, dxbc::Src::kXXXX));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          color_packed_in_r0x_and_r1x = true;
          for (u32 i = 0; i < 2; ++i) {
            a.OpMAd(dxbc::Dest::R(i), dxbc::Src::R(i),
                    dxbc::Src::LF(1023.0f, 1023.0f, 1023.0f, 3.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(i), dxbc::Src::R(i));
            for (u32 j = 1; j < 4; ++j) {
              a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(j == 3 ? 2 : 10),
                      dxbc::Src::LU(j * 10), dxbc::Src::R(i).Select(j),
                      dxbc::Src::R(i, dxbc::Src::kXXXX));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          color_packed_in_r0x_and_r1x = true;
          for (u32 i = 0; i < 2; ++i) {
            // Float16 has a wider range for color and alpha, and NaNs, so clamp and convert.
            for (u32 j = 0; j < 3; ++j) {
              DXBCShaderTranslator::UnclampedFloat32To7e3(a, i, j, i, j, 2, 0);
              if (j) {
                a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(10), dxbc::Src::LU(j * 10),
                        dxbc::Src::R(i).Select(j), dxbc::Src::R(i, dxbc::Src::kXXXX));
              }
            }
            a.OpMov(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW), true);
            a.OpMAd(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW),
                    dxbc::Src::LF(3.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW));
            a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(2), dxbc::Src::LU(30),
                    dxbc::Src::R(i, dxbc::Src::kWWWW), dxbc::Src::R(i, dxbc::Src::kXXXX));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
          if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            for (u32 i = 0; i < 2; ++i) {
              a.OpBFI(dxbc::Dest::O(0, 1 << i), dxbc::Src::LU(16), dxbc::Src::LU(16),
                      dxbc::Src::R(i, dxbc::Src::kYYYY), dxbc::Src::R(i, dxbc::Src::kXXXX));
            }
          } else {
            a.OpMov(dxbc::Dest::O(0, 0b0011), dxbc::Src::R(0));
            a.OpMov(dxbc::Dest::O(0, 0b1100), dxbc::Src::R(1, 0b0100 << 4));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            a.OpBFI(dxbc::Dest::O(0, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(16),
                    dxbc::Src::R(1, 0b1101), dxbc::Src::R(1, 0b1000));
          } else {
            a.OpMov(dxbc::Dest::O(0), dxbc::Src::R(1));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
          color_packed_in_r0x_and_r1x = true;
        } break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            a.OpMov(dxbc::Dest::O(0, 0b0011), dxbc::Src::R(1));
          } else {
            a.OpUBFE(dxbc::Dest::O(0), dxbc::Src::LU(16), dxbc::Src::LU(0, 16, 0, 16),
                     dxbc::Src::R(1, 0b01010000));
          }
        } break;
      }
    } else {
      REX_ASSERT((rs & kTransferUsedRootParameterDepthSRVBit) != 0);
      color_packed_in_r0x_and_r1x = true;
      for (u32 i = 0; i < 2; ++i) {
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to nearest even, as adding +0.5 and truncating turns black into red.
            a.OpMul(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW),
                    dxbc::Src::LF(static_cast<f32>(0xFFFFFF)));
            a.OpRoundNE(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW));
            a.OpFToU(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            // Uses r1.y as a temp. Conversion in pixel shaders is exact, so it truncates.
            DXBCShaderTranslator::PreClampedDepthTo20e4(
                a, i, 3, i, 3, 1, 1,
                !GetDepthFloat24ConvertInPixelShader() && GetDepthFloat24Round(), true);
          } break;
        }
        a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(24), dxbc::Src::LU(8),
                dxbc::Src::R(i, dxbc::Src::kWWWW), dxbc::Src::R(i, dxbc::Src::kXXXX));
      }
    }
    if (color_packed_in_r0x_and_r1x) {
      if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
        a.OpMov(dxbc::Dest::O(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX));
        a.OpMov(dxbc::Dest::O(0, 0b0010), dxbc::Src::R(1, dxbc::Src::kXXXX));
      } else {
        for (u32 i = 0; i < 2; ++i) {
          a.OpUBFE(dxbc::Dest::O(0, 0b11 << (i * 2)), dxbc::Src::LU(16),
                   dxbc::Src::LU(0, 16, 0, 16), dxbc::Src::R(i, dxbc::Src::kXXXX));
        }
      }
    }
  } else {
    // color_packed_in_r1x means a raw 32bpp color value was written for common handling.
    bool color_packed_in_r1x = false;
    bool depth_loaded_in_guest_format = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (dest_is_stencil_bit) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
              DXBCShaderTranslator::PreSaturatedLinearToPWLGamma(a, 1, 0, 1, 0, 2, 0, 2, 1);
            }
            a.OpMAd(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
                    dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
                      dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
            if (source_color_format != dest_color_format) {
              if (dest_color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8) {
                for (u32 i = 0; i < 3; ++i) {
                  DXBCShaderTranslator::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 2, 0, 2, 1);
                }
              } else {
                for (u32 i = 0; i < 3; ++i) {
                  DXBCShaderTranslator::PWLGammaToLinear(a, 1, i, 1, i, true, 2, 0, 2, 1);
                }
              }
            }
            a.OpMov(dxbc::Dest::O(0), dxbc::Src::R(1));
          } else if (mode.output == TransferOutput::kDepth) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
              for (u32 i = osgn_parameter_index_sv_stencil_ref != UINT32_MAX ? 0 : 1; i < 3; ++i) {
                DXBCShaderTranslator::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 2, 0, 2, 1);
              }
            }
            // Skips the red component when only depth is needed, not stencil.
            a.OpMAd(dxbc::Dest::R(
                        1, osgn_parameter_index_sv_stencil_ref != UINT32_MAX ? 0b1111 : 0b1110),
                    dxbc::Src::R(1), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1, 0b1110), dxbc::Src::R(1));
            if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
              a.OpFToU(dxbc::Dest::OStencilRef(), dxbc::Src::R(1, dxbc::Src::kXXXX));
            }
            // r1.y = 0xGGBB0000.
            a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(8), dxbc::Src::LU(8),
                    dxbc::Src::R(1, dxbc::Src::kZZZZ), dxbc::Src::R(1, dxbc::Src::kYYYY));
            // r1.w = 0xGGBBAA00.
            a.OpBFI(dxbc::Dest::R(1, 0b1000), dxbc::Src::LU(8), dxbc::Src::LU(16),
                    dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::R(1, dxbc::Src::kYYYY));
          } else {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
              for (u32 i = 0; i < 3; ++i) {
                DXBCShaderTranslator::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 2, 0, 2, 1);
              }
            }
            color_packed_in_r1x = true;
            a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
            for (u32 i = 1; i < 4; ++i) {
              a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(i * 8),
                      dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          if (dest_is_stencil_bit) {
            a.OpMAd(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
                    dxbc::Src::LF(1023.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
                      dest_color_format ==
                          xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)) {
            a.OpMov(dxbc::Dest::O(0), dxbc::Src::R(1));
          } else {
            color_packed_in_r1x = true;
            a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1),
                    dxbc::Src::LF(1023.0f, 1023.0f, 1023.0f, 3.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
            for (u32 i = 1; i < 4; ++i) {
              a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(i == 3 ? 2 : 10),
                      dxbc::Src::LU(i * 10), dxbc::Src::R(1).Select(i),
                      dxbc::Src::R(1, dxbc::Src::kXXXX));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          if (dest_is_stencil_bit) {
            DXBCShaderTranslator::UnclampedFloat32To7e3(a, 1, 0, 1, 0, 2, 0);
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
                      dest_color_format ==
                          xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
            a.OpMov(dxbc::Dest::O(0), dxbc::Src::R(1));
          } else {
            color_packed_in_r1x = true;
            // Float16 has a wider range for color and alpha, and NaNs, so clamp and convert.
            for (u32 i = 0; i < 3; ++i) {
              DXBCShaderTranslator::UnclampedFloat32To7e3(a, 1, i, 1, i, 2, 0);
              if (i) {
                a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(10), dxbc::Src::LU(i * 10),
                        dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
              }
            }
            a.OpMov(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW), true);
            a.OpMAd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                    dxbc::Src::LF(3.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
            a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(2), dxbc::Src::LU(30),
                    dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::R(1, dxbc::Src::kXXXX));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          if (dest_is_stencil_bit) {
            // High bits are unneeded as only the bit already loaded to red is checked.
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
                      dest_color_format == xenos::ColorRenderTargetFormat::k_16_16_FLOAT)) {
            a.OpMov(dxbc::Dest::O(0, 0b0011), dxbc::Src::R(1));
          } else {
            color_packed_in_r1x = true;
            a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(16), dxbc::Src::LU(16),
                    dxbc::Src::R(1, dxbc::Src::kYYYY), dxbc::Src::R(1, dxbc::Src::kXXXX));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          color_packed_in_r1x = true;
        } break;
      }
    } else if (rs & kTransferUsedRootParameterDepthSRVBit) {
      if (dest_is_color || dest_depth_format != source_depth_format) {
        // Reinterprets the depth as color or another depth format, converting it within r1.w.
        depth_loaded_in_guest_format = true;
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to nearest even, as adding +0.5 and truncating turns black into red.
            a.OpMul(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                    dxbc::Src::LF(static_cast<f32>(0xFFFFFF)));
            a.OpRoundNE(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
            a.OpFToU(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            // Uses r1.y as a temp. Conversion in pixel shaders is exact, so it truncates.
            DXBCShaderTranslator::PreClampedDepthTo20e4(
                a, 1, 3, 1, 3, 1, 1,
                !GetDepthFloat24ConvertInPixelShader() && GetDepthFloat24Round(), true);
          } break;
        }
        if (dest_is_color) {
          color_packed_in_r1x = true;
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(24), dxbc::Src::LU(8),
                  dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
      }
    }
    switch (mode.output) {
      case TransferOutput::kColor:
        // Unless a special path ran, unpack the raw 32bpp value. Any register is free as a temp.
        if (color_packed_in_r1x) {
          switch (dest_color_format) {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
              a.OpUBFE(dxbc::Dest::R(1), dxbc::Src::LU(8), dxbc::Src::LU(0, 8, 16, 24),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1), dxbc::Src::R(1));
              a.OpMul(dxbc::Dest::O(0), dxbc::Src::R(1), dxbc::Src::LF(1.0f / 255.0f));
            } break;
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
              // 8_8_8_8_GAMMA is stored as linear in R16G16B16A16_UNORM.
              a.OpUBFE(dxbc::Dest::R(1), dxbc::Src::LU(8), dxbc::Src::LU(0, 8, 16, 24),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1), dxbc::Src::R(1));
              a.OpMul(dxbc::Dest::R(1, 0b0111), dxbc::Src::R(1), dxbc::Src::LF(1.0f / 255.0f));
              a.OpMul(dxbc::Dest::O(0, 0b1000), dxbc::Src::R(1), dxbc::Src::LF(1.0f / 255.0f));
              for (u32 i = 0; i < 3; ++i) {
                DXBCShaderTranslator::PWLGammaToLinear(a, 1, i, 1, i, true, 0, 0, 0, 1);
              }
              a.OpMov(dxbc::Dest::O(0, 0b0111), dxbc::Src::R(1));
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
              a.OpUBFE(dxbc::Dest::R(1), dxbc::Src::LU(10, 10, 10, 2), dxbc::Src::LU(0, 10, 20, 30),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1), dxbc::Src::R(1));
              a.OpMul(dxbc::Dest::O(0), dxbc::Src::R(1),
                      dxbc::Src::LF(1.0f / 1023.0f, 1.0f / 1023.0f, 1.0f / 1023.0f, 1.0f / 3.0f));
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
              // Color uses r1.yz as temps.
              for (u32 i = 0; i < 3; ++i) {
                DXBCShaderTranslator::Float7e3To32(a, dxbc::Dest::O(0, 1 << i), 1, 0, i * 10, 1, 1,
                                                   1, 2);
              }
              a.OpUBFE(dxbc::Dest::R(1, 0b1000), dxbc::Src::LU(2), dxbc::Src::LU(30),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
              a.OpMul(dxbc::Dest::O(0, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                      dxbc::Src::LF(1.0f / 3.0f));
            } break;
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
              a.OpUBFE(dxbc::Dest::O(0, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(0, 16, 0, 0),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
            } break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
              a.OpMov(dxbc::Dest::O(0, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
            } break;
            default:
              // A 64bpp format (handled separately) or an invalid one.
              REX_UNHANDLED_CASE(dest_color_format);
          }
        }
        break;
      case TransferOutput::kDepth:
        if (source_is_color || depth_loaded_in_guest_format) {
          if (color_packed_in_r1x) {
            a.OpUBFE(dxbc::Dest::R(1, 0b1000), dxbc::Src::LU(24), dxbc::Src::LU(8),
                     dxbc::Src::R(1, dxbc::Src::kXXXX));
            if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
              // Depth to depth is handled earlier, near the stencil load.
              a.OpUBFE(dxbc::Dest::OStencilRef(), dxbc::Src::LU(8), dxbc::Src::LU(0),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
            }
          }
          // r1.w = guest depth. `if` suits values uniform across whole waves.
          if (rs & kTransferUsedRootParameterHostDepthSRVBit) {
            // Loads host depth to r0.x and writes it to r1.w when it matches the guest depth.

            // r0.xy = destination tile pixel, r0.z = destination tile index, r1.w = guest depth.

            if (key.host_depth_source_is_copy) {
              REX_ASSERT(key.host_depth_source_msaa_samples == xenos::MSAASamples::k1X);
              if (key.dest_msaa_samples >= xenos::MSAASamples::k2X) {
                if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
                  a.OpBFI(dxbc::Dest::R(0, 0b0001), dxbc::Src::LU(31), dxbc::Src::LU(1),
                          dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
                }
                // Native 2x vertical sample is 1/0 in bit 0, 4x and 2x as 4x is 0/1 in bit 1.
                if (key.dest_msaa_samples == xenos::MSAASamples::k2X && msaa_2x_supported_) {
                  a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                          dxbc::Src::R(0, dxbc::Src::kYYYY), dest_sample);
                  a.OpXOr(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
                          dxbc::Src::LU(1));
                } else {
                  // r0.w is a temp.
                  a.OpUShR(dxbc::Dest::R(0, 0b1000), dest_sample, dxbc::Src::LU(1));
                  a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                          dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(0, dxbc::Src::kWWWW));
                }
              }
              // The tile index is unwrapped as host depth is written from the buffer start.
              a.OpUMAd(dxbc::Dest::R(0, 0b0001), dxbc::Src::LU(tile_width_samples),
                       dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(0, dxbc::Src::kXXXX));
              a.OpUMAd(dxbc::Dest::R(0, 0b0001),
                       dxbc::Src::LU(tile_width_samples * tile_height_samples),
                       dxbc::Src::R(0, dxbc::Src::kZZZZ), dxbc::Src::R(0, dxbc::Src::kXXXX));
              a.OpLd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), 0b0001,
                     dxbc::Src::T(srv_index_host_depth, kTransferSRVRegisterHostDepth,
                                  dxbc::Src::kXXXX));
            } else {
              a.OpIBFE(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(xenos::kEDRAMBaseTilesBits + 1),
                       dxbc::Src::LU(xenos::kEDRAMPitchTilesBits * 2),
                       dxbc::Src::CB(cbuffer_index_host_depth_address,
                                     kTransferCBVRegisterHostDepthAddress, 0, dxbc::Src::kXXXX));
              // A negative sum is a transfer across addressing wrapping, fixed by the mask below.
              a.OpIAdd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kZZZZ),
                       dxbc::Src::R(0, dxbc::Src::kWWWW));
              a.OpAnd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kZZZZ),
                      dxbc::Src::LU(xenos::kEDRAMTileCount - 1));
              // Same conversion as for the guest source, for 32bpp to 32bpp only.
              dxbc::Src host_depth_source_sample(dest_sample);
              if (key.host_depth_source_msaa_samples != key.dest_msaa_samples) {
                if (key.host_depth_source_msaa_samples >= xenos::MSAASamples::k4X) {
                  if (key.dest_msaa_samples == xenos::MSAASamples::k2X) {
                    // Horizontal pixels to samples, vertical sample to the second bit.
                    host_depth_source_sample = dxbc::Src::R(0, dxbc::Src::kWWWW);
                    if (msaa_2x_supported_) {
                      a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(31), dxbc::Src::LU(1),
                              dest_sample, dxbc::Src::R(0, dxbc::Src::kXXXX));
                      a.OpXOr(dxbc::Dest::R(0, 0b1000), host_depth_source_sample,
                              dxbc::Src::LU(1 << 1));
                    } else {
                      a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(0),
                              dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
                    }
                    a.OpUShR(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
                             dxbc::Src::LU(1));
                  } else {
                    a.OpAnd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kXXXX),
                            dxbc::Src::LU(1));
                    host_depth_source_sample = dxbc::Src::R(0, dxbc::Src::kWWWW);
                    a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(1),
                            dxbc::Src::R(0, dxbc::Src::kYYYY), host_depth_source_sample);
                    a.OpUShR(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0), dxbc::Src::LU(1));
                  }
                } else {
                  // Only X is handled here, and Y by common code.
                  if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
                    a.OpBFI(dxbc::Dest::R(0, 0b0001), dxbc::Src::LU(31), dxbc::Src::LU(1),
                            dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
                  }
                }
                // Host depth source Y and sample index for 1x and 2x sources.
                if (key.host_depth_source_msaa_samples < xenos::MSAASamples::k4X) {
                  if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
                    if (key.host_depth_source_msaa_samples == xenos::MSAASamples::k2X) {
                      // Vertical samples (second bit) to source samples (1, 0 native, 0, 3 as 4x).
                      a.OpUShR(dxbc::Dest::R(0, 0b1000), dest_sample, dxbc::Src::LU(1));
                      host_depth_source_sample = dxbc::Src::R(0, dxbc::Src::kWWWW);
                      if (msaa_2x_supported_) {
                        a.OpXOr(dxbc::Dest::R(0, 0b1000), host_depth_source_sample,
                                dxbc::Src::LU(1));
                      } else {
                        a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(1),
                                host_depth_source_sample, host_depth_source_sample);
                      }
                    } else {
                      // Vertical samples (second bit) to Y pixels, with r0.w as a temp.
                      a.OpUShR(dxbc::Dest::R(0, 0b1000), dest_sample, dxbc::Src::LU(1));
                      a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                              dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(0, dxbc::Src::kWWWW));
                    }
                  } else {
                    if (key.host_depth_source_msaa_samples == xenos::MSAASamples::k2X) {
                      // Vertical pixels to vertical samples (1, 0 native 2x, 0, 3 2x as 4x).
                      a.OpAnd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kYYYY),
                              dxbc::Src::LU(1));
                      host_depth_source_sample = dxbc::Src::R(0, dxbc::Src::kWWWW);
                      if (msaa_2x_supported_) {
                        a.OpXOr(dxbc::Dest::R(0, 0b1000), host_depth_source_sample,
                                dxbc::Src::LU(1));
                      } else {
                        a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(1),
                                host_depth_source_sample, host_depth_source_sample);
                      }
                      a.OpUShR(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
                               dxbc::Src::LU(1));
                    } else {
                      // Vertical samples to vertical pixels, with r0.w as a temp.
                      if (msaa_2x_supported_) {
                        a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                                dxbc::Src::R(0, dxbc::Src::kYYYY), dest_sample);
                        a.OpXOr(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
                                dxbc::Src::LU(1));
                      } else {
                        a.OpUShR(dxbc::Dest::R(0, 0b1000), dest_sample, dxbc::Src::LU(1));
                        a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                                dxbc::Src::R(0, dxbc::Src::kYYYY),
                                dxbc::Src::R(0, dxbc::Src::kWWWW));
                      }
                    }
                  }
                }
              }
              a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEDRAMPitchTilesBits),
                       dxbc::Src::LU(xenos::kEDRAMPitchTilesBits),
                       dxbc::Src::CB(cbuffer_index_host_depth_address,
                                     kTransferCBVRegisterHostDepthAddress, 0, dxbc::Src::kXXXX));
              a.OpUDiv(dxbc::Dest::R(0, 0b0100), dxbc::Dest::R(1, 0b0001),
                       dxbc::Src::R(0, dxbc::Src::kZZZZ), dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUMAd(dxbc::Dest::R(0, 0b0001),
                       dxbc::Src::LU(tile_width_samples >>
                                     static_cast<u32>(key.host_depth_source_msaa_samples >=
                                                      xenos::MSAASamples::k4X)),
                       dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::R(0, dxbc::Src::kXXXX));
              a.OpUMAd(dxbc::Dest::R(0, 0b0010),
                       dxbc::Src::LU(tile_height_samples >>
                                     static_cast<u32>(key.host_depth_source_msaa_samples >=
                                                      xenos::MSAASamples::k2X)),
                       dxbc::Src::R(0, dxbc::Src::kZZZZ), dxbc::Src::R(0, dxbc::Src::kYYYY));
              if (key.host_depth_source_msaa_samples != xenos::MSAASamples::k1X) {
                a.OpLdMS(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0), 0b0011,
                         dxbc::Src::T(srv_index_host_depth, kTransferSRVRegisterHostDepth,
                                      dxbc::Src::kXXXX),
                         host_depth_source_sample);
              } else {
                a.OpMov(dxbc::Dest::R(0, 0b0100), dxbc::Src::LU(0));
                a.OpLd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, 0b10000100), 0b1011,
                       dxbc::Src::T(srv_index_host_depth, kTransferSRVRegisterHostDepth,
                                    dxbc::Src::kXXXX));
              }
            }
            // Host depth in r0.x converts to the guest format in r0.y, with r0.z as a temp.
            switch (dest_depth_format) {
              case xenos::DepthRenderTargetFormat::kD24S8: {
                // Round to nearest even, as adding +0.5 and truncating turns black into red.
                a.OpMul(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kXXXX),
                        dxbc::Src::LF(static_cast<f32>(0xFFFFFF)));
                a.OpRoundNE(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY));
                a.OpFToU(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY));
              } break;
              case xenos::DepthRenderTargetFormat::kD24FS8: {
                // Conversion in pixel shaders is exact, so it truncates.
                DXBCShaderTranslator::PreClampedDepthTo20e4(
                    a, 0, 1, 0, 0, 0, 2,
                    !GetDepthFloat24ConvertInPixelShader() && GetDepthFloat24Round(), true);
              } break;
            }
            a.OpIEq(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
                    dxbc::Src::R(1, dxbc::Src::kWWWW));
            a.OpIf(true, dxbc::Src::R(0, dxbc::Src::kYYYY));
            // Up to date host depth is written to oDepth at host precision.
            a.OpMov(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(0, dxbc::Src::kXXXX));
            a.OpElse();
          }
          // Converts with r0.x as a temp.
          switch (dest_depth_format) {
            case xenos::DepthRenderTargetFormat::kD24S8: {
              a.OpUShR(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(1, dxbc::Src::kWWWW),
                       dxbc::Src::LU(23));
              a.OpIAdd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                       dxbc::Src::R(0, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
              a.OpMul(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                      dxbc::Src::LF(1.0f / static_cast<f32>(1 << 24)));
            } break;
            case xenos::DepthRenderTargetFormat::kD24FS8: {
              DXBCShaderTranslator::Depth20e4To32(a, dxbc::Dest::R(1, 0b1000), 1, 3, 0, 1, 3, 0, 0,
                                                  true);
            } break;
          }
          if (rs & kTransferUsedRootParameterHostDepthSRVBit) {
            a.OpEndIf();
          }
        }
        a.OpMov(dxbc::Dest::ODepth(), dxbc::Src::R(1, dxbc::Src::kWWWW));
        break;
      case TransferOutput::kStencilBit:
        REX_ASSERT(cbuffer_index_stencil_mask != UINT32_MAX);
        a.OpAnd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
                dxbc::Src::CB(cbuffer_index_stencil_mask, kTransferCBVRegisterStencilMask, 0,
                              dxbc::Src::kXXXX));
        a.OpDiscard(false, dxbc::Src::R(0, dxbc::Src::kXXXX));
        break;
    }
  }

  if (dest_is_color) {
    u32 dest_color_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(dest_color_format);
    u32 dest_color_unwritten_mask =
        0b1111 & ~static_cast<u32>((1 << dest_color_component_count) - 1);
    if (dest_color_component_count < 4) {
      a.OpMov(dxbc::Dest::O(0, dest_color_unwritten_mask), dxbc::Src::LU(0));
    }
  }

  a.OpRet();

  built_shader_[shex_position_dwords + 1] =
      static_cast<u32>(built_shader_.size()) - shex_position_dwords;

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = static_cast<u32>(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             built_shader_[blob_offset_position_dwords++];
  }

  if (shader_uses_stencil_reference_output) {
    built_shader_[blob_offset_position_dwords] =
        static_cast<u32>(blob_position_dwords * sizeof(u32));
    u32 sfi0_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
    built_shader_.resize(sfi0_position_dwords + sizeof(dxbc::ShaderFeatureInfo) / sizeof(u32));
    auto& shader_feature_info =
        *reinterpret_cast<dxbc::ShaderFeatureInfo*>(built_shader_.data() + sfi0_position_dwords);
    shader_feature_info.feature_flags[0] |= dxbc::kShaderFeature0StencilRef;
    {
      auto& blob_header =
          *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
      blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderFeatureInfo;
      blob_position_dwords = static_cast<u32>(built_shader_.size());
      blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                               built_shader_[blob_offset_position_dwords++];
    }
  }

  built_shader_[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 stat_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(stat_position_dwords + sizeof(dxbc::Statistics) / sizeof(u32));
  std::memcpy(built_shader_.data() + stat_position_dwords, &stat, sizeof(dxbc::Statistics));
  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = static_cast<u32>(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             built_shader_[blob_offset_position_dwords++];
  }

  u32 built_shader_size_bytes = static_cast<u32>(built_shader_.size() * sizeof(u32));
  {
    auto& container_header = *reinterpret_cast<dxbc::ContainerHeader*>(built_shader_.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = built_shader_size_bytes;
    container_header.blob_count = blob_count;
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(built_shader_.data()),
                          static_cast<unsigned int>(built_shader_size_bytes),
                          container_header.hash);
  }

  ID3D12PipelineState* const* pipelines;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  D3D12_INPUT_ELEMENT_DESC pipeline_input_element_desc;
  pipeline_input_element_desc.SemanticName = "POSITION";
  pipeline_input_element_desc.SemanticIndex = 0;
  pipeline_input_element_desc.Format = DXGI_FORMAT_R32G32_FLOAT;
  pipeline_input_element_desc.InputSlot = 0;
  pipeline_input_element_desc.AlignedByteOffset = 0;
  pipeline_input_element_desc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
  pipeline_input_element_desc.InstanceDataStepRate = 0;
  auto pipeline_desc = objects::BuildGraphicsPipelineStateDesc();
  pipeline_desc.pRootSignature = transfer_root_signatures_[static_cast<size_t>(
      use_stencil_reference_output_ ? mode.root_signature_with_stencil_ref
                                    : mode.root_signature_no_stencil_ref)];
  pipeline_desc.VS.pShaderBytecode = shaders::kPassthroughPositionXYVS;
  pipeline_desc.VS.BytecodeLength = sizeof(shaders::kPassthroughPositionXYVS);
  pipeline_desc.PS.pShaderBytecode = built_shader_.data();
  pipeline_desc.PS.BytecodeLength = built_shader_size_bytes;
  if (key.dest_msaa_samples == xenos::MSAASamples::k2X && !msaa_2x_supported_) {
    // Emulated 2x uses samples 0 and 3 of 4x as its samples 0 and 1.
    pipeline_desc.SampleMask = 0b1001;
    pipeline_desc.SampleDesc.Count = 4;
  } else {
    pipeline_desc.SampleMask = UINT_MAX;
    pipeline_desc.SampleDesc.Count = UINT(1) << UINT(key.dest_msaa_samples);
  }
  pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
  pipeline_desc.InputLayout.pInputElementDescs = &pipeline_input_element_desc;
  pipeline_desc.InputLayout.NumElements = 1;
  pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  if (dest_is_stencil_bit) {
    pipeline_desc.DepthStencilState.StencilEnable = TRUE;
    pipeline_desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    pipeline_desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    pipeline_desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    pipeline_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline_desc.DepthStencilState.BackFace = pipeline_desc.DepthStencilState.FrontFace;
    pipeline_desc.DSVFormat = GetDepthDSVDXGIFormat(dest_depth_format);
    // Null pointers are stored on failure too, so creation is not retried.
    std::array<ID3D12PipelineState*, 8>& stencil_bit_pipelines =
        transfer_stencil_bit_pipelines_
            .emplace(std::piecewise_construct, std::make_tuple(key), std::make_tuple())
            .first->second;
    bool stencil_pipelines_created = true;
    for (u32 i = 0; i < 8; ++i) {
      pipeline_desc.DepthStencilState.StencilWriteMask = UINT8(1) << i;
      if (SUCCEEDED(device->CreateGraphicsPipelineState(&pipeline_desc,
                                                        IID_PPV_ARGS(&stencil_bit_pipelines[i])))) {
        continue;
      }
      stencil_pipelines_created = false;
      for (u32 j = 0; j < i; ++j) {
        stencil_bit_pipelines[j]->Release();
        stencil_bit_pipelines[j] = nullptr;
      }
      break;
    }
    pipelines = stencil_pipelines_created ? stencil_bit_pipelines.data() : nullptr;
  } else {
    if (dest_is_color) {
      pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pipeline_desc.NumRenderTargets = 1;
      pipeline_desc.RTVFormats[0] = GetColorOwnershipTransferDXGIFormat(dest_color_format);
    } else {
      pipeline_desc.DepthStencilState.DepthEnable = TRUE;
      pipeline_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      pipeline_desc.DepthStencilState.DepthFunc =
          REXCVAR_GET(::rex::gpu::cvars::depth_transfer_not_equal_test)
              ? D3D12_COMPARISON_FUNC_NOT_EQUAL
              : D3D12_COMPARISON_FUNC_ALWAYS;
      if (use_stencil_reference_output_) {
        pipeline_desc.DepthStencilState.StencilEnable = TRUE;
        pipeline_desc.DepthStencilState.StencilWriteMask = UINT8_MAX;
        pipeline_desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        pipeline_desc.DepthStencilState.FrontFace.StencilDepthFailOp =
            REXCVAR_GET(::rex::gpu::cvars::depth_transfer_not_equal_test) ? D3D12_STENCIL_OP_REPLACE
                                                                          : D3D12_STENCIL_OP_KEEP;
        pipeline_desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
        // ALWAYS, not NOT_EQUAL, so a different stencil does not affect depth writing.
        pipeline_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pipeline_desc.DepthStencilState.BackFace = pipeline_desc.DepthStencilState.FrontFace;
      }
      pipeline_desc.DSVFormat = GetDepthDSVDXGIFormat(dest_depth_format);
    }
    ID3D12PipelineState* pipeline;
    if (FAILED(device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)))) {
      pipeline = nullptr;
    }
    // The null pointer is stored on failure too, and the result points to the persistent entry.
    ID3D12PipelineState*& inserted_pipeline =
        transfer_pipelines_.emplace(key, pipeline).first->second;
    pipelines = inserted_pipeline ? &inserted_pipeline : nullptr;
  }

  if (!pipelines) {
    // Stencil bit copying uses only the stencil SRV, so srv_index_depth can't identify the source.
    const char* source_format_name =
        (rs & kTransferUsedRootParameterColorSRVBit)
            ? xenos::GetColorRenderTargetFormatName(source_color_format)
            : xenos::GetDepthRenderTargetFormatName(source_depth_format);
    const char* dest_format_name = mode.output == TransferOutput::kColor
                                       ? xenos::GetColorRenderTargetFormatName(dest_color_format)
                                       : xenos::GetDepthRenderTargetFormatName(dest_depth_format);
    if (srv_index_host_depth != UINT32_MAX) {
      REXLOG_ERROR(gpu,
                   "D3D12RenderTargetCache: Failed to create a render target ownership "
                   "transfer pipeline for {}-sample {} + {}-sample host depth{} -> "
                   "{}-sample {} for mode {}",
                   static_cast<u32>(1) << u32(key.source_msaa_samples), source_format_name,
                   static_cast<u32>(1) << u32(key.host_depth_source_msaa_samples),
                   key.host_depth_source_is_copy ? " copy" : "",
                   static_cast<u32>(1) << u32(key.dest_msaa_samples), dest_format_name,
                   static_cast<u32>(key.mode));
    } else {
      REXLOG_ERROR(gpu,
                   "D3D12RenderTargetCache: Failed to create a render target ownership "
                   "transfer pipeline for {}-sample {} -> {}-sample {} for mode {}",
                   static_cast<u32>(1) << u32(key.source_msaa_samples), source_format_name,
                   static_cast<u32>(1) << u32(key.dest_msaa_samples), dest_format_name,
                   static_cast<u32>(key.mode));
    }
  }
  return pipelines;
}

void D3D12RenderTargetCache::PerformTransfersAndResolveClears(
    u32 render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const u64* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle) {
  REX_ASSERT(GetPath() == Path::kHostRenderTargets);

  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  u64 current_submission = command_processor_.GetCurrentSubmission();
  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();

  bool resolve_clear_needed = render_target_resolve_clear_values && resolve_clear_rectangle;
  D3D12_RECT clear_rect;
  if (resolve_clear_needed) {
    // The rectangle is already clamped by the common render target cache setup.
    clear_rect.left = static_cast<LONG>(resolve_clear_rectangle->x_pixels) *
                      static_cast<LONG>(GetDrawResolutionScaleX());
    clear_rect.top = static_cast<LONG>(resolve_clear_rectangle->y_pixels) *
                     static_cast<LONG>(GetDrawResolutionScaleY());
    clear_rect.right = (static_cast<LONG>(resolve_clear_rectangle->x_pixels) +
                        static_cast<LONG>(resolve_clear_rectangle->width_pixels)) *
                       static_cast<LONG>(GetDrawResolutionScaleX());
    clear_rect.bottom = (static_cast<LONG>(resolve_clear_rectangle->y_pixels) +
                         static_cast<LONG>(resolve_clear_rectangle->height_pixels)) *
                        static_cast<LONG>(GetDrawResolutionScaleY());
  }

  // Stores host depth for the single depth destination whose host depth source is itself.
  bool host_depth_store_set_up = false;
  for (u32 i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_d3d12_rt = *static_cast<D3D12RenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_d3d12_rt.GetKey();
    if (!dest_rt_key.is_depth) {
      continue;
    }
    const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
    for (const Transfer& transfer : depth_transfers) {
      if (transfer.host_depth_source != dest_rt) {
        continue;
      }
      if (!host_depth_store_set_up) {
        // Descriptor 0 is the source, and 1 is EDRAM when bindful.
        gpu::d3d12::objects::DescriptorCPUGPUHandlePair host_depth_store_descriptors[2];
        if (!command_processor_.RequestOneUseSingleViewDescriptors(
                1 + static_cast<u32>(!bindless_resources_used_), host_depth_store_descriptors)) {
          continue;
        }
        command_list.D3DSetComputeRootSignature(host_depth_store_root_signature_);
        if (bindless_resources_used_) {
          command_list.D3DSetComputeRootDescriptorTable(
              kHostDepthStoreRootParameterDest,
              command_processor_.GetEDRAMUintPow2BindlessUAVHandlePair(4).second);
        } else {
          const gpu::d3d12::objects::DescriptorCPUGPUHandlePair& host_depth_store_descriptor_dest =
              host_depth_store_descriptors[1];
          WriteEDRAMUintPow2UAVDescriptor(host_depth_store_descriptor_dest.first, 4);
          command_list.D3DSetComputeRootDescriptorTable(kHostDepthStoreRootParameterDest,
                                                        host_depth_store_descriptor_dest.second);
        }
        const gpu::d3d12::objects::DescriptorCPUGPUHandlePair& host_depth_store_descriptor_source =
            host_depth_store_descriptors[0];
        device->CopyDescriptorsSimple(1, host_depth_store_descriptor_source.first,
                                      dest_d3d12_rt.GetDescriptorSRV().GetHandle(),
                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        command_list.D3DSetComputeRootDescriptorTable(kHostDepthStoreRootParameterSource,
                                                      host_depth_store_descriptor_source.second);
        HostDepthStoreRenderTargetConstant host_depth_store_render_target_constant =
            GetHostDepthStoreRenderTargetConstant(dest_rt_key.pitch_tiles_at_32bpp,
                                                  msaa_2x_supported_);
        command_list.D3DSetComputeRoot32BitConstants(
            kHostDepthStoreRootParameterConstants,
            sizeof(host_depth_store_render_target_constant) / sizeof(u32),
            &host_depth_store_render_target_constant,
            offsetof(HostDepthStoreConstants, render_target) / sizeof(u32));
        // Not combined with transfer barriers, as the transition to DEPTH_WRITE follows anyway.
        TransitionEDRAMBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        command_processor_.PushTransitionBarrier(
            dest_d3d12_rt.GetResource(),
            dest_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        command_processor_.SetExternalPipeline(
            host_depth_store_pipelines_[static_cast<size_t>(dest_rt_key.msaa_samples)]);
        host_depth_store_set_up = true;
      }
      Transfer::Rectangle transfer_rectangles[Transfer::kMaxRectanglesWithCutout];
      u32 transfer_rectangle_count = transfer.GetRectangles(
          dest_rt_key.base_tiles, dest_rt_key.pitch_tiles_at_32bpp, dest_rt_key.msaa_samples, false,
          transfer_rectangles, resolve_clear_rectangle);
      REX_ASSERT(transfer_rectangle_count != 0);
      HostDepthStoreRectangleConstant host_depth_store_rectangle_constant;
      for (u32 j = 0; j < transfer_rectangle_count; ++j) {
        u32 group_count_x, group_count_y;
        GetHostDepthStoreRectangleInfo(transfer_rectangles[j], dest_rt_key.msaa_samples,
                                       host_depth_store_rectangle_constant, group_count_x,
                                       group_count_y);
        command_list.D3DSetComputeRoot32BitConstants(
            kHostDepthStoreRootParameterConstants,
            sizeof(host_depth_store_rectangle_constant) / sizeof(u32),
            &host_depth_store_rectangle_constant,
            offsetof(HostDepthStoreConstants, rectangle) / sizeof(u32));
        command_processor_.SubmitBarriers();
        command_list.D3DDispatch(group_count_x, group_count_y, 1);
        MarkEDRAMBufferModified();
      }
    }
    break;
  }

  for (u32 i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_d3d12_rt = *static_cast<D3D12RenderTarget*>(dest_rt);
    const std::vector<Transfer>& dest_transfers = render_target_transfers[i];
    if (!resolve_clear_needed && dest_transfers.empty()) {
      continue;
    }
    // Sources transition only if not used as destinations earlier.
    for (const Transfer& transfer : render_target_transfers[i]) {
      bool source_previously_used_as_dest = false;
      bool host_depth_source_previously_used_as_dest = false;
      for (u32 j = 0; j < i; ++j) {
        if (render_target_transfers[j].empty()) {
          continue;
        }
        const RenderTarget* previous_rt = render_targets[j];
        if (transfer.source == previous_rt) {
          source_previously_used_as_dest = true;
        }
        if (transfer.host_depth_source == previous_rt) {
          host_depth_source_previously_used_as_dest = true;
        }
      }
      if (!source_previously_used_as_dest) {
        auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
        command_processor_.PushTransitionBarrier(
            source_d3d12_rt.GetResource(),
            source_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      // host_depth_source == dest_rt reads the EDRAM buffer instead, so no transition.
      if (transfer.host_depth_source && transfer.host_depth_source != dest_rt &&
          !host_depth_source_previously_used_as_dest) {
        auto& host_depth_source_d3d12_rt =
            *static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
        command_processor_.PushTransitionBarrier(
            host_depth_source_d3d12_rt.GetResource(),
            host_depth_source_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
    }
    // The destination transitions only if not used as a source earlier.
    bool dest_used_previously_as_source = false;
    for (u32 j = 0; j < i; ++j) {
      for (const Transfer& previous_transfer : render_target_transfers[j]) {
        if (previous_transfer.source == dest_rt || previous_transfer.host_depth_source == dest_rt) {
          dest_used_previously_as_source = true;
          break;
        }
      }
    }
    if (!dest_used_previously_as_source) {
      D3D12_RESOURCE_STATES dest_state = dest_d3d12_rt.GetKey().is_depth
                                             ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                             : D3D12_RESOURCE_STATE_RENDER_TARGET;
      command_processor_.PushTransitionBarrier(
          dest_d3d12_rt.GetResource(), dest_d3d12_rt.SetResourceState(dest_state), dest_state);
    }
  }
  if (host_depth_store_set_up) {
    TransitionEDRAMBuffer(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  for (u32 i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    for (const Transfer& transfer : render_target_transfers[i]) {
      REX_ASSERT(transfer.source != nullptr);
      auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
      source_d3d12_rt.SetTemporarySRVDescriptorIndex(UINT32_MAX);
      source_d3d12_rt.SetTemporarySRVDescriptorIndexStencil(UINT32_MAX);
      auto* host_depth_source_d3d12_rt =
          static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
      if (host_depth_source_d3d12_rt) {
        host_depth_source_d3d12_rt->SetTemporarySRVDescriptorIndex(UINT32_MAX);
        host_depth_source_d3d12_rt->SetTemporarySRVDescriptorIndexStencil(UINT32_MAX);
      }
    }
  }
  current_temporary_descriptors_cpu_.clear();
  u32 host_depth_copy_srv_index;
  if (host_depth_store_set_up && !bindless_resources_used_) {
    host_depth_copy_srv_index = static_cast<u32>(current_temporary_descriptors_cpu_.size());
    current_temporary_descriptors_cpu_.push_back(
        provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                      std::to_underlying(EDRAMBufferDescriptorIndex::kR32UintSRV)));
  } else {
    host_depth_copy_srv_index = UINT32_MAX;
  }
  for (u32 i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    for (const Transfer& transfer : render_target_transfers[i]) {
      REX_ASSERT(transfer.source != nullptr);
      auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
      if (source_d3d12_rt.GetTemporarySRVDescriptorIndex() == UINT32_MAX) {
        source_d3d12_rt.SetTemporarySRVDescriptorIndex(
            static_cast<u32>(current_temporary_descriptors_cpu_.size()));
        current_temporary_descriptors_cpu_.push_back(
            source_d3d12_rt.GetDescriptorSRV().GetHandle());
      }
      if (source_d3d12_rt.GetKey().is_depth &&
          source_d3d12_rt.GetTemporarySRVDescriptorIndexStencil() == UINT32_MAX) {
        source_d3d12_rt.SetTemporarySRVDescriptorIndexStencil(
            static_cast<u32>(current_temporary_descriptors_cpu_.size()));
        current_temporary_descriptors_cpu_.push_back(
            source_d3d12_rt.GetDescriptorSRVStencil().GetHandle());
      }
      auto* host_depth_source_d3d12_rt =
          static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
      // host_depth_source == dest_rt uses the EDRAM buffer instead.
      if (host_depth_source_d3d12_rt && host_depth_source_d3d12_rt != dest_rt &&
          host_depth_source_d3d12_rt->GetTemporarySRVDescriptorIndex() == UINT32_MAX) {
        host_depth_source_d3d12_rt->SetTemporarySRVDescriptorIndex(
            static_cast<u32>(current_temporary_descriptors_cpu_.size()));
        current_temporary_descriptors_cpu_.push_back(
            host_depth_source_d3d12_rt->GetDescriptorSRV().GetHandle());
      }
    }
  }
  u32 descriptor_count = static_cast<u32>(current_temporary_descriptors_cpu_.size());
  current_temporary_descriptors_gpu_.resize(descriptor_count);
  if (!command_processor_.RequestOneUseSingleViewDescriptors(
          descriptor_count, current_temporary_descriptors_gpu_.data())) {
    return;
  }
  for (u32 i = 0; i < descriptor_count; ++i) {
    device->CopyDescriptorsSimple(1, current_temporary_descriptors_gpu_[i].first,
                                  current_temporary_descriptors_cpu_[i],
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }

  bool transfer_viewport_set = false;
  f32 pixels_to_ndc_unscaled = 2.0f / static_cast<f32>(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
  f32 pixels_to_ndc_x = pixels_to_ndc_unscaled * static_cast<f32>(GetDrawResolutionScaleX());
  f32 pixels_to_ndc_y = pixels_to_ndc_unscaled * static_cast<f32>(GetDrawResolutionScaleY());

  TransferRootSignatureIndex last_transfer_root_signature_index =
      TransferRootSignatureIndex::kCount;
  u32 transfer_root_parameters_set = 0;
  u32 last_descriptor_index_color = UINT32_MAX;
  u32 last_descriptor_index_depth = UINT32_MAX;
  u32 last_descriptor_index_stencil = UINT32_MAX;
  u32 last_descriptor_index_host_depth_non_copy = UINT32_MAX;
  bool last_descriptor_host_depth_is_copy = false;
  TransferAddressConstant last_address_constant;
  TransferAddressConstant last_host_depth_address_constant;

  for (u32 i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& current_transfers = render_target_transfers[i];
    if (current_transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto& dest_d3d12_rt = *static_cast<D3D12RenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_d3d12_rt.GetKey();

    // Late barrier for cross copying that prevented merging barriers.
    D3D12_RESOURCE_STATES dest_state = dest_rt_key.is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                            : D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_processor_.PushTransitionBarrier(
        dest_d3d12_rt.GetResource(), dest_d3d12_rt.SetResourceState(dest_state), dest_state);

    if (!current_transfers.empty()) {
      are_current_command_list_render_targets_valid_ = false;
      if (dest_rt_key.is_depth) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dest_d3d12_rt.GetDescriptorDraw().GetHandle();
        command_list.D3DOMSetRenderTargets(0, nullptr, FALSE, &dsv_handle);
        if (!use_stencil_reference_output_) {
          command_processor_.SetStencilReference(UINT8_MAX);
        }
      } else {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle =
            dest_d3d12_rt.GetDescriptorLoadSeparate().IsValid()
                ? dest_d3d12_rt.GetDescriptorLoadSeparate().GetHandle()
                : dest_d3d12_rt.GetDescriptorDraw().GetHandle();
        command_list.D3DOMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
      }

      u32 dest_pitch_tiles = dest_rt_key.GetPitchTiles();
      bool dest_is_64bpp = dest_rt_key.Is64bpp();

      bool need_stencil_bit_draws = dest_rt_key.is_depth && !use_stencil_reference_output_;
      current_transfer_invocations_.clear();
      current_transfer_invocations_.reserve(current_transfers.size()
                                            << u32(need_stencil_bit_draws));
      u32 rt_sort_index = 0;
      TransferShaderKey new_transfer_shader_key;
      new_transfer_shader_key.dest_msaa_samples = dest_rt_key.msaa_samples;
      new_transfer_shader_key.dest_resource_format = dest_rt_key.resource_format;
      u32 stencil_clear_rectangle_count = 0;
      for (u32 j = 0; j <= static_cast<u32>(need_stencil_bit_draws); ++j) {
        // j == 1 writes stencil bits, with their own root signature and no host depth source.
        for (const Transfer& transfer : current_transfers) {
          auto* host_depth_source_d3d12_rt =
              static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_d3d12_rt) {
            host_depth_source_d3d12_rt->SetTemporarySortIndex(UINT32_MAX);
          }
          REX_ASSERT(transfer.source != nullptr);
          auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
          source_d3d12_rt.SetTemporarySortIndex(UINT32_MAX);
        }
        for (const Transfer& transfer : current_transfers) {
          REX_ASSERT(transfer.source != nullptr);
          auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
          D3D12RenderTarget* host_depth_source_d3d12_rt =
              j ? nullptr : static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_d3d12_rt &&
              host_depth_source_d3d12_rt->GetTemporarySortIndex() == UINT32_MAX) {
            host_depth_source_d3d12_rt->SetTemporarySortIndex(rt_sort_index++);
          }
          if (source_d3d12_rt.GetTemporarySortIndex() == UINT32_MAX) {
            source_d3d12_rt.SetTemporarySortIndex(rt_sort_index++);
          }
          RenderTargetKey source_rt_key = source_d3d12_rt.GetKey();
          new_transfer_shader_key.source_msaa_samples = source_rt_key.msaa_samples;
          new_transfer_shader_key.source_resource_format = source_rt_key.resource_format;
          bool host_depth_source_is_copy = host_depth_source_d3d12_rt == &dest_d3d12_rt;
          new_transfer_shader_key.host_depth_source_is_copy = host_depth_source_is_copy;
          // The host depth copy buffer has only raw samples.
          new_transfer_shader_key.host_depth_source_msaa_samples =
              (host_depth_source_d3d12_rt && !host_depth_source_is_copy)
                  ? host_depth_source_d3d12_rt->GetKey().msaa_samples
                  : xenos::MSAASamples::k1X;
          if (j) {
            new_transfer_shader_key.mode = source_rt_key.is_depth
                                               ? TransferMode::kDepthToStencilBit
                                               : TransferMode::kColorToStencilBit;
            stencil_clear_rectangle_count += transfer.GetRectangles(
                dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
                nullptr, resolve_clear_rectangle);
          } else {
            if (dest_rt_key.is_depth) {
              if (host_depth_source_d3d12_rt) {
                new_transfer_shader_key.mode = source_rt_key.is_depth
                                                   ? TransferMode::kDepthAndHostDepthToDepth
                                                   : TransferMode::kColorAndHostDepthToDepth;
              } else {
                new_transfer_shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToDepth
                                                                      : TransferMode::kColorToDepth;
              }
            } else {
              new_transfer_shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToColor
                                                                    : TransferMode::kColorToColor;
            }
          }
          current_transfer_invocations_.emplace_back(transfer, new_transfer_shader_key);
          if (j) {
            current_transfer_invocations_.back().transfer.host_depth_source = nullptr;
          }
        }
      }
      std::sort(current_transfer_invocations_.begin(), current_transfer_invocations_.end());

      // Clears the stencil to 0, as bits that need to be 1 are set by discarding samples.
      if (stencil_clear_rectangle_count) {
        command_processor_.SubmitBarriers();
        D3D12_RECT* stencil_clear_rect_write_ptr = command_list.ClearDepthStencilViewAllocatedRects(
            dest_d3d12_rt.GetDescriptorDraw().GetHandle(), D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0,
            stencil_clear_rectangle_count);
        REX_ASSERT(stencil_clear_rect_write_ptr != nullptr);
        for (const Transfer& transfer : current_transfers) {
          Transfer::Rectangle transfer_stencil_clear_rectangles[Transfer::kMaxRectanglesWithCutout];
          u32 transfer_stencil_clear_rectangle_count = transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_stencil_clear_rectangles, resolve_clear_rectangle);
          for (u32 j = 0; j < transfer_stencil_clear_rectangle_count; ++j) {
            const Transfer::Rectangle& stencil_clear_rectangle =
                transfer_stencil_clear_rectangles[j];
            stencil_clear_rect_write_ptr->left =
                static_cast<LONG>(stencil_clear_rectangle.x_pixels) *
                static_cast<LONG>(GetDrawResolutionScaleX());
            stencil_clear_rect_write_ptr->top =
                static_cast<LONG>(stencil_clear_rectangle.y_pixels) *
                static_cast<LONG>(GetDrawResolutionScaleY());
            stencil_clear_rect_write_ptr->right =
                (static_cast<LONG>(stencil_clear_rectangle.x_pixels) +
                 static_cast<LONG>(stencil_clear_rectangle.width_pixels)) *
                static_cast<LONG>(GetDrawResolutionScaleX());
            stencil_clear_rect_write_ptr->bottom =
                (static_cast<LONG>(stencil_clear_rectangle.y_pixels) +
                 static_cast<LONG>(stencil_clear_rectangle.height_pixels)) *
                static_cast<LONG>(GetDrawResolutionScaleY());
            ++stencil_clear_rect_write_ptr;
          }
        }
      }

      if (!transfer_viewport_set) {
        transfer_viewport_set = true;
        // NDC is passed directly, so the viewport is the maximum size, with a power of two scale.
        D3D12_VIEWPORT transfer_viewport;
        transfer_viewport.TopLeftX = 0.0f;
        transfer_viewport.TopLeftY = 0.0f;
        transfer_viewport.Width = static_cast<f32>(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        transfer_viewport.Height = static_cast<f32>(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        transfer_viewport.MinDepth = 0.0f;
        transfer_viewport.MaxDepth = 1.0f;
        command_processor_.SetViewport(transfer_viewport);
        D3D12_RECT transfer_scissor;
        transfer_scissor.left = 0;
        transfer_scissor.top = 0;
        transfer_scissor.right = LONG(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        transfer_scissor.bottom = LONG(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        command_processor_.SetScissorRect(transfer_scissor);
      }

      for (auto it = current_transfer_invocations_.cbegin();
           it != current_transfer_invocations_.cend(); ++it) {
        const TransferInvocation& transfer_invocation_first = *it;
        auto it_merged_first = it, it_merged_last = it;
        u32 transfer_rectangle_count = transfer_invocation_first.transfer.GetRectangles(
            dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
            nullptr, resolve_clear_rectangle);
        for (auto it_merge = std::next(it_merged_first);
             it_merge != current_transfer_invocations_.cend(); ++it_merge) {
          if (!transfer_invocation_first.CanBeMergedIntoOneDraw(*it_merge)) {
            break;
          }
          transfer_rectangle_count += it_merge->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              nullptr, resolve_clear_rectangle);
          it_merged_last = it_merge;
        }
        REX_ASSERT(transfer_rectangle_count != 0);
        it = it_merged_last;

        REX_ASSERT(it->transfer.source != nullptr);
        auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(it->transfer.source);
        auto* host_depth_source_d3d12_rt =
            static_cast<D3D12RenderTarget*>(it->transfer.host_depth_source);
        TransferShaderKey transfer_shader_key = it->shader_key;
        const TransferModeInfo& transfer_mode_info =
            kTransferModes[static_cast<size_t>(transfer_shader_key.mode)];
        TransferRootSignatureIndex transfer_root_signature_index =
            use_stencil_reference_output_ ? transfer_mode_info.root_signature_with_stencil_ref
                                          : transfer_mode_info.root_signature_no_stencil_ref;
        u32 transfer_root_parameters_used =
            kTransferUsedRootParameters[static_cast<size_t>(transfer_root_signature_index)];
        bool is_stencil_bit =
            (transfer_root_parameters_used & kTransferUsedRootParameterStencilMaskConstantBit) != 0;

        // Late barriers for cross copying that prevented merging barriers.
        command_processor_.PushTransitionBarrier(
            source_d3d12_rt.GetResource(),
            source_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (host_depth_source_d3d12_rt) {
          if (transfer_shader_key.host_depth_source_is_copy) {
            TransitionEDRAMBuffer(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
          } else {
            command_processor_.PushTransitionBarrier(
                host_depth_source_d3d12_rt->GetResource(),
                host_depth_source_d3d12_rt->SetResourceState(
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
          }
        }

        u32 transfer_vertex_count = 6 * transfer_rectangle_count;
        D3D12_VERTEX_BUFFER_VIEW transfer_rectangle_buffer_view;
        transfer_rectangle_buffer_view.StrideInBytes = sizeof(f32) * 2;
        transfer_rectangle_buffer_view.SizeInBytes =
            transfer_rectangle_buffer_view.StrideInBytes * transfer_vertex_count;
        f32* transfer_rectangle_write_ptr =
            reinterpret_cast<f32*>(transfer_vertex_buffer_pool_->Request(
                current_submission, transfer_rectangle_buffer_view.SizeInBytes, sizeof(f32),
                nullptr, nullptr, &transfer_rectangle_buffer_view.BufferLocation));
        if (!transfer_rectangle_write_ptr) {
          continue;
        }
        for (auto it_merged = it_merged_first; it_merged <= it_merged_last; ++it_merged) {
          Transfer::Rectangle transfer_invocation_rectangles[Transfer::kMaxRectanglesWithCutout];
          u32 transfer_invocation_rectangle_count = it_merged->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_invocation_rectangles, resolve_clear_rectangle);
          REX_ASSERT(transfer_invocation_rectangle_count != 0);
          for (u32 j = 0; j < transfer_invocation_rectangle_count; ++j) {
            const Transfer::Rectangle& transfer_rectangle = transfer_invocation_rectangles[j];
            f32 transfer_rectangle_x0 =
                -1.0f + static_cast<f32>(transfer_rectangle.x_pixels) * pixels_to_ndc_x;
            f32 transfer_rectangle_y0 =
                1.0f - static_cast<f32>(transfer_rectangle.y_pixels) * pixels_to_ndc_y;
            f32 transfer_rectangle_x1 =
                transfer_rectangle_x0 +
                static_cast<f32>(transfer_rectangle.width_pixels) * pixels_to_ndc_x;
            f32 transfer_rectangle_y1 =
                transfer_rectangle_y0 -
                static_cast<f32>(transfer_rectangle.height_pixels) * pixels_to_ndc_y;
            // Vertices are (x0, y0) (x1, y0) (x0, y1) then (x0, y1) (x1, y0) (x1, y1).
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
          }
        }
        command_processor_.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command_list.D3DIASetVertexBuffers(0, 1, &transfer_rectangle_buffer_view);

        ID3D12PipelineState* const* transfer_pipelines =
            GetOrCreateTransferPipelines(transfer_shader_key);
        if (!transfer_pipelines) {
          continue;
        }
        if (last_transfer_root_signature_index != transfer_root_signature_index) {
          last_transfer_root_signature_index = transfer_root_signature_index;
          command_processor_.SetExternalGraphicsRootSignature(
              transfer_root_signatures_[static_cast<size_t>(transfer_root_signature_index)]);
          transfer_root_parameters_set = 0;
        }

        if (transfer_root_parameters_used & kTransferUsedRootParameterColorSRVBit) {
          u32 descriptor_index_color = source_d3d12_rt.GetTemporarySRVDescriptorIndex();
          REX_ASSERT(descriptor_index_color != UINT32_MAX);
          if (last_descriptor_index_color != descriptor_index_color) {
            last_descriptor_index_color = descriptor_index_color;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterColorSRVBit;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterDepthSRVBit) {
          u32 descriptor_index_depth = source_d3d12_rt.GetTemporarySRVDescriptorIndex();
          REX_ASSERT(descriptor_index_depth != UINT32_MAX);
          if (last_descriptor_index_depth != descriptor_index_depth) {
            last_descriptor_index_depth = descriptor_index_depth;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterDepthSRVBit;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterStencilSRVBit) {
          u32 descriptor_index_stencil = source_d3d12_rt.GetTemporarySRVDescriptorIndexStencil();
          REX_ASSERT(descriptor_index_stencil != UINT32_MAX);
          if (last_descriptor_index_stencil != descriptor_index_stencil) {
            last_descriptor_index_stencil = descriptor_index_stencil;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterStencilSRVBit;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterHostDepthSRVBit) {
          if (transfer_shader_key.host_depth_source_is_copy) {
            if (!last_descriptor_host_depth_is_copy) {
              last_descriptor_host_depth_is_copy = true;
              transfer_root_parameters_set &= ~kTransferUsedRootParameterHostDepthSRVBit;
            }
          } else {
            REX_ASSERT(host_depth_source_d3d12_rt != nullptr);
            u32 descriptor_index_host_depth =
                host_depth_source_d3d12_rt->GetTemporarySRVDescriptorIndex();
            REX_ASSERT(descriptor_index_host_depth != UINT32_MAX);
            if (last_descriptor_host_depth_is_copy ||
                last_descriptor_index_host_depth_non_copy != descriptor_index_host_depth) {
              transfer_root_parameters_set &= ~kTransferUsedRootParameterHostDepthSRVBit;
            }
            last_descriptor_host_depth_is_copy = false;
            last_descriptor_index_host_depth_non_copy = descriptor_index_host_depth;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterAddressConstantBit) {
          RenderTargetKey source_rt_key = source_d3d12_rt.GetKey();
          TransferAddressConstant address_constant;
          address_constant.dest_pitch = dest_pitch_tiles;
          address_constant.source_pitch = source_rt_key.GetPitchTiles();
          address_constant.source_to_dest =
              static_cast<i32>(dest_rt_key.base_tiles) - static_cast<i32>(source_rt_key.base_tiles);
          if (last_address_constant != address_constant) {
            last_address_constant = address_constant;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterAddressConstantBit;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterHostDepthAddressConstantBit) {
          REX_ASSERT(host_depth_source_d3d12_rt != nullptr);
          RenderTargetKey host_depth_source_rt_key = host_depth_source_d3d12_rt->GetKey();
          TransferAddressConstant host_depth_address_constant;
          host_depth_address_constant.dest_pitch = dest_pitch_tiles;
          host_depth_address_constant.source_pitch = host_depth_source_rt_key.GetPitchTiles();
          host_depth_address_constant.source_to_dest =
              static_cast<i32>(dest_rt_key.base_tiles) -
              static_cast<i32>(host_depth_source_rt_key.base_tiles);
          if (last_host_depth_address_constant != host_depth_address_constant) {
            last_host_depth_address_constant = host_depth_address_constant;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterHostDepthAddressConstantBit;
          }
        }

        u32 transfer_root_parameters_unset =
            transfer_root_parameters_used & ~transfer_root_parameters_set;
        if (transfer_root_parameters_unset &
            kTransferUsedRootParameterHostDepthAddressConstantBit) {
          command_list.D3DSetGraphicsRoot32BitConstants(
              rex::BitCount(transfer_root_parameters_used &
                            (kTransferUsedRootParameterHostDepthAddressConstantBit - 1)),
              sizeof(last_host_depth_address_constant) / sizeof(u32),
              &last_host_depth_address_constant, 0);
          transfer_root_parameters_set |= kTransferUsedRootParameterHostDepthAddressConstantBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterHostDepthSRVBit) {
          D3D12_GPU_DESCRIPTOR_HANDLE descriptor_gpu_handle;
          if (last_descriptor_host_depth_is_copy) {
            if (bindless_resources_used_) {
              descriptor_gpu_handle =
                  command_processor_
                      .GetSystemBindlessViewHandlePair(
                          D3D12CommandProcessor::SystemBindlessView ::kEDRAMR32UintSRV)
                      .second;
            } else {
              REX_ASSERT(host_depth_copy_srv_index != UINT32_MAX);
              descriptor_gpu_handle =
                  current_temporary_descriptors_gpu_[host_depth_copy_srv_index].second;
            }
          } else {
            REX_ASSERT(last_descriptor_index_host_depth_non_copy != UINT32_MAX);
            descriptor_gpu_handle =
                current_temporary_descriptors_gpu_[last_descriptor_index_host_depth_non_copy]
                    .second;
          }
          command_list.D3DSetGraphicsRootDescriptorTable(
              rex::BitCount(transfer_root_parameters_used &
                            (kTransferUsedRootParameterHostDepthSRVBit - 1)),
              descriptor_gpu_handle);
          transfer_root_parameters_set |= kTransferUsedRootParameterHostDepthSRVBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterAddressConstantBit) {
          command_list.D3DSetGraphicsRoot32BitConstants(
              rex::BitCount(transfer_root_parameters_used &
                            (kTransferUsedRootParameterAddressConstantBit - 1)),
              sizeof(last_address_constant) / sizeof(u32), &last_address_constant, 0);
          transfer_root_parameters_set |= kTransferUsedRootParameterAddressConstantBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterStencilSRVBit) {
          REX_ASSERT(last_descriptor_index_stencil != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              rex::BitCount(transfer_root_parameters_used &
                            (kTransferUsedRootParameterStencilSRVBit - 1)),
              current_temporary_descriptors_gpu_[last_descriptor_index_stencil].second);
          transfer_root_parameters_set |= kTransferUsedRootParameterStencilSRVBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterDepthSRVBit) {
          REX_ASSERT(last_descriptor_index_depth != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              rex::BitCount(transfer_root_parameters_used &
                            (kTransferUsedRootParameterDepthSRVBit - 1)),
              current_temporary_descriptors_gpu_[last_descriptor_index_depth].second);
          transfer_root_parameters_set |= kTransferUsedRootParameterDepthSRVBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterColorSRVBit) {
          REX_ASSERT(last_descriptor_index_color != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              rex::BitCount(transfer_root_parameters_used &
                            (kTransferUsedRootParameterColorSRVBit - 1)),
              current_temporary_descriptors_gpu_[last_descriptor_index_color].second);
          transfer_root_parameters_set |= kTransferUsedRootParameterColorSRVBit;
        }

        command_processor_.SubmitBarriers();
        for (u32 j = 0; j <= static_cast<u32>(is_stencil_bit) * 7; ++j) {
          if (is_stencil_bit) {
            u32 transfer_stencil_bit = static_cast<u32>(1) << j;
            command_list.D3DSetGraphicsRoot32BitConstants(
                rex::BitCount(transfer_root_parameters_used &
                              (kTransferUsedRootParameterStencilMaskConstantBit - 1)),
                1, &transfer_stencil_bit, 0);
          }
          command_processor_.SetExternalPipeline(transfer_pipelines[j]);
          command_list.D3DDrawInstanced(transfer_vertex_count, 1, 0, 0);
        }
      }
    }

    if (resolve_clear_needed) {
      u64 clear_value = render_target_resolve_clear_values[i];
      if (dest_rt_key.is_depth) {
        u32 depth_guest_clear_value = (static_cast<u32>(clear_value) >> 8) & 0xFFFFFF;
        f32 depth_host_clear_value = 0.0f;
        switch (dest_rt_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            depth_host_clear_value = xenos::UNorm24To32(depth_guest_clear_value);
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            // The multiplier remaps [0, 2) to [0, 1).
            depth_host_clear_value = xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            break;
        }
        command_processor_.PushTransitionBarrier(
            dest_d3d12_rt.GetResource(),
            dest_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_DEPTH_WRITE),
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        command_processor_.SubmitBarriers();
        command_list.D3DClearDepthStencilView(dest_d3d12_rt.GetDescriptorDraw().GetHandle(),
                                              D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                              depth_host_clear_value, UINT(clear_value) & 0xFF, 1,
                                              &clear_rect);
      } else {
        f32 color_clear_value[4] = {};
        bool clear_via_drawing = false;
        switch (dest_rt_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
            for (u32 j = 0; j < 4; ++j) {
              color_clear_value[j] =
                  static_cast<f32>((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            // 8_8_8_8_GAMMA is stored as linear in R16G16B16A16_UNORM.
            for (u32 j = 0; j < 4; ++j) {
              color_clear_value[j] =
                  static_cast<f32>((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            for (u32 j = 0; j < 3; ++j) {
              color_clear_value[j] = xenos::PWLGammaToLinear(color_clear_value[j]);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (u32 j = 0; j < 3; ++j) {
              color_clear_value[j] =
                  static_cast<f32>((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            color_clear_value[3] = static_cast<f32>((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (u32 j = 0; j < 3; ++j) {
              color_clear_value[j] = xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            color_clear_value[3] = static_cast<f32>((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            // Uint loads both, ignoring the -32...32 vs. -1...1 setting like color clear via depth.
            for (u32 j = 0; j < 2; ++j) {
              color_clear_value[j] = static_cast<f32>((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            // Uint loads both, ignoring the -32...32 vs. -1...1 setting like color clear via depth.
            for (u32 j = 0; j < 4; ++j) {
              color_clear_value[j] = static_cast<f32>((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            // Uint preserves denormals and NaNs.
            color_clear_value[0] = static_cast<f32>(static_cast<u32>(clear_value));
            // Numbers above 2^24 lack a step of 1 as floats, so clear by drawing a uint rectangle.
            if (static_cast<u64>(color_clear_value[0]) != static_cast<u32>(clear_value)) {
              clear_via_drawing = true;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            // Uint preserves denormals and NaNs.
            color_clear_value[0] = static_cast<f32>(static_cast<u32>(clear_value));
            color_clear_value[1] = static_cast<f32>(static_cast<u32>(clear_value >> 32));
            // Numbers above 2^24 lack a step of 1 as floats, so clear by drawing a uint rectangle.
            if (static_cast<u64>(color_clear_value[0]) != static_cast<u32>(clear_value) ||
                static_cast<u64>(color_clear_value[1]) != static_cast<u32>(clear_value >> 32)) {
              clear_via_drawing = true;
            }
          } break;
        }
        command_processor_.PushTransitionBarrier(
            dest_d3d12_rt.GetResource(),
            dest_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (clear_via_drawing) {
          D3D12_CPU_DESCRIPTOR_HANDLE clear_rtv_handle =
              dest_d3d12_rt.GetDescriptorLoadSeparate().IsValid()
                  ? dest_d3d12_rt.GetDescriptorLoadSeparate().GetHandle()
                  : dest_d3d12_rt.GetDescriptorDraw().GetHandle();
          command_list.D3DOMSetRenderTargets(1, &clear_rtv_handle, FALSE, nullptr);
          are_current_command_list_render_targets_valid_ = true;
          D3D12_VIEWPORT clear_viewport;
          clear_viewport.TopLeftX = static_cast<f32>(clear_rect.left);
          clear_viewport.TopLeftY = static_cast<f32>(clear_rect.top);
          clear_viewport.Width = static_cast<f32>(clear_rect.right - clear_rect.left);
          clear_viewport.Height = static_cast<f32>(clear_rect.bottom - clear_rect.top);
          clear_viewport.MinDepth = 0.0f;
          clear_viewport.MaxDepth = 1.0f;
          command_processor_.SetViewport(clear_viewport);
          command_processor_.SetScissorRect(clear_rect);
          command_processor_.SetExternalGraphicsRootSignature(uint32_rtv_clear_root_signature_);
          u32 clear_via_drawing_value[2] = {static_cast<u32>(clear_value),
                                            static_cast<u32>(clear_value >> 32)};
          command_list.D3DSetGraphicsRoot32BitConstants(0, 2, clear_via_drawing_value, 0);
          command_processor_.SetExternalPipeline(
              uint32_rtv_clear_pipelines_[static_cast<size_t>(
                  dest_rt_key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_32_32_FLOAT)]
                                         [static_cast<size_t>(dest_rt_key.msaa_samples)]);
          command_processor_.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          command_list.D3DDrawInstanced(3, 1, 0, 0);
        } else {
          command_processor_.SubmitBarriers();
          command_list.D3DClearRenderTargetView(
              dest_d3d12_rt.GetDescriptorLoadSeparate().IsValid()
                  ? dest_d3d12_rt.GetDescriptorLoadSeparate().GetHandle()
                  : dest_d3d12_rt.GetDescriptorDraw().GetHandle(),
              color_clear_value, 1, &clear_rect);
        }
      }
    }
  }
}

void D3D12RenderTargetCache::SetCommandListRenderTargets(
    RenderTarget* const* depth_and_color_render_targets) {
  REX_ASSERT(GetPath() == Path::kHostRenderTargets);

  if (depth_and_color_render_targets[0]) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(depth_and_color_render_targets[0]);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.GetResource(), d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_DEPTH_WRITE),
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }
  for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    RenderTarget* render_target = depth_and_color_render_targets[1 + i];
    if (!render_target) {
      continue;
    }
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(render_target);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.GetResource(), d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
  }

  if (are_current_command_list_render_targets_valid_ &&
      !std::equal(std::begin(current_command_list_render_targets_),
                  std::end(current_command_list_render_targets_), depth_and_color_render_targets)) {
    are_current_command_list_render_targets_valid_ = false;
  }
  if (!are_current_command_list_render_targets_valid_) {
    std::copy_n(depth_and_color_render_targets, std::size(current_command_list_render_targets_),
                std::begin(current_command_list_render_targets_));
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    if (depth_and_color_render_targets[0]) {
      dsv_handle = static_cast<const D3D12RenderTarget*>(depth_and_color_render_targets[0])
                       ->GetDescriptorDraw()
                       .GetHandle();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[xenos::kMaxColorRenderTargets];
    u32 rtv_count = 0;
    for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      const RenderTarget* render_target = depth_and_color_render_targets[1 + i];
      if (!render_target) {
        continue;
      }
      while (rtv_count < i) {
        rtv_handles[rtv_count++] = render_target->GetKey().msaa_samples != xenos::MSAASamples::k1X
                                       ? null_rtv_descriptor_ms_.GetHandle()
                                       : null_rtv_descriptor_ss_.GetHandle();
      }
      auto& d3d12_rt = *static_cast<const D3D12RenderTarget*>(render_target);
      rtv_handles[rtv_count++] = d3d12_rt.GetDescriptorDraw().GetHandle();
    }
    command_processor_.GetDeferredCommandList().D3DOMSetRenderTargets(
        rtv_count, rtv_handles, FALSE, depth_and_color_render_targets[0] ? &dsv_handle : nullptr);
    are_current_command_list_render_targets_valid_ = true;
  }
}

ID3D12PipelineState* D3D12RenderTargetCache::GetOrCreateDumpPipeline(DumpPipelineKey key) {
  auto pipeline_it = dump_pipelines_.find(key);
  if (pipeline_it != dump_pipelines_.end()) {
    return pipeline_it->second;
  }

  // built_shader_.resize() invalidates pointers and zeroes the new memory.

  built_shader_.clear();

  // RDEF, ISGN, OSGN, SHEX, STAT.
  constexpr u32 kBlobCount = 5;

  built_shader_.resize(sizeof(dxbc::ContainerHeader) / sizeof(u32) + kBlobCount);
  u32 blob_offset_position_dwords = sizeof(dxbc::ContainerHeader) / sizeof(u32);
  u32 blob_position_dwords = static_cast<u32>(built_shader_.size());
  constexpr u32 kBlobHeaderSizeDwords = sizeof(dxbc::BlobHeader) / sizeof(u32);

  u32 name_ptr;

  built_shader_[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 rdef_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;

  built_shader_.resize(rdef_position_dwords + sizeof(dxbc::RdefHeader) / sizeof(u32));
  dxbc::AppendAlignedString(built_shader_, "ReXGlue");

  // Constant types are uint only, named "dword" when scalar.
  name_ptr = static_cast<u32>((built_shader_.size() - rdef_position_dwords) * sizeof(u32));
  u32 rdef_dword_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "dword");
  u32 rdef_type_uint_position_dwords = static_cast<u32>(built_shader_.size());
  u32 rdef_type_uint_ptr =
      static_cast<u32>((rdef_type_uint_position_dwords - rdef_position_dwords) * sizeof(u32));
  built_shader_.resize(rdef_type_uint_position_dwords + sizeof(dxbc::RdefType) / sizeof(u32));
  {
    auto& rdef_type_uint =
        *reinterpret_cast<dxbc::RdefType*>(built_shader_.data() + rdef_type_uint_position_dwords);
    rdef_type_uint.variable_class = dxbc::RdefVariableClass::kScalar;
    rdef_type_uint.variable_type = dxbc::RdefVariableType::kUInt;
    rdef_type_uint.row_count = 1;
    rdef_type_uint.column_count = 1;
    rdef_type_uint.name_ptr = rdef_dword_name_ptr;
  }

  enum Constant : u8 {
    kConstantOffsets,
    kConstantPitches,
    kConstantCount,
  };
  name_ptr = static_cast<u32>((built_shader_.size() - rdef_position_dwords) * sizeof(u32));
  u32 rdef_xe_edram_dump_offsets_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram_dump_offsets");
  u32 rdef_xe_edram_dump_pitches_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram_dump_pitches");
  u32 rdef_constants_position_dwords = static_cast<u32>(built_shader_.size());
  u32 rdef_constants_ptr =
      static_cast<u32>((rdef_constants_position_dwords - rdef_position_dwords) * sizeof(u32));
  built_shader_.resize(rdef_constants_position_dwords +
                       sizeof(dxbc::RdefVariable) / sizeof(u32) * kConstantCount);
  {
    auto rdef_constants = reinterpret_cast<dxbc::RdefVariable*>(built_shader_.data() +
                                                                rdef_constants_position_dwords);
    dxbc::RdefVariable& rdef_constant_offsets = rdef_constants[kConstantOffsets];
    rdef_constant_offsets.name_ptr = rdef_xe_edram_dump_offsets_name_ptr;
    rdef_constant_offsets.size_bytes = sizeof(u32);
    rdef_constant_offsets.flags = dxbc::kRdefVariableFlagUsed;
    rdef_constant_offsets.type_ptr = rdef_type_uint_ptr;
    rdef_constant_offsets.start_texture = UINT32_MAX;
    rdef_constant_offsets.start_sampler = UINT32_MAX;
    dxbc::RdefVariable& rdef_constant_pitches = rdef_constants[kConstantPitches];
    rdef_constant_pitches.name_ptr = rdef_xe_edram_dump_pitches_name_ptr;
    rdef_constant_pitches.size_bytes = sizeof(u32);
    rdef_constant_pitches.flags = dxbc::kRdefVariableFlagUsed;
    rdef_constant_pitches.type_ptr = rdef_type_uint_ptr;
    rdef_constant_pitches.start_texture = UINT32_MAX;
    rdef_constant_pitches.start_sampler = UINT32_MAX;
  }

  // Constant buffers reuse the constant names.
  u32 rdef_cbuffer_position_dwords = static_cast<u32>(built_shader_.size());
  built_shader_.resize(rdef_cbuffer_position_dwords +
                       sizeof(dxbc::RdefCbuffer) / sizeof(u32) * kDumpCbufferCount);
  {
    auto rdef_cbuffers =
        reinterpret_cast<dxbc::RdefCbuffer*>(built_shader_.data() + rdef_cbuffer_position_dwords);
    dxbc::RdefCbuffer& rdef_cbuffer_offsets = rdef_cbuffers[kDumpCbufferOffsets];
    rdef_cbuffer_offsets.name_ptr = rdef_xe_edram_dump_offsets_name_ptr;
    rdef_cbuffer_offsets.variable_count = 1;
    rdef_cbuffer_offsets.variables_ptr =
        static_cast<u32>(rdef_constants_ptr + sizeof(dxbc::RdefVariable) * kConstantOffsets);
    rdef_cbuffer_offsets.size_vector_aligned_bytes = sizeof(u32) * 4;
    dxbc::RdefCbuffer& rdef_cbuffer_pitches = rdef_cbuffers[kDumpCbufferPitches];
    rdef_cbuffer_pitches.name_ptr = rdef_xe_edram_dump_pitches_name_ptr;
    rdef_cbuffer_pitches.variable_count = 1;
    rdef_cbuffer_pitches.variables_ptr =
        static_cast<u32>(rdef_constants_ptr + sizeof(dxbc::RdefVariable) * kConstantPitches);
    rdef_cbuffer_pitches.size_vector_aligned_bytes = sizeof(u32) * 4;
  }

  u32 rdef_binding_count = 1 + key.is_depth + 1 + kDumpCbufferCount;
  name_ptr = static_cast<u32>((built_shader_.size() - rdef_position_dwords) * sizeof(u32));
  u32 rdef_xe_edram_dump_source_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram_dump_source");
  u32 rdef_xe_edram_dump_stencil_name_ptr = name_ptr;
  if (key.is_depth) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram_dump_stencil");
  }
  u32 rdef_xe_edram_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram");
  u32 rdef_binding_position_dwords = static_cast<u32>(built_shader_.size());
  built_shader_.resize(rdef_binding_position_dwords +
                       sizeof(dxbc::RdefInputBind) / sizeof(u32) * rdef_binding_count);
  bool source_is_uint;
  if (key.is_depth) {
    source_is_uint = false;
  } else {
    GetColorOwnershipTransferDXGIFormat(key.GetColorFormat(), &source_is_uint);
  }
  dxbc::ResourceReturnType source_return_type =
      source_is_uint ? dxbc::ResourceReturnType::kUInt : dxbc::ResourceReturnType::kFloat;
  u32 source_component_count =
      key.is_depth ? 1 : xenos::GetColorRenderTargetFormatComponentCount(key.GetColorFormat());
  bool format_is_64bpp =
      !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(key.GetColorFormat());
  {
    auto rdef_bindings =
        reinterpret_cast<dxbc::RdefInputBind*>(built_shader_.data() + rdef_binding_position_dwords);
    u32 rdef_binding_index = 0;
    dxbc::RdefInputBind& rdef_binding_source = rdef_bindings[rdef_binding_index++];
    rdef_binding_source.name_ptr = rdef_xe_edram_dump_source_name_ptr;
    rdef_binding_source.type = dxbc::RdefInputType::kTexture;
    rdef_binding_source.return_type = source_return_type;
    if (key.msaa_samples != xenos::MSAASamples::k1X) {
      rdef_binding_source.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
      // Sample count is dynamic on Shader Model 5.
    } else {
      rdef_binding_source.dimension = dxbc::RdefDimension::kSRVTexture2D;
      rdef_binding_source.sample_count = UINT32_MAX;
    }
    rdef_binding_source.bind_count = 1;
    rdef_binding_source.flags = (source_component_count - 1)
                                << dxbc::kRdefInputFlagsComponentsShift;
    if (key.is_depth) {
      dxbc::RdefInputBind& rdef_binding_stencil = rdef_bindings[rdef_binding_index++];
      rdef_binding_stencil.name_ptr = rdef_xe_edram_dump_stencil_name_ptr;
      rdef_binding_stencil.type = dxbc::RdefInputType::kTexture;
      rdef_binding_stencil.return_type = dxbc::ResourceReturnType::kUInt;
      rdef_binding_stencil.dimension = rdef_binding_source.dimension;
      rdef_binding_stencil.sample_count = rdef_binding_source.sample_count;
      rdef_binding_stencil.bind_point = 1;
      rdef_binding_stencil.bind_count = 1;
      rdef_binding_stencil.flags = dxbc::kRdefInputFlags2Component;
      rdef_binding_stencil.id = 1;
    }
    dxbc::RdefInputBind& rdef_binding_edram = rdef_bindings[rdef_binding_index++];
    rdef_binding_edram.name_ptr = rdef_xe_edram_name_ptr;
    rdef_binding_edram.type = dxbc::RdefInputType::kUAVRWTyped;
    rdef_binding_edram.return_type = dxbc::ResourceReturnType::kUInt;
    rdef_binding_edram.dimension = dxbc::RdefDimension::kUAVBuffer;
    rdef_binding_edram.sample_count = UINT32_MAX;
    rdef_binding_edram.bind_count = 1;
    rdef_binding_edram.flags = format_is_64bpp ? dxbc::kRdefInputFlags2Component : 0;
    dxbc::RdefInputBind& rdef_binding_offsets = rdef_bindings[rdef_binding_index++];
    rdef_binding_offsets.name_ptr = rdef_xe_edram_dump_offsets_name_ptr;
    rdef_binding_offsets.type = dxbc::RdefInputType::kCbuffer;
    rdef_binding_offsets.bind_point = kDumpCbufferOffsets;
    rdef_binding_offsets.bind_count = 1;
    rdef_binding_offsets.flags = dxbc::kRdefInputFlagUserPacked;
    rdef_binding_offsets.id = kDumpCbufferOffsets;
    dxbc::RdefInputBind& rdef_binding_pitches = rdef_bindings[rdef_binding_index++];
    rdef_binding_pitches.name_ptr = rdef_xe_edram_dump_pitches_name_ptr;
    rdef_binding_pitches.type = dxbc::RdefInputType::kCbuffer;
    rdef_binding_pitches.bind_point = kDumpCbufferPitches;
    rdef_binding_pitches.bind_count = 1;
    rdef_binding_pitches.flags = dxbc::kRdefInputFlagUserPacked;
    rdef_binding_pitches.id = kDumpCbufferPitches;
  }

  {
    auto& rdef_header =
        *reinterpret_cast<dxbc::RdefHeader*>(built_shader_.data() + rdef_position_dwords);
    rdef_header.cbuffer_count = kDumpCbufferCount;
    rdef_header.cbuffers_ptr =
        static_cast<u32>((rdef_cbuffer_position_dwords - rdef_position_dwords) * sizeof(u32));
    rdef_header.input_bind_count = rdef_binding_count;
    rdef_header.input_binds_ptr =
        static_cast<u32>((rdef_binding_position_dwords - rdef_position_dwords) * sizeof(u32));
    rdef_header.shader_model = dxbc::RdefShaderModel::kComputeShader51;
    rdef_header.compile_flags =
        dxbc::kCompileFlagNoPreshader | dxbc::kCompileFlagPreferFlowControl |
        dxbc::kCompileFlagIeeeStrictness | dxbc::kCompileFlagAllResourcesBound;
    rdef_header.generator_name_ptr = sizeof(dxbc::RdefHeader);
    rdef_header.fourcc = dxbc::RdefHeader::FourCC::k51;
    rdef_header.InitializeSizes();
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = static_cast<u32>(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             built_shader_[blob_offset_position_dwords++];
  }

  for (u32 i = 0; i < 2; ++i) {
    built_shader_[blob_offset_position_dwords] =
        static_cast<u32>(blob_position_dwords * sizeof(u32));
    u32 signature_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
    built_shader_.resize(signature_position_dwords + sizeof(dxbc::Signature) / sizeof(u32));
    {
      auto& signature =
          *reinterpret_cast<dxbc::Signature*>(built_shader_.data() + signature_position_dwords);
      // The signature is empty, so the parameter pointer is at the end.
      signature.parameter_info_ptr = sizeof(dxbc::Signature);
    }
    {
      auto& blob_header =
          *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
      blob_header.fourcc = i ? dxbc::BlobHeader::FourCC::kOutputSignature
                             : dxbc::BlobHeader::FourCC::kInputSignature;
      blob_position_dwords = static_cast<u32>(built_shader_.size());
      blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                               built_shader_[blob_offset_position_dwords++];
    }
  }

  built_shader_[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 shex_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(shex_position_dwords);

  built_shader_.push_back(dxbc::VersionToken(dxbc::ProgramType::kComputeShader, 5, 1));
  built_shader_.push_back(0);

  dxbc::Statistics stat{};
  dxbc::Assembler a(built_shader_, stat);

  a.OpDclGlobalFlags(dxbc::kGlobalFlagAllResourcesBound);
  a.OpDclConstantBuffer(
      dxbc::Src::CB(dxbc::Src::kDcl, kDumpCbufferOffsets, kDumpCbufferOffsets, kDumpCbufferOffsets),
      1);
  a.OpDclConstantBuffer(
      dxbc::Src::CB(dxbc::Src::kDcl, kDumpCbufferPitches, kDumpCbufferPitches, kDumpCbufferPitches),
      1);
  dxbc::ResourceDimension source_dimension = key.msaa_samples != xenos::MSAASamples::k1X
                                                 ? dxbc::ResourceDimension::kTexture2DMS
                                                 : dxbc::ResourceDimension::kTexture2D;
  a.OpDclResource(
      source_dimension,
      dxbc::ResourceReturnTypeX4Token(source_is_uint ? dxbc::ResourceReturnType::kUInt
                                                     : dxbc::ResourceReturnType::kFloat),
      dxbc::Src::T(dxbc::Src::kDcl, 0, 0, 0));
  if (key.is_depth) {
    a.OpDclResource(source_dimension,
                    dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kUInt),
                    dxbc::Src::T(dxbc::Src::kDcl, 1, 1, 1));
  }
  a.OpDclUnorderedAccessViewTyped(dxbc::ResourceDimension::kBuffer, 0,
                                  dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kUInt),
                                  dxbc::Src::U(dxbc::Src::kDcl, 0, 0, 0));
  a.OpDclInput(dxbc::Dest::VThreadID(0b0011));
  // r0 = addressing then conversion scratch, r1 = addressing scratch then data.
  stat.temp_register_count = 2;
  a.OpDclTemps(stat.temp_register_count);
  a.OpDclThreadGroup(40, 16, 1);

  u32 draw_resolution_scale_x = this->GetDrawResolutionScaleX();
  u32 draw_resolution_scale_y = this->GetDrawResolutionScaleY();

  u32 tile_width = (xenos::kEDRAMTileWidthSamples * draw_resolution_scale_x) >>
                   static_cast<u32>(format_is_64bpp);
  u32 tile_height = xenos::kEDRAMTileHeightSamples * draw_resolution_scale_y;

  // r0.xy = sample XY within the tile, r0.zw = tile XY.
  a.OpUDiv(dxbc::Dest::R(0, 0b1100), dxbc::Dest::R(0, 0b0011), dxbc::Src::VThreadID(0b01000100),
           dxbc::Src::LU(tile_width, tile_height, tile_width, tile_height));

  a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEDRAMPitchTilesBits), dxbc::Src::LU(0),
           dxbc::Src::CB(kDumpCbufferPitches, kDumpCbufferPitches, 0, dxbc::Src::kXXXX));
  a.OpUMAd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
           dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::R(0, dxbc::Src::kZZZZ));

  // r0.z = first EDRAM tile index of the dispatch, r0.w = tile index relative to the dump base.
  a.OpUBFE(dxbc::Dest::R(0, 0b0100), dxbc::Src::LU(xenos::kEDRAMBaseTilesBits + 1),
           dxbc::Src::LU(0),
           dxbc::Src::CB(kDumpCbufferOffsets, kDumpCbufferOffsets, 0, dxbc::Src::kXXXX));
  // r0.w is unwrapped, so after subtracting the base the texture address stays nonnegative.
  a.OpIAdd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
           dxbc::Src::R(0, dxbc::Src::kZZZZ));
  // r0.z = wrapped tile index in the EDRAM.
  a.OpAnd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kWWWW),
          dxbc::Src::LU(xenos::kEDRAMTileCount - 1));
  a.OpUMAd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kZZZZ),
           dxbc::Src::LU(draw_resolution_scale_x * draw_resolution_scale_y *
                         (xenos::kEDRAMTileWidthSamples >> static_cast<u32>(format_is_64bpp)) *
                         xenos::kEDRAMTileHeightSamples),
           dxbc::Src::R(0, dxbc::Src::kXXXX));
  // r0.z = sample offset in the EDRAM without the depth column swapping.
  a.OpUMAd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(tile_width),
           dxbc::Src::R(0, dxbc::Src::kZZZZ));
  if (key.is_depth) {
    u32 tile_width_half = tile_width >> 1;
    // r1.x = all ones in the right 40-sample half, 0 otherwise.
    a.OpUGE(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
            dxbc::Src::LU(tile_width_half));
    // r1.x = offset swapping the 40-sample halves for depth.
    a.OpMovC(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
             dxbc::Src::LI(-static_cast<i32>(tile_width_half)),
             dxbc::Src::LI(static_cast<i32>(tile_width_half)));
    // r0.z = final EDRAM sample offset with the 40-sample columns swapped.
    a.OpIAdd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kZZZZ),
             dxbc::Src::R(1, dxbc::Src::kXXXX));
  }

  // r1.x = source texture base tile index.
  a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEDRAMBaseTilesBits),
           dxbc::Src::LU(xenos::kEDRAMBaseTilesBits + 1),
           dxbc::Src::CB(kDumpCbufferOffsets, kDumpCbufferOffsets, 0, dxbc::Src::kXXXX));
  // r0.w = linear tile index in the source texture.
  a.OpIAdd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
           -dxbc::Src::R(1, dxbc::Src::kXXXX));
  // r1.x = source texture pitch in tiles.
  a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEDRAMPitchTilesBits),
           dxbc::Src::LU(xenos::kEDRAMPitchTilesBits),
           dxbc::Src::CB(kDumpCbufferPitches, kDumpCbufferPitches, 0, dxbc::Src::kXXXX));
  // r0.w = tile X in the source texture, r1.x = tile row.
  a.OpUDiv(dxbc::Dest::R(1, 0b0001), dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
           dxbc::Src::R(1, dxbc::Src::kXXXX));
  // r0.xy = sample XY within the source texture.
  a.OpUMAd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kWWWW), dxbc::Src::LU(tile_width),
           dxbc::Src::R(0, dxbc::Src::kXXXX));
  a.OpUMAd(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(1, dxbc::Src::kXXXX),
           dxbc::Src::LU(xenos::kEDRAMTileHeightSamples * draw_resolution_scale_y),
           dxbc::Src::R(0, dxbc::Src::kYYYY));
  // The source coordinates are r0.xy, and r0.w is the LOD for a single sampled source.
  dxbc::Src source_address_src(dxbc::Src::R(0, 0b11000100));
  if (key.msaa_samples >= xenos::MSAASamples::k2X) {
    if (key.msaa_samples >= xenos::MSAASamples::k4X) {
      // 4x sample index in r0.w, bit 0 horizontal and bit 1 vertical.
      a.OpAnd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
      a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(1),
              dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(0, dxbc::Src::kWWWW));
      // r0.xy = pixel XY in the source texture.
      a.OpUShR(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0), dxbc::Src::LU(1));
    } else {
      // 2x sample index in r0.w is the vertical sample, converted from the guest to Direct3D 10.1+.
      a.OpAnd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(1));
      a.OpMovC(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
               dxbc::Src::LU(draw_state::GetD3D10SampleIndexForGuest2xMSAA(1, msaa_2x_supported_)),
               dxbc::Src::LU(draw_state::GetD3D10SampleIndexForGuest2xMSAA(0, msaa_2x_supported_)));
      a.OpUShR(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(1));
    }
    a.OpLdMS(dxbc::Dest::R(1, (1 << source_component_count) - 1), source_address_src, 0b0011,
             dxbc::Src::T(0, 0), dxbc::Src::R(0, dxbc::Src::kWWWW));
    if (key.is_depth) {
      a.OpLdMS(dxbc::Dest::R(1, 0b0010), source_address_src, 0b0011, dxbc::Src::T(1, 1),
               dxbc::Src::R(0, dxbc::Src::kWWWW));
    }
  } else {
    // r0.w = LOD 0 for the single sampled load.
    a.OpMov(dxbc::Dest::R(0, 0b1000), dxbc::Src::LF(0.0f));
    a.OpLd(dxbc::Dest::R(1, (1 << source_component_count) - 1), source_address_src, 0b1011,
           dxbc::Src::T(0, 0));
    if (key.is_depth) {
      a.OpLd(dxbc::Dest::R(1, 0b0010), source_address_src, 0b1011, dxbc::Src::T(1, 1));
    }
  }

  // The packed result goes to r1.x for 32bpp or r1.xy for 64bpp, with r0.xyw as scratch.
  if (key.is_depth) {
    switch (key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24S8:
        // Round to nearest even, as adding +0.5 and truncating turns black into red.
        a.OpMul(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
                dxbc::Src::LF(static_cast<f32>(0xFFFFFF)));
        a.OpRoundNE(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
        a.OpFToU(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
        break;
      case xenos::DepthRenderTargetFormat::kD24FS8:
        // Uses r0.x as a temp. Conversion in pixel shaders is exact, so it truncates.
        DXBCShaderTranslator::PreClampedDepthTo20e4(
            a, 1, 0, 1, 0, 0, 0, !GetDepthFloat24ConvertInPixelShader() && GetDepthFloat24Round(),
            true);
        break;
    }
    a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(24), dxbc::Src::LU(8),
            dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::R(1, dxbc::Src::kYYYY));
  } else {
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
        // 8_8_8_8_GAMMA is stored as linear in R16G16B16A16_UNORM.
        REX_ASSERT(!source_is_uint);
        for (u32 i = 0; i < 3; ++i) {
          DXBCShaderTranslator::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 0, 0, 0, 1);
        }
        a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
        a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
        for (u32 i = 1; i < 4; ++i) {
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(i * 8),
                  dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
        break;
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        if (!source_is_uint) {
          a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
          a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
        }
        for (u32 i = 1; i < 4; ++i) {
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(i * 8),
                  dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
        if (!source_is_uint) {
          a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1), dxbc::Src::LF(1023.0f, 1023.0f, 1023.0f, 3.0f),
                  dxbc::Src::LF(0.5f));
          a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
        }
        for (u32 i = 1; i < 4; ++i) {
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(i == 3 ? 2 : 10), dxbc::Src::LU(i * 10),
                  dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
        // Float16 has a wider range for color and alpha, and NaNs, so color is clamped.
        DXBCShaderTranslator::UnclampedFloat32To7e3(a, 1, 0, 1, 0, 0, 0);
        for (u32 i = 1; i < 3; ++i) {
          DXBCShaderTranslator::UnclampedFloat32To7e3(a, 0, 0, 1, i, 0, 1);
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(10), dxbc::Src::LU(i * 10),
                  dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
        a.OpMov(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW), true);
        a.OpMAd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::LF(3.0f),
                dxbc::Src::LF(0.5f));
        a.OpFToU(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
        a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(2), dxbc::Src::LU(30),
                dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::R(1, dxbc::Src::kXXXX));
        break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        REX_ASSERT(source_is_uint);
        a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(16), dxbc::Src::LU(16),
                dxbc::Src::R(1, dxbc::Src::kYYYY), dxbc::Src::R(1, dxbc::Src::kXXXX));
        break;
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
        REX_ASSERT(source_is_uint);
        a.OpBFI(dxbc::Dest::R(1, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(16),
                dxbc::Src::R(1, 0b1101), dxbc::Src::R(1, 0b1000));
        break;
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
        REX_ASSERT(source_is_uint);
        break;
    }
  }

  a.OpStoreUAVTyped(dxbc::Dest::U(0, 0), dxbc::Src::R(0, dxbc::Src::kZZZZ), 1,
                    dxbc::Src::R(1, format_is_64bpp ? 0b0100 : dxbc::Src::kXXXX));

  a.OpRet();

  built_shader_[shex_position_dwords + 1] =
      static_cast<u32>(built_shader_.size()) - shex_position_dwords;

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = static_cast<u32>(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             built_shader_[blob_offset_position_dwords++];
  }

  built_shader_[blob_offset_position_dwords] = static_cast<u32>(blob_position_dwords * sizeof(u32));
  u32 stat_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(stat_position_dwords + sizeof(dxbc::Statistics) / sizeof(u32));
  std::memcpy(built_shader_.data() + stat_position_dwords, &stat, sizeof(dxbc::Statistics));
  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = static_cast<u32>(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(u32) -
                             built_shader_[blob_offset_position_dwords++];
  }

  u32 built_shader_size_bytes = static_cast<u32>(built_shader_.size() * sizeof(u32));
  {
    auto& container_header = *reinterpret_cast<dxbc::ContainerHeader*>(built_shader_.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = built_shader_size_bytes;
    container_header.blob_count = kBlobCount;
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(built_shader_.data()),
                          static_cast<unsigned int>(built_shader_size_bytes),
                          container_header.hash);
  }

  ID3D12PipelineState* pipeline = gpu::d3d12::objects::CreateComputePipeline(
      command_processor_.GetD3D12Provider().GetDevice(), built_shader_.data(),
      built_shader_size_bytes,
      key.is_depth ? dump_root_signature_depth_ : dump_root_signature_color_);
  const char* format_name = key.is_depth
                                ? xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat())
                                : xenos::GetColorRenderTargetFormatName(key.GetColorFormat());
  if (pipeline) {
    std::u16string pipeline_name = rex::ToUTF16(std::format(
        "RT Dump {} {}xMSAA", format_name, static_cast<u32>(1) << u32(key.msaa_samples)));
    pipeline->SetName(reinterpret_cast<LPCWSTR>(pipeline_name.c_str()));
  } else {
    REXLOG_ERROR(gpu,
                 "D3D12RenderTargetCache: Failed to create a render target dumping "
                 "pipeline for {}-sample render targets with format {}",
                 static_cast<u32>(1) << u32(key.msaa_samples), format_name);
  }
  // A failed creation stores null to avoid retrying.
  dump_pipelines_.emplace(key, pipeline);
  return pipeline;
}

ID3D12PipelineState* D3D12RenderTargetCache::GetOrCreateDirectResolvePipeline(
    DirectResolvePipelineKey key) {
  auto pipeline_it = direct_resolve_pipelines_.find(key);
  if (pipeline_it != direct_resolve_pipelines_.end()) {
    return pipeline_it->second;
  }
  ID3D12PipelineState* pipeline = nullptr;
  size_t copy_shader_index = static_cast<size_t>(key.copy_shader);
  if (copy_shader_index < size_t(draw_state::ResolveCopyShaderIndex::kCount)) {
    pipeline = resolve_copy_pipelines_[copy_shader_index];
  }
  direct_resolve_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool D3D12RenderTargetCache::TryResolveCopyDirectly(const draw_state::ResolveInfo& resolve_info,
                                                    draw_state::ResolveCopyShaderIndex copy_shader,
                                                    bool draw_resolution_scaled) {
  ++direct_resolve_attempt_count_;
  (void)copy_shader;
  (void)draw_resolution_scaled;
  if (!direct_resolve_root_signature_color_ || !direct_resolve_root_signature_depth_) {
    return false;
  }

  u32 dump_base;
  u32 dump_row_length_used;
  u32 dump_rows;
  u32 dump_pitch;
  resolve_info.GetCopyEDRAMTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
  GetResolveCopyDispatchesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_, direct_resolve_dispatches_);
  if (direct_resolve_dispatches_.empty()) {
    return false;
  }

  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    const auto* render_target = static_cast<const D3D12RenderTarget*>(rectangle.render_target);
    if (render_target == nullptr) {
      return false;
    }
    DumpPipelineKey dump_pipeline_key;
    dump_pipeline_key.msaa_samples = render_target->GetKey().msaa_samples;
    dump_pipeline_key.resource_format = render_target->GetKey().resource_format;
    dump_pipeline_key.is_depth = render_target->GetKey().is_depth;
    if (!GetOrCreateDumpPipeline(dump_pipeline_key)) {
      return false;
    }
    DirectResolvePipelineKey direct_pipeline_key;
    direct_pipeline_key.dump_pipeline_key = dump_pipeline_key;
    direct_pipeline_key.copy_shader = copy_shader;
    direct_pipeline_key.draw_resolution_scaled = draw_resolution_scaled;
    if (!GetOrCreateDirectResolvePipeline(direct_pipeline_key)) {
      return false;
    }
  }

  return DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch);
}

bool D3D12RenderTargetCache::DumpRenderTargets(u32 dump_base, u32 dump_row_length_used,
                                               u32 dump_rows, u32 dump_pitch) {
  REX_ASSERT(GetPath() == Path::kHostRenderTargets);

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_);
  if (dump_rectangles_.empty()) {
    return true;
  }

  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    d3d12_rt.SetTemporarySortIndex(UINT32_MAX);
    d3d12_rt.SetTemporarySRVDescriptorIndex(UINT32_MAX);
    d3d12_rt.SetTemporarySRVDescriptorIndexStencil(UINT32_MAX);
  }
  TransitionEDRAMBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  dump_invocations_.clear();
  dump_invocations_.reserve(dump_rectangles_.size());
  current_temporary_descriptors_cpu_.clear();
  bool any_sources_32bpp_64bpp[2] = {};
  u32 rt_sort_index = 0;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.GetResource(),
        d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (d3d12_rt.GetTemporarySortIndex() == UINT32_MAX) {
      d3d12_rt.SetTemporarySortIndex(rt_sort_index++);
    }
    if (d3d12_rt.GetTemporarySRVDescriptorIndex() == UINT32_MAX) {
      d3d12_rt.SetTemporarySRVDescriptorIndex(
          static_cast<u32>(current_temporary_descriptors_cpu_.size()));
      current_temporary_descriptors_cpu_.push_back(d3d12_rt.GetDescriptorSRV().GetHandle());
    }
    RenderTargetKey rt_key = d3d12_rt.GetKey();
    if (rt_key.is_depth && d3d12_rt.GetTemporarySRVDescriptorIndexStencil() == UINT32_MAX) {
      d3d12_rt.SetTemporarySRVDescriptorIndexStencil(
          static_cast<u32>(current_temporary_descriptors_cpu_.size()));
      current_temporary_descriptors_cpu_.push_back(d3d12_rt.GetDescriptorSRVStencil().GetHandle());
    }
    any_sources_32bpp_64bpp[static_cast<size_t>(rt_key.Is64bpp())] = true;
    DumpPipelineKey pipeline_key;
    pipeline_key.msaa_samples = rt_key.msaa_samples;
    pipeline_key.resource_format = rt_key.resource_format;
    pipeline_key.is_depth = rt_key.is_depth;
    dump_invocations_.emplace_back(rectangle, pipeline_key);
  }
  size_t edram_uav_indices[2] = {SIZE_MAX, SIZE_MAX};
  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  if (!bindless_resources_used_) {
    if (any_sources_32bpp_64bpp[0]) {
      edram_uav_indices[0] = current_temporary_descriptors_cpu_.size();
      current_temporary_descriptors_cpu_.push_back(provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          std::to_underlying(EDRAMBufferDescriptorIndex::kR32UintUAV)));
    }
    if (any_sources_32bpp_64bpp[1]) {
      edram_uav_indices[1] = current_temporary_descriptors_cpu_.size();
      current_temporary_descriptors_cpu_.push_back(provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          std::to_underlying(EDRAMBufferDescriptorIndex::kR32G32UintUAV)));
    }
  }

  ID3D12Device* device = provider.GetDevice();
  u32 descriptor_count = static_cast<u32>(current_temporary_descriptors_cpu_.size());
  current_temporary_descriptors_gpu_.resize(descriptor_count);
  if (!command_processor_.RequestOneUseSingleViewDescriptors(
          descriptor_count, current_temporary_descriptors_gpu_.data())) {
    return false;
  }
  for (u32 i = 0; i < descriptor_count; ++i) {
    device->CopyDescriptorsSimple(1, current_temporary_descriptors_gpu_[i].first,
                                  current_temporary_descriptors_cpu_[i],
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }

  std::sort(dump_invocations_.begin(), dump_invocations_.end());

  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  ID3D12RootSignature* last_root_signature = nullptr;
  u32 root_parameters_set = 0;
  u32 last_descriptor_index_source = UINT32_MAX;
  u32 last_descriptor_index_stencil = UINT32_MAX;
  bool last_edram_uav_is_64bpp = false;
  DumpOffsets last_offsets;
  DumpPitches last_pitches;
  bool all_pipelines_available = true;
  for (const DumpInvocation& invocation : dump_invocations_) {
    const ResolveCopyDumpRectangle& rectangle = invocation.rectangle;
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = d3d12_rt.GetKey();
    DumpPipelineKey pipeline_key = invocation.pipeline_key;
    ID3D12PipelineState* pipeline = GetOrCreateDumpPipeline(pipeline_key);
    if (!pipeline) {
      all_pipelines_available = false;
      continue;
    }
    command_processor_.SetExternalPipeline(pipeline);

    ID3D12RootSignature* root_signature =
        pipeline_key.is_depth ? dump_root_signature_depth_ : dump_root_signature_color_;
    if (last_root_signature != root_signature) {
      last_root_signature = root_signature;
      command_list.D3DSetComputeRootSignature(root_signature);
      root_parameters_set = 0;
    }

    DumpRootParameter root_parameter_edram =
        pipeline_key.is_depth ? kDumpRootParameterDepthEDRAM : kDumpRootParameterColorEDRAM;
    u32 root_parameter_edram_bit = static_cast<u32>(1) << root_parameter_edram;
    bool format_is_64bpp = rt_key.Is64bpp();
    if (last_edram_uav_is_64bpp != format_is_64bpp) {
      last_edram_uav_is_64bpp = format_is_64bpp;
      root_parameters_set &= ~root_parameter_edram_bit;
    }
    if (!(root_parameters_set & root_parameter_edram_bit)) {
      D3D12_GPU_DESCRIPTOR_HANDLE descriptor_handle_edram;
      if (bindless_resources_used_) {
        descriptor_handle_edram = command_processor_
                                      .GetEDRAMUintPow2BindlessUAVHandlePair(
                                          2 + static_cast<u32>(last_edram_uav_is_64bpp))
                                      .second;
      } else {
        REX_ASSERT(edram_uav_indices[static_cast<size_t>(last_edram_uav_is_64bpp)] != SIZE_MAX);
        descriptor_handle_edram =
            current_temporary_descriptors_gpu_[edram_uav_indices[static_cast<size_t>(
                                                   last_edram_uav_is_64bpp)]]
                .second;
      }
      command_list.D3DSetComputeRootDescriptorTable(root_parameter_edram, descriptor_handle_edram);
      root_parameters_set |= root_parameter_edram_bit;
    }

    DumpRootParameter root_parameter_pitches =
        pipeline_key.is_depth ? kDumpRootParameterDepthPitches : kDumpRootParameterColorPitches;
    u32 root_parameter_pitches_bit = static_cast<u32>(1) << root_parameter_pitches;
    DumpPitches pitches;
    pitches.dest_pitch = dump_pitch;
    pitches.source_pitch = rt_key.GetPitchTiles();
    if (last_pitches != pitches) {
      last_pitches = pitches;
      root_parameters_set &= ~root_parameter_pitches_bit;
    }
    if (!(root_parameters_set & root_parameter_pitches_bit)) {
      command_list.D3DSetComputeRoot32BitConstants(
          root_parameter_pitches, sizeof(last_pitches) / sizeof(u32), &last_pitches, 0);
      root_parameters_set |= root_parameter_pitches_bit;
    }

    if (pipeline_key.is_depth) {
      constexpr u32 kDumpRootParameterDepthStencilBit = static_cast<u32>(1)
                                                        << kDumpRootParameterDepthStencil;
      u32 descriptor_index_stencil = d3d12_rt.GetTemporarySRVDescriptorIndexStencil();
      REX_ASSERT(descriptor_index_stencil != UINT32_MAX);
      if (last_descriptor_index_stencil != descriptor_index_stencil) {
        last_descriptor_index_stencil = descriptor_index_stencil;
        root_parameters_set &= ~kDumpRootParameterDepthStencilBit;
      }
      if (!(root_parameters_set & kDumpRootParameterDepthStencilBit)) {
        command_list.D3DSetComputeRootDescriptorTable(
            kDumpRootParameterDepthStencil,
            current_temporary_descriptors_gpu_[last_descriptor_index_stencil].second);
        root_parameters_set |= kDumpRootParameterDepthStencilBit;
      }
    }

    constexpr u32 kDumpRootParameterSourceBit = static_cast<u32>(1) << kDumpRootParameterSource;
    u32 descriptor_index_source = d3d12_rt.GetTemporarySRVDescriptorIndex();
    REX_ASSERT(descriptor_index_source != UINT32_MAX);
    if (last_descriptor_index_source != descriptor_index_source) {
      last_descriptor_index_source = descriptor_index_source;
      root_parameters_set &= ~kDumpRootParameterSourceBit;
    }
    if (!(root_parameters_set & kDumpRootParameterSourceBit)) {
      command_list.D3DSetComputeRootDescriptorTable(
          kDumpRootParameterSource,
          current_temporary_descriptors_gpu_[last_descriptor_index_source].second);
      root_parameters_set |= kDumpRootParameterSourceBit;
    }

    constexpr u32 kDumpRootParameterOffsetsBit = static_cast<u32>(1) << kDumpRootParameterOffsets;
    DumpOffsets offsets;
    offsets.source_base_tiles = rt_key.base_tiles;
    ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    u32 dispatch_count = rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (u32 i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      offsets.dispatch_first_tile = dump_base + dispatch.offset;
      if (last_offsets != offsets) {
        last_offsets = offsets;
        root_parameters_set &= ~kDumpRootParameterOffsetsBit;
      }
      if (!(root_parameters_set & kDumpRootParameterOffsetsBit)) {
        command_list.D3DSetComputeRoot32BitConstants(
            kDumpRootParameterOffsets, sizeof(last_offsets) / sizeof(u32), &last_offsets, 0);
        root_parameters_set |= kDumpRootParameterOffsetsBit;
      }
      command_processor_.SubmitBarriers();
      // Each group covers 40x16 samples times scale, so a 32bpp tile takes 2 groups at 1x1 scale.
      command_list.D3DDispatch((dispatch.width_tiles * GetDrawResolutionScaleX())
                                   << u32(!format_is_64bpp),
                               dispatch.height_tiles * GetDrawResolutionScaleY(), 1);
    }
    MarkEDRAMBufferModified();
  }
  return all_pipelines_available;
}

bool D3D12RenderTargetCache::IsFixed16TruncatedToMinus1To1() const {
  return GetPath() == Path::kHostRenderTargets &&
         !REXCVAR_GET(::rex::gpu::cvars::snorm16_render_target_full_range);
}

}  // namespace rex::gpu::d3d12
