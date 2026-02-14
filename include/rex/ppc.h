/**
 * @file        ppc.h
 * @brief       PPC recompilation support — umbrella header
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @usage       #include <rex/ppc.h>
 *              PPC_HOOK(rex_RtlAllocateHeap, RtlAllocateHeap_entry)
 */

#pragma once

#include <rex/ppc/context.h>
#include <rex/ppc/exceptions.h>
#include <rex/ppc/function.h>
#include <rex/ppc/memory.h>
#include <rex/ppc/types.h>
