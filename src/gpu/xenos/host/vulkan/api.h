/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/assert.h>
#include <rex/platform.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#if REX_PLATFORM_ANDROID
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#endif

#if REX_PLATFORM_GNU_LINUX
#ifndef VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#endif
// Set from the platform, never the build system, as it selects VulkanInstance::Functions members.
#ifndef VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include <wayland-client.h>
#endif

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#endif

#if REX_PLATFORM_MAC
#ifndef VK_USE_PLATFORM_METAL_EXT
#define VK_USE_PLATFORM_METAL_EXT
#endif
#endif

#include <vulkan/vulkan.h>
