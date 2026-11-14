/**
 * @file        gpu/xenos/host/vulkan/vulkan_to_string.h
 * @brief       The Vulkan C++ binding to_string functions, which only a few sources use
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "gpu/xenos/host/vulkan/api.h"

// vulkan_to_string.hpp includes vulkan.hpp, which includes it back unless this is defined.
#ifndef VULKAN_HPP_NO_TO_STRING
#define VULKAN_HPP_NO_TO_STRING
#endif

#include <vulkan/vulkan_hpp_macros.hpp>
#include <vulkan/vulkan_to_string.hpp>
