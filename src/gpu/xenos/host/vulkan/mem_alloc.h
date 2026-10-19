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
// Include vulkan.h from third_party, not the system directory, before vk_mem_alloc.h.

#include "gpu/xenos/host/vulkan/device.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
// Silences the Clang pointer nullability completeness warnings.
#ifndef VMA_NULLABLE
#define VMA_NULLABLE
#endif
#ifndef VMA_NOT_NULL
#define VMA_NOT_NULL
#endif
#include <vk_mem_alloc.h>

namespace rex {
namespace gpu {
namespace vulkan {

VmaAllocator CreateVMAAllocator(const VulkanDevice* vulkan_device, bool externally_synchronized);

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
