#pragma once
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

// Make sure vulkan.h is included from third_party (rather than from the system
// include directory) before vk_mem_alloc.h.

#include <rex/ui/vulkan/device.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
// Work around the pointer nullability completeness warnings on Clang.
#ifndef VMA_NULLABLE
#define VMA_NULLABLE
#endif
#ifndef VMA_NOT_NULL
#define VMA_NOT_NULL
#endif
#include <vk_mem_alloc.h>

namespace rex {
namespace ui {
namespace vulkan {

VmaAllocator CreateVmaAllocator(const VulkanDevice* vulkan_device, bool externally_synchronized);

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
