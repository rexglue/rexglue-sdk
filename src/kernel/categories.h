/**
 * @file        kernel/categories.h
 * @brief       The log categories of the kernel exports
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging/log.h>

REXLOG_CATEGORY(kernel, "Kernel exports of xboxkrnl")
REXLOG_CATEGORY(title, "Debug output the title writes through DbgPrint and OutputDebugString")
