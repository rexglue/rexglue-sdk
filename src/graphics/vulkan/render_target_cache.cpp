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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <SPIRV/GLSL.std.450.h>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/frame_dump.h>
#include <rex/graphics/frame_events.h>
#include <rex/graphics/pipeline/shader/spirv_builder.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_STRING(render_target_path_vulkan, "", "GPU/Vulkan",
                      "Vulkan render target implementation path")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(edram_merge_tiles, true, "GPU/Vulkan",
                    "When the game renders the frame in two tiles (the same draws twice, the "
                    "second time with a window offset, because the frame doesn't fit EDRAM), "
                    "rasterize the first tile's draws over the whole frame and drop the second "
                    "tile's (Fable II: about 850 of 5,000 draws per frame)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(edram_forward_4x_clears, true, "GPU/Vulkan",
                    "Apply Direct3D 9 clears, which Xbox 360 games draw through a 4x "
                    "multisampled view of the surface's EDRAM memory, straight to the host "
                    "render target that holds that memory, instead of drawing them to a render "
                    "target for the view and copying the result back (Fable II: about 75 "
                    "copies per frame)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(resolve_direct_hdr_depth, true, "GPU/Vulkan",
                    "With resolve_direct_to_texture, also resolve 2:10:10:10 float color "
                    "(Fable II's HDR scene, both tiles) and float24 depth straight into the "
                    "textures made of the destination, through a compute shader that converts "
                    "them as the path through memory does")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DECLARE(int32_t, resolve_direct_to_texture);
REXCVAR_DECLARE(int32_t, resolve_direct_skip_memory);

REXCVAR_DEFINE_BOOL(edram_skip_layout_mismatch_transfers, true, "GPU/Vulkan",
                    "Don't copy EDRAM contents between two host render targets that lay the "
                    "same EDRAM memory out differently (different pitch, sample count or bytes "
                    "per pixel): the pixels would arrive scrambled, so a game can only be "
                    "reusing the memory, not the picture. Each render target then keeps its own "
                    "previous contents instead. Copies to and from 4x multisampled views, which "
                    "Direct3D 9 uses for clearing, are kept")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skip_color_overwrite_transfers, false, "GPU/Vulkan",
                    "Don't copy the previous EDRAM contents into a color render target when "
                    "the draw that needs it is a single rectangle writing every pixel of the "
                    "copied area unconditionally (no blending, alpha test, discard, depth or "
                    "stencil test, all components written)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// DEFINE_string(
//     render_target_path_vulkan, "",
//     "Render target emulation path to use on Vulkan.\n"
//     "Use: [any, fbo, fsi]\n"
//     " fbo:\n"
//     "  Host framebuffers and fixed-function blending and depth / stencil "
//     "testing, copying between render targets when needed.\n"
//     "  Lower accuracy (limited pixel format support).\n"
//     "  Performance limited primarily by render target layout changes requiring "
//     "copying, but generally higher.\n"
//     " fsi:\n"
//     "  Manual pixel packing, blending and depth / stencil testing, with free "
//     "render target layout changes.\n"
//     "  Requires a GPU supporting fragment shader interlock.\n"
//     "  Highest accuracy (all pixel formats handled in software).\n"
//     "  Performance limited primarily by overdraw.\n"
//     " Any other value:\n"
//     "  Choose what is considered the most optimal for the system (currently "
//     "always FB because the FSI path is much slower now).",
//     "GPU");

namespace rex::graphics::vulkan {

// Generated with `xb buildshaders`.
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
    VulkanRenderTargetCache::kResolveCopyShaders[size_t(
        draw_util::ResolveCopyShaderIndex::kCount)] = {
        {shaders::resolve_fast_32bpp_1x2xmsaa_cs, sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_cs),
         shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_32bpp_4xmsaa_cs, sizeof(shaders::resolve_fast_32bpp_4xmsaa_cs),
         shaders::resolve_fast_32bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_1x2xmsaa_cs, sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_cs),
         shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_4xmsaa_cs, sizeof(shaders::resolve_fast_64bpp_4xmsaa_cs),
         shaders::resolve_fast_64bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_full_8bpp_cs, sizeof(shaders::resolve_full_8bpp_cs),
         shaders::resolve_full_8bpp_scaled_cs, sizeof(shaders::resolve_full_8bpp_scaled_cs)},
        {shaders::resolve_full_16bpp_cs, sizeof(shaders::resolve_full_16bpp_cs),
         shaders::resolve_full_16bpp_scaled_cs, sizeof(shaders::resolve_full_16bpp_scaled_cs)},
        {shaders::resolve_full_32bpp_cs, sizeof(shaders::resolve_full_32bpp_cs),
         shaders::resolve_full_32bpp_scaled_cs, sizeof(shaders::resolve_full_32bpp_scaled_cs)},
        {shaders::resolve_full_64bpp_cs, sizeof(shaders::resolve_full_64bpp_cs),
         shaders::resolve_full_64bpp_scaled_cs, sizeof(shaders::resolve_full_64bpp_scaled_cs)},
        {shaders::resolve_full_128bpp_cs, sizeof(shaders::resolve_full_128bpp_cs),
         shaders::resolve_full_128bpp_scaled_cs, sizeof(shaders::resolve_full_128bpp_scaled_cs)},
};

const VulkanRenderTargetCache::TransferPipelineLayoutInfo
    VulkanRenderTargetCache::kTransferPipelineLayoutInfos[size_t(
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
    VulkanRenderTargetCache::kTransferModes[size_t(TransferMode::kCount)] = {
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

VulkanRenderTargetCache::VulkanRenderTargetCache(const RegisterFile& register_file,
                                                 const memory::Memory& memory,
                                                 uint32_t draw_resolution_scale_x,
                                                 uint32_t draw_resolution_scale_y,
                                                 VulkanCommandProcessor& command_processor)
    : RenderTargetCache(register_file, memory, draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor) {}

VulkanRenderTargetCache::~VulkanRenderTargetCache() {
  Shutdown(true);
}

bool VulkanRenderTargetCache::Initialize(uint32_t shared_memory_binding_count) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanInstance::Functions& ifn = vulkan_device->vulkan_instance()->functions();
  const VkPhysicalDevice physical_device = vulkan_device->physical_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();

  bool fsi_path_supported =
      (device_properties.fragmentShaderSampleInterlock ||
       device_properties.fragmentShaderPixelInterlock) &&
      device_properties.fragmentStoresAndAtomics && device_properties.sampleRateShading &&
      device_properties.standardSampleLocations &&
      shared_memory_binding_count < device_properties.maxPerStageDescriptorStorageBuffers;
  if (REXCVAR_GET(render_target_path_vulkan) == "fsi") {
    path_ = Path::kPixelShaderInterlock;
  } else {
    path_ = Path::kHostRenderTargets;
  }
  // Fragment shader interlock is a feature implemented by pretty advanced GPUs,
  // closer to Direct3D 11 / OpenGL ES 3.2 level mainly, not Direct3D 10 /
  // OpenGL ES 3.1. Thus, it's fine to demand a wide range of other optional
  // features for the fragment shader interlock backend to work.
  if (path_ == Path::kPixelShaderInterlock) {
    // Interlocking between fragments with common sample coverage is enough, but
    // interlocking more is acceptable too (fragmentShaderShadingRateInterlock
    // would be okay too, but it's unlikely that an implementation would
    // advertise only it and not any other ones, as it's a very specific feature
    // interacting with another optional feature that is variable shading rate,
    // so there's no need to overcomplicate the checks and the shader execution
    // mode setting).
    // Sample-rate shading is required by certain SPIR-V revisions to access the
    // sample mask fragment shader input.
    // Stanard sample locations are needed for calculating the depth at the
    // samples.
    // It's unlikely that a device exposing fragment shader interlock won't have
    // a large enough storage buffer range and a sufficient SSBO slot count for
    // all the shared memory buffers and the EDRAM buffer - an in a conflict
    // between, for instance, the ability to vfetch and memexport in fragment
    // shaders, and the usage of fragment shader interlock, prefer the former
    // for simplicity.
    if (!fsi_path_supported) {
      path_ = Path::kHostRenderTargets;
    }
  }

  // Format support.
  constexpr VkFormatFeatureFlags kUsedDepthFormatFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  constexpr VkFormatFeatureFlags kUsedColorFormatFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  bool gamma_render_target_as_unorm16_requested =
      path_ == Path::kHostRenderTargets && REXCVAR_GET(gamma_render_target_as_unorm16);
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
      REXGPU_WARN(
          "VulkanRenderTargetCache: R16G16_SNORM render target support is unavailable; "
          "falling back to R16G16_SFLOAT for k_16_16");
    } else {
      color_rg16_draw_format_supported = false;
    }
  }
  bool color_rgba16_draw_format_supported = true;
  if (!color_rgba16_snorm_supported) {
    if (color_rgba16_sfloat_supported) {
      color_rgba16_draw_format_fallback_to_float_ = true;
      REXGPU_WARN(
          "VulkanRenderTargetCache: R16G16B16A16_SNORM render target support is unavailable; "
          "falling back to R16G16B16A16_SFLOAT for k_16_16_16_16");
    } else {
      color_rgba16_draw_format_supported = false;
    }
  }
  if (path_ == Path::kHostRenderTargets &&
      (!color_rg16_draw_format_supported || !color_rgba16_draw_format_supported)) {
    if (fsi_path_supported) {
      REXGPU_WARN(
          "VulkanRenderTargetCache: Host render target 16-bit formats are unsupported "
          "(R16G16: {}, R16G16B16A16: {}); switching to fragment shader interlock "
          "path for D3D12 parity",
          color_rg16_draw_format_supported ? "available" : "unavailable",
          color_rgba16_draw_format_supported ? "available" : "unavailable");
      path_ = Path::kPixelShaderInterlock;
    } else {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Host render target 16-bit formats are unsupported "
          "(R16G16: {}, R16G16B16A16: {}), and fragment shader interlock fallback "
          "is unavailable",
          color_rg16_draw_format_supported ? "available" : "unavailable",
          color_rgba16_draw_format_supported ? "available" : "unavailable");
      return false;
    }
  }
  if (path_ == Path::kHostRenderTargets && gamma_render_target_as_unorm16_requested &&
      !color_rgba16_unorm_supported) {
    if (fsi_path_supported) {
      REXGPU_WARN(
          "VulkanRenderTargetCache: R16G16B16A16_UNORM render target support "
          "is unavailable for k_8_8_8_8_GAMMA linear storage; switching to "
          "fragment shader interlock path for D3D12 parity");
      path_ = Path::kPixelShaderInterlock;
    } else {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: R16G16B16A16_UNORM render target support "
          "is unavailable for k_8_8_8_8_GAMMA linear storage, and fragment "
          "shader interlock fallback is unavailable");
      return false;
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
    REXGPU_WARN(
        "VulkanRenderTargetCache: R16G16/R16G16B16A16 UINT ownership transfer formats are not "
        "supported");
  }
  color_32bit_transfer_uint_formats_supported_ =
      (color_r32_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures &&
      (color_rg32_uint_properties.optimalTilingFeatures & kUsedColorFormatFeatures) ==
          kUsedColorFormatFeatures;
  if (!color_32bit_transfer_uint_formats_supported_) {
    REXGPU_WARN(
        "VulkanRenderTargetCache: R32/R32G32 UINT ownership transfer formats are not supported");
  }
  if ((device_properties.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) &&
      !(device_properties.sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_4_BIT)) {
    REXGPU_WARN("VulkanRenderTargetCache: 4x integer sampled-image support is unavailable");
  }

  // 2x MSAA support.
  if (REXCVAR_GET(native_2x_msaa)) {
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
      REXGPU_WARN(
          "VulkanRenderTargetCache: Host render target ownership transfers "
          "can't be bit-exact on this device; switching to fragment shader "
          "interlock path for D3D12 parity");
      path_ = Path::kPixelShaderInterlock;
    } else {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Bit-exact host render target ownership "
          "transfers require UINT transfer formats and integer sampled-image "
          "MSAA support, and fragment shader interlock fallback is unavailable");
      return false;
    }
  }

  // Descriptor set layouts.
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
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with one storage buffer");
    Shutdown();
    return false;
  }
  descriptor_set_layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    REXGPU_ERROR(
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
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with two sampled images");
    Shutdown();
    return false;
  }

  // Descriptor set pools.
  // The pool sizes were chosen without a specific reason.
  VkDescriptorPoolSize descriptor_set_layout_size;
  descriptor_set_layout_size.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_layout_size.descriptorCount = 1;
  descriptor_set_pool_sampled_image_ = std::make_unique<ui::vulkan::SingleLayoutDescriptorSetPool>(
      vulkan_device, 256, 1, &descriptor_set_layout_size, descriptor_set_layout_sampled_image_);
  descriptor_set_layout_size.descriptorCount = 2;
  descriptor_set_pool_sampled_image_x2_ =
      std::make_unique<ui::vulkan::SingleLayoutDescriptorSetPool>(
          vulkan_device, 256, 1, &descriptor_set_layout_size,
          descriptor_set_layout_sampled_image_x2_);

  // EDRAM contents reinterpretation buffer.
  // 90 MB with 9x resolution scaling - within the minimum
  // maxStorageBufferRange.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device,
          VkDeviceSize(xenos::kEdramSizeBytes *
                       (draw_resolution_scale_x() * draw_resolution_scale_y())),
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, edram_buffer_, edram_buffer_memory_)) {
    REXGPU_ERROR("VulkanRenderTargetCache: Failed to create the EDRAM buffer");
    Shutdown();
    return false;
  }
  if (GetPath() == Path::kPixelShaderInterlock) {
    // The first operation will likely be drawing.
    edram_buffer_usage_ = EdramBufferUsage::kFragmentReadWrite;
  } else {
    // The first operation will likely be depth self-comparison.
    edram_buffer_usage_ = EdramBufferUsage::kFragmentRead;
  }
  edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
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
    REXGPU_ERROR(
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
    REXGPU_ERROR(
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

  // Resolve copy pipeline layout.
  VkDescriptorSetLayout resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetCount] = {};
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetEdram] =
      descriptor_set_layout_storage_buffer_;
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetDest] =
      command_processor_.GetSingleTransientDescriptorLayout(
          VulkanCommandProcessor::SingleTransientDescriptorLayout ::kStorageBufferCompute);
  VkPushConstantRange resolve_copy_push_constant_range;
  resolve_copy_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  resolve_copy_push_constant_range.offset = 0;
  // Potentially binding all of the shared memory at 1x resolution, but only
  // portions with scaled resolution.
  resolve_copy_push_constant_range.size =
      draw_resolution_scaled ? sizeof(draw_util::ResolveCopyShaderConstants::DestRelative)
                             : sizeof(draw_util::ResolveCopyShaderConstants);
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
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the resolve copy pipeline "
        "layout");
    Shutdown();
    return false;
  }

  // Direct resolve pipeline layouts (destination storage buffer + source image).
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
    pipeline_layout_create_info.setLayoutCount = uint32_t(rex::countof(descriptor_set_layouts));
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

  // Resolve copy pipelines.
  for (size_t i = 0; i < size_t(draw_util::ResolveCopyShaderIndex::kCount); ++i) {
    const draw_util::ResolveCopyShaderInfo& resolve_copy_shader_info =
        draw_util::resolve_copy_shader_info[i];
    const ResolveCopyShaderCode& resolve_copy_shader_code = kResolveCopyShaders[i];
    // Somewhat verification whether resolve_copy_shaders_ is up to date.
    assert_true(resolve_copy_shader_code.unscaled && resolve_copy_shader_code.unscaled_size_bytes &&
                resolve_copy_shader_code.scaled && resolve_copy_shader_code.scaled_size_bytes);
    VkPipeline resolve_copy_pipeline = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_copy_pipeline_layout_,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled
                               : resolve_copy_shader_code.unscaled,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled_size_bytes
                               : resolve_copy_shader_code.unscaled_size_bytes);
    if (resolve_copy_pipeline == VK_NULL_HANDLE) {
      REXGPU_ERROR(
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
    // Host render targets.

    gamma_render_target_as_unorm16_ = gamma_render_target_as_unorm16_requested;

    depth_float24_round_ = REXCVAR_GET(depth_float24_round);
    depth_float24_convert_in_pixel_shader_ = REXCVAR_GET(depth_float24_convert_in_pixel_shader);

    // Host depth storing pipeline layout.
    VkDescriptorSetLayout host_depth_store_descriptor_set_layouts[] = {
        // Destination EDRAM storage buffer.
        descriptor_set_layout_storage_buffer_,
        // Source depth / stencil texture (only depth is used).
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
        uint32_t(rex::countof(host_depth_store_descriptor_set_layouts));
    host_depth_store_pipeline_layout_create_info.pSetLayouts =
        host_depth_store_descriptor_set_layouts;
    host_depth_store_pipeline_layout_create_info.pushConstantRangeCount = 1;
    host_depth_store_pipeline_layout_create_info.pPushConstantRanges =
        &host_depth_store_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &host_depth_store_pipeline_layout_create_info, nullptr,
                                   &host_depth_store_pipeline_layout_) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the host depth storing "
          "pipeline layout");
      Shutdown();
      return false;
    }
    const std::pair<const uint32_t*, size_t> host_depth_store_shaders[] = {
        {shaders::host_depth_store_1xmsaa_cs, sizeof(shaders::host_depth_store_1xmsaa_cs)},
        {shaders::host_depth_store_2xmsaa_cs, sizeof(shaders::host_depth_store_2xmsaa_cs)},
        {shaders::host_depth_store_4xmsaa_cs, sizeof(shaders::host_depth_store_4xmsaa_cs)},
    };
    for (size_t i = 0; i < rex::countof(host_depth_store_shaders); ++i) {
      const std::pair<const uint32_t*, size_t> host_depth_store_shader =
          host_depth_store_shaders[i];
      VkPipeline host_depth_store_pipeline = ui::vulkan::util::CreateComputePipeline(
          vulkan_device, host_depth_store_pipeline_layout_, host_depth_store_shader.first,
          host_depth_store_shader.second);
      if (host_depth_store_pipeline == VK_NULL_HANDLE) {
        REXGPU_ERROR(
            "VulkanRenderTargetCache: Failed to create the {}-sample host "
            "depth storing pipeline",
            uint32_t(1) << i);
        Shutdown();
        return false;
      }
      host_depth_store_pipelines_[i] = host_depth_store_pipeline;
    }

    // Transfer and clear vertex buffer, for quads of up to tile granularity.
    transfer_vertex_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
        vulkan_device, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        std::max(
            ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
            sizeof(float) * 2 * 6 * Transfer::kMaxCutoutBorderRectangles * xenos::kEdramTileCount));

    // Transfer vertex shader.
    transfer_passthrough_vertex_shader_ =
        ui::vulkan::util::CreateShaderModule(vulkan_device, shaders::passthrough_position_xy_vs,
                                             sizeof(shaders::passthrough_position_xy_vs));
    if (transfer_passthrough_vertex_shader_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the render target "
          "ownership transfer vertex shader");
      Shutdown();
      return false;
    }

    // Transfer pipeline layouts.
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
    for (size_t i = 0; i < size_t(TransferPipelineLayoutIndex::kCount); ++i) {
      const TransferPipelineLayoutInfo& transfer_pipeline_layout_info =
          kTransferPipelineLayoutInfos[i];
      transfer_pipeline_layout_create_info.setLayoutCount = 0;
      uint32_t transfer_pipeline_layout_descriptor_sets_remaining =
          transfer_pipeline_layout_info.used_descriptor_sets;
      uint32_t transfer_pipeline_layout_descriptor_set_index;
      while (rex::bit_scan_forward(transfer_pipeline_layout_descriptor_sets_remaining,
                                   &transfer_pipeline_layout_descriptor_set_index)) {
        transfer_pipeline_layout_descriptor_sets_remaining &=
            ~(uint32_t(1) << transfer_pipeline_layout_descriptor_set_index);
        VkDescriptorSetLayout transfer_pipeline_layout_descriptor_set_layout = VK_NULL_HANDLE;
        switch (TransferUsedDescriptorSet(transfer_pipeline_layout_descriptor_set_index)) {
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
            assert_unhandled_case(
                TransferUsedDescriptorSet(transfer_pipeline_layout_descriptor_set_index));
        }
        transfer_pipeline_layout_descriptor_set_layouts[transfer_pipeline_layout_create_info
                                                            .setLayoutCount++] =
            transfer_pipeline_layout_descriptor_set_layout;
      }
      transfer_pipeline_layout_push_constant_range.size =
          uint32_t(sizeof(uint32_t) *
                   rex::bit_count(transfer_pipeline_layout_info.used_push_constant_dwords));
      transfer_pipeline_layout_create_info.pushConstantRangeCount =
          transfer_pipeline_layout_info.used_push_constant_dwords ? 1 : 0;
      if (dfn.vkCreatePipelineLayout(device, &transfer_pipeline_layout_create_info, nullptr,
                                     &transfer_pipeline_layouts_[i]) != VK_SUCCESS) {
        REXGPU_ERROR(
            "VulkanRenderTargetCache: Failed to create the render target "
            "ownership transfer pipeline layout {}",
            i);
        Shutdown();
        return false;
      }
    }

    // Dump pipeline layouts.
    VkDescriptorSetLayout dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetCount];
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetEdram] =
        descriptor_set_layout_storage_buffer_;
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_;
    VkPushConstantRange dump_pipeline_layout_push_constant_range;
    dump_pipeline_layout_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    dump_pipeline_layout_push_constant_range.offset = 0;
    dump_pipeline_layout_push_constant_range.size = sizeof(uint32_t) * kDumpPushConstantCount;
    VkPipelineLayoutCreateInfo dump_pipeline_layout_create_info;
    dump_pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    dump_pipeline_layout_create_info.pNext = nullptr;
    dump_pipeline_layout_create_info.flags = 0;
    dump_pipeline_layout_create_info.setLayoutCount =
        uint32_t(rex::countof(dump_pipeline_layout_descriptor_set_layouts));
    dump_pipeline_layout_create_info.pSetLayouts = dump_pipeline_layout_descriptor_set_layouts;
    dump_pipeline_layout_create_info.pushConstantRangeCount = 1;
    dump_pipeline_layout_create_info.pPushConstantRanges =
        &dump_pipeline_layout_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &dump_pipeline_layout_create_info, nullptr,
                                   &dump_pipeline_layout_color_) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the color render target "
          "dumping pipeline layout");
      Shutdown();
      return false;
    }
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_x2_;
    if (dfn.vkCreatePipelineLayout(device, &dump_pipeline_layout_create_info, nullptr,
                                   &dump_pipeline_layout_depth_) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the depth render target "
          "dumping pipeline layout");
      Shutdown();
      return false;
    }
  } else if (path_ == Path::kPixelShaderInterlock) {
    // Pixel (fragment) shader interlock.

    // Keep parity with D3D12 ROV, which uses 2x-as-4x in this path.
    msaa_2x_attachments_supported_ = false;
    msaa_2x_no_attachments_supported_ = false;

    // Blending is done in linear space directly in shaders.
    gamma_render_target_as_unorm16_ = false;

    // Always true float24 depth rounded to the nearest even.
    depth_float24_round_ = true;
    depth_float24_convert_in_pixel_shader_ = true;

    // The pipeline layout and the pipelines for clearing the EDRAM buffer in
    // resolves.
    VkPushConstantRange resolve_fsi_clear_push_constant_range;
    resolve_fsi_clear_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_fsi_clear_push_constant_range.offset = 0;
    resolve_fsi_clear_push_constant_range.size = sizeof(draw_util::ResolveClearShaderConstants);
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
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the resolve EDRAM buffer "
          "clear pipeline layout");
      Shutdown();
      return false;
    }
    resolve_fsi_clear_32bpp_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_fsi_clear_pipeline_layout_,
        draw_resolution_scaled ? shaders::resolve_clear_32bpp_scaled_cs
                               : shaders::resolve_clear_32bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_32bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_32bpp_cs));
    if (resolve_fsi_clear_32bpp_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the 32bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_fsi_clear_64bpp_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_fsi_clear_pipeline_layout_,
        draw_resolution_scaled ? shaders::resolve_clear_64bpp_scaled_cs
                               : shaders::resolve_clear_64bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_64bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_64bpp_cs));
    if (resolve_fsi_clear_64bpp_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the 64bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }

    // Common render pass.
    VkSubpassDescription fsi_subpass = {};
    fsi_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    // Fragment shader interlock provides synchronization and ordering within a
    // subpass, create an external by-region dependency to maintain interlocking
    // between passes. Framebuffer-global dependencies will be made with
    // explicit barriers when the addressing of the EDRAM buffer relatively to
    // the fragment coordinates is changed.
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
    fsi_render_pass_create_info.dependencyCount = uint32_t(rex::countof(fsi_subpass_dependencies));
    fsi_render_pass_create_info.pDependencies = fsi_subpass_dependencies;
    if (dfn.vkCreateRenderPass(device, &fsi_render_pass_create_info, nullptr, &fsi_render_pass_) !=
        VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the fragment shader "
          "interlock render backend render pass");
      Shutdown();
      return false;
    }

    // Common framebuffer.
    VkFramebufferCreateInfo fsi_framebuffer_create_info;
    fsi_framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fsi_framebuffer_create_info.pNext = nullptr;
    fsi_framebuffer_create_info.flags = 0;
    fsi_framebuffer_create_info.renderPass = fsi_render_pass_;
    fsi_framebuffer_create_info.attachmentCount = 0;
    fsi_framebuffer_create_info.pAttachments = nullptr;
    fsi_framebuffer_create_info.width =
        std::min(xenos::kTexture2DCubeMaxWidthHeight * draw_resolution_scale_x(),
                 device_properties.maxFramebufferWidth);
    fsi_framebuffer_create_info.height =
        std::min(xenos::kTexture2DCubeMaxWidthHeight * draw_resolution_scale_y(),
                 device_properties.maxFramebufferHeight);
    fsi_framebuffer_create_info.layers = 1;
    if (dfn.vkCreateFramebuffer(device, &fsi_framebuffer_create_info, nullptr,
                                &fsi_framebuffer_.framebuffer) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTargetCache: Failed to create the fragment shader "
          "interlock render backend framebuffer");
      Shutdown();
      return false;
    }
    fsi_framebuffer_.host_extent.width = fsi_framebuffer_create_info.width;
    fsi_framebuffer_.host_extent.height = fsi_framebuffer_create_info.height;
  } else {
    assert_unhandled_case(path_);
    Shutdown();
    return false;
  }

  // Reset the last update structures, to keep the defaults consistent between
  // paths regardless of whether the update for the path actually modifies them.
  last_update_render_pass_key_ = RenderPassKey();
  last_update_render_pass_ = VK_NULL_HANDLE;
  last_update_framebuffer_pitch_tiles_at_32bpp_ = 0;
  std::memset(last_update_framebuffer_attachments_, 0,
              sizeof(last_update_framebuffer_attachments_));
  last_update_framebuffer_ = VK_NULL_HANDLE;

  InitializeCommon();
  return true;
}

void VulkanRenderTargetCache::Shutdown(bool from_destructor) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Destroy all render targets before the descriptor set pool is destroyed -
  // may happen if shutting down the VulkanRenderTargetCache by destroying it,
  // so ShutdownCommon is called by the RenderTargetCache destructor, when it's
  // already too late.
  DestroyAllRenderTargets(true);
  DestroyDirectConvertResources();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_fsi_clear_64bpp_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_fsi_clear_32bpp_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_fsi_clear_pipeline_layout_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                         fsi_framebuffer_.framebuffer);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device, fsi_render_pass_);

  for (const auto& dump_pipeline_pair : dump_pipelines_) {
    // May be null to prevent recreation attempts.
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
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         direct_resolve_pipeline_layout_depth_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         direct_resolve_pipeline_layout_color_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         dump_pipeline_layout_depth_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         dump_pipeline_layout_color_);

  for (const auto& transfer_pipeline_array_pair : transfer_pipelines_) {
    for (VkPipeline transfer_pipeline : transfer_pipeline_array_pair.second) {
      // May be null to prevent recreation attempts.
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
  for (size_t i = 0; i < size_t(TransferPipelineLayoutIndex::kCount); ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                           transfer_pipeline_layouts_[i]);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         transfer_passthrough_vertex_shader_);
  transfer_vertex_buffer_pool_.reset();

  for (size_t i = 0; i < rex::countof(host_depth_store_pipelines_); ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                           host_depth_store_pipelines_[i]);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
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
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, resolve_copy_pipeline);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_copy_pipeline_layout_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         edram_storage_buffer_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, edram_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, edram_buffer_memory_);

  descriptor_set_pool_sampled_image_x2_.reset();
  descriptor_set_pool_sampled_image_.reset();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_sampled_image_x2_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_sampled_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_storage_buffer_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanRenderTargetCache::ClearCache() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Framebuffer objects must be destroyed because they reference views of
  // attachment images, which may be removed by the common ClearCache.
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

bool VulkanRenderTargetCache::Resolve(const memory::Memory& memory,
                                      VulkanSharedMemory& shared_memory,
                                      VulkanTextureCache& texture_cache,
                                      uint32_t& written_address_out, uint32_t& written_length_out,
                                      bool memory_copy_required) {
  written_address_out = 0;
  written_length_out = 0;

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  draw_util::ResolveInfo resolve_info;
  if (!draw_util::GetResolveInfo(register_file(), memory, draw_resolution_scale_x(),
                                 draw_resolution_scale_y(), IsFixedRG16TruncatedToMinus1To1(),
                                 IsFixedRGBA16TruncatedToMinus1To1(), resolve_info)) {
    return false;
  }

  // Nothing to copy/clear.
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }

  if (g_frame_dump.active) {
    ++g_frame_dump.frame_resolves;
    const bool depth_source = resolve_info.IsCopyingDepth();
    const draw_util::ResolveEdramInfo edram_info =
        depth_source ? resolve_info.depth_edram_info : resolve_info.color_edram_info;
    FrameDumpEvent(fmt::format(
        "resolve: source {} {} @ {}t <{}t> {}xMSAA, rectangle {}x{} at +{},{}, samples {}; "
        "destination {:08X} (written {:08X}+{:X}) format {} endian {} swap {} exp bias {} number "
        "{}, texture {}x{}, at {},{}{}{}{}",
        depth_source ? "depth" : "color",
        depth_source
            ? xenos::GetDepthRenderTargetFormatName(
                  xenos::DepthRenderTargetFormat(edram_info.format))
            : xenos::GetColorRenderTargetFormatName(
                  xenos::ColorRenderTargetFormat(edram_info.format)),
        uint32_t(edram_info.base_tiles), uint32_t(edram_info.pitch_tiles),
        uint32_t(1) << uint32_t(edram_info.msaa_samples),
        uint32_t(resolve_info.coordinate_info.width_div_8) * 8, resolve_info.height_div_8 * 8,
        uint32_t(resolve_info.coordinate_info.edram_offset_x_div_8) * 8,
        uint32_t(resolve_info.coordinate_info.edram_offset_y_div_8) * 8,
        uint32_t(resolve_info.copy_dest_coordinate_info.copy_sample_select),
        resolve_info.copy_dest_base, resolve_info.copy_dest_extent_start,
        resolve_info.copy_dest_extent_length,
        uint32_t(resolve_info.copy_dest_info.copy_dest_format),
        uint32_t(resolve_info.copy_dest_info.copy_dest_endian),
        uint32_t(resolve_info.copy_dest_info.copy_dest_swap),
        int32_t(resolve_info.copy_dest_info.copy_dest_exp_bias),
        uint32_t(resolve_info.copy_dest_info.copy_dest_number),
        uint32_t(resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32,
        uint32_t(resolve_info.copy_dest_coordinate_info.height_aligned_div_32) * 32,
        uint32_t(resolve_info.copy_dest_coordinate_info.offset_x_div_8) * 8,
        uint32_t(resolve_info.copy_dest_coordinate_info.offset_y_div_8) * 8,
        resolve_info.copy_dest_extent_length ? "" : ", no copy",
        resolve_info.IsClearingColor() ? ", clears color" : "",
        resolve_info.IsClearingDepth() ? ", clears depth" : ""));
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

  // The resolves of a frame whose two tiles were merged
  // (GetTileMergeDrawAction): the second tile's rows are below the first
  // tile's in the host color render target, not at the top of EDRAM.
  bool tile_merge_second_resolve = false;
  if (tile_merge_.merged_frame && resolve_info.copy_dest_extent_length &&
      tile_merge_.merged_frame == command_processor_.GetCurrentFrame()) {
    const RegisterFile& regs = register_file();
    const uint32_t resolve_height = resolve_info.height_div_8 << xenos::kResolveAlignmentPixelsLog2;
    const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
    const uint32_t tile_height_pixels =
        xenos::kEdramTileHeightSamples >>
        uint32_t(surface_info.msaa_samples >= xenos::MsaaSamples::k2X);
    const uint32_t second_tile_height =
        tile_merge_.frame_height - tile_merge_.first_tile_height;
    uint32_t layout_surface, layout_color, layout_depth;
    GetTileMergeLayout(layout_surface, layout_color, layout_depth);
    if (!resolve_info.IsCopyingDepth() && resolve_info.rb_copy_control.copy_src_select == 0 &&
        layout_surface == tile_merge_.surface_info && layout_color == tile_merge_.color_info &&
        !resolve_info.coordinate_info.edram_offset_y_div_8) {
      if (!tile_merge_.first_tile_resolved) {
        if (resolve_height == tile_merge_.first_tile_height) {
          tile_merge_.first_tile_resolved = true;
        }
      } else if (resolve_height >= second_tile_height &&
                 resolve_height < second_tile_height + xenos::kResolveAlignmentPixels &&
                 !(tile_merge_.first_tile_height % tile_height_pixels)) {
        tile_merge_second_resolve = true;
        RenderTargetKey color_key;
        color_key.base_tiles = resolve_info.color_original_base;
        color_key.pitch_tiles_at_32bpp =
            resolve_info.color_edram_info.pitch_tiles >>
            uint32_t(resolve_info.color_edram_info.format_is_64bpp);
        color_key.msaa_samples = resolve_info.color_edram_info.msaa_samples;
        color_key.is_depth = 0;
        color_key.resource_format = uint32_t(GetColorKeyFormat(
            xenos::ColorRenderTargetFormat(resolve_info.color_edram_info.format)));
        dump_source_shift_key_ = color_key;
        dump_source_shift_tiles_ = (tile_merge_.first_tile_height / tile_height_pixels) *
                                   resolve_info.color_edram_info.pitch_tiles;
      }
    }
  }
  struct DumpSourceShiftReset {
    uint32_t& shift;
    ~DumpSourceShiftReset() { shift = 0; }
  } dump_source_shift_reset{dump_source_shift_tiles_};

  // Direct resolve: a single-format copy of one host render target's
  // rectangle straight into the host textures that hold this data.
  VulkanRenderTarget* direct_source = nullptr;
  int32_t direct_source_x = 0, direct_source_y = 0;
  bool direct_skip_memory = false;
  bool direct_invalidate_others = false;
  const int32_t direct_mode = REXCVAR_GET(resolve_direct_to_texture);
  DeferredCommandBuffer::ImageCopyMode direct_copy_mode =
      DeferredCommandBuffer::ImageCopyMode::kCopy;
  // Source formats whose host image holds 8 bits per component, and
  // destination formats of 8 or 10 bits per component: converting between
  // them by a blit gives the same values as the path through memory.
  const auto direct_source_format =
      xenos::ColorRenderTargetFormat(resolve_info.color_edram_info.format);
  const bool direct_source_format_usable =
      !resolve_info.IsCopyingDepth() &&
      (direct_source_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
       (direct_source_format == xenos::ColorRenderTargetFormat::k_2_10_10_10 &&
        GetColorVulkanFormat(direct_source_format) == VK_FORMAT_A8B8G8R8_UNORM_PACK32));
  const bool direct_same_format =
      direct_source_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 &&
      resolve_info.copy_dest_info.copy_dest_format == xenos::ColorFormat::k_8_8_8_8;
  if (direct_mode > 0 && GetPath() == Path::kHostRenderTargets && !draw_resolution_scaled &&
      resolve_info.copy_dest_extent_length && direct_source_format_usable &&
      !tile_merge_second_resolve) {
    const draw_util::ResolveEdramInfo edram_info = resolve_info.color_edram_info;
    const xenos::CopySampleSelect sample_select =
        resolve_info.copy_dest_coordinate_info.copy_sample_select;
    // Multisampled sources only by averaging all samples, without a format
    // change.
    const bool samples_usable =
        edram_info.msaa_samples == xenos::MsaaSamples::k1X ||
        (direct_mode >= 2 && direct_same_format &&
         ((edram_info.msaa_samples == xenos::MsaaSamples::k2X &&
           sample_select == xenos::CopySampleSelect::k01) ||
          (edram_info.msaa_samples == xenos::MsaaSamples::k4X &&
           sample_select == xenos::CopySampleSelect::k0123)));
    direct_copy_mode = edram_info.msaa_samples != xenos::MsaaSamples::k1X
                           ? DeferredCommandBuffer::ImageCopyMode::kResolve
                       : direct_same_format ? DeferredCommandBuffer::ImageCopyMode::kCopy
                                            : DeferredCommandBuffer::ImageCopyMode::kBlit;
    uint32_t span_base, span_row_length, span_rows, span_pitch;
    resolve_info.GetCopyEdramTileSpan(span_base, span_row_length, span_rows, span_pitch);
    GetResolveCopyRectanglesToDump(span_base, span_row_length, span_rows, span_pitch,
                                   dump_rectangles_);
    // One render target must own the whole source area, with the layout the
    // resolve assumes.
    if (samples_usable && dump_rectangles_.size() == 1) {
      const ResolveCopyDumpRectangle& rectangle = dump_rectangles_.front();
      auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
      const RenderTargetKey rt_key = vulkan_rt.key();
      const uint32_t width = uint32_t(resolve_info.coordinate_info.width_div_8)
                             << xenos::kResolveAlignmentPixelsLog2;
      const uint32_t height = resolve_info.height_div_8 << xenos::kResolveAlignmentPixelsLog2;
      if (!rectangle.row_first && rectangle.rows == span_rows && !rectangle.row_first_start &&
          rectangle.row_last_end == span_row_length && !rt_key.is_depth &&
          rt_key.GetColorFormat() == direct_source_format &&
          rt_key.msaa_samples == edram_info.msaa_samples &&
          rt_key.GetPitchTiles() == edram_info.pitch_tiles &&
          edram_info.base_tiles >= rt_key.base_tiles) {
        // (Destination formats other than 8888 and 2:10:10:10, such as
        // 1:5:5:5, are reached by a blit as well - see direct_same_format.)
        const uint32_t tile_offset = edram_info.base_tiles - rt_key.base_tiles;
        const uint32_t tile_width_pixels =
            xenos::kEdramTileWidthSamples >>
            uint32_t(rt_key.msaa_samples >= xenos::MsaaSamples::k4X);
        const uint32_t tile_height_pixels =
            xenos::kEdramTileHeightSamples >>
            uint32_t(rt_key.msaa_samples >= xenos::MsaaSamples::k2X);
        const uint32_t source_x =
            (tile_offset % edram_info.pitch_tiles) * tile_width_pixels +
            (uint32_t(resolve_info.coordinate_info.edram_offset_x_div_8)
             << xenos::kResolveAlignmentPixelsLog2);
        const uint32_t source_y =
            (tile_offset / edram_info.pitch_tiles) * tile_height_pixels +
            (uint32_t(resolve_info.coordinate_info.edram_offset_y_div_8)
             << xenos::kResolveAlignmentPixelsLog2);
        bool only_dependents = false;
        if (source_x + width <= rt_key.GetWidth() &&
            source_y + height <=
                GetRenderTargetHeight(rt_key.pitch_tiles_at_32bpp, rt_key.msaa_samples) &&
            texture_cache.PrepareDirectResolve(resolve_info, width, height, only_dependents)) {
          direct_source = &vulkan_rt;
          direct_source_x = int32_t(source_x);
          direct_source_y = int32_t(source_y);
          const int32_t skip_mode = REXCVAR_GET(resolve_direct_skip_memory);
          direct_skip_memory =
              !memory_copy_required && (skip_mode >= 2 || (skip_mode == 1 && only_dependents));
          direct_invalidate_others = direct_skip_memory && !only_dependents;
        }
      }
    }
  }

  // 2:10:10:10 float color and float24 depth (resolve_direct_hdr_depth):
  // converted by a compute shader into a scratch image, then copied. The
  // second tile of a merged frame too: its rows are below the first tile's in
  // the render target, and its destination continues the first tile's
  // texture.
  uint32_t direct_convert_mode = UINT32_MAX;
  uint32_t direct_convert_width = 0, direct_convert_height = 0;
  int32_t direct_convert_samples[2] = {0, -1};
  if (!direct_source && direct_mode > 0 && REXCVAR_GET(resolve_direct_hdr_depth) &&
      GetPath() == Path::kHostRenderTargets && !draw_resolution_scaled &&
      resolve_info.copy_dest_extent_length && !resolve_info.copy_dest_info.copy_dest_exp_bias &&
      !resolve_info.copy_dest_info.copy_dest_array) {
    const bool copying_depth = resolve_info.IsCopyingDepth();
    const uint32_t dest_format = uint32_t(resolve_info.copy_dest_info.copy_dest_format);
    const draw_util::ResolveEdramInfo edram_info =
        copying_depth ? resolve_info.depth_edram_info : resolve_info.color_edram_info;
    uint32_t mode = UINT32_MAX;
    if (copying_depth) {
      if (xenos::DepthRenderTargetFormat(edram_info.format) ==
              xenos::DepthRenderTargetFormat::kD24FS8 &&
          dest_format == uint32_t(xenos::TextureFormat::k_24_8_FLOAT)) {
        mode = kDirectConvertDepth;
      }
    } else if (xenos::ColorRenderTargetFormat(edram_info.format) ==
                   xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT &&
               dest_format == uint32_t(xenos::ColorFormat::k_16_16_16_16_FLOAT)) {
      mode = kDirectConvertHdr;
    }
    // The samples: one, or (color only) the average of both of 2x.
    bool samples_ok = false;
    const xenos::CopySampleSelect sample_select =
        resolve_info.copy_dest_coordinate_info.copy_sample_select;
    if (mode != UINT32_MAX) {
      if (edram_info.msaa_samples == xenos::MsaaSamples::k1X) {
        samples_ok = true;
        direct_convert_samples[0] = 0;
      } else if (edram_info.msaa_samples == xenos::MsaaSamples::k2X &&
                 msaa_2x_attachments_supported_) {
        const int32_t host_0 =
            int32_t(draw_util::GetD3D10SampleIndexForGuest2xMSAA(0, true));
        const int32_t host_1 =
            int32_t(draw_util::GetD3D10SampleIndexForGuest2xMSAA(1, true));
        if (sample_select == xenos::CopySampleSelect::k0) {
          samples_ok = true;
          direct_convert_samples[0] = host_0;
        } else if (sample_select == xenos::CopySampleSelect::k1) {
          samples_ok = true;
          direct_convert_samples[0] = host_1;
        } else if (sample_select == xenos::CopySampleSelect::k01 &&
                   mode == kDirectConvertHdr) {
          samples_ok = true;
          direct_convert_samples[0] = host_0;
          direct_convert_samples[1] = host_1;
        }
      }
    }
    if (samples_ok) {
      uint32_t span_base, span_row_length, span_rows, span_pitch;
      resolve_info.GetCopyEdramTileSpan(span_base, span_row_length, span_rows, span_pitch);
      GetResolveCopyRectanglesToDump(span_base, span_row_length, span_rows, span_pitch,
                                     dump_rectangles_);
      if (dump_rectangles_.size() == 1) {
        const ResolveCopyDumpRectangle& rectangle = dump_rectangles_.front();
        auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
        const RenderTargetKey rt_key = vulkan_rt.key();
        const bool format_matches =
            copying_depth ? (rt_key.is_depth &&
                             rt_key.GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24FS8)
                          : (!rt_key.is_depth &&
                             rt_key.GetColorFormat() ==
                                 GetColorKeyFormat(
                                     xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT));
        const uint32_t width = uint32_t(resolve_info.coordinate_info.width_div_8)
                               << xenos::kResolveAlignmentPixelsLog2;
        const uint32_t height = resolve_info.height_div_8 << xenos::kResolveAlignmentPixelsLog2;
        if (!rectangle.row_first && rectangle.rows == span_rows && !rectangle.row_first_start &&
            rectangle.row_last_end == span_row_length && format_matches &&
            rt_key.msaa_samples == edram_info.msaa_samples &&
            rt_key.GetPitchTiles() == edram_info.pitch_tiles &&
            edram_info.base_tiles >= rt_key.base_tiles) {
          const uint32_t tile_offset = edram_info.base_tiles - rt_key.base_tiles;
          const uint32_t tile_width_pixels =
              xenos::kEdramTileWidthSamples >>
              uint32_t(rt_key.msaa_samples >= xenos::MsaaSamples::k4X);
          const uint32_t tile_height_pixels =
              xenos::kEdramTileHeightSamples >>
              uint32_t(rt_key.msaa_samples >= xenos::MsaaSamples::k2X);
          uint32_t source_x = (tile_offset % edram_info.pitch_tiles) * tile_width_pixels +
                              (uint32_t(resolve_info.coordinate_info.edram_offset_x_div_8)
                               << xenos::kResolveAlignmentPixelsLog2);
          uint32_t source_y = (tile_offset / edram_info.pitch_tiles) * tile_height_pixels +
                              (uint32_t(resolve_info.coordinate_info.edram_offset_y_div_8)
                               << xenos::kResolveAlignmentPixelsLog2);
          if (tile_merge_second_resolve) {
            source_y += tile_merge_.first_tile_height;
          }
          bool only_dependents = false;
          if (source_x + width <= rt_key.GetWidth() &&
              source_y + height <=
                  GetRenderTargetHeight(rt_key.pitch_tiles_at_32bpp, rt_key.msaa_samples) &&
              width <= kDirectConvertImageSize && height <= kDirectConvertImageSize &&
              texture_cache.PrepareDirectResolve(resolve_info, width, height, only_dependents)) {
            direct_source = &vulkan_rt;
            direct_source_x = int32_t(source_x);
            direct_source_y = int32_t(source_y);
            direct_convert_mode = mode;
            direct_convert_width = width;
            direct_convert_height = height;
            const int32_t skip_mode = REXCVAR_GET(resolve_direct_skip_memory);
            direct_skip_memory =
                !memory_copy_required && (skip_mode >= 2 || (skip_mode == 1 && only_dependents));
            direct_invalidate_others = direct_skip_memory && !only_dependents;
          }
        }
      }
    }
  }

  // Copying.
  bool copied = false;
  if (resolve_info.copy_dest_extent_length && !direct_skip_memory) {
    draw_util::ResolveCopyShaderConstants copy_shader_constants;
    uint32_t copy_group_count_x, copy_group_count_y;
    draw_util::ResolveCopyShaderIndex copy_shader =
        resolve_info.GetCopyShader(draw_resolution_scale_x(), draw_resolution_scale_y(),
                                   copy_shader_constants, copy_group_count_x, copy_group_count_y);
    assert_true(copy_group_count_x && copy_group_count_y);
    if (copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown) {
      const draw_util::ResolveCopyShaderInfo& copy_shader_info =
          draw_util::resolve_copy_shader_info[size_t(copy_shader)];
      bool direct_resolved = false;
      CountFrameEvent(g_frame_events.resolves);
      if (GetPath() == Path::kHostRenderTargets) {
        // GPU time breakdown: copying the host render targets to the EDRAM
        // buffer.
        command_processor_.SetGpuTimeCategory(VulkanCommandProcessor::kGpuTimeResolveDump);
        {
          uint32_t span_base, span_row_length, span_rows, span_pitch;
          resolve_info.GetCopyEdramTileSpan(span_base, span_row_length, span_rows, span_pitch);
          CountFrameEvent(g_frame_events.resolve_dump_tiles, uint64_t(span_row_length) * span_rows);
        }
        if (REXCVAR_GET(direct_host_resolve)) {
          direct_resolved =
              TryResolveCopyDirectly(resolve_info, copy_shader, draw_resolution_scaled);
          if (direct_resolved) {
            ++direct_resolve_success_count_;
          } else {
            ++direct_resolve_fallback_count_;
          }
        }
        if (!direct_resolved) {
          // Dump the current contents of the render targets owning the affected
          // range to edram_buffer_.
          uint32_t dump_base;
          uint32_t dump_row_length_used;
          uint32_t dump_rows;
          uint32_t dump_pitch;
          resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
          if (!DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch)) {
            REXGPU_ERROR("VulkanRenderTargetCache: Failed to dump host render targets for resolve");
            return false;
          }
        }
      }

      uint32_t copy_dest_range_unscaled = resolve_info.copy_dest_extent_start -
                                          resolve_info.copy_dest_base +
                                          resolve_info.copy_dest_extent_length;
      uint64_t copy_dest_base = resolve_info.copy_dest_base;
      uint64_t copy_dest_range_length = copy_dest_range_unscaled;
      uint64_t copy_dest_use_start = resolve_info.copy_dest_extent_start;
      uint64_t copy_dest_use_length = resolve_info.copy_dest_extent_length;
      if (draw_resolution_scaled) {
        if (!texture_cache.GetScaledResolveRange(
                resolve_info.copy_dest_base, copy_dest_range_unscaled,
                copy_shader_info.dest_bpe_log2, copy_dest_base, copy_dest_range_length) ||
            !texture_cache.GetScaledResolveRange(
                resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length,
                copy_shader_info.dest_bpe_log2, copy_dest_use_start, copy_dest_use_length)) {
          REXGPU_ERROR(
              "VulkanRenderTargetCache: Failed to map scaled resolve "
              "destination range (base={:08X}, length={:08X})",
              resolve_info.copy_dest_base, copy_dest_range_unscaled);
          return false;
        }
      }

      // Make sure there is memory to write to.
      bool copy_dest_committed;
      if (draw_resolution_scaled) {
        copy_dest_committed = texture_cache.CommitScaledResolveRange(
            resolve_info.copy_dest_base, copy_dest_range_unscaled, copy_shader_info.dest_bpe_log2);
      } else {
        copy_dest_committed = shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                                         resolve_info.copy_dest_extent_length);
      }
      if (!copy_dest_committed) {
        REXGPU_ERROR(
            "VulkanRenderTargetCache: Failed to obtain the resolve destination "
            "memory region");
      } else {
        // TODO(Triang3l): Switching between descriptors if exceeding
        // maxStorageBufferRange.
        // TODO(Triang3l): Use a single 512 MB shared memory binding if
        // possible.
        VkDescriptorSet descriptor_set_dest = command_processor_.AllocateSingleTransientDescriptor(
            VulkanCommandProcessor::SingleTransientDescriptorLayout ::kStorageBufferCompute);
        if (descriptor_set_dest != VK_NULL_HANDLE) {
          // Write the destination descriptor.
          VkDescriptorBufferInfo write_descriptor_set_dest_buffer_info;
          write_descriptor_set_dest_buffer_info.buffer = draw_resolution_scaled
                                                             ? texture_cache.scaled_resolve_buffer()
                                                             : shared_memory.buffer();
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

          // Submit the resolve.
          command_processor_.SetGpuTimeCategory(VulkanCommandProcessor::kGpuTimeResolve);
          if (draw_resolution_scaled) {
            texture_cache.UseScaledResolveBufferForWrite(copy_dest_use_start, copy_dest_use_length);
          } else {
            shared_memory.Use(VulkanSharedMemory::Usage::kComputeWrite,
                              std::pair<uint32_t, uint32_t>(uint32_t(copy_dest_use_start),
                                                            uint32_t(copy_dest_use_length)));
          }
          UseEdramBuffer(EdramBufferUsage::kComputeRead);
          command_processor_.BindExternalComputePipeline(
              resolve_copy_pipelines_[size_t(copy_shader)]);
          VkDescriptorSet descriptor_sets[kResolveCopyDescriptorSetCount] = {};
          descriptor_sets[kResolveCopyDescriptorSetEdram] = edram_storage_buffer_descriptor_set_;
          descriptor_sets[kResolveCopyDescriptorSetDest] = descriptor_set_dest;
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_COMPUTE, resolve_copy_pipeline_layout_, 0,
              uint32_t(rex::countof(descriptor_sets)), descriptor_sets, 0, nullptr);
          if (draw_resolution_scaled) {
            command_buffer.CmdVkPushConstants(
                resolve_copy_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(copy_shader_constants.dest_relative), &copy_shader_constants.dest_relative);
          } else {
            // TODO(Triang3l): Proper dest_base in case of one 512 MB shared
            // memory binding, or multiple shared memory bindings in case of
            // splitting due to maxStorageBufferRange overflow.
            copy_shader_constants.dest_base -=
                uint32_t(write_descriptor_set_dest_buffer_info.offset);
            command_buffer.CmdVkPushConstants(
                resolve_copy_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(copy_shader_constants), &copy_shader_constants);
          }
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(copy_group_count_x, copy_group_count_y, 1);

          // Invalidate textures and mark the range as scaled if needed.
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

  if (direct_source && direct_convert_mode != UINT32_MAX) {
    // Converted into the scratch image, then copied from there.
    command_processor_.SetGpuTimeCategory(VulkanCommandProcessor::kGpuTimeResolve);
    VkImage converted = RunDirectConvert(
        direct_convert_mode, *direct_source, direct_source_x, direct_source_y,
        direct_convert_width, direct_convert_height, direct_convert_samples[0],
        direct_convert_samples[1]);
    if (converted == VK_NULL_HANDLE) {
      // Too late to take the path through memory if it was skipped: this
      // only happens if the shader or the scratch image can't be created,
      // the first time.
      REXGPU_ERROR("VulkanRenderTargetCache: direct HDR/depth resolve conversion failed");
    } else {
      texture_cache.CommitDirectResolve(resolve_info, converted,
                                        DeferredCommandBuffer::ImageCopyMode::kCopy, 0, 0,
                                        direct_invalidate_others);
    }
  } else if (direct_source) {
    command_processor_.SetGpuTimeCategory(VulkanCommandProcessor::kGpuTimeResolve);
    VulkanRenderTarget& vulkan_rt = *direct_source;
    command_processor_.PushImageMemoryBarrier(
        vulkan_rt.image(), ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT),
        vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
        vulkan_rt.current_access_mask(), VK_ACCESS_TRANSFER_READ_BIT, vulkan_rt.current_layout(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vulkan_rt.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    texture_cache.CommitDirectResolve(resolve_info, vulkan_rt.image(), direct_copy_mode,
                                      direct_source_x, direct_source_y, direct_invalidate_others);
  }
  if (direct_source) {
    CountFrameEvent(g_frame_events.resolves_direct);
    if (direct_skip_memory) {
      // Not counted by the copy to memory.
      CountFrameEvent(g_frame_events.resolves);
      CountFrameEvent(g_frame_events.resolves_direct_memory_skipped);
    }
    if (g_frame_dump.active) {
      ++g_frame_dump.frame_direct_resolves;
      FrameDumpEvent(fmt::format("  -> copied directly to {} texture(s), memory copy {}",
                                 texture_cache.direct_resolve_texture_count(),
                                 direct_invalidate_others ? "skipped (other textures outdated)"
                                 : direct_skip_memory     ? "skipped"
                                                          : "kept"));
    }
  }

  // Clearing.
  bool cleared = false;
  bool clear_depth = resolve_info.IsClearingDepth();
  bool clear_color = resolve_info.IsClearingColor();
  if (clear_depth || clear_color) {
    CountFrameEvent(g_frame_events.resolve_clears);
    command_processor_.SetGpuTimeCategory(VulkanCommandProcessor::kGpuTimeResolveClear);
    switch (GetPath()) {
      case Path::kHostRenderTargets: {
        Transfer::Rectangle clear_rectangle;
        RenderTarget* clear_render_targets[2];
        // If PrepareHostRenderTargetsResolveClear returns false, may be just an
        // empty region (success) or an error - don't care.
        if (PrepareHostRenderTargetsResolveClear(resolve_info, clear_rectangle,
                                                 clear_render_targets[0], clear_transfers_[0],
                                                 clear_render_targets[1], clear_transfers_[1])) {
          uint64_t clear_values[2];
          clear_values[0] = resolve_info.rb_depth_clear;
          clear_values[1] =
              resolve_info.rb_color_clear | (uint64_t(resolve_info.rb_color_clear_lo) << 32);
          PerformTransfersAndResolveClears(2, clear_render_targets, clear_transfers_, clear_values,
                                           &clear_rectangle);
          if (tile_merge_second_resolve && clear_render_targets[1]) {
            // The same for the rows the second tile was really drawn to.
            Transfer::Rectangle merged_rectangle = clear_rectangle;
            merged_rectangle.y_pixels += tile_merge_.first_tile_height;
            const RenderTargetKey merged_key = clear_render_targets[1]->key();
            const uint32_t merged_height_limit =
                GetRenderTargetHeight(merged_key.pitch_tiles_at_32bpp, merged_key.msaa_samples);
            if (merged_rectangle.y_pixels < merged_height_limit) {
              merged_rectangle.height_pixels = std::min(
                  merged_rectangle.height_pixels, merged_height_limit - merged_rectangle.y_pixels);
              clear_transfers_[0].clear();
              PerformTransfersAndResolveClears(1, &clear_render_targets[1], clear_transfers_,
                                               &clear_values[1], &merged_rectangle);
            }
          }
        }

        cleared = true;
      } break;
      case Path::kPixelShaderInterlock: {
        UseEdramBuffer(EdramBufferUsage::kComputeWrite);
        // Should be safe to only commit once (if was accessed as unordered or
        // with fragment shader interlock previously - if there was nothing to
        // copy, only to clear, for some reason, for instance), overlap of the
        // depth and the color ranges is highly unlikely.
        CommitEdramBufferShaderWrites();
        command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                               resolve_fsi_clear_pipeline_layout_, 0, 1,
                                               &edram_storage_buffer_descriptor_set_, 0, nullptr);
        std::pair<uint32_t, uint32_t> clear_group_count = resolve_info.GetClearShaderGroupCount(
            draw_resolution_scale_x(), draw_resolution_scale_y());
        assert_true(clear_group_count.first && clear_group_count.second);
        if (clear_depth) {
          command_processor_.BindExternalComputePipeline(resolve_fsi_clear_32bpp_pipeline_);
          draw_util::ResolveClearShaderConstants depth_clear_constants;
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
          draw_util::ResolveClearShaderConstants color_clear_constants;
          resolve_info.GetColorClearShaderConstants(color_clear_constants);
          if (clear_depth) {
            // Non-RT-specific constants have already been set.
            command_buffer.CmdVkPushConstants(
                resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                uint32_t(offsetof(draw_util::ResolveClearShaderConstants, rt_specific)),
                sizeof(color_clear_constants.rt_specific), &color_clear_constants.rt_specific);
          } else {
            command_buffer.CmdVkPushConstants(
                resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(color_clear_constants), &color_clear_constants);
          }
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(clear_group_count.first, clear_group_count.second, 1);
        }
        MarkEdramBufferModified();
        cleared = true;
      } break;
      default:
        assert_unhandled_case(GetPath());
    }
  } else {
    cleared = true;
  }

  if (tile_merge_second_resolve) {
    // Done with this frame's tiles.
    tile_merge_.merged_frame = 0;
  }

  return copied && cleared;
}

uint32_t VulkanRenderTargetCache::SkipTransfersOverwrittenByDraw(
    const Shader& vertex_shader, RenderTarget* const* render_targets,
    std::vector<Transfer>* transfers) {
  bool any_color_transfers = false;
  for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    any_color_transfers |= render_targets[i] && !transfers[i].empty();
  }
  const Shader* pixel_shader = next_update_pixel_shader_;
  if (!any_color_transfers || !pixel_shader) {
    return 0;
  }
  const RegisterFile& regs = register_file();

  // Whole-draw conditions: every covered sample of every render target gets
  // the pixel shader's output - nothing can reject a pixel or a sample.
  if (pixel_shader->kills_pixels()) {
    return 0;
  }
  // A culled rectangle draws nothing; wireframe draws only the edges.
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  // (Only kDualMode selects point or line polygons; other values are
  // triangles, see xenos::PolygonModeEnable.)
  if (pa_su_sc_mode_cntl.cull_front || pa_su_sc_mode_cntl.cull_back ||
      pa_su_sc_mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode) {
    return 0;
  }
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  if ((rb_colorcontrol.alpha_test_enable &&
       rb_colorcontrol.alpha_func != xenos::CompareFunction::kAlways) ||
      rb_colorcontrol.alpha_to_mask_enable) {
    return 0;
  }
  const reg::RB_DEPTHCONTROL depth_control = next_update_depth_control_;
  if ((depth_control.z_enable && depth_control.zfunc != xenos::CompareFunction::kAlways) ||
      (depth_control.stencil_enable &&
       (depth_control.stencilfunc != xenos::CompareFunction::kAlways ||
        (depth_control.backface_enable &&
         depth_control.stencilfunc_bf != xenos::CompareFunction::kAlways)))) {
    return 0;
  }
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const xenos::MsaaSamples msaa_samples = rb_surface_info.msaa_samples;
  if (msaa_samples != xenos::MsaaSamples::k1X &&
      (regs[XE_GPU_REG_PA_SC_AA_MASK] & 0xF) != 0xF) {
    return 0;
  }
  float left, top, right, bottom;
  if (!draw_extent_estimator().EstimateRectangle(vertex_shader, left, top, right, bottom)) {
    return 0;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, false);

  const uint32_t surface_pitch = rb_surface_info.surface_pitch;
  const uint32_t tile_height_pixels =
      xenos::kEdramTileHeightSamples >> uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  const uint32_t rb_color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  uint32_t skipped = 0;
  for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    RenderTarget* render_target = render_targets[i];
    std::vector<Transfer>& rt_transfers = transfers[i];
    if (!render_target || rt_transfers.empty()) {
      continue;
    }
    const uint32_t rt_index = i - 1;
    if (!pixel_shader->writes_color_target(rt_index)) {
      continue;
    }
    // All components of the format written, without blending.
    auto color_info =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[rt_index]);
    const uint32_t component_mask =
        (uint32_t(1) << xenos::GetColorRenderTargetFormatComponentCount(color_info.color_format)) -
        1;
    if (((rb_color_mask >> (4 * rt_index)) & component_mask) != component_mask) {
      continue;
    }
    auto blend_control =
        regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[rt_index]);
    if (blend_control.color_srcblend != xenos::BlendFactor::kOne ||
        blend_control.color_destblend != xenos::BlendFactor::kZero ||
        blend_control.color_comb_fcn != xenos::BlendOp::kAdd ||
        blend_control.alpha_srcblend != xenos::BlendFactor::kOne ||
        blend_control.alpha_destblend != xenos::BlendFactor::kZero ||
        blend_control.alpha_comb_fcn != xenos::BlendOp::kAdd) {
      continue;
    }
    // The rectangle and the scissor must cover every row of tiles the
    // transfers would copy, across the surface width.
    const uint32_t pitch_tiles = render_target->key().GetPitchTiles();
    uint32_t end_tiles = 0;
    for (const Transfer& transfer : rt_transfers) {
      end_tiles = std::max(end_tiles, transfer.end_tiles);
    }
    const uint32_t rows_pixels = (end_tiles + pitch_tiles - 1) / pitch_tiles * tile_height_pixels;
    if (!(left <= 0.0f && top <= 0.0f && right >= float(surface_pitch) &&
          bottom >= float(rows_pixels))) {
      continue;
    }
    if (scissor.offset[0] || scissor.offset[1] || scissor.extent[0] < surface_pitch ||
        scissor.extent[1] < rows_pixels) {
      continue;
    }
    skipped += uint32_t(rt_transfers.size());
    rt_transfers.clear();
  }
  return skipped;
}

void VulkanRenderTargetCache::GetTileMergeLayout(uint32_t& surface_out, uint32_t& color_out,
                                                 uint32_t& depth_out) const {
  const RegisterFile& regs = register_file();
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  surface_out = uint32_t(surface_info.surface_pitch) | (uint32_t(surface_info.msaa_samples) << 16);
  const auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
  color_out = uint32_t(color_info.color_base) |
              (uint32_t(GetColorKeyFormat(color_info.color_format)) << 16);
  const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  depth_out = uint32_t(depth_info.depth_base) | (uint32_t(depth_info.depth_format) << 16);
}

VulkanRenderTargetCache::TileMergeAction VulkanRenderTargetCache::GetTileMergeDrawAction(
    uint64_t frame, uint64_t bin_select, uint64_t bin_mask, uint32_t& scissor_bottom_out) {
  scissor_bottom_out = 0;
  if (GetPath() != Path::kHostRenderTargets || IsDrawResolutionScaled() ||
      !REXCVAR_GET(edram_merge_tiles)) {
    return TileMergeAction::kNone;
  }
  const RegisterFile& regs = register_file();
  const xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) {
    return TileMergeAction::kNone;
  }
  // Depth-only draws (a depth pass within a tile) don't use color 0, so its
  // register may hold anything.
  const bool depth_only = edram_mode == xenos::EdramMode::kDepthOnly;
  const auto scissor_tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  const auto scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  const auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  if (scissor_tl.window_offset_disable || scissor_tl.tl_x || window_offset.window_x_offset) {
    return TileMergeAction::kNone;
  }
  uint32_t surface_info, color_info, depth_info;
  GetTileMergeLayout(surface_info, color_info, depth_info);
  TileMerge& merge = tile_merge_;
  // Bit 31 is not a bin.
  constexpr uint64_t kBins = UINT64_C(0x7FFFFFFF);

  if (window_offset.window_y_offset < 0) {
    // The second tile: the rows from -window_y_offset down, drawn at the top
    // of EDRAM.
    const uint32_t first_tile_height = uint32_t(-window_offset.window_y_offset);
    if (scissor_tl.tl_y != first_tile_height || scissor_br.br_y <= first_tile_height) {
      return TileMergeAction::kNone;
    }
    const bool same = merge.learned && merge.surface_info == surface_info &&
                      (depth_only || merge.color_info == color_info) &&
                      merge.depth_info == depth_info &&
                      merge.first_tile_height == first_tile_height &&
                      merge.frame_height == scissor_br.br_y;
    if (!same && depth_only) {
      // Learn the layout from draws with color only.
      return TileMergeAction::kNone;
    }
    if (!same) {
      if (merge.learned && merge.surface_info == surface_info && merge.color_info == color_info &&
          merge.depth_info == depth_info && merge.first_tile_height == first_tile_height &&
          scissor_br.br_y < merge.frame_height) {
        // A draw with a scissor of its own ending above the bottom of the
        // frame - not a different layout.
        return TileMergeAction::kNone;
      }
      if (merge.merged_frame == frame) {
        // Not while a merged frame is being drawn: the second tile's resolve
        // still has to read the merged rows.
        return TileMergeAction::kNone;
      }
      // Use the layout from the next frame on.
      merge.learned = true;
      merge.surface_info = surface_info;
      merge.color_info = color_info;
      merge.depth_info = depth_info;
      merge.first_tile_height = first_tile_height;
      merge.frame_height = scissor_br.br_y;
      merge.merged_frame = 0;
      return TileMergeAction::kNone;
    }
    if (merge.merged_frame != frame) {
      return TileMergeAction::kNone;
    }
    return (bin_mask & merge.first_tile_bin_selects & kBins)
               ? TileMergeAction::kSkipDraw
               : TileMergeAction::kDrawWithoutWindowOffset;
  }

  if (!merge.learned || window_offset.window_y_offset || scissor_tl.tl_y ||
      scissor_br.br_y != merge.first_tile_height || merge.surface_info != surface_info ||
      (!depth_only && merge.color_info != color_info) || merge.depth_info != depth_info) {
    return TileMergeAction::kNone;
  }
  // The first tile, clipped at its bottom edge.
  if (merge.merged_frame != frame) {
    merge.merged_frame = frame;
    merge.first_tile_bin_selects = 0;
    merge.first_tile_resolved = false;
  }
  merge.first_tile_bin_selects |= bin_select;
  scissor_bottom_out = merge.frame_height;
  return TileMergeAction::kExtendScissor;
}

bool VulkanRenderTargetCache::TryForwardClearThrough4xView(
    const memory::Memory& memory, const Shader& vertex_shader, const Shader* pixel_shader,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask) {
  // The vertex and pixel shaders of the Direct3D 9 clear in Fable II: position
  // and color straight from the vertex buffer (three floats, then four).
  constexpr uint64_t kClearVertexShader = UINT64_C(0x1E6883FCCDE1F688);
  constexpr uint64_t kClearPixelShader = UINT64_C(0xA4A965C189287B99);
  if (GetPath() != Path::kHostRenderTargets || IsDrawResolutionScaled() ||
      !REXCVAR_GET(edram_forward_4x_clears) ||
      vertex_shader.ucode_data_hash() != kClearVertexShader ||
      (pixel_shader && pixel_shader->ucode_data_hash() != kClearPixelShader)) {
    return false;
  }
  // Why a clear stays a draw, in the frame dump (the line of the check).
  auto reject = [&](int line) {
    if (g_frame_dump.active) {
      FrameDumpEvent(fmt::format("  clear through a 4x view not forwarded (render_target_cache.cpp:{})",
                                 line));
    }
    return false;
  };
  const RegisterFile& regs = register_file();
  const auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const auto draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (rb_surface_info.msaa_samples != xenos::MsaaSamples::k4X ||
      draw_initiator.prim_type != xenos::PrimitiveType::kRectangleList ||
      draw_initiator.source_select != xenos::SourceSelect::kAutoIndex ||
      draw_initiator.num_indices != 3) {
    return reject(__LINE__);
  }
  // (In the depth-only EDRAM mode there is no pixel shader, and so no color
  // written.)
  const xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) {
    return reject(__LINE__);
  }
  // Positions must be window coordinates already, with nothing able to reject
  // a pixel or a sample.
  const auto vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  if (vte_cntl.vport_x_scale_ena || vte_cntl.vport_x_offset_ena || vte_cntl.vport_y_scale_ena ||
      vte_cntl.vport_y_offset_ena || vte_cntl.vport_z_scale_ena || vte_cntl.vport_z_offset_ena) {
    return reject(__LINE__);
  }
  const auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  if (window_offset.window_x_offset || window_offset.window_y_offset) {
    return reject(__LINE__);
  }
  const auto mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (mode_cntl.cull_front || mode_cntl.cull_back ||
      mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode) {
    return reject(__LINE__);
  }
  if ((regs[XE_GPU_REG_PA_SC_AA_MASK] & 0xF) != 0xF) {
    return reject(__LINE__);
  }
  // (Alpha test and alpha to coverage need a pixel shader's alpha; without
  // one - depth and stencil only - they don't apply.)
  const auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  if (pixel_shader && ((rb_colorcontrol.alpha_test_enable &&
                        rb_colorcontrol.alpha_func != xenos::CompareFunction::kAlways) ||
                       rb_colorcontrol.alpha_to_mask_enable)) {
    return reject(__LINE__);
  }

  // What the clear writes.
  const reg::RB_DEPTHCONTROL depth_control = normalized_depth_control;
  bool writes_depth = false;
  if (depth_control.z_enable || depth_control.stencil_enable) {
    // Only depth and stencil replaced together (the host clears both).
    if (!depth_control.z_enable || !depth_control.z_write_enable ||
        depth_control.zfunc != xenos::CompareFunction::kAlways || !depth_control.stencil_enable ||
        depth_control.stencilfunc != xenos::CompareFunction::kAlways ||
        depth_control.stencilzpass != xenos::StencilOp::kReplace ||
        regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask != 0xFF) {
      return reject(__LINE__);
    }
    writes_depth = true;
  }
  bool writes_color = false;
  if (pixel_shader && (normalized_color_mask & 0xF)) {
    if ((normalized_color_mask & ~uint32_t(0xF)) || (normalized_color_mask & 0xF) != 0xF) {
      return reject(__LINE__);
    }
    const auto blend_control =
        regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]);
    if (blend_control.color_srcblend != xenos::BlendFactor::kOne ||
        blend_control.color_destblend != xenos::BlendFactor::kZero ||
        blend_control.color_comb_fcn != xenos::BlendOp::kAdd ||
        blend_control.alpha_srcblend != xenos::BlendFactor::kOne ||
        blend_control.alpha_destblend != xenos::BlendFactor::kZero ||
        blend_control.alpha_comb_fcn != xenos::BlendOp::kAdd) {
      return reject(__LINE__);
    }
    writes_color = true;
  }
  if (!writes_depth && !writes_color) {
    return reject(__LINE__);
  }

  // The rectangle, depth and color, from the three vertices.
  const auto& bindings = vertex_shader.vertex_bindings();
  if (bindings.size() != 1 || bindings[0].stride_words != 7) {
    return reject(__LINE__);
  }
  const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(bindings[0].fetch_constant);
  const uint32_t first_vertex = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  if (fetch.endian != xenos::Endian::k8in32 ||
      uint64_t(first_vertex + 3) * 7 > uint64_t(fetch.size)) {
    return reject(__LINE__);
  }
  const uint8_t* vertices = memory.TranslatePhysical<const uint8_t*>(fetch.address << 2);
  if (!vertices) {
    return reject(__LINE__);
  }
  float values[3][7];
  for (uint32_t i = 0; i < 3; ++i) {
    for (uint32_t j = 0; j < 7; ++j) {
      uint32_t raw;
      std::memcpy(&raw, vertices + (size_t(first_vertex + i) * 7 + j) * 4, sizeof(raw));
      raw = rex::byte_swap(raw);
      std::memcpy(&values[i][j], &raw, sizeof(raw));
    }
  }
  for (uint32_t i = 1; i < 3; ++i) {
    // One depth and one color for the whole rectangle.
    if (std::memcmp(&values[i][2], &values[0][2], sizeof(float) * 5)) {
      return reject(__LINE__);
    }
  }
  float min_x = values[0][0], max_x = values[0][0], min_y = values[0][1], max_y = values[0][1];
  for (uint32_t i = 1; i < 3; ++i) {
    min_x = std::min(min_x, values[i][0]);
    max_x = std::max(max_x, values[i][0]);
    min_y = std::min(min_y, values[i][1]);
    max_y = std::max(max_y, values[i][1]);
  }
  for (uint32_t i = 0; i < 3; ++i) {
    // Axis-aligned: every vertex in a corner.
    if ((values[i][0] != min_x && values[i][0] != max_x) ||
        (values[i][1] != min_y && values[i][1] != max_y)) {
      return reject(__LINE__);
    }
  }
  // Pixels whose centers are inside (pixel centers at .0 need the half-pixel
  // offset), within the scissor. The edges must be on pixel boundaries, or
  // only some of the four samples of the edge pixels would be covered.
  const float pixel_offset =
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  const float edges[4] = {min_x + pixel_offset, min_y + pixel_offset, max_x + pixel_offset,
                          max_y + pixel_offset};
  int32_t rect[4];
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(edges[i] > -16384.0f && edges[i] < 16384.0f)) {
      return reject(__LINE__);
    }
    const float rounded = std::round(edges[i]);
    if (std::fabs(edges[i] - rounded) > 1.0f / 64.0f) {
      return reject(__LINE__);
    }
    rect[i] = int32_t(rounded);
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  rect[0] = std::max(rect[0], int32_t(scissor.offset[0]));
  rect[1] = std::max(rect[1], int32_t(scissor.offset[1]));
  rect[2] = std::min(rect[2], int32_t(scissor.offset[0] + scissor.extent[0]));
  rect[3] = std::min(rect[3], int32_t(scissor.offset[1] + scissor.extent[1]));
  if (rect[0] >= rect[2] || rect[1] >= rect[3]) {
    // Clears nothing.
    return reject(__LINE__);
  }
  // Written depth saturates as the guest stores it (unorm24 to [0, 1], 20e4
  // to [0, 2) in Float32To20e4); NaN as 0.
  float depth = values[0][2];
  if (!(depth >= 0.0f)) {
    depth = 0.0f;
  }
  if (regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8) {
    depth = std::min(depth, 1.0f);
  }

  // The render targets holding the memory under the views.
  const uint32_t view_pitch_tiles =
      ((rb_surface_info.surface_pitch << 1) + (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples;
  struct Target {
    RenderTargetKey key;
    Transfer::Rectangle rectangle;
    uint64_t clear_value;
  };
  Target targets[2];
  uint32_t target_count = 0;
  if (writes_depth) {
    const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    Target& target = targets[target_count];
    if (!FindClearThrough4xViewTarget(depth_info.depth_base, view_pitch_tiles, uint32_t(rect[0]),
                                      uint32_t(rect[1]), uint32_t(rect[2]), uint32_t(rect[3]),
                                      target.key, target.rectangle, true,
                                      int32_t(depth_info.depth_format))) {
      if (g_frame_dump.active) {
        FrameDumpEvent(fmt::format(
            "  (no target for the depth clear: view base {}t pitch {}t, rectangle {},{}-{},{}, "
            "format {})",
            uint32_t(depth_info.depth_base), view_pitch_tiles, rect[0], rect[1], rect[2], rect[3],
            uint32_t(depth_info.depth_format)));
      }
      return reject(__LINE__);
    }
    // The guest depth and stencil bits: as they are in a depth render target
    // of the same format, or seen as a 32-bit color (Direct3D 9 clears color
    // surfaces to some values this way).
    const uint32_t stencil = regs.Get<reg::RB_STENCILREFMASK>().stencilref;
    const uint32_t depth_bits =
        depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8
            ? xenos::Float32To20e4(depth, true)
            : uint32_t(depth * float(0xFFFFFF) + 0.5f);
    target.clear_value = ((depth_bits & 0xFFFFFF) << 8) | stencil;
    if (target.key.is_depth && target.key.GetDepthFormat() != depth_info.depth_format) {
      return reject(__LINE__);
    }
    ++target_count;
  }
  if (writes_color) {
    const auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
    Target& target = targets[target_count];
    if (color_info.color_exp_bias ||
        !FindClearThrough4xViewTarget(color_info.color_base, view_pitch_tiles, uint32_t(rect[0]),
                                      uint32_t(rect[1]), uint32_t(rect[2]), uint32_t(rect[3]),
                                      target.key, target.rectangle, false,
                                      int32_t(GetColorKeyFormat(color_info.color_format))) ||
        target.key.is_depth || target.key.GetColorFormat() != color_info.color_format) {
      return reject(__LINE__);
    }
    float color[4];
    for (uint32_t i = 0; i < 4; ++i) {
      color[i] = values[0][3 + i];
      if (!(color[i] >= 0.0f)) {
        color[i] = 0.0f;
      }
      color[i] = std::min(color[i], 1.0f);
    }
    switch (color_info.color_format) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        target.clear_value = uint32_t(color[0] * 255.0f + 0.5f) |
                             (uint32_t(color[1] * 255.0f + 0.5f) << 8) |
                             (uint32_t(color[2] * 255.0f + 0.5f) << 16) |
                             (uint32_t(color[3] * 255.0f + 0.5f) << 24);
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        target.clear_value = uint32_t(color[0] * 1023.0f + 0.5f) |
                             (uint32_t(color[1] * 1023.0f + 0.5f) << 10) |
                             (uint32_t(color[2] * 1023.0f + 0.5f) << 20) |
                             (uint32_t(color[3] * 3.0f + 0.5f) << 30);
        break;
      default:
        return reject(__LINE__);
    }
    ++target_count;
  }
  if (target_count == 2 && targets[0].key == targets[1].key) {
    return reject(__LINE__);
  }

  // Clear them.
  for (uint32_t i = 0; i < target_count; ++i) {
    const Target& target = targets[i];
    RenderTarget* render_target =
        TakeOwnershipForClear(target.key, target.rectangle, clear_transfers_[0]);
    if (!render_target) {
      continue;
    }
    PerformTransfersAndResolveClears(1, &render_target, clear_transfers_, &target.clear_value,
                                     &target.rectangle);
    if (g_frame_dump.active) {
      FrameDumpEvent(fmt::format(
          "  clear through a 4x view forwarded to [{}]: {}x{} at {},{}, value {:08X}",
          target.key.GetDebugName(), target.rectangle.width_pixels,
          target.rectangle.height_pixels, target.rectangle.x_pixels, target.rectangle.y_pixels,
          uint32_t(target.clear_value)));
    }
  }
  CountFrameEvent(g_frame_events.clears_forwarded);
  if (g_frame_dump.active) {
    ++g_frame_dump.frame_clears_forwarded;
  }
  return true;
}

namespace {

std::string BuildDirectConvertGlsl(bool depth, bool multisampled) {
  std::string source =
      "#version 450\n"
      "#extension GL_EXT_samplerless_texture_functions : require\n";
  source += depth ? "#define XE_DEPTH 1\n" : "#define XE_DEPTH 0\n";
  source += multisampled ? "#define XE_MS 1\n" : "#define XE_MS 0\n";
  source += R"GLSL(
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#if XE_MS
layout(set = 0, binding = 0) uniform texture2DMS xe_source;
#else
layout(set = 0, binding = 0) uniform texture2D xe_source;
#endif
#if XE_DEPTH
layout(set = 1, binding = 0, r32f) uniform writeonly image2D xe_dest;
#else
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D xe_dest;
#endif
layout(push_constant) uniform XeConstants {
  ivec2 source_offset;
  uvec2 size;
  int sample_0;
  int sample_1;  // < 0: one sample
  uint round_depth;
} xe_c;

vec4 XeLoad(ivec2 p, int s) {
#if XE_MS
  return texelFetch(xe_source, p, s);
#else
  return texelFetch(xe_source, p, 0);
#endif
}

// Host float -> 7e3 as the EDRAM dump stores it (clamped to [0, 31.875],
// rounded to the nearest even) -> float as the resolve reads it.
float XeTo7e3AndBack(float f) {
  f = isnan(f) ? 0.0 : clamp(f, 0.0, 31.875);
  uint u = floatBitsToUint(f);
  uint biased;
  if (u < 0x3E800000u) {
    biased = ((u & 0x7FFFFFu) | 0x800000u) >> min(125u - (u >> 23u), 24u);
  } else {
    biased = u + 0xC2000000u;
  }
  uint f10 = ((biased + 0x7FFFu + ((biased >> 16u) & 1u)) >> 16u) & 0x3FFu;
  if (f10 == 0u) {
    return 0.0;
  }
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7u;
  if (exponent == 0u) {
    uint shift = 7u - uint(findMSB(mantissa));
    exponent = 1u - shift;
    mantissa = (mantissa << shift) & 0x7Fu;
  }
  return uintBitsToFloat(((exponent + 124u) << 23u) | (mantissa << 16u));
}

vec4 XeHdrSample(vec4 v) {
  float a = isnan(v.a) ? 0.0 : clamp(v.a, 0.0, 1.0);
  return vec4(XeTo7e3AndBack(v.r), XeTo7e3AndBack(v.g), XeTo7e3AndBack(v.b),
              floor(a * 3.0 + 0.5) * (1.0 / 3.0));
}

// Host depth (guest depth * 0.5) -> 20e4 as the EDRAM dump stores it -> float
// as the k_24_8_FLOAT texture load decodes it.
float XeDepthTo20e4AndBack(float host_depth) {
  float f = host_depth * 2.0;
  if (!(f > 0.0)) {
    return 0.0;
  }
  uint u = floatBitsToUint(f);
  uint f24;
  if (u >= 0x3FFFFFF8u) {
    f24 = 0xFFFFFFu;
  } else {
    if (u < 0x38800000u) {
      u = (0x800000u | (u & 0x7FFFFFu)) >> min(113u - (u >> 23u), 24u);
    } else {
      u += 0xC8000000u;
    }
    if (xe_c.round_depth != 0u) {
      u += 3u + ((u >> 3u) & 1u);
    }
    f24 = (u >> 3u) & 0xFFFFFFu;
  }
  if (f24 == 0u) {
    return 0.0;
  }
  uint mantissa = f24 & 0xFFFFFu;
  uint exponent = f24 >> 20u;
  if (exponent == 0u) {
    uint shift = 20u - uint(findMSB(mantissa));
    exponent = 1u - shift;
    mantissa = (mantissa << shift) & 0xFFFFFu;
  }
  return uintBitsToFloat(((exponent + 112u) << 23u) | (mantissa << 3u));
}

void main() {
  uvec2 id = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(id, xe_c.size))) {
    return;
  }
  ivec2 p = xe_c.source_offset + ivec2(id);
#if XE_DEPTH
  imageStore(xe_dest, ivec2(id), vec4(XeDepthTo20e4AndBack(XeLoad(p, xe_c.sample_0).r)));
#else
  vec4 color = XeHdrSample(XeLoad(p, xe_c.sample_0));
  if (xe_c.sample_1 >= 0) {
    color = (color + XeHdrSample(XeLoad(p, xe_c.sample_1))) * 0.5;
  }
  imageStore(xe_dest, ivec2(id), color);
#endif
}
)GLSL";
  return source;
}

struct DirectConvertConstants {
  int32_t source_offset[2];
  uint32_t size[2];
  int32_t sample_0;
  int32_t sample_1;
  uint32_t round_depth;
};

}  // namespace

bool VulkanRenderTargetCache::EnsureDirectConvertLayouts() {
  if (direct_convert_pipeline_layouts_[0] != VK_NULL_HANDLE) {
    return true;
  }
  if (direct_convert_layouts_failed_) {
    return false;
  }
  direct_convert_layouts_failed_ = true;
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 1;
  set_layout_info.pBindings = &binding;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                      &direct_convert_storage_layout_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 2;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &direct_convert_descriptor_pool_) !=
      VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push_constants = {};
  push_constants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constants.size = sizeof(DirectConvertConstants);
  for (uint32_t mode = 0; mode < 2; ++mode) {
    VkDescriptorSetLayout set_layouts[2] = {
        mode == kDirectConvertDepth ? descriptor_set_layout_sampled_image_x2_
                                    : descriptor_set_layout_sampled_image_,
        direct_convert_storage_layout_};
    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = set_layouts;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_constants;
    if (dfn.vkCreatePipelineLayout(device, &layout_info, nullptr,
                                   &direct_convert_pipeline_layouts_[mode]) != VK_SUCCESS) {
      return false;
    }
  }
  direct_convert_layouts_failed_ = false;
  return true;
}

VkPipeline VulkanRenderTargetCache::GetDirectConvertPipeline(uint32_t mode, bool multisampled) {
  VkPipeline& pipeline = direct_convert_pipelines_[mode][multisampled];
  if (pipeline != VK_NULL_HANDLE || direct_convert_pipeline_failed_[mode][multisampled]) {
    return pipeline;
  }
  direct_convert_pipeline_failed_[mode][multisampled] = true;
  if (!EnsureDirectConvertLayouts()) {
    return VK_NULL_HANDLE;
  }
  std::vector<uint32_t> spirv;
  std::string error;
  if (!command_processor_.CompileGlslToSpirv(
          VK_SHADER_STAGE_COMPUTE_BIT,
          BuildDirectConvertGlsl(mode == kDirectConvertDepth, multisampled), spirv, error)) {
    REXGPU_ERROR("VulkanRenderTargetCache: Failed to compile the direct resolve shader: {}",
                 error);
    return VK_NULL_HANDLE;
  }
  pipeline = ui::vulkan::util::CreateComputePipeline(command_processor_.GetVulkanDevice(),
                                                     direct_convert_pipeline_layouts_[mode],
                                                     spirv.data(), spirv.size() * sizeof(uint32_t));
  if (pipeline != VK_NULL_HANDLE) {
    direct_convert_pipeline_failed_[mode][multisampled] = false;
  }
  return pipeline;
}

bool VulkanRenderTargetCache::EnsureDirectConvertImage(uint32_t mode) {
  DirectConvertImage& scratch = direct_convert_images_[mode];
  if (scratch.image != VK_NULL_HANDLE) {
    return true;
  }
  if (scratch.failed || !EnsureDirectConvertLayouts()) {
    return false;
  }
  scratch.failed = true;
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const VkFormat format =
      mode == kDirectConvertDepth ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT;
  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {kDirectConvertImageSize, kDirectConvertImageSize, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          scratch.image, scratch.memory)) {
    return false;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = scratch.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &scratch.view) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorPool = direct_convert_descriptor_pool_;
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &direct_convert_storage_layout_;
  if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &scratch.descriptor_set) !=
      VK_SUCCESS) {
    return false;
  }
  VkDescriptorImageInfo descriptor_image = {};
  descriptor_image.imageView = scratch.view;
  descriptor_image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = scratch.descriptor_set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  write.pImageInfo = &descriptor_image;
  dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  scratch.failed = false;
  scratch.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  return true;
}

VkImage VulkanRenderTargetCache::RunDirectConvert(uint32_t mode, VulkanRenderTarget& source,
                                                  int32_t source_x, int32_t source_y,
                                                  uint32_t width, uint32_t height,
                                                  int32_t host_sample_0, int32_t host_sample_1) {
  if (width > kDirectConvertImageSize || height > kDirectConvertImageSize ||
      !EnsureDirectConvertImage(mode)) {
    return VK_NULL_HANDLE;
  }
  const RenderTargetKey key = source.key();
  VkPipeline pipeline =
      GetDirectConvertPipeline(mode, key.msaa_samples != xenos::MsaaSamples::k1X);
  if (pipeline == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  DirectConvertImage& scratch = direct_convert_images_[mode];
  command_processor_.PushImageMemoryBarrier(
      source.image(),
      ui::vulkan::util::InitializeSubresourceRange(
          key.is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                       : VK_IMAGE_ASPECT_COLOR_BIT),
      source.current_stage_mask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      source.current_access_mask(), VK_ACCESS_SHADER_READ_BIT, source.current_layout(),
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  source.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  // The previous contents aren't needed (a copy from it may still be reading).
  const bool scratch_was_used = scratch.layout != VK_IMAGE_LAYOUT_UNDEFINED;
  command_processor_.PushImageMemoryBarrier(
      scratch.image, ui::vulkan::util::InitializeSubresourceRange(),
      scratch_was_used ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      scratch_was_used ? VkAccessFlags(VK_ACCESS_TRANSFER_READ_BIT) : VkAccessFlags(0),
      VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  command_processor_.BindExternalComputePipeline(pipeline);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  const VkPipelineLayout layout = direct_convert_pipeline_layouts_[mode];
  VkDescriptorSet descriptor_sets[2] = {source.GetDescriptorSetTransferSource(),
                                        scratch.descriptor_set};
  command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2,
                                         descriptor_sets, 0, nullptr);
  DirectConvertConstants constants;
  constants.source_offset[0] = source_x;
  constants.source_offset[1] = source_y;
  constants.size[0] = width;
  constants.size[1] = height;
  constants.sample_0 = host_sample_0;
  constants.sample_1 = host_sample_1;
  constants.round_depth = uint32_t(depth_float24_round());
  command_buffer.CmdVkPushConstants(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                                    &constants);
  command_processor_.SubmitBarriers(true);
  command_buffer.CmdVkDispatch((width + 7) / 8, (height + 7) / 8, 1);
  command_processor_.PushImageMemoryBarrier(
      scratch.image, ui::vulkan::util::InitializeSubresourceRange(),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  scratch.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  return scratch.image;
}

void VulkanRenderTargetCache::DestroyDirectConvertResources() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (auto& by_mode : direct_convert_pipelines_) {
    for (VkPipeline& pipeline : by_mode) {
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
    }
  }
  for (VkPipelineLayout& layout : direct_convert_pipeline_layouts_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, layout);
  }
  for (DirectConvertImage& scratch : direct_convert_images_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, scratch.view);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, scratch.image);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, scratch.memory);
    scratch.descriptor_set = VK_NULL_HANDLE;
    scratch.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         direct_convert_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         direct_convert_storage_layout_);
}

bool VulkanRenderTargetCache::Update(bool is_rasterization_done,
                                     reg::RB_DEPTHCONTROL normalized_depth_control,
                                     uint32_t normalized_color_mask, const Shader& vertex_shader) {
  if (!RenderTargetCache::Update(is_rasterization_done, normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    return false;
  }

  auto rb_surface_info = register_file().Get<reg::RB_SURFACE_INFO>();

  RenderPassKey render_pass_key;
  // Needed even with the fragment shader interlock render backend for passing
  // the sample count to the pipeline cache.
  render_pass_key.msaa_samples = rb_surface_info.msaa_samples;

  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      RenderTarget* const* depth_and_color_render_targets =
          last_update_accumulated_render_targets();
      if (depth_overwritten_by_next_update_ && depth_and_color_render_targets[0]) {
        // Slot 0 is depth, and the draw replaces it, so its old contents are not needed.
        const_cast<std::vector<Transfer>*>(last_update_transfers())[0].clear();
      }
      if (REXCVAR_GET(skip_color_overwrite_transfers)) {
        CountFrameEvent(
            g_frame_events.rt_transfers_skipped,
            SkipTransfersOverwrittenByDraw(
                vertex_shader, depth_and_color_render_targets,
                const_cast<std::vector<Transfer>*>(last_update_transfers())));
      }
      if (g_frame_dump.active) {
        // Render target binding changes.
        static uint32_t dumped_keys[1 + xenos::kMaxColorRenderTargets];
        static uint32_t dumped_capture = UINT32_MAX;
        uint32_t keys[1 + xenos::kMaxColorRenderTargets];
        for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
          keys[i] = depth_and_color_render_targets[i]
                        ? depth_and_color_render_targets[i]->key().key
                        : 0;
        }
        if (dumped_capture != g_frame_dump.captures ||
            std::memcmp(dumped_keys, keys, sizeof(keys))) {
          dumped_capture = g_frame_dump.captures;
          std::memcpy(dumped_keys, keys, sizeof(keys));
          std::string line = fmt::format("bind (surface pitch {}):", uint32_t(rb_surface_info.surface_pitch));
          for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
            if (depth_and_color_render_targets[i]) {
              line += fmt::format(" {}=[{}]", i ? fmt::format("color{}", i - 1) : "depth",
                                  depth_and_color_render_targets[i]->key().GetDebugName());
            }
          }
          FrameDumpEvent(line);
        }
      }
      {
        // Transfers between render targets with different EDRAM layouts carry
        // no usable picture.
        const bool skip_layout_mismatch = REXCVAR_GET(edram_skip_layout_mismatch_transfers);
        auto* transfers = const_cast<std::vector<Transfer>*>(last_update_transfers());
        for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
          RenderTarget* dest = depth_and_color_render_targets[i];
          if (!dest || transfers[i].empty()) {
            continue;
          }
          const RenderTargetKey dest_key = dest->key();
          size_t kept = 0;
          for (size_t t = 0; t < transfers[i].size(); ++t) {
            const Transfer& transfer = transfers[i][t];
            const RenderTargetKey source_key =
                transfer.source ? transfer.source->key() : RenderTargetKey();
            const bool layout_mismatch =
                transfer.source &&
                (source_key.pitch_tiles_at_32bpp != dest_key.pitch_tiles_at_32bpp ||
                 source_key.msaa_samples != dest_key.msaa_samples ||
                 source_key.Is64bpp() != dest_key.Is64bpp());
            // Not when either is 4x multisampled: Direct3D 9 on the Xbox 360
            // clears a surface through a 4x view of the same memory (four
            // samples written per pixel), and a cleared area is the same
            // picture in any layout.
            const bool clear_alias =
                transfer.source && (source_key.msaa_samples == xenos::MsaaSamples::k4X ||
                                    dest_key.msaa_samples == xenos::MsaaSamples::k4X);
            const bool skip = skip_layout_mismatch && layout_mismatch && !clear_alias;
            if (g_frame_dump.active) {
              FrameDumpEvent(fmt::format(
                  "  transfer{} to [{}] from [{}] tiles {}-{} ({}){}", skip ? " SKIPPED" : "",
                  dest_key.GetDebugName(),
                  transfer.source ? source_key.GetDebugName() : std::string("nothing"),
                  transfer.start_tiles, transfer.end_tiles,
                  transfer.end_tiles - transfer.start_tiles,
                  clear_alias       ? ", 4x view (clear)"
                  : layout_mismatch ? ", layouts differ"
                  : source_key.base_tiles != dest_key.base_tiles ? ", base differs"
                                                                 : ", same layout"));
              if (skip) {
                ++g_frame_dump.frame_transfers_skipped;
              } else {
                ++g_frame_dump.frame_transfers;
              }
            }
            if (skip) {
              CountFrameEvent(g_frame_events.rt_transfers_skipped);
            } else {
              if (kept != t) {
                transfers[i][kept] = transfer;
              }
              ++kept;
            }
          }
          transfers[i].erase(transfers[i].begin() + kept, transfers[i].end());
        }
      }
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        for (const Transfer& transfer : last_update_transfers()[i]) {
          CountFrameEvent(g_frame_events.rt_transfers);
          CountFrameEvent(g_frame_events.rt_transfer_tiles,
                          transfer.end_tiles - transfer.start_tiles);
          if (i == 0) {
            CountFrameEvent(g_frame_events.rt_transfers_to_depth);
          }
        }
      }

      PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                       depth_and_color_render_targets, last_update_transfers());

      if (depth_and_color_render_targets[0]) {
        render_pass_key.depth_and_color_used |= 1 << 0;
        render_pass_key.depth_format = depth_and_color_render_targets[0]->key().GetDepthFormat();
      }
      if (depth_and_color_render_targets[1]) {
        render_pass_key.depth_and_color_used |= 1 << 1;
        render_pass_key.color_0_view_format =
            depth_and_color_render_targets[1]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[2]) {
        render_pass_key.depth_and_color_used |= 1 << 2;
        render_pass_key.color_1_view_format =
            depth_and_color_render_targets[2]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[3]) {
        render_pass_key.depth_and_color_used |= 1 << 3;
        render_pass_key.color_2_view_format =
            depth_and_color_render_targets[3]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[4]) {
        render_pass_key.depth_and_color_used |= 1 << 4;
        render_pass_key.color_3_view_format =
            depth_and_color_render_targets[4]->key().GetColorFormat();
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
        // Framebuffer for a different render pass needed now.
        framebuffer = nullptr;
      }

      uint32_t pitch_tiles_at_32bpp =
          ((rb_surface_info.surface_pitch
            << uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X)) +
           (xenos::kEdramTileWidthSamples - 1)) /
          xenos::kEdramTileWidthSamples;
      if (framebuffer) {
        if (last_update_framebuffer_pitch_tiles_at_32bpp_ != pitch_tiles_at_32bpp ||
            std::memcmp(last_update_framebuffer_attachments_, depth_and_color_render_targets,
                        sizeof(last_update_framebuffer_attachments_))) {
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

      // Successful update - write the new configuration.
      last_update_render_pass_key_ = render_pass_key;
      last_update_render_pass_ = render_pass;
      last_update_framebuffer_pitch_tiles_at_32bpp_ = pitch_tiles_at_32bpp;
      std::memcpy(last_update_framebuffer_attachments_, depth_and_color_render_targets,
                  sizeof(last_update_framebuffer_attachments_));
      last_update_framebuffer_ = framebuffer;

      // Transition the used render targets.
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
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
            vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                i ? VK_IMAGE_ASPECT_COLOR_BIT
                  : (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)),
            vulkan_rt.current_stage_mask(), rt_dst_stage_mask, vulkan_rt.current_access_mask(),
            rt_dst_access_mask, vulkan_rt.current_layout(), rt_new_layout);
        vulkan_rt.SetUsage(rt_dst_stage_mask, rt_dst_access_mask, rt_new_layout);
      }
    } break;

    case Path::kPixelShaderInterlock: {
      // For FSI, only the barrier is needed - already scheduled if required.
      // But the buffer will be used for FSI drawing now.
      UseEdramBuffer(EdramBufferUsage::kFragmentReadWrite);
      // Commit preceding unordered (but not FSI) writes like clears as they
      // aren't synchronized with FSI accesses.
      CommitEdramBufferShaderWrites(EdramBufferModificationStatus::kViaUnordered);
      // TODO(Triang3l): Check if this draw call modifies color or depth /
      // stencil, at least coarsely, to prevent useless barriers.
      MarkEdramBufferModified(EdramBufferModificationStatus::kViaFragmentShaderInterlock);
      last_update_render_pass_key_ = render_pass_key;
      last_update_render_pass_ = fsi_render_pass_;
      last_update_framebuffer_ = &fsi_framebuffer_;
    } break;

    default:
      assert_unhandled_case(GetPath());
      return false;
  }

  return true;
}

void VulkanRenderTargetCache::GetLastUpdateRenderingAttachments(
    VkRenderingAttachmentInfo* color_attachments, uint32_t* color_attachment_count_out,
    VkRenderingAttachmentInfo* depth_attachment,
    VkRenderingAttachmentInfo* stencil_attachment) const {
  RenderPassKey key = last_update_render_pass_key_;
  RenderTarget* const* rts = last_update_accumulated_render_targets();

  std::memset(depth_attachment, 0, sizeof(VkRenderingAttachmentInfo));
  std::memset(stencil_attachment, 0, sizeof(VkRenderingAttachmentInfo));

  if ((key.depth_and_color_used & 0b1) && rts[0]) {
    const auto* vulkan_rt = static_cast<const VulkanRenderTarget*>(rts[0]);
    depth_attachment->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment->pNext = nullptr;
    depth_attachment->imageView = vulkan_rt->view_depth_stencil();
    depth_attachment->imageLayout = VulkanRenderTarget::kDepthDrawLayout;
    depth_attachment->resolveMode = VK_RESOLVE_MODE_NONE;
    depth_attachment->resolveImageView = VK_NULL_HANDLE;
    depth_attachment->resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment->clearValue = {};
    *stencil_attachment = *depth_attachment;
  }

  uint32_t color_attachment_count = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    VkRenderingAttachmentInfo& color_attachment = color_attachments[i];
    std::memset(&color_attachment, 0, sizeof(VkRenderingAttachmentInfo));
    if ((key.depth_and_color_used & (1 << (1 + i))) && rts[1 + i]) {
      const auto* vulkan_rt = static_cast<const VulkanRenderTarget*>(rts[1 + i]);
      color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_attachment.pNext = nullptr;
      color_attachment.imageView = vulkan_rt->view_depth_color();
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
  assert_true(GetPath() == Path::kHostRenderTargets);

  auto it = render_passes_.find(key);
  if (it != render_passes_.end()) {
    return it->second;
  }

  VkSampleCountFlagBits samples;
  switch (key.msaa_samples) {
    case xenos::MsaaSamples::k1X:
      samples = VK_SAMPLE_COUNT_1_BIT;
      break;
    case xenos::MsaaSamples::k2X:
      samples = IsMsaa2xSupported(key.depth_and_color_used != 0) ? VK_SAMPLE_COUNT_2_BIT
                                                                 : VK_SAMPLE_COUNT_4_BIT;
      break;
    case xenos::MsaaSamples::k4X:
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
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    VkAttachmentReference& color_attachment = color_attachments[i];
    color_attachment.layout = VulkanRenderTarget::kColorDrawLayout;
    uint32_t attachment_bit = uint32_t(1) << (1 + i);
    if (!(key.depth_and_color_used & attachment_bit)) {
      color_attachment.attachment = VK_ATTACHMENT_UNUSED;
      continue;
    }
    uint32_t attachment_index = rex::bit_count(key.depth_and_color_used & (attachment_bit - 1));
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
  subpass.colorAttachmentCount = 32 - rex::lzcnt(uint32_t(key.depth_and_color_used >> 1));
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
  render_pass_create_info.attachmentCount = rex::bit_count(key.depth_and_color_used);
  render_pass_create_info.pAttachments = attachments;
  render_pass_create_info.subpassCount = 1;
  render_pass_create_info.pSubpasses = &subpass;
  render_pass_create_info.dependencyCount =
      key.depth_and_color_used ? uint32_t(rex::countof(subpass_dependencies)) : 0;
  render_pass_create_info.pDependencies = subpass_dependencies;

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkRenderPass render_pass;
  if (dfn.vkCreateRenderPass(device, &render_pass_create_info, nullptr, &render_pass) !=
      VK_SUCCESS) {
    REXGPU_ERROR("VulkanRenderTargetCache: Failed to create a render pass");
    render_passes_.emplace(key, VK_NULL_HANDLE);
    return VK_NULL_HANDLE;
  }
  render_passes_.emplace(key, render_pass);
  return render_pass;
}

VkFormat VulkanRenderTargetCache::GetDepthVulkanFormat(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24S8 && depth_unorm24_vulkan_format_supported()) {
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
      assert_unhandled_case(format);
      return VK_FORMAT_UNDEFINED;
  }
}

VkFormat VulkanRenderTargetCache::GetColorOwnershipTransferVulkanFormat(
    xenos::ColorRenderTargetFormat format, xenos::MsaaSamples msaa_samples,
    bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  VkSampleCountFlagBits transfer_sample_count =
      VkSampleCountFlagBits(uint32_t(1) << uint32_t(msaa_samples));
  if (transfer_sample_count == VK_SAMPLE_COUNT_2_BIT && !msaa_2x_attachments_supported_) {
    // 2x guest color render targets are allocated as 4x host render targets in
    // this mode.
    transfer_sample_count = VK_SAMPLE_COUNT_4_BIT;
  }
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  bool integer_transfer_sample_count_supported =
      (device_properties.framebufferColorSampleCounts & transfer_sample_count) != 0 &&
      (device_properties.sampledImageIntegerSampleCounts & transfer_sample_count) != 0;
  // Floating-point numbers have NaNs that need to be propagated without
  // modifications to the bit representation, and SNORM has two representations
  // of -1.
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
  const ui::vulkan::VulkanDevice* const vulkan_device =
      render_target_cache_.command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      key().is_depth ? *render_target_cache_.descriptor_set_pool_sampled_image_x2_
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

uint32_t VulkanRenderTargetCache::GetMaxRenderTargetWidth() const {
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  return std::min(device_properties.maxFramebufferWidth, device_properties.maxImageDimension2D);
}

uint32_t VulkanRenderTargetCache::GetMaxRenderTargetHeight() const {
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  return std::min(device_properties.maxFramebufferHeight, device_properties.maxImageDimension2D);
}

RenderTargetCache::RenderTarget* VulkanRenderTargetCache::CreateRenderTarget(RenderTargetKey key) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Create the image.

  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.extent.width = key.GetWidth() * draw_resolution_scale_x();
  image_create_info.extent.height =
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples) * draw_resolution_scale_y();
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  if (key.msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_attachments_supported_) {
    image_create_info.samples = VK_SAMPLE_COUNT_4_BIT;
  } else {
    image_create_info.samples = VkSampleCountFlagBits(uint32_t(1) << uint32_t(key.msaa_samples));
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
    // For direct resolves (resolve_direct_to_texture).
    image_create_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    transfer_format = GetColorOwnershipTransferVulkanFormat(color_format, key.msaa_samples);
    is_srgb_view_needed = false;
    if (image_create_info.format != transfer_format || is_srgb_view_needed) {
      image_create_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }
    image_create_info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  if (image_create_info.format == VK_FORMAT_UNDEFINED) {
    REXGPU_ERROR("VulkanRenderTargetCache: Unknown {} render target format {}",
                 key.is_depth ? "depth" : "color", static_cast<uint32_t>(key.resource_format));
    return nullptr;
  }
  VkImage image;
  VkDeviceMemory memory;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_create_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, image,
          memory)) {
    REXGPU_ERROR(
        "VulkanRenderTarget: Failed to create a {}x{} {}xMSAA {} render target "
        "image",
        image_create_info.extent.width, image_create_info.extent.height,
        uint32_t(1) << uint32_t(key.msaa_samples), key.GetFormatName());
    return nullptr;
  }

  // Create the image views.

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
  view_create_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange(
      key.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageView view_depth_color;
  if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_depth_color) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTarget: Failed to create a {} view for a {}x{} {}xMSAA {} "
        "render target",
        key.is_depth ? "depth" : "color", image_create_info.extent.width,
        image_create_info.extent.height, uint32_t(1) << uint32_t(key.msaa_samples),
        key.GetFormatName());
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
      REXGPU_ERROR(
          "VulkanRenderTarget: Failed to create a depth / stencil view for a "
          "{}x{} {}xMSAA {} render target",
          image_create_info.extent.width, image_create_info.extent.height,
          uint32_t(1) << uint32_t(key.msaa_samples),
          xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat()));
      dfn.vkDestroyImageView(device, view_depth_color, nullptr);
      dfn.vkDestroyImage(device, image, nullptr);
      dfn.vkFreeMemory(device, memory, nullptr);
      return nullptr;
    }
    view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view_stencil) != VK_SUCCESS) {
      REXGPU_ERROR(
          "VulkanRenderTarget: Failed to create a stencil view for a {}x{} "
          "{}xMSAA render target",
          image_create_info.extent.width, image_create_info.extent.height,
          uint32_t(1) << uint32_t(key.msaa_samples),
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
        REXGPU_ERROR(
            "VulkanRenderTarget: Failed to create an sRGB view for a {}x{} "
            "{}xMSAA render target",
            image_create_info.extent.width, image_create_info.extent.height,
            uint32_t(1) << uint32_t(key.msaa_samples),
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
        REXGPU_ERROR(
            "VulkanRenderTarget: Failed to create a transfer view for a {}x{} "
            "{}xMSAA {} render target",
            image_create_info.extent.width, image_create_info.extent.height,
            uint32_t(1) << uint32_t(key.msaa_samples), key.GetFormatName());
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

  ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      key.is_depth ? *descriptor_set_pool_sampled_image_x2_ : *descriptor_set_pool_sampled_image_;
  size_t descriptor_set_index_transfer_source = descriptor_set_pool.Allocate();
  if (descriptor_set_index_transfer_source == SIZE_MAX) {
    REXGPU_ERROR(
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
      descriptor_set_pool.Get(descriptor_set_index_transfer_source);
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

  return new VulkanRenderTarget(key, *this, image, memory, view_depth_color, view_depth_stencil,
                                view_stencil, view_srgb, view_color_transfer_separate,
                                descriptor_set_index_transfer_source);
}

bool VulkanRenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return !depth_unorm24_vulkan_format_supported();
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return !depth_float24_convert_in_pixel_shader_;
  }
  return false;
}

bool VulkanRenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

void VulkanRenderTargetCache::RequestPixelShaderInterlockBarrier() {
  // Keep parity with D3D12 ROV interlock barrier requests by committing any
  // pending EDRAM shader writes, not only FSI writes.
  CommitEdramBufferShaderWrites();
}

void VulkanRenderTargetCache::GetEdramBufferUsageMasks(EdramBufferUsage usage,
                                                       VkPipelineStageFlags& stage_mask_out,
                                                       VkAccessFlags& access_mask_out) {
  switch (usage) {
    case EdramBufferUsage::kFragmentRead:
      stage_mask_out = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT;
      break;
    case EdramBufferUsage::kFragmentReadWrite:
      stage_mask_out = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      break;
    case EdramBufferUsage::kComputeRead:
      stage_mask_out = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT;
      break;
    case EdramBufferUsage::kComputeWrite:
      stage_mask_out = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_WRITE_BIT;
      break;
    case EdramBufferUsage::kTransferRead:
      stage_mask_out = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask_out = VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case EdramBufferUsage::kTransferWrite:
      stage_mask_out = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask_out = VK_ACCESS_TRANSFER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

void VulkanRenderTargetCache::UseEdramBuffer(EdramBufferUsage new_usage) {
  if (edram_buffer_usage_ == new_usage) {
    return;
  }
  VkPipelineStageFlags src_stage_mask, dst_stage_mask;
  VkAccessFlags src_access_mask, dst_access_mask;
  GetEdramBufferUsageMasks(edram_buffer_usage_, src_stage_mask, src_access_mask);
  GetEdramBufferUsageMasks(new_usage, dst_stage_mask, dst_access_mask);
  if (command_processor_.PushBufferMemoryBarrier(edram_buffer_, 0, VK_WHOLE_SIZE, src_stage_mask,
                                                 dst_stage_mask, src_access_mask,
                                                 dst_access_mask)) {
    // Resetting edram_buffer_modification_status_ only if the barrier has been
    // truly inserted.
    edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
  }
  edram_buffer_usage_ = new_usage;
}

void VulkanRenderTargetCache::MarkEdramBufferModified(
    EdramBufferModificationStatus modification_status) {
  assert_true(modification_status != EdramBufferModificationStatus::kUnmodified);
  switch (edram_buffer_usage_) {
    case EdramBufferUsage::kFragmentReadWrite:
      // max because being modified via unordered access requires stricter
      // synchronization than via fragment shader interlocks.
      edram_buffer_modification_status_ =
          std::max(edram_buffer_modification_status_, modification_status);
      break;
    case EdramBufferUsage::kComputeWrite:
      assert_true(modification_status == EdramBufferModificationStatus::kViaUnordered);
      modification_status = EdramBufferModificationStatus::kViaUnordered;
      edram_buffer_modification_status_ =
          std::max(edram_buffer_modification_status_, modification_status);
      break;
    default:
      assert_always(
          "While changing the usage of the EDRAM buffer before marking it as "
          "modified is handled safely (but will cause spurious marking as "
          "modified after the changes have been implicitly committed by the "
          "usage switch), normally that shouldn't be done and is an "
          "indication of architectural mistakes. Alternatively, this may "
          "indicate that the usage switch has been forgotten before writing, "
          "which is a clearly invalid situation.");
  }
}

void VulkanRenderTargetCache::CommitEdramBufferShaderWrites(
    EdramBufferModificationStatus commit_status) {
  assert_true(commit_status != EdramBufferModificationStatus::kUnmodified);
  if (edram_buffer_modification_status_ < commit_status) {
    return;
  }
  VkPipelineStageFlags stage_mask;
  VkAccessFlags access_mask;
  GetEdramBufferUsageMasks(edram_buffer_usage_, stage_mask, access_mask);
  if (!(access_mask & VK_ACCESS_SHADER_WRITE_BIT)) {
    // Keep behavior robust similarly to D3D12 when an unexpected state is
    // encountered: avoid emitting an invalid barrier but still treat writes as
    // committed for ownership tracking.
    assert_always("EDRAM writes committed from a non-shader-write usage");
    edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
    PixelShaderInterlockFullEdramBarrierPlaced();
    return;
  }
  command_processor_.PushBufferMemoryBarrier(
      edram_buffer_, 0, VK_WHOLE_SIZE, stage_mask, stage_mask, access_mask, access_mask,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
  PixelShaderInterlockFullEdramBarrierPlaced();
}

const VulkanRenderTargetCache::Framebuffer*
VulkanRenderTargetCache::GetHostRenderTargetsFramebuffer(
    RenderPassKey render_pass_key, uint32_t pitch_tiles_at_32bpp,
    const RenderTarget* const* depth_and_color_render_targets) {
  FramebufferKey key;
  key.render_pass_key = render_pass_key;
  key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
  if (render_pass_key.depth_and_color_used & (1 << 0)) {
    key.depth_base_tiles = depth_and_color_render_targets[0]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 1)) {
    key.color_0_base_tiles = depth_and_color_render_targets[1]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 2)) {
    key.color_1_base_tiles = depth_and_color_render_targets[2]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 3)) {
    key.color_2_base_tiles = depth_and_color_render_targets[3]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 4)) {
    key.color_3_base_tiles = depth_and_color_render_targets[4]->key().base_tiles;
  }
  auto it = framebuffers_.find(key);
  if (it != framebuffers_.end()) {
    return &it->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();

  VkRenderPass render_pass = GetHostRenderTargetsRenderPass(render_pass_key);
  if (render_pass == VK_NULL_HANDLE) {
    return nullptr;
  }

  VkImageView attachments[1 + xenos::kMaxColorRenderTargets];
  uint32_t attachment_count = 0;
  uint32_t depth_and_color_rts_remaining = render_pass_key.depth_and_color_used;
  uint32_t rt_index;
  while (rex::bit_scan_forward(depth_and_color_rts_remaining, &rt_index)) {
    depth_and_color_rts_remaining &= ~(uint32_t(1) << rt_index);
    const auto& vulkan_rt =
        *static_cast<const VulkanRenderTarget*>(depth_and_color_render_targets[rt_index]);
    VkImageView attachment;
    if (rt_index) {
      attachment = render_pass_key.color_rts_use_transfer_formats ? vulkan_rt.view_color_transfer()
                                                                  : vulkan_rt.view_depth_color();
    } else {
      attachment = vulkan_rt.view_depth_stencil();
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
    assert_zero(render_pass_key.depth_and_color_used);
    // Still needed for occlusion queries.
    host_extent.width = xenos::kTexture2DCubeMaxWidthHeight;
    host_extent.height = xenos::kTexture2DCubeMaxWidthHeight;
  }
  // Limiting to the device limit for the case of no attachments, for which
  // there's no limit imposed by the sizes of the attachments that have been
  // created successfully.
  host_extent.width = std::min(host_extent.width * draw_resolution_scale_x(),
                               device_properties.maxFramebufferWidth);
  host_extent.height = std::min(host_extent.height * draw_resolution_scale_y(),
                                device_properties.maxFramebufferHeight);
  framebuffer_create_info.width = host_extent.width;
  framebuffer_create_info.height = host_extent.height;
  framebuffer_create_info.layers = 1;
  VkFramebuffer framebuffer;
  if (dfn.vkCreateFramebuffer(device, &framebuffer_create_info, nullptr, &framebuffer) !=
      VK_SUCCESS) {
    return nullptr;
  }
  // Creates at a persistent location - safe to use pointers.
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

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();

  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;

  SpirvBuilder builder(spv::Spv_1_0, (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
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

  const TransferModeInfo& mode = kTransferModes[size_t(key.mode)];
  const TransferPipelineLayoutInfo& pipeline_layout_info =
      kTransferPipelineLayoutInfos[size_t(mode.pipeline_layout)];

  // If not dest_is_color, it's depth, or stencil bit - 40-sample columns are
  // swapped as opposed to color source.
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
  // If not source_is_color, it's depth / stencil - 40-sample columns are
  // swapped as opposed to color destination.
  bool source_is_color =
      (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetColorTextureBit) != 0;
  bool source_is_64bpp;
  uint32_t source_color_format_component_count;
  uint32_t source_color_texture_component_mask;
  bool source_color_is_uint;
  spv::Id source_color_component_type;
  if (source_is_color) {
    assert_zero(pipeline_layout_info.used_descriptor_sets &
                kTransferUsedDescriptorSetDepthStencilTexturesBit);
    source_is_64bpp = xenos::IsColorRenderTargetFormat64bpp(source_color_format);
    source_color_format_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(source_color_format);
    if (mode.output == TransferOutput::kStencilBit) {
      if (source_is_64bpp && !dest_is_64bpp) {
        // Need one component, but choosing from the two 32bpp halves of the
        // 64bpp sample.
        source_color_texture_component_mask =
            0b1 | (0b1 << (source_color_format_component_count >> 1));
      } else {
        // Red is at least 8 bits per component in all formats.
        source_color_texture_component_mask = 0b1;
      }
    } else {
      source_color_texture_component_mask =
          (uint32_t(1) << source_color_format_component_count) - 1;
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

  // Outputs.
  bool shader_uses_stencil_reference_output =
      mode.output == TransferOutput::kDepth &&
      vulkan_device->extensions().ext_EXT_shader_stencil_export;
  bool dest_color_is_uint = false;
  uint32_t dest_color_component_count = 0;
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
              ? builder.makeVectorType(type_fragment_data_component, dest_color_component_count)
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

  // Bindings.
  // Generating SPIR-V 1.0, no need to add bindings to the entry point's
  // interface until SPIR-V 1.4.
  // Color source.
  bool source_is_multisampled = key.source_msaa_samples != xenos::MsaaSamples::k1X;
  spv::Id source_color_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetColorTextureBit) {
    source_color_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(source_color_component_type, spv::Dim2D, false, false,
                              source_is_multisampled, 1, spv::ImageFormatUnknown),
        "xe_transfer_color");
    builder.addDecoration(source_color_texture, spv::DecorationDescriptorSet,
                          rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                                         (kTransferUsedDescriptorSetColorTextureBit - 1)));
    builder.addDecoration(source_color_texture, spv::DecorationBinding, 0);
  }
  // Depth / stencil source.
  spv::Id source_depth_texture = spv::NoResult;
  spv::Id source_stencil_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetDepthStencilTexturesBit) {
    uint32_t source_depth_stencil_descriptor_set =
        rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                       (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1));
    // Using `depth == false` in makeImageType because comparisons are not
    // required, and other values of `depth` are causing issues in drivers.
    // https://github.com/microsoft/DirectXShaderCompiler/issues/1107
    if (mode.output != TransferOutput::kStencilBit) {
      source_depth_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_float, spv::Dim2D, false, false, source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_depth");
      builder.addDecoration(source_depth_texture, spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_depth_texture, spv::DecorationBinding, 0);
    }
    if (mode.output != TransferOutput::kDepth || shader_uses_stencil_reference_output) {
      source_stencil_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_uint, spv::Dim2D, false, false, source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_stencil");
      builder.addDecoration(source_stencil_texture, spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
    }
  }
  // Host depth source buffer.
  spv::Id host_depth_source_buffer = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetHostDepthBufferBit) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeRuntimeArray(type_uint));
    // Storage buffers have std430 packing, no padding to 4-component vectors.
    builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride, sizeof(uint32_t));
    spv::Id type_host_depth_source_buffer =
        builder.makeStructType(id_vector_temp, "XeTransferHostDepthBuffer");
    builder.addMemberName(type_host_depth_source_buffer, 0, "host_depth");
    builder.addMemberDecoration(type_host_depth_source_buffer, 0, spv::DecorationNonWritable);
    builder.addMemberDecoration(type_host_depth_source_buffer, 0, spv::DecorationOffset, 0);
    // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // BufferBlock.
    builder.addDecoration(type_host_depth_source_buffer, spv::DecorationBufferBlock);
    // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // Uniform.
    host_depth_source_buffer =
        builder.createVariable(spv::NoPrecision, spv::StorageClassUniform,
                               type_host_depth_source_buffer, "xe_transfer_host_depth_buffer");
    builder.addDecoration(host_depth_source_buffer, spv::DecorationDescriptorSet,
                          rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                                         (kTransferUsedDescriptorSetHostDepthBufferBit - 1)));
    builder.addDecoration(host_depth_source_buffer, spv::DecorationBinding, 0);
  }
  // Host depth source texture (the depth / stencil descriptor set is reused,
  // but stencil is not needed).
  spv::Id host_depth_source_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
    host_depth_source_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_float, spv::Dim2D, false, false,
                              key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X, 1,
                              spv::ImageFormatUnknown),
        "xe_transfer_host_depth");
    builder.addDecoration(
        host_depth_source_texture, spv::DecorationDescriptorSet,
        rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                       (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)));
    builder.addDecoration(host_depth_source_texture, spv::DecorationBinding, 0);
  }
  // Push constants.
  id_vector_temp.clear();
  uint32_t push_constants_member_host_depth_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordHostDepthAddressBit) {
    push_constants_member_host_depth_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords & kTransferUsedPushConstantDwordAddressBit) {
    push_constants_member_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_stencil_mask = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordStencilMaskBit) {
    push_constants_member_stencil_mask = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  spv::Id push_constants = spv::NoResult;
  if (!id_vector_temp.empty()) {
    spv::Id type_push_constants = builder.makeStructType(id_vector_temp, "XeTransferPushConstants");
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordHostDepthAddressBit) {
      assert_true(push_constants_member_host_depth_address != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_host_depth_address,
                            "host_depth_address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_host_depth_address, spv::DecorationOffset,
          sizeof(uint32_t) *
              rex::bit_count(pipeline_layout_info.used_push_constant_dwords &
                             (kTransferUsedPushConstantDwordHostDepthAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords & kTransferUsedPushConstantDwordAddressBit) {
      assert_true(push_constants_member_address != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_address, "address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_address, spv::DecorationOffset,
          sizeof(uint32_t) * rex::bit_count(pipeline_layout_info.used_push_constant_dwords &
                                            (kTransferUsedPushConstantDwordAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordStencilMaskBit) {
      assert_true(push_constants_member_stencil_mask != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_stencil_mask,
                            "stencil_mask");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_stencil_mask, spv::DecorationOffset,
          sizeof(uint32_t) * rex::bit_count(pipeline_layout_info.used_push_constant_dwords &
                                            (kTransferUsedPushConstantDwordStencilMaskBit - 1)));
    }
    builder.addDecoration(type_push_constants, spv::DecorationBlock);
    push_constants = builder.createVariable(spv::NoPrecision, spv::StorageClassPushConstant,
                                            type_push_constants, "xe_transfer_push_constants");
  }

  // Coordinate inputs.
  spv::Id input_fragment_coord =
      builder.createVariable(spv::NoPrecision, spv::StorageClassInput, type_float4, "gl_FragCoord");
  builder.addDecoration(input_fragment_coord, spv::DecorationBuiltIn, spv::BuiltInFragCoord);
  main_interface.push_back(input_fragment_coord);
  spv::Id input_sample_id = spv::NoResult;
  spv::Id spec_const_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
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

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function = builder.makeFunctionEntry(
      spv::NoPrecision, type_void, "main", main_param_types, main_precisions, &main_entry);

  // Working with unsigned numbers for simplicity now, bitcasting to signed will
  // be done at texture fetch.

  uint32_t tile_width_samples = xenos::kEdramTileWidthSamples * draw_resolution_scale_x();
  uint32_t tile_height_samples = xenos::kEdramTileHeightSamples * draw_resolution_scale_y();

  // Split the destination pixel index into 32bpp tile and 32bpp-tile-relative
  // pixel index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the pixel position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
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
      tile_width_samples >>
      (uint32_t(dest_is_64bpp) + uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k4X)));
  spv::Id dest_tile_index_x =
      builder.createBinOp(spv::OpUDiv, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_tile_pixel_x =
      builder.createBinOp(spv::OpUMod, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_pixel_y = builder.createCompositeExtract(dest_pixel_coord, type_uint, 1);
  spv::Id const_dest_tile_height_pixels = builder.makeUintConstant(
      tile_height_samples >> uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k2X));
  spv::Id dest_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, dest_pixel_y, const_dest_tile_height_pixels);
  spv::Id dest_tile_pixel_y =
      builder.createBinOp(spv::OpUMod, type_uint, dest_pixel_y, const_dest_tile_height_pixels);

  assert_true(push_constants_member_address != UINT32_MAX);
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(int32_t(push_constants_member_address)));
  spv::Id address_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants, id_vector_temp),
      spv::NoPrecision);

  // Calculate the 32bpp tile index from its X and Y parts.
  spv::Id dest_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.createTriOp(spv::OpBitFieldUExtract, type_uint, address_constant,
                              builder.makeUintConstant(0),
                              builder.makeUintConstant(xenos::kEdramPitchTilesBits)),
          dest_tile_index_y),
      dest_tile_index_x);

  // Load the destination sample index.
  spv::Id dest_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (device_properties.sampleRateShading) {
      assert_true(input_sample_id != spv::NoResult);
      dest_sample_id = builder.createUnaryOp(spv::OpBitcast, type_uint,
                                             builder.createLoad(input_sample_id, spv::NoPrecision));
    } else {
      assert_true(spec_const_sample_id != spv::NoResult);
      // Already uint.
      dest_sample_id = spec_const_sample_id;
    }
  }

  // Transform the destination framebuffer pixel and sample coordinates into the
  // source texture pixel and sample coordinates.

  // First sample bit at 4x with Vulkan standard locations - horizontal sample.
  // Second sample bit at 4x with Vulkan standard locations - vertical sample.
  // At 2x:
  // - Native 2x: top is 1 in Vulkan, bottom is 0.
  // - 2x as 4x: top is 0, bottom is 3.

  spv::Id source_sample_id = dest_sample_id;
  spv::Id source_tile_pixel_x = dest_tile_pixel_x;
  spv::Id source_tile_pixel_y = dest_tile_pixel_y;
  spv::Id source_color_half = spv::NoResult;
  if (!source_is_64bpp && dest_is_64bpp) {
    // 32bpp -> 64bpp, need two samples of the source.
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 32bpp -> 64bpp, 4x ->.
      // Source has 32bpp halves in two adjacent samples.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 4x -> 4x.
        // 1 destination horizontal sample = 2 source horizontal samples.
        // D p0,0 s0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1,0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p0,0 s0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p0,0 s1,1 = S p1,0 s0,1 | S p1,0 s1,1
        // Thus destination horizontal sample -> source horizontal pixel,
        // vertical samples are 1:1.
        source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                               builder.makeUintConstant(1 << 1));
        source_tile_pixel_x = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, dest_sample_id,
                                                   dest_tile_pixel_x, builder.makeUintConstant(1),
                                                   builder.makeUintConstant(31));
      } else if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
        // 32bpp -> 64bpp, 4x -> 2x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 s0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p1,0 s0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p1,0 s1 = S p1,0 s0,1 | S p1,0 s1,1
        // Pixel index can be reused. Sample 1 (for native 2x) or 0 (for 2x as
        // 4x) should become samples 01, sample 0 or 3 should become samples 23.
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
        // 32bpp -> 64bpp, 4x -> 1x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // Horizontal pixel index can be reused. Vertical pixel 1 should
        // become sample 2.
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, builder.makeUintConstant(0), dest_tile_pixel_y,
            builder.makeUintConstant(1), builder.makeUintConstant(1));
        source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_y, builder.makeUintConstant(1));
      }
    } else {
      // 32bpp -> 64bpp, 1x/2x ->.
      // Source has 32bpp halves in two adjacent pixels.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 1x/2x -> 4x.
        // The X part.
        // 1 destination horizontal sample = 2 source horizontal pixels.
        source_tile_pixel_x = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftLeftLogical, type_uint, dest_tile_pixel_x,
                                builder.makeUintConstant(2)),
            dest_sample_id, builder.makeUintConstant(1), builder.makeUintConstant(1));
        // Y is handled by common code.
      } else {
        // 32bpp -> 64bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 1 destination horizontal pixel = 2 source horizontal pixels.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
        // Y is handled by common code.
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    // 64bpp -> 32bpp, also the half to load.
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 64bpp -> 32bpp, -> 4x.
      // The needed half is in the destination horizontal sample index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 4x.
        // D p0,0 s0,0 = S s0,0 low
        // D p0,0 s1,0 = S s0,0 high
        // D p1,0 s0,0 = S s1,0 low
        // D p1,0 s1,0 = S s1,0 high
        // Vertical pixel and sample (second bit) addressing is the same.
        // However, 1 horizontal destination pixel = 1 horizontal source sample.
        source_sample_id = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, dest_sample_id,
                                                dest_tile_pixel_x, builder.makeUintConstant(0),
                                                builder.makeUintConstant(1));
        // 2 destination horizontal samples = 1 source horizontal sample, thus
        // 2 destination horizontal pixels = 1 source horizontal pixel.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 4x.
        // 2 destination horizontal samples = 1 source horizontal pixel, thus
        // 1 destination horizontal pixel = 1 source horizontal pixel. Can reuse
        // horizontal pixel index.
        // Y is handled by common code.
      }
      // Half from the destination horizontal sample index.
      source_color_half = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1));
    } else {
      // 64bpp -> 32bpp, -> 1x/2x.
      // The needed half is in the destination horizontal pixel index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 1x/2x.
        // (Destination horizontal pixel >> 1) & 1 = source horizontal sample
        // (first bit).
        source_sample_id =
            builder.createTriOp(spv::OpBitFieldUExtract, type_uint, dest_tile_pixel_x,
                                builder.makeUintConstant(1), builder.makeUintConstant(1));
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // 64bpp -> 32bpp, 4x -> 2x.
          // Destination vertical samples (1/0 in the first bit for native 2x or
          // 0/1 in the second bit for 2x as 4x) = source vertical samples
          // (second bit).
          if (msaa_2x_attachments_supported_) {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, source_sample_id,
                builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(1));
          } else {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id, source_sample_id,
                builder.makeUintConstant(0), builder.makeUintConstant(1));
          }
        } else {
          // 64bpp -> 32bpp, 4x -> 1x.
          // 1 destination vertical pixel = 1 source vertical sample.
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_tile_pixel_y,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_y, builder.makeUintConstant(1));
        }
        // 2 destination horizontal pixels = 1 source horizontal sample.
        // 4 destination horizontal pixels = 1 source horizontal pixel.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(2));
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 2 destination horizontal pixels = 1 destination source pixel.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
        // Y is handled by common code.
      }
      // Half from the destination horizontal pixel index.
      source_color_half = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                              builder.makeUintConstant(1));
    }
    assert_true(source_color_half != spv::NoResult);
  } else {
    // Same bit count.
    if (key.source_msaa_samples != key.dest_msaa_samples) {
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // Same BPP, 4x -> 1x/2x.
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // Same BPP, 4x -> 2x.
          // Horizontal pixels to samples. Vertical sample (1/0 in the first bit
          // for native 2x or 0/1 in the second bit for 2x as 4x) to second
          // sample bit.
          if (msaa_2x_attachments_supported_) {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(31));
          } else {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                builder.makeUintConstant(0), builder.makeUintConstant(1));
          }
          source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_x, builder.makeUintConstant(1));
        } else {
          // Same BPP, 4x -> 1x.
          // Pixels to samples.
          source_sample_id = builder.createQuadOp(
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
        // Same BPP, 1x/2x -> 1x/2x/4x (as long as they're different).
        // Only the X part - Y is handled by common code.
        if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
          // Horizontal samples to pixels.
          source_tile_pixel_x = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
              builder.makeUintConstant(1), builder.makeUintConstant(31));
        }
      }
    }
  }
  // Common source Y and sample index for 1x/2x AA sources, independent of bits
  // per sample.
  if (key.source_msaa_samples < xenos::MsaaSamples::k4X &&
      key.source_msaa_samples != key.dest_msaa_samples) {
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 1x/2x -> 4x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 4x.
        // Vertical samples (second bit) of 4x destination to vertical sample
        // (1, 0 for native 2x, or 0, 3 for 2x as 4x) of 2x source.
        source_sample_id = builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                               builder.makeUintConstant(1));
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint, source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_sample_id,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
        }
      } else {
        // 1x -> 4x.
        // Vertical samples (second bit) to Y pixels.
        source_tile_pixel_y = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                builder.makeUintConstant(1)),
            dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
      }
    } else {
      // 1x/2x -> different 1x/2x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 1x.
        // Vertical pixels of 2x destination to vertical samples (1, 0 for
        // native 2x, or 0, 3 for 2x as 4x) of 1x source.
        source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                                               builder.makeUintConstant(1));
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint, source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_sample_id,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
        }
        source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_y, builder.makeUintConstant(1));
      } else {
        // 1x -> 2x.
        // Vertical samples (1/0 in the first bit for native 2x or 0/1 in the
        // second bit for 2x as 4x) of 2x destination to vertical pixels of 1x
        // source.
        if (msaa_2x_attachments_supported_) {
          source_tile_pixel_y = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
        } else {
          source_tile_pixel_y = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
        }
      }
    }
  }

  uint32_t source_pixel_width_dwords_log2 =
      uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k4X) + uint32_t(source_is_64bpp);

  if (source_is_color != dest_is_color) {
    // Copying between color and depth / stencil - swap 40-32bpp-sample columns
    // in the pixel index within the source 32bpp tile.
    uint32_t source_32bpp_tile_half_pixels =
        tile_width_samples >> (1 + source_pixel_width_dwords_log2);
    source_tile_pixel_x = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int, source_tile_pixel_x),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(spv::OpULessThan, builder.makeBoolType(), source_tile_pixel_x,
                                    builder.makeUintConstant(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(int32_t(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(-int32_t(source_32bpp_tile_half_pixels)))));
  }

  // Transform the destination 32bpp tile index into the source. After the
  // addition, it may be negative - in which case, the transfer is done across
  // EDRAM addressing wrapping, and xenos::kEdramTileCount must be added to it,
  // but `& (xenos::kEdramTileCount - 1)` handles that regardless of the sign.
  spv::Id source_tile_index = builder.createBinOp(
      spv::OpBitwiseAnd, type_uint,
      builder.createUnaryOp(
          spv::OpBitcast, type_uint,
          builder.createBinOp(
              spv::OpIAdd, type_int,
              builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
              builder.createTriOp(spv::OpBitFieldSExtract, type_int,
                                  builder.createUnaryOp(spv::OpBitcast, type_int, address_constant),
                                  builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                                  builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1)))),
      builder.makeUintConstant(xenos::kEdramTileCount - 1));
  // Split the source 32bpp tile index into X and Y tile index within the source
  // image.
  spv::Id source_pitch_tiles =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, address_constant,
                          builder.makeUintConstant(xenos::kEdramPitchTilesBits),
                          builder.makeUintConstant(xenos::kEdramPitchTilesBits));
  spv::Id source_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x =
      builder.createBinOp(spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  // Finally calculate the source texture coordinates.
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
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(tile_height_samples >> uint32_t(key.source_msaa_samples >=
                                                                       xenos::MsaaSamples::k2X)),
              source_tile_index_y),
          source_tile_pixel_y));

  // Load the source.

  spv::Builder::TextureParameters source_texture_parameters = {};
  id_vector_temp.clear();
  id_vector_temp.push_back(source_pixel_x_int);
  id_vector_temp.push_back(source_pixel_y_int);
  spv::Id source_coordinates[2] = {
      builder.createCompositeConstruct(type_int2, id_vector_temp),
  };
  spv::Id source_sample_ids_int[2] = {};
  if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
    source_sample_ids_int[0] = builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  } else {
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  // Go to the next sample or pixel along X if need to load two dwords.
  bool source_load_is_two_32bpp_samples = !source_is_64bpp && dest_is_64bpp;
  if (source_load_is_two_32bpp_samples) {
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
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
    assert_true(source_color_component_type != spv::NoType);
    spv::Id source_color_vec4_type = builder.makeVectorType(source_color_component_type, 4);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      spv::Id source_color_vec4 = builder.createTextureCall(
          spv::NoPrecision, source_color_vec4_type, false, true, false, false, false,
          source_texture_parameters, spv::ImageOperandsMaskNone);
      uint32_t source_color_components_remaining = source_color_texture_component_mask;
      uint32_t source_color_component_index;
      while (
          rex::bit_scan_forward(source_color_components_remaining, &source_color_component_index)) {
        source_color_components_remaining &= ~(uint32_t(1) << source_color_component_index);
        source_color[i][source_color_component_index] = builder.createCompositeExtract(
            source_color_vec4, source_color_component_type, source_color_component_index);
      }
    }
  }
  spv::Id source_depth_float[2] = {};
  if (source_depth_texture != spv::NoResult) {
    source_texture_parameters.sampler = builder.createLoad(source_depth_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
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
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_stencil[i] = builder.createCompositeExtract(
          builder.createTextureCall(spv::NoPrecision, type_uint4, false, true, false, false, false,
                                    source_texture_parameters, spv::ImageOperandsMaskNone),
          type_uint, 0);
    }
  }

  // Pick the needed 32bpp half of the 64bpp color.
  if (source_is_64bpp && !dest_is_64bpp) {
    uint32_t source_color_half_component_count = source_color_format_component_count >> 1;
    assert_true(source_color_half != spv::NoResult);
    spv::Id source_color_is_second_half = builder.createBinOp(
        spv::OpINotEqual, type_bool, source_color_half, builder.makeUintConstant(0));
    if (mode.output == TransferOutput::kStencilBit) {
      source_color[0][0] = builder.createTriOp(
          spv::OpSelect, source_color_component_type, source_color_is_second_half,
          source_color[0][source_color_half_component_count], source_color[0][0]);
    } else {
      for (uint32_t i = 0; i < source_color_half_component_count; ++i) {
        source_color[0][i] = builder.createTriOp(
            spv::OpSelect, source_color_component_type, source_color_is_second_half,
            source_color[0][source_color_half_component_count + i], source_color[0][i]);
      }
    }
  }

  if (output_fragment_stencil_ref != spv::NoResult && source_stencil[0] != spv::NoResult) {
    // For the depth -> depth case, write the stencil directly to the output.
    assert_true(mode.output == TransferOutput::kDepth);
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
  auto PWLGammaToLinear = [&](spv::Id gamma, bool gamma_pre_saturated) -> spv::Id {
    if (!gamma_pre_saturated) {
      gamma = builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
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
                                 builder.createUnaryBuiltinCall(
                                     type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                     builder.createBinOp(spv::OpFMul, type_float, linear, scale)));
    return builder.createBinOp(spv::OpFMul, type_float, linear,
                               builder.makeFloatConstant(1.0f / 1023.0f));
  };
  auto LinearToPWLGamma = [&](spv::Id linear, bool linear_pre_saturated) -> spv::Id {
    if (!linear_pre_saturated) {
      linear = builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
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
                            builder.createUnaryBuiltinCall(
                                type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                builder.createBinOp(spv::OpFMul, type_float, linear, scale)),
                            builder.makeFloatConstant(1.0f / 255.0f)),
        offset);
  };
  auto PackSource16ComponentToUint = [&](spv::Id component) -> spv::Id {
    if (source_color_is_uint) {
      return component;
    }
    if (source_color_16_is_float) {
      id_vector_temp.clear();
      id_vector_temp.push_back(component);
      id_vector_temp.push_back(const_float_0);
      spv::Id packed_half = builder.createUnaryBuiltinCall(
          type_uint, ext_inst_glsl_std_450, GLSLstd450PackHalf2x16,
          builder.createCompositeConstruct(type_float2, id_vector_temp));
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_half, const_uint_0,
                                 const_uint_16);
    }
    spv::Id component_clamped =
        builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, component,
                                     const_float_minus_1, const_float_1);
    spv::Id component_rounded = builder.createUnaryBuiltinCall(
        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
        builder.createBinOp(spv::OpFMul, type_float, component_clamped, const_float_32767));
    spv::Id component_snorm =
        builder.createUnaryOp(spv::OpConvertFToS, type_int, component_rounded);
    return builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                               builder.createUnaryOp(spv::OpBitcast, type_uint, component_snorm),
                               const_uint_0, const_uint_16);
  };
  auto PackSource16PairToUint32 = [&](spv::Id component_0, spv::Id component_1) -> spv::Id {
    return builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, PackSource16ComponentToUint(component_0),
        PackSource16ComponentToUint(component_1), const_uint_16, const_uint_16);
  };
  auto UnpackDest16ComponentFromUint32 = [&](spv::Id packed_word,
                                             uint32_t component_index) -> spv::Id {
    spv::Id component_offset = component_index & 1 ? const_uint_16 : const_uint_0;
    if (dest_color_is_uint) {
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_word, component_offset,
                                 const_uint_16);
    }
    if (dest_color_16_is_float) {
      spv::Id component_pair = builder.createUnaryBuiltinCall(
          type_float2, ext_inst_glsl_std_450, GLSLstd450UnpackHalf2x16, packed_word);
      return builder.createCompositeExtract(component_pair, type_float, component_index & 1);
    }
    spv::Id component_snorm = builder.createTriOp(spv::OpBitFieldSExtract, type_int, packed_word,
                                                  component_offset, const_uint_16);
    spv::Id component_float =
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createUnaryOp(spv::OpConvertSToF, type_float, component_snorm),
                            const_float_inv_32767);
    return builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                        component_float, const_float_minus_1, const_float_1);
  };

  if (dest_is_64bpp) {
    // Construct the 64bpp color from two 32-bit samples or one 64-bit sample.
    // If `packed` (two uints) are created, use the generic path involving
    // unpacking.
    // Otherwise, the fragment data output must be written to directly by the
    // reached control flow path.
    spv::Id packed[2] = {};
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (gamma_render_target_as_unorm16_) {
            // 8_8_8_8_GAMMA is represented by linear stored in
            // R16G16B16A16_UNORM.
            for (uint32_t i = 0; i < 2; ++i) {
              for (uint32_t j = 0; j < 3; ++j) {
                source_color[i][j] = LinearToPWLGamma(source_color[i][j], true);
              }
            }
          }
        }
          [[fallthrough]];
        case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
          spv::Id component_width = builder.makeUintConstant(8);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float, source_color[i][0], unorm_scale),
                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
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
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[i][0],
                                                        unorm_scale_rgb),
                                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
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
          for (uint32_t i = 0; i < 2; ++i) {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed[i] = SpirvShaderTranslator::UnclampedFloat32To7e3(builder, source_color[i][0],
                                                                     ext_inst_glsl_std_450);
            for (uint32_t j = 1; j < 3; ++j) {
              packed[i] =
                  builder.createQuadOp(spv::OpBitFieldInsert, type_uint, packed[i],
                                       SpirvShaderTranslator::UnclampedFloat32To7e3(
                                           builder, source_color[i][j], ext_inst_glsl_std_450),
                                       builder.makeUintConstant(10 * j), width_rgb);
            }
            // Saturate and convert the alpha.
            spv::Id alpha_saturated =
                builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                             source_color[i][3], float_0, float_1);
            packed[i] = builder.createQuadOp(
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
        // Route through packed uint32 words so mixed source/destination
        // transfer component types (integer vs float fallback) are handled
        // uniformly.
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = PackSource16PairToUint32(source_color[i][0], source_color[i][1]);
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] =
                PackSource16PairToUint32(source_color[0][i << 1], source_color[0][(i << 1) + 1]);
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[i][0];
            if (!source_color_is_uint) {
              packed[i] = builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[0][i];
            if (!source_color_is_uint) {
              packed[i] = builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
      }
    } else {
      assert_true(source_depth_texture != spv::NoResult);
      assert_true(source_stencil_texture != spv::NoResult);
      spv::Id depth_offset = builder.makeUintConstant(8);
      spv::Id depth_width = builder.makeUintConstant(24);
      for (uint32_t i = 0; i < 2; ++i) {
        spv::Id depth24 = spv::NoResult;
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the
            // correct conversion, adding +0.5 and rounding towards zero results
            // in red instead of black in the 4D5307E6 clear shader.
            depth24 = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(spv::OpFMul, type_float, source_depth_float[i],
                                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[i], depth_float24_round(), true, ext_inst_glsl_std_450);
          } break;
        }
        // Merge depth and stencil.
        packed[i] = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil[i],
                                         depth24, depth_offset, depth_width);
      }
    }
    // Common path unless there was a specialized one - unpack two packed 32-bit
    // parts.
    if (packed[0] != spv::NoResult) {
      assert_true(packed[1] != spv::NoResult);
      if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
        id_vector_temp.clear();
        id_vector_temp.push_back(packed[0]);
        id_vector_temp.push_back(packed[1]);
        // If integer transfer formats are unavailable for this sample count,
        // ownership transfer falls back to float formats and raw bits are
        // passed via bitcasts.
        if (!dest_color_is_uint) {
          for (spv::Id& float32 : id_vector_temp) {
            float32 = builder.createUnaryOp(spv::OpBitcast, type_float, float32);
          }
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                            output_fragment_data);
      } else {
        id_vector_temp.clear();
        for (uint32_t i = 0; i < 4; ++i) {
          id_vector_temp.push_back(UnpackDest16ComponentFromUint32(packed[i >> 1], i));
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                            output_fragment_data);
      }
    }
  } else {
    // If `packed` is created, use the generic path involving unpacking.
    // - For a color destination, the packed 32bpp color.
    // - For a depth / stencil destination, stencil in 0:7, depth in 8:31
    //   normally, or depth in 0:23 and zeros in 24:31 with packed_only_depth.
    // - For a stencil bit, stencil in 0:7.
    // Otherwise, the fragment data or fragment depth / stencil output must be
    // written to directly by the reached control flow path.
    spv::Id packed = spv::NoResult;
    bool packed_only_depth = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (mode.output == TransferOutput::kStencilBit) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              source_color[0][0] = LinearToPWLGamma(source_color[0][0], true);
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
              // Color space conversion between k_8_8_8_8 and
              // k_8_8_8_8_GAMMA.
              if (dest_color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8) {
                for (uint32_t i = 0; i < 3; ++i) {
                  source_color[0][i] = LinearToPWLGamma(source_color[0][i], true);
                }
              } else {
                for (uint32_t i = 0; i < 3; ++i) {
                  source_color[0][i] = PWLGammaToLinear(source_color[0][i], true);
                }
              }
            }
            // Same or converted format - passthrough.
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else if (mode.output == TransferOutput::kDepth) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              for (uint32_t i = output_fragment_stencil_ref != spv::NoResult ? 0 : 1; i < 3; ++i) {
                source_color[0][i] = LinearToPWLGamma(source_color[0][i], true);
              }
            }
            // When need only depth, not stencil, skip the red component.
            packed_only_depth = true;
            if (output_fragment_stencil_ref != spv::NoResult) {
              // Write the red component to the stencil reference.
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
            // Put depth in 0:23.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][1],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
            spv::Id component_width = builder.makeUintConstant(8);
            for (uint32_t i = 2; i < 4; ++i) {
              packed = builder.createQuadOp(
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
              for (uint32_t i = 0; i < 3; ++i) {
                source_color[0][i] = LinearToPWLGamma(source_color[0][i], true);
              }
            }
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
            spv::Id component_width = builder.makeUintConstant(8);
            for (uint32_t i = 1; i < 4; ++i) {
              packed = builder.createQuadOp(
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
            for (uint32_t i = 0; i < 4; ++i) {
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
              for (uint32_t i = 1; i < 4; ++i) {
                packed = builder.createQuadOp(
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
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed = SpirvShaderTranslator::UnclampedFloat32To7e3(builder, source_color[0][0],
                                                                  ext_inst_glsl_std_450);
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              for (uint32_t i = 1; i < 3; ++i) {
                packed =
                    builder.createQuadOp(spv::OpBitFieldInsert, type_uint, packed,
                                         SpirvShaderTranslator::UnclampedFloat32To7e3(
                                             builder, source_color[0][i], ext_inst_glsl_std_450),
                                         builder.makeUintConstant(10 * i), width_rgb);
              }
              // Saturate and convert the alpha.
              spv::Id alpha_saturated = builder.createTriBuiltinCall(
                  type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, source_color[0][3],
                  builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
              packed = builder.createQuadOp(
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
          // Route through packed uint32 so destination transfer type (integer
          // vs float fallback) is reconstructed consistently.
          packed = PackSource16ComponentToUint(source_color[0][0]);
          if (mode.output != TransferOutput::kStencilBit) {
            packed = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, packed,
                                          PackSource16ComponentToUint(source_color[0][1]),
                                          const_uint_16, const_uint_16);
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
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
            // Round to the nearest even integer. This seems to be the correct
            // conversion, adding +0.5 and rounding towards zero results in red
            // instead of black in the 4D5307E6 clear shader.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(spv::OpFMul, type_float, source_depth_float[0],
                                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            packed = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[0], depth_float24_round(), true, ext_inst_glsl_std_450);
          } break;
        }
        if (mode.output == TransferOutput::kDepth) {
          packed_only_depth = true;
        } else {
          // Merge depth and stencil.
          packed = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil[0], packed,
                                        builder.makeUintConstant(8), builder.makeUintConstant(24));
        }
      }
    }
    switch (mode.output) {
      case TransferOutput::kColor: {
        // Unless a special path was taken, unpack the raw 32bpp value into the
        // 32bpp color output.
        if (packed != spv::NoResult) {
          switch (dest_color_format) {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8:
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
              spv::Id component_width = builder.makeUintConstant(8);
              spv::Id unorm_scale = builder.makeFloatConstant(1.0f / 255.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
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
                // 8_8_8_8_GAMMA is represented by linear stored in
                // R16G16B16A16_UNORM.
                for (uint32_t i = 0; i < 3; ++i) {
                  id_vector_temp[i] = PWLGammaToLinear(id_vector_temp[i], true);
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
              for (uint32_t i = 0; i < 4; ++i) {
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
              // Color.
              for (uint32_t i = 0; i < 3; ++i) {
                id_vector_temp.push_back(SpirvShaderTranslator::Float7e3To32(
                    builder, packed, 10 * i, false, ext_inst_glsl_std_450));
              }
              // Alpha.
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
              for (uint32_t i = 0; i < 2; ++i) {
                id_vector_temp.push_back(UnpackDest16ComponentFromUint32(packed, i));
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
              // Float32 is transferred as uint32 to preserve NaN encodings.
              // If the integer transfer format is unavailable for this sample
              // count, the transfer format is float and this bitcast path is
              // used.
              spv::Id float32 = packed;
              if (!dest_color_is_uint) {
                float32 = builder.createUnaryOp(spv::OpBitcast, type_float, float32);
              }
              builder.createStore(float32, output_fragment_data);
            } break;
            default:
              // A 64bpp format (handled separately) or an invalid one.
              assert_unhandled_case(dest_color_format);
          }
        }
      } break;
      case TransferOutput::kDepth: {
        if (packed) {
          spv::Id guest_depth24 = packed;
          if (!packed_only_depth) {
            // Extract the depth bits.
            guest_depth24 = builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_depth24,
                                                builder.makeUintConstant(8));
          }
          // Load the host float32 depth, check if, when converted to the guest
          // format, it's the same as the guest source, thus up to date, and if
          // it is, write host float32 depth, otherwise do the guest -> host
          // conversion.
          spv::Id host_depth32 = spv::NoResult;
          if (host_depth_source_texture != spv::NoResult) {
            // Convert position and sample index from within the destination
            // tile to within the host depth source tile, like for the guest
            // render target, but for 32bpp -> 32bpp only.
            spv::Id host_depth_source_sample_id = dest_sample_id;
            spv::Id host_depth_source_tile_pixel_x = dest_tile_pixel_x;
            spv::Id host_depth_source_tile_pixel_y = dest_tile_pixel_y;
            if (key.host_depth_source_msaa_samples != key.dest_msaa_samples) {
              if (key.host_depth_source_msaa_samples >= xenos::MsaaSamples::k4X) {
                // 4x -> 1x/2x.
                if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
                  // 4x -> 2x.
                  // Horizontal pixels to samples. Vertical sample (1/0 in the
                  // first bit for native 2x or 0/1 in the second bit for 2x as
                  // 4x) to second sample bit.
                  if (msaa_2x_attachments_supported_) {
                    host_depth_source_sample_id = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                        builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1)),
                        builder.makeUintConstant(1), builder.makeUintConstant(31));
                  } else {
                    host_depth_source_sample_id = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                        builder.makeUintConstant(0), builder.makeUintConstant(1));
                  }
                  host_depth_source_tile_pixel_x =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1));
                } else {
                  // 4x -> 1x.
                  // Pixels to samples.
                  host_depth_source_sample_id = builder.createQuadOp(
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
                // 1x/2x -> 1x/2x/4x (as long as they're different).
                // Only the X part - Y is handled by common code.
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // Horizontal samples to pixels.
                  host_depth_source_tile_pixel_x = builder.createQuadOp(
                      spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                      builder.makeUintConstant(1), builder.makeUintConstant(31));
                }
              }
              // Host depth source Y and sample index for 1x/2x AA sources.
              if (key.host_depth_source_msaa_samples < xenos::MsaaSamples::k4X) {
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // 1x/2x -> 4x.
                  if (key.host_depth_source_msaa_samples == xenos::MsaaSamples::k2X) {
                    // 2x -> 4x.
                    // Vertical samples (second bit) of 4x destination to
                    // vertical sample (1, 0 for native 2x, or 0, 3 for 2x as
                    // 4x) of 2x source.
                    host_depth_source_sample_id =
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1));
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_sample_id = builder.createBinOp(
                          spv::OpBitwiseXor, type_uint, host_depth_source_sample_id,
                          builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint, host_depth_source_sample_id,
                          host_depth_source_sample_id, builder.makeUintConstant(1),
                          builder.makeUintConstant(1));
                    }
                  } else {
                    // 1x -> 4x.
                    // Vertical samples (second bit) to Y pixels.
                    host_depth_source_tile_pixel_y = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint,
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1)),
                        dest_tile_pixel_y, builder.makeUintConstant(1),
                        builder.makeUintConstant(31));
                  }
                } else {
                  // 1x/2x -> different 1x/2x.
                  if (key.host_depth_source_msaa_samples == xenos::MsaaSamples::k2X) {
                    // 2x -> 1x.
                    // Vertical pixels of 2x destination to vertical samples (1,
                    // 0 for native 2x, or 0, 3 for 2x as 4x) of 1x source.
                    host_depth_source_sample_id =
                        builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                                            builder.makeUintConstant(1));
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_sample_id = builder.createBinOp(
                          spv::OpBitwiseXor, type_uint, host_depth_source_sample_id,
                          builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint, host_depth_source_sample_id,
                          host_depth_source_sample_id, builder.makeUintConstant(1),
                          builder.makeUintConstant(1));
                    }
                    host_depth_source_tile_pixel_y =
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                                            builder.makeUintConstant(1));
                  } else {
                    // 1x -> 2x.
                    // Vertical samples (1/0 in the first bit for native 2x or
                    // 0/1 in the second bit for 2x as 4x) of 2x destination to
                    // vertical pixels of 1x source.
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_tile_pixel_y = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    } else {
                      host_depth_source_tile_pixel_y = builder.createQuadOp(
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
            assert_true(push_constants_member_host_depth_address != UINT32_MAX);
            id_vector_temp.clear();
            id_vector_temp.push_back(
                builder.makeIntConstant(int32_t(push_constants_member_host_depth_address)));
            spv::Id host_depth_address_constant =
                builder.createLoad(builder.createAccessChain(spv::StorageClassPushConstant,
                                                             push_constants, id_vector_temp),
                                   spv::NoPrecision);
            // Transform the destination tile index into the host depth source.
            // After the addition, it may be negative - in which case, the
            // transfer is done across EDRAM addressing wrapping, and
            // xenos::kEdramTileCount must be added to it, but
            // `& (xenos::kEdramTileCount - 1)` handles that regardless of the
            // sign.
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
                            builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                            builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1)))),
                builder.makeUintConstant(xenos::kEdramTileCount - 1));
            // Split the host depth source tile index into X and Y tile index
            // within the source image.
            spv::Id host_depth_source_pitch_tiles =
                builder.createTriOp(spv::OpBitFieldUExtract, type_uint, host_depth_address_constant,
                                    builder.makeUintConstant(xenos::kEdramPitchTilesBits),
                                    builder.makeUintConstant(xenos::kEdramPitchTilesBits));
            spv::Id host_depth_source_tile_index_y =
                builder.createBinOp(spv::OpUDiv, type_uint, host_depth_source_tile_index,
                                    host_depth_source_pitch_tiles);
            spv::Id host_depth_source_tile_index_x =
                builder.createBinOp(spv::OpUMod, type_uint, host_depth_source_tile_index,
                                    host_depth_source_pitch_tiles);
            // Finally calculate the host depth source texture coordinates.
            spv::Id host_depth_source_pixel_x_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(tile_width_samples >>
                                                                 uint32_t(key.source_msaa_samples >=
                                                                          xenos::MsaaSamples::k4X)),
                                        host_depth_source_tile_index_x),
                    host_depth_source_tile_pixel_x));
            spv::Id host_depth_source_pixel_y_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(tile_height_samples >>
                                                                 uint32_t(key.source_msaa_samples >=
                                                                          xenos::MsaaSamples::k2X)),
                                        host_depth_source_tile_index_y),
                    host_depth_source_tile_pixel_y));
            // Load the host depth source.
            spv::Builder::TextureParameters host_depth_source_texture_parameters = {};
            host_depth_source_texture_parameters.sampler =
                builder.createLoad(host_depth_source_texture, spv::NoPrecision);
            id_vector_temp.clear();
            id_vector_temp.push_back(host_depth_source_pixel_x_int);
            id_vector_temp.push_back(host_depth_source_pixel_y_int);
            host_depth_source_texture_parameters.coords =
                builder.createCompositeConstruct(type_int2, id_vector_temp);
            if (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X) {
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
            // Get the address in the EDRAM scratch buffer and load from there.
            // The beginning of the buffer is (0, 0) of the destination.
            // 40-sample columns are not swapped for addressing simplicity
            // (because this is used for depth -> depth transfers, where
            // swapping isn't needed).
            // Convert samples to pixels.
            assert_true(key.host_depth_source_msaa_samples == xenos::MsaaSamples::k1X);
            spv::Id dest_tile_sample_x = dest_tile_pixel_x;
            spv::Id dest_tile_sample_y = dest_tile_pixel_y;
            if (key.dest_msaa_samples >= xenos::MsaaSamples::k2X) {
              if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                // Horizontal sample index in bit 0.
                dest_tile_sample_x = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                    builder.makeUintConstant(1), builder.makeUintConstant(31));
              }
              // Vertical sample index as 1 or 0 in bit 0 for true 2x or as 0
              // or 1 in bit 1 for 4x or for 2x emulated as 4x.
              dest_tile_sample_y = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint,
                  builder.createBinOp((key.dest_msaa_samples == xenos::MsaaSamples::k2X &&
                                       msaa_2x_attachments_supported_)
                                          ? spv::OpBitwiseXor
                                          : spv::OpShiftRightLogical,
                                      type_uint, dest_sample_id, builder.makeUintConstant(1)),
                  dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
            }
            // Combine the tile sample index and the tile index.
            // The tile index doesn't need to be wrapped, as the host depth is
            // written to the beginning of the buffer, without the base offset.
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
            // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is
            // generated, it's Uniform.
            host_depth32 = builder.createUnaryOp(
                spv::OpBitcast, type_float,
                builder.createLoad(
                    builder.createAccessChain(spv::StorageClassUniform, host_depth_source_buffer,
                                              id_vector_temp),
                    spv::NoPrecision));
          }
          spv::Block* depth24_to_depth32_header = builder.getBuildPoint();
          spv::Id depth24_to_depth32_convert_id = spv::NoResult;
          spv::Block* depth24_to_depth32_merge = nullptr;
          spv::Id host_depth24 = spv::NoResult;
          if (host_depth32 != spv::NoResult) {
            // Convert the host depth value to the guest format and check if it
            // matches the value in the currently owning guest render target.
            switch (dest_depth_format) {
              case xenos::DepthRenderTargetFormat::kD24S8: {
                // Round to the nearest even integer. This seems to be the
                // correct conversion, adding +0.5 and rounding towards zero
                // results in red instead of black in the 4D5307E6 clear shader.
                host_depth24 = builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createUnaryBuiltinCall(
                        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                        builder.createBinOp(spv::OpFMul, type_float, host_depth32,
                                            builder.makeFloatConstant(float(0xFFFFFF)))));
              } break;
              case xenos::DepthRenderTargetFormat::kD24FS8: {
                host_depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                    builder, host_depth32, depth_float24_round(), true, ext_inst_glsl_std_450);
              } break;
            }
            assert_true(host_depth24 != spv::NoResult);
            // Update the header block pointer after the conversion (to avoid
            // assuming that the conversion doesn't branch).
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
          // Convert the guest 24-bit depth to float32 (in an open conditional
          // if the host depth is also loaded).
          spv::Id guest_depth32 = spv::NoResult;
          switch (dest_depth_format) {
            case xenos::DepthRenderTargetFormat::kD24S8: {
              // Multiplying by 1.0 / 0xFFFFFF produces an incorrect result (for
              // 0xC00000, for instance - which is 2_10_10_10 clear to 0001) -
              // rescale from 0...0xFFFFFF to 0...0x1000000 doing what true
              // float division followed by multiplication does (on x86-64 MSVC
              // with default SSE rounding) - values starting from 0x800000
              // become bigger by 1; then accurately bias the result's exponent.
              guest_depth32 = builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(
                      spv::OpConvertUToF, type_float,
                      builder.createBinOp(
                          spv::OpIAdd, type_uint, guest_depth24,
                          builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_depth24,
                                              builder.makeUintConstant(23)))),
                  builder.makeFloatConstant(1.0f / float(1 << 24)));
            } break;
            case xenos::DepthRenderTargetFormat::kD24FS8: {
              guest_depth32 = SpirvShaderTranslator::Depth20e4To32(builder, guest_depth24, 0, true,
                                                                   false, ext_inst_glsl_std_450);
            } break;
          }
          assert_true(guest_depth32 != spv::NoResult);
          spv::Id fragment_depth32 = guest_depth32;
          if (host_depth32 != spv::NoResult) {
            assert_not_null(depth24_to_depth32_merge);
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
          // Unpack the stencil into the stencil reference output if needed and
          // not already written.
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
          // Kill the sample if the needed stencil bit is not set.
          assert_true(push_constants_member_stencil_mask != UINT32_MAX);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.makeIntConstant(int32_t(push_constants_member_stencil_mask)));
          spv::Id stencil_mask_constant =
              builder.createLoad(builder.createAccessChain(spv::StorageClassPushConstant,
                                                           push_constants, id_vector_temp),
                                 spv::NoPrecision);
          SpirvBuilder::IfBuilder stencil_kill_if(
              builder.createBinOp(
                  spv::OpIEqual, type_bool,
                  builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed, stencil_mask_constant),
                  builder.makeUintConstant(0)),
              spv::SelectionControlMaskNone, builder);
          builder.createNoResultOp(spv::OpKill);
          // OpKill terminates the block.
          stencil_kill_if.makeEndIf(false);
        }
      } break;
    }
  }

  // End the main function and make it the entry point.
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

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the shader module, and store the handle even if creation fails not
  // to try to create it again later.
  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      vulkan_device, reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (shader_module == VK_NULL_HANDLE) {
    REXGPU_ERROR(
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

  const TransferModeInfo& mode = kTransferModes[size_t(key.shader_key.mode)];

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && device_properties.dynamicRendering;

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

  uint32_t dest_sample_count = uint32_t(1) << uint32_t(key.shader_key.dest_msaa_samples);
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
  uint32_t sample_id_specialization_constant;
  VkSpecializationInfo sample_id_specialization_info;
  if (dest_is_masked_sample) {
    sample_id_specialization_map_entry.constantID = 0;
    sample_id_specialization_map_entry.offset = 0;
    sample_id_specialization_map_entry.size = sizeof(uint32_t);
    sample_id_specialization_constant = 0;
    sample_id_specialization_info.mapEntryCount = 1;
    sample_id_specialization_info.pMapEntries = &sample_id_specialization_map_entry;
    sample_id_specialization_info.dataSize = sizeof(sample_id_specialization_constant);
    sample_id_specialization_info.pData = &sample_id_specialization_constant;
    shader_stages[1].pSpecializationInfo = &sample_id_specialization_info;
  }

  VkVertexInputBindingDescription vertex_input_binding;
  vertex_input_binding.binding = 0;
  vertex_input_binding.stride = sizeof(float) * 2;
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

  // Dynamic, to stay within maxViewportDimensions while preferring a
  // power-of-two factor for converting from pixel coordinates to NDC for exact
  // precision.
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

  // For samples other than the first, will be changed for the pipelines for
  // other samples.
  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample_state.rasterizationSamples =
      (dest_sample_count == 2 && !msaa_2x_attachments_supported_)
          ? VK_SAMPLE_COUNT_4_BIT
          : VkSampleCountFlagBits(dest_sample_count);
  if (dest_sample_count > 1) {
    if (device_properties.sampleRateShading) {
      multisample_state.sampleShadingEnable = VK_TRUE;
      multisample_state.minSampleShading = 1.0f;
      if (dest_sample_count == 2 && !msaa_2x_attachments_supported_) {
        // Emulating 2x MSAA as samples 0 and 3 of 4x MSAA when 2x is not
        // supported.
        sample_mask = 0b1001;
      }
    } else {
      sample_mask = 0b1;
    }
    if (sample_mask != UINT32_MAX) {
      multisample_state.pSampleMask = &sample_mask;
    }
  }

  // Whether the depth / stencil state is used depends on the presence of a
  // depth attachment in the render pass - but not making assumptions about
  // whether the render pass contains any specific attachments, so setting up
  // valid depth / stencil state unconditionally.
  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  if (mode.output == TransferOutput::kDepth) {
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_TRUE;
    depth_stencil_state.depthCompareOp =
        REXCVAR_GET(depth_transfer_not_equal_test) ? VK_COMPARE_OP_NOT_EQUAL : VK_COMPARE_OP_ALWAYS;
  }
  if ((mode.output == TransferOutput::kDepth &&
       vulkan_device->extensions().ext_EXT_shader_stencil_export) ||
      mode.output == TransferOutput::kStencilBit) {
    depth_stencil_state.stencilTestEnable = VK_TRUE;
    depth_stencil_state.front.failOp = VK_STENCIL_OP_KEEP;
    depth_stencil_state.front.passOp = VK_STENCIL_OP_REPLACE;
    depth_stencil_state.front.depthFailOp = VK_STENCIL_OP_REPLACE;
    // Using ALWAYS, not NOT_EQUAL, so depth writing is unaffected by stencil
    // being different.
    depth_stencil_state.front.compareOp = VK_COMPARE_OP_ALWAYS;
    // Will be dynamic for stencil bit output.
    depth_stencil_state.front.writeMask = UINT8_MAX;
    depth_stencil_state.front.reference = UINT8_MAX;
    depth_stencil_state.back = depth_stencil_state.front;
  }

  // Whether the color blend state is used depends on the presence of color
  // attachments in the render pass - but not making assumptions about whether
  // the render pass contains any specific attachments, so setting up valid
  // color blend state unconditionally.
  VkPipelineColorBlendAttachmentState color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend_state.attachmentCount =
      32 - rex::lzcnt(key.render_pass_key.depth_and_color_used >> 1);
  color_blend_state.pAttachments = color_blend_attachments;
  if (mode.output == TransferOutput::kColor) {
    assert_true(device_properties.independentBlend);
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
  pipeline_create_info.stageCount = uint32_t(rex::countof(shader_stages));
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
  pipeline_create_info.layout = transfer_pipeline_layouts_[size_t(mode.pipeline_layout)];
  pipeline_create_info.renderPass = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                    &pipelines[0]) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create the render target ownership "
        "transfer pipeline for render pass 0x{:08X}, shader 0x{:08X}",
        key.render_pass_key.key, key.shader_key.key);
    transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
    return nullptr;
  }
  if (dest_is_masked_sample) {
    assert_true(multisample_state.pSampleMask == &sample_mask);
    pipeline_create_info.flags =
        (pipeline_create_info.flags & ~VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) |
        VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    pipeline_create_info.basePipelineHandle = pipelines[0];
    for (uint32_t i = 1; i < dest_sample_count; ++i) {
      // Emulating 2x MSAA as samples 0 and 3 of 4x MSAA when 2x is not
      // supported.
      uint32_t host_sample_index =
          (dest_sample_count == 2 && !msaa_2x_attachments_supported_ && i == 1) ? 3 : i;
      sample_id_specialization_constant = host_sample_index;
      sample_mask = uint32_t(1) << host_sample_index;
      if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                        &pipelines[i]) != VK_SUCCESS) {
        REXGPU_ERROR(
            "VulkanRenderTargetCache: Failed to create the render target "
            "ownership transfer pipeline for render pass 0x{:08X}, shader "
            "0x{:08X}, sample {}",
            key.render_pass_key.key, key.shader_key.key, i);
        for (uint32_t j = 0; j < i; ++j) {
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
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  uint64_t current_submission = command_processor_.GetCurrentSubmission();
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

  bool resolve_clear_needed = render_target_resolve_clear_values && resolve_clear_rectangle;
  VkClearRect resolve_clear_rect;
  if (resolve_clear_needed) {
    // Assuming the rectangle is already clamped by the setup function from the
    // common render target cache.
    resolve_clear_rect.rect.offset.x =
        int32_t(resolve_clear_rectangle->x_pixels * draw_resolution_scale_x());
    resolve_clear_rect.rect.offset.y =
        int32_t(resolve_clear_rectangle->y_pixels * draw_resolution_scale_y());
    resolve_clear_rect.rect.extent.width =
        resolve_clear_rectangle->width_pixels * draw_resolution_scale_x();
    resolve_clear_rect.rect.extent.height =
        resolve_clear_rectangle->height_pixels * draw_resolution_scale_y();
    resolve_clear_rect.baseArrayLayer = 0;
    resolve_clear_rect.layerCount = 1;
  }

  // Do host depth storing for the depth destination (assuming there can be only
  // one depth destination) where depth destination == host depth source.
  bool host_depth_store_set_up = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_vulkan_rt.key();
    if (!dest_rt_key.is_depth) {
      continue;
    }
    const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
    for (const Transfer& transfer : depth_transfers) {
      if (transfer.host_depth_source != dest_rt) {
        continue;
      }
      if (!host_depth_store_set_up) {
        // Pipeline.
        command_processor_.BindExternalComputePipeline(
            host_depth_store_pipelines_[size_t(dest_rt_key.msaa_samples)]);
        // Descriptor set bindings.
        VkDescriptorSet host_depth_store_descriptor_sets[] = {
            edram_storage_buffer_descriptor_set_,
            dest_vulkan_rt.GetDescriptorSetTransferSource(),
        };
        command_buffer.CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_COMPUTE, host_depth_store_pipeline_layout_, 0,
            uint32_t(rex::countof(host_depth_store_descriptor_sets)),
            host_depth_store_descriptor_sets, 0, nullptr);
        // Render target constant.
        HostDepthStoreRenderTargetConstant host_depth_store_render_target_constant =
            GetHostDepthStoreRenderTargetConstant(dest_rt_key.pitch_tiles_at_32bpp,
                                                  msaa_2x_attachments_supported_);
        command_buffer.CmdVkPushConstants(
            host_depth_store_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            uint32_t(offsetof(HostDepthStoreConstants, render_target)),
            sizeof(host_depth_store_render_target_constant),
            &host_depth_store_render_target_constant);
        // Barriers - don't need to try to combine them with the rest of
        // render target transfer barriers now - if this happens, after host
        // depth storing, SHADER_READ -> DEPTH_STENCIL_ATTACHMENT_WRITE will be
        // done anyway even in the best case, so it's not possible to have all
        // the barriers in one place here.
        UseEdramBuffer(EdramBufferUsage::kComputeWrite);
        // Always transitioning both depth and stencil, not storing separate
        // usage flags for depth and stencil.
        command_processor_.PushImageMemoryBarrier(
            dest_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                         VK_IMAGE_ASPECT_STENCIL_BIT),
            dest_vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            dest_vulkan_rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT,
            dest_vulkan_rt.current_layout(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dest_vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        host_depth_store_set_up = true;
      }
      Transfer::Rectangle transfer_rectangles[Transfer::kMaxRectanglesWithCutout];
      uint32_t transfer_rectangle_count = transfer.GetRectangles(
          dest_rt_key.base_tiles, dest_rt_key.pitch_tiles_at_32bpp, dest_rt_key.msaa_samples, false,
          transfer_rectangles, resolve_clear_rectangle);
      assert_not_zero(transfer_rectangle_count);
      HostDepthStoreRectangleConstant host_depth_store_rectangle_constant;
      for (uint32_t j = 0; j < transfer_rectangle_count; ++j) {
        uint32_t group_count_x, group_count_y;
        GetHostDepthStoreRectangleInfo(transfer_rectangles[j], dest_rt_key.msaa_samples,
                                       host_depth_store_rectangle_constant, group_count_x,
                                       group_count_y);
        command_buffer.CmdVkPushConstants(
            host_depth_store_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            uint32_t(offsetof(HostDepthStoreConstants, rectangle)),
            sizeof(host_depth_store_rectangle_constant), &host_depth_store_rectangle_constant);
        command_processor_.SubmitBarriers(true);
        command_buffer.CmdVkDispatch(group_count_x, group_count_y, 1);
        MarkEdramBufferModified();
      }
    }
    break;
  }

  constexpr VkPipelineStageFlags kSourceStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags kSourceAccessMask = VK_ACCESS_SHADER_READ_BIT;
  constexpr VkImageLayout kSourceLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  // Try to insert as many barriers as possible in one place, hoping that in the
  // best case (no cross-copying between current render targets), barriers will
  // need to be only inserted here, not between transfers. In case of
  // cross-copying, if the destination use is going to happen before the source
  // use, choose the destination state, otherwise the source state - to match
  // the order in which transfers will actually happen (otherwise there will be
  // just a useless switch back and forth).
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    const std::vector<Transfer>& dest_transfers = render_target_transfers[i];
    if (!resolve_clear_needed && dest_transfers.empty()) {
      continue;
    }
    // Transition the destination, only if not going to be used as a source
    // earlier.
    bool dest_used_previously_as_source = false;
    for (uint32_t j = 0; j < i; ++j) {
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
          dest_vulkan_rt.image(),
          ui::vulkan::util::InitializeSubresourceRange(
              dest_vulkan_rt.key().is_depth
                  ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                  : VK_IMAGE_ASPECT_COLOR_BIT),
          dest_vulkan_rt.current_stage_mask(), dest_dst_stage_mask,
          dest_vulkan_rt.current_access_mask(), dest_dst_access_mask,
          dest_vulkan_rt.current_layout(), dest_new_layout);
      dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask, dest_new_layout);
    }
    // Transition the sources, only if not going to be used as destinations
    // earlier.
    for (const Transfer& transfer : dest_transfers) {
      bool source_previously_used_as_dest = false;
      bool host_depth_source_previously_used_as_dest = false;
      for (uint32_t j = 0; j < i; ++j) {
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
            source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                source_vulkan_rt.key().is_depth
                    ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                    : VK_IMAGE_ASPECT_COLOR_BIT),
            source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            source_vulkan_rt.current_access_mask(), kSourceAccessMask,
            source_vulkan_rt.current_layout(), kSourceLayout);
        source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
      }
      // transfer.host_depth_source == dest_rt means the EDRAM buffer will be
      // used instead, no need to transition.
      if (transfer.host_depth_source && transfer.host_depth_source != dest_rt &&
          !host_depth_source_previously_used_as_dest) {
        auto& host_depth_source_vulkan_rt =
            *static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
        command_processor_.PushImageMemoryBarrier(
            host_depth_source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                         VK_IMAGE_ASPECT_STENCIL_BIT),
            host_depth_source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            host_depth_source_vulkan_rt.current_access_mask(), kSourceAccessMask,
            host_depth_source_vulkan_rt.current_layout(), kSourceLayout);
        host_depth_source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
      }
    }
  }
  if (host_depth_store_set_up) {
    // Will be reading copied host depth from the EDRAM buffer.
    UseEdramBuffer(EdramBufferUsage::kFragmentRead);
  }

  // Perform the transfers and clears.

  TransferPipelineLayoutIndex last_transfer_pipeline_layout_index =
      TransferPipelineLayoutIndex::kCount;
  uint32_t transfer_descriptor_sets_bound = 0;
  uint32_t transfer_push_constants_set = 0;
  VkDescriptorSet last_descriptor_set_host_depth_stencil_textures = VK_NULL_HANDLE;
  VkDescriptorSet last_descriptor_set_depth_stencil_textures = VK_NULL_HANDLE;
  VkDescriptorSet last_descriptor_set_color_texture = VK_NULL_HANDLE;
  TransferAddressConstant last_host_depth_address_constant;
  TransferAddressConstant last_address_constant;

  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& current_transfers = render_target_transfers[i];
    if (current_transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_vulkan_rt.key();

    // Late barriers in case there was cross-copying that prevented merging of
    // barriers.
    {
      VkPipelineStageFlags dest_dst_stage_mask;
      VkAccessFlags dest_dst_access_mask;
      VkImageLayout dest_new_layout;
      dest_vulkan_rt.GetDrawUsage(&dest_dst_stage_mask, &dest_dst_access_mask, &dest_new_layout);
      command_processor_.PushImageMemoryBarrier(
          dest_vulkan_rt.image(),
          ui::vulkan::util::InitializeSubresourceRange(
              dest_rt_key.is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                   : VK_IMAGE_ASPECT_COLOR_BIT),
          dest_vulkan_rt.current_stage_mask(), dest_dst_stage_mask,
          dest_vulkan_rt.current_access_mask(), dest_dst_access_mask,
          dest_vulkan_rt.current_layout(), dest_new_layout);
      dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask, dest_new_layout);
    }

    // Get the objects needed for transfers to the destination.
    // TODO(Triang3l): Reuse the guest render pass for transfers where possible
    // (if the Vulkan format used for drawing is also usable for transfers - for
    // instance, R8G8B8A8_UNORM can be used for both, so the guest pass can be
    // reused, but R16G16B16A16_SFLOAT render targets use R16G16B16A16_UINT for
    // transfers, so the transfer pass has to be separate) to avoid stores and
    // loads on tile-based devices to make this actually applicable. Also
    // overall perform all non-cross-copying transfers for the current
    // framebuffer configuration in a single pass, to load / store only once.
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
    // Don't enter the render pass immediately - may still insert source
    // barriers later.
    VkImageView transfer_dest_view = dest_rt_key.is_depth ? dest_vulkan_rt.view_depth_stencil()
                                                          : dest_vulkan_rt.view_color_transfer();

    if (!current_transfers.empty()) {
      uint32_t dest_pitch_tiles = dest_rt_key.GetPitchTiles();
      bool dest_is_64bpp = dest_rt_key.Is64bpp();

      // Gather shader keys and sort to reduce pipeline state and binding
      // switches. Also gather stencil rectangles to clear if needed.
      bool need_stencil_bit_draws =
          dest_rt_key.is_depth && !vulkan_device->extensions().ext_EXT_shader_stencil_export;
      current_transfer_invocations_.clear();
      current_transfer_invocations_.reserve(current_transfers.size()
                                            << uint32_t(need_stencil_bit_draws));
      uint32_t rt_sort_index = 0;
      TransferShaderKey new_transfer_shader_key;
      new_transfer_shader_key.dest_msaa_samples = dest_rt_key.msaa_samples;
      new_transfer_shader_key.dest_resource_format = dest_rt_key.resource_format;
      uint32_t stencil_clear_rectangle_count = 0;
      for (uint32_t j = 0; j <= uint32_t(need_stencil_bit_draws); ++j) {
        // j == 0 - color or depth.
        // j == 1 - stencil bits.
        // Stencil bit writing always requires a different root signature,
        // handle these separately. Stencil never has a host depth source.
        // Clear previously set sort indices.
        for (const Transfer& transfer : current_transfers) {
          auto host_depth_source_vulkan_rt =
              static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_vulkan_rt) {
            host_depth_source_vulkan_rt->SetTemporarySortIndex(UINT32_MAX);
          }
          assert_not_null(transfer.source);
          auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
          source_vulkan_rt.SetTemporarySortIndex(UINT32_MAX);
        }
        for (const Transfer& transfer : current_transfers) {
          assert_not_null(transfer.source);
          auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
          VulkanRenderTarget* host_depth_source_vulkan_rt =
              j ? nullptr : static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_vulkan_rt &&
              host_depth_source_vulkan_rt->temporary_sort_index() == UINT32_MAX) {
            host_depth_source_vulkan_rt->SetTemporarySortIndex(rt_sort_index++);
          }
          if (source_vulkan_rt.temporary_sort_index() == UINT32_MAX) {
            source_vulkan_rt.SetTemporarySortIndex(rt_sort_index++);
          }
          RenderTargetKey source_rt_key = source_vulkan_rt.key();
          new_transfer_shader_key.source_msaa_samples = source_rt_key.msaa_samples;
          new_transfer_shader_key.source_resource_format = source_rt_key.resource_format;
          bool host_depth_source_is_copy = host_depth_source_vulkan_rt == &dest_vulkan_rt;
          // The host depth copy buffer has only raw samples.
          new_transfer_shader_key.host_depth_source_msaa_samples =
              (host_depth_source_vulkan_rt && !host_depth_source_is_copy)
                  ? host_depth_source_vulkan_rt->key().msaa_samples
                  : xenos::MsaaSamples::k1X;
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
        assert_not_null(it->transfer.source);
        auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(it->transfer.source);
        command_processor_.PushImageMemoryBarrier(
            source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                source_vulkan_rt.key().is_depth
                    ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                    : VK_IMAGE_ASPECT_COLOR_BIT),
            source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            source_vulkan_rt.current_access_mask(), kSourceAccessMask,
            source_vulkan_rt.current_layout(), kSourceLayout);
        source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
        auto host_depth_source_vulkan_rt =
            static_cast<VulkanRenderTarget*>(it->transfer.host_depth_source);
        if (host_depth_source_vulkan_rt) {
          TransferShaderKey transfer_shader_key = it->shader_key;
          if (transfer_shader_key.mode == TransferMode::kDepthAndHostDepthCopyToDepth ||
              transfer_shader_key.mode == TransferMode::kColorAndHostDepthCopyToDepth) {
            // Reading copied host depth from the EDRAM buffer.
            UseEdramBuffer(EdramBufferUsage::kFragmentRead);
          } else {
            // Reading host depth from the texture.
            command_processor_.PushImageMemoryBarrier(
                host_depth_source_vulkan_rt->image(),
                ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                             VK_IMAGE_ASPECT_STENCIL_BIT),
                host_depth_source_vulkan_rt->current_stage_mask(), kSourceStageMask,
                host_depth_source_vulkan_rt->current_access_mask(), kSourceAccessMask,
                host_depth_source_vulkan_rt->current_layout(), kSourceLayout);
            host_depth_source_vulkan_rt->SetUsage(kSourceStageMask, kSourceAccessMask,
                                                  kSourceLayout);
          }
        }
      }

      // Perform the transfers for the render target.

      command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
          transfer_render_pass, transfer_framebuffer, transfer_dest_view, dest_rt_key.is_depth);

      if (stencil_clear_rectangle_count) {
        VkClearAttachment* stencil_clear_attachment;
        VkClearRect* stencil_clear_rect_write_ptr;
        command_buffer.CmdClearAttachmentsEmplace(1, stencil_clear_attachment,
                                                  stencil_clear_rectangle_count,
                                                  stencil_clear_rect_write_ptr);
        stencil_clear_attachment->aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        stencil_clear_attachment->colorAttachment = 0;
        stencil_clear_attachment->clearValue.depthStencil.depth = 0.0f;
        stencil_clear_attachment->clearValue.depthStencil.stencil = 0;
        for (const Transfer& transfer : current_transfers) {
          Transfer::Rectangle transfer_stencil_clear_rectangles[Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_stencil_clear_rectangle_count = transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_stencil_clear_rectangles, resolve_clear_rectangle);
          for (uint32_t j = 0; j < transfer_stencil_clear_rectangle_count; ++j) {
            const Transfer::Rectangle& stencil_clear_rectangle =
                transfer_stencil_clear_rectangles[j];
            stencil_clear_rect_write_ptr->rect.offset.x =
                int32_t(stencil_clear_rectangle.x_pixels * draw_resolution_scale_x());
            stencil_clear_rect_write_ptr->rect.offset.y =
                int32_t(stencil_clear_rectangle.y_pixels * draw_resolution_scale_y());
            stencil_clear_rect_write_ptr->rect.extent.width =
                stencil_clear_rectangle.width_pixels * draw_resolution_scale_x();
            stencil_clear_rect_write_ptr->rect.extent.height =
                stencil_clear_rectangle.height_pixels * draw_resolution_scale_y();
            stencil_clear_rect_write_ptr->baseArrayLayer = 0;
            stencil_clear_rect_write_ptr->layerCount = 1;
            ++stencil_clear_rect_write_ptr;
          }
        }
      }

      // Prefer power of two viewports for exact division by simply biasing the
      // exponent.
      VkViewport transfer_viewport;
      transfer_viewport.x = 0.0f;
      transfer_viewport.y = 0.0f;
      transfer_viewport.width =
          float(std::min(rex::next_pow2(transfer_framebuffer->host_extent.width),
                         vulkan_device->properties().maxViewportDimensions[0]));
      transfer_viewport.height =
          float(std::min(rex::next_pow2(transfer_framebuffer->host_extent.height),
                         vulkan_device->properties().maxViewportDimensions[1]));
      transfer_viewport.minDepth = 0.0f;
      transfer_viewport.maxDepth = 1.0f;
      command_processor_.SetViewport(transfer_viewport);
      float pixels_to_ndc_x = (2.0f / transfer_viewport.width) * float(draw_resolution_scale_x());
      float pixels_to_ndc_y = (2.0f / transfer_viewport.height) * float(draw_resolution_scale_y());
      VkRect2D transfer_scissor;
      transfer_scissor.offset.x = 0;
      transfer_scissor.offset.y = 0;
      transfer_scissor.extent = transfer_framebuffer->host_extent;
      command_processor_.SetScissor(transfer_scissor);

      for (auto it = current_transfer_invocations_.cbegin();
           it != current_transfer_invocations_.cend(); ++it) {
        const TransferInvocation& transfer_invocation_first = *it;
        // Will be merging transfers from the same source into one mesh.
        auto it_merged_first = it, it_merged_last = it;
        uint32_t transfer_rectangle_count = transfer_invocation_first.transfer.GetRectangles(
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
        assert_not_zero(transfer_rectangle_count);
        // Skip the merged transfers in the subsequent iterations.
        it = it_merged_last;

        assert_not_null(it->transfer.source);
        auto& source_vulkan_rt = *static_cast<VulkanRenderTarget*>(it->transfer.source);
        auto host_depth_source_vulkan_rt =
            static_cast<VulkanRenderTarget*>(it->transfer.host_depth_source);
        TransferShaderKey transfer_shader_key = it->shader_key;
        const TransferModeInfo& transfer_mode_info =
            kTransferModes[size_t(transfer_shader_key.mode)];
        TransferPipelineLayoutIndex transfer_pipeline_layout_index =
            transfer_mode_info.pipeline_layout;
        const TransferPipelineLayoutInfo& transfer_pipeline_layout_info =
            kTransferPipelineLayoutInfos[size_t(transfer_pipeline_layout_index)];
        uint32_t transfer_sample_pipeline_count = vulkan_device->properties().sampleRateShading
                                                      ? 1
                                                      : uint32_t(1)
                                                            << uint32_t(dest_rt_key.msaa_samples);
        bool transfer_is_stencil_bit = (transfer_pipeline_layout_info.used_push_constant_dwords &
                                        kTransferUsedPushConstantDwordStencilMaskBit) != 0;

        uint32_t transfer_vertex_count = 6 * transfer_rectangle_count;
        VkBuffer transfer_vertex_buffer;
        VkDeviceSize transfer_vertex_buffer_offset;
        float* transfer_rectangle_write_ptr =
            reinterpret_cast<float*>(transfer_vertex_buffer_pool_->Request(
                current_submission, sizeof(float) * 2 * transfer_vertex_count, sizeof(float),
                transfer_vertex_buffer, transfer_vertex_buffer_offset));
        if (!transfer_rectangle_write_ptr) {
          continue;
        }
        for (auto it_merged = it_merged_first; it_merged <= it_merged_last; ++it_merged) {
          Transfer::Rectangle transfer_invocation_rectangles[Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_invocation_rectangle_count = it_merged->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_invocation_rectangles, resolve_clear_rectangle);
          assert_not_zero(transfer_invocation_rectangle_count);
          for (uint32_t j = 0; j < transfer_invocation_rectangle_count; ++j) {
            const Transfer::Rectangle& transfer_rectangle = transfer_invocation_rectangles[j];
            float transfer_rectangle_x0 = -1.0f + transfer_rectangle.x_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y0 = -1.0f + transfer_rectangle.y_pixels * pixels_to_ndc_y;
            float transfer_rectangle_x1 =
                transfer_rectangle_x0 + transfer_rectangle.width_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y1 =
                transfer_rectangle_y0 + transfer_rectangle.height_pixels * pixels_to_ndc_y;
            // O-*
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            // *-*
            // |/
            // O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            // *-O
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   O
            //  /|
            // *-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   *
            //  /|
            // O-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            //   *
            //  /|
            // *-O
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

        // Invalidate outdated bindings.
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
          assert_not_null(host_depth_source_vulkan_rt);
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
          assert_not_null(host_depth_source_vulkan_rt);
          RenderTargetKey host_depth_source_rt_key = host_depth_source_vulkan_rt->key();
          TransferAddressConstant host_depth_address_constant;
          host_depth_address_constant.dest_pitch = dest_pitch_tiles;
          host_depth_address_constant.source_pitch = host_depth_source_rt_key.GetPitchTiles();
          host_depth_address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) - int32_t(host_depth_source_rt_key.base_tiles);
          if (last_host_depth_address_constant != host_depth_address_constant) {
            last_host_depth_address_constant = host_depth_address_constant;
            transfer_push_constants_set &= ~kTransferUsedPushConstantDwordHostDepthAddressBit;
          }
        }
        if (transfer_pipeline_layout_info.used_push_constant_dwords &
            kTransferUsedPushConstantDwordAddressBit) {
          RenderTargetKey source_rt_key = source_vulkan_rt.key();
          TransferAddressConstant address_constant;
          address_constant.dest_pitch = dest_pitch_tiles;
          address_constant.source_pitch = source_rt_key.GetPitchTiles();
          address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) - int32_t(source_rt_key.base_tiles);
          if (last_address_constant != address_constant) {
            last_address_constant = address_constant;
            transfer_push_constants_set &= ~kTransferUsedPushConstantDwordAddressBit;
          }
        }

        // Apply the new bindings.
        // TODO(Triang3l): Merge binding updates into spans.
        VkPipelineLayout transfer_pipeline_layout =
            transfer_pipeline_layouts_[size_t(transfer_pipeline_layout_index)];
        uint32_t transfer_descriptor_sets_unbound =
            transfer_pipeline_layout_info.used_descriptor_sets & ~transfer_descriptor_sets_bound;
        if (transfer_descriptor_sets_unbound & kTransferUsedDescriptorSetHostDepthBufferBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                             (kTransferUsedDescriptorSetHostDepthBufferBit - 1)),
              1, &edram_storage_buffer_descriptor_set_, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetHostDepthBufferBit;
        }
        if (transfer_descriptor_sets_unbound &
            kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                             (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)),
              1, &last_descriptor_set_host_depth_stencil_textures, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetHostDepthStencilTexturesBit;
        }
        if (transfer_descriptor_sets_unbound & kTransferUsedDescriptorSetDepthStencilTexturesBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                             (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1)),
              1, &last_descriptor_set_depth_stencil_textures, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetDepthStencilTexturesBit;
        }
        if (transfer_descriptor_sets_unbound & kTransferUsedDescriptorSetColorTextureBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              rex::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                             (kTransferUsedDescriptorSetColorTextureBit - 1)),
              1, &last_descriptor_set_color_texture, 0, nullptr);
          transfer_descriptor_sets_bound |= kTransferUsedDescriptorSetColorTextureBit;
        }
        uint32_t transfer_push_constants_unset =
            transfer_pipeline_layout_info.used_push_constant_dwords & ~transfer_push_constants_set;
        if (transfer_push_constants_unset & kTransferUsedPushConstantDwordHostDepthAddressBit) {
          command_buffer.CmdVkPushConstants(
              transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
              sizeof(uint32_t) *
                  rex::bit_count(transfer_pipeline_layout_info.used_push_constant_dwords &
                                 (kTransferUsedPushConstantDwordHostDepthAddressBit - 1)),
              sizeof(uint32_t), &last_host_depth_address_constant);
          transfer_push_constants_set |= kTransferUsedPushConstantDwordHostDepthAddressBit;
        }
        if (transfer_push_constants_unset & kTransferUsedPushConstantDwordAddressBit) {
          command_buffer.CmdVkPushConstants(
              transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
              sizeof(uint32_t) *
                  rex::bit_count(transfer_pipeline_layout_info.used_push_constant_dwords &
                                 (kTransferUsedPushConstantDwordAddressBit - 1)),
              sizeof(uint32_t), &last_address_constant);
          transfer_push_constants_set |= kTransferUsedPushConstantDwordAddressBit;
        }

        for (uint32_t j = 0; j < transfer_sample_pipeline_count; ++j) {
          if (j) {
            command_processor_.BindExternalGraphicsPipeline(transfer_pipelines[j]);
          }
          for (uint32_t k = 0; k < uint32_t(transfer_is_stencil_bit ? 8 : 1); ++k) {
            if (transfer_is_stencil_bit) {
              uint32_t transfer_stencil_bit = uint32_t(1) << k;
              command_buffer.CmdVkPushConstants(
                  transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                  sizeof(uint32_t) *
                      rex::bit_count(transfer_pipeline_layout_info.used_push_constant_dwords &
                                     (kTransferUsedPushConstantDwordStencilMaskBit - 1)),
                  sizeof(uint32_t), &transfer_stencil_bit);
              command_buffer.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                      transfer_stencil_bit);
            }
            command_buffer.CmdVkDraw(transfer_vertex_count, 1, 0, 0);
          }
        }
      }
    }

    // Perform the clear.
    if (resolve_clear_needed) {
      command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
          transfer_render_pass, transfer_framebuffer, transfer_dest_view, dest_rt_key.is_depth);
      VkClearAttachment resolve_clear_attachment;
      resolve_clear_attachment.colorAttachment = 0;
      std::memset(&resolve_clear_attachment.clearValue, 0,
                  sizeof(resolve_clear_attachment.clearValue));
      uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_rt_key.is_depth) {
        resolve_clear_attachment.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        uint32_t depth_guest_clear_value = (uint32_t(clear_value) >> 8) & 0xFFFFFF;
        switch (dest_rt_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            resolve_clear_attachment.clearValue.depthStencil.depth =
                xenos::UNorm24To32(depth_guest_clear_value);
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            // Taking [0, 2) -> [0, 1) remapping into account.
            resolve_clear_attachment.clearValue.depthStencil.depth =
                xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            break;
        }
        resolve_clear_attachment.clearValue.depthStencil.stencil = uint32_t(clear_value) & 0xFF;
      } else {
        resolve_clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bool dest_color_transfer_is_uint = false;
        GetColorOwnershipTransferVulkanFormat(
            dest_rt_key.GetColorFormat(), dest_rt_key.msaa_samples, &dest_color_transfer_is_uint);
        switch (dest_rt_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8:
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            for (uint32_t j = 0; j < 4; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            if (dest_rt_key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                gamma_render_target_as_unorm16_) {
              // 8_8_8_8_GAMMA is represented by linear stored in
              // R16G16B16A16_UNORM.
              for (uint32_t j = 0; j < 3; ++j) {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    xenos::PWLGammaToLinear(resolve_clear_attachment.clearValue.color.float32[j]);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            resolve_clear_attachment.clearValue.color.float32[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            resolve_clear_attachment.clearValue.color.float32[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            // Using uint for transfers and clears of both. Disregarding the
            // current -32...32 vs. -1...1 settings for consistency with color
            // clear via depth aliasing.
            for (uint32_t j = 0; j < 2; ++j) {
              uint16_t component = uint16_t((clear_value >> (j * 16)) & 0xFFFF);
              if (dest_color_transfer_is_uint) {
                resolve_clear_attachment.clearValue.color.uint32[j] = component;
              } else if (IsColor16FormatFloatLike(dest_rt_key.GetColorFormat())) {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    rex::xenos_half_to_float(component);
              } else {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    std::max(float(int16_t(component)) * (1.0f / 32767.0f), -1.0f);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            // Using uint for transfers and clears of both. Disregarding the
            // current -32...32 vs. -1...1 settings for consistency with color
            // clear via depth aliasing.
            for (uint32_t j = 0; j < 4; ++j) {
              uint16_t component = uint16_t((clear_value >> (j * 16)) & 0xFFFF);
              if (dest_color_transfer_is_uint) {
                resolve_clear_attachment.clearValue.color.uint32[j] = component;
              } else if (IsColor16FormatFloatLike(dest_rt_key.GetColorFormat())) {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    rex::xenos_half_to_float(component);
              } else {
                resolve_clear_attachment.clearValue.color.float32[j] =
                    std::max(float(int16_t(component)) * (1.0f / 32767.0f), -1.0f);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            uint32_t component = uint32_t(clear_value);
            if (dest_color_transfer_is_uint) {
              // Using uint for proper denormal and NaN handling.
              resolve_clear_attachment.clearValue.color.uint32[0] = component;
            } else {
              std::memcpy(&resolve_clear_attachment.clearValue.color.float32[0], &component,
                          sizeof(component));
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            uint32_t component_0 = uint32_t(clear_value);
            uint32_t component_1 = uint32_t(clear_value >> 32);
            if (dest_color_transfer_is_uint) {
              // Using uint for proper denormal and NaN handling.
              resolve_clear_attachment.clearValue.color.uint32[0] = component_0;
              resolve_clear_attachment.clearValue.color.uint32[1] = component_1;
            } else {
              std::memcpy(&resolve_clear_attachment.clearValue.color.float32[0], &component_0,
                          sizeof(component_0));
              std::memcpy(&resolve_clear_attachment.clearValue.color.float32[1], &component_1,
                          sizeof(component_1));
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

  SpirvBuilder builder(spv::Spv_1_0, (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
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

  // Bindings.
  // EDRAM buffer.
  bool format_is_64bpp =
      !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(key.GetColorFormat());
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeRuntimeArray(format_is_64bpp ? type_uint2 : type_uint));
  // Storage buffers have std430 packing, no padding to 4-component vectors.
  builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride,
                        sizeof(uint32_t) << uint32_t(format_is_64bpp));
  spv::Id type_edram = builder.makeStructType(id_vector_temp, "XeEdram");
  builder.addMemberName(type_edram, 0, "edram");
  builder.addMemberDecoration(type_edram, 0, spv::DecorationNonReadable);
  builder.addMemberDecoration(type_edram, 0, spv::DecorationOffset, 0);
  // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // BufferBlock.
  builder.addDecoration(type_edram, spv::DecorationBufferBlock);
  // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // Uniform.
  spv::Id edram_buffer =
      builder.createVariable(spv::NoPrecision, spv::StorageClassUniform, type_edram, "xe_edram");
  builder.addDecoration(edram_buffer, spv::DecorationDescriptorSet, kDumpDescriptorSetEdram);
  builder.addDecoration(edram_buffer, spv::DecorationBinding, 0);
  // Color or depth source.
  bool source_is_multisampled = key.msaa_samples != xenos::MsaaSamples::k1X;
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
  // Stencil source.
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
  // Push constants.
  id_vector_temp.clear();
  id_vector_temp.reserve(kDumpPushConstantCount);
  for (uint32_t i = 0; i < kDumpPushConstantCount; ++i) {
    id_vector_temp.push_back(type_uint);
  }
  spv::Id type_push_constants = builder.makeStructType(id_vector_temp, "XeEdramDumpPushConstants");
  builder.addMemberName(type_push_constants, kDumpPushConstantPitches, "pitches");
  builder.addMemberDecoration(type_push_constants, kDumpPushConstantPitches, spv::DecorationOffset,
                              int(sizeof(uint32_t) * kDumpPushConstantPitches));
  builder.addMemberName(type_push_constants, kDumpPushConstantOffsets, "offsets");
  builder.addMemberDecoration(type_push_constants, kDumpPushConstantOffsets, spv::DecorationOffset,
                              int(sizeof(uint32_t) * kDumpPushConstantOffsets));
  builder.addDecoration(type_push_constants, spv::DecorationBlock);
  spv::Id push_constants =
      builder.createVariable(spv::NoPrecision, spv::StorageClassPushConstant, type_push_constants,
                             "xe_edram_dump_push_constants");

  // gl_GlobalInvocationID input.
  spv::Id input_global_invocation_id = builder.createVariable(
      spv::NoPrecision, spv::StorageClassInput, type_uint3, "gl_GlobalInvocationID");
  builder.addDecoration(input_global_invocation_id, spv::DecorationBuiltIn,
                        spv::BuiltInGlobalInvocationId);

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function = builder.makeFunctionEntry(
      spv::NoPrecision, type_void, "main", main_param_types, main_precisions, &main_entry);

  // For now, as the exact addressing in 64bpp render targets relatively to
  // 32bpp is unknown, treating 64bpp tiles as storing 40x16 samples rather than
  // 80x16 for simplicity of addressing into the texture.

  // Split the destination sample index into the 32bpp tile and the
  // 32bpp-tile-relative sample index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the sample position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  spv::Id global_invocation_id = builder.createLoad(input_global_invocation_id, spv::NoPrecision);
  spv::Id rectangle_sample_x = builder.createCompositeExtract(global_invocation_id, type_uint, 0);
  uint32_t tile_width =
      (xenos::kEdramTileWidthSamples >> uint32_t(format_is_64bpp)) * draw_resolution_scale_x();
  spv::Id const_tile_width = builder.makeUintConstant(tile_width);
  spv::Id rectangle_tile_index_x =
      builder.createBinOp(spv::OpUDiv, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id tile_sample_x =
      builder.createBinOp(spv::OpUMod, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id rectangle_sample_y = builder.createCompositeExtract(global_invocation_id, type_uint, 1);
  uint32_t tile_height = xenos::kEdramTileHeightSamples * draw_resolution_scale_y();
  spv::Id const_tile_height = builder.makeUintConstant(tile_height);
  spv::Id rectangle_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, rectangle_sample_y, const_tile_height);
  spv::Id tile_sample_y =
      builder.createBinOp(spv::OpUMod, type_uint, rectangle_sample_y, const_tile_height);

  // Get the tile index in the EDRAM relative to the dump rectangle base tile.
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(kDumpPushConstantPitches));
  spv::Id pitches_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants, id_vector_temp),
      spv::NoPrecision);
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_edram_pitch_tiles_bits = builder.makeUintConstant(xenos::kEdramPitchTilesBits);
  spv::Id rectangle_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint,
                          builder.createTriOp(spv::OpBitFieldUExtract, type_uint, pitches_constant,
                                              const_uint_0, const_edram_pitch_tiles_bits),
                          rectangle_tile_index_y),
      rectangle_tile_index_x);
  // Add the base tile in the dispatch to the dispatch-local tile index, not
  // wrapping yet so in case of a wraparound, the address relative to the base
  // in the image after subtraction of the base won't be negative.
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(kDumpPushConstantOffsets));
  spv::Id offsets_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants, id_vector_temp),
      spv::NoPrecision);
  spv::Id const_edram_base_tiles_bits_plus_1 =
      builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1);
  spv::Id edram_tile_index_non_wrapped =
      builder.createBinOp(spv::OpIAdd, type_uint,
                          builder.createTriOp(spv::OpBitFieldUExtract, type_uint, offsets_constant,
                                              const_uint_0, const_edram_base_tiles_bits_plus_1),
                          rectangle_tile_index);

  // Combine the tile sample index and the tile index, wrapping the tile
  // addressing, into the EDRAM sample index.
  spv::Id edram_sample_address = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint, builder.makeUintConstant(tile_width * tile_height),
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, edram_tile_index_non_wrapped,
                              builder.makeUintConstant(xenos::kEdramTileCount - 1))),
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(spv::OpIMul, type_uint, const_tile_width, tile_sample_y),
          tile_sample_x));
  if (key.is_depth) {
    // Swap 40-sample columns in the depth buffer in the destination address to
    // get the final address of the sample in the EDRAM.
    uint32_t tile_width_half = tile_width >> 1;
    edram_sample_address = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int, edram_sample_address),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(spv::OpULessThan, builder.makeBoolType(), tile_sample_x,
                                    builder.makeUintConstant(tile_width_half)),
                builder.makeIntConstant(int32_t(tile_width_half)),
                builder.makeIntConstant(-int32_t(tile_width_half)))));
  }

  // Get the linear tile index within the source texture.
  spv::Id source_tile_index = builder.createBinOp(
      spv::OpISub, type_uint, edram_tile_index_non_wrapped,
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, offsets_constant,
                          const_edram_base_tiles_bits_plus_1,
                          builder.makeUintConstant(xenos::kEdramBaseTilesBits)));
  // Split the linear tile index in the source texture into X and Y in tiles.
  spv::Id source_pitch_tiles =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, pitches_constant,
                          const_edram_pitch_tiles_bits, const_edram_pitch_tiles_bits);
  spv::Id source_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x =
      builder.createBinOp(spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  // Combine the source tile offset and the sample index within the tile.
  spv::Id source_sample_x = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_width, source_tile_index_x),
      tile_sample_x);
  spv::Id source_sample_y = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_height, source_tile_index_y),
      tile_sample_y);
  // Get the source pixel coordinate and the sample index within the pixel.
  spv::Id source_pixel_x = source_sample_x, source_pixel_y = source_sample_y;
  spv::Id source_sample_id = spv::NoResult;
  if (source_is_multisampled) {
    spv::Id const_uint_1 = builder.makeUintConstant(1);
    source_pixel_y =
        builder.createBinOp(spv::OpShiftRightLogical, type_uint, source_sample_y, const_uint_1);
    if (key.msaa_samples >= xenos::MsaaSamples::k4X) {
      source_pixel_x =
          builder.createBinOp(spv::OpShiftRightLogical, type_uint, source_sample_x, const_uint_1);
      // 4x MSAA source texture sample index - bit 0 for horizontal, bit 1 for
      // vertical.
      source_sample_id = builder.createQuadOp(
          spv::OpBitFieldInsert, type_uint,
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_x, const_uint_1),
          source_sample_y, const_uint_1, const_uint_1);
    } else {
      // 2x MSAA source texture sample index - convert from the guest to
      // the Vulkan standard sample locations.
      source_sample_id = builder.createTriOp(
          spv::OpSelect, type_uint,
          builder.createBinOp(
              spv::OpINotEqual, builder.makeBoolType(),
              builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_y, const_uint_1),
              const_uint_0),
          builder.makeUintConstant(
              draw_util::GetD3D10SampleIndexForGuest2xMSAA(1, msaa_2x_attachments_supported_)),
          builder.makeUintConstant(
              draw_util::GetD3D10SampleIndexForGuest2xMSAA(0, msaa_2x_attachments_supported_)));
    }
  }

  // Load the source, and pack the value into one or two 32-bit integers.
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
  auto LinearToPWLGamma = [&](spv::Id linear, bool linear_pre_saturated) -> spv::Id {
    if (!linear_pre_saturated) {
      linear = builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
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
                            builder.createUnaryBuiltinCall(
                                type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                builder.createBinOp(spv::OpFMul, type_float, linear, scale)),
                            builder.makeFloatConstant(1.0f / 255.0f)),
        offset);
  };
  auto PackDumpSource16ComponentToUint = [&](spv::Id component) -> spv::Id {
    if (source_is_uint) {
      return component;
    }
    if (source_color_16_is_float) {
      id_vector_temp.clear();
      id_vector_temp.push_back(component);
      id_vector_temp.push_back(const_float_0);
      spv::Id packed_half = builder.createUnaryBuiltinCall(
          type_uint, ext_inst_glsl_std_450, GLSLstd450PackHalf2x16,
          builder.createCompositeConstruct(builder.makeVectorType(type_float, 2), id_vector_temp));
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_half, const_uint_0,
                                 const_uint_16);
    }
    spv::Id component_clamped =
        builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, component,
                                     const_float_minus_1, const_float_1);
    spv::Id component_rounded = builder.createUnaryBuiltinCall(
        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
        builder.createBinOp(spv::OpFMul, type_float, component_clamped, const_float_32767));
    spv::Id component_snorm =
        builder.createUnaryOp(spv::OpConvertFToS, type_int, component_rounded);
    return builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                               builder.createUnaryOp(spv::OpBitcast, type_uint, component_snorm),
                               const_uint_0, const_uint_16);
  };
  auto PackDumpSource16PairToUint32 = [&](spv::Id component_0, spv::Id component_1) -> spv::Id {
    return builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, PackDumpSource16ComponentToUint(component_0),
        PackDumpSource16ComponentToUint(component_1), const_uint_16, const_uint_16);
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
        // Round to the nearest even integer. This seems to be the correct
        // conversion, adding +0.5 and rounding towards zero results in red
        // instead of black in the 4D5307E6 clear shader.
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createUnaryBuiltinCall(
                type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                builder.createBinOp(spv::OpFMul, type_float, source_depth32,
                                    builder.makeFloatConstant(float(0xFFFFFF)))));
      } break;
      case xenos::DepthRenderTargetFormat::kD24FS8: {
        packed[0] = SpirvShaderTranslator::PreClampedDepthTo20e4(
            builder, source_depth32, depth_float24_round(), true, ext_inst_glsl_std_450);
      } break;
    }
    packed[0] = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil, packed[0],
                                     builder.makeUintConstant(8), builder.makeUintConstant(24));
  } else {
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
        if (gamma_render_target_as_unorm16_) {
          // 8_8_8_8_GAMMA is represented by linear stored in
          // R16G16B16A16_UNORM.
          id_vector_temp.clear();
          for (uint32_t i = 0; i < 3; ++i) {
            id_vector_temp.push_back(
                LinearToPWLGamma(builder.createCompositeExtract(source_vec4, type_float, i), true));
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
        for (uint32_t i = 1; i < 4; ++i) {
          spv::Id color_i = builder.createCompositeExtract(source_vec4, type_float, i);
          if (key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
              gamma_render_target_as_unorm16_ && i < 3) {
            color_i = id_vector_temp[i];
          }
          packed[0] = builder.createQuadOp(
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
        for (uint32_t i = 1; i < 4; ++i) {
          packed[0] = builder.createQuadOp(
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
        // Float16 has a wider range for both color and alpha, also NaNs - clamp
        // and convert.
        packed[0] = SpirvShaderTranslator::UnclampedFloat32To7e3(
            builder, builder.createCompositeExtract(source_vec4, type_float, 0),
            ext_inst_glsl_std_450);
        spv::Id width_rgb = builder.makeUintConstant(10);
        for (uint32_t i = 1; i < 3; ++i) {
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              SpirvShaderTranslator::UnclampedFloat32To7e3(
                  builder, builder.createCompositeExtract(source_vec4, type_float, i),
                  ext_inst_glsl_std_450),
              builder.makeUintConstant(10 * i), width_rgb);
        }
        // Saturate and convert the alpha.
        spv::Id alpha_saturated = builder.createTriBuiltinCall(
            type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
            builder.createCompositeExtract(source_vec4, type_float, 3),
            builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
        packed[0] = builder.createQuadOp(
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
        for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
          packed[i] = PackDumpSource16PairToUint32(
              builder.createCompositeExtract(source_vec4, source_component_type, 2 * i),
              builder.createCompositeExtract(source_vec4, source_component_type, 2 * i + 1));
        }
      } break;
      // Float32 is transferred as uint32 to preserve NaN encodings. However,
      // multisampled sampled image support is optional in Vulkan.
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
        for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
          spv::Id& packed_ref = packed[i];
          packed_ref = builder.createCompositeExtract(source_vec4, source_component_type, i);
          if (!source_is_uint) {
            packed_ref = builder.createUnaryOp(spv::OpBitcast, type_uint, packed_ref);
          }
        }
      } break;
    }
  }

  // Write the packed value to the EDRAM buffer.
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
  // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // Uniform.
  builder.createStore(store_value, builder.createAccessChain(spv::StorageClassUniform, edram_buffer,
                                                             id_vector_temp));

  // End the main function and make it the entry point.
  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeLocalSize, kDumpSamplesPerGroupX,
                           kDumpSamplesPerGroupY, 1);
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelGLCompute, main_function, "main");
  // Bindings only need to be added to the entry point's interface starting with
  // SPIR-V 1.4 - emitting 1.0 here, so only inputs / outputs.
  entry_point->addIdOperand(input_global_invocation_id);

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the pipeline, and store the handle even if creation fails not to try
  // to create it again later.
  VkPipeline pipeline = ui::vulkan::util::CreateComputePipeline(
      command_processor_.GetVulkanDevice(),
      key.is_depth ? dump_pipeline_layout_depth_ : dump_pipeline_layout_color_,
      reinterpret_cast<const uint32_t*>(shader_code.data()), sizeof(uint32_t) * shader_code.size());
  if (pipeline == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "VulkanRenderTargetCache: Failed to create a render target dumping "
        "pipeline for {}-sample render targets with format {}",
        UINT32_C(1) << uint32_t(key.msaa_samples),
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
  // Until dedicated direct host RT -> shared memory shaders are added, reuse
  // the resolve copy pipelines to keep all resolve shader modes wired for the
  // direct preflight path.
  size_t copy_shader_index = size_t(key.copy_shader);
  if (copy_shader_index < size_t(draw_util::ResolveCopyShaderIndex::kCount)) {
    pipeline = resolve_copy_pipelines_[copy_shader_index];
  }
  direct_resolve_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool VulkanRenderTargetCache::TryResolveCopyDirectly(const draw_util::ResolveInfo& resolve_info,
                                                     draw_util::ResolveCopyShaderIndex copy_shader,
                                                     bool draw_resolution_scaled) {
  ++direct_resolve_attempt_count_;
  (void)copy_shader;
  (void)draw_resolution_scaled;
  if (direct_resolve_pipeline_layout_color_ == VK_NULL_HANDLE ||
      direct_resolve_pipeline_layout_depth_ == VK_NULL_HANDLE) {
    return false;
  }

  uint32_t dump_base;
  uint32_t dump_row_length_used;
  uint32_t dump_rows;
  uint32_t dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
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
    dump_pipeline_key.msaa_samples = render_target->key().msaa_samples;
    dump_pipeline_key.resource_format = render_target->key().resource_format;
    dump_pipeline_key.is_depth = render_target->key().is_depth;
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

  // Dedicated direct resolve dispatches are staged behind the same preflight;
  // keep using the existing dump path until source-image direct shaders land.
  return DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch);
}

bool VulkanRenderTargetCache::DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,
                                                uint32_t dump_rows, uint32_t dump_pitch) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_);
  if (dump_rectangles_.empty()) {
    return true;
  }

  // Clear previously set temporary indices.
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    static_cast<VulkanRenderTarget*>(rectangle.render_target)->SetTemporarySortIndex(UINT32_MAX);
  }
  // Gather all needed barriers and info needed to sort the invocations.
  UseEdramBuffer(EdramBufferUsage::kComputeWrite);
  dump_invocations_.clear();
  dump_invocations_.reserve(dump_rectangles_.size());
  uint32_t rt_sort_index = 0;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.key();
    command_processor_.PushImageMemoryBarrier(
        vulkan_rt.image(),
        ui::vulkan::util::InitializeSubresourceRange(
            rt_key.is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                            : VK_IMAGE_ASPECT_COLOR_BIT),
        vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        vulkan_rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT, vulkan_rt.current_layout(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (vulkan_rt.temporary_sort_index() == UINT32_MAX) {
      vulkan_rt.SetTemporarySortIndex(rt_sort_index++);
    }
    DumpPipelineKey pipeline_key;
    pipeline_key.msaa_samples = rt_key.msaa_samples;
    pipeline_key.resource_format = rt_key.resource_format;
    pipeline_key.is_depth = rt_key.is_depth;
    dump_invocations_.emplace_back(rectangle, pipeline_key);
  }

  // Sort the invocations to reduce context and binding switches.
  std::sort(dump_invocations_.begin(), dump_invocations_.end());

  // Dump the render targets.
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  bool edram_buffer_bound = false;
  VkDescriptorSet last_source_descriptor_set = VK_NULL_HANDLE;
  DumpPitches last_pitches;
  DumpOffsets last_offsets;
  bool pitches_bound = false, offsets_bound = false;
  bool all_pipelines_available = true;
  for (const DumpInvocation& invocation : dump_invocations_) {
    const ResolveCopyDumpRectangle& rectangle = invocation.rectangle;
    auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.key();
    DumpPipelineKey pipeline_key = invocation.pipeline_key;
    VkPipeline pipeline = GetDumpPipeline(pipeline_key);
    if (!pipeline) {
      all_pipelines_available = false;
      continue;
    }
    command_processor_.BindExternalComputePipeline(pipeline);

    VkPipelineLayout pipeline_layout =
        rt_key.is_depth ? dump_pipeline_layout_depth_ : dump_pipeline_layout_color_;

    // Only need to bind the EDRAM buffer once (relying on pipeline layout
    // compatibility).
    if (!edram_buffer_bound) {
      edram_buffer_bound = true;
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                             kDumpDescriptorSetEdram, 1,
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
                                        sizeof(uint32_t) * kDumpPushConstantPitches,
                                        sizeof(last_pitches), &last_pitches);
    }

    DumpOffsets offsets;
    offsets.source_base_tiles = rt_key.base_tiles;
    // The second tile of a merged frame: its rows are in the render target
    // this many tiles below the EDRAM tiles being dumped.
    const uint32_t source_shift_tiles =
        rt_key == dump_source_shift_key_ ? dump_source_shift_tiles_ : 0;
    ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count = rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      offsets.dispatch_first_tile = dump_base + dispatch.offset;
      offsets.source_base_tiles = rt_key.base_tiles;
      if (source_shift_tiles) {
        // The shader writes EDRAM tile (first + i) & 2047 and reads source
        // tile first + i - base, with first in 12 bits and base in 11. Find
        // an equivalent first (the same modulo the EDRAM size) and a base
        // in range that read source_shift_tiles further.
        const int32_t first = int32_t(dump_base + dispatch.offset);
        const int32_t wanted = first - int32_t(rt_key.base_tiles) + int32_t(source_shift_tiles);
        bool found = false;
        for (int32_t k = 0; k < 2 && !found; ++k) {
          const int32_t new_first =
              (first & int32_t(xenos::kEdramTileCount - 1)) + k * int32_t(xenos::kEdramTileCount);
          const int32_t new_base = new_first - wanted;
          if (new_base >= 0 && new_base < int32_t(xenos::kEdramTileCount)) {
            offsets.dispatch_first_tile = uint32_t(new_first);
            offsets.source_base_tiles = uint32_t(new_base);
            found = true;
          }
        }
        if (!found) {
          REXGPU_WARN("Merged tile resolve: can't address the second tile's rows (shift {})",
                      source_shift_tiles);
        }
      }
      if (last_offsets != offsets) {
        last_offsets = offsets;
        offsets_bound = false;
      }
      if (!offsets_bound) {
        offsets_bound = true;
        command_buffer.CmdVkPushConstants(pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                          sizeof(uint32_t) * kDumpPushConstantOffsets,
                                          sizeof(last_offsets), &last_offsets);
      }
      command_processor_.SubmitBarriers(true);
      command_buffer.CmdVkDispatch(
          (draw_resolution_scale_x() *
               (xenos::kEdramTileWidthSamples >> uint32_t(rt_key.Is64bpp())) *
               dispatch.width_tiles +
           (kDumpSamplesPerGroupX - 1)) /
              kDumpSamplesPerGroupX,
          (draw_resolution_scale_y() * xenos::kEdramTileHeightSamples * dispatch.height_tiles +
           (kDumpSamplesPerGroupY - 1)) /
              kDumpSamplesPerGroupY,
          1);
    }
    MarkEdramBufferModified();
  }
  return all_pipelines_available;
}

}  // namespace rex::graphics::vulkan
