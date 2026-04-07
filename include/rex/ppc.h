/**
 * @file        ppc.h
 * @brief       PPC recompilation support -- umbrella header
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/hook.h>
#include <rex/types.h>

#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/ppc/memory.h>
#include <rex/ppc/stack.h>

// Consumer-facing using declarations
using rex::ppc::FindPPCFuncByName;
using rex::ppc::GetPPCFuncRegistry;
using rex::ppc::GuestToHostFunction;
using rex::ppc::HostToGuestFunction;
using rex::ppc::ImportFunction;
