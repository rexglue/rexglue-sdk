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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/platform.h>
#include "gpu/xenos/host/vulkan/presenter.h"
#include "gpu/xenos/host/vulkan/objects.h"

#if REX_PLATFORM_GNU_LINUX
#include <rex/gpu/surface_gnulinux.h>
#endif
#if REX_PLATFORM_WIN32
#include <rex/gpu/surface_win.h>
#endif
#if REX_PLATFORM_MAC
#include <rex/gpu/surface_mac.h>
#include <rex/move_only_function.h>
#endif

#include "gpu/xenos/vulkan/cvar_table.h"

namespace rex {
namespace gpu {
namespace vulkan {

namespace shaders {
#include "../shaders/vulkan_spirv/guest_output_bilinear_dither_ps.h"
#include "../shaders/vulkan_spirv/guest_output_bilinear_ps.h"
#include "../shaders/vulkan_spirv/guest_output_triangle_strip_rect_vs.h"
}  // namespace shaders

VulkanPresenter::PaintContext::Submission::~Submission() {
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  if (draw_command_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyCommandPool(device, draw_command_pool_, nullptr);
  }

  if (present_semaphore_ != VK_NULL_HANDLE) {
    dfn.vkDestroySemaphore(device, present_semaphore_, nullptr);
  }
  if (acquire_semaphore_ != VK_NULL_HANDLE) {
    dfn.vkDestroySemaphore(device, acquire_semaphore_, nullptr);
  }
}

bool VulkanPresenter::PaintContext::Submission::Initialize() {
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  VkSemaphoreCreateInfo semaphore_create_info;
  semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphore_create_info.pNext = nullptr;
  semaphore_create_info.flags = 0;
  if (dfn.vkCreateSemaphore(device, &semaphore_create_info, nullptr, &acquire_semaphore_) !=
      VK_SUCCESS) {
    REX_FAIL(ui, false,
             "VulkanPresenter: Failed to create a swapchain image acquisition "
             "semaphore");
  }
  if (dfn.vkCreateSemaphore(device, &semaphore_create_info, nullptr, &present_semaphore_) !=
      VK_SUCCESS) {
    REX_FAIL(ui, false,
             "VulkanPresenter: Failed to create a swapchain image presentation "
             "semaphore");
  }

  VkCommandPoolCreateInfo command_pool_create_info;
  command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  command_pool_create_info.pNext = nullptr;
  command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  command_pool_create_info.queueFamilyIndex = vulkan_device_->GetQueueFamilyGraphicsCompute();
  if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr, &draw_command_pool_) !=
      VK_SUCCESS) {
    REX_FAIL(ui, false,
             "VulkanPresenter: Failed to create a command pool for drawing to a "
             "swapchain");
  }
  VkCommandBufferAllocateInfo command_buffer_allocate_info;
  command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_buffer_allocate_info.pNext = nullptr;
  command_buffer_allocate_info.commandPool = draw_command_pool_;
  command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_buffer_allocate_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &draw_command_buffer_) !=
      VK_SUCCESS) {
    REX_FAIL(ui, false,
             "VulkanPresenter: Failed to allocate a command buffer for drawing to a "
             "swapchain");
  }

  return true;
}

VulkanPresenter::~VulkanPresenter() {
  paint_context_.DestroySwapchainAndVulkanSurface();

  ui_submission_tracker_.Shutdown();
  guest_output_image_refresher_submission_tracker_.Shutdown();

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  if (paint_context_.swapchain_render_pass != VK_NULL_HANDLE) {
    dfn.vkDestroyRenderPass(device, paint_context_.swapchain_render_pass, nullptr);
  }

  for (const PaintContext::UISetupCommandBuffer& ui_setup_command_buffer :
       paint_context_.ui_setup_command_buffers) {
    dfn.vkDestroyCommandPool(device, ui_setup_command_buffer.command_pool, nullptr);
  }

  objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                paint_context_.guest_output_descriptor_pool);
  for (PaintContext::GuestOutputPaintPipeline& guest_output_paint_pipeline :
       paint_context_.guest_output_paint_pipelines) {
    objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                  guest_output_paint_pipeline.swapchain_pipeline);
  }

  for (VkShaderModule& shader_module : guest_output_paint_fs_) {
    objects::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, shader_module);
  }
  objects::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, guest_output_paint_vs_);
  for (VkPipelineLayout& pipeline_layout : guest_output_paint_pipeline_layouts_) {
    objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, pipeline_layout);
  }
  objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                guest_output_paint_image_descriptor_set_layout_);
}

gpu::Surface::TypeFlags VulkanPresenter::GetSurfaceTypesSupportedByInstance(
    const VulkanInstance::Extensions& instance_extensions) {
  if (!instance_extensions.ext_KHR_surface) {
    return 0;
  }
  gpu::Surface::TypeFlags type_flags = 0;
#if REX_PLATFORM_GNU_LINUX
  if (instance_extensions.ext_KHR_wayland_surface) {
    type_flags |= gpu::Surface::kTypeFlagWaylandSurface;
  }
  if (instance_extensions.ext_KHR_xcb_surface) {
    type_flags |= gpu::Surface::kTypeFlagXCBWindow;
  }
#endif
#if REX_PLATFORM_WIN32
  if (instance_extensions.ext_KHR_win32_surface) {
    type_flags |= gpu::Surface::kTypeFlagWin32HWND;
  }
#endif
#if REX_PLATFORM_MAC
  if (instance_extensions.ext_EXT_metal_surface) {
    type_flags |= gpu::Surface::kTypeFlagCAMetalLayer;
  }
#endif
  return type_flags;
}

gpu::Surface::TypeFlags VulkanPresenter::GetSupportedSurfaceTypes() const {
  if (!vulkan_device_->GetExtensions().ext_KHR_swapchain) {
    return 0;
  }
  return GetSurfaceTypesSupportedByInstance(vulkan_device_->GetVulkanInstance()->GetExtensions());
}

VkCommandBuffer VulkanPresenter::AcquireUISetupCommandBufferFromUIThread() {
  if (paint_context_.ui_setup_command_buffer_current_index != SIZE_MAX) {
    return paint_context_
        .ui_setup_command_buffers[paint_context_.ui_setup_command_buffer_current_index]
        .command_buffer;
  }

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  VkCommandBufferBeginInfo command_buffer_begin_info;
  command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  command_buffer_begin_info.pNext = nullptr;
  command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  command_buffer_begin_info.pInheritanceInfo = nullptr;

  if (!paint_context_.ui_setup_command_buffers.empty()) {
    u64 submission_index_completed = ui_submission_tracker_.UpdateAndGetCompletedSubmission();
    for (size_t i = 0; i < paint_context_.ui_setup_command_buffers.size(); ++i) {
      PaintContext::UISetupCommandBuffer& ui_setup_command_buffer =
          paint_context_.ui_setup_command_buffers[i];
      if (ui_setup_command_buffer.last_usage_submission_index > submission_index_completed) {
        continue;
      }
      if (dfn.vkResetCommandPool(device, ui_setup_command_buffer.command_pool, 0) != VK_SUCCESS) {
        REX_FAIL(ui, VK_NULL_HANDLE, "VulkanPresenter: Failed to reset a UI setup command pool");
      }
      if (dfn.vkBeginCommandBuffer(ui_setup_command_buffer.command_buffer,
                                   &command_buffer_begin_info) != VK_SUCCESS) {
        REX_FAIL(ui, VK_NULL_HANDLE,
                 "VulkanPresenter: Failed to begin UI setup command buffer "
                 "recording");
      }
      paint_context_.ui_setup_command_buffer_current_index = i;
      ui_setup_command_buffer.last_usage_submission_index =
          ui_submission_tracker_.GetCurrentSubmission();
      return ui_setup_command_buffer.command_buffer;
    }
  }

  VkCommandPoolCreateInfo command_pool_create_info;
  command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  command_pool_create_info.pNext = nullptr;
  command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  command_pool_create_info.queueFamilyIndex = vulkan_device_->GetQueueFamilyGraphicsCompute();
  VkCommandPool new_command_pool;
  if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr, &new_command_pool) !=
      VK_SUCCESS) {
    REX_FAIL(ui, VK_NULL_HANDLE, "VulkanPresenter: Failed to create a UI setup command pool");
  }
  VkCommandBufferAllocateInfo command_buffer_allocate_info;
  command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_buffer_allocate_info.pNext = nullptr;
  command_buffer_allocate_info.commandPool = new_command_pool;
  command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_buffer_allocate_info.commandBufferCount = 1;
  VkCommandBuffer new_command_buffer;
  if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &new_command_buffer) !=
      VK_SUCCESS) {
    REXLOG_ERROR(ui, "VulkanPresenter: Failed to allocate a UI setup command buffer");
    dfn.vkDestroyCommandPool(device, new_command_pool, nullptr);
    return VK_NULL_HANDLE;
  }
  if (dfn.vkBeginCommandBuffer(new_command_buffer, &command_buffer_begin_info) != VK_SUCCESS) {
    REXLOG_ERROR(ui, "VulkanPresenter: Failed to begin UI setup command buffer recording");
    dfn.vkDestroyCommandPool(device, new_command_pool, nullptr);
    return VK_NULL_HANDLE;
  }
  paint_context_.ui_setup_command_buffer_current_index =
      paint_context_.ui_setup_command_buffers.size();
  paint_context_.ui_setup_command_buffers.emplace_back(
      new_command_pool, new_command_buffer, ui_submission_tracker_.GetCurrentSubmission());
  return new_command_buffer;
}

Presenter::SurfacePaintConnectResult
VulkanPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    gpu::Surface& new_surface, u32 new_surface_width, u32 new_surface_height,
    [[maybe_unused]] bool was_paintable, bool& is_vsync_implicit_out) {
  const VulkanInstance* const vulkan_instance = vulkan_device_->GetVulkanInstance();
  const VulkanInstance::Functions& ifn = vulkan_instance->GetFunctions();
  const VkInstance instance = vulkan_instance->GetInstance();
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  VkFormat new_swapchain_format;

  if (paint_context_.vulkan_surface != VK_NULL_HANDLE) {
    VkSwapchainKHR old_swapchain = paint_context_.PrepareForSwapchainRetirement();
    bool surface_unusable;
    paint_context_.swapchain = PaintContext::CreateSwapchainForVulkanSurface(
        vulkan_device_, paint_context_.vulkan_surface, new_surface_width, new_surface_height,
        old_swapchain, paint_context_.present_queue_family, new_swapchain_format,
        paint_context_.swapchain_extent, paint_context_.swapchain_is_fifo, surface_unusable);
    if (old_swapchain != VK_NULL_HANDLE) {
      dfn.vkDestroySwapchainKHR(device, old_swapchain, nullptr);
    }
    if (paint_context_.swapchain == VK_NULL_HANDLE) {
      paint_context_.DestroySwapchainAndVulkanSurface();
    }
  }

  if (paint_context_.swapchain == VK_NULL_HANDLE) {
    REX_ASSERT(paint_context_.vulkan_surface == VK_NULL_HANDLE);
    gpu::Surface::TypeIndex surface_type = new_surface.GetType();
    if (!(GetSupportedSurfaceTypes() & (gpu::Surface::TypeFlags(1) << surface_type))) {
      REX_FAIL(ui, SurfacePaintConnectResult::kFailureSurfaceUnusable,
               "VulkanPresenter: Tried to create a Vulkan surface for an "
               "unsupported ReXGlue surface type");
    }
    VkResult vulkan_surface_create_result = VK_ERROR_UNKNOWN;
    switch (surface_type) {
#if REX_PLATFORM_GNU_LINUX
      case gpu::Surface::kTypeIndexXCBWindow: {
        auto& xcb_window_surface = static_cast<const gpu::XCBWindowSurface&>(new_surface);
        VkXcbSurfaceCreateInfoKHR surface_create_info;
        surface_create_info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        surface_create_info.pNext = nullptr;
        surface_create_info.flags = 0;
        surface_create_info.connection = xcb_window_surface.GetConnection();
        surface_create_info.window = xcb_window_surface.GetWindow();
        vulkan_surface_create_result = ifn.vkCreateXcbSurfaceKHR(
            instance, &surface_create_info, nullptr, &paint_context_.vulkan_surface);
      } break;
      case gpu::Surface::kTypeIndexWaylandSurface: {
        auto& wayland_surface = static_cast<const gpu::WaylandSurface&>(new_surface);
        VkWaylandSurfaceCreateInfoKHR surface_create_info;
        surface_create_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        surface_create_info.pNext = nullptr;
        surface_create_info.flags = 0;
        surface_create_info.display = wayland_surface.GetDisplay();
        surface_create_info.surface = wayland_surface.GetSurface();
        vulkan_surface_create_result = ifn.vkCreateWaylandSurfaceKHR(
            instance, &surface_create_info, nullptr, &paint_context_.vulkan_surface);
      } break;
#endif
#if REX_PLATFORM_WIN32
      case gpu::Surface::kTypeIndexWin32HWND: {
        auto& win32_hwnd_surface = static_cast<const gpu::Win32HWNDSurface&>(new_surface);
        VkWin32SurfaceCreateInfoKHR surface_create_info;
        surface_create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        surface_create_info.pNext = nullptr;
        surface_create_info.flags = 0;
        surface_create_info.hinstance = win32_hwnd_surface.GetHinstance();
        surface_create_info.hwnd = win32_hwnd_surface.GetHWND();
        vulkan_surface_create_result = ifn.vkCreateWin32SurfaceKHR(
            instance, &surface_create_info, nullptr, &paint_context_.vulkan_surface);
      } break;
#endif
#if REX_PLATFORM_MAC
      case gpu::Surface::kTypeIndexCAMetalLayer: {
        auto& metal_surface = static_cast<const gpu::CAMetalLayerSurface&>(new_surface);
        VkMetalSurfaceCreateInfoEXT surface_create_info;
        surface_create_info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
        surface_create_info.pNext = nullptr;
        surface_create_info.flags = 0;
        surface_create_info.pLayer = metal_surface.GetLayer();
        vulkan_surface_create_result = ifn.vkCreateMetalSurfaceEXT(
            instance, &surface_create_info, nullptr, &paint_context_.vulkan_surface);
      } break;
#endif
      default:
        REX_UNHANDLED_CASE(surface_type);
        REX_FAIL(ui, SurfacePaintConnectResult::kFailureSurfaceUnusable,
                 "VulkanPresenter: Tried to create a Vulkan surface for an "
                 "unknown ReXGlue surface type");
    }
    if (vulkan_surface_create_result != VK_SUCCESS) {
      REX_FAIL(ui, SurfacePaintConnectResult::kFailure,
               "VulkanPresenter: Failed to create a Vulkan surface");
    }
    bool surface_unusable;
    paint_context_.swapchain = PaintContext::CreateSwapchainForVulkanSurface(
        vulkan_device_, paint_context_.vulkan_surface, new_surface_width, new_surface_height,
        VK_NULL_HANDLE, paint_context_.present_queue_family, new_swapchain_format,
        paint_context_.swapchain_extent, paint_context_.swapchain_is_fifo, surface_unusable);
    if (paint_context_.swapchain == VK_NULL_HANDLE) {
      ifn.vkDestroySurfaceKHR(instance, paint_context_.vulkan_surface, nullptr);
      paint_context_.vulkan_surface = VK_NULL_HANDLE;
      return surface_unusable ? SurfacePaintConnectResult::kFailureSurfaceUnusable
                              : SurfacePaintConnectResult::kFailure;
    }
  }

  // On failure from here, call paint_context_.DestroySwapchainAndVulkanSurface before returning.
  if (paint_context_.swapchain_render_pass_format != new_swapchain_format) {
    objects::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                                  paint_context_.swapchain_render_pass);
    paint_context_.swapchain_render_pass_format = new_swapchain_format;
  }
  if (paint_context_.swapchain_render_pass == VK_NULL_HANDLE) {
    VkAttachmentDescription render_pass_attachment;
    render_pass_attachment.flags = 0;
    render_pass_attachment.format = new_swapchain_format;
    render_pass_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    render_pass_attachment.loadOp = REXCVAR_GET(::rex::gpu::cvars::render_pass_clear)
                                        ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                        : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    render_pass_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    render_pass_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    render_pass_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    render_pass_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    render_pass_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference render_pass_color_attachment;
    render_pass_color_attachment.attachment = 0;
    render_pass_color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription render_pass_subpass = {};
    render_pass_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    render_pass_subpass.colorAttachmentCount = 1;
    render_pass_subpass.pColorAttachments = &render_pass_color_attachment;
    VkSubpassDependency render_pass_dependencies[2];
    render_pass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    render_pass_dependencies[0].dstSubpass = 0;
    // srcStageMask is the semaphore wait stage.
    render_pass_dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    render_pass_dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    render_pass_dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    // UI drawing may blend at any moment, so the color attachment read access is included.
    render_pass_dependencies[0].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    render_pass_dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    render_pass_dependencies[1].srcSubpass = 0;
    render_pass_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    render_pass_dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    // Semaphores are signaled at VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT.
    render_pass_dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    render_pass_dependencies[1].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    render_pass_dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    render_pass_dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    VkRenderPassCreateInfo render_pass_create_info;
    render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_create_info.pNext = nullptr;
    render_pass_create_info.flags = 0;
    render_pass_create_info.attachmentCount = 1;
    render_pass_create_info.pAttachments = &render_pass_attachment;
    render_pass_create_info.subpassCount = 1;
    render_pass_create_info.pSubpasses = &render_pass_subpass;
    render_pass_create_info.dependencyCount =
        static_cast<u32>(rex::CountOf(render_pass_dependencies));
    render_pass_create_info.pDependencies = render_pass_dependencies;
    VkRenderPass new_render_pass;
    if (dfn.vkCreateRenderPass(device, &render_pass_create_info, nullptr, &new_render_pass) !=
        VK_SUCCESS) {
      REXLOG_ERROR(ui,
                   "VulkanPresenter: Failed to create the render pass for drawing to "
                   "swapchain images");
      paint_context_.DestroySwapchainAndVulkanSurface();
      return SurfacePaintConnectResult::kFailure;
    }
    paint_context_.swapchain_render_pass = new_render_pass;
    paint_context_.swapchain_render_pass_format = new_swapchain_format;
    paint_context_.swapchain_render_pass_clear_load_op =
        render_pass_attachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
  }

  paint_context_.swapchain_images.clear();
  VkResult swapchain_images_get_result;
  for (;;) {
    u32 swapchain_image_count = static_cast<u32>(paint_context_.swapchain_images.size());
    bool swapchain_images_were_empty = !swapchain_image_count;
    swapchain_images_get_result = dfn.vkGetSwapchainImagesKHR(
        device, paint_context_.swapchain, &swapchain_image_count,
        swapchain_images_were_empty ? nullptr : paint_context_.swapchain_images.data());
    // The first call with a count of 0 returns SUCCESS, not INCOMPLETE.
    if (swapchain_images_get_result == VK_SUCCESS || swapchain_images_get_result == VK_INCOMPLETE) {
      paint_context_.swapchain_images.resize(swapchain_image_count);
      if (swapchain_images_get_result == VK_SUCCESS &&
          (!swapchain_images_were_empty || !swapchain_image_count)) {
        break;
      }
    } else {
      break;
    }
  }
  if (swapchain_images_get_result != VK_SUCCESS) {
    REXLOG_ERROR(ui, "VulkanPresenter: Failed to get swapchain images");
    paint_context_.DestroySwapchainAndVulkanSurface();
    return SurfacePaintConnectResult::kFailure;
  }

  REX_ASSERT(paint_context_.swapchain_framebuffers.empty());
  paint_context_.swapchain_framebuffers.reserve(paint_context_.swapchain_images.size());
  VkImageViewCreateInfo image_view_create_info;
  image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_create_info.pNext = nullptr;
  image_view_create_info.flags = 0;
  image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image_view_create_info.format = new_swapchain_format;
  image_view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  image_view_create_info.subresourceRange.baseMipLevel = 0;
  image_view_create_info.subresourceRange.levelCount = 1;
  image_view_create_info.subresourceRange.baseArrayLayer = 0;
  image_view_create_info.subresourceRange.layerCount = 1;
  VkImageView image_view;
  VkFramebufferCreateInfo framebuffer_create_info;
  framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_create_info.pNext = nullptr;
  framebuffer_create_info.flags = 0;
  framebuffer_create_info.renderPass = paint_context_.swapchain_render_pass;
  framebuffer_create_info.attachmentCount = 1;
  framebuffer_create_info.pAttachments = &image_view;
  framebuffer_create_info.width = paint_context_.swapchain_extent.width;
  framebuffer_create_info.height = paint_context_.swapchain_extent.height;
  framebuffer_create_info.layers = 1;
  for (VkImage image : paint_context_.swapchain_images) {
    image_view_create_info.image = image;
    if (dfn.vkCreateImageView(device, &image_view_create_info, nullptr, &image_view) !=
        VK_SUCCESS) {
      REXLOG_ERROR(ui, "VulkanPresenter: Failed to create a swapchain image view");
      paint_context_.DestroySwapchainAndVulkanSurface();
      return SurfacePaintConnectResult::kFailure;
    }
    VkFramebuffer framebuffer;
    if (dfn.vkCreateFramebuffer(device, &framebuffer_create_info, nullptr, &framebuffer) !=
        VK_SUCCESS) {
      REXLOG_ERROR(ui, "VulkanPresenter: Failed to create a swapchain framebuffer");
      dfn.vkDestroyImageView(device, image_view, nullptr);
      paint_context_.DestroySwapchainAndVulkanSurface();
      return SurfacePaintConnectResult::kFailure;
    }
    paint_context_.swapchain_framebuffers.emplace_back(image_view, framebuffer);
  }

  is_vsync_implicit_out = paint_context_.swapchain_is_fifo;
  return SurfacePaintConnectResult::kSuccess;
}

void VulkanPresenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  paint_context_.DestroySwapchainAndVulkanSurface();
}

bool VulkanPresenter::RefreshGuestOutputImpl(
    u32 mailbox_index, u32 frontbuffer_width, u32 frontbuffer_height,
    rex::MoveOnlyFunction<bool(GuestOutputRefreshContext& context)> refresher,
    bool& is_8bpc_out_ref) {
  REX_ASSERT(frontbuffer_width != 0);
  REX_ASSERT(frontbuffer_height != 0);
  VkExtent2D max_framebuffer_extent =
      objects::GetMax2DFramebufferExtent(vulkan_device_->GetProperties());
  if (frontbuffer_width > max_framebuffer_extent.width ||
      frontbuffer_height > max_framebuffer_extent.height) {
    // Guest output writes do not rescale, and a guest texture above the maximum cannot load.
    return false;
  }

  GuestOutputImageInstance& image_instance = guest_output_images_[mailbox_index];
  if (image_instance.image && (image_instance.image->GetExtent().width != frontbuffer_width ||
                               image_instance.image->GetExtent().height != frontbuffer_height)) {
    guest_output_image_refresher_submission_tracker_.AwaitSubmissionCompletion(
        image_instance.last_refresher_submission);
    image_instance.image.reset();
  }
  if (!image_instance.image) {
    std::unique_ptr<GuestOutputImage> new_image =
        GuestOutputImage::Create(vulkan_device_, frontbuffer_width, frontbuffer_height);
    if (!new_image) {
      return false;
    }
    image_instance.SetToNewImage(std::move(new_image), guest_output_image_next_version_++);
  }

  VulkanGuestOutputRefreshContext context(is_8bpc_out_ref, image_instance.image->GetImage(),
                                          image_instance.image->GetView(), image_instance.version,
                                          image_instance.ever_successfully_refreshed);
  bool refresher_succeeded = refresher(context);
  if (refresher_succeeded) {
    image_instance.ever_successfully_refreshed = true;
  }
  // Even on failure the refresher may have submitted commands using the image, so signal anyway.
  image_instance.last_refresher_submission =
      guest_output_image_refresher_submission_tracker_.GetCurrentSubmission();
  // The fence is signaled here, as a vkQueueSubmit fence signal covers all earlier commands.
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  {
    VulkanSubmissionTracker::FenceAcquisition fence_acqusition(
        guest_output_image_refresher_submission_tracker_.AcquireFenceToAdvanceSubmission());
    const VulkanDevice::Queue::Acquisition queue_acquisition =
        vulkan_device_->AcquireQueue(vulkan_device_->GetQueueFamilyGraphicsCompute(), 0);
    if (dfn.vkQueueSubmit(queue_acquisition.GetQueue(), 0, nullptr, fence_acqusition.GetFence()) !=
        VK_SUCCESS) {
      fence_acqusition.SubmissionSucceededSignalFailed();
    }
  }

  return refresher_succeeded;
}

VkSwapchainKHR VulkanPresenter::PaintContext::CreateSwapchainForVulkanSurface(
    const VulkanDevice* vulkan_device, VkSurfaceKHR surface, u32 width, u32 height,
    VkSwapchainKHR old_swapchain, u32& present_queue_family_out, VkFormat& image_format_out,
    VkExtent2D& image_extent_out, bool& is_fifo_out, bool& ui_surface_unusable_out) {
  ui_surface_unusable_out = false;

  const VulkanInstance::Functions& ifn = vulkan_device->GetVulkanInstance()->GetFunctions();
  const VkPhysicalDevice physical_device = vulkan_device->GetPhysicalDevice();
  const VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  VkSurfaceCapabilitiesKHR surface_capabilities;
  if (ifn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface,
                                                    &surface_capabilities) != VK_SUCCESS) {
    REXLOG_ERROR(ui, "VulkanPresenter: Failed to get Vulkan surface capabilities");
    return VK_NULL_HANDLE;
  }

  VkExtent2D max_framebuffer_extent =
      objects::GetMax2DFramebufferExtent(vulkan_device->GetProperties());
  VkExtent2D image_extent;
  image_extent.width = std::min(std::max(std::min(width, max_framebuffer_extent.width),
                                         surface_capabilities.minImageExtent.width),
                                surface_capabilities.maxImageExtent.width);
  image_extent.height = std::min(std::max(std::min(height, max_framebuffer_extent.height),
                                          surface_capabilities.minImageExtent.height),
                                 surface_capabilities.maxImageExtent.height);
  if (!image_extent.width || !image_extent.height ||
      image_extent.width > max_framebuffer_extent.width ||
      image_extent.height > max_framebuffer_extent.height) {
    return VK_NULL_HANDLE;
  }

  u32 queue_family_index_present = UINT32_MAX;
  const std::vector<VulkanDevice::QueueFamily>& queue_families = vulkan_device->GetQueueFamilies();
  VkBool32 queue_family_present_supported;
  // Prefer the graphics and compute queue family to avoid the concurrent sharing mode.
  u32 queue_family_index_graphics_compute = vulkan_device->GetQueueFamilyGraphicsCompute();
  const VulkanDevice::QueueFamily& queue_family_graphics_compute =
      queue_families[queue_family_index_graphics_compute];
  if (queue_family_graphics_compute.may_support_presentation &&
      !queue_family_graphics_compute.queues.empty() &&
      ifn.vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, queue_family_index_graphics_compute,
                                               surface,
                                               &queue_family_present_supported) == VK_SUCCESS &&
      queue_family_present_supported) {
    queue_family_index_present = queue_family_index_graphics_compute;
  } else {
    for (u32 i = 0; i < u32(queue_families.size()); ++i) {
      const VulkanDevice::QueueFamily& queue_family = queue_families[i];
      if (!queue_family.queues.empty() && queue_family.may_support_presentation &&
          ifn.vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface,
                                                   &queue_family_present_supported) == VK_SUCCESS &&
          queue_family_present_supported) {
        queue_family_index_present = i;
        break;
      }
    }
  }
  if (queue_family_index_present == UINT32_MAX) {
    // Not unusable, as it may become presentable after moving to a different display.
    return VK_NULL_HANDLE;
  }

  if (!(surface_capabilities.supportedTransforms &
        (VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR))) {
    REX_FAIL(ui, VK_NULL_HANDLE,
             "VulkanPresenter: The surface doesn't support identity or "
             "window-system-controlled transform");
  }

  std::vector<VkSurfaceFormatKHR> surface_formats;
  VkResult surface_formats_get_result;
  for (;;) {
    u32 surface_format_count = static_cast<u32>(surface_formats.size());
    bool surface_formats_were_empty = !surface_format_count;
    surface_formats_get_result = ifn.vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_device, surface, &surface_format_count,
        surface_formats_were_empty ? nullptr : surface_formats.data());
    // The first call with a count of 0 returns SUCCESS, not INCOMPLETE.
    if (surface_formats_get_result == VK_SUCCESS || surface_formats_get_result == VK_INCOMPLETE) {
      surface_formats.resize(surface_format_count);
      if (surface_formats_get_result == VK_SUCCESS &&
          (!surface_formats_were_empty || !surface_format_count)) {
        break;
      }
    } else {
      break;
    }
  }
  if (surface_formats_get_result != VK_SUCCESS) {
    surface_formats.clear();
  }
  // GNU/Linux X11 and Windows DWM use B8G8R8A8.
  static const VkFormat kFormat8888Primary = VK_FORMAT_B8G8R8A8_UNORM;
  static const VkFormat kFormat8888Secondary = VK_FORMAT_R8G8B8A8_UNORM;
  VkSurfaceFormatKHR image_format;
  if (surface_formats.empty() ||
      (surface_formats.size() == 1 || surface_formats[0].format == VK_FORMAT_UNDEFINED)) {
    // Any format may be chosen when the implementation reports only UNDEFINED.
    image_format.format = kFormat8888Primary;
    image_format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  } else {
    // Preference order is OS preferred sRGB 8888, any sRGB 8888, any 8888, any sRGB, the first.
    auto format_8888_primary_it = surface_formats.cend();
    auto format_8888_secondary_it = surface_formats.cend();
    auto any_non_8888_srgb_it = surface_formats.cend();
    for (auto it = surface_formats.cbegin(); it != surface_formats.cend(); ++it) {
      if (it->format != kFormat8888Primary && it->format != kFormat8888Secondary) {
        if (it->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            any_non_8888_srgb_it == surface_formats.cend()) {
          any_non_8888_srgb_it = it;
        }
        continue;
      }
      auto& preferred_8888_it =
          it->format == kFormat8888Primary ? format_8888_primary_it : format_8888_secondary_it;
      if (preferred_8888_it == surface_formats.cend()) {
        preferred_8888_it = it;
        continue;
      }
      if (it->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
          preferred_8888_it->colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        preferred_8888_it = it;
      }
    }
    if (format_8888_primary_it != surface_formats.cend() &&
        format_8888_secondary_it != surface_formats.cend()) {
      if (format_8888_primary_it->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
          format_8888_secondary_it->colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        image_format = *format_8888_primary_it;
      } else {
        image_format = *format_8888_secondary_it;
      }
    } else if (format_8888_primary_it != surface_formats.cend()) {
      image_format = *format_8888_primary_it;
    } else if (format_8888_secondary_it != surface_formats.cend()) {
      image_format = *format_8888_secondary_it;
    } else if (any_non_8888_srgb_it != surface_formats.cend()) {
      image_format = *any_non_8888_srgb_it;
    } else {
      image_format = surface_formats.front();
    }
  }

  std::vector<VkPresentModeKHR> present_modes;
  VkResult present_modes_get_result;
  for (;;) {
    u32 present_mode_count = static_cast<u32>(present_modes.size());
    bool present_modes_were_empty = !present_mode_count;
    present_modes_get_result = ifn.vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_device, surface, &present_mode_count,
        present_modes_were_empty ? nullptr : present_modes.data());
    // The first call with a count of 0 returns SUCCESS, not INCOMPLETE.
    if (present_modes_get_result == VK_SUCCESS || present_modes_get_result == VK_INCOMPLETE) {
      present_modes.resize(present_mode_count);
      if (present_modes_get_result == VK_SUCCESS &&
          (!present_modes_were_empty || !present_mode_count)) {
        break;
      }
    } else {
      break;
    }
  }
  if (present_modes_get_result != VK_SUCCESS) {
    // FIFO is required everywhere, so assume it alone on error.
    present_modes.clear();
  }

  VkSwapchainCreateInfoKHR swapchain_create_info;
  swapchain_create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapchain_create_info.pNext = nullptr;
  swapchain_create_info.flags = 0;
  swapchain_create_info.surface = surface;
  swapchain_create_info.minImageCount =
      std::max(kSubmissionCount, surface_capabilities.minImageCount);
  if (surface_capabilities.maxImageCount) {
    swapchain_create_info.minImageCount =
        std::min(swapchain_create_info.minImageCount, surface_capabilities.maxImageCount);
  }
  swapchain_create_info.imageFormat = image_format.format;
  swapchain_create_info.imageColorSpace = image_format.colorSpace;
  swapchain_create_info.imageExtent = image_extent;
  swapchain_create_info.imageArrayLayers = 1;
  swapchain_create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  u32 swapchain_queue_family_indices[2];
  if (queue_family_index_graphics_compute != queue_family_index_present) {
    swapchain_queue_family_indices[0] = queue_family_index_graphics_compute;
    swapchain_queue_family_indices[1] = queue_family_index_present;
    swapchain_create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    swapchain_create_info.queueFamilyIndexCount = 2;
    swapchain_create_info.pQueueFamilyIndices = swapchain_queue_family_indices;
  } else {
    swapchain_create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_create_info.queueFamilyIndexCount = 0;
    swapchain_create_info.pQueueFamilyIndices = nullptr;
  }
  swapchain_create_info.preTransform =
      (surface_capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
          : VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;
  // Prefer opaque, else inherit from the window system, else any as guest output writes alpha 1.
  if (surface_capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
    swapchain_create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  } else if (surface_capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
    swapchain_create_info.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  } else {
    u32 composite_alpha_shift;
    if (!rex::bit_scan_forward(static_cast<u32>(surface_capabilities.supportedCompositeAlpha),
                               &composite_alpha_shift)) {
      REX_FAIL(ui, VK_NULL_HANDLE,
               "VulkanPresenter: The surface doesn't support any composite alpha "
               "mode");
    }
    swapchain_create_info.compositeAlpha =
        VkCompositeAlphaFlagBitsKHR(static_cast<u32>(1) << composite_alpha_shift);
  }
  // Update the cvar descriptions when these priorities change.
  if (REXCVAR_GET(::rex::gpu::cvars::allow_present_mode_immediate) &&
      std::find(present_modes.cbegin(), present_modes.cend(), VK_PRESENT_MODE_IMMEDIATE_KHR) !=
          present_modes.cend()) {
    swapchain_create_info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
  } else if (REXCVAR_GET(::rex::gpu::cvars::allow_present_mode_mailbox) &&
             std::find(present_modes.cbegin(), present_modes.cend(), VK_PRESENT_MODE_MAILBOX_KHR) !=
                 present_modes.cend()) {
    swapchain_create_info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
  } else if (REXCVAR_GET(::rex::gpu::cvars::allow_present_mode_fifo_relaxed) &&
             std::find(present_modes.cbegin(), present_modes.cend(),
                       VK_PRESENT_MODE_FIFO_RELAXED_KHR) != present_modes.cend()) {
    swapchain_create_info.presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
  } else {
    swapchain_create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  }
  swapchain_create_info.clipped = VK_TRUE;
  swapchain_create_info.oldSwapchain = old_swapchain;
  VkSwapchainKHR swapchain;
  if (dfn.vkCreateSwapchainKHR(device, &swapchain_create_info, nullptr, &swapchain) != VK_SUCCESS) {
    REX_FAIL(ui, VK_NULL_HANDLE, "VulkanPresenter: Failed to create a swapchain");
  }
  REXLOG_INFO(ui,
              "VulkanPresenter: Created {}x{} swapchain with format {}, color space "
              "{}, presentation mode {}",
              swapchain_create_info.imageExtent.width, swapchain_create_info.imageExtent.height,
              static_cast<u32>(swapchain_create_info.imageFormat),
              static_cast<u32>(swapchain_create_info.imageColorSpace),
              static_cast<u32>(swapchain_create_info.presentMode));

  present_queue_family_out = queue_family_index_present;
  image_format_out = swapchain_create_info.imageFormat;
  image_extent_out = swapchain_create_info.imageExtent;
  is_fifo_out = swapchain_create_info.presentMode == VK_PRESENT_MODE_FIFO_KHR ||
                swapchain_create_info.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
  return swapchain;
}

VkSwapchainKHR VulkanPresenter::PaintContext::PrepareForSwapchainRetirement() {
  if (swapchain != VK_NULL_HANDLE) {
    submission_tracker.AwaitAllSubmissionsCompletion();
  }
  const VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  for (const SwapchainFramebuffer& framebuffer : swapchain_framebuffers) {
    dfn.vkDestroyFramebuffer(device, framebuffer.framebuffer, nullptr);
    dfn.vkDestroyImageView(device, framebuffer.image_view, nullptr);
  }
  swapchain_framebuffers.clear();
  swapchain_images.clear();
  swapchain_extent.width = 0;
  swapchain_extent.height = 0;
  VkSwapchainKHR old_swapchain = swapchain;
  swapchain = nullptr;
  return old_swapchain;
}

void VulkanPresenter::PaintContext::DestroySwapchainAndVulkanSurface() {
  VkSwapchainKHR old_swapchain = PrepareForSwapchainRetirement();
  if (old_swapchain != VK_NULL_HANDLE) {
    vulkan_device->GetFunctions().vkDestroySwapchainKHR(vulkan_device->GetDevice(), old_swapchain,
                                                        nullptr);
  }
  present_queue_family = UINT32_MAX;
  if (vulkan_surface != VK_NULL_HANDLE) {
    const VulkanInstance* vulkan_instance = vulkan_device->GetVulkanInstance();
    vulkan_instance->GetFunctions().vkDestroySurfaceKHR(vulkan_instance->GetInstance(),
                                                        vulkan_surface, nullptr);
    vulkan_surface = VK_NULL_HANDLE;
  }
}

VulkanPresenter::GuestOutputImage::~GuestOutputImage() {
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();
  if (view_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_, nullptr);
  }
  if (image_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImage(device, image_, nullptr);
  }
  if (memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, memory_, nullptr);
  }
}

bool VulkanPresenter::GuestOutputImage::Initialize() {
  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.format = kGuestOutputFormat;
  image_create_info.extent.width = extent_.width;
  image_create_info.extent.height = extent_.height;
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!gpu::vulkan::objects::CreateDedicatedAllocationImage(
          vulkan_device_, image_create_info, gpu::vulkan::objects::MemoryPurpose::kDeviceLocal,
          image_, memory_)) {
    REX_FAIL(ui, false, "VulkanPresenter: Failed to create a guest output image");
  }

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  VkImageViewCreateInfo image_view_create_info;
  image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_create_info.pNext = nullptr;
  image_view_create_info.flags = 0;
  image_view_create_info.image = image_;
  image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image_view_create_info.format = kGuestOutputFormat;
  image_view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  image_view_create_info.subresourceRange.baseMipLevel = 0;
  image_view_create_info.subresourceRange.levelCount = 1;
  image_view_create_info.subresourceRange.baseArrayLayer = 0;
  image_view_create_info.subresourceRange.layerCount = 1;
  if (dfn.vkCreateImageView(device, &image_view_create_info, nullptr, &view_) != VK_SUCCESS) {
    REX_FAIL(ui, false, "VulkanPresenter: Failed to create a guest output image view");
  }

  return true;
}

Presenter::PaintResult VulkanPresenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  u64 current_paint_submission_index = paint_context_.submission_tracker.GetCurrentSubmission();
  u64 paint_submission_count = static_cast<u64>(paint_context_.submissions.size());
  if (current_paint_submission_index >= paint_submission_count) {
    paint_context_.submission_tracker.AwaitSubmissionCompletion(current_paint_submission_index -
                                                                paint_submission_count);
  }
  const PaintContext::Submission& paint_submission =
      *paint_context_.submissions[current_paint_submission_index % paint_submission_count];

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  VkCommandPool draw_command_pool = paint_submission.GetDrawCommandPool();
  if (dfn.vkResetCommandPool(device, draw_command_pool, 0) != VK_SUCCESS) {
    REX_FAIL(ui, PaintResult::kNotPresented,
             "VulkanPresenter: Failed to reset a command buffer for drawing to the "
             "swapchain");
  }

  VkCommandBuffer draw_command_buffer = paint_submission.GetDrawCommandBuffer();
  VkCommandBufferBeginInfo command_buffer_begin_info;
  command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  command_buffer_begin_info.pNext = nullptr;
  command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  command_buffer_begin_info.pInheritanceInfo = nullptr;
  if (dfn.vkBeginCommandBuffer(draw_command_buffer, &command_buffer_begin_info) != VK_SUCCESS) {
    REX_FAIL(ui, PaintResult::kNotPresented,
             "VulkanPresenter: Failed to begin recording the command buffer for "
             "drawing to the swapchain");
  }
  // vkResetCommandPool resets a command buffer left open, so early returns are safe.

  VkSemaphore acquire_semaphore = paint_submission.GetAcquireSemaphore();
  u32 swapchain_image_index;
  VkResult acquire_result =
      dfn.vkAcquireNextImageKHR(device, paint_context_.swapchain, UINT64_MAX, acquire_semaphore,
                                VK_NULL_HANDLE, &swapchain_image_index);
  switch (acquire_result) {
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR:
      break;
    case VK_ERROR_DEVICE_LOST:
      REX_FAIL(ui, PaintResult::kGPULostResponsible,
               "VulkanPresenter: Failed to acquire the swapchain image as the "
               "device has been lost");
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_SURFACE_LOST_KHR:
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
      // Not an error, as this may occur normally while resizing on some platforms.
      REXLOG_INFO(ui,
                  "VulkanPresenter: Presentation to the swapchain image has been "
                  "dropped as the swapchain or the surface has become outdated");
      return PaintResult::kNotPresentedConnectionOutdated;
    default:
      REX_FAIL(ui, PaintResult::kNotPresented,
               "VulkanPresenter: Failed to acquire the swapchain image");
  }

  // Non-zero extents are needed for the viewport width and the guest output rectangle.
  REX_ASSERT(paint_context_.swapchain_extent.width != 0);
  REX_ASSERT(paint_context_.swapchain_extent.height != 0);

  bool swapchain_image_clear_needed = true;
  VkClearAttachment swapchain_image_clear_attachment;
  swapchain_image_clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  swapchain_image_clear_attachment.colorAttachment = 0;
  swapchain_image_clear_attachment.clearValue.color.float32[0] = 0.0f;
  swapchain_image_clear_attachment.clearValue.color.float32[1] = 0.0f;
  swapchain_image_clear_attachment.clearValue.color.float32[2] = 0.0f;
  swapchain_image_clear_attachment.clearValue.color.float32[3] = 1.0f;

  VkRenderPassBeginInfo swapchain_render_pass_begin_info;
  swapchain_render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  swapchain_render_pass_begin_info.pNext = nullptr;
  swapchain_render_pass_begin_info.renderPass = paint_context_.swapchain_render_pass;
  swapchain_render_pass_begin_info.framebuffer =
      paint_context_.swapchain_framebuffers[swapchain_image_index].framebuffer;
  swapchain_render_pass_begin_info.renderArea.offset.x = 0;
  swapchain_render_pass_begin_info.renderArea.offset.y = 0;
  swapchain_render_pass_begin_info.renderArea.extent = paint_context_.swapchain_extent;
  swapchain_render_pass_begin_info.clearValueCount = 0;
  swapchain_render_pass_begin_info.pClearValues = nullptr;
  if (paint_context_.swapchain_render_pass_clear_load_op) {
    swapchain_render_pass_begin_info.clearValueCount = 1;
    swapchain_render_pass_begin_info.pClearValues = &swapchain_image_clear_attachment.clearValue;
    swapchain_image_clear_needed = false;
  }

  bool swapchain_image_pass_begun = false;

  GuestOutputProperties guest_output_properties;
  GuestOutputPaintConfig guest_output_paint_config;
  std::shared_ptr<GuestOutputImage> guest_output_image;
  {
    u32 guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(ConsumeGuestOutput(
        guest_output_mailbox_index, &guest_output_properties, &guest_output_paint_config));
    if (guest_output_mailbox_index != UINT32_MAX) {
      REX_ASSERT(guest_output_images_[guest_output_mailbox_index].ever_successfully_refreshed);
      guest_output_image = guest_output_images_[guest_output_mailbox_index].image;
    }
    // Holding the reference makes leaving the consumer critical section safe.
  }

  if (guest_output_image) {
    VkExtent2D max_framebuffer_extent =
        objects::GetMax2DFramebufferExtent(vulkan_device_->GetProperties());
    GuestOutputPaintFlow guest_output_flow = GetGuestOutputPaintFlow(
        guest_output_properties, paint_context_.swapchain_extent.width,
        paint_context_.swapchain_extent.height, max_framebuffer_extent.width,
        max_framebuffer_extent.height, guest_output_paint_config);
    if (guest_output_flow.has_effect) {
      size_t guest_output_image_paint_ref_index = SIZE_MAX;
      size_t guest_output_image_paint_ref_new_index = SIZE_MAX;
      for (size_t i = 0; i < paint_context_.guest_output_image_paint_refs.size(); ++i) {
        const std::pair<u64, std::shared_ptr<GuestOutputImage>>& guest_output_image_paint_ref =
            paint_context_.guest_output_image_paint_refs[i];
        if (guest_output_image_paint_ref.second == guest_output_image) {
          guest_output_image_paint_ref_index = i;
          break;
        }
        if (guest_output_image_paint_ref_new_index == SIZE_MAX &&
            (!guest_output_image_paint_ref.second || !guest_output_image_paint_ref.first)) {
          guest_output_image_paint_ref_new_index = i;
        }
      }
      if (guest_output_image_paint_ref_index == SIZE_MAX) {
        if (guest_output_image_paint_ref_new_index == SIZE_MAX) {
          guest_output_image_paint_ref_new_index = 0;
          for (size_t i = 1; i < paint_context_.guest_output_image_paint_refs.size(); ++i) {
            if (paint_context_.guest_output_image_paint_refs[i].first <
                paint_context_.guest_output_image_paint_refs[guest_output_image_paint_ref_new_index]
                    .first) {
              guest_output_image_paint_ref_new_index = i;
            }
          }
          paint_context_.submission_tracker.AwaitSubmissionCompletion(
              paint_context_.guest_output_image_paint_refs[guest_output_image_paint_ref_new_index]
                  .first);
        }
        guest_output_image_paint_ref_index = guest_output_image_paint_ref_new_index;
        // The submission index is set when the image is used and not dropped on error.
        paint_context_.guest_output_image_paint_refs[guest_output_image_paint_ref_index] =
            std::make_pair(static_cast<u64>(0), guest_output_image);
        VkDescriptorImageInfo guest_output_image_descriptor_image_info;
        guest_output_image_descriptor_image_info.sampler = VK_NULL_HANDLE;
        guest_output_image_descriptor_image_info.imageView = guest_output_image->GetView();
        guest_output_image_descriptor_image_info.imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet guest_output_image_descriptor_write;
        guest_output_image_descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        guest_output_image_descriptor_write.pNext = nullptr;
        guest_output_image_descriptor_write.dstSet =
            paint_context_.guest_output_descriptor_sets
                [PaintContext::kGuestOutputDescriptorSetGuestOutput0Sampled +
                 guest_output_image_paint_ref_index];
        guest_output_image_descriptor_write.dstBinding = 0;
        guest_output_image_descriptor_write.dstArrayElement = 0;
        guest_output_image_descriptor_write.descriptorCount = 1;
        guest_output_image_descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        guest_output_image_descriptor_write.pImageInfo = &guest_output_image_descriptor_image_info;
        guest_output_image_descriptor_write.pBufferInfo = nullptr;
        guest_output_image_descriptor_write.pTexelBufferView = nullptr;
        dfn.vkUpdateDescriptorSets(device, 1, &guest_output_image_descriptor_write, 0, nullptr);
      }

      {
        GuestOutputPaintEffect swapchain_effect = guest_output_flow.effect;
        PaintContext::GuestOutputPaintPipeline& swapchain_effect_pipeline =
            paint_context_.guest_output_paint_pipelines[static_cast<size_t>(swapchain_effect)];
        if (swapchain_effect_pipeline.swapchain_pipeline != VK_NULL_HANDLE &&
            swapchain_effect_pipeline.swapchain_format !=
                paint_context_.swapchain_render_pass_format) {
          paint_context_.submission_tracker.AwaitSubmissionCompletion(
              paint_context_.guest_output_image_paint_last_submission);
          objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                        swapchain_effect_pipeline.swapchain_pipeline);
        }
        if (swapchain_effect_pipeline.swapchain_pipeline == VK_NULL_HANDLE) {
          REX_ASSERT(guest_output_paint_fs_[static_cast<size_t>(swapchain_effect)] !=
                     VK_NULL_HANDLE);
          swapchain_effect_pipeline.swapchain_pipeline = CreateGuestOutputPaintPipeline(
              swapchain_effect, paint_context_.swapchain_render_pass);
          if (swapchain_effect_pipeline.swapchain_pipeline == VK_NULL_HANDLE) {
            guest_output_flow.has_effect = false;
          } else {
            swapchain_effect_pipeline.swapchain_format =
                paint_context_.swapchain_render_pass_format;
          }
        }
      }

      if (guest_output_flow.has_effect) {
        paint_context_.guest_output_image_paint_refs[guest_output_image_paint_ref_index].first =
            current_paint_submission_index;
        paint_context_.guest_output_image_paint_last_submission = current_paint_submission_index;
        VkViewport guest_output_viewport;
        guest_output_viewport.x = 0.0f;
        guest_output_viewport.y = 0.0f;
        guest_output_viewport.minDepth = 0.0f;
        guest_output_viewport.maxDepth = 1.0f;
        VkRect2D guest_output_scissor;
        guest_output_scissor.offset.x = 0;
        guest_output_scissor.offset.y = 0;
        GuestOutputPaintEffect effect = guest_output_flow.effect;

        std::pair<u32, u32> effect_rect_size = guest_output_flow.effect_output_size;
        if (guest_output_flow.output_x == 0 && guest_output_flow.output_y == 0 &&
            effect_rect_size.first < paint_context_.swapchain_extent.width &&
            effect_rect_size.second < paint_context_.swapchain_extent.height) {
          static std::atomic<bool> logged_swapchain_expand = false;
          if (!logged_swapchain_expand.exchange(true)) {
            REXLOG_WARN(ui,
                        "VulkanPresenter: expanding final guest output rect from "
                        "{}x{} to swapchain {}x{} to avoid output only in the top left",
                        effect_rect_size.first, effect_rect_size.second,
                        paint_context_.swapchain_extent.width,
                        paint_context_.swapchain_extent.height);
          }
          effect_rect_size = std::make_pair(paint_context_.swapchain_extent.width,
                                            paint_context_.swapchain_extent.height);
          guest_output_flow.effect_output_size = effect_rect_size;
          guest_output_flow.output_x = 0;
          guest_output_flow.output_y = 0;
          guest_output_flow.letterbox_clear_rectangle_count = 0;
        }
        i32 effect_rect_x = guest_output_flow.output_x;
        i32 effect_rect_y = guest_output_flow.output_y;
        dfn.vkCmdBeginRenderPass(draw_command_buffer, &swapchain_render_pass_begin_info,
                                 VK_SUBPASS_CONTENTS_INLINE);
        swapchain_image_pass_begun = true;
        guest_output_viewport.width = static_cast<f32>(paint_context_.swapchain_extent.width);
        guest_output_viewport.height = static_cast<f32>(paint_context_.swapchain_extent.height);
        guest_output_scissor.extent.width = paint_context_.swapchain_extent.width;
        guest_output_scissor.extent.height = paint_context_.swapchain_extent.height;
        dfn.vkCmdSetViewport(draw_command_buffer, 0, 1, &guest_output_viewport);
        dfn.vkCmdSetScissor(draw_command_buffer, 0, 1, &guest_output_scissor);

        const PaintContext::GuestOutputPaintPipeline& effect_pipeline =
            paint_context_.guest_output_paint_pipelines[static_cast<size_t>(effect)];
        VkPipeline effect_vulkan_pipeline = effect_pipeline.swapchain_pipeline;
        REX_ASSERT(effect_vulkan_pipeline != VK_NULL_HANDLE);
        dfn.vkCmdBindPipeline(draw_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              effect_vulkan_pipeline);

        GuestOutputPaintPipelineLayoutIndex guest_output_paint_pipeline_layout_index =
            GetGuestOutputPaintPipelineLayoutIndex(effect);
        VkPipelineLayout guest_output_paint_pipeline_layout =
            guest_output_paint_pipeline_layouts_[guest_output_paint_pipeline_layout_index];

        PaintContext::GuestOutputDescriptorSet effect_src_descriptor_set =
            PaintContext::GuestOutputDescriptorSet(
                PaintContext::kGuestOutputDescriptorSetGuestOutput0Sampled +
                guest_output_image_paint_ref_index);
        dfn.vkCmdBindDescriptorSets(
            draw_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            guest_output_paint_pipeline_layout, 0, 1,
            &paint_context_.guest_output_descriptor_sets[effect_src_descriptor_set], 0, nullptr);

        GuestOutputPaintRectangleConstants effect_rect_constants;
        f32 effect_x_to_ndc = 2.0f / guest_output_viewport.width;
        f32 effect_y_to_ndc = 2.0f / guest_output_viewport.height;
        effect_rect_constants.x = -1.0f + static_cast<f32>(effect_rect_x) * effect_x_to_ndc;
        effect_rect_constants.y = -1.0f + static_cast<f32>(effect_rect_y) * effect_y_to_ndc;
        effect_rect_constants.width = static_cast<f32>(effect_rect_size.first) * effect_x_to_ndc;
        effect_rect_constants.height = static_cast<f32>(effect_rect_size.second) * effect_y_to_ndc;
        dfn.vkCmdPushConstants(draw_command_buffer, guest_output_paint_pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(effect_rect_constants),
                               &effect_rect_constants);

        u32 effect_constants_size = 0;
        union {
          BilinearConstants bilinear;
        } effect_constants;
        switch (guest_output_paint_pipeline_layout_index) {
          case kGuestOutputPaintPipelineLayoutIndexBilinear: {
            effect_constants_size = sizeof(effect_constants.bilinear);
            effect_constants.bilinear.Initialize(guest_output_flow);
          } break;
          default:
            break;
        }
        if (effect_constants_size) {
          dfn.vkCmdPushConstants(draw_command_buffer, guest_output_paint_pipeline_layout,
                                 VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(effect_rect_constants),
                                 effect_constants_size, &effect_constants);
        }

        dfn.vkCmdDraw(draw_command_buffer, 4, 1, 0, 0);

        if (swapchain_image_clear_needed && guest_output_flow.letterbox_clear_rectangle_count) {
          VkClearRect letterbox_clear_vulkan_rectangles[GuestOutputPaintFlow::kMaxClearRectangles];
          for (size_t i = 0; i < guest_output_flow.letterbox_clear_rectangle_count; ++i) {
            VkClearRect& letterbox_clear_vulkan_rectangle = letterbox_clear_vulkan_rectangles[i];
            const GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle =
                guest_output_flow.letterbox_clear_rectangles[i];
            letterbox_clear_vulkan_rectangle.rect.offset.x =
                static_cast<i32>(letterbox_clear_rectangle.x);
            letterbox_clear_vulkan_rectangle.rect.offset.y =
                static_cast<i32>(letterbox_clear_rectangle.y);
            letterbox_clear_vulkan_rectangle.rect.extent.width = letterbox_clear_rectangle.width;
            letterbox_clear_vulkan_rectangle.rect.extent.height = letterbox_clear_rectangle.height;
            letterbox_clear_vulkan_rectangle.baseArrayLayer = 0;
            letterbox_clear_vulkan_rectangle.layerCount = 1;
          }
          dfn.vkCmdClearAttachments(
              draw_command_buffer, 1, &swapchain_image_clear_attachment,
              static_cast<u32>(guest_output_flow.letterbox_clear_rectangle_count),
              letterbox_clear_vulkan_rectangles);
        }
        swapchain_image_clear_needed = false;
      }
    }
  }

  // Done after the paint tracker waits so the completed submission value is the most current.
  u64 completed_paint_submission =
      paint_context_.submission_tracker.UpdateAndGetCompletedSubmission();
  for (std::pair<u64, std::shared_ptr<GuestOutputImage>>& guest_output_image_paint_ref :
       paint_context_.guest_output_image_paint_refs) {
    if (!guest_output_image_paint_ref.second ||
        guest_output_image_paint_ref.second == guest_output_image) {
      continue;
    }
    if (completed_paint_submission >= guest_output_image_paint_ref.first) {
      guest_output_image_paint_ref.second.reset();
    }
  }

  if (!swapchain_image_pass_begun) {
    dfn.vkCmdBeginRenderPass(draw_command_buffer, &swapchain_render_pass_begin_info,
                             VK_SUBPASS_CONTENTS_INLINE);
  }
  if (swapchain_image_clear_needed) {
    VkClearRect swapchain_image_clear_rectangle;
    swapchain_image_clear_rectangle.rect.offset.x = 0;
    swapchain_image_clear_rectangle.rect.offset.y = 0;
    swapchain_image_clear_rectangle.rect.extent = paint_context_.swapchain_extent;
    swapchain_image_clear_rectangle.baseArrayLayer = 0;
    swapchain_image_clear_rectangle.layerCount = 1;
    dfn.vkCmdClearAttachments(draw_command_buffer, 1, &swapchain_image_clear_attachment, 1,
                              &swapchain_image_clear_rectangle);
    swapchain_image_clear_needed = false;
  }

  if (execute_ui_drawers) {
    VulkanOverlayDrawContext ui_draw_context(
        *this, paint_context_.swapchain_extent.width, paint_context_.swapchain_extent.height,
        draw_command_buffer, ui_submission_tracker_.GetCurrentSubmission(),
        ui_submission_tracker_.UpdateAndGetCompletedSubmission(),
        paint_context_.swapchain_render_pass, paint_context_.swapchain_render_pass_format);
    ExecuteUIDrawersFromUIThread(ui_draw_context);
  }

  dfn.vkCmdEndRenderPass(draw_command_buffer);

  dfn.vkEndCommandBuffer(draw_command_buffer);

  VkPipelineStageFlags acquire_semaphore_wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkCommandBuffer command_buffers[2];
  u32 command_buffer_count = 0;
  // UI setup command buffers are accessed only when execute_ui_drawers identifies the UI thread.
  size_t ui_setup_command_buffer_index =
      execute_ui_drawers ? paint_context_.ui_setup_command_buffer_current_index : SIZE_MAX;
  if (ui_setup_command_buffer_index != SIZE_MAX) {
    PaintContext::UISetupCommandBuffer& ui_setup_command_buffer =
        paint_context_.ui_setup_command_buffers[ui_setup_command_buffer_index];
    dfn.vkEndCommandBuffer(ui_setup_command_buffer.command_buffer);
    command_buffers[command_buffer_count++] = ui_setup_command_buffer.command_buffer;
    // Released regardless of the result, so UI drawers retry failed work themselves.
    paint_context_.ui_setup_command_buffer_current_index = SIZE_MAX;
  }
  command_buffers[command_buffer_count++] = draw_command_buffer;
  VkSemaphore present_semaphore = paint_submission.GetPresentSemaphore();
  VkSubmitInfo submit_info;
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.pNext = nullptr;
  submit_info.waitSemaphoreCount = 1;
  submit_info.pWaitSemaphores = &acquire_semaphore;
  submit_info.pWaitDstStageMask = &acquire_semaphore_wait_stage;
  submit_info.commandBufferCount = command_buffer_count;
  submit_info.pCommandBuffers = command_buffers;
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &present_semaphore;
  {
    VulkanSubmissionTracker::FenceAcquisition fence_acqusition(
        paint_context_.submission_tracker.AcquireFenceToAdvanceSubmission());
    VulkanSubmissionTracker::FenceAcquisition ui_fence_acquisition;
    if (execute_ui_drawers) {
      ui_fence_acquisition = ui_submission_tracker_.AcquireFenceToAdvanceSubmission();
    }
    VkResult submit_result;
    {
      const VulkanDevice::Queue::Acquisition queue_acquisition =
          vulkan_device_->AcquireQueue(vulkan_device_->GetQueueFamilyGraphicsCompute(), 0);
      submit_result = dfn.vkQueueSubmit(queue_acquisition.GetQueue(), 1, &submit_info,
                                        fence_acqusition.GetFence());
      if (ui_fence_acquisition.GetFence() != VK_NULL_HANDLE && submit_result == VK_SUCCESS) {
        if (dfn.vkQueueSubmit(queue_acquisition.GetQueue(), 0, nullptr,
                              ui_fence_acquisition.GetFence()) != VK_SUCCESS) {
          ui_fence_acquisition.SubmissionSucceededSignalFailed();
        }
      }
    }
    if (submit_result != VK_SUCCESS) {
      REXLOG_ERROR(ui, "VulkanPresenter: Failed to submit command buffers");
      fence_acqusition.SubmissionFailedOrDropped();
      ui_fence_acquisition.SubmissionFailedOrDropped();
      if (ui_setup_command_buffer_index != SIZE_MAX) {
        // Reusable at once, as the completed index stays behind and would never recycle it.
        paint_context_.ui_setup_command_buffers[ui_setup_command_buffer_index]
            .last_usage_submission_index = 0;
      }
      // An acquired image stays acquired, so request a new swapchain, not kNotPresented.
      return PaintResult::kNotPresentedConnectionOutdated;
    }
  }

  VkPresentInfoKHR present_info;
  present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present_info.pNext = nullptr;
  present_info.waitSemaphoreCount = 1;
  present_info.pWaitSemaphores = &present_semaphore;
  present_info.swapchainCount = 1;
  present_info.pSwapchains = &paint_context_.swapchain;
  present_info.pImageIndices = &swapchain_image_index;
  present_info.pResults = nullptr;
  VkResult present_result;
  {
    const VulkanDevice::Queue::Acquisition queue_acquisition =
        vulkan_device_->AcquireQueue(paint_context_.present_queue_family, 0);
    present_result = dfn.vkQueuePresentKHR(queue_acquisition.GetQueue(), &present_info);
  }
  switch (present_result) {
    case VK_SUCCESS:
      return PaintResult::kPresented;
    case VK_SUBOPTIMAL_KHR:
      return PaintResult::kPresentedSuboptimal;
    case VK_ERROR_DEVICE_LOST:
      REX_FAIL(ui, PaintResult::kGPULostResponsible,
               "VulkanPresenter: Failed to present the swapchain image as the "
               "device has been lost");
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_SURFACE_LOST_KHR:
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
      // Not an error, as this may occur normally while resizing on some platforms.
      REXLOG_INFO(ui,
                  "VulkanPresenter: Presentation to the swapchain image has been "
                  "dropped as the swapchain or the surface has become outdated");
      return PaintResult::kNotPresentedConnectionOutdated;
    default:
      REXLOG_ERROR(ui, "VulkanPresenter: Failed to present the swapchain image");
      // An acquired image stays acquired, so request a new swapchain, not kNotPresented.
      return PaintResult::kNotPresentedConnectionOutdated;
  }
}

bool VulkanPresenter::InitializeSurfaceIndependent() {
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  VkDescriptorSetLayoutBinding guest_output_image_sampler_bindings[2];
  guest_output_image_sampler_bindings[0].binding = 0;
  guest_output_image_sampler_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  guest_output_image_sampler_bindings[0].descriptorCount = 1;
  guest_output_image_sampler_bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  guest_output_image_sampler_bindings[0].pImmutableSamplers = nullptr;
  const VkSampler sampler_linear_clamp =
      ui_samplers_->GetSamplers()[UISamplers::kSamplerIndexLinearClampToEdge];
  guest_output_image_sampler_bindings[1].binding = 1;
  guest_output_image_sampler_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  guest_output_image_sampler_bindings[1].descriptorCount = 1;
  guest_output_image_sampler_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  guest_output_image_sampler_bindings[1].pImmutableSamplers = &sampler_linear_clamp;
  VkDescriptorSetLayoutCreateInfo guest_output_paint_image_descriptor_set_layout_create_info;
  guest_output_paint_image_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  guest_output_paint_image_descriptor_set_layout_create_info.pNext = nullptr;
  guest_output_paint_image_descriptor_set_layout_create_info.flags = 0;
  guest_output_paint_image_descriptor_set_layout_create_info.bindingCount =
      static_cast<u32>(rex::CountOf(guest_output_image_sampler_bindings));
  guest_output_paint_image_descriptor_set_layout_create_info.pBindings =
      guest_output_image_sampler_bindings;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &guest_output_paint_image_descriptor_set_layout_create_info, nullptr,
          &guest_output_paint_image_descriptor_set_layout_) != VK_SUCCESS) {
    REX_FAIL(ui, false,
             "VulkanPresenter: Failed to create the guest output image descriptor "
             "set layout");
  }

  VkPushConstantRange guest_output_paint_push_constant_ranges[2];
  VkPushConstantRange& guest_output_paint_push_constant_range_rect =
      guest_output_paint_push_constant_ranges[0];
  guest_output_paint_push_constant_range_rect.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  guest_output_paint_push_constant_range_rect.offset = 0;
  guest_output_paint_push_constant_range_rect.size = sizeof(GuestOutputPaintRectangleConstants);
  VkPushConstantRange& guest_output_paint_push_constant_range =
      guest_output_paint_push_constant_ranges[1];
  guest_output_paint_push_constant_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  guest_output_paint_push_constant_range.offset =
      guest_output_paint_push_constant_ranges[0].offset +
      guest_output_paint_push_constant_ranges[0].size;
  VkPipelineLayoutCreateInfo guest_output_paint_pipeline_layout_create_info;
  guest_output_paint_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  guest_output_paint_pipeline_layout_create_info.pNext = nullptr;
  guest_output_paint_pipeline_layout_create_info.flags = 0;
  guest_output_paint_pipeline_layout_create_info.setLayoutCount = 1;
  guest_output_paint_pipeline_layout_create_info.pSetLayouts =
      &guest_output_paint_image_descriptor_set_layout_;
  guest_output_paint_pipeline_layout_create_info.pPushConstantRanges =
      guest_output_paint_push_constant_ranges;
  for (size_t i = 0; i < size_t(kGuestOutputPaintPipelineLayoutCount); ++i) {
    switch (GuestOutputPaintPipelineLayoutIndex(i)) {
      case kGuestOutputPaintPipelineLayoutIndexBilinear:
        guest_output_paint_push_constant_range.size = sizeof(BilinearConstants);
        break;
      default:
        REX_UNHANDLED_CASE(GuestOutputPaintPipelineLayoutIndex(i));
        continue;
    }
    guest_output_paint_pipeline_layout_create_info.pushConstantRangeCount =
        1 + static_cast<u32>(guest_output_paint_push_constant_range.size != 0);
    if (dfn.vkCreatePipelineLayout(device, &guest_output_paint_pipeline_layout_create_info, nullptr,
                                   &guest_output_paint_pipeline_layouts_[i]) != VK_SUCCESS) {
      REX_FAIL(ui, false,
               "VulkanPresenter: Failed to create a guest output presentation "
               "pipeline layout with {} bytes of push constants",
               guest_output_paint_push_constant_range_rect.size +
                   guest_output_paint_push_constant_range.size);
    }
  }

  VkShaderModuleCreateInfo shader_module_create_info;
  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.pNext = nullptr;
  shader_module_create_info.flags = 0;
  shader_module_create_info.codeSize = sizeof(shaders::kGuestOutputTriangleStripRectVS);
  shader_module_create_info.pCode = shaders::kGuestOutputTriangleStripRectVS;
  if (dfn.vkCreateShaderModule(device, &shader_module_create_info, nullptr,
                               &guest_output_paint_vs_) != VK_SUCCESS) {
    REX_FAIL(ui, false,
             "VulkanPresenter: Failed to create the guest output presentation "
             "vertex shader module");
  }
  for (size_t i = 0; i < size_t(GuestOutputPaintEffect::kCount); ++i) {
    GuestOutputPaintEffect guest_output_paint_effect = GuestOutputPaintEffect(i);
    switch (guest_output_paint_effect) {
      case GuestOutputPaintEffect::kBilinear:
        shader_module_create_info.codeSize = sizeof(shaders::kGuestOutputBilinearPS);
        shader_module_create_info.pCode = shaders::kGuestOutputBilinearPS;
        break;
      case GuestOutputPaintEffect::kBilinearDither:
        shader_module_create_info.codeSize = sizeof(shaders::kGuestOutputBilinearDitherPS);
        shader_module_create_info.pCode = shaders::kGuestOutputBilinearDitherPS;
        break;
      default:
        continue;
    }
    if (dfn.vkCreateShaderModule(device, &shader_module_create_info, nullptr,
                                 &guest_output_paint_fs_[i]) != VK_SUCCESS) {
      REX_FAIL(ui, false,
               "VulkanPresenter: Failed to create the guest output painting shader "
               "module for effect {}",
               i);
    }
  }

  for (auto& submission : paint_context_.submissions) {
    submission = PaintContext::Submission::Create(vulkan_device_);
    if (!submission) {
      return false;
    }
  }

  VkDescriptorPoolSize guest_output_paint_descriptor_pool_sizes[2];
  guest_output_paint_descriptor_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  guest_output_paint_descriptor_pool_sizes[0].descriptorCount =
      PaintContext::kGuestOutputDescriptorSetCount;
  // Required even with immutable samplers, or descriptor set allocation fails.
  guest_output_paint_descriptor_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  guest_output_paint_descriptor_pool_sizes[1].descriptorCount =
      PaintContext::kGuestOutputDescriptorSetCount;
  VkDescriptorPoolCreateInfo guest_output_paint_descriptor_pool_create_info;
  guest_output_paint_descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  guest_output_paint_descriptor_pool_create_info.pNext = nullptr;
  guest_output_paint_descriptor_pool_create_info.flags = 0;
  guest_output_paint_descriptor_pool_create_info.maxSets =
      PaintContext::kGuestOutputDescriptorSetCount;
  guest_output_paint_descriptor_pool_create_info.poolSizeCount =
      static_cast<u32>(rex::CountOf(guest_output_paint_descriptor_pool_sizes));
  guest_output_paint_descriptor_pool_create_info.pPoolSizes =
      guest_output_paint_descriptor_pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &guest_output_paint_descriptor_pool_create_info, nullptr,
                                 &paint_context_.guest_output_descriptor_pool) != VK_SUCCESS) {
    REX_FAIL(ui, false,
             "VulkanPresenter: Failed to create the guest output painting "
             "descriptor pool");
  }
  VkDescriptorSetLayout
      guest_output_paint_descriptor_set_layouts[PaintContext::kGuestOutputDescriptorSetCount];
  std::fill(guest_output_paint_descriptor_set_layouts,
            guest_output_paint_descriptor_set_layouts +
                rex::CountOf(guest_output_paint_descriptor_set_layouts),
            guest_output_paint_image_descriptor_set_layout_);
  VkDescriptorSetAllocateInfo guest_output_paint_descriptor_set_allocate_info;
  guest_output_paint_descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  guest_output_paint_descriptor_set_allocate_info.pNext = nullptr;
  guest_output_paint_descriptor_set_allocate_info.descriptorPool =
      paint_context_.guest_output_descriptor_pool;
  guest_output_paint_descriptor_set_allocate_info.descriptorSetCount =
      PaintContext::kGuestOutputDescriptorSetCount;
  guest_output_paint_descriptor_set_allocate_info.pSetLayouts =
      guest_output_paint_descriptor_set_layouts;
  if (dfn.vkAllocateDescriptorSets(device, &guest_output_paint_descriptor_set_allocate_info,
                                   paint_context_.guest_output_descriptor_sets) != VK_SUCCESS) {
    REX_FAIL(ui, false,
             "VulkanPresenter: Failed to allocate the guest output painting "
             "descriptor sets");
  }

  return InitializeCommonSurfaceIndependent();
}

VkPipeline VulkanPresenter::CreateGuestOutputPaintPipeline(GuestOutputPaintEffect effect,
                                                           VkRenderPass render_pass) {
  VkPipelineShaderStageCreateInfo stages[2] = {
      objects::BuildShaderStage(VK_SHADER_STAGE_VERTEX_BIT, VK_NULL_HANDLE),
      objects::BuildShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, VK_NULL_HANDLE)};
  stages[0].module = guest_output_paint_vs_;
  stages[1].module = guest_output_paint_fs_[static_cast<size_t>(effect)];
  REX_ASSERT(stages[1].module != VK_NULL_HANDLE);

  VkPipelineVertexInputStateCreateInfo vertex_input_state = {};
  vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {};
  input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

  VkPipelineViewportStateCreateInfo viewport_state = {};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization_state.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample_state =
      objects::BuildMultisampleState(VK_SAMPLE_COUNT_1_BIT);

  VkPipelineColorBlendAttachmentState color_blend_attachment_state = {};
  color_blend_attachment_state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                                VK_COLOR_COMPONENT_G_BIT |
                                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend_state.attachmentCount = 1;
  color_blend_state.pAttachments = &color_blend_attachment_state;

  static const VkDynamicState kPipelineDynamicStates[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  VkPipelineDynamicStateCreateInfo dynamic_state = {};
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.dynamicStateCount = static_cast<u32>(rex::CountOf(kPipelineDynamicStates));
  dynamic_state.pDynamicStates = kPipelineDynamicStates;

  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = nullptr;
  pipeline_create_info.flags = 0;
  pipeline_create_info.stageCount = static_cast<u32>(rex::CountOf(stages));
  pipeline_create_info.pStages = stages;
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState = nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = nullptr;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout =
      guest_output_paint_pipeline_layouts_[GetGuestOutputPaintPipelineLayoutIndex(effect)];
  pipeline_create_info.renderPass = render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  VkPipeline pipeline;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                    &pipeline) != VK_SUCCESS) {
    REX_FAIL(ui, VK_NULL_HANDLE,
             "VulkanPresenter: Failed to create the guest output painting pipeline "
             "for effect {}",
             static_cast<size_t>(effect));
  }
  return pipeline;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
