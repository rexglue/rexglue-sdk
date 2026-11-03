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

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <SPIRV/GLSL.std.450.h>

#include <rex/assert.h>
#include "gpu/xenos/pipeline/shader/spirv_builder.h"
#include "gpu/xenos/pipeline/shader/spirv_translator.h"
#include "gpu/xenos/pipeline/texture/cache.h"
#include <rex/xenos/register.h>
#include "gpu/xenos/draw_state.h"
#include "gpu/xenos/vulkan/command_processor.h"
#include "gpu/xenos/vulkan/deferred_command_buffer.h"
#include "gpu/xenos/vulkan/render_target_cache.h"
#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include <rex/xenos/state.h>
#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/bit.h>
#include "gpu/xenos/host/vulkan/objects.h"

namespace rex::gpu::vulkan {

namespace shaders {
#include "../shaders/vulkan_spirv/host_depth_store_1xmsaa_cs.h"
#include "../shaders/vulkan_spirv/host_depth_store_2xmsaa_cs.h"
#include "../shaders/vulkan_spirv/host_depth_store_4xmsaa_cs.h"
#include "../shaders/vulkan_spirv/passthrough_position_xy_vs.h"
#include "../shaders/vulkan_spirv/resolve_clear_32bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_clear_32bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_clear_64bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_clear_64bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_4xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_4xmsaa_cs.h"
#include "../shaders/vulkan_spirv/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_128bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_128bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_16bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_16bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_32bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_32bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_64bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_64bpp_scaled_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_8bpp_cs.h"
#include "../shaders/vulkan_spirv/resolve_full_8bpp_scaled_cs.h"
}  // namespace shaders

const VulkanRenderTargetCache::ResolveCopyShaderCode
    VulkanRenderTargetCache::kResolveCopyShaders[static_cast<size_t>(
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

const VulkanRenderTargetCache::TransferPipelineLayoutInfo
    VulkanRenderTargetCache::kTransferPipelineLayoutInfos[static_cast<size_t>(
        TransferPipelineLayoutIndex::kCount)] = {
        // kColor
        {kTransferUsedDescriptorSetColorTextureBit, kTransferUsedPushConstantDwordAddressBit},
        // kDepth
        {kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordAddressBit},
        // kColorToStencilBit
        {kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordAddressBit | kTransferUsedPushConstantDwordStencilMaskBit},
        // kDepthToStencilBit
        {kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordAddressBit | kTransferUsedPushConstantDwordStencilMaskBit},
        // kColorAndHostDepthTexture
        {kTransferUsedDescriptorSetHostDepthStencilTexturesBit |
             kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kColorAndHostDepthBuffer
        {kTransferUsedDescriptorSetHostDepthBufferBit | kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kDepthAndHostDepthTexture
        {kTransferUsedDescriptorSetHostDepthStencilTexturesBit |
             kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kDepthAndHostDepthBuffer
        {kTransferUsedDescriptorSetHostDepthBufferBit |
             kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
};

const VulkanRenderTargetCache::TransferModeInfo
    VulkanRenderTargetCache::kTransferModes[static_cast<size_t>(TransferMode::kCount)] = {
        // kColorToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColor},
        // kColorToColor
        {TransferOutput::kColor, TransferPipelineLayoutIndex::kColor},
        // kDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepth},
        // kDepthToColor
        {TransferOutput::kColor, TransferPipelineLayoutIndex::kDepth},
        // kColorToStencilBit
        {TransferOutput::kStencilBit, TransferPipelineLayoutIndex::kColorToStencilBit},
        // kDepthToStencilBit
        {TransferOutput::kStencilBit, TransferPipelineLayoutIndex::kDepthToStencilBit},
        // kColorAndHostDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColorAndHostDepthTexture},
        // kDepthAndHostDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepthAndHostDepthTexture},
        // kColorAndHostDepthCopyToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColorAndHostDepthBuffer},
        // kDepthAndHostDepthCopyToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepthAndHostDepthBuffer},
};

VulkanRenderTargetCache::VulkanRenderTargetCache(const xenos::RegisterFile& register_file,
                                                 const XenosSettings& settings,
                                                 IPhysicalMemory& memory,
                                                 u32 draw_resolution_scale_x,
                                                 u32 draw_resolution_scale_y,
                                                 VulkanCommandProcessor& command_processor)
    : RenderTargetCache(register_file, settings, memory, draw_resolution_scale_x,
                        draw_resolution_scale_y),
      command_processor_(command_processor) {}

VulkanRenderTargetCache::~VulkanRenderTargetCache() {
  Shutdown(true);
}

bool VulkanRenderTargetCache::Initialize(u32 shared_memory_binding_count) {
  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanInstance::Functions& ifn =
      vulkan_device->GetVulkanInstance()->GetFunctions();
  const VkPhysicalDevice physical_device = vulkan_device->GetPhysicalDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  const gpu::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->GetProperties();

  bool fsi_path_supported =
      (device_properties.fragmentShaderSampleInterlock ||
       device_properties.fragmentShaderPixelInterlock) &&
      device_properties.fragmentStoresAndAtomics && device_properties.sampleRateShading &&
      device_properties.standardSampleLocations &&
      shared_memory_binding_count < device_properties.maxPerStageDescriptorStorageBuffers;
  if (command_processor_.GetVulkanSettings().render_target_path.Get() == "fsi") {
    path_ = Path::kPixelShaderInterlock;
  } else {
    path_ = Path::kHostRenderTargets;
  }
  if (path_ == Path::kPixelShaderInterlock) {
    if (!fsi_path_supported) {
      path_ = Path::kHostRenderTargets;
    }
  }

  constexpr VkFormatFeatureFlags kUsedDepthFormatFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  constexpr VkFormatFeatureFlags kUsedColorFormatFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  bool gamma_render_target_as_unorm16_requested =
      path_ == Path::kHostRenderTargets &&
      command_processor_.GetXenosSettings().gamma_render_target_as_unorm16.Get();
  VkFormatProperties depth_unorm24_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_D24_UNORM_S8_UINT,
                                          &depth_unorm24_properties);
  depth_unorm24_vulkan_format_supported_ = (depth_unorm24_properties.optimalTilingFeatures &
                                            kUsedDepthFormatFeatures) == kUsedDepthFormatFeatures;
  VkFormatProperties color_rg16_snorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16_SNORM,
                                          &color_rg16_snorm_properties);
  VkFormatProperties color_rgba16_snorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16B16A16_SNORM,
                                          &color_rgba16_snorm_properties);
  VkFormatProperties color_rg16_sfloat_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16_SFLOAT,
                                          &color_rg16_sfloat_properties);
  VkFormatProperties color_rgba16_sfloat_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16B16A16_SFLOAT,
                                          &color_rgba16_sfloat_properties);
  VkFormatProperties color_rgba16_unorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16B16A16_UNORM,
                                          &color_rgba16_unorm_properties);
  bool color_rg16_snorm_supported = (color_rg16_snorm_properties.optimalTilingFeatures &
                                     kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rgba16_snorm_supported = (color_rgba16_snorm_properties.optimalTilingFeatures &
                                       kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rg16_sfloat_supported = (color_rg16_sfloat_properties.optimalTilingFeatures &
                                      kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rgba16_sfloat_supported = (color_rgba16_sfloat_properties.optimalTilingFeatures &
                                        kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rgba16_unorm_supported = (color_rgba16_unorm_properties.optimalTilingFeatures &
                                       kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
  bool color_rg16_draw_format_supported = true;
  if (!color_rg16_snorm_supported) {
    if (color_rg16_sfloat_supported) {
      color_rg16_draw_format_fallback_to_float_ = true;
      REXLOG_WARN(gpu,
                  "VulkanRenderTargetCache: R16G16_SNORM render target support is unavailable, "
                  "falling back to R16G16_SFLOAT for k_16_16");
    } else {
      color_rg16_draw_format_supported = false;
    }
  }
  bool color_rgba16_draw_format_supported = true;
  if (!color_rgba16_snorm_supported) {
    if (color_rgba16_sfloat_supported) {
      color_rgba16_draw_format_fallback_to_float_ = true;
      REXLOG_WARN(
          gpu,
          "VulkanRenderTargetCache: R16G16B16A16_SNORM render target support is unavailable, "
          "falling back to R16G16B16A16_SFLOAT for k_16_16_16_16");
    } else {
      color_rgba16_draw_format_supported = false;
    }
  }
  if (path_ == Path::kHostRenderTargets &&
      (!color_rg16_draw_format_supported || !color_rgba16_draw_format_supported)) {
    if (fsi_path_supported) {
      REXLOG_WARN(gpu,
                  "VulkanRenderTargetCache: Host render target 16-bit formats are unsupported "
                  "(R16G16: {}, R16G16B16A16: {}), switching to fragment shader interlock "
                  "path for D3D12 parity",
                  color_rg16_draw_format_supported ? "available" : "unavailable",
                  color_rgba16_draw_format_supported ? "available" : "unavailable");
      path_ = Path::kPixelShaderInterlock;
    } else {
      REX_FAIL(gpu, false,
               "VulkanRenderTargetCache: Host render target 16-bit formats are unsupported "
               "(R16G16: {}, R16G16B16A16: {}), and fragment shader interlock fallback "
               "is unavailable",
               color_rg16_draw_format_supported ? "available" : "unavailable",
               color_rgba16_draw_format_supported ? "available" : "unavailable");
    }
  }
  if (path_ == Path::kHostRenderTargets && gamma_render_target_as_unorm16_requested &&
      !color_rgba16_unorm_supported) {
    if (fsi_path_supported) {
      REXLOG_WARN(gpu,
                  "VulkanRenderTargetCache: R16G16B16A16_UNORM render target support "
                  "is unavailable for k_8_8_8_8_GAMMA linear storage. Switching to "
                  "fragment shader interlock path for D3D12 parity");
      path_ = Path::kPixelShaderInterlock;
    } else {
      REX_FAIL(gpu, false,
               "VulkanRenderTargetCache: R16G16B16A16_UNORM render target support "
               "is unavailable for k_8_8_8_8_GAMMA linear storage, and fragment "
               "shader interlock fallback is unavailable");
    }
  }
  VkFormatProperties color_rg16_uint_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16_UINT,
                                          &color_rg16_uint_properties);
  VkFormatProperties color_rgba16_uint_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16B16A16_UINT,
                                          &color_rgba16_uint_properties);
  VkFormatProperties color_r32_uint_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R32_UINT,
                                          &color_r32_uint_properties);
  VkFormatProperties color_rg32_uint_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R32G32_UINT,
                                          &color_rg32_uint_properties);
  color_16bit_transfer_uint_formats_supported_ =
      (color_rg16_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures &&
      (color_rgba16_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures;
  if (!color_16bit_transfer_uint_formats_supported_) {
    REXLOG_WARN(
        gpu,
        "VulkanRenderTargetCache: R16G16/R16G16B16A16 UINT ownership transfer formats are not "
        "supported");
  }
  color_32bit_transfer_uint_formats_supported_ =
      (color_r32_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures &&
      (color_rg32_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures;
  if (!color_32bit_transfer_uint_formats_supported_) {
    REXLOG_WARN(
        gpu,
        "VulkanRenderTargetCache: R32/R32G32 UINT ownership transfer formats are not supported");
  }
  if ((device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) &&
      !(device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_4_BIT)) {
    REXLOG_WARN(gpu, "VulkanRenderTargetCache: 4x integer sampled image support is unavailable");
  }

  if (command_processor_.GetXenosSettings().native_2x_msaa.Get()) {
    msaa_2x_attachments_supported_ =
        (device_properties.framebufferColorSampleCounts &
         device_properties.framebufferDepthSampleCounts &
         device_properties.framebufferStencilSampleCounts &
         device_properties.sampledImageColorSampleCounts &
         device_properties.sampledImageDepthSampleCounts &
         device_properties.sampledImageStencilSampleCounts & VK_SAMPLE_COUNT_2_BIT);
    msaa_2x_no_attachments_supported_ =
        (device_properties.framebufferNoAttachmentsSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0;
  } else {
    msaa_2x_attachments_supported_ = false;
    msaa_2x_no_attachments_supported_ = false;
  }
  bool integer_transfer_sample_1x_supported =
      (device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0 &&
      (device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
  bool integer_transfer_sample_2x_supported =
      (device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0 &&
      (device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0;
  bool integer_transfer_sample_4x_supported =
      (device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) == 0 ||
      (device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_4_BIT) != 0;
  bool bit_exact_host_color_transfer_supported =
      color_16bit_transfer_uint_formats_supported_ &&
      color_32bit_transfer_uint_formats_supported_ && integer_transfer_sample_1x_supported &&
      integer_transfer_sample_4x_supported &&
      (!msaa_2x_attachments_supported_ || integer_transfer_sample_2x_supported);
  if (path_ == Path::kHostRenderTargets && !bit_exact_host_color_transfer_supported) {
    if (fsi_path_supported) {
      REXLOG_WARN(gpu,
                  "VulkanRenderTargetCache: Host render target ownership transfers "
                  "can't be bit exact on this device. Switching to fragment shader "
                  "interlock path for D3D12 parity");
      path_ = Path::kPixelShaderInterlock;
    } else {
      REX_FAIL(gpu, false,
               "VulkanRenderTargetCache: Bit exact host render target ownership "
               "transfers require UINT transfer formats and integer sampled-image "
               "MSAA support, and fragment shader interlock fallback is unavailable");
    }
  }

  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings[2];
  descriptor_set_layout_bindings[0].binding = 0;
  descriptor_set_layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_bindings[0].descriptorCount = 1;
  descriptor_set_layout_bindings[0].stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_bindings[0].pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 1;
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_storage_buffer_) != VK_SUCCESS) {
    REXLOG_ERROR(gpu,
                 "VulkanRenderTargetCache: Failed to create the descriptor set layout "
                 "with one storage buffer");
    Shutdown();
    return false;
  }
  descriptor_set_layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    REXLOG_ERROR(gpu,
                 "VulkanRenderTargetCache: Failed to create the descriptor set layout "
                 "with one sampled image");
    Shutdown();
    return false;
  }
  descriptor_set_layout_bindings[1].binding = 1;
  descriptor_set_layout_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_layout_bindings[1].descriptorCount = 1;
  descriptor_set_layout_bindings[1].stageFlags = descriptor_set_layout_bindings[0].stageFlags;
  descriptor_set_layout_bindings[1].pImmutableSamplers = nullptr;
  descriptor_set_layout_create_info.bindingCount = 2;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_sampled_image_x2_) != VK_SUCCESS) {
    REXLOG_ERROR(gpu,
                 "VulkanRenderTargetCache: Failed to create the descriptor set layout "
                 "with two sampled images");
    Shutdown();
    return false;
  }

  VkDescriptorPoolSize descriptor_set_layout_size;
  descriptor_set_layout_size.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_layout_size.descriptorCount = 1;
  descriptor_set_pool_sampled_image_ = std::make_unique<gpu::vulkan::SingleLayoutDescriptorSetPool>(
      vulkan_device, 256, 1, &descriptor_set_layout_size, descriptor_set_layout_sampled_image_);
  descriptor_set_layout_size.descriptorCount = 2;
  descriptor_set_pool_sampled_image_x2_ =
      std::make_unique<gpu::vulkan::SingleLayoutDescriptorSetPool>(
          vulkan_device, 256, 1, &descriptor_set_layout_size,
          descriptor_set_layout_sampled_image_x2_);

  // 90 MB at 9x resolution scaling, within the minimum maxStorageBufferRange.
  if (!gpu::vulkan::objects::CreateDedicatedAllocationBuffer(
          vulkan_device,
          VkDeviceSize{xenos::kEDRAMSizeBytes} * GetDrawResolutionScaleX() *
              GetDrawResolutionScaleY(),
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          gpu::vulkan::objects::MemoryPurpose::kDeviceLocal, edram_buffer_, edram_buffer_memory_)) {
    REXLOG_ERROR(gpu, "VulkanRenderTargetCache: Failed to create the EDRAM buffer");
    Shutdown();
    return false;
  }
  if (GetPath() == Path::kPixelShaderInterlock) {
    edram_buffer_usage_ = EDRAMBufferUsage::kFragmentReadWrite;
  } else {
    edram_buffer_usage_ = EDRAMBufferUsage::kFragmentRead;
  }
  edram_buffer_modification_status_ = EDRAMBufferModificationStatus::kUnmodified;
  VkDescriptorPoolSize edram_storage_buffer_descriptor_pool_size;
  edram_storage_buffer_descriptor_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  edram_storage_buffer_descriptor_pool_size.descriptorCount = 1;
  VkDescriptorPoolCreateInfo edram_storage_buffer_descriptor_pool_create_info;
  edram_storage_buffer_descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  edram_storage_buffer_descriptor_pool_create_info.pNext = nullptr;
  edram_storage_buffer_descriptor_pool_create_info.flags = 0;
  edram_storage_buffer_descriptor_pool_create_info.maxSets = 1;
  edram_storage_buffer_descriptor_pool_create_info.poolSizeCount = 1;
  edram_storage_buffer_descriptor_pool_create_info.pPoolSizes =
      &edram_storage_buffer_descriptor_pool_size;
  if (dfn.vkCreateDescriptorPool(device, &edram_storage_buffer_descriptor_pool_create_info, nullptr,
                                 &edram_storage_buffer_descriptor_pool_) != VK_SUCCESS) {
    REXLOG_ERROR(gpu,
                 "VulkanRenderTargetCache: Failed to create the EDRAM buffer storage "
                 "buffer descriptor pool");
    Shutdown();
    return false;
  }
  VkDescriptorSetAllocateInfo edram_storage_buffer_descriptor_set_allocate_info;
  edram_storage_buffer_descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  edram_storage_buffer_descriptor_set_allocate_info.pNext = nullptr;
  edram_storage_buffer_descriptor_set_allocate_info.descriptorPool =
      edram_storage_buffer_descriptor_pool_;
  edram_storage_buffer_descriptor_set_allocate_info.descriptorSetCount = 1;
  edram_storage_buffer_descriptor_set_allocate_info.pSetLayouts =
      &descriptor_set_layout_storage_buffer_;
  if (dfn.vkAllocateDescriptorSets(device, &edram_storage_buffer_descriptor_set_allocate_info,
                                   &edram_storage_buffer_descriptor_set_) != VK_SUCCESS) {
    REXLOG_ERROR(gpu,
                 "VulkanRenderTargetCache: Failed to allocate the EDRAM buffer storage "
                 "buffer descriptor set");
    Shutdown();
    return false;
  }
  VkDescriptorBufferInfo edram_storage_buffer_descriptor_buffer_info;
  edram_storage_buffer_descriptor_buffer_info.buffer = edram_buffer_;
  edram_storage_buffer_descriptor_buffer_info.offset = 0;
  edram_storage_buffer_descriptor_buffer_info.range = VK_WHOLE_SIZE;
  VkWriteDescriptorSet edram_storage_buffer_descriptor_write;
  edram_storage_buffer_descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  edram_storage_buffer_descriptor_write.pNext = nullptr;
  edram_storage_buffer_descriptor_write.dstSet = edram_storage_buffer_descriptor_set_;
  edram_storage_buffer_descriptor_write.dstBinding = 0;
  edram_storage_buffer_descriptor_write.dstArrayElement = 0;
  edram_storage_buffer_descriptor_write.descriptorCount = 1;
  edram_storage_buffer_descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  edram_storage_buffer_descriptor_write.pImageInfo = nullptr;
  edram_storage_buffer_descriptor_write.pBufferInfo = &edram_storage_buffer_descriptor_buffer_info;
  edram_storage_buffer_descriptor_write.pTexelBufferView = nullptr;
  dfn.vkUpdateDescriptorSets(device, 1, &edram_storage_buffer_descriptor_write, 0, nullptr);

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  VkDescriptorSetLayout resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetCount] = {};
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetEDRAM] =
      descriptor_set_layout_storage_buffer_;
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetDest] =
      command_processor_.GetSingleTransientDescriptorLayout(
          VulkanCommandProcessor::SingleTransientDescriptorLayout ::kStorageBufferCompute);
  VkPushConstantRange resolve_copy_push_constant_range;
  resolve_copy_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  resolve_copy_push_constant_range.offset = 0;
  // Unscaled resolve may bind all of shared memory, scaled resolve only portions of it.
  resolve_copy_push_constant_range.size =
      draw_resolution_scaled ? sizeof(draw_state::ResolveCopyShaderConstants::DestRelative)
                             : sizeof(draw_state::ResolveCopyShaderConstants);
  VkPipelineLayoutCreateInfo resolve_copy_pipeline_layout_create_info;
  resolve_copy_pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  resolve_copy_pipeline_layout_create_info.pNext = nullptr;
  resolve_copy_pipeline_layout_create_info.flags = 0;
  resolve_copy_pipeline_layout_create_info.setLayoutCount = kResolveCopyDescriptorSetCount;
  resolve_copy_pipeline_layout_create_info.pSetLayouts = resolve_copy_descriptor_set_layouts;
  resolve_copy_pipeline_layout_create_info.pushConstantRangeCount = 1;
  resolve_copy_pipeline_layout_create_info.pPushConstantRanges = &resolve_copy_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &resolve_copy_pipeline_layout_create_info, nullptr,
                                 &resolve_copy_pipeline_layout_) != VK_SUCCESS) {
    REXLOG_ERROR(gpu,
                 "VulkanRenderTargetCache: Failed to create the resolve copy pipeline "
                 "layout");
    Shutdown();
    return false;
  }

  auto create_direct_resolve_pipeline_layout = [&](VkDescriptorSetLayout source_layout,
                                                   VkPipelineLayout* pipeline_layout_out) {
    VkDescriptorSetLayout descriptor_set_layouts[] = {
        command_processor_.GetSingleTransientDescriptorLayout(
            VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute),
        source_layout,
    };
    VkPushConstantRange push_constant_range;
    push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(DirectResolvePushConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_create_info;
    pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.pNext = nullptr;
    pipeline_layout_create_info.flags = 0;
    pipeline_layout_create_info.setLayoutCount =
        static_cast<u32>(rex::CountOf(descriptor_set_layouts));
    pipeline_layout_create_info.pSetLayouts = descriptor_set_layouts;
    pipeline_layout_create_info.pushConstantRangeCount = 1;
    pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                   pipeline_layout_out) != VK_SUCCESS) {
      *pipeline_layout_out = VK_NULL_HANDLE;
    }
  };
  create_direct_resolve_pipeline_layout(descriptor_set_layout_sampled_image_,
                                        &direct_resolve_pipeline_layout_color_);
  create_direct_resolve_pipeline_layout(descriptor_set_layout_sampled_image_x2_,
                                        &direct_resolve_pipeline_layout_depth_);

  for (size_t i = 0; i < static_cast<size_t>(draw_state::ResolveCopyShaderIndex::kCount); ++i) {
    const draw_state::ResolveCopyShaderTraits& resolve_copy_shader_info =
        draw_state::kResolveCopyShaderInfo[i];
    const ResolveCopyShaderCode& resolve_copy_shader_code = kResolveCopyShaders[i];
    REX_ASSERT(resolve_copy_shader_code.unscaled && resolve_copy_shader_code.unscaled_size_bytes &&
               resolve_copy_shader_code.scaled && resolve_copy_shader_code.scaled_size_bytes);
    VkPipeline resolve_copy_pipeline = gpu::vulkan::objects::CreateComputePipeline(
        vulkan_device, resolve_copy_pipeline_layout_,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled
                               : resolve_copy_shader_code.unscaled,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled_size_bytes
                               : resolve_copy_shader_code.unscaled_size_bytes);
    if (resolve_copy_pipeline == VK_NULL_HANDLE) {
      REXLOG_ERROR(gpu,
                   "VulkanRenderTargetCache: Failed to create the resolve copy "
                   "pipeline {}",
                   resolve_copy_shader_info.debug_name);
      Shutdown();
      return false;
    }
    vulkan_device->SetObjectName(VK_OBJECT_TYPE_PIPELINE, resolve_copy_pipeline,
                                 resolve_copy_shader_info.debug_name);
    resolve_copy_pipelines_[i] = resolve_copy_pipeline;
  }

  if (path_ == Path::kHostRenderTargets) {
    gamma_render_target_as_unorm16_ = gamma_render_target_as_unorm16_requested;

    depth_float24_round_ = command_processor_.GetXenosSettings().depth_float24_round.Get();
    depth_float24_convert_in_pixel_shader_ =
        command_processor_.GetXenosSettings().depth_float24_convert_in_pixel_shader.Get();

    VkDescriptorSetLayout host_depth_store_descriptor_set_layouts[] = {
        descriptor_set_layout_storage_buffer_,
        // Only depth is used.
        descriptor_set_layout_sampled_image_x2_,
    };
    VkPushConstantRange host_depth_store_push_constant_range;
    host_depth_store_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    host_depth_store_push_constant_range.offset = 0;
    host_depth_store_push_constant_range.size = sizeof(HostDepthStoreConstants);
    VkPipelineLayoutCreateInfo host_depth_store_pipeline_layout_create_info;
    host_depth_store_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    host_depth_store_pipeline_layout_create_info.pNext = nullptr;
    host_depth_store_pipeline_layout_create_info.flags = 0;
    host_depth_store_pipeline_layout_create_info.setLayoutCount =
        static_cast<u32>(rex::CountOf(host_depth_store_descriptor_set_layouts));
    host_depth_store_pipeline_layout_create_info.pSetLayouts =
        host_depth_store_descriptor_set_layouts;
    host_depth_store_pipeline_layout_create_info.pushConstantRangeCount = 1;
    host_depth_store_pipeline_layout_create_info.pPushConstantRanges =
        &host_depth_store_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &host_depth_store_pipeline_layout_create_info, nullptr,
                                   &host_depth_store_pipeline_layout_) != VK_SUCCESS) {
      REXLOG_ERROR(gpu,
                   "VulkanRenderTargetCache: Failed to create the host depth store "
                   "pipeline layout");
      Shutdown();
      return false;
    }
    const std::pair<const u32*, size_t> host_depth_store_shaders[] = {
        {shaders::kHostDepthStore1xMSAACS, sizeof(shaders::kHostDepthStore1xMSAACS)},
        {shaders::kHostDepthStore2xMSAACS, sizeof(shaders::kHostDepthStore2xMSAACS)},
        {shaders::kHostDepthStore4xMSAACS, sizeof(shaders::kHostDepthStore4xMSAACS)},
    };
    for (size_t i = 0; i < rex::CountOf(host_depth_store_shaders); ++i) {
      const std::pair<const u32*, size_t> host_depth_store_shader = host_depth_store_shaders[i];
      VkPipeline host_depth_store_pipeline = gpu::vulkan::objects::CreateComputePipeline(
          vulkan_device, host_depth_store_pipeline_layout_, host_depth_store_shader.first,
          host_depth_store_shader.second);
      if (host_depth_store_pipeline == VK_NULL_HANDLE) {
        REXLOG_ERROR(gpu,
                     "VulkanRenderTargetCache: Failed to create the {}-sample host "
                     "depth write pipeline",
                     static_cast<u32>(1) << i);
        Shutdown();
        return false;
      }
      host_depth_store_pipelines_[i] = host_depth_store_pipeline;
    }

    // Transfer and clear vertex buffer, for quads of up to tile granularity.
    transfer_vertex_buffer_pool_ = std::make_unique<gpu::vulkan::VulkanUploadBufferPool>(
        vulkan_device, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        std::max(
            gpu::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
            sizeof(f32) * 2 * 6 * Transfer::kMaxCutoutBorderRectangles * xenos::kEDRAMTileCount));

    transfer_passthrough_vertex_shader_ =
        gpu::vulkan::objects::CreateShaderModule(vulkan_device, shaders::kPassthroughPositionXYVS,
                                                 sizeof(shaders::kPassthroughPositionXYVS));
    if (transfer_passthrough_vertex_shader_ == VK_NULL_HANDLE) {
      REXLOG_ERROR(gpu,
                   "VulkanRenderTargetCache: Failed to create the render target "
                   "ownership transfer vertex shader");
      Shutdown();
      return false;
    }

    VkDescriptorSetLayout
        transfer_pipeline_layout_descriptor_set_layouts[kTransferUsedDescriptorSetCount];
    VkPushConstantRange transfer_pipeline_layout_push_constant_range;
    transfer_pipeline_layout_push_constant_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    transfer_pipeline_layout_push_constant_range.offset = 0;
    VkPipelineLayoutCreateInfo transfer_pipeline_layout_create_info;
    transfer_pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    transfer_pipeline_layout_create_info.pNext = nullptr;
    transfer_pipeline_layout_create_info.flags = 0;
    transfer_pipeline_layout_create_info.pSetLayouts =
        transfer_pipeline_layout_descriptor_set_layouts;
    transfer_pipeline_layout_create_info.pPushConstantRanges =
        &transfer_pipeline_layout_push_constant_range;
    for (size_t i = 0; i < static_cast<size_t>(TransferPipelineLayoutIndex::kCount); ++i) {
      const TransferPipelineLayoutInfo& transfer_pipeline_layout_info =
          kTransferPipelineLayoutInfos[i];
      transfer_pipeline_layout_create_info.setLayoutCount = 0;
      u32 transfer_pipeline_layout_descriptor_sets_remaining =
          transfer_pipeline_layout_info.used_descriptor_sets;
      u32 transfer_pipeline_layout_descriptor_set_index;
      while (rex::bit_scan_forward(transfer_pipeline_layout_descriptor_sets_remaining,
                                   &transfer_pipeline_layout_descriptor_set_index)) {
        transfer_pipeline_layout_descriptor_sets_remaining &=
            ~(static_cast<u32>(1) << transfer_pipeline_layout_descriptor_set_index);
        VkDescriptorSetLayout transfer_pipeline_layout_descriptor_set_layout = VK_NULL_HANDLE;
        switch (
            static_cast<TransferUsedDescriptorSet>(transfer_pipeline_layout_descriptor_set_index)) {
          case kTransferUsedDescriptorSetHostDepthBuffer:
            transfer_pipeline_layout_descriptor_set_layout = descriptor_set_layout_storage_buffer_;
            break;
          case kTransferUsedDescriptorSetHostDepthStencilTextures:
          case kTransferUsedDescriptorSetDepthStencilTextures:
            transfer_pipeline_layout_descriptor_set_layout =
                descriptor_set_layout_sampled_image_x2_;
            break;
          case kTransferUsedDescriptorSetColorTexture:
            transfer_pipeline_layout_descriptor_set_layout = descriptor_set_layout_sampled_image_;
            break;
          default:
            REX_UNHANDLED_CASE(static_cast<TransferUsedDescriptorSet>(
                transfer_pipeline_layout_descriptor_set_index));
        }
        transfer_pipeline_layout_descriptor_set_layouts[transfer_pipeline_layout_create_info
                                                            .setLayoutCount++] =
            transfer_pipeline_layout_descriptor_set_layout;
      }
      transfer_pipeline_layout_push_constant_range.size = static_cast<u32>(
          sizeof(u32) * rex::BitCount(transfer_pipeline_layout_info.used_push_constant_dwords));
      transfer_pipeline_layout_create_info.pushConstantRangeCount =
          transfer_pipeline_layout_info.used_push_constant_dwords ? 1 : 0;
      if (dfn.vkCreatePipelineLayout(device, &transfer_pipeline_layout_create_info, nullptr,
                                     &transfer_pipeline_layouts_[i]) != VK_SUCCESS) {
        REXLOG_ERROR(gpu,
                     "VulkanRenderTargetCache: Failed to create the render target "
                     "ownership transfer pipeline layout {}",
                     i);
        Shutdown();
        return false;
      }
    }

    VkDescriptorSetLayout dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetCount];
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetEDRAM] =
        descriptor_set_layout_storage_buffer_;
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_;
    VkPushConstantRange dump_pipeline_layout_push_constant_range;
    dump_pipeline_layout_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    dump_pipeline_layout_push_constant_range.offset = 0;
    dump_pipeline_layout_push_constant_range.size = sizeof(u32) * kDumpPushConstantCount;
    VkPipelineLayoutCreateInfo dump_pipeline_layout_create_info;
    dump_pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    dump_pipeline_layout_create_info.pNext = nullptr;
    dump_pipeline_layout_create_info.flags = 0;
    dump_pipeline_layout_create_info.setLayoutCount =
        static_cast<u32>(rex::CountOf(dump_pipeline_layout_descriptor_set_layouts));
    dump_pipeline_layout_create_info.pSetLayouts = dump_pipeline_layout_descriptor_set_layouts;
    dump_pipeline_layout_create_info.pushConstantRangeCount = 1;
    dump_pipeline_layout_create_info.pPushConstantRanges =
        &dump_pipeline_layout_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &dump_pipeline_layout_create_info, nullptr,
                                   &dump_pipeline_layout_color_) != VK_SUCCESS) {
      REXLOG_ERROR(gpu,
                   "VulkanRenderTargetCache: Failed to create the color render target "
                   "dumping pipeline layout");
      Shutdown();
      return false;
    }
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_x2_;
    if (dfn.vkCreatePipelineLayout(device, &dump_pipeline_layout_create_info, nullptr,
                                   &dump_pipeline_layout_depth_) != VK_SUCCESS) {
      REXLOG_ERROR(gpu,
                   "VulkanRenderTargetCache: Failed to create the depth render target "
                   "dumping pipeline layout");
      Shutdown();
      return false;
    }
  } else if (path_ == Path::kPixelShaderInterlock) {
    // This path emulates 2x as 4x like D3D12 ROV.
    msaa_2x_attachments_supported_ = false;
    msaa_2x_no_attachments_supported_ = false;

    // Blending is done in linear space in shaders.
    gamma_render_target_as_unorm16_ = false;

    // float24 depth is always rounded to the nearest even.
    depth_float24_round_ = true;
    depth_float24_convert_in_pixel_shader_ = true;

    VkPushConstantRange resolve_fsi_clear_push_constant_range;
    resolve_fsi_clear_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_fsi_clear_push_constant_range.offset = 0;
    resolve_fsi_clear_push_constant_range.size = sizeof(draw_state::ResolveClearShaderConstants);
    VkPipelineLayoutCreateInfo resolve_fsi_clear_pipeline_layout_create_info;
    resolve_fsi_clear_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    resolve_fsi_clear_pipeline_layout_create_info.pNext = nullptr;
    resolve_fsi_clear_pipeline_layout_create_info.flags = 0;
    resolve_fsi_clear_pipeline_layout_create_info.setLayoutCount = 1;
    resolve_fsi_clear_pipeline_layout_create_info.pSetLayouts =
        &descriptor_set_layout_storage_buffer_;
    resolve_fsi_clear_pipeline_layout_create_info.pushConstantRangeCount = 1;
    resolve_fsi_clear_pipeline_layout_create_info.pPushConstantRanges =
        &resolve_fsi_clear_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &resolve_fsi_clear_pipeline_layout_create_info, nullptr,
                                   &resolve_fsi_clear_pipeline_layout_) != VK_SUCCESS) {
      REXLOG_ERROR(gpu,
                   "VulkanRenderTargetCache: Failed to create the resolve EDRAM buffer "
                   "clear pipeline layout");
      Shutdown();
      return false;
    }
    resolve_fsi_clear_32bpp_pipeline_ = gpu::vulkan::objects::CreateComputePipeline(
        vulkan_device, resolve_fsi_clear_pipeline_layout_,
        draw_resolution_scaled ? shaders::kResolveClear32BPPScaledCS
                               : shaders::kResolveClear32BPPCS,
        draw_resolution_scaled ? sizeof(shaders::kResolveClear32BPPScaledCS)
                               : sizeof(shaders::kResolveClear32BPPCS));
    if (resolve_fsi_clear_32bpp_pipeline_ == VK_NULL_HANDLE) {
      REXLOG_ERROR(gpu,
                   "VulkanRenderTargetCache: Failed to create the 32bpp resolve EDRAM "
                   "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_fsi_clear_64bpp_pipeline_ = gpu::vulkan::objects::CreateComputePipeline(
        vulkan_device, resolve_fsi_clear_pipeline_layout_,
        draw_resolution_scaled ? shaders::kResolveClear64BPPScaledCS
                               : shaders::kResolveClear64BPPCS,
        draw_resolution_scaled ? sizeof(shaders::kResolveClear64BPPScaledCS)
                               : sizeof(shaders::kResolveClear64BPPCS));
    if (resolve_fsi_clear_64bpp_pipeline_ == VK_NULL_HANDLE) {
      REXLOG_ERROR(gpu,
                   "VulkanRenderTargetCache: Failed to create the 64bpp resolve EDRAM "
                   "buffer clear pipeline");
      Shutdown();
      return false;
    }

    VkSubpassDescription fsi_subpass = {};
    fsi_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkSubpassDependency fsi_subpass_dependencies[2];
    fsi_subpass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    fsi_subpass_dependencies[0].dstSubpass = 0;
    fsi_subpass_dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    fsi_subpass_dependencies[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    fsi_subpass_dependencies[0].srcAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fsi_subpass_dependencies[0].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fsi_subpass_dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    fsi_subpass_dependencies[1] = fsi_subpass_dependencies[0];
    std::swap(fsi_subpass_dependencies[1].srcSubpass, fsi_subpass_dependencies[1].dstSubpass);
    VkRenderPassCreateInfo fsi_render_pass_create_info;
    fsi_render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    fsi_render_pass_create_info.pNext = nullptr;
    fsi_render_pass_create_info.flags = 0;
    fsi_render_pass_create_info.attachmentCount = 0;
    fsi_render_pass_create_info.pAttachments = nullptr;
    fsi_render_pass_create_info.subpassCount = 1;
    fsi_render_pass_create_info.pSubpasses = &fsi_subpass;
    fsi_render_pass_create_info.dependencyCount =
        static_cast<u32>(rex::CountOf(fsi_subpass_dependencies));
    fsi_render_pass_create_info.pDependencies = fsi_subpass_dependencies;
    if (dfn.vkCreateRenderPass(device, &fsi_render_pass_create_info, nullptr, &fsi_render_pass_) !=
        VK_SUCCESS) {
      REXLOG_ERROR(gpu,
                   "VulkanRenderTargetCache: Failed to create the fragment shader "
                   "interlock render backend render pass");
      Shutdown();
      return false;
    }

    VkFramebufferCreateInfo fsi_framebuffer_create_info;
    fsi_framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fsi_framebuffer_create_info.pNext = nullptr;
    fsi_framebuffer_create_info.flags = 0;
    fsi_framebuffer_create_info.renderPass = fsi_render_pass_;
    fsi_framebuffer_create_info.attachmentCount = 0;
    fsi_framebuffer_create_info.pAttachments = nullptr;
    fsi_framebuffer_create_info.width =
        std::min(xenos::kTexture2DCubeMaxWidthHeight * GetDrawResolutionScaleX(),
                 device_properties.maxFramebufferWidth);
    fsi_framebuffer_create_info.height =
        std::min(xenos::kTexture2DCubeMaxWidthHeight * GetDrawResolutionScaleY(),
                 device_properties.maxFramebufferHeight);
    fsi_framebuffer_create_info.layers = 1;
    if (dfn.vkCreateFramebuffer(device, &fsi_framebuffer_create_info, nullptr,
                                &fsi_framebuffer_.framebuffer) != VK_SUCCESS) {
      REXLOG_ERROR(gpu,
                   "VulkanRenderTargetCache: Failed to create the fragment shader "
                   "interlock render backend framebuffer");
      Shutdown();
      return false;
    }
    fsi_framebuffer_.host_extent.width = fsi_framebuffer_create_info.width;
    fsi_framebuffer_.host_extent.height = fsi_framebuffer_create_info.height;
  } else {
    REX_UNHANDLED_CASE(path_);
    Shutdown();
    return false;
  }

  last_update_render_pass_key_ = RenderPassKey();
  last_update_render_pass_ = VK_NULL_HANDLE;
  last_update_framebuffer_pitch_tiles_at_32bpp_ = 0;
  std::ranges::fill(last_update_framebuffer_attachments_, nullptr);
  last_update_framebuffer_ = VK_NULL_HANDLE;

  InitializeCommon();
  return true;
}

void VulkanRenderTargetCache::Shutdown(bool from_destructor) {
  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  // Must precede pool destruction, as ShutdownCommon runs too late from the destructor.
  DestroyAllRenderTargets();

  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             resolve_fsi_clear_64bpp_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             resolve_fsi_clear_32bpp_pipeline_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             resolve_fsi_clear_pipeline_layout_);

  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                             fsi_framebuffer_.framebuffer);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device, fsi_render_pass_);

  for (const auto& dump_pipeline_pair : dump_pipelines_) {
    if (dump_pipeline_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, dump_pipeline_pair.second, nullptr);
    }
  }
  dump_pipelines_.clear();
  for (const auto& direct_resolve_pipeline_pair : direct_resolve_pipelines_) {
    bool aliased_resolve_copy_pipeline = false;
    for (VkPipeline resolve_copy_pipeline : resolve_copy_pipelines_) {
      if (direct_resolve_pipeline_pair.second == resolve_copy_pipeline) {
        aliased_resolve_copy_pipeline = true;
        break;
      }
    }
    if (direct_resolve_pipeline_pair.second != VK_NULL_HANDLE && !aliased_resolve_copy_pipeline) {
      dfn.vkDestroyPipeline(device, direct_resolve_pipeline_pair.second, nullptr);
    }
  }
  direct_resolve_pipelines_.clear();
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             direct_resolve_pipeline_layout_depth_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             direct_resolve_pipeline_layout_color_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             dump_pipeline_layout_depth_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             dump_pipeline_layout_color_);

  for (const auto& transfer_pipeline_array_pair : transfer_pipelines_) {
    for (VkPipeline transfer_pipeline : transfer_pipeline_array_pair.second) {
      if (transfer_pipeline != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, transfer_pipeline, nullptr);
      }
    }
  }
  transfer_pipelines_.clear();
  for (const auto& transfer_shader_pair : transfer_shaders_) {
    if (transfer_shader_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, transfer_shader_pair.second, nullptr);
    }
  }
  transfer_shaders_.clear();
  for (size_t i = 0; i < static_cast<size_t>(TransferPipelineLayoutIndex::kCount); ++i) {
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                               transfer_pipeline_layouts_[i]);
  }
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                             transfer_passthrough_vertex_shader_);
  transfer_vertex_buffer_pool_.reset();

  for (auto& pipeline : host_depth_store_pipelines_) {
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
  }
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             host_depth_store_pipeline_layout_);

  last_update_framebuffer_ = VK_NULL_HANDLE;
  for (const auto& framebuffer_pair : framebuffers_) {
    dfn.vkDestroyFramebuffer(device, framebuffer_pair.second.framebuffer, nullptr);
  }
  framebuffers_.clear();

  last_update_render_pass_ = VK_NULL_HANDLE;
  for (const auto& render_pass_pair : render_passes_) {
    if (render_pass_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_pair.second, nullptr);
    }
  }
  render_passes_.clear();

  for (VkPipeline& resolve_copy_pipeline : resolve_copy_pipelines_) {
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                               resolve_copy_pipeline);
  }
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             resolve_copy_pipeline_layout_);

  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                             edram_storage_buffer_descriptor_pool_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, edram_buffer_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device, edram_buffer_memory_);

  descriptor_set_pool_sampled_image_x2_.reset();
  descriptor_set_pool_sampled_image_.reset();

  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                             descriptor_set_layout_sampled_image_x2_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                             descriptor_set_layout_sampled_image_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                             descriptor_set_layout_storage_buffer_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanRenderTargetCache::ClearCache() {
  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  // Framebuffers reference attachment image views, which the common ClearCache may remove.
  last_update_framebuffer_ = VK_NULL_HANDLE;
  for (const auto& framebuffer_pair : framebuffers_) {
    dfn.vkDestroyFramebuffer(device, framebuffer_pair.second.framebuffer, nullptr);
  }
  framebuffers_.clear();

  last_update_render_pass_ = VK_NULL_HANDLE;
  for (const auto& render_pass_pair : render_passes_) {
    dfn.vkDestroyRenderPass(device, render_pass_pair.second, nullptr);
  }
  render_passes_.clear();

  RenderTargetCache::ClearCache();
}

void VulkanRenderTargetCache::CompletedSubmissionUpdated() {
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  }
}

void VulkanRenderTargetCache::EndSubmission() {
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->FlushWrites();
  }
}

bool VulkanRenderTargetCache::Resolve(IPhysicalMemory& memory, VulkanSharedMemory& shared_memory,
                                      VulkanTextureCache& texture_cache, u32& written_address_out,
                                      u32& written_length_out) {
  written_address_out = 0;
  written_length_out = 0;

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  draw_state::ResolveRequest resolve_info;
  if (!draw_state::GetResolveInfo(GetXenosSettings(), GetRegisterFile(), memory,
                                  GetDrawResolutionScaleX(), GetDrawResolutionScaleY(),
                                  IsFixedRG16TruncatedToMinus1To1(),
                                  IsFixedRGBA16TruncatedToMinus1To1(), resolve_info)) {
    return false;
  }

  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }

  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  DeferredCommandBuffer& command_buffer = command_processor_.GetDeferredCommandBuffer();

  bool copied = false;
  if (resolve_info.copy_dest_extent_length) {
    draw_state::ResolveCopyShaderConstants copy_shader_constants;
    u32 copy_group_count_x, copy_group_count_y;
    draw_state::ResolveCopyShaderIndex copy_shader =
        resolve_info.GetCopyShader(GetDrawResolutionScaleX(), GetDrawResolutionScaleY(),
                                   copy_shader_constants, copy_group_count_x, copy_group_count_y);
    REX_ASSERT(copy_group_count_x && copy_group_count_y);
    if (copy_shader != draw_state::ResolveCopyShaderIndex::kUnknown) {
      const draw_state::ResolveCopyShaderTraits& copy_shader_info =
          draw_state::kResolveCopyShaderInfo[static_cast<size_t>(copy_shader)];
      bool direct_resolved = false;
      if (GetPath() == Path::kHostRenderTargets) {
        if (command_processor_.GetXenosSettings().direct_host_resolve.Get()) {
          direct_resolved = TryDirectResolve(resolve_info, copy_shader, draw_resolution_scaled);
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
                     "VulkanRenderTargetCache: Failed to dump host render targets for resolve");
          }
        }
      }

      u32 copy_dest_range_unscaled = resolve_info.copy_dest_extent_start -
                                     resolve_info.copy_dest_base +
                                     resolve_info.copy_dest_extent_length;
      u64 copy_dest_base = resolve_info.copy_dest_base;
      u64 copy_dest_range_length = copy_dest_range_unscaled;
      u64 copy_dest_use_start = resolve_info.copy_dest_extent_start;
      u64 copy_dest_use_length = resolve_info.copy_dest_extent_length;
      if (draw_resolution_scaled) {
        if (!texture_cache.GetScaledResolveRange(
                resolve_info.copy_dest_base, copy_dest_range_unscaled,
                copy_shader_info.dest_bpe_log2, copy_dest_base, copy_dest_range_length) ||
            !texture_cache.GetScaledResolveRange(
                resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length,
                copy_shader_info.dest_bpe_log2, copy_dest_use_start, copy_dest_use_length)) {
          REX_FAIL(gpu, false,
                   "VulkanRenderTargetCache: Failed to map scaled resolve "
                   "destination range (base={:08X}, length={:08X})",
                   resolve_info.copy_dest_base, copy_dest_range_unscaled);
        }
      }

      bool copy_dest_committed;
      if (draw_resolution_scaled) {
        copy_dest_committed = texture_cache.CommitScaledResolveRange(
            resolve_info.copy_dest_base, copy_dest_range_unscaled, copy_shader_info.dest_bpe_log2);
      } else {
        copy_dest_committed = shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                                         resolve_info.copy_dest_extent_length);
      }
      if (!copy_dest_committed) {
        REXLOG_ERROR(gpu,
                     "VulkanRenderTargetCache: Failed to obtain the resolve destination "
                     "memory region");
      } else {
        VkDescriptorSet descriptor_set_dest = command_processor_.AllocateSingleTransientDescriptor(
            VulkanCommandProcessor::SingleTransientDescriptorLayout ::kStorageBufferCompute);
        if (descriptor_set_dest != VK_NULL_HANDLE) {
          VkDescriptorBufferInfo write_descriptor_set_dest_buffer_info;
          write_descriptor_set_dest_buffer_info.buffer =
              draw_resolution_scaled ? texture_cache.GetScaledResolveBuffer()
                                     : shared_memory.GetBuffer();
          write_descriptor_set_dest_buffer_info.offset = copy_dest_base;
          write_descriptor_set_dest_buffer_info.range = copy_dest_range_length;
          VkWriteDescriptorSet write_descriptor_set_dest;
          write_descriptor_set_dest.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          write_descriptor_set_dest.pNext = nullptr;
          write_descriptor_set_dest.dstSet = descriptor_set_dest;
          write_descriptor_set_dest.dstBinding = 0;
          write_descriptor_set_dest.dstArrayElement = 0;
          write_descriptor_set_dest.descriptorCount = 1;
          write_descriptor_set_dest.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          write_descriptor_set_dest.pImageInfo = nullptr;
          write_descriptor_set_dest.pBufferInfo = &write_descriptor_set_dest_buffer_info;
          write_descriptor_set_dest.pTexelBufferView = nullptr;
          dfn.vkUpdateDescriptorSets(device, 1, &write_descriptor_set_dest, 0, nullptr);

          if (draw_resolution_scaled) {
            texture_cache.UseScaledResolveBufferForWrite(copy_dest_use_start, copy_dest_use_length);
          } else {
            shared_memory.Use(VulkanSharedMemory::Usage::kComputeWrite,
                              std::pair<u32, u32>(static_cast<u32>(copy_dest_use_start),
                                                  static_cast<u32>(copy_dest_use_length)));
          }
          UseEDRAMBuffer(EDRAMBufferUsage::kComputeRead);
          command_processor_.BindExternalComputePipeline(
              resolve_copy_pipelines_[static_cast<size_t>(copy_shader)]);
          VkDescriptorSet descriptor_sets[kResolveCopyDescriptorSetCount] = {};
          descriptor_sets[kResolveCopyDescriptorSetEDRAM] = edram_storage_buffer_descriptor_set_;
          descriptor_sets[kResolveCopyDescriptorSetDest] = descriptor_set_dest;
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_COMPUTE, resolve_copy_pipeline_layout_, 0,
              static_cast<u32>(rex::CountOf(descriptor_sets)), descriptor_sets, 0, nullptr);
          if (draw_resolution_scaled) {
            command_buffer.CmdVkPushConstants(
                resolve_copy_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(copy_shader_constants.dest_relative), &copy_shader_constants.dest_relative);
          } else {
            copy_shader_constants.dest_base -=
                static_cast<u32>(write_descriptor_set_dest_buffer_info.offset);
            command_buffer.CmdVkPushConstants(
                resolve_copy_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(copy_shader_constants), &copy_shader_constants);
          }
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(copy_group_count_x, copy_group_count_y, 1);

          texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                            resolve_info.copy_dest_extent_length);
          written_address_out = resolve_info.copy_dest_extent_start;
          written_length_out = resolve_info.copy_dest_extent_length;
          copied = true;
        }
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
        // False means an empty region (success) or an error, and both are ignored.
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
        UseEDRAMBuffer(EDRAMBufferUsage::kComputeWrite);
        // The depth and color ranges are assumed not to overlap, so committing once is enough.
        CommitEDRAMBufferShaderWrites();
        command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                               resolve_fsi_clear_pipeline_layout_, 0, 1,
                                               &edram_storage_buffer_descriptor_set_, 0, nullptr);
        std::pair<u32, u32> clear_group_count = resolve_info.GetClearShaderGroupCount(
            GetDrawResolutionScaleX(), GetDrawResolutionScaleY());
        REX_ASSERT(clear_group_count.first && clear_group_count.second);
        if (clear_depth) {
          command_processor_.BindExternalComputePipeline(resolve_fsi_clear_32bpp_pipeline_);
          draw_state::ResolveClearShaderConstants depth_clear_constants;
          resolve_info.GetDepthClearShaderConstants(depth_clear_constants);
          command_buffer.CmdVkPushConstants(resolve_fsi_clear_pipeline_layout_,
                                            VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                            sizeof(depth_clear_constants), &depth_clear_constants);
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(clear_group_count.first, clear_group_count.second, 1);
        }
        if (clear_color) {
          command_processor_.BindExternalComputePipeline(
              resolve_info.color_edram_info.format_is_64bpp ? resolve_fsi_clear_64bpp_pipeline_
                                                            : resolve_fsi_clear_32bpp_pipeline_);
          draw_state::ResolveClearShaderConstants color_clear_constants;
          resolve_info.GetColorClearShaderConstants(color_clear_constants);
          if (clear_depth) {
            // The depth clear already pushed the shared constants.
            command_buffer.CmdVkPushConstants(
                resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                static_cast<u32>(offsetof(draw_state::ResolveClearShaderConstants, rt_specific)),
                sizeof(color_clear_constants.rt_specific), &color_clear_constants.rt_specific);
          } else {
            command_buffer.CmdVkPushConstants(
                resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(color_clear_constants), &color_clear_constants);
          }
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(clear_group_count.first, clear_group_count.second, 1);
        }
        MarkEDRAMBufferModified();
        cleared = true;
      } break;
      default:
        REX_UNHANDLED_CASE(GetPath());
    }
  } else {
    cleared = true;
  }

  return copied && cleared;
}

bool VulkanRenderTargetCache::Update(bool is_rasterization_done,
                                     xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
                                     u32 normalized_color_mask, const Shader& vertex_shader) {
  if (!RenderTargetCache::Update(is_rasterization_done, normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    return false;
  }

  auto rb_surface_info = GetRegisterFile().Get<xenos::reg::RB_SURFACE_INFO>();

  RenderPassKey render_pass_key;
  // Needed on the interlock path too, to pass the sample count to the pipeline cache.
  render_pass_key.msaa_samples = rb_surface_info.msaa_samples;

  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      RenderTarget* const* depth_and_color_render_targets = GetLastUpdateAccumulatedRenderTargets();

      PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                       depth_and_color_render_targets, GetLastUpdateTransfers());

      if (depth_and_color_render_targets[0]) {
        render_pass_key.depth_and_color_used |= 1 << 0;
        render_pass_key.depth_format = depth_and_color_render_targets[0]->GetKey().GetDepthFormat();
      }
      if (depth_and_color_render_targets[1]) {
        render_pass_key.depth_and_color_used |= 1 << 1;
        render_pass_key.color_0_view_format =
            depth_and_color_render_targets[1]->GetKey().GetColorFormat();
      }
      if (depth_and_color_render_targets[2]) {
        render_pass_key.depth_and_color_used |= 1 << 2;
        render_pass_key.color_1_view_format =
            depth_and_color_render_targets[2]->GetKey().GetColorFormat();
      }
      if (depth_and_color_render_targets[3]) {
        render_pass_key.depth_and_color_used |= 1 << 3;
        render_pass_key.color_2_view_format =
            depth_and_color_render_targets[3]->GetKey().GetColorFormat();
      }
      if (depth_and_color_render_targets[4]) {
        render_pass_key.depth_and_color_used |= 1 << 4;
        render_pass_key.color_3_view_format =
            depth_and_color_render_targets[4]->GetKey().GetColorFormat();
      }

      const Framebuffer* framebuffer = last_update_framebuffer_;
      VkRenderPass render_pass = last_update_render_pass_key_ == render_pass_key
                                     ? last_update_render_pass_
                                     : VK_NULL_HANDLE;
      if (render_pass == VK_NULL_HANDLE) {
        render_pass = GetHostRenderTargetsRenderPass(render_pass_key);
        if (render_pass == VK_NULL_HANDLE) {
          return false;
        }
        framebuffer = nullptr;
      }

      u32 pitch_tiles_at_32bpp = ((rb_surface_info.surface_pitch << static_cast<u32>(
                                       rb_surface_info.msaa_samples >= xenos::MSAASamples::k4X)) +
                                  (xenos::kEDRAMTileWidthSamples - 1)) /
                                 xenos::kEDRAMTileWidthSamples;
      if (framebuffer) {
        if (last_update_framebuffer_pitch_tiles_at_32bpp_ != pitch_tiles_at_32bpp ||
            !std::equal(std::begin(last_update_framebuffer_attachments_),
                        std::end(last_update_framebuffer_attachments_),
                        depth_and_color_render_targets)) {
          framebuffer = nullptr;
        }
      }
      if (!framebuffer) {
        framebuffer = GetHostRenderTargetsFramebuffer(render_pass_key, pitch_tiles_at_32bpp,
                                                      depth_and_color_render_targets);
        if (!framebuffer) {
          return false;
        }
      }

      last_update_render_pass_key_ = render_pass_key;
      last_update_render_pass_ = render_pass;
      last_update_framebuffer_pitch_tiles_at_32bpp_ = pitch_tiles_at_32bpp;
      std::copy_n(depth_and_color_render_targets, std::size(last_update_framebuffer_attachments_),
                  std::begin(last_update_framebuffer_attachments_));
      last_update_framebuffer_ = framebuffer;

      for (u32 i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        RenderTarget* rt = depth_and_color_render_targets[i];
        if (!rt) {
          continue;
        }
        auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rt);
        VkPipelineStageFlags rt_dst_stage_mask;
        VkAccessFlags rt_dst_access_mask;
        VkImageLayout rt_new_layout;
        VulkanRenderTarget::GetDrawUsage(i == 0, &rt_dst_stage_mask, &rt_dst_access_mask,
                                         &rt_new_layout);
        command_processor_.PushImageMemoryBarrier(
            vulkan_rt.GetImage(),
            gpu::vulkan::objects::BuildSubresourceRange(
                i ? VK_IMAGE_ASPECT_COLOR_BIT
                  : (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)),
            vulkan_rt.GetCurrentStageMask(), rt_dst_stage_mask, vulkan_rt.GetCurrentAccessMask(),
            rt_dst_access_mask, vulkan_rt.GetCurrentLayout(), rt_new_layout);
        vulkan_rt.SetUsage(rt_dst_stage_mask, rt_dst_access_mask, rt_new_layout);
      }
    } break;

    case Path::kPixelShaderInterlock: {
      UseEDRAMBuffer(EDRAMBufferUsage::kFragmentReadWrite);
      // Unordered writes like clears are not synchronized with FSI accesses, so commit them.
      CommitEDRAMBufferShaderWrites(EDRAMBufferModificationStatus::kViaUnordered);
      MarkEDRAMBufferModified(EDRAMBufferModificationStatus::kViaFragmentShaderInterlock);
      last_update_render_pass_key_ = render_pass_key;
      last_update_render_pass_ = fsi_render_pass_;
      last_update_framebuffer_ = &fsi_framebuffer_;
    } break;

    default:
      REX_UNHANDLED_CASE(GetPath());
      return false;
  }

  return true;
}

void VulkanRenderTargetCache::GetLastUpdateRenderingAttachments(
    VkRenderingAttachmentInfo* color_attachments, u32* color_attachment_count_out,
    VkRenderingAttachmentInfo* depth_attachment,
    VkRenderingAttachmentInfo* stencil_attachment) const {
  RenderPassKey key = last_update_render_pass_key_;
  RenderTarget* const* rts = GetLastUpdateAccumulatedRenderTargets();

  *depth_attachment = {};
  *stencil_attachment = {};

  if ((key.depth_and_color_used & 0b1) && rts[0]) {
    const auto* vulkan_rt = static_cast<const VulkanRenderTarget*>(rts[0]);
    depth_attachment->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment->pNext = nullptr;
    depth_attachment->imageView = vulkan_rt->GetViewDepthStencil();
    depth_attachment->imageLayout = VulkanRenderTarget::kDepthDrawLayout;
    depth_attachment->resolveMode = VK_RESOLVE_MODE_NONE;
    depth_attachment->resolveImageView = VK_NULL_HANDLE;
    depth_attachment->resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment->clearValue = {};
    *stencil_attachment = *depth_attachment;
  }

  u32 color_attachment_count = 0;
  for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    VkRenderingAttachmentInfo& color_attachment = color_attachments[i];
    color_attachment = {};
    if ((key.depth_and_color_used & (1 << (1 + i))) && rts[1 + i]) {
      const auto* vulkan_rt = static_cast<const VulkanRenderTarget*>(rts[1 + i]);
      color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_attachment.pNext = nullptr;
      color_attachment.imageView = vulkan_rt->GetViewDepthColor();
      color_attachment.imageLayout = VulkanRenderTarget::kColorDrawLayout;
      color_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      color_attachment.resolveImageView = VK_NULL_HANDLE;
      color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_attachment.clearValue = {};
      color_attachment_count = i + 1;
    }
  }
  *color_attachment_count_out = color_attachment_count;
}

VkRenderPass VulkanRenderTargetCache::GetHostRenderTargetsRenderPass(RenderPassKey key) {
  REX_ASSERT(GetPath() == Path::kHostRenderTargets);

  auto it = render_passes_.find(key);
  if (it != render_passes_.end()) {
    return it->second;
  }

  VkSampleCountFlagBits samples;
  switch (key.msaa_samples) {
    case xenos::MSAASamples::k1X:
      samples = VK_SAMPLE_COUNT_1_BIT;
      break;
    case xenos::MSAASamples::k2X:
      samples = IsMSAA2xSupported(key.depth_and_color_used != 0) ? VK_SAMPLE_COUNT_2_BIT
                                                                 : VK_SAMPLE_COUNT_4_BIT;
      break;
    case xenos::MSAASamples::k4X:
      samples = VK_SAMPLE_COUNT_4_BIT;
      break;
    default:
      return VK_NULL_HANDLE;
  }

  VkAttachmentDescription attachments[1 + xenos::kMaxColorRenderTargets];
  if (key.depth_and_color_used & 0b1) {
    VkAttachmentDescription& attachment = attachments[0];
    attachment.flags = 0;
    attachment.format = GetDepthVulkanFormat(key.depth_format);
    attachment.samples = samples;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VulkanRenderTarget::kDepthDrawLayout;
    attachment.finalLayout = VulkanRenderTarget::kDepthDrawLayout;
  }
  VkAttachmentReference color_attachments[xenos::kMaxColorRenderTargets];
  xenos::ColorRenderTargetFormat color_formats[] = {
      key.color_0_view_format,
      key.color_1_view_format,
      key.color_2_view_format,
      key.color_3_view_format,
  };
  for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    VkAttachmentReference& color_attachment = color_attachments[i];
    color_attachment.layout = VulkanRenderTarget::kColorDrawLayout;
    u32 attachment_bit = static_cast<u32>(1) << (1 + i);
    if (!(key.depth_and_color_used & attachment_bit)) {
      color_attachment.attachment = VK_ATTACHMENT_UNUSED;
      continue;
    }
    u32 attachment_index = rex::BitCount(key.depth_and_color_used & (attachment_bit - 1));
    color_attachment.attachment = attachment_index;
    VkAttachmentDescription& attachment = attachments[attachment_index];
    attachment.flags = 0;
    xenos::ColorRenderTargetFormat color_format = color_formats[i];
    attachment.format = key.color_rts_use_transfer_formats
                            ? GetColorOwnershipTransferVulkanFormat(color_format, key.msaa_samples)
                            : GetColorVulkanFormat(color_format);
    attachment.samples = samples;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VulkanRenderTarget::kColorDrawLayout;
    attachment.finalLayout = VulkanRenderTarget::kColorDrawLayout;
  }

  VkAttachmentReference depth_stencil_attachment;
  depth_stencil_attachment.attachment = (key.depth_and_color_used & 0b1) ? 0 : VK_ATTACHMENT_UNUSED;
  depth_stencil_attachment.layout = VulkanRenderTarget::kDepthDrawLayout;

  VkSubpassDescription subpass;
  subpass.flags = 0;
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.inputAttachmentCount = 0;
  subpass.pInputAttachments = nullptr;
  subpass.colorAttachmentCount = 32 - rex::lzcnt(static_cast<u32>(key.depth_and_color_used >> 1));
  subpass.pColorAttachments = color_attachments;
  subpass.pResolveAttachments = nullptr;
  subpass.pDepthStencilAttachment =
      (key.depth_and_color_used & 0b1) ? &depth_stencil_attachment : nullptr;
  subpass.preserveAttachmentCount = 0;
  subpass.pPreserveAttachments = nullptr;

  VkPipelineStageFlags dependency_stage_mask = 0;
  VkAccessFlags dependency_access_mask = 0;
  if (key.depth_and_color_used & 0b1) {
    dependency_stage_mask |= VulkanRenderTarget::kDepthDrawStageMask;
    dependency_access_mask |= VulkanRenderTarget::kDepthDrawAccessMask;
  }
  if (key.depth_and_color_used >> 1) {
    dependency_stage_mask |= VulkanRenderTarget::kColorDrawStageMask;
    dependency_access_mask |= VulkanRenderTarget::kColorDrawAccessMask;
  }
  VkSubpassDependency subpass_dependencies[2];
  subpass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  subpass_dependencies[0].dstSubpass = 0;
  subpass_dependencies[0].srcStageMask = dependency_stage_mask;
  subpass_dependencies[0].dstStageMask = dependency_stage_mask;
  subpass_dependencies[0].srcAccessMask = dependency_access_mask;
  subpass_dependencies[0].dstAccessMask = dependency_access_mask;
  subpass_dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
  subpass_dependencies[1].srcSubpass = 0;
  subpass_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  subpass_dependencies[1].srcStageMask = dependency_stage_mask;
  subpass_dependencies[1].dstStageMask = dependency_stage_mask;
  subpass_dependencies[1].srcAccessMask = dependency_access_mask;
  subpass_dependencies[1].dstAccessMask = dependency_access_mask;
  subpass_dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  VkRenderPassCreateInfo render_pass_create_info;
  render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_create_info.pNext = nullptr;
  render_pass_create_info.flags = 0;
  render_pass_create_info.attachmentCount = rex::BitCount(key.depth_and_color_used);
  render_pass_create_info.pAttachments = attachments;
  render_pass_create_info.subpassCount = 1;
  render_pass_create_info.pSubpasses = &subpass;
  render_pass_create_info.dependencyCount =
      key.depth_and_color_used ? static_cast<u32>(rex::CountOf(subpass_dependencies)) : 0;
  render_pass_create_info.pDependencies = subpass_dependencies;

  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  VkRenderPass render_pass;
  if (dfn.vkCreateRenderPass(device, &render_pass_create_info, nullptr, &render_pass) !=
      VK_SUCCESS) {
    REXLOG_ERROR(gpu, "VulkanRenderTargetCache: Failed to create a render pass");
    render_passes_.emplace(key, VK_NULL_HANDLE);
    return VK_NULL_HANDLE;
  }
  render_passes_.emplace(key, render_pass);
  return render_pass;
}

VkFormat VulkanRenderTargetCache::GetDepthVulkanFormat(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24S8 && IsDepthUnorm24VulkanFormatSupported()) {
    return VK_FORMAT_D24_UNORM_S8_UINT;
  }
  return VK_FORMAT_D32_SFLOAT_S8_UINT;
}

bool VulkanRenderTargetCache::IsColor16FormatFloatLike(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      return color_rg16_draw_format_fallback_to_float_;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return color_rgba16_draw_format_fallback_to_float_;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return true;
    default:
      return false;
  }
}

VkFormat VulkanRenderTargetCache::GetColorVulkanFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return gamma_render_target_as_unorm16_ ? VK_FORMAT_R16G16B16A16_UNORM
                                             : VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return color_rg16_draw_format_fallback_to_float_ ? VK_FORMAT_R16G16_SFLOAT
                                                       : VK_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return color_rgba16_draw_format_fallback_to_float_ ? VK_FORMAT_R16G16B16A16_SFLOAT
                                                         : VK_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      REX_UNHANDLED_CASE(format);
      return VK_FORMAT_UNDEFINED;
  }
}

VkFormat VulkanRenderTargetCache::GetColorOwnershipTransferVulkanFormat(
    xenos::ColorRenderTargetFormat format, xenos::MSAASamples msaa_samples,
    bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  VkSampleCountFlagBits transfer_sample_count =
      static_cast<VkSampleCountFlagBits>(static_cast<u32>(1) << static_cast<u32>(msaa_samples));
  if (transfer_sample_count == VK_SAMPLE_COUNT_2_BIT && !msaa_2x_attachments_supported_) {
    // 2x guest color render targets are allocated as 4x host render targets in this mode.
    transfer_sample_count = VK_SAMPLE_COUNT_4_BIT;
  }
  const gpu::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->GetProperties();
  bool integer_transfer_sample_count_supported =
      (device_properties.framebufferColorSampleCounts & transfer_sample_count) != 0 &&
      (device_properties.sampledImageIntegerSampleCounts & transfer_sample_count) != 0;
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      if (color_16bit_transfer_uint_formats_supported_ && integer_transfer_sample_count_supported) {
        return VK_FORMAT_R16G16_UINT;
      }
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      if (color_16bit_transfer_uint_formats_supported_ && integer_transfer_sample_count_supported) {
        return VK_FORMAT_R16G16B16A16_UINT;
      }
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      if (color_32bit_transfer_uint_formats_supported_ && integer_transfer_sample_count_supported) {
        return VK_FORMAT_R32_UINT;
      }
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      if (color_32bit_transfer_uint_formats_supported_ && integer_transfer_sample_count_supported) {
        return VK_FORMAT_R32G32_UINT;
      }
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
  }
}

VulkanRenderTargetCache::VulkanRenderTarget::~VulkanRenderTarget() {
  const gpu::vulkan::VulkanDevice* const vulkan_device =
      render_target_cache_.command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  gpu::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      GetKey().is_depth ? *render_target_cache_.descriptor_set_pool_sampled_image_x2_
                        : *render_target_cache_.descriptor_set_pool_sampled_image_;
  descriptor_set_pool.Free(descriptor_set_index_transfer_source_);
  if (view_color_transfer_separate_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_color_transfer_separate_, nullptr);
  }
  if (view_srgb_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_srgb_, nullptr);
  }
  if (view_stencil_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_stencil_, nullptr);
  }
  if (view_depth_stencil_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_depth_stencil_, nullptr);
  }
  dfn.vkDestroyImageView(device, view_depth_color_, nullptr);
  dfn.vkDestroyImage(device, image_, nullptr);
  dfn.vkFreeMemory(device, memory_, nullptr);
}

u32 VulkanRenderTargetCache::GetMaxRenderTargetWidth() const {
  const gpu::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->GetProperties();
  return std::min(device_properties.maxFramebufferWidth, device_properties.maxImageDimension2D);
}

u32 VulkanRenderTargetCache::GetMaxRenderTargetHeight() const {
  const gpu::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->GetProperties();
  return std::min(device_properties.maxFramebufferHeight, device_properties.maxImageDimension2D);
}

std::unique_ptr<RenderTargetCache::RenderTarget> VulkanRenderTargetCache::CreateRenderTarget(
    RenderTargetKey key) {
  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.extent.width = key.GetWidth() * GetDrawResolutionScaleX();
  image_create_info.extent.height =
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples) * GetDrawResolutionScaleY();
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  if (key.msaa_samples == xenos::MSAASamples::k2X && !msaa_2x_attachments_supported_) {
    image_create_info.samples = VK_SAMPLE_COUNT_4_BIT;
  } else {
    image_create_info.samples = static_cast<VkSampleCountFlagBits>(
        static_cast<u32>(1) << static_cast<u32>(key.msaa_samples));
  }
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkFormat transfer_format;
  bool is_srgb_view_needed = false;
  if (key.is_depth) {
    image_create_info.format = GetDepthVulkanFormat(key.GetDepthFormat());
    transfer_format = image_create_info.format;
    image_create_info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  } else {
    xenos::ColorRenderTargetFormat color_format = key.GetColorFormat();
    image_create_info.format = GetColorVulkanFormat(color_format);
    transfer_format = GetColorOwnershipTransferVulkanFormat(color_format, key.msaa_samples);
    is_srgb_view_needed = false;
    if (image_create_info.format != transfer_format || is_srgb_view_needed) {
      image_create_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }
    image_create_info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  if (image_create_info.format == VK_FORMAT_UNDEFINED) {
    REX_FAIL(gpu, nullptr, "VulkanRenderTargetCache: Unknown {} render target format {}",
             key.is_depth ? "depth" : "color", static_cast<u32>(key.resource_format));
  }
  VkImage image;
  VkDeviceMemory memory;
  if (!gpu::vulkan::objects::CreateDedicatedAllocationImage(
          vulkan_device, image_create_info, gpu::vulkan::objects::MemoryPurpose::kDeviceLocal,
          image, memory)) {
    REX_FAIL(gpu, nullptr,
             "VulkanRenderTarget: Failed to create a {}x{} {}xMSAA {} render target "
             "image",
             image_create_info.extent.width, image_create_info.extent.height,
             static_cast<u32>(1) << static_cast<u32>(key.msaa_samples), key.GetFormatName());
  }

  VkImageViewCreateInfo view_create_info;
  view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_create_info.pNext = nullptr;
  view_create_info.flags = 0;
  view_create_info.image = image;
  view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_create_info.format = image_create_info.format;
  view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.subresourceRange = gpu::vulkan::objects::BuildSubresourceRange(
      key.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageView view_depth_color;
  if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_depth_color) != VK_SUCCESS) {
    REXLOG_ERROR(gpu,
                 "VulkanRenderTarget: Failed to create a {} view for a {}x{} {}xMSAA {} "
                 "render target",
                 key.is_depth ? "depth" : "color", image_create_info.extent.width,
                 image_create_info.extent.height,
                 static_cast<u32>(1) << static_cast<u32>(key.msaa_samples), key.GetFormatName());
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return nullptr;
  }
  VkImageView view_depth_stencil = VK_NULL_HANDLE;
  VkImageView view_stencil = VK_NULL_HANDLE;
  VkImageView view_srgb = VK_NULL_HANDLE;
  VkImageView view_color_transfer_separate = VK_NULL_HANDLE;
  if (key.is_depth) {
    view_create_info.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_depth_stencil) !=
        VK_SUCCESS) {
      REXLOG_ERROR(gpu,
                   "VulkanRenderTarget: Failed to create a depth/stencil view for a "
                   "{}x{} {}xMSAA {} render target",
                   image_create_info.extent.width, image_create_info.extent.height,
                   static_cast<u32>(1) << static_cast<u32>(key.msaa_samples),
                   xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat()));
      dfn.vkDestroyImageView(device, view_depth_color, nullptr);
      dfn.vkDestroyImage(device, image, nullptr);
      dfn.vkFreeMemory(device, memory, nullptr);
      return nullptr;
    }
    view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_stencil) != VK_SUCCESS) {
      REXLOG_ERROR(gpu,
                   "VulkanRenderTarget: Failed to create a stencil view for a {}x{} "
                   "{}xMSAA render target",
                   image_create_info.extent.width, image_create_info.extent.height,
                   static_cast<u32>(1) << static_cast<u32>(key.msaa_samples),
                   xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat()));
      dfn.vkDestroyImageView(device, view_depth_stencil, nullptr);
      dfn.vkDestroyImageView(device, view_depth_color, nullptr);
      dfn.vkDestroyImage(device, image, nullptr);
      dfn.vkFreeMemory(device, memory, nullptr);
      return nullptr;
    }
  } else {
    if (is_srgb_view_needed) {
      view_create_info.format = VK_FORMAT_R8G8B8A8_SRGB;
      if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_srgb) != VK_SUCCESS) {
        REXLOG_ERROR(gpu,
                     "VulkanRenderTarget: Failed to create an sRGB view for a {}x{} "
                     "{}xMSAA render target",
                     image_create_info.extent.width, image_create_info.extent.height,
                     static_cast<u32>(1) << static_cast<u32>(key.msaa_samples),
                     xenos::GetColorRenderTargetFormatName(key.GetColorFormat()));
        dfn.vkDestroyImageView(device, view_depth_color, nullptr);
        dfn.vkDestroyImage(device, image, nullptr);
        dfn.vkFreeMemory(device, memory, nullptr);
        return nullptr;
      }
    }
    if (transfer_format != image_create_info.format) {
      view_create_info.format = transfer_format;
      if (dfn.vkCreateImageView(device, &view_create_info, nullptr,
                                &view_color_transfer_separate) != VK_SUCCESS) {
        REXLOG_ERROR(gpu,
                     "VulkanRenderTarget: Failed to create a transfer view for a {}x{} "
                     "{}xMSAA {} render target",
                     image_create_info.extent.width, image_create_info.extent.height,
                     static_cast<u32>(1) << static_cast<u32>(key.msaa_samples),
                     key.GetFormatName());
        if (view_srgb != VK_NULL_HANDLE) {
          dfn.vkDestroyImageView(device, view_srgb, nullptr);
        }
        dfn.vkDestroyImageView(device, view_depth_color, nullptr);
        dfn.vkDestroyImage(device, image, nullptr);
        dfn.vkFreeMemory(device, memory, nullptr);
        return nullptr;
      }
    }
  }

  gpu::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      key.is_depth ? *descriptor_set_pool_sampled_image_x2_ : *descriptor_set_pool_sampled_image_;
  size_t descriptor_set_index_transfer_source = descriptor_set_pool.Allocate();
  if (descriptor_set_index_transfer_source == SIZE_MAX) {
    REXLOG_ERROR(gpu,
                 "VulkanRenderTargetCache: Failed to allocate sampled image descriptors "
                 "for a {} render target",
                 key.is_depth ? "depth/stencil" : "color");
    if (view_color_transfer_separate != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, view_color_transfer_separate, nullptr);
    }
    if (view_srgb != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, view_srgb, nullptr);
    }
    dfn.vkDestroyImageView(device, view_depth_color, nullptr);
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return nullptr;
  }
  VkDescriptorSet descriptor_set_transfer_source =
      descriptor_set_pool.GetDescriptorSet(descriptor_set_index_transfer_source);
  VkWriteDescriptorSet descriptor_set_write[2];
  VkDescriptorImageInfo descriptor_set_write_depth_color;
  descriptor_set_write_depth_color.sampler = VK_NULL_HANDLE;
  descriptor_set_write_depth_color.imageView = view_color_transfer_separate != VK_NULL_HANDLE
                                                   ? view_color_transfer_separate
                                                   : view_depth_color;
  descriptor_set_write_depth_color.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  descriptor_set_write[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptor_set_write[0].pNext = nullptr;
  descriptor_set_write[0].dstSet = descriptor_set_transfer_source;
  descriptor_set_write[0].dstBinding = 0;
  descriptor_set_write[0].dstArrayElement = 0;
  descriptor_set_write[0].descriptorCount = 1;
  descriptor_set_write[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_write[0].pImageInfo = &descriptor_set_write_depth_color;
  descriptor_set_write[0].pBufferInfo = nullptr;
  descriptor_set_write[0].pTexelBufferView = nullptr;
  VkDescriptorImageInfo descriptor_set_write_stencil;
  if (key.is_depth) {
    descriptor_set_write_stencil.sampler = VK_NULL_HANDLE;
    descriptor_set_write_stencil.imageView = view_stencil;
    descriptor_set_write_stencil.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_set_write[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write[1].pNext = nullptr;
    descriptor_set_write[1].dstSet = descriptor_set_transfer_source;
    descriptor_set_write[1].dstBinding = 1;
    descriptor_set_write[1].dstArrayElement = 0;
    descriptor_set_write[1].descriptorCount = 1;
    descriptor_set_write[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_write[1].pImageInfo = &descriptor_set_write_stencil;
    descriptor_set_write[1].pBufferInfo = nullptr;
    descriptor_set_write[1].pTexelBufferView = nullptr;
  }
  dfn.vkUpdateDescriptorSets(device, key.is_depth ? 2 : 1, descriptor_set_write, 0, nullptr);

  return std::make_unique<VulkanRenderTarget>(
      key, *this, image, memory, view_depth_color, view_depth_stencil, view_stencil, view_srgb,
      view_color_transfer_separate, descriptor_set_index_transfer_source);
}

bool VulkanRenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return !IsDepthUnorm24VulkanFormatSupported();
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return !depth_float24_convert_in_pixel_shader_;
  }
  return false;
}

bool VulkanRenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

void VulkanRenderTargetCache::RequestPixelShaderInterlockBarrier() {
  // Commit all pending EDRAM shader writes, not only FSI writes, like D3D12 ROV.
  CommitEDRAMBufferShaderWrites();
}

void VulkanRenderTargetCache::GetEDRAMBufferUsageMasks(EDRAMBufferUsage usage,
                                                       VkPipelineStageFlags& stage_mask_out,
                                                       VkAccessFlags& access_mask_out) {
  switch (usage) {
    case EDRAMBufferUsage::kFragmentRead:
      stage_mask_out = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT;
      break;
    case EDRAMBufferUsage::kFragmentReadWrite:
      stage_mask_out = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      break;
    case EDRAMBufferUsage::kComputeRead:
      stage_mask_out = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT;
      break;
    case EDRAMBufferUsage::kComputeWrite:
      stage_mask_out = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_WRITE_BIT;
      break;
    case EDRAMBufferUsage::kTransferRead:
      stage_mask_out = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask_out = VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case EDRAMBufferUsage::kTransferWrite:
      stage_mask_out = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask_out = VK_ACCESS_TRANSFER_WRITE_BIT;
      break;
    default:
      REX_UNHANDLED_CASE(usage);
  }
}

void VulkanRenderTargetCache::UseEDRAMBuffer(EDRAMBufferUsage new_usage) {
  if (edram_buffer_usage_ == new_usage) {
    return;
  }
  VkPipelineStageFlags src_stage_mask, dst_stage_mask;
  VkAccessFlags src_access_mask, dst_access_mask;
  GetEDRAMBufferUsageMasks(edram_buffer_usage_, src_stage_mask, src_access_mask);
  GetEDRAMBufferUsageMasks(new_usage, dst_stage_mask, dst_access_mask);
  if (command_processor_.PushBufferMemoryBarrier(edram_buffer_, 0, VK_WHOLE_SIZE, src_stage_mask,
                                                 dst_stage_mask, src_access_mask,
                                                 dst_access_mask)) {
    edram_buffer_modification_status_ = EDRAMBufferModificationStatus::kUnmodified;
  }
  edram_buffer_usage_ = new_usage;
}

void VulkanRenderTargetCache::MarkEDRAMBufferModified(
    EDRAMBufferModificationStatus modification_status) {
  REX_ASSERT(modification_status != EDRAMBufferModificationStatus::kUnmodified);
  switch (edram_buffer_usage_) {
    case EDRAMBufferUsage::kFragmentReadWrite:
      // Unordered modification needs stricter synchronization than interlock, hence max.
      edram_buffer_modification_status_ =
          std::max(edram_buffer_modification_status_, modification_status);
      break;
    case EDRAMBufferUsage::kComputeWrite:
      REX_ASSERT(modification_status == EDRAMBufferModificationStatus::kViaUnordered);
      modification_status = EDRAMBufferModificationStatus::kViaUnordered;
      edram_buffer_modification_status_ =
          std::max(edram_buffer_modification_status_, modification_status);
      break;
    default:
      REX_ASSERT_ALWAYS(
          "While changing the usage of the EDRAM buffer before marking it as "
          "modified is handled safely (but will cause spurious marking as "
          "modified after the changes have been implicitly committed by the "
          "usage switch), normally that shouldn't be done and is an "
          "indication of architectural mistakes. Alternatively, this may "
          "indicate that the usage switch has been forgotten before writing, "
          "which is a clearly invalid situation.");
  }
}

void VulkanRenderTargetCache::CommitEDRAMBufferShaderWrites(
    EDRAMBufferModificationStatus commit_status) {
  REX_ASSERT(commit_status != EDRAMBufferModificationStatus::kUnmodified);
  if (edram_buffer_modification_status_ < commit_status) {
    return;
  }
  VkPipelineStageFlags stage_mask;
  VkAccessFlags access_mask;
  GetEDRAMBufferUsageMasks(edram_buffer_usage_, stage_mask, access_mask);
  if (!(access_mask & VK_ACCESS_SHADER_WRITE_BIT)) {
    REX_ASSERT_ALWAYS("EDRAM writes committed from a usage without shader writes");
    edram_buffer_modification_status_ = EDRAMBufferModificationStatus::kUnmodified;
    PixelShaderInterlockFullEDRAMBarrierPlaced();
    return;
  }
  command_processor_.PushBufferMemoryBarrier(
      edram_buffer_, 0, VK_WHOLE_SIZE, stage_mask, stage_mask, access_mask, access_mask,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  edram_buffer_modification_status_ = EDRAMBufferModificationStatus::kUnmodified;
  PixelShaderInterlockFullEDRAMBarrierPlaced();
}

const VulkanRenderTargetCache::Framebuffer*
VulkanRenderTargetCache::GetHostRenderTargetsFramebuffer(
    RenderPassKey render_pass_key, u32 pitch_tiles_at_32bpp,
    const RenderTarget* const* depth_and_color_render_targets) {
  FramebufferKey key;
  key.render_pass_key = render_pass_key;
  key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
  if (render_pass_key.depth_and_color_used & (1 << 0)) {
    key.depth_base_tiles = depth_and_color_render_targets[0]->GetKey().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 1)) {
    key.color_0_base_tiles = depth_and_color_render_targets[1]->GetKey().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 2)) {
    key.color_1_base_tiles = depth_and_color_render_targets[2]->GetKey().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 3)) {
    key.color_2_base_tiles = depth_and_color_render_targets[3]->GetKey().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 4)) {
    key.color_3_base_tiles = depth_and_color_render_targets[4]->GetKey().base_tiles;
  }
  auto it = framebuffers_.find(key);
  if (it != framebuffers_.end()) {
    return &it->second;
  }

  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  const gpu::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->GetProperties();

  VkRenderPass render_pass = GetHostRenderTargetsRenderPass(render_pass_key);
  if (render_pass == VK_NULL_HANDLE) {
    return nullptr;
  }

  VkImageView attachments[1 + xenos::kMaxColorRenderTargets];
  u32 attachment_count = 0;
  u32 depth_and_color_rts_remaining = render_pass_key.depth_and_color_used;
  u32 rt_index;
  while (rex::bit_scan_forward(depth_and_color_rts_remaining, &rt_index)) {
    depth_and_color_rts_remaining &= ~(static_cast<u32>(1) << rt_index);
    const auto& vulkan_rt =
        *static_cast<const VulkanRenderTarget*>(depth_and_color_render_targets[rt_index]);
    VkImageView attachment;
    if (rt_index) {
      attachment = render_pass_key.color_rts_use_transfer_formats ? vulkan_rt.GetViewColorTransfer()
                                                                  : vulkan_rt.GetViewDepthColor();
    } else {
      attachment = vulkan_rt.GetViewDepthStencil();
    }
    attachments[attachment_count++] = attachment;
  }

  VkFramebufferCreateInfo framebuffer_create_info;
  framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_create_info.pNext = nullptr;
  framebuffer_create_info.flags = 0;
  framebuffer_create_info.renderPass = render_pass;
  framebuffer_create_info.attachmentCount = attachment_count;
  framebuffer_create_info.pAttachments = attachments;
  VkExtent2D host_extent;
  if (pitch_tiles_at_32bpp) {
    host_extent.width =
        RenderTargetKey::GetWidth(pitch_tiles_at_32bpp, render_pass_key.msaa_samples);
    host_extent.height = GetRenderTargetHeight(pitch_tiles_at_32bpp, render_pass_key.msaa_samples);
  } else {
    REX_ASSERT(render_pass_key.depth_and_color_used == 0);
    // Needed for occlusion queries.
    host_extent.width = xenos::kTexture2DCubeMaxWidthHeight;
    host_extent.height = xenos::kTexture2DCubeMaxWidthHeight;
  }
  // Clamp to the device limit, as no attachments impose one when there are none.
  host_extent.width = std::min(host_extent.width * GetDrawResolutionScaleX(),
                               device_properties.maxFramebufferWidth);
  host_extent.height = std::min(host_extent.height * GetDrawResolutionScaleY(),
                                device_properties.maxFramebufferHeight);
  framebuffer_create_info.width = host_extent.width;
  framebuffer_create_info.height = host_extent.height;
  framebuffer_create_info.layers = 1;
  VkFramebuffer framebuffer;
  if (dfn.vkCreateFramebuffer(device, &framebuffer_create_info, nullptr, &framebuffer) !=
      VK_SUCCESS) {
    return nullptr;
  }
  // unordered_map node addresses are stable, so returned pointers stay valid.
  return &framebuffers_
              .emplace(std::piecewise_construct, std::forward_as_tuple(key),
                       std::forward_as_tuple(framebuffer, host_extent))
              .first->second;
}

VkShaderModule VulkanRenderTargetCache::GetTransferShader(TransferShaderKey key) {
  auto shader_it = transfer_shaders_.find(key);
  if (shader_it != transfer_shaders_.end()) {
    return shader_it->second;
  }

  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->GetProperties();

  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;

  SPIRVBuilder builder(spv::Spv_1_0, (SPIRVShaderTranslator::kSPIRVMagicToolID << 16) | 1, nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint4 = builder.makeVectorType(type_uint, 4);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float2 = builder.makeVectorType(type_float, 2);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);

  const TransferModeInfo& mode = kTransferModes[static_cast<size_t>(key.mode)];
  const TransferPipelineLayoutInfo& pipeline_layout_info =
      kTransferPipelineLayoutInfos[static_cast<size_t>(mode.pipeline_layout)];

  bool dest_is_color = (mode.output == TransferOutput::kColor);
  xenos::ColorRenderTargetFormat dest_color_format =
      static_cast<xenos::ColorRenderTargetFormat>(key.dest_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      static_cast<xenos::DepthRenderTargetFormat>(key.dest_resource_format);
  bool dest_is_64bpp = dest_is_color && xenos::IsColorRenderTargetFormat64bpp(dest_color_format);

  xenos::ColorRenderTargetFormat source_color_format =
      static_cast<xenos::ColorRenderTargetFormat>(key.source_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      static_cast<xenos::DepthRenderTargetFormat>(key.source_resource_format);
  bool source_is_color =
      (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetColorTextureBit) != 0;
  bool source_is_64bpp;
  u32 source_color_format_component_count;
  u32 source_color_texture_component_mask;
  bool source_color_is_uint;
  spv::Id source_color_component_type;
  if (source_is_color) {
    REX_ASSERT((pipeline_layout_info.used_descriptor_sets &
                kTransferUsedDescriptorSetDepthStencilTexturesBit) == 0);
    source_is_64bpp = xenos::IsColorRenderTargetFormat64bpp(source_color_format);
    source_color_format_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(source_color_format);
    if (mode.output == TransferOutput::kStencilBit) {
      if (source_is_64bpp && !dest_is_64bpp) {
        // One component, chosen from the two 32bpp halves of the 64bpp sample.
        source_color_texture_component_mask =
            0b1 | (0b1 << (source_color_format_component_count >> 1));
      } else {
        // Red is at least 8 bits per component in all formats.
        source_color_texture_component_mask = 0b1;
      }
    } else {
      source_color_texture_component_mask =
          (static_cast<u32>(1) << source_color_format_component_count) - 1;
    }
    GetColorOwnershipTransferVulkanFormat(source_color_format, key.source_msaa_samples,
                                          &source_color_is_uint);
    source_color_component_type = source_color_is_uint ? type_uint : type_float;
  } else {
    source_is_64bpp = false;
    source_color_format_component_count = 0;
    source_color_texture_component_mask = 0;
    source_color_is_uint = false;
    source_color_component_type = spv::NoType;
  }

  std::vector<spv::Id> main_interface;

  bool shader_uses_stencil_reference_output =
      mode.output == TransferOutput::kDepth &&
      vulkan_device->GetExtensions().ext_EXT_shader_stencil_export;
  bool dest_color_is_uint = false;
  u32 dest_color_component_count = 0;
  spv::Id type_fragment_data_component = spv::NoResult;
  spv::Id type_fragment_data = spv::NoResult;
  spv::Id output_fragment_data = spv::NoResult;
  spv::Id output_fragment_depth = spv::NoResult;
  spv::Id output_fragment_stencil_ref = spv::NoResult;
  switch (mode.output) {
    case TransferOutput::kColor:
      GetColorOwnershipTransferVulkanFormat(dest_color_format, key.dest_msaa_samples,
                                            &dest_color_is_uint);
      dest_color_component_count =
          xenos::GetColorRenderTargetFormatComponentCount(dest_color_format);
      type_fragment_data_component = dest_color_is_uint ? type_uint : type_float;
      type_fragment_data =
          dest_color_component_count > 1
              ? builder.makeVectorType(type_fragment_data_component,
                                       static_cast<int>(dest_color_component_count))
              : type_fragment_data_component;
      output_fragment_data =
          builder.createVariable(spv::NoPrecision, spv::StorageClassOutput, type_fragment_data,
                                 "xe_transfer_fragment_data");
      builder.addDecoration(output_fragment_data, spv::DecorationLocation, key.dest_color_rt_index);
      main_interface.push_back(output_fragment_data);
      break;
    case TransferOutput::kDepth:
      output_fragment_depth = builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                     type_float, "gl_FragDepth");
      builder.addDecoration(output_fragment_depth, spv::DecorationBuiltIn, spv::BuiltInFragDepth);
      main_interface.push_back(output_fragment_depth);
      if (shader_uses_stencil_reference_output) {
        builder.addExtension("SPV_EXT_shader_stencil_export");
        builder.addCapability(spv::CapabilityStencilExportEXT);
        output_fragment_stencil_ref = builder.createVariable(
            spv::NoPrecision, spv::StorageClassOutput, type_int, "gl_FragStencilRefARB");
        builder.addDecoration(output_fragment_stencil_ref, spv::DecorationBuiltIn,
                              spv::BuiltInFragStencilRefEXT);
        main_interface.push_back(output_fragment_stencil_ref);
      }
      break;
    default:
      break;
  }

  // SPIR-V 1.0 needs no bindings in the entry point interface until SPIR-V 1.4.
  bool source_is_multisampled = key.source_msaa_samples != xenos::MSAASamples::k1X;
  spv::Id source_color_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetColorTextureBit) {
    source_color_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(source_color_component_type, spv::Dim2D, false, false,
                              source_is_multisampled, 1, spv::ImageFormatUnknown),
        "xe_transfer_color");
    builder.addDecoration(
        source_color_texture, spv::DecorationDescriptorSet,
        static_cast<int>(rex::BitCount(pipeline_layout_info.used_descriptor_sets &
                                       (kTransferUsedDescriptorSetColorTextureBit - 1))));
    builder.addDecoration(source_color_texture, spv::DecorationBinding, 0);
  }
  spv::Id source_depth_texture = spv::NoResult;
  spv::Id source_stencil_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetDepthStencilTexturesBit) {
    u32 source_depth_stencil_descriptor_set =
        rex::BitCount(pipeline_layout_info.used_descriptor_sets &
                      (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1));
    // depth is false in makeImageType, as other values cause driver issues.
    if (mode.output != TransferOutput::kStencilBit) {
      source_depth_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_float, spv::Dim2D, false, false, source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_depth");
      builder.addDecoration(source_depth_texture, spv::DecorationDescriptorSet,
                            static_cast<int>(source_depth_stencil_descriptor_set));
      builder.addDecoration(source_depth_texture, spv::DecorationBinding, 0);
    }
    if (mode.output != TransferOutput::kDepth || shader_uses_stencil_reference_output) {
      source_stencil_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_uint, spv::Dim2D, false, false, source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_stencil");
      builder.addDecoration(source_stencil_texture, spv::DecorationDescriptorSet,
                            static_cast<int>(source_depth_stencil_descriptor_set));
      builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
    }
  }
  spv::Id host_depth_source_buffer = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetHostDepthBufferBit) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeRuntimeArray(type_uint));
    // Storage buffers use std430 packing, so there is no vec4 padding.
    builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride, sizeof(u32));
    spv::Id type_host_depth_source_buffer =
        builder.makeStructType(id_vector_temp, "XeTransferHostDepthBuffer");
    builder.addMemberName(type_host_depth_source_buffer, 0, "host_depth");
    builder.addMemberDecoration(type_host_depth_source_buffer, 0, spv::DecorationNonWritable);
    builder.addMemberDecoration(type_host_depth_source_buffer, 0, spv::DecorationOffset, 0);
    // BufferBlock rather than Block, as SPIR-V 1.0 is generated.
    builder.addDecoration(type_host_depth_source_buffer, spv::DecorationBufferBlock);
    // Uniform rather than StorageBuffer, as SPIR-V 1.0 is generated.
    host_depth_source_buffer =
        builder.createVariable(spv::NoPrecision, spv::StorageClassUniform,
                               type_host_depth_source_buffer, "xe_transfer_host_depth_buffer");
    builder.addDecoration(
        host_depth_source_buffer, spv::DecorationDescriptorSet,
        static_cast<int>(rex::BitCount(pipeline_layout_info.used_descriptor_sets &
                                       (kTransferUsedDescriptorSetHostDepthBufferBit - 1))));
    builder.addDecoration(host_depth_source_buffer, spv::DecorationBinding, 0);
  }
  // The depth and stencil descriptor set is reused here, without stencil.
  spv::Id host_depth_source_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
    host_depth_source_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_float, spv::Dim2D, false, false,
                              key.host_depth_source_msaa_samples != xenos::MSAASamples::k1X, 1,
                              spv::ImageFormatUnknown),
        "xe_transfer_host_depth");
    builder.addDecoration(host_depth_source_texture, spv::DecorationDescriptorSet,
                          static_cast<int>(rex::BitCount(
                              pipeline_layout_info.used_descriptor_sets &
                              (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1))));
    builder.addDecoration(host_depth_source_texture, spv::DecorationBinding, 0);
  }
  id_vector_temp.clear();
  u32 push_constants_member_host_depth_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordHostDepthAddressBit) {
    push_constants_member_host_depth_address = static_cast<u32>(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  u32 push_constants_member_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords & kTransferUsedPushConstantDwordAddressBit) {
    push_constants_member_address = static_cast<u32>(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  u32 push_constants_member_stencil_mask = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordStencilMaskBit) {
    push_constants_member_stencil_mask = static_cast<u32>(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  spv::Id push_constants = spv::NoResult;
  if (!id_vector_temp.empty()) {
    spv::Id type_push_constants = builder.makeStructType(id_vector_temp, "XeTransferPushConstants");
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordHostDepthAddressBit) {
      REX_ASSERT(push_constants_member_host_depth_address != UINT32_MAX);
      builder.addMemberName(type_push_constants,
                            static_cast<int>(push_constants_member_host_depth_address),
                            "host_depth_address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_host_depth_address, spv::DecorationOffset,
          static_cast<int>(sizeof(u32) *
                           rex::BitCount(pipeline_layout_info.used_push_constant_dwords &
                                         (kTransferUsedPushConstantDwordHostDepthAddressBit - 1))));
    }
    if (pipeline_layout_info.used_push_constant_dwords & kTransferUsedPushConstantDwordAddressBit) {
      REX_ASSERT(push_constants_member_address != UINT32_MAX);
      builder.addMemberName(type_push_constants, static_cast<int>(push_constants_member_address),
                            "address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_address, spv::DecorationOffset,
          static_cast<int>(sizeof(u32) *
                           rex::BitCount(pipeline_layout_info.used_push_constant_dwords &
                                         (kTransferUsedPushConstantDwordAddressBit - 1))));
    }
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordStencilMaskBit) {
      REX_ASSERT(push_constants_member_stencil_mask != UINT32_MAX);
      builder.addMemberName(type_push_constants,
                            static_cast<int>(push_constants_member_stencil_mask), "stencil_mask");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_stencil_mask, spv::DecorationOffset,
          static_cast<int>(sizeof(u32) *
                           rex::BitCount(pipeline_layout_info.used_push_constant_dwords &
                                         (kTransferUsedPushConstantDwordStencilMaskBit - 1))));
    }
    builder.addDecoration(type_push_constants, spv::DecorationBlock);
    push_constants = builder.createVariable(spv::NoPrecision, spv::StorageClassPushConstant,
                                            type_push_constants, "xe_transfer_push_constants");
  }

  spv::Id input_fragment_coord =
      builder.createVariable(spv::NoPrecision, spv::StorageClassInput, type_float4, "gl_FragCoord");
  builder.addDecoration(input_fragment_coord, spv::DecorationBuiltIn, spv::BuiltInFragCoord);
  main_interface.push_back(input_fragment_coord);
  spv::Id input_sample_id = spv::NoResult;
  spv::Id spec_const_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MSAASamples::k1X) {
    if (device_properties.sampleRateShading) {
      // One draw for all samples.
      builder.addCapability(spv::CapabilitySampleRateShading);
      input_sample_id =
          builder.createVariable(spv::NoPrecision, spv::StorageClassInput, type_int, "gl_SampleID");
      builder.addDecoration(input_sample_id, spv::DecorationFlat);
      builder.addDecoration(input_sample_id, spv::DecorationBuiltIn, spv::BuiltInSampleId);
      main_interface.push_back(input_sample_id);
    } else {
      // One sample per draw, with different sample masks.
      spec_const_sample_id = builder.makeUintConstant(0, true);
      builder.addName(spec_const_sample_id, "xe_transfer_sample_id");
      builder.addDecoration(spec_const_sample_id, spv::DecorationSpecId, 0);
    }
  }

  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function = builder.makeFunctionEntry(
      spv::NoPrecision, type_void, "main", main_param_types, main_precisions, &main_entry);

  // Unsigned numbers are used throughout, with bitcasting to signed at texture fetch.

  u32 tile_width_samples = xenos::kEDRAMTileWidthSamples * GetDrawResolutionScaleX();
  u32 tile_height_samples = xenos::kEDRAMTileHeightSamples * GetDrawResolutionScaleY();

  uint_vector_temp.clear();
  uint_vector_temp.push_back(0);
  uint_vector_temp.push_back(1);
  spv::Id dest_pixel_coord = builder.createUnaryOp(
      spv::OpConvertFToU, type_uint2,
      builder.createRvalueSwizzle(spv::NoPrecision, type_float2,
                                  builder.createLoad(input_fragment_coord, spv::NoPrecision),
                                  uint_vector_temp));
  spv::Id dest_pixel_x = builder.createCompositeExtract(dest_pixel_coord, type_uint, 0);
  spv::Id const_dest_tile_width_pixels = builder.makeUintConstant(
      tile_width_samples >> (static_cast<u32>(dest_is_64bpp) +
                             static_cast<u32>(key.dest_msaa_samples >= xenos::MSAASamples::k4X)));
  spv::Id dest_tile_index_x =
      builder.createBinOp(spv::OpUDiv, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_tile_pixel_x =
      builder.createBinOp(spv::OpUMod, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_pixel_y = builder.createCompositeExtract(dest_pixel_coord, type_uint, 1);
  spv::Id const_dest_tile_height_pixels = builder.makeUintConstant(
      tile_height_samples >> static_cast<u32>(key.dest_msaa_samples >= xenos::MSAASamples::k2X));
  spv::Id dest_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, dest_pixel_y, const_dest_tile_height_pixels);
  spv::Id dest_tile_pixel_y =
      builder.createBinOp(spv::OpUMod, type_uint, dest_pixel_y, const_dest_tile_height_pixels);

  REX_ASSERT(push_constants_member_address != UINT32_MAX);
  id_vector_temp.clear();
  id_vector_temp.push_back(
      builder.makeIntConstant(static_cast<i32>(push_constants_member_address)));
  spv::Id address_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants, id_vector_temp),
      spv::NoPrecision);

  spv::Id dest_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.createTriOp(spv::OpBitFieldUExtract, type_uint, address_constant,
                              builder.makeUintConstant(0),
                              builder.makeUintConstant(xenos::kEDRAMPitchTilesBits)),
          dest_tile_index_y),
      dest_tile_index_x);

  spv::Id dest_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MSAASamples::k1X) {
    if (device_properties.sampleRateShading) {
      REX_ASSERT(input_sample_id != spv::NoResult);
      dest_sample_id = builder.createUnaryOp(spv::OpBitcast, type_uint,
                                             builder.createLoad(input_sample_id, spv::NoPrecision));
    } else {
      REX_ASSERT(spec_const_sample_id != spv::NoResult);
      dest_sample_id = spec_const_sample_id;
    }
  }

  spv::Id source_sample_id = dest_sample_id;
  spv::Id source_tile_pixel_x = dest_tile_pixel_x;
  spv::Id source_tile_pixel_y = dest_tile_pixel_y;
  spv::Id source_color_half = spv::NoResult;
  if (!source_is_64bpp && dest_is_64bpp) {
    if (key.source_msaa_samples >= xenos::MSAASamples::k4X) {
      if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
        source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                               builder.makeUintConstant(1 << 1));
        source_tile_pixel_x = builder.CreateQuadOp(spv::OpBitFieldInsert, type_uint, dest_sample_id,
                                                   dest_tile_pixel_x, builder.makeUintConstant(1),
                                                   builder.makeUintConstant(31));
      } else if (key.dest_msaa_samples == xenos::MSAASamples::k2X) {
        if (msaa_2x_attachments_supported_) {
          source_sample_id =
              builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                  builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                                      builder.makeUintConstant(1)),
                                  builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                                 builder.makeUintConstant(1 << 1));
        }
      } else {
        source_sample_id = builder.CreateQuadOp(
            spv::OpBitFieldInsert, type_uint, builder.makeUintConstant(0), dest_tile_pixel_y,
            builder.makeUintConstant(1), builder.makeUintConstant(1));
        source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_y, builder.makeUintConstant(1));
      }
    } else {
      if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
        source_tile_pixel_x = builder.CreateQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftLeftLogical, type_uint, dest_tile_pixel_x,
                                builder.makeUintConstant(2)),
            dest_sample_id, builder.makeUintConstant(1), builder.makeUintConstant(1));
      } else {
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
      if (key.source_msaa_samples >= xenos::MSAASamples::k4X) {
        source_sample_id = builder.CreateQuadOp(spv::OpBitFieldInsert, type_uint, dest_sample_id,
                                                dest_tile_pixel_x, builder.makeUintConstant(0),
                                                builder.makeUintConstant(1));
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
      } else {
      }
      source_color_half = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1));
    } else {
      if (key.source_msaa_samples >= xenos::MSAASamples::k4X) {
        source_sample_id =
            builder.createTriOp(spv::OpBitFieldUExtract, type_uint, dest_tile_pixel_x,
                                builder.makeUintConstant(1), builder.makeUintConstant(1));
        if (key.dest_msaa_samples == xenos::MSAASamples::k2X) {
          if (msaa_2x_attachments_supported_) {
            source_sample_id = builder.CreateQuadOp(
                spv::OpBitFieldInsert, type_uint, source_sample_id,
                builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(1));
          } else {
            source_sample_id = builder.CreateQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id, source_sample_id,
                builder.makeUintConstant(0), builder.makeUintConstant(1));
          }
        } else {
          source_sample_id = builder.CreateQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_tile_pixel_y,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_y, builder.makeUintConstant(1));
        }
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(2));
      } else {
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
      }
      source_color_half = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                              builder.makeUintConstant(1));
    }
    REX_ASSERT(source_color_half != spv::NoResult);
  } else {
    if (key.source_msaa_samples != key.dest_msaa_samples) {
      if (key.source_msaa_samples >= xenos::MSAASamples::k4X) {
        if (key.dest_msaa_samples == xenos::MSAASamples::k2X) {
          if (msaa_2x_attachments_supported_) {
            source_sample_id = builder.CreateQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(31));
          } else {
            source_sample_id = builder.CreateQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                builder.makeUintConstant(0), builder.makeUintConstant(1));
          }
          source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_x, builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.CreateQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(1));
          source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_x, builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_y, builder.makeUintConstant(1));
        }
      } else {
        if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
          source_tile_pixel_x = builder.CreateQuadOp(
              spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
              builder.makeUintConstant(1), builder.makeUintConstant(31));
        }
      }
    }
  }
  if (key.source_msaa_samples < xenos::MSAASamples::k4X &&
      key.source_msaa_samples != key.dest_msaa_samples) {
    if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
      if (key.source_msaa_samples == xenos::MSAASamples::k2X) {
        source_sample_id = builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                               builder.makeUintConstant(1));
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint, source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.CreateQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_sample_id,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
        }
      } else {
        source_tile_pixel_y = builder.CreateQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                builder.makeUintConstant(1)),
            dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
      }
    } else {
      if (key.source_msaa_samples == xenos::MSAASamples::k2X) {
        source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                                               builder.makeUintConstant(1));
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint, source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.CreateQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_sample_id,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
        }
        source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_y, builder.makeUintConstant(1));
      } else {
        if (msaa_2x_attachments_supported_) {
          source_tile_pixel_y = builder.CreateQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
        } else {
          source_tile_pixel_y = builder.CreateQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
        }
      }
    }
  }

  u32 source_pixel_width_dwords_log2 =
      static_cast<u32>(key.source_msaa_samples >= xenos::MSAASamples::k4X) +
      static_cast<u32>(source_is_64bpp);

  if (source_is_color != dest_is_color) {
    // Color and depth or stencil swap 40 sample columns within the source 32bpp tile.
    u32 source_32bpp_tile_half_pixels = tile_width_samples >> (1 + source_pixel_width_dwords_log2);
    source_tile_pixel_x = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int, source_tile_pixel_x),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(spv::OpULessThan, builder.makeBoolType(), source_tile_pixel_x,
                                    builder.makeUintConstant(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(static_cast<i32>(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(-static_cast<i32>(source_32bpp_tile_half_pixels)))));
  }

  spv::Id source_tile_index = builder.createBinOp(
      spv::OpBitwiseAnd, type_uint,
      builder.createUnaryOp(
          spv::OpBitcast, type_uint,
          builder.createBinOp(
              spv::OpIAdd, type_int,
              builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
              builder.createTriOp(spv::OpBitFieldSExtract, type_int,
                                  builder.createUnaryOp(spv::OpBitcast, type_int, address_constant),
                                  builder.makeUintConstant(xenos::kEDRAMPitchTilesBits * 2),
                                  builder.makeUintConstant(xenos::kEDRAMBaseTilesBits + 1)))),
      builder.makeUintConstant(xenos::kEDRAMTileCount - 1));
  spv::Id source_pitch_tiles =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, address_constant,
                          builder.makeUintConstant(xenos::kEDRAMPitchTilesBits),
                          builder.makeUintConstant(xenos::kEDRAMPitchTilesBits));
  spv::Id source_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x =
      builder.createBinOp(spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_pixel_x_int = builder.createUnaryOp(
      spv::OpBitcast, type_int,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(tile_width_samples >> source_pixel_width_dwords_log2),
              source_tile_index_x),
          source_tile_pixel_x));
  spv::Id source_pixel_y_int = builder.createUnaryOp(
      spv::OpBitcast, type_int,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(spv::OpIMul, type_uint,
                              builder.makeUintConstant(tile_height_samples >>
                                                       static_cast<u32>(key.source_msaa_samples >=
                                                                        xenos::MSAASamples::k2X)),
                              source_tile_index_y),
          source_tile_pixel_y));

  spv::Builder::TextureParameters source_texture_parameters = {};
  id_vector_temp.clear();
  id_vector_temp.push_back(source_pixel_x_int);
  id_vector_temp.push_back(source_pixel_y_int);
  spv::Id source_coordinates[2] = {
      builder.createCompositeConstruct(type_int2, id_vector_temp),
  };
  spv::Id source_sample_ids_int[2] = {};
  if (key.source_msaa_samples != xenos::MSAASamples::k1X) {
    source_sample_ids_int[0] = builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  } else {
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  bool source_load_is_two_32bpp_samples = !source_is_64bpp && dest_is_64bpp;
  if (source_load_is_two_32bpp_samples) {
    if (key.source_msaa_samples >= xenos::MSAASamples::k4X) {
      source_coordinates[1] = source_coordinates[0];
      source_sample_ids_int[1] = builder.createBinOp(
          spv::OpBitwiseOr, type_int, source_sample_ids_int[0], builder.makeIntConstant(1));
    } else {
      id_vector_temp.clear();
      id_vector_temp.push_back(builder.createBinOp(spv::OpBitwiseOr, type_int, source_pixel_x_int,
                                                   builder.makeIntConstant(1)));
      id_vector_temp.push_back(source_pixel_y_int);
      source_coordinates[1] = builder.createCompositeConstruct(type_int2, id_vector_temp);
      source_sample_ids_int[1] = source_sample_ids_int[0];
    }
  }
  spv::Id source_color[2][4] = {};
  if (source_color_texture != spv::NoResult) {
    source_texture_parameters.sampler = builder.createLoad(source_color_texture, spv::NoPrecision);
    REX_ASSERT(source_color_component_type != spv::NoType);
    spv::Id source_color_vec4_type = builder.makeVectorType(source_color_component_type, 4);
    for (u32 i = 0; i <= static_cast<u32>(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      spv::Id source_color_vec4 = builder.createTextureCall(
          spv::NoPrecision, source_color_vec4_type, false, true, false, false, false,
          source_texture_parameters, spv::ImageOperandsMaskNone);
      u32 source_color_components_remaining = source_color_texture_component_mask;
      u32 source_color_component_index;
      while (
          rex::bit_scan_forward(source_color_components_remaining, &source_color_component_index)) {
        source_color_components_remaining &= ~(static_cast<u32>(1) << source_color_component_index);
        source_color[i][source_color_component_index] = builder.createCompositeExtract(
            source_color_vec4, source_color_component_type, source_color_component_index);
      }
    }
  }
  spv::Id source_depth_float[2] = {};
  if (source_depth_texture != spv::NoResult) {
    source_texture_parameters.sampler = builder.createLoad(source_depth_texture, spv::NoPrecision);
    for (u32 i = 0; i <= static_cast<u32>(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_depth_float[i] = builder.createCompositeExtract(
          builder.createTextureCall(spv::NoPrecision, type_float4, false, true, false, false, false,
                                    source_texture_parameters, spv::ImageOperandsMaskNone),
          type_float, 0);
    }
  }
  spv::Id source_stencil[2] = {};
  if (source_stencil_texture != spv::NoResult) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    for (u32 i = 0; i <= static_cast<u32>(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_stencil[i] = builder.createCompositeExtract(
          builder.createTextureCall(spv::NoPrecision, type_uint4, false, true, false, false, false,
                                    source_texture_parameters, spv::ImageOperandsMaskNone),
          type_uint, 0);
    }
  }

  if (source_is_64bpp && !dest_is_64bpp) {
    u32 source_color_half_component_count = source_color_format_component_count >> 1;
    REX_ASSERT(source_color_half != spv::NoResult);
    spv::Id source_color_is_second_half = builder.createBinOp(
        spv::OpINotEqual, type_bool, source_color_half, builder.makeUintConstant(0));
    if (mode.output == TransferOutput::kStencilBit) {
      source_color[0][0] = builder.createTriOp(
          spv::OpSelect, source_color_component_type, source_color_is_second_half,
          source_color[0][source_color_half_component_count], source_color[0][0]);
    } else {
      for (u32 i = 0; i < source_color_half_component_count; ++i) {
        source_color[0][i] = builder.createTriOp(
            spv::OpSelect, source_color_component_type, source_color_is_second_half,
            source_color[0][source_color_half_component_count + i], source_color[0][i]);
      }
    }
  }

  if (output_fragment_stencil_ref != spv::NoResult && source_stencil[0] != spv::NoResult) {
    REX_ASSERT(mode.output == TransferOutput::kDepth);
    builder.createStore(builder.createUnaryOp(spv::OpBitcast, type_int, source_stencil[0]),
                        output_fragment_stencil_ref);
  }

  const bool source_color_16_is_float = IsColor16FormatFloatLike(source_color_format);
  const bool dest_color_16_is_float = IsColor16FormatFloatLike(dest_color_format);
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_uint_16 = builder.makeUintConstant(16);
  spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
  spv::Id const_float_1 = builder.makeFloatConstant(1.0f);
  spv::Id const_float_minus_1 = builder.makeFloatConstant(-1.0f);
  spv::Id const_float_32767 = builder.makeFloatConstant(32767.0f);
  spv::Id const_float_inv_32767 = builder.makeFloatConstant(1.0f / 32767.0f);
  auto pwl_gamma_to_linear = [&](spv::Id gamma, bool gamma_pre_saturated) -> spv::Id {
    if (!gamma_pre_saturated) {
      gamma = builder.CreateTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                           gamma, const_float_0, const_float_1);
    }
    spv::Id is_piece_at_least_3 = builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, gamma,
                                                      builder.makeFloatConstant(192.0f / 255.0f));
    spv::Id scale_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                               builder.makeFloatConstant(8.0f / 1024.0f),
                                               builder.makeFloatConstant(4.0f / 1024.0f));
    spv::Id offset_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                                builder.makeFloatConstant(-1024.0f),
                                                builder.makeFloatConstant(-256.0f));
    spv::Id is_piece_at_least_1 = builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, gamma,
                                                      builder.makeFloatConstant(64.0f / 255.0f));
    spv::Id scale_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                               builder.makeFloatConstant(2.0f / 1024.0f),
                                               builder.makeFloatConstant(1.0f / 1024.0f));
    spv::Id offset_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                                builder.makeFloatConstant(-64.0f), const_float_0);
    spv::Id is_piece_at_least_2 = builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, gamma,
                                                      builder.makeFloatConstant(96.0f / 255.0f));
    spv::Id scale = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
    spv::Id offset = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);
    spv::Id linear = builder.createBinOp(
        spv::OpFAdd, type_float,
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createBinOp(spv::OpFMul, type_float, gamma,
                                                builder.makeFloatConstant(255.0f * 1024.0f)),
                            scale),
        offset);
    linear = builder.createBinOp(spv::OpFAdd, type_float, linear,
                                 builder.CreateUnaryBuiltinCall(
                                     type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                     builder.createBinOp(spv::OpFMul, type_float, linear, scale)));
    return builder.createBinOp(spv::OpFMul, type_float, linear,
                               builder.makeFloatConstant(1.0f / 1023.0f));
  };
  auto linear_to_pwl_gamma = [&](spv::Id linear, bool linear_pre_saturated) -> spv::Id {
    if (!linear_pre_saturated) {
      linear = builder.CreateTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                            linear, const_float_0, const_float_1);
    }
    spv::Id is_piece_at_least_3 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, linear,
                            builder.makeFloatConstant(512.0f / 1023.0f));
    spv::Id scale_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                               builder.makeFloatConstant(1023.0f / 8.0f),
                                               builder.makeFloatConstant(1023.0f / 4.0f));
    spv::Id offset_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                                builder.makeFloatConstant(128.0f / 255.0f),
                                                builder.makeFloatConstant(64.0f / 255.0f));
    spv::Id is_piece_at_least_1 = builder.createBinOp(
        spv::OpFOrdGreaterThanEqual, type_bool, linear, builder.makeFloatConstant(64.0f / 1023.0f));
    spv::Id scale_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                               builder.makeFloatConstant(1023.0f / 2.0f),
                                               builder.makeFloatConstant(1023.0f));
    spv::Id offset_1_or_0 =
        builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                            builder.makeFloatConstant(32.0f / 255.0f), const_float_0);
    spv::Id is_piece_at_least_2 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, linear,
                            builder.makeFloatConstant(128.0f / 1023.0f));
    spv::Id scale = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
    spv::Id offset = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);
    return builder.createBinOp(
        spv::OpFAdd, type_float,
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.CreateUnaryBuiltinCall(
                                type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                builder.createBinOp(spv::OpFMul, type_float, linear, scale)),
                            builder.makeFloatConstant(1.0f / 255.0f)),
        offset);
  };
  auto pack_source16_component_to_uint = [&](spv::Id component) -> spv::Id {
    if (source_color_is_uint) {
      return component;
    }
    if (source_color_16_is_float) {
      id_vector_temp.clear();
      id_vector_temp.push_back(component);
      id_vector_temp.push_back(const_float_0);
      spv::Id packed_half = builder.CreateUnaryBuiltinCall(
          type_uint, ext_inst_glsl_std_450, GLSLstd450PackHalf2x16,
          builder.createCompositeConstruct(type_float2, id_vector_temp));
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_half, const_uint_0,
                                 const_uint_16);
    }
    spv::Id component_clamped =
        builder.CreateTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, component,
                                     const_float_minus_1, const_float_1);
    spv::Id component_rounded = builder.CreateUnaryBuiltinCall(
        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
        builder.createBinOp(spv::OpFMul, type_float, component_clamped, const_float_32767));
    spv::Id component_snorm =
        builder.createUnaryOp(spv::OpConvertFToS, type_int, component_rounded);
    return builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                               builder.createUnaryOp(spv::OpBitcast, type_uint, component_snorm),
                               const_uint_0, const_uint_16);
  };
  auto pack_source16_pair_to_uint32 = [&](spv::Id component_0, spv::Id component_1) -> spv::Id {
    return builder.CreateQuadOp(
        spv::OpBitFieldInsert, type_uint, pack_source16_component_to_uint(component_0),
        pack_source16_component_to_uint(component_1), const_uint_16, const_uint_16);
  };
  auto unpack_dest16_component_from_uint32 = [&](spv::Id packed_word,
                                                 u32 component_index) -> spv::Id {
    spv::Id component_offset = component_index & 1 ? const_uint_16 : const_uint_0;
    if (dest_color_is_uint) {
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_word, component_offset,
                                 const_uint_16);
    }
    if (dest_color_16_is_float) {
      spv::Id component_pair = builder.CreateUnaryBuiltinCall(
          type_float2, ext_inst_glsl_std_450, GLSLstd450UnpackHalf2x16, packed_word);
      return builder.createCompositeExtract(component_pair, type_float, component_index & 1);
    }
    spv::Id component_snorm = builder.createTriOp(spv::OpBitFieldSExtract, type_int, packed_word,
                                                  component_offset, const_uint_16);
    spv::Id component_float =
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createUnaryOp(spv::OpConvertSToF, type_float, component_snorm),
                            const_float_inv_32767);
    return builder.CreateTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                        component_float, const_float_minus_1, const_float_1);
  };

  if (dest_is_64bpp) {
    // packed holds two uints for the generic unpack path. Other paths write the output themselves.
    spv::Id packed[2] = {};
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (gamma_render_target_as_unorm16_) {
            // 8_8_8_8_GAMMA is stored as linear in R16G16B16A16_UNORM.
            for (u32 i = 0; i < 2; ++i) {
              for (u32 j = 0; j < 3; ++j) {
                source_color[i][j] = linear_to_pwl_gamma(source_color[i][j], true);
              }
            }
          }
        }
          [[fallthrough]];
        case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
          spv::Id component_width = builder.makeUintConstant(8);
          for (u32 i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float, source_color[i][0], unorm_scale),
                    unorm_round_offset));
            for (u32 j = 1; j < 4; ++j) {
              packed[i] = builder.CreateQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(spv::OpFAdd, type_float,
                                          builder.createBinOp(spv::OpFMul, type_float,
                                                              source_color[i][j], unorm_scale),
                                          unorm_round_offset)),
                  builder.makeUintConstant(8 * j), component_width);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id width_a = builder.makeUintConstant(2);
          for (u32 i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[i][0],
                                                        unorm_scale_rgb),
                                    unorm_round_offset));
            for (u32 j = 1; j < 4; ++j) {
              packed[i] = builder.CreateQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, source_color[i][j],
                                              j == 3 ? unorm_scale_a : unorm_scale_rgb),
                          unorm_round_offset)),
                  builder.makeUintConstant(10 * j), j == 3 ? width_a : width_rgb);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id float_0 = builder.makeFloatConstant(0.0f);
          spv::Id float_1 = builder.makeFloatConstant(1.0f);
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id offset_a = builder.makeUintConstant(30);
          spv::Id width_a = builder.makeUintConstant(2);
          for (u32 i = 0; i < 2; ++i) {
            // Float16 has a wider range for color and alpha, and NaNs, so clamp and convert.
            packed[i] = SPIRVShaderTranslator::UnclampedFloat32To7e3(builder, source_color[i][0],
                                                                     ext_inst_glsl_std_450);
            for (u32 j = 1; j < 3; ++j) {
              packed[i] =
                  builder.CreateQuadOp(spv::OpBitFieldInsert, type_uint, packed[i],
                                       SPIRVShaderTranslator::UnclampedFloat32To7e3(
                                           builder, source_color[i][j], ext_inst_glsl_std_450),
                                       builder.makeUintConstant(10 * j), width_rgb);
            }
            spv::Id alpha_saturated =
                builder.CreateTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                             source_color[i][3], float_0, float_1);
            packed[i] = builder.CreateQuadOp(
                spv::OpBitFieldInsert, type_uint, packed[i],
                builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createBinOp(spv::OpFAdd, type_float,
                                        builder.createBinOp(spv::OpFMul, type_float,
                                                            alpha_saturated, unorm_scale_a),
                                        unorm_round_offset)),
                offset_a, width_a);
          }
        } break;
        // 16 bit pairs go through packed uint32 words, which handles mixed integer and float types.
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
          for (u32 i = 0; i < 2; ++i) {
            packed[i] = pack_source16_pair_to_uint32(source_color[i][0], source_color[i][1]);
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          for (u32 i = 0; i < 2; ++i) {
            packed[i] = pack_source16_pair_to_uint32(source_color[0][i << 1],
                                                     source_color[0][(i << 1) + 1]);
          }
        } break;
        // Float32 goes as uint32 to keep NaN encodings, though integer MSAA images are optional.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
          for (u32 i = 0; i < 2; ++i) {
            packed[i] = source_color[i][0];
            if (!source_color_is_uint) {
              packed[i] = builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          for (u32 i = 0; i < 2; ++i) {
            packed[i] = source_color[0][i];
            if (!source_color_is_uint) {
              packed[i] = builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
      }
    } else {
      REX_ASSERT(source_depth_texture != spv::NoResult);
      REX_ASSERT(source_stencil_texture != spv::NoResult);
      spv::Id depth_offset = builder.makeUintConstant(8);
      spv::Id depth_width = builder.makeUintConstant(24);
      for (u32 i = 0; i < 2; ++i) {
        spv::Id depth24 = spv::NoResult;
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Nearest even rounding, as +0.5 with truncation makes black red in a clear shader.
            depth24 = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.CreateUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(spv::OpFMul, type_float, source_depth_float[i],
                                        builder.makeFloatConstant(static_cast<f32>(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            depth24 = SPIRVShaderTranslator::PreClampedDepthTo20e4(builder, source_depth_float[i],
                                                                   IsDepthFloat24Rounded(), true,
                                                                   ext_inst_glsl_std_450);
          } break;
        }
        packed[i] = builder.CreateQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil[i],
                                         depth24, depth_offset, depth_width);
      }
    }
    if (packed[0] != spv::NoResult) {
      REX_ASSERT(packed[1] != spv::NoResult);
      if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
        id_vector_temp.clear();
        id_vector_temp.push_back(packed[0]);
        id_vector_temp.push_back(packed[1]);
        // Without integer transfer formats for this sample count, raw bits go via float bitcasts.
        if (!dest_color_is_uint) {
          for (spv::Id& float32 : id_vector_temp) {
            float32 = builder.createUnaryOp(spv::OpBitcast, type_float, float32);
          }
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                            output_fragment_data);
      } else {
        id_vector_temp.clear();
        for (u32 i = 0; i < 4; ++i) {
          id_vector_temp.push_back(unpack_dest16_component_from_uint32(packed[i >> 1], i));
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                            output_fragment_data);
      }
    }
  } else {
    // packed holds the generic unpack path value. Other paths write the output themselves.
    spv::Id packed = spv::NoResult;
    bool packed_only_depth = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (mode.output == TransferOutput::kStencilBit) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              source_color[0][0] = linear_to_pwl_gamma(source_color[0][0], true);
            }
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
                      dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
            if (source_color_format != dest_color_format) {
              if (dest_color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8) {
                for (u32 i = 0; i < 3; ++i) {
                  source_color[0][i] = linear_to_pwl_gamma(source_color[0][i], true);
                }
              } else {
                for (u32 i = 0; i < 3; ++i) {
                  source_color[0][i] = pwl_gamma_to_linear(source_color[0][i], true);
                }
              }
            }
            id_vector_temp.clear();
            for (u32 i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else if (mode.output == TransferOutput::kDepth) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              for (u32 i = output_fragment_stencil_ref != spv::NoResult ? 0 : 1; i < 3; ++i) {
                source_color[0][i] = linear_to_pwl_gamma(source_color[0][i], true);
              }
            }
            packed_only_depth = true;
            if (output_fragment_stencil_ref != spv::NoResult) {
              builder.createStore(
                  builder.createUnaryOp(
                      spv::OpBitcast, type_int,
                      builder.createUnaryOp(
                          spv::OpConvertFToU, type_uint,
                          builder.createBinOp(
                              spv::OpFAdd, type_float,
                              builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                  builder.makeFloatConstant(255.0f)),
                              builder.makeFloatConstant(0.5f)))),
                  output_fragment_stencil_ref);
            }
            // Depth goes in bits 0 to 23.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][1],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
            spv::Id component_width = builder.makeUintConstant(8);
            for (u32 i = 2; i < 4; ++i) {
              packed = builder.CreateQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, source_color[0][i],
                                              builder.makeFloatConstant(255.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(8 * (i - 1)), component_width);
            }
          } else {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              for (u32 i = 0; i < 3; ++i) {
                source_color[0][i] = linear_to_pwl_gamma(source_color[0][i], true);
              }
            }
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
            spv::Id component_width = builder.makeUintConstant(8);
            for (u32 i = 1; i < 4; ++i) {
              packed = builder.CreateQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, source_color[0][i],
                                              builder.makeFloatConstant(255.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(8 * i), component_width);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
               dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)) {
            id_vector_temp.clear();
            for (u32 i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else {
            spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
            spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        unorm_scale_rgb),
                                    unorm_round_offset));
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              for (u32 i = 1; i < 4; ++i) {
                packed = builder.CreateQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    builder.createUnaryOp(
                        spv::OpConvertFToU, type_uint,
                        builder.createBinOp(
                            spv::OpFAdd, type_float,
                            builder.createBinOp(spv::OpFMul, type_float, source_color[0][i],
                                                i == 3 ? unorm_scale_a : unorm_scale_rgb),
                            unorm_round_offset)),
                    builder.makeUintConstant(10 * i), i == 3 ? width_a : width_rgb);
              }
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
               dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
            id_vector_temp.clear();
            for (u32 i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else {
            // Float16 has a wider range for color and alpha, and NaNs, so clamp and convert.
            packed = SPIRVShaderTranslator::UnclampedFloat32To7e3(builder, source_color[0][0],
                                                                  ext_inst_glsl_std_450);
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              for (u32 i = 1; i < 3; ++i) {
                packed =
                    builder.CreateQuadOp(spv::OpBitFieldInsert, type_uint, packed,
                                         SPIRVShaderTranslator::UnclampedFloat32To7e3(
                                             builder, source_color[0][i], ext_inst_glsl_std_450),
                                         builder.makeUintConstant(10 * i), width_rgb);
              }
              spv::Id alpha_saturated = builder.CreateTriBuiltinCall(
                  type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, source_color[0][3],
                  builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
              packed = builder.CreateQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, alpha_saturated,
                                              builder.makeFloatConstant(3.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(30), builder.makeUintConstant(2));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          // Packing through uint32 reconstructs the destination transfer type (integer or float).
          packed = pack_source16_component_to_uint(source_color[0][0]);
          if (mode.output != TransferOutput::kStencilBit) {
            packed = builder.CreateQuadOp(spv::OpBitFieldInsert, type_uint, packed,
                                          pack_source16_component_to_uint(source_color[0][1]),
                                          const_uint_16, const_uint_16);
          }
        } break;
        // Float32 goes as uint32 to keep NaN encodings, though integer MSAA images are optional.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          packed = source_color[0][0];
          if (!source_color_is_uint) {
            packed = builder.createUnaryOp(spv::OpBitcast, type_uint, packed);
          }
        } break;
      }
    } else if (source_depth_float[0] != spv::NoResult) {
      if (mode.output == TransferOutput::kDepth && dest_depth_format == source_depth_format) {
        builder.createStore(source_depth_float[0], output_fragment_depth);
      } else {
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Nearest even rounding, as +0.5 with truncation makes black red in a clear shader.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.CreateUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(spv::OpFMul, type_float, source_depth_float[0],
                                        builder.makeFloatConstant(static_cast<f32>(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            packed = SPIRVShaderTranslator::PreClampedDepthTo20e4(builder, source_depth_float[0],
                                                                  IsDepthFloat24Rounded(), true,
                                                                  ext_inst_glsl_std_450);
          } break;
        }
        if (mode.output == TransferOutput::kDepth) {
          packed_only_depth = true;
        } else {
          packed = builder.CreateQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil[0], packed,
                                        builder.makeUintConstant(8), builder.makeUintConstant(24));
        }
      }
    }
    switch (mode.output) {
      case TransferOutput::kColor: {
        if (packed != spv::NoResult) {
          switch (dest_color_format) {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8:
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
              spv::Id component_width = builder.makeUintConstant(8);
              spv::Id unorm_scale = builder.makeFloatConstant(1.0f / 255.0f);
              id_vector_temp.clear();
              for (u32 i = 0; i < 4; ++i) {
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed,
                                            builder.makeUintConstant(8 * i), component_width)),
                    unorm_scale));
              }
              if (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                  gamma_render_target_as_unorm16_) {
                // 8_8_8_8_GAMMA is stored as linear in R16G16B16A16_UNORM.
                for (u32 i = 0; i < 3; ++i) {
                  id_vector_temp[i] = pwl_gamma_to_linear(id_vector_temp[i], true);
                }
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_rgb = builder.makeFloatConstant(1.0f / 1023.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              spv::Id unorm_scale_a = builder.makeFloatConstant(1.0f / 3.0f);
              id_vector_temp.clear();
              for (u32 i = 0; i < 4; ++i) {
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed,
                                            builder.makeUintConstant(10 * i),
                                            i == 3 ? width_a : width_rgb)),
                    i == 3 ? unorm_scale_a : unorm_scale_rgb));
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
              id_vector_temp.clear();
              for (u32 i = 0; i < 3; ++i) {
                id_vector_temp.push_back(SPIRVShaderTranslator::Float7e3To32(
                    builder, packed, 10 * i, false, ext_inst_glsl_std_450));
              }
              id_vector_temp.push_back(builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(spv::OpConvertUToF, type_float,
                                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                                                            packed, builder.makeUintConstant(30),
                                                            builder.makeUintConstant(2))),
                  builder.makeFloatConstant(1.0f / 3.0f)));
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
              id_vector_temp.clear();
              for (u32 i = 0; i < 2; ++i) {
                id_vector_temp.push_back(unpack_dest16_component_from_uint32(packed, i));
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
              // uint32 keeps NaN encodings, bitcast when the transfer format is float.
              spv::Id float32 = packed;
              if (!dest_color_is_uint) {
                float32 = builder.createUnaryOp(spv::OpBitcast, type_float, float32);
              }
              builder.createStore(float32, output_fragment_data);
            } break;
            default:
              // A 64bpp format (handled separately) or an invalid one.
              REX_UNHANDLED_CASE(dest_color_format);
          }
        }
      } break;
      case TransferOutput::kDepth: {
        if (packed) {
          spv::Id guest_depth24 = packed;
          if (!packed_only_depth) {
            guest_depth24 = builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_depth24,
                                                builder.makeUintConstant(8));
          }
          spv::Id host_depth32 = spv::NoResult;
          if (host_depth_source_texture != spv::NoResult) {
            spv::Id host_depth_source_sample_id = dest_sample_id;
            spv::Id host_depth_source_tile_pixel_x = dest_tile_pixel_x;
            spv::Id host_depth_source_tile_pixel_y = dest_tile_pixel_y;
            if (key.host_depth_source_msaa_samples != key.dest_msaa_samples) {
              if (key.host_depth_source_msaa_samples >= xenos::MSAASamples::k4X) {
                if (key.dest_msaa_samples == xenos::MSAASamples::k2X) {
                  if (msaa_2x_attachments_supported_) {
                    host_depth_source_sample_id = builder.CreateQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                        builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1)),
                        builder.makeUintConstant(1), builder.makeUintConstant(31));
                  } else {
                    host_depth_source_sample_id = builder.CreateQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                        builder.makeUintConstant(0), builder.makeUintConstant(1));
                  }
                  host_depth_source_tile_pixel_x =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1));
                } else {
                  host_depth_source_sample_id = builder.CreateQuadOp(
                      spv::OpBitFieldInsert, type_uint,
                      builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1)),
                      dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(1));
                  host_depth_source_tile_pixel_x =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1));
                  host_depth_source_tile_pixel_y =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                                          builder.makeUintConstant(1));
                }
              } else {
                if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
                  host_depth_source_tile_pixel_x = builder.CreateQuadOp(
                      spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                      builder.makeUintConstant(1), builder.makeUintConstant(31));
                }
              }
              if (key.host_depth_source_msaa_samples < xenos::MSAASamples::k4X) {
                if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
                  if (key.host_depth_source_msaa_samples == xenos::MSAASamples::k2X) {
                    host_depth_source_sample_id =
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1));
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_sample_id = builder.createBinOp(
                          spv::OpBitwiseXor, type_uint, host_depth_source_sample_id,
                          builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id = builder.CreateQuadOp(
                          spv::OpBitFieldInsert, type_uint, host_depth_source_sample_id,
                          host_depth_source_sample_id, builder.makeUintConstant(1),
                          builder.makeUintConstant(1));
                    }
                  } else {
                    host_depth_source_tile_pixel_y = builder.CreateQuadOp(
                        spv::OpBitFieldInsert, type_uint,
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1)),
                        dest_tile_pixel_y, builder.makeUintConstant(1),
                        builder.makeUintConstant(31));
                  }
                } else {
                  if (key.host_depth_source_msaa_samples == xenos::MSAASamples::k2X) {
                    host_depth_source_sample_id =
                        builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                                            builder.makeUintConstant(1));
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_sample_id = builder.createBinOp(
                          spv::OpBitwiseXor, type_uint, host_depth_source_sample_id,
                          builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id = builder.CreateQuadOp(
                          spv::OpBitFieldInsert, type_uint, host_depth_source_sample_id,
                          host_depth_source_sample_id, builder.makeUintConstant(1),
                          builder.makeUintConstant(1));
                    }
                    host_depth_source_tile_pixel_y =
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                                            builder.makeUintConstant(1));
                  } else {
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_tile_pixel_y = builder.CreateQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    } else {
                      host_depth_source_tile_pixel_y = builder.CreateQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    }
                  }
                }
              }
            }
            REX_ASSERT(push_constants_member_host_depth_address != UINT32_MAX);
            id_vector_temp.clear();
            id_vector_temp.push_back(builder.makeIntConstant(
                static_cast<i32>(push_constants_member_host_depth_address)));
            spv::Id host_depth_address_constant =
                builder.createLoad(builder.createAccessChain(spv::StorageClassPushConstant,
                                                             push_constants, id_vector_temp),
                                   spv::NoPrecision);
            spv::Id host_depth_source_tile_index = builder.createBinOp(
                spv::OpBitwiseAnd, type_uint,
                builder.createUnaryOp(
                    spv::OpBitcast, type_uint,
                    builder.createBinOp(
                        spv::OpIAdd, type_int,
                        builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
                        builder.createTriOp(
                            spv::OpBitFieldSExtract, type_int,
                            builder.createUnaryOp(spv::OpBitcast, type_int,
                                                  host_depth_address_constant),
                            builder.makeUintConstant(xenos::kEDRAMPitchTilesBits * 2),
                            builder.makeUintConstant(xenos::kEDRAMBaseTilesBits + 1)))),
                builder.makeUintConstant(xenos::kEDRAMTileCount - 1));
            spv::Id host_depth_source_pitch_tiles =
                builder.createTriOp(spv::OpBitFieldUExtract, type_uint, host_depth_address_constant,
                                    builder.makeUintConstant(xenos::kEDRAMPitchTilesBits),
                                    builder.makeUintConstant(xenos::kEDRAMPitchTilesBits));
            spv::Id host_depth_source_tile_index_y =
                builder.createBinOp(spv::OpUDiv, type_uint, host_depth_source_tile_index,
                                    host_depth_source_pitch_tiles);
            spv::Id host_depth_source_tile_index_x =
                builder.createBinOp(spv::OpUMod, type_uint, host_depth_source_tile_index,
                                    host_depth_source_pitch_tiles);
            spv::Id host_depth_source_pixel_x_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(
                        spv::OpIMul, type_uint,
                        builder.makeUintConstant(
                            tile_width_samples >>
                            static_cast<u32>(key.source_msaa_samples >= xenos::MSAASamples::k4X)),
                        host_depth_source_tile_index_x),
                    host_depth_source_tile_pixel_x));
            spv::Id host_depth_source_pixel_y_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(
                        spv::OpIMul, type_uint,
                        builder.makeUintConstant(
                            tile_height_samples >>
                            static_cast<u32>(key.source_msaa_samples >= xenos::MSAASamples::k2X)),
                        host_depth_source_tile_index_y),
                    host_depth_source_tile_pixel_y));
            spv::Builder::TextureParameters host_depth_source_texture_parameters = {};
            host_depth_source_texture_parameters.sampler =
                builder.createLoad(host_depth_source_texture, spv::NoPrecision);
            id_vector_temp.clear();
            id_vector_temp.push_back(host_depth_source_pixel_x_int);
            id_vector_temp.push_back(host_depth_source_pixel_y_int);
            host_depth_source_texture_parameters.coords =
                builder.createCompositeConstruct(type_int2, id_vector_temp);
            if (key.host_depth_source_msaa_samples != xenos::MSAASamples::k1X) {
              host_depth_source_texture_parameters.sample =
                  builder.createUnaryOp(spv::OpBitcast, type_int, host_depth_source_sample_id);
            } else {
              host_depth_source_texture_parameters.lod = builder.makeIntConstant(0);
            }
            host_depth32 = builder.createCompositeExtract(
                builder.createTextureCall(spv::NoPrecision, type_float4, false, true, false, false,
                                          false, host_depth_source_texture_parameters,
                                          spv::ImageOperandsMaskNone),
                type_float, 0);
          } else if (host_depth_source_buffer != spv::NoResult) {
            REX_ASSERT(key.host_depth_source_msaa_samples == xenos::MSAASamples::k1X);
            spv::Id dest_tile_sample_x = dest_tile_pixel_x;
            spv::Id dest_tile_sample_y = dest_tile_pixel_y;
            if (key.dest_msaa_samples >= xenos::MSAASamples::k2X) {
              if (key.dest_msaa_samples >= xenos::MSAASamples::k4X) {
                // Horizontal sample index in bit 0.
                dest_tile_sample_x = builder.CreateQuadOp(
                    spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                    builder.makeUintConstant(1), builder.makeUintConstant(31));
              }
              // Vertical sample is in bit 0 (1 or 0) for native 2x, else in bit 1 (0 or 1).
              dest_tile_sample_y = builder.CreateQuadOp(
                  spv::OpBitFieldInsert, type_uint,
                  builder.createBinOp((key.dest_msaa_samples == xenos::MSAASamples::k2X &&
                                       msaa_2x_attachments_supported_)
                                          ? spv::OpBitwiseXor
                                          : spv::OpShiftRightLogical,
                                      type_uint, dest_sample_id, builder.makeUintConstant(1)),
                  dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
            }
            spv::Id host_depth_offset = builder.createBinOp(
                spv::OpIAdd, type_uint,
                builder.createBinOp(
                    spv::OpIMul, type_uint,
                    builder.makeUintConstant(tile_width_samples * tile_height_samples),
                    dest_tile_index),
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(tile_width_samples),
                                        dest_tile_sample_y),
                    dest_tile_sample_x));
            id_vector_temp.clear();
            // The only SSBO structure member.
            id_vector_temp.push_back(builder.makeIntConstant(0));
            id_vector_temp.push_back(
                builder.createUnaryOp(spv::OpBitcast, type_int, host_depth_offset));
            // Uniform rather than StorageBuffer, as SPIR-V 1.0 is generated.
            host_depth32 = builder.createUnaryOp(
                spv::OpBitcast, type_float,
                builder.createLoad(
                    builder.createAccessChain(spv::StorageClassUniform, host_depth_source_buffer,
                                              id_vector_temp),
                    spv::NoPrecision));
          }
          spv::Block* depth24_to_depth32_header = builder.getBuildPoint();
          spv::Block* depth24_to_depth32_merge = nullptr;
          spv::Id host_depth24 = spv::NoResult;
          if (host_depth32 != spv::NoResult) {
            switch (dest_depth_format) {
              case xenos::DepthRenderTargetFormat::kD24S8: {
                // Nearest even rounding, as +0.5 with truncation makes black red in a clear shader.
                host_depth24 = builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.CreateUnaryBuiltinCall(
                        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                        builder.createBinOp(
                            spv::OpFMul, type_float, host_depth32,
                            builder.makeFloatConstant(static_cast<f32>(0xFFFFFF)))));
              } break;
              case xenos::DepthRenderTargetFormat::kD24FS8: {
                host_depth24 = SPIRVShaderTranslator::PreClampedDepthTo20e4(
                    builder, host_depth32, IsDepthFloat24Rounded(), true, ext_inst_glsl_std_450);
              } break;
            }
            REX_ASSERT(host_depth24 != spv::NoResult);
            // The conversion may branch, so refresh the header block pointer.
            depth24_to_depth32_header = builder.getBuildPoint();
            spv::Id host_depth_outdated =
                builder.createBinOp(spv::OpINotEqual, type_bool, guest_depth24, host_depth24);
            spv::Block& depth24_to_depth32_convert_entry = builder.makeNewBlock();
            {
              spv::Block& depth24_to_depth32_merge_block = builder.makeNewBlock();
              depth24_to_depth32_merge = &depth24_to_depth32_merge_block;
            }
            builder.createSelectionMerge(depth24_to_depth32_merge, spv::SelectionControlMaskNone);
            builder.createConditionalBranch(host_depth_outdated, &depth24_to_depth32_convert_entry,
                                            depth24_to_depth32_merge);
            builder.setBuildPoint(&depth24_to_depth32_convert_entry);
          }
          // Runs inside the open conditional when the host depth is loaded.
          spv::Id guest_depth32 = spv::NoResult;
          switch (dest_depth_format) {
            case xenos::DepthRenderTargetFormat::kD24S8: {
              guest_depth32 = builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(
                      spv::OpConvertUToF, type_float,
                      builder.createBinOp(
                          spv::OpIAdd, type_uint, guest_depth24,
                          builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_depth24,
                                              builder.makeUintConstant(23)))),
                  builder.makeFloatConstant(1.0f / static_cast<f32>(1 << 24)));
            } break;
            case xenos::DepthRenderTargetFormat::kD24FS8: {
              guest_depth32 = SPIRVShaderTranslator::Depth20e4To32(builder, guest_depth24, 0, true,
                                                                   false, ext_inst_glsl_std_450);
            } break;
          }
          REX_ASSERT(guest_depth32 != spv::NoResult);
          spv::Id fragment_depth32 = guest_depth32;
          if (host_depth32 != spv::NoResult) {
            REX_ASSERT(depth24_to_depth32_merge != nullptr);
            spv::Id depth24_to_depth32_result_block_id = builder.getBuildPoint()->getId();
            builder.createBranch(depth24_to_depth32_merge);
            builder.setBuildPoint(depth24_to_depth32_merge);
            id_vector_temp.clear();
            id_vector_temp.push_back(guest_depth32);
            id_vector_temp.push_back(depth24_to_depth32_result_block_id);
            id_vector_temp.push_back(host_depth32);
            id_vector_temp.push_back(depth24_to_depth32_header->getId());
            fragment_depth32 = builder.createOp(spv::OpPhi, type_float, id_vector_temp);
          }
          builder.createStore(fragment_depth32, output_fragment_depth);
          if (!packed_only_depth && output_fragment_stencil_ref != spv::NoResult) {
            builder.createStore(
                builder.createUnaryOp(spv::OpBitcast, type_int,
                                      builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed,
                                                          builder.makeUintConstant(UINT8_MAX))),
                output_fragment_stencil_ref);
          }
        }
      } break;
      case TransferOutput::kStencilBit: {
        if (packed) {
          REX_ASSERT(push_constants_member_stencil_mask != UINT32_MAX);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.makeIntConstant(static_cast<i32>(push_constants_member_stencil_mask)));
          spv::Id stencil_mask_constant =
              builder.createLoad(builder.createAccessChain(spv::StorageClassPushConstant,
                                                           push_constants, id_vector_temp),
                                 spv::NoPrecision);
          SPIRVBuilder::IfBuilder stencil_kill_if(
              builder.createBinOp(
                  spv::OpIEqual, type_bool,
                  builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed, stencil_mask_constant),
                  builder.makeUintConstant(0)),
              spv::SelectionControlMaskNone, builder);
          builder.createNoResultOp(spv::OpKill);
          // OpKill terminates the block.
          stencil_kill_if.MakeEndIf(false);
        }
      } break;
    }
  }

  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeOriginUpperLeft);
  if (output_fragment_depth != spv::NoResult) {
    builder.addExecutionMode(main_function, spv::ExecutionModeDepthReplacing);
  }
  if (output_fragment_stencil_ref != spv::NoResult) {
    builder.addExecutionMode(main_function, spv::ExecutionModeStencilRefReplacingEXT);
  }
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelFragment, main_function, "main");
  for (spv::Id interface_id : main_interface) {
    entry_point->addIdOperand(interface_id);
  }

  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);
  if (const std::string dump_path = command_processor_.GetXenosSettings().dump_shaders.Get();
      !dump_path.empty()) {
    gpu::vulkan::objects::DumpShader(dump_path, std::format("transfer_{:08X}", key.key),
                                     shader_code);
  }

  // The handle is stored even on failure, so creation is not retried.
  VkShaderModule shader_module = gpu::vulkan::objects::CreateShaderModule(
      vulkan_device, reinterpret_cast<const u32*>(shader_code.data()),
      sizeof(u32) * shader_code.size());
  if (shader_module == VK_NULL_HANDLE) {
    REXLOG_ERROR(gpu,
                 "VulkanRenderTargetCache: Failed to create the render target ownership "
                 "transfer shader 0x{:08X}",
                 key.key);
  }
  transfer_shaders_.emplace(key, shader_module);
  return shader_module;
}

VkPipeline const* VulkanRenderTargetCache::GetTransferPipelines(TransferPipelineKey key) {
  auto pipeline_it = transfer_pipelines_.find(key);
  if (pipeline_it != transfer_pipelines_.end()) {
    return pipeline_it->second[0] != VK_NULL_HANDLE ? pipeline_it->second.data() : nullptr;
  }

  const TransferModeInfo& mode = kTransferModes[static_cast<size_t>(key.shader_key.mode)];

  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  const gpu::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->GetProperties();
  bool use_dynamic_rendering = command_processor_.GetVulkanSettings().dynamic_rendering.Get() &&
                               device_properties.dynamicRendering;

  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (!use_dynamic_rendering) {
    render_pass = GetHostRenderTargetsRenderPass(key.render_pass_key);
    if (render_pass == VK_NULL_HANDLE) {
      transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
      return nullptr;
    }
  }

  VkShaderModule fragment_shader_module = GetTransferShader(key.shader_key);
  if (fragment_shader_module == VK_NULL_HANDLE) {
    transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
    return nullptr;
  }

  u32 dest_sample_count = static_cast<u32>(1) << static_cast<u32>(key.shader_key.dest_msaa_samples);
  bool dest_is_masked_sample = dest_sample_count > 1 && !device_properties.sampleRateShading;

  VkPipelineShaderStageCreateInfo shader_stages[2];
  shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[0].pNext = nullptr;
  shader_stages[0].flags = 0;
  shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shader_stages[0].module = transfer_passthrough_vertex_shader_;
  shader_stages[0].pName = "main";
  shader_stages[0].pSpecializationInfo = nullptr;
  shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[1].pNext = nullptr;
  shader_stages[1].flags = 0;
  shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shader_stages[1].module = fragment_shader_module;
  shader_stages[1].pName = "main";
  shader_stages[1].pSpecializationInfo = nullptr;
  VkSpecializationMapEntry sample_id_specialization_map_entry;
  u32 sample_id_specialization_constant;
  VkSpecializationInfo sample_id_specialization_info;
  if (dest_is_masked_sample) {
    sample_id_specialization_map_entry.constantID = 0;
    sample_id_specialization_map_entry.offset = 0;
    sample_id_specialization_map_entry.size = sizeof(u32);
    sample_id_specialization_constant = 0;
    sample_id_specialization_info.mapEntryCount = 1;
    sample_id_specialization_info.pMapEntries = &sample_id_specialization_map_entry;
    sample_id_specialization_info.dataSize = sizeof(sample_id_specialization_constant);
    sample_id_specialization_info.pData = &sample_id_specialization_constant;
    shader_stages[1].pSpecializationInfo = &sample_id_specialization_info;
  }

  VkVertexInputBindingDescription vertex_input_binding;
  vertex_input_binding.binding = 0;
  vertex_input_binding.stride = sizeof(f32) * 2;
  vertex_input_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription vertex_input_attribute;
  vertex_input_attribute.location = 0;
  vertex_input_attribute.binding = 0;
  vertex_input_attribute.format = VK_FORMAT_R32G32_SFLOAT;
  vertex_input_attribute.offset = 0;
  VkPipelineVertexInputStateCreateInfo vertex_input_state;
  vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input_state.pNext = nullptr;
  vertex_input_state.flags = 0;
  vertex_input_state.vertexBindingDescriptionCount = 1;
  vertex_input_state.pVertexBindingDescriptions = &vertex_input_binding;
  vertex_input_state.vertexAttributeDescriptionCount = 1;
  vertex_input_state.pVertexAttributeDescriptions = &vertex_input_attribute;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
  input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.pNext = nullptr;
  input_assembly_state.flags = 0;
  input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  input_assembly_state.primitiveRestartEnable = VK_FALSE;

  // Dynamic viewport stays within maxViewportDimensions and keeps the NDC factor a power of two.
  VkPipelineViewportStateCreateInfo viewport_state;
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.flags = 0;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = nullptr;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization_state.lineWidth = 1.0f;

  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state =
      objects::BuildMultisampleState((dest_sample_count == 2 && !msaa_2x_attachments_supported_)
                                         ? VK_SAMPLE_COUNT_4_BIT
                                         : static_cast<VkSampleCountFlagBits>(dest_sample_count));
  if (dest_sample_count > 1) {
    if (device_properties.sampleRateShading) {
      multisample_state.sampleShadingEnable = VK_TRUE;
      multisample_state.minSampleShading = 1.0f;
      if (dest_sample_count == 2 && !msaa_2x_attachments_supported_) {
        // 2x is emulated as samples 0 and 3 of 4x when native 2x is unsupported.
        sample_mask = 0b1001;
      }
    } else {
      sample_mask = 0b1;
    }
    if (sample_mask != UINT32_MAX) {
      multisample_state.pSampleMask = &sample_mask;
    }
  }

  // Valid depth and stencil state is set up unconditionally, as attachments depend on the pass.
  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  if (mode.output == TransferOutput::kDepth) {
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_TRUE;
    depth_stencil_state.depthCompareOp =
        command_processor_.GetXenosSettings().depth_transfer_not_equal_test.Get()
            ? VK_COMPARE_OP_NOT_EQUAL
            : VK_COMPARE_OP_ALWAYS;
  }
  if ((mode.output == TransferOutput::kDepth &&
       vulkan_device->GetExtensions().ext_EXT_shader_stencil_export) ||
      mode.output == TransferOutput::kStencilBit) {
    depth_stencil_state.stencilTestEnable = VK_TRUE;
    depth_stencil_state.front.failOp = VK_STENCIL_OP_KEEP;
    depth_stencil_state.front.passOp = VK_STENCIL_OP_REPLACE;
    depth_stencil_state.front.depthFailOp = VK_STENCIL_OP_REPLACE;
    // ALWAYS rather than NOT_EQUAL, so depth writing is unaffected by a differing stencil.
    depth_stencil_state.front.compareOp = VK_COMPARE_OP_ALWAYS;
    depth_stencil_state.front.writeMask = UINT8_MAX;
    depth_stencil_state.front.reference = UINT8_MAX;
    depth_stencil_state.back = depth_stencil_state.front;
  }

  // Valid color blend state is set up unconditionally, as attachments depend on the pass.
  VkPipelineColorBlendAttachmentState color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend_state.attachmentCount =
      32 - rex::lzcnt(key.render_pass_key.depth_and_color_used >> 1);
  color_blend_state.pAttachments = color_blend_attachments;
  if (mode.output == TransferOutput::kColor) {
    REX_ASSERT(device_properties.independentBlend);
    color_blend_attachments[key.shader_key.dest_color_rt_index].colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
  }

  VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {};
  VkFormat color_attachment_format = VK_FORMAT_UNDEFINED;
  VkFormat depth_attachment_format = VK_FORMAT_UNDEFINED;
  VkFormat stencil_attachment_format = VK_FORMAT_UNDEFINED;
  if (use_dynamic_rendering) {
    pipeline_rendering_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipeline_rendering_create_info.pNext = nullptr;
    pipeline_rendering_create_info.viewMask = 0;
    if (key.render_pass_key.depth_and_color_used & 0b1) {
      depth_attachment_format = GetDepthVulkanFormat(key.render_pass_key.depth_format);
      stencil_attachment_format = depth_attachment_format;
      pipeline_rendering_create_info.colorAttachmentCount = 0;
      pipeline_rendering_create_info.pColorAttachmentFormats = nullptr;
    } else {
      color_attachment_format =
          GetColorOwnershipTransferVulkanFormat(key.render_pass_key.color_0_view_format);
      pipeline_rendering_create_info.colorAttachmentCount = 1;
      pipeline_rendering_create_info.pColorAttachmentFormats = &color_attachment_format;
    }
    pipeline_rendering_create_info.depthAttachmentFormat = depth_attachment_format;
    pipeline_rendering_create_info.stencilAttachmentFormat = stencil_attachment_format;
  }

  std::array<VkDynamicState, 3> dynamic_states;
  VkPipelineDynamicStateCreateInfo dynamic_state;
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.pNext = nullptr;
  dynamic_state.flags = 0;
  dynamic_state.dynamicStateCount = 0;
  dynamic_state.pDynamicStates = dynamic_states.data();
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
  if (mode.output == TransferOutput::kStencilBit) {
    dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
  }

  std::array<VkPipeline, 4> pipelines{};
  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = use_dynamic_rendering ? &pipeline_rendering_create_info : nullptr;
  pipeline_create_info.flags = 0;
  if (dest_is_masked_sample) {
    pipeline_create_info.flags |= VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
  }
  pipeline_create_info.stageCount = static_cast<u32>(rex::CountOf(shader_stages));
  pipeline_create_info.pStages = shader_stages;
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState = nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = &depth_stencil_state;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout =
      transfer_pipeline_layouts_[static_cast<size_t>(mode.pipeline_layout)];
  pipeline_create_info.renderPass = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                    &pipelines[0]) != VK_SUCCESS) {
    REXLOG_ERROR(gpu,
                 "VulkanRenderTargetCache: Failed to create the render target ownership "
                 "transfer pipeline for render pass 0x{:08X}, shader 0x{:08X}",
                 key.render_pass_key.key, key.shader_key.key);
    transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
    return nullptr;
  }
  if (dest_is_masked_sample) {
    REX_ASSERT(multisample_state.pSampleMask == &sample_mask);
    pipeline_create_info.flags =
        (pipeline_create_info.flags & ~VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) |
        VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    pipeline_create_info.basePipelineHandle = pipelines[0];
    for (u32 i = 1; i < dest_sample_count; ++i) {
      // 2x is emulated as samples 0 and 3 of 4x when native 2x is unsupported.
      u32 host_sample_index =
          (dest_sample_count == 2 && !msaa_2x_attachments_supported_ && i == 1) ? 3 : i;
      sample_id_specialization_constant = host_sample_index;
      sample_mask = static_cast<u32>(1) << host_sample_index;
      if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                        &pipelines[i]) != VK_SUCCESS) {
        REXLOG_ERROR(gpu,
                     "VulkanRenderTargetCache: Failed to create the render target "
                     "ownership transfer pipeline for render pass 0x{:08X}, shader "
                     "0x{:08X}, sample {}",
                     key.render_pass_key.key, key.shader_key.key, i);
        for (u32 j = 0; j < i; ++j) {
          dfn.vkDestroyPipeline(device, pipelines[j], nullptr);
        }
        transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
        return nullptr;
      }
    }
  }
  return transfer_pipelines_.emplace(key, pipelines).first->second.data();
}

void VulkanRenderTargetCache::PerformTransfersAndResolveClears(
    u32 render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const u64* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle) {
  REX_ASSERT(GetPath() == Path::kHostRenderTargets);

  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  u64 current_submission = command_processor_.GetCurrentSubmission();
  DeferredCommandBuffer& command_buffer = command_processor_.GetDeferredCommandBuffer();

  bool resolve_clear_needed = render_target_resolve_clear_values && resolve_clear_rectangle;
  VkClearRect resolve_clear_rect;
  if (resolve_clear_needed) {
    resolve_clear_rect.rect.offset.x =
        static_cast<i32>(resolve_clear_rectangle->x_pixels * GetDrawResolutionScaleX());
    resolve_clear_rect.rect.offset.y =
        static_cast<i32>(resolve_clear_rectangle->y_pixels * GetDrawResolutionScaleY());
    resolve_clear_rect.rect.extent.width =
        resolve_clear_rectangle->width_pixels * GetDrawResolutionScaleX();
    resolve_clear_rect.rect.extent.height =
        resolve_clear_rectangle->height_pixels * GetDrawResolutionScaleY();
    resolve_clear_rect.baseArrayLayer = 0;
    resolve_clear_rect.layerCount = 1;
  }

  // Stores host depth when the only depth destination is also its own host depth source.
  bool host_depth_store_set_up = false;
  for (u32 i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_vulkan_rt.GetKey();
    if (!dest_rt_key.is_depth) {
      continue;
    }
    const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
    for (const Transfer& transfer : depth_transfers) {
      if (transfer.host_depth_source != dest_rt) {
        continue;
      }
      if (!host_depth_store_set_up) {
        command_processor_.BindExternalComputePipeline(
            host_depth_store_pipelines_[static_cast<size_t>(dest_rt_key.msaa_samples)]);
        VkDescriptorSet host_depth_store_descriptor_sets[] = {
            edram_storage_buffer_descriptor_set_,
            dest_vulkan_rt.GetDescriptorSetTransferSource(),
        };
        command_buffer.CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_COMPUTE, host_depth_store_pipeline_layout_, 0,
            static_cast<u32>(rex::CountOf(host_depth_store_descriptor_sets)),
            host_depth_store_descriptor_sets, 0, nullptr);
        HostDepthStoreRenderTargetConstant host_depth_store_render_target_constant =
            GetHostDepthStoreRenderTargetConstant(dest_rt_key.pitch_tiles_at_32bpp,
                                                  msaa_2x_attachments_supported_);
        command_buffer.CmdVkPushConstants(
            host_depth_store_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            static_cast<u32>(offsetof(HostDepthStoreConstants, render_target)),
            sizeof(host_depth_store_render_target_constant),
            &host_depth_store_render_target_constant);
        // Barriers stay separate as the shader read to attachment write one is needed anyway.
        UseEDRAMBuffer(EDRAMBufferUsage::kComputeWrite);
        // Depth and stencil always transition together, as usage flags are not stored separately.
        command_processor_.PushImageMemoryBarrier(
            dest_vulkan_rt.GetImage(),
            gpu::vulkan::objects::BuildSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                        VK_IMAGE_ASPECT_STENCIL_BIT),
            dest_vulkan_rt.GetCurrentStageMask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            dest_vulkan_rt.GetCurrentAccessMask(), VK_ACCESS_SHADER_READ_BIT,
            dest_vulkan_rt.GetCurrentLayout(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dest_vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
        GetHostDepthStoreRectangleDispatch(transfer_rectangles[j], dest_rt_key.msaa_samples,
                                           host_depth_store_rectangle_constant, group_count_x,
                                           group_count_y);
        command_buffer.CmdVkPushConstants(
            host_depth_store_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            static_cast<u32>(offsetof(HostDepthStoreConstants, rectangle)),
            sizeof(host_depth_store_rectangle_constant), &host_depth_store_rectangle_constant);
        command_processor_.SubmitBarriers(true);
        command_buffer.CmdVkDispatch(group_count_x, group_count_y, 1);
        MarkEDRAMBufferModified();
      }
    }
    break;
  }

  constexpr VkPipelineStageFlags kSourceStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags kSourceAccessMask = VK_ACCESS_SHADER_READ_BIT;
  constexpr VkImageLayout kSourceLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  // Barriers are merged here, using the destination state when its use precedes the source use.
  for (u32 i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    const std::vector<Transfer>& dest_transfers = render_target_transfers[i];
    if (!resolve_clear_needed && dest_transfers.empty()) {
      continue;
    }
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
      auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
      VkPipelineStageFlags dest_dst_stage_mask;
      VkAccessFlags dest_dst_access_mask;
      VkImageLayout dest_new_layout;
      dest_vulkan_rt.GetDrawUsage(&dest_dst_stage_mask, &dest_dst_access_mask, &dest_new_layout);
      command_processor_.PushImageMemoryBarrier(
          dest_vulkan_rt.GetImage(),
          gpu::vulkan::objects::BuildSubresourceRange(
              dest_vulkan_rt.GetKey().is_depth
                  ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                  : VK_IMAGE_ASPECT_COLOR_BIT),
          dest_vulkan_rt.GetCurrentStageMask(), dest_dst_stage_mask,
          dest_vulkan_rt.GetCurrentAccessMask(), dest_dst_access_mask,
          dest_vulkan_rt.GetCurrentLayout(), dest_new_layout);
      dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask, dest_new_layout);
    }
    for (const Transfer& transfer : dest_transfers) {
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
        auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
        command_processor_.PushImageMemoryBarrier(
            source_vulkan_rt.GetImage(),
            gpu::vulkan::objects::BuildSubresourceRange(
                source_vulkan_rt.GetKey().is_depth
                    ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                    : VK_IMAGE_ASPECT_COLOR_BIT),
            source_vulkan_rt.GetCurrentStageMask(), kSourceStageMask,
            source_vulkan_rt.GetCurrentAccessMask(), kSourceAccessMask,
            source_vulkan_rt.GetCurrentLayout(), kSourceLayout);
        source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
      }
      // host_depth_source == dest_rt reads the EDRAM buffer instead, so no transition is needed.
      if (transfer.host_depth_source && transfer.host_depth_source != dest_rt &&
          !host_depth_source_previously_used_as_dest) {
        auto& host_depth_source_vulkan_rt =
            *static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
        command_processor_.PushImageMemoryBarrier(
            host_depth_source_vulkan_rt.GetImage(),
            gpu::vulkan::objects::BuildSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                        VK_IMAGE_ASPECT_STENCIL_BIT),
            host_depth_source_vulkan_rt.GetCurrentStageMask(), kSourceStageMask,
            host_depth_source_vulkan_rt.GetCurrentAccessMask(), kSourceAccessMask,
            host_depth_source_vulkan_rt.GetCurrentLayout(), kSourceLayout);
        host_depth_source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
      }
    }
  }
  if (host_depth_store_set_up) {
    UseEDRAMBuffer(EDRAMBufferUsage::kFragmentRead);
  }

  TransferPipelineLayoutIndex last_transfer_pipeline_layout_index =
      TransferPipelineLayoutIndex::kCount;
  u32 transfer_descriptor_sets_bound = 0;
  u32 transfer_push_constants_set = 0;
  VkDescriptorSet last_descriptor_set_host_depth_stencil_textures = VK_NULL_HANDLE;
  VkDescriptorSet last_descriptor_set_depth_stencil_textures = VK_NULL_HANDLE;
  VkDescriptorSet last_descriptor_set_color_texture = VK_NULL_HANDLE;
  TransferAddressConstant last_host_depth_address_constant;
  TransferAddressConstant last_address_constant;

  for (u32 i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& current_transfers = render_target_transfers[i];
    if (current_transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_vulkan_rt.GetKey();

    // Barriers for cross copies that could not be merged earlier.
    {
      VkPipelineStageFlags dest_dst_stage_mask;
      VkAccessFlags dest_dst_access_mask;
      VkImageLayout dest_new_layout;
      dest_vulkan_rt.GetDrawUsage(&dest_dst_stage_mask, &dest_dst_access_mask, &dest_new_layout);
      command_processor_.PushImageMemoryBarrier(
          dest_vulkan_rt.GetImage(),
          gpu::vulkan::objects::BuildSubresourceRange(
              dest_rt_key.is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                   : VK_IMAGE_ASPECT_COLOR_BIT),
          dest_vulkan_rt.GetCurrentStageMask(), dest_dst_stage_mask,
          dest_vulkan_rt.GetCurrentAccessMask(), dest_dst_access_mask,
          dest_vulkan_rt.GetCurrentLayout(), dest_new_layout);
      dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask, dest_new_layout);
    }

    RenderPassKey transfer_render_pass_key;
    transfer_render_pass_key.msaa_samples = dest_rt_key.msaa_samples;
    if (dest_rt_key.is_depth) {
      transfer_render_pass_key.depth_and_color_used = 0b1;
      transfer_render_pass_key.depth_format = dest_rt_key.GetDepthFormat();
    } else {
      transfer_render_pass_key.depth_and_color_used = 0b1 << 1;
      transfer_render_pass_key.color_0_view_format = dest_rt_key.GetColorFormat();
      transfer_render_pass_key.color_rts_use_transfer_formats = 1;
    }
    VkRenderPass transfer_render_pass = GetHostRenderTargetsRenderPass(transfer_render_pass_key);
    if (transfer_render_pass == VK_NULL_HANDLE) {
      continue;
    }
    const RenderTarget* transfer_framebuffer_render_targets[1 + xenos::kMaxColorRenderTargets] = {};
    transfer_framebuffer_render_targets[dest_rt_key.is_depth ? 0 : 1] = dest_rt;
    const Framebuffer* transfer_framebuffer =
        GetHostRenderTargetsFramebuffer(transfer_render_pass_key, dest_rt_key.pitch_tiles_at_32bpp,
                                        transfer_framebuffer_render_targets);
    if (!transfer_framebuffer) {
      continue;
    }
    // The render pass is entered later, as source barriers may be inserted.
    VkImageView transfer_dest_view = dest_rt_key.is_depth ? dest_vulkan_rt.GetViewDepthStencil()
                                                          : dest_vulkan_rt.GetViewColorTransfer();

    if (!current_transfers.empty()) {
      u32 dest_pitch_tiles = dest_rt_key.GetPitchTiles();
      bool dest_is_64bpp = dest_rt_key.Is64bpp();

      bool need_stencil_bit_draws =
          dest_rt_key.is_depth && !vulkan_device->GetExtensions().ext_EXT_shader_stencil_export;
      current_transfer_invocations_.clear();
      current_transfer_invocations_.reserve(current_transfers.size()
                                            << static_cast<u32>(need_stencil_bit_draws));
      u32 rt_sort_index = 0;
      TransferShaderKey new_transfer_shader_key;
      new_transfer_shader_key.dest_msaa_samples = dest_rt_key.msaa_samples;
      new_transfer_shader_key.dest_resource_format = dest_rt_key.resource_format;
      u32 stencil_clear_rectangle_count = 0;
      for (u32 j = 0; j <= static_cast<u32>(need_stencil_bit_draws); ++j) {
        // j is 0 for color or depth and 1 for stencil bits (own pipeline layout, no host depth).
        for (const Transfer& transfer : current_transfers) {
          auto host_depth_source_vulkan_rt =
              static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_vulkan_rt) {
            host_depth_source_vulkan_rt->SetTemporarySortIndex(UINT32_MAX);
          }
          REX_ASSERT(transfer.source != nullptr);
          auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
          source_vulkan_rt.SetTemporarySortIndex(UINT32_MAX);
        }
        for (const Transfer& transfer : current_transfers) {
          REX_ASSERT(transfer.source != nullptr);
          auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
          VulkanRenderTarget* host_depth_source_vulkan_rt =
              j ? nullptr : static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_vulkan_rt &&
              host_depth_source_vulkan_rt->GetTemporarySortIndex() == UINT32_MAX) {
            host_depth_source_vulkan_rt->SetTemporarySortIndex(rt_sort_index++);
          }
          if (source_vulkan_rt.GetTemporarySortIndex() == UINT32_MAX) {
            source_vulkan_rt.SetTemporarySortIndex(rt_sort_index++);
          }
          RenderTargetKey source_rt_key = source_vulkan_rt.GetKey();
          new_transfer_shader_key.source_msaa_samples = source_rt_key.msaa_samples;
          new_transfer_shader_key.source_resource_format = source_rt_key.resource_format;
          bool host_depth_source_is_copy = host_depth_source_vulkan_rt == &dest_vulkan_rt;
          // The host depth copy buffer has only raw samples.
          new_transfer_shader_key.host_depth_source_msaa_samples =
              (host_depth_source_vulkan_rt && !host_depth_source_is_copy)
                  ? host_depth_source_vulkan_rt->GetKey().msaa_samples
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
              if (host_depth_source_vulkan_rt) {
                if (host_depth_source_is_copy) {
                  new_transfer_shader_key.mode = source_rt_key.is_depth
                                                     ? TransferMode::kDepthAndHostDepthCopyToDepth
                                                     : TransferMode::kColorAndHostDepthCopyToDepth;
                } else {
                  new_transfer_shader_key.mode = source_rt_key.is_depth
                                                     ? TransferMode::kDepthAndHostDepthToDepth
                                                     : TransferMode::kColorAndHostDepthToDepth;
                }
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

      for (auto it = current_transfer_invocations_.cbegin();
           it != current_transfer_invocations_.cend(); ++it) {
        REX_ASSERT(it->transfer.source != nullptr);
        auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(it->transfer.source);
        command_processor_.PushImageMemoryBarrier(
            source_vulkan_rt.GetImage(),
            gpu::vulkan::objects::BuildSubresourceRange(
                source_vulkan_rt.GetKey().is_depth
                    ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                    : VK_IMAGE_ASPECT_COLOR_BIT),
            source_vulkan_rt.GetCurrentStageMask(), kSourceStageMask,
            source_vulkan_rt.GetCurrentAccessMask(), kSourceAccessMask,
            source_vulkan_rt.GetCurrentLayout(), kSourceLayout);
        source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
        auto host_depth_source_vulkan_rt =
            static_cast<VulkanRenderTarget*>(it->transfer.host_depth_source);
        if (host_depth_source_vulkan_rt) {
          TransferShaderKey transfer_shader_key = it->shader_key;
          if (transfer_shader_key.mode == TransferMode::kDepthAndHostDepthCopyToDepth ||
              transfer_shader_key.mode == TransferMode::kColorAndHostDepthCopyToDepth) {
            UseEDRAMBuffer(EDRAMBufferUsage::kFragmentRead);
          } else {
            command_processor_.PushImageMemoryBarrier(
                host_depth_source_vulkan_rt->GetImage(),
                gpu::vulkan::objects::BuildSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                            VK_IMAGE_ASPECT_STENCIL_BIT),
                host_depth_source_vulkan_rt->GetCurrentStageMask(), kSourceStageMask,
                host_depth_source_vulkan_rt->GetCurrentAccessMask(), kSourceAccessMask,
                host_depth_source_vulkan_rt->GetCurrentLayout(), kSourceLayout);
            host_depth_source_vulkan_rt->SetUsage(kSourceStageMask, kSourceAccessMask,
                                                  kSourceLayout);
          }
        }
      }

      command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
          transfer_render_pass, transfer_framebuffer, transfer_dest_view, dest_rt_key.is_depth);

      if (stencil_clear_rectangle_count) {
        VkClearAttachment* stencil_clear_attachment;
        VkClearRect* stencil_clear_rect_write_ptr;
        command_buffer.CmdVkClearAttachmentsEmplace(1, stencil_clear_attachment,
                                                    stencil_clear_rectangle_count,
                                                    stencil_clear_rect_write_ptr);
        stencil_clear_attachment->aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        stencil_clear_attachment->colorAttachment = 0;
        stencil_clear_attachment->clearValue.depthStencil.depth = 0.0f;
        stencil_clear_attachment->clearValue.depthStencil.stencil = 0;
        for (const Transfer& transfer : current_transfers) {
          Transfer::Rectangle transfer_stencil_clear_rectangles[Transfer::kMaxRectanglesWithCutout];
          u32 transfer_stencil_clear_rectangle_count = transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_stencil_clear_rectangles, resolve_clear_rectangle);
          for (u32 j = 0; j < transfer_stencil_clear_rectangle_count; ++j) {
            const Transfer::Rectangle& stencil_clear_rectangle =
                transfer_stencil_clear_rectangles[j];
            stencil_clear_rect_write_ptr->rect.offset.x =
                static_cast<i32>(stencil_clear_rectangle.x_pixels * GetDrawResolutionScaleX());
            stencil_clear_rect_write_ptr->rect.offset.y =
                static_cast<i32>(stencil_clear_rectangle.y_pixels * GetDrawResolutionScaleY());
            stencil_clear_rect_write_ptr->rect.extent.width =
                stencil_clear_rectangle.width_pixels * GetDrawResolutionScaleX();
            stencil_clear_rect_write_ptr->rect.extent.height =
                stencil_clear_rectangle.height_pixels * GetDrawResolutionScaleY();
            stencil_clear_rect_write_ptr->baseArrayLayer = 0;
            stencil_clear_rect_write_ptr->layerCount = 1;
            ++stencil_clear_rect_write_ptr;
          }
        }
      }

      // Power of two viewports allow exact division by biasing the exponent.
      VkViewport transfer_viewport;
      transfer_viewport.x = 0.0f;
      transfer_viewport.y = 0.0f;
      transfer_viewport.width =
          static_cast<f32>(std::min(rex::next_pow2(transfer_framebuffer->host_extent.width),
                                    vulkan_device->GetProperties().maxViewportDimensions[0]));
      transfer_viewport.height =
          static_cast<f32>(std::min(rex::next_pow2(transfer_framebuffer->host_extent.height),
                                    vulkan_device->GetProperties().maxViewportDimensions[1]));
      transfer_viewport.minDepth = 0.0f;
      transfer_viewport.maxDepth = 1.0f;
      command_processor_.SetViewport(transfer_viewport);
      f32 pixels_to_ndc_x =
          (2.0f / transfer_viewport.width) * static_cast<f32>(GetDrawResolutionScaleX());
      f32 pixels_to_ndc_y =
          (2.0f / transfer_viewport.height) * static_cast<f32>(GetDrawResolutionScaleY());
      VkRect2D transfer_scissor;
      transfer_scissor.offset.x = 0;
      transfer_scissor.offset.y = 0;
      transfer_scissor.extent = transfer_framebuffer->host_extent;
      command_processor_.SetScissor(transfer_scissor);

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
        auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(it->transfer.source);
        auto host_depth_source_vulkan_rt =
            static_cast<VulkanRenderTarget*>(it->transfer.host_depth_source);
        TransferShaderKey transfer_shader_key = it->shader_key;
        const TransferModeInfo& transfer_mode_info =
            kTransferModes[static_cast<size_t>(transfer_shader_key.mode)];
        TransferPipelineLayoutIndex transfer_pipeline_layout_index =
            transfer_mode_info.pipeline_layout;
        const TransferPipelineLayoutInfo& transfer_pipeline_layout_info =
            kTransferPipelineLayoutInfos[static_cast<size_t>(transfer_pipeline_layout_index)];
        u32 transfer_sample_pipeline_count =
            vulkan_device->GetProperties().sampleRateShading
                ? 1
                : static_cast<u32>(1) << static_cast<u32>(dest_rt_key.msaa_samples);
        bool transfer_is_stencil_bit = (transfer_pipeline_layout_info.used_push_constant_dwords &
                                        kTransferUsedPushConstantDwordStencilMaskBit) != 0;

        u32 transfer_vertex_count = 6 * transfer_rectangle_count;
        VkBuffer transfer_vertex_buffer;
        VkDeviceSize transfer_vertex_buffer_offset;
        f32* transfer_rectangle_write_ptr =
            reinterpret_cast<f32*>(transfer_vertex_buffer_pool_->Request(
                current_submission, sizeof(f32) * 2 * transfer_vertex_count, sizeof(f32),
                transfer_vertex_buffer, transfer_vertex_buffer_offset));
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
                -1.0f + static_cast<f32>(transfer_rectangle.y_pixels) * pixels_to_ndc_y;
            f32 transfer_rectangle_x1 =
                transfer_rectangle_x0 +
                static_cast<f32>(transfer_rectangle.width_pixels) * pixels_to_ndc_x;
            f32 transfer_rectangle_y1 =
                transfer_rectangle_y0 +
                static_cast<f32>(transfer_rectangle.height_pixels) * pixels_to_ndc_y;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
          }
        }
        command_buffer.CmdVkBindVertexBuffers(0, 1, &transfer_vertex_buffer,
                                              &transfer_vertex_buffer_offset);

        const VkPipeline* transfer_pipelines = GetTransferPipelines(
            TransferPipelineKey(transfer_render_pass_key, transfer_shader_key));
        if (!transfer_pipelines) {
          continue;
        }
        command_processor_.BindExternalGraphicsPipeline(transfer_pipelines[0]);
        if (last_transfer_pipeline_layout_index != transfer_pipeline_layout_index) {
          last_transfer_pipeline_layout_index = transfer_pipeline_layout_index;
          transfer_descriptor_sets_bound = 0;
          transfer_push_constants_set = 0;
        }

        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
          REX_ASSERT(host_depth_source_vulkan_rt != nullptr);
          VkDescriptorSet descriptor_set_host_depth_stencil_textures =
              host_depth_source_vulkan_rt->GetDescriptorSetTransferSource();
          if (last_descriptor_set_host_depth_stencil_textures !=
              descriptor_set_host_depth_stencil_textures) {
            last_descriptor_set_host_depth_stencil_textures =
                descriptor_set_host_depth_stencil_textures;
            transfer_descriptor_sets_bound &=
                ~kTransferUsedDescriptorSetHostDepthStencilTexturesBit;
          }
        }
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetDepthStencilTexturesBit) {
          VkDescriptorSet descriptor_set_depth_stencil_textures =
              source_vulkan_rt.GetDescriptorSetTransferSource();
          if (last_descriptor_set_depth_stencil_textures != descriptor_set_depth_stencil_textures) {
            last_descriptor_set_depth_stencil_textures = descriptor_set_depth_stencil_textures;
            transfer_descriptor_sets_bound &= ~kTransferUsedDescriptorSetDepthStencilTexturesBit;
          }
        }
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetColorTextureBit) {
          VkDescriptorSet descriptor_set_color_texture =
              source_vulkan_rt.GetDescriptorSetTransferSource();
          if (last_descriptor_set_color_texture != descriptor_set_color_texture) {
            last_descriptor_set_color_texture = descriptor_set_color_texture;
            transfer_descriptor_sets_bound &= ~kTransferUsedDescriptorSetColorTextureBit;
          }
        }
        if (transfer_pipeline_layout_info.used_push_constant_dwords &
            kTransferUsedPushConstantDwordHostDepthAddressBit) {
          REX_ASSERT(host_depth_source_vulkan_rt != nullptr);
          RenderTargetKey host_depth_source_rt_key = host_depth_source_vulkan_rt->GetKey();
          TransferAddressConstant host_depth_address_constant;
          host_depth_address_constant.dest_pitch = dest_pitch_tiles;
          host_depth_address_constant.source_pitch = host_depth_source_rt_key.GetPitchTiles();
          host_depth_address_constant.source_to_dest =
              static_cast<i32>(dest_rt_key.base_tiles) -
              static_cast<i32>(host_depth_source_rt_key.base_tiles);
          if (last_host_depth_address_constant != host_depth_address_constant) {
            last_host_depth_address_constant = host_depth_address_constant;
            transfer_push_constants_set &= ~kTransferUsedPushConstantDwordHostDepthAddressBit;
          }
        }
        if (transfer_pipeline_layout_info.used_push_constant_dwords &
            kTransferUsedPushConstantDwordAddressBit) {
          RenderTargetKey source_rt_key = source_vulkan_rt.GetKey();
          TransferAddressConstant address_constant;
          address_constant.dest_pitch = dest_pitch_tiles;
          address_constant.source_pitch = source_rt_key.GetPitchTiles();
          address_constant.source_to_dest =
              static_cast<i32>(dest_rt_key.base_tiles) - static_cast<i32>(source_rt_key.base_tiles);
          if (last_address_constant != address_constant) {
            last_address_constant = address_constant;
            transfer_push_constants_set &= ~kTransferUsedPushConstantDwordAddressBit;
          }
        }

        VkPipelineLayout transfer_pipeline_layout =
            transfer_pipeline_layouts_[static_cast<size_t>(transfer_pipeline_layout_index)];
        u32 transfer_descriptor_sets_unbound =
            transfer_pipeline_layout_info.used_descriptor_sets & ~transfer_descriptor_sets_bound;
        if (transfer_descriptor_sets_unbound & kTransferUsedDescriptorSetHostDepthBufferBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::BitCount(transfer_pipeline_layout_info.used_descriptor_sets &
                            (kTransferUsedDescriptorSetHostDepthBufferBit - 1)),
              1, &edram_storage_buffer_descriptor_set_, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetHostDepthBufferBit;
        }
        if (transfer_descriptor_sets_unbound &
            kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::BitCount(transfer_pipeline_layout_info.used_descriptor_sets &
                            (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)),
              1, &last_descriptor_set_host_depth_stencil_textures, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetHostDepthStencilTexturesBit;
        }
        if (transfer_descriptor_sets_unbound & kTransferUsedDescriptorSetDepthStencilTexturesBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::BitCount(transfer_pipeline_layout_info.used_descriptor_sets &
                            (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1)),
              1, &last_descriptor_set_depth_stencil_textures, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetDepthStencilTexturesBit;
        }
        if (transfer_descriptor_sets_unbound & kTransferUsedDescriptorSetColorTextureBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::BitCount(transfer_pipeline_layout_info.used_descriptor_sets &
                            (kTransferUsedDescriptorSetColorTextureBit - 1)),
              1, &last_descriptor_set_color_texture, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetColorTextureBit;
        }
        u32 transfer_push_constants_unset =
            transfer_pipeline_layout_info.used_push_constant_dwords & ~transfer_push_constants_set;
        if (transfer_push_constants_unset & kTransferUsedPushConstantDwordHostDepthAddressBit) {
          command_buffer.CmdVkPushConstants(
              transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
              sizeof(u32) * rex::BitCount(transfer_pipeline_layout_info.used_push_constant_dwords &
                                          (kTransferUsedPushConstantDwordHostDepthAddressBit - 1)),
              sizeof(u32), &last_host_depth_address_constant);
          transfer_push_constants_set |= kTransferUsedPushConstantDwordHostDepthAddressBit;
        }
        if (transfer_push_constants_unset & kTransferUsedPushConstantDwordAddressBit) {
          command_buffer.CmdVkPushConstants(
              transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
              sizeof(u32) * rex::BitCount(transfer_pipeline_layout_info.used_push_constant_dwords &
                                          (kTransferUsedPushConstantDwordAddressBit - 1)),
              sizeof(u32), &last_address_constant);
          transfer_push_constants_set |= kTransferUsedPushConstantDwordAddressBit;
        }

        for (u32 j = 0; j < transfer_sample_pipeline_count; ++j) {
          if (j) {
            command_processor_.BindExternalGraphicsPipeline(transfer_pipelines[j]);
          }
          for (u32 k = 0; k < static_cast<u32>(transfer_is_stencil_bit ? 8 : 1); ++k) {
            if (transfer_is_stencil_bit) {
              u32 transfer_stencil_bit = static_cast<u32>(1) << k;
              command_buffer.CmdVkPushConstants(
                  transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                  sizeof(u32) *
                      rex::BitCount(transfer_pipeline_layout_info.used_push_constant_dwords &
                                    (kTransferUsedPushConstantDwordStencilMaskBit - 1)),
                  sizeof(u32), &transfer_stencil_bit);
              command_buffer.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                      transfer_stencil_bit);
            }
            command_buffer.CmdVkDraw(transfer_vertex_count, 1, 0, 0);
          }
        }
      }
    }

    if (resolve_clear_needed) {
      command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
          transfer_render_pass, transfer_framebuffer, transfer_dest_view, dest_rt_key.is_depth);
      VkClearAttachment resolve_clear_attachment;
      resolve_clear_attachment.colorAttachment = 0;
      resolve_clear_attachment.clearValue = {};
      u64 clear_value = render_target_resolve_clear_values[i];
      if (dest_rt_key.is_depth) {
        resolve_clear_attachment.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        u32 depth_guest_clear_value = (static_cast<u32>(clear_value) >> 8) & 0xFFFFFF;
        switch (dest_rt_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            resolve_clear_attachment.clearValue.depthStencil.depth =
                xenos::UNorm24To32(depth_guest_clear_value);
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            // Accounts for the remapping of [0, 2) to [0, 1).
            resolve_clear_attachment.clearValue.depthStencil.depth =
                xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            break;
        }
        resolve_clear_attachment.clearValue.depthStencil.stencil =
            static_cast<u32>(clear_value) & 0xFF;
      } else {
        resolve_clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bool dest_color_transfer_is_uint = false;
        GetColorOwnershipTransferVulkanFormat(
            dest_rt_key.GetColorFormat(), dest_rt_key.msaa_samples, &dest_color_transfer_is_uint);
        switch (dest_rt_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8:
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            for (u32 j = 0; j < 4; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  static_cast<f32>((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            if (dest_rt_key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              // 8_8_8_8_GAMMA is stored as linear in R16G16B16A16_UNORM.
              for (u32 j = 0; j < 3; ++j) {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    xenos::PWLGammaToLinear(resolve_clear_attachment.clearValue.color.float32[j]);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (u32 j = 0; j < 3; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  static_cast<f32>((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            resolve_clear_attachment.clearValue.color.float32[3] =
                static_cast<f32>((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (u32 j = 0; j < 3; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            resolve_clear_attachment.clearValue.color.float32[3] =
                static_cast<f32>((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            // Ignores the -32...32 vs -1...1 setting, like color clears via depth aliasing.
            for (u32 j = 0; j < 2; ++j) {
              u16 component = static_cast<u16>((clear_value >> (j * 16)) & 0xFFFF);
              if (dest_color_transfer_is_uint) {
                resolve_clear_attachment.clearValue.color.uint32[j] = component;
              } else if (IsColor16FormatFloatLike(dest_rt_key.GetColorFormat())) {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    rex::xenos::HalfToFloat(component);
              } else {
                resolve_clear_attachment.clearValue.color.float32[j] = std::max(
                    static_cast<f32>(static_cast<i16>(component)) * (1.0f / 32767.0f), -1.0f);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            // Ignores the -32...32 vs -1...1 setting, like color clears via depth aliasing.
            for (u32 j = 0; j < 4; ++j) {
              u16 component = static_cast<u16>((clear_value >> (j * 16)) & 0xFFFF);
              if (dest_color_transfer_is_uint) {
                resolve_clear_attachment.clearValue.color.uint32[j] = component;
              } else if (IsColor16FormatFloatLike(dest_rt_key.GetColorFormat())) {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    rex::xenos::HalfToFloat(component);
              } else {
                resolve_clear_attachment.clearValue.color.float32[j] = std::max(
                    static_cast<f32>(static_cast<i16>(component)) * (1.0f / 32767.0f), -1.0f);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            u32 component = static_cast<u32>(clear_value);
            if (dest_color_transfer_is_uint) {
              // Uses uint to keep denormals and NaNs.
              resolve_clear_attachment.clearValue.color.uint32[0] = component;
            } else {
              resolve_clear_attachment.clearValue.color.float32[0] = std::bit_cast<f32>(component);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            u32 component_0 = static_cast<u32>(clear_value);
            u32 component_1 = static_cast<u32>(clear_value >> 32);
            if (dest_color_transfer_is_uint) {
              // Uses uint to keep denormals and NaNs.
              resolve_clear_attachment.clearValue.color.uint32[0] = component_0;
              resolve_clear_attachment.clearValue.color.uint32[1] = component_1;
            } else {
              resolve_clear_attachment.clearValue.color.float32[0] =
                  std::bit_cast<f32>(component_0);
              resolve_clear_attachment.clearValue.color.float32[1] =
                  std::bit_cast<f32>(component_1);
            }
          } break;
        }
      }
      command_buffer.CmdVkClearAttachments(1, &resolve_clear_attachment, 1, &resolve_clear_rect);
    }
  }
}

VkPipeline VulkanRenderTargetCache::GetDumpPipeline(DumpPipelineKey key) {
  auto pipeline_it = dump_pipelines_.find(key);
  if (pipeline_it != dump_pipelines_.end()) {
    return pipeline_it->second;
  }

  std::vector<spv::Id> id_vector_temp;

  SPIRVBuilder builder(spv::Spv_1_0, (SPIRVShaderTranslator::kSPIRVMagicToolID << 16) | 1, nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint3 = builder.makeVectorType(type_uint, 3);
  spv::Id type_float = builder.makeFloatType(32);

  bool format_is_64bpp =
      !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(key.GetColorFormat());
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeRuntimeArray(format_is_64bpp ? type_uint2 : type_uint));
  // Storage buffers use std430 packing, so there is no vec4 padding.
  builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride,
                        static_cast<int>(sizeof(u32) << static_cast<u32>(format_is_64bpp)));
  spv::Id type_edram = builder.makeStructType(id_vector_temp, "XeEdram");
  builder.addMemberName(type_edram, 0, "edram");
  builder.addMemberDecoration(type_edram, 0, spv::DecorationNonReadable);
  builder.addMemberDecoration(type_edram, 0, spv::DecorationOffset, 0);
  // BufferBlock rather than Block, as SPIR-V 1.0 is generated.
  builder.addDecoration(type_edram, spv::DecorationBufferBlock);
  // Uniform rather than StorageBuffer, as SPIR-V 1.0 is generated.
  spv::Id edram_buffer =
      builder.createVariable(spv::NoPrecision, spv::StorageClassUniform, type_edram, "xe_edram");
  builder.addDecoration(edram_buffer, spv::DecorationDescriptorSet, kDumpDescriptorSetEDRAM);
  builder.addDecoration(edram_buffer, spv::DecorationBinding, 0);
  bool source_is_multisampled = key.msaa_samples != xenos::MSAASamples::k1X;
  bool source_is_uint;
  if (key.is_depth) {
    source_is_uint = false;
  } else {
    GetColorOwnershipTransferVulkanFormat(key.GetColorFormat(), key.msaa_samples, &source_is_uint);
  }
  spv::Id source_component_type = source_is_uint ? type_uint : type_float;
  spv::Id source_texture = builder.createVariable(
      spv::NoPrecision, spv::StorageClassUniformConstant,
      builder.makeImageType(source_component_type, spv::Dim2D, false, false, source_is_multisampled,
                            1, spv::ImageFormatUnknown),
      "xe_edram_dump_source");
  builder.addDecoration(source_texture, spv::DecorationDescriptorSet, kDumpDescriptorSetSource);
  builder.addDecoration(source_texture, spv::DecorationBinding, 0);
  spv::Id source_stencil_texture = spv::NoResult;
  if (key.is_depth) {
    source_stencil_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_uint, spv::Dim2D, false, false, source_is_multisampled, 1,
                              spv::ImageFormatUnknown),
        "xe_edram_dump_stencil");
    builder.addDecoration(source_stencil_texture, spv::DecorationDescriptorSet,
                          kDumpDescriptorSetSource);
    builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
  }
  id_vector_temp.clear();
  id_vector_temp.reserve(kDumpPushConstantCount);
  for (u32 i = 0; i < kDumpPushConstantCount; ++i) {
    id_vector_temp.push_back(type_uint);
  }
  spv::Id type_push_constants = builder.makeStructType(id_vector_temp, "XeEdramDumpPushConstants");
  builder.addMemberName(type_push_constants, kDumpPushConstantPitches, "pitches");
  builder.addMemberDecoration(type_push_constants, kDumpPushConstantPitches, spv::DecorationOffset,
                              static_cast<int>(sizeof(u32) * kDumpPushConstantPitches));
  builder.addMemberName(type_push_constants, kDumpPushConstantOffsets, "offsets");
  builder.addMemberDecoration(type_push_constants, kDumpPushConstantOffsets, spv::DecorationOffset,
                              static_cast<int>(sizeof(u32) * kDumpPushConstantOffsets));
  builder.addDecoration(type_push_constants, spv::DecorationBlock);
  spv::Id push_constants =
      builder.createVariable(spv::NoPrecision, spv::StorageClassPushConstant, type_push_constants,
                             "xe_edram_dump_push_constants");

  spv::Id input_global_invocation_id = builder.createVariable(
      spv::NoPrecision, spv::StorageClassInput, type_uint3, "gl_GlobalInvocationID");
  builder.addDecoration(input_global_invocation_id, spv::DecorationBuiltIn,
                        spv::BuiltInGlobalInvocationId);

  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function = builder.makeFunctionEntry(
      spv::NoPrecision, type_void, "main", main_param_types, main_precisions, &main_entry);

  spv::Id global_invocation_id = builder.createLoad(input_global_invocation_id, spv::NoPrecision);
  spv::Id rectangle_sample_x = builder.createCompositeExtract(global_invocation_id, type_uint, 0);
  u32 tile_width = (xenos::kEDRAMTileWidthSamples >> static_cast<u32>(format_is_64bpp)) *
                   GetDrawResolutionScaleX();
  spv::Id const_tile_width = builder.makeUintConstant(tile_width);
  spv::Id rectangle_tile_index_x =
      builder.createBinOp(spv::OpUDiv, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id tile_sample_x =
      builder.createBinOp(spv::OpUMod, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id rectangle_sample_y = builder.createCompositeExtract(global_invocation_id, type_uint, 1);
  u32 tile_height = xenos::kEDRAMTileHeightSamples * GetDrawResolutionScaleY();
  spv::Id const_tile_height = builder.makeUintConstant(tile_height);
  spv::Id rectangle_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, rectangle_sample_y, const_tile_height);
  spv::Id tile_sample_y =
      builder.createBinOp(spv::OpUMod, type_uint, rectangle_sample_y, const_tile_height);

  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(kDumpPushConstantPitches));
  spv::Id pitches_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants, id_vector_temp),
      spv::NoPrecision);
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_edram_pitch_tiles_bits = builder.makeUintConstant(xenos::kEDRAMPitchTilesBits);
  spv::Id rectangle_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint,
                          builder.createTriOp(spv::OpBitFieldUExtract, type_uint, pitches_constant,
                                              const_uint_0, const_edram_pitch_tiles_bits),
                          rectangle_tile_index_y),
      rectangle_tile_index_x);
  // Not wrapped yet, so the address relative to the base stays non negative after subtraction.
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(kDumpPushConstantOffsets));
  spv::Id offsets_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants, id_vector_temp),
      spv::NoPrecision);
  spv::Id const_edram_base_tiles_bits_plus_1 =
      builder.makeUintConstant(xenos::kEDRAMBaseTilesBits + 1);
  spv::Id edram_tile_index_non_wrapped =
      builder.createBinOp(spv::OpIAdd, type_uint,
                          builder.createTriOp(spv::OpBitFieldUExtract, type_uint, offsets_constant,
                                              const_uint_0, const_edram_base_tiles_bits_plus_1),
                          rectangle_tile_index);

  spv::Id edram_sample_address = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint, builder.makeUintConstant(tile_width * tile_height),
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, edram_tile_index_non_wrapped,
                              builder.makeUintConstant(xenos::kEDRAMTileCount - 1))),
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(spv::OpIMul, type_uint, const_tile_width, tile_sample_y),
          tile_sample_x));
  if (key.is_depth) {
    // Depth buffers swap 40 sample columns in the destination address.
    u32 tile_width_half = tile_width >> 1;
    edram_sample_address = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int, edram_sample_address),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(spv::OpULessThan, builder.makeBoolType(), tile_sample_x,
                                    builder.makeUintConstant(tile_width_half)),
                builder.makeIntConstant(static_cast<i32>(tile_width_half)),
                builder.makeIntConstant(-static_cast<i32>(tile_width_half)))));
  }

  spv::Id source_tile_index = builder.createBinOp(
      spv::OpISub, type_uint, edram_tile_index_non_wrapped,
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, offsets_constant,
                          const_edram_base_tiles_bits_plus_1,
                          builder.makeUintConstant(xenos::kEDRAMBaseTilesBits)));
  spv::Id source_pitch_tiles =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, pitches_constant,
                          const_edram_pitch_tiles_bits, const_edram_pitch_tiles_bits);
  spv::Id source_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x =
      builder.createBinOp(spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_sample_x = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_width, source_tile_index_x),
      tile_sample_x);
  spv::Id source_sample_y = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_height, source_tile_index_y),
      tile_sample_y);
  spv::Id source_pixel_x = source_sample_x, source_pixel_y = source_sample_y;
  spv::Id source_sample_id = spv::NoResult;
  if (source_is_multisampled) {
    spv::Id const_uint_1 = builder.makeUintConstant(1);
    source_pixel_y =
        builder.createBinOp(spv::OpShiftRightLogical, type_uint, source_sample_y, const_uint_1);
    if (key.msaa_samples >= xenos::MSAASamples::k4X) {
      source_pixel_x =
          builder.createBinOp(spv::OpShiftRightLogical, type_uint, source_sample_x, const_uint_1);
      // The 4x sample index has the horizontal sample in bit 0 and the vertical one in bit 1.
      source_sample_id = builder.CreateQuadOp(
          spv::OpBitFieldInsert, type_uint,
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_x, const_uint_1),
          source_sample_y, const_uint_1, const_uint_1);
    } else {
      // The 2x sample index is converted from the guest to Vulkan standard sample locations.
      source_sample_id = builder.createTriOp(
          spv::OpSelect, type_uint,
          builder.createBinOp(
              spv::OpINotEqual, builder.makeBoolType(),
              builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_y, const_uint_1),
              const_uint_0),
          builder.makeUintConstant(
              draw_state::GetD3D10SampleIndexForGuest2xMSAA(1, msaa_2x_attachments_supported_)),
          builder.makeUintConstant(
              draw_state::GetD3D10SampleIndexForGuest2xMSAA(0, msaa_2x_attachments_supported_)));
    }
  }

  spv::Id packed[2] = {};
  spv::Builder::TextureParameters source_texture_parameters = {};
  source_texture_parameters.sampler = builder.createLoad(source_texture, spv::NoPrecision);
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_x));
  id_vector_temp.push_back(builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_y));
  source_texture_parameters.coords = builder.createCompositeConstruct(type_int2, id_vector_temp);
  if (source_is_multisampled) {
    source_texture_parameters.sample =
        builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  } else {
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  spv::Id source_vec4 = builder.createTextureCall(
      spv::NoPrecision, builder.makeVectorType(source_component_type, 4), false, true, false, false,
      false, source_texture_parameters, spv::ImageOperandsMaskNone);
  const bool source_color_16_is_float =
      !key.is_depth && IsColor16FormatFloatLike(key.GetColorFormat());
  spv::Id const_uint_16 = builder.makeUintConstant(16);
  spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
  spv::Id const_float_1 = builder.makeFloatConstant(1.0f);
  spv::Id const_float_minus_1 = builder.makeFloatConstant(-1.0f);
  spv::Id const_float_32767 = builder.makeFloatConstant(32767.0f);
  auto linear_to_pwl_gamma = [&](spv::Id linear, bool linear_pre_saturated) -> spv::Id {
    if (!linear_pre_saturated) {
      linear = builder.CreateTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                            linear, const_float_0, const_float_1);
    }
    spv::Id is_piece_at_least_3 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, builder.makeBoolType(), linear,
                            builder.makeFloatConstant(512.0f / 1023.0f));
    spv::Id scale_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                               builder.makeFloatConstant(1023.0f / 8.0f),
                                               builder.makeFloatConstant(1023.0f / 4.0f));
    spv::Id offset_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                                builder.makeFloatConstant(128.0f / 255.0f),
                                                builder.makeFloatConstant(64.0f / 255.0f));
    spv::Id is_piece_at_least_1 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, builder.makeBoolType(), linear,
                            builder.makeFloatConstant(64.0f / 1023.0f));
    spv::Id scale_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                               builder.makeFloatConstant(1023.0f / 2.0f),
                                               builder.makeFloatConstant(1023.0f));
    spv::Id offset_1_or_0 =
        builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                            builder.makeFloatConstant(32.0f / 255.0f), const_float_0);
    spv::Id is_piece_at_least_2 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, builder.makeBoolType(), linear,
                            builder.makeFloatConstant(128.0f / 1023.0f));
    spv::Id scale = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
    spv::Id offset = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);
    return builder.createBinOp(
        spv::OpFAdd, type_float,
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.CreateUnaryBuiltinCall(
                                type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                builder.createBinOp(spv::OpFMul, type_float, linear, scale)),
                            builder.makeFloatConstant(1.0f / 255.0f)),
        offset);
  };
  auto pack_dump_source16_component_to_uint = [&](spv::Id component) -> spv::Id {
    if (source_is_uint) {
      return component;
    }
    if (source_color_16_is_float) {
      id_vector_temp.clear();
      id_vector_temp.push_back(component);
      id_vector_temp.push_back(const_float_0);
      spv::Id packed_half = builder.CreateUnaryBuiltinCall(
          type_uint, ext_inst_glsl_std_450, GLSLstd450PackHalf2x16,
          builder.createCompositeConstruct(builder.makeVectorType(type_float, 2), id_vector_temp));
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_half, const_uint_0,
                                 const_uint_16);
    }
    spv::Id component_clamped =
        builder.CreateTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, component,
                                     const_float_minus_1, const_float_1);
    spv::Id component_rounded = builder.CreateUnaryBuiltinCall(
        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
        builder.createBinOp(spv::OpFMul, type_float, component_clamped, const_float_32767));
    spv::Id component_snorm =
        builder.createUnaryOp(spv::OpConvertFToS, type_int, component_rounded);
    return builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                               builder.createUnaryOp(spv::OpBitcast, type_uint, component_snorm),
                               const_uint_0, const_uint_16);
  };
  auto pack_dump_source16_pair_to_uint32 = [&](spv::Id component_0,
                                               spv::Id component_1) -> spv::Id {
    return builder.CreateQuadOp(
        spv::OpBitFieldInsert, type_uint, pack_dump_source16_component_to_uint(component_0),
        pack_dump_source16_component_to_uint(component_1), const_uint_16, const_uint_16);
  };
  if (key.is_depth) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    spv::Id source_stencil = builder.createCompositeExtract(
        builder.createTextureCall(spv::NoPrecision, builder.makeVectorType(type_uint, 4), false,
                                  true, false, false, false, source_texture_parameters,
                                  spv::ImageOperandsMaskNone),
        type_uint, 0);
    spv::Id source_depth32 = builder.createCompositeExtract(source_vec4, type_float, 0);
    switch (key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24S8: {
        // Nearest even rounding, as +0.5 with truncation makes black red in a clear shader.
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.CreateUnaryBuiltinCall(
                type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                builder.createBinOp(spv::OpFMul, type_float, source_depth32,
                                    builder.makeFloatConstant(static_cast<f32>(0xFFFFFF)))));
      } break;
      case xenos::DepthRenderTargetFormat::kD24FS8: {
        packed[0] = SPIRVShaderTranslator::PreClampedDepthTo20e4(
            builder, source_depth32, IsDepthFloat24Rounded(), true, ext_inst_glsl_std_450);
      } break;
    }
    packed[0] = builder.CreateQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil, packed[0],
                                     builder.makeUintConstant(8), builder.makeUintConstant(24));
  } else {
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
        if (gamma_render_target_as_unorm16_) {
          // 8_8_8_8_GAMMA is stored as linear in R16G16B16A16_UNORM.
          id_vector_temp.clear();
          for (u32 i = 0; i < 3; ++i) {
            id_vector_temp.push_back(linear_to_pwl_gamma(
                builder.createCompositeExtract(source_vec4, type_float, i), true));
          }
        }
      }
        [[fallthrough]];
      case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
        spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
        spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
        spv::Id color_0 = builder.createCompositeExtract(source_vec4, type_float, 0);
        if (key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
            gamma_render_target_as_unorm16_) {
          color_0 = id_vector_temp[0];
        }
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(spv::OpFAdd, type_float,
                                builder.createBinOp(spv::OpFMul, type_float, color_0, unorm_scale),
                                unorm_round_offset));
        spv::Id component_width = builder.makeUintConstant(8);
        for (u32 i = 1; i < 4; ++i) {
          spv::Id color_i = builder.createCompositeExtract(source_vec4, type_float, i);
          if (key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
              gamma_render_target_as_unorm16_ && i < 3) {
            color_i = id_vector_temp[i];
          }
          packed[0] = builder.CreateQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(spv::OpConvertFToU, type_uint,
                                    builder.createBinOp(spv::OpFAdd, type_float,
                                                        builder.createBinOp(spv::OpFMul, type_float,
                                                                            color_i, unorm_scale),
                                                        unorm_round_offset)),
              builder.makeUintConstant(8 * i), component_width);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
        spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
        spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(
                spv::OpFAdd, type_float,
                builder.createBinOp(spv::OpFMul, type_float,
                                    builder.createCompositeExtract(source_vec4, type_float, 0),
                                    unorm_scale_rgb),
                unorm_round_offset));
        spv::Id width_rgb = builder.makeUintConstant(10);
        spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
        spv::Id width_a = builder.makeUintConstant(2);
        for (u32 i = 1; i < 4; ++i) {
          packed[0] = builder.CreateQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(
                  spv::OpConvertFToU, type_uint,
                  builder.createBinOp(spv::OpFAdd, type_float,
                                      builder.createBinOp(spv::OpFMul, type_float,
                                                          builder.createCompositeExtract(
                                                              source_vec4, type_float, i),
                                                          i == 3 ? unorm_scale_a : unorm_scale_rgb),
                                      unorm_round_offset)),
              builder.makeUintConstant(10 * i), i == 3 ? width_a : width_rgb);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
        // Float16 has a wider range for color and alpha, and NaNs, so clamp and convert.
        packed[0] = SPIRVShaderTranslator::UnclampedFloat32To7e3(
            builder, builder.createCompositeExtract(source_vec4, type_float, 0),
            ext_inst_glsl_std_450);
        spv::Id width_rgb = builder.makeUintConstant(10);
        for (u32 i = 1; i < 3; ++i) {
          packed[0] = builder.CreateQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              SPIRVShaderTranslator::UnclampedFloat32To7e3(
                  builder, builder.createCompositeExtract(source_vec4, type_float, i),
                  ext_inst_glsl_std_450),
              builder.makeUintConstant(10 * i), width_rgb);
        }
        spv::Id alpha_saturated = builder.CreateTriBuiltinCall(
            type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
            builder.createCompositeExtract(source_vec4, type_float, 3),
            builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
        packed[0] = builder.CreateQuadOp(
            spv::OpBitFieldInsert, type_uint, packed[0],
            builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, alpha_saturated,
                                                        builder.makeFloatConstant(3.0f)),
                                    builder.makeFloatConstant(0.5f))),
            builder.makeUintConstant(30), builder.makeUintConstant(2));
      } break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
        for (u32 i = 0; i <= static_cast<u32>(format_is_64bpp); ++i) {
          packed[i] = pack_dump_source16_pair_to_uint32(
              builder.createCompositeExtract(source_vec4, source_component_type, 2 * i),
              builder.createCompositeExtract(source_vec4, source_component_type, 2 * i + 1));
        }
      } break;
      // Float32 goes as uint32 to keep NaN encodings, though integer MSAA images are optional.
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
        for (u32 i = 0; i <= static_cast<u32>(format_is_64bpp); ++i) {
          spv::Id& packed_ref = packed[i];
          packed_ref = builder.createCompositeExtract(source_vec4, source_component_type, i);
          if (!source_is_uint) {
            packed_ref = builder.createUnaryOp(spv::OpBitcast, type_uint, packed_ref);
          }
        }
      } break;
    }
  }

  spv::Id store_value = packed[0];
  if (format_is_64bpp) {
    id_vector_temp.clear();
    id_vector_temp.push_back(packed[0]);
    id_vector_temp.push_back(packed[1]);
    store_value = builder.createCompositeConstruct(type_uint2, id_vector_temp);
  }
  id_vector_temp.clear();
  // The only SSBO structure member.
  id_vector_temp.push_back(builder.makeIntConstant(0));
  id_vector_temp.push_back(builder.createUnaryOp(spv::OpBitcast, type_int, edram_sample_address));
  // Uniform rather than StorageBuffer, as SPIR-V 1.0 is generated.
  builder.createStore(store_value, builder.createAccessChain(spv::StorageClassUniform, edram_buffer,
                                                             id_vector_temp));

  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeLocalSize, kDumpSamplesPerGroupX,
                           kDumpSamplesPerGroupY, 1);
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelGLCompute, main_function, "main");
  // SPIR-V 1.0 needs only inputs and outputs in the entry point interface, not bindings.
  entry_point->addIdOperand(input_global_invocation_id);

  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);
  if (const std::string dump_path = command_processor_.GetXenosSettings().dump_shaders.Get();
      !dump_path.empty()) {
    gpu::vulkan::objects::DumpShader(dump_path, std::format("edram_dump_{:08X}", key.key),
                                     shader_code);
  }

  // The handle is stored even on failure, so creation is not retried.
  VkPipeline pipeline = gpu::vulkan::objects::CreateComputePipeline(
      command_processor_.GetVulkanDevice(),
      key.is_depth ? dump_pipeline_layout_depth_ : dump_pipeline_layout_color_,
      reinterpret_cast<const u32*>(shader_code.data()), sizeof(u32) * shader_code.size());
  if (pipeline == VK_NULL_HANDLE) {
    REXLOG_ERROR(gpu,
                 "VulkanRenderTargetCache: Failed to create a render target dumping "
                 "pipeline for {}-sample render targets with format {}",
                 UINT32_C(1) << static_cast<u32>(key.msaa_samples),
                 key.is_depth ? xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat())
                              : xenos::GetColorRenderTargetFormatName(key.GetColorFormat()));
  }
  dump_pipelines_.emplace(key, pipeline);
  return pipeline;
}

VkPipeline VulkanRenderTargetCache::GetDirectResolvePipeline(DirectResolvePipelineKey key) {
  auto pipeline_it = direct_resolve_pipelines_.find(key);
  if (pipeline_it != direct_resolve_pipelines_.end()) {
    return pipeline_it->second;
  }
  VkPipeline pipeline = VK_NULL_HANDLE;
  // Reuses the resolve copy pipelines, as no dedicated direct resolve shaders exist.
  size_t copy_shader_index = static_cast<size_t>(key.copy_shader);
  if (copy_shader_index < static_cast<size_t>(draw_state::ResolveCopyShaderIndex::kCount)) {
    pipeline = resolve_copy_pipelines_[copy_shader_index];
  }
  direct_resolve_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool VulkanRenderTargetCache::TryDirectResolve(const draw_state::ResolveRequest& resolve_info,
                                               draw_state::ResolveCopyShaderIndex copy_shader,
                                               bool draw_resolution_scaled) {
  ++direct_resolve_attempt_count_;
  if (direct_resolve_pipeline_layout_color_ == VK_NULL_HANDLE ||
      direct_resolve_pipeline_layout_depth_ == VK_NULL_HANDLE) {
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
    const auto* render_target = static_cast<const VulkanRenderTarget*>(rectangle.render_target);
    if (render_target == nullptr) {
      return false;
    }
    DumpPipelineKey dump_pipeline_key;
    dump_pipeline_key.msaa_samples = render_target->GetKey().msaa_samples;
    dump_pipeline_key.resource_format = render_target->GetKey().resource_format;
    dump_pipeline_key.is_depth = render_target->GetKey().is_depth;
    if (GetDumpPipeline(dump_pipeline_key) == VK_NULL_HANDLE) {
      return false;
    }
    DirectResolvePipelineKey direct_pipeline_key;
    direct_pipeline_key.dump_pipeline_key = dump_pipeline_key;
    direct_pipeline_key.copy_shader = copy_shader;
    direct_pipeline_key.draw_resolution_scaled = draw_resolution_scaled;
    if (GetDirectResolvePipeline(direct_pipeline_key) == VK_NULL_HANDLE) {
      return false;
    }
  }

  // Falls back to the dump path, as no source image direct shaders exist.
  return DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch);
}

bool VulkanRenderTargetCache::DumpRenderTargets(u32 dump_base, u32 dump_row_length_used,
                                                u32 dump_rows, u32 dump_pitch) {
  REX_ASSERT(GetPath() == Path::kHostRenderTargets);

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_);
  if (dump_rectangles_.empty()) {
    return true;
  }

  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    static_cast<VulkanRenderTarget*>(rectangle.render_target)->SetTemporarySortIndex(UINT32_MAX);
  }
  UseEDRAMBuffer(EDRAMBufferUsage::kComputeWrite);
  dump_invocations_.clear();
  dump_invocations_.reserve(dump_rectangles_.size());
  u32 rt_sort_index = 0;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.GetKey();
    command_processor_.PushImageMemoryBarrier(
        vulkan_rt.GetImage(),
        gpu::vulkan::objects::BuildSubresourceRange(
            rt_key.is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                            : VK_IMAGE_ASPECT_COLOR_BIT),
        vulkan_rt.GetCurrentStageMask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        vulkan_rt.GetCurrentAccessMask(), VK_ACCESS_SHADER_READ_BIT, vulkan_rt.GetCurrentLayout(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (vulkan_rt.GetTemporarySortIndex() == UINT32_MAX) {
      vulkan_rt.SetTemporarySortIndex(rt_sort_index++);
    }
    DumpPipelineKey pipeline_key;
    pipeline_key.msaa_samples = rt_key.msaa_samples;
    pipeline_key.resource_format = rt_key.resource_format;
    pipeline_key.is_depth = rt_key.is_depth;
    dump_invocations_.emplace_back(rectangle, pipeline_key);
  }

  std::sort(dump_invocations_.begin(), dump_invocations_.end());

  DeferredCommandBuffer& command_buffer = command_processor_.GetDeferredCommandBuffer();
  bool edram_buffer_bound = false;
  VkDescriptorSet last_source_descriptor_set = VK_NULL_HANDLE;
  DumpPitches last_pitches;
  DumpOffsets last_offsets;
  bool pitches_bound = false, offsets_bound = false;
  bool all_pipelines_available = true;
  for (const DumpInvocation& invocation : dump_invocations_) {
    const ResolveCopyDumpRectangle& rectangle = invocation.rectangle;
    auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.GetKey();
    DumpPipelineKey pipeline_key = invocation.pipeline_key;
    VkPipeline pipeline = GetDumpPipeline(pipeline_key);
    if (!pipeline) {
      all_pipelines_available = false;
      continue;
    }
    command_processor_.BindExternalComputePipeline(pipeline);

    VkPipelineLayout pipeline_layout =
        rt_key.is_depth ? dump_pipeline_layout_depth_ : dump_pipeline_layout_color_;

    // Pipeline layout compatibility keeps the EDRAM buffer bound, so it is bound once.
    if (!edram_buffer_bound) {
      edram_buffer_bound = true;
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                             kDumpDescriptorSetEDRAM, 1,
                                             &edram_storage_buffer_descriptor_set_, 0, nullptr);
    }

    VkDescriptorSet source_descriptor_set = vulkan_rt.GetDescriptorSetTransferSource();
    if (last_source_descriptor_set != source_descriptor_set) {
      last_source_descriptor_set = source_descriptor_set;
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                             kDumpDescriptorSetSource, 1, &source_descriptor_set, 0,
                                             nullptr);
    }

    DumpPitches pitches;
    pitches.dest_pitch = dump_pitch;
    pitches.source_pitch = rt_key.GetPitchTiles();
    if (last_pitches != pitches) {
      last_pitches = pitches;
      pitches_bound = false;
    }
    if (!pitches_bound) {
      pitches_bound = true;
      command_buffer.CmdVkPushConstants(pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                        sizeof(u32) * kDumpPushConstantPitches,
                                        sizeof(last_pitches), &last_pitches);
    }

    DumpOffsets offsets;
    offsets.source_base_tiles = rt_key.base_tiles;
    ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    u32 dispatch_count = rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (u32 i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      offsets.dispatch_first_tile = dump_base + dispatch.offset;
      if (last_offsets != offsets) {
        last_offsets = offsets;
        offsets_bound = false;
      }
      if (!offsets_bound) {
        offsets_bound = true;
        command_buffer.CmdVkPushConstants(pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                          sizeof(u32) * kDumpPushConstantOffsets,
                                          sizeof(last_offsets), &last_offsets);
      }
      command_processor_.SubmitBarriers(true);
      command_buffer.CmdVkDispatch(
          (GetDrawResolutionScaleX() *
               (xenos::kEDRAMTileWidthSamples >> static_cast<u32>(rt_key.Is64bpp())) *
               dispatch.width_tiles +
           (kDumpSamplesPerGroupX - 1)) /
              kDumpSamplesPerGroupX,
          (GetDrawResolutionScaleY() * xenos::kEDRAMTileHeightSamples * dispatch.height_tiles +
           (kDumpSamplesPerGroupY - 1)) /
              kDumpSamplesPerGroupY,
          1);
    }
    MarkEDRAMBufferModified();
  }
  return all_pipelines_available;
}

bool VulkanRenderTargetCache::IsFixedRG16TruncatedToMinus1To1() const {
  return GetPath() == Path::kHostRenderTargets && !color_rg16_draw_format_fallback_to_float_ &&
         !command_processor_.GetXenosSettings().snorm16_render_target_full_range.Get();
}

bool VulkanRenderTargetCache::IsFixedRGBA16TruncatedToMinus1To1() const {
  return GetPath() == Path::kHostRenderTargets && !color_rgba16_draw_format_fallback_to_float_ &&
         !command_processor_.GetXenosSettings().snorm16_render_target_full_range.Get();
}

}  // namespace rex::gpu::vulkan
