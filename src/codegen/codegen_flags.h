/**
 * @file        codegen/codegen_flags.h
 * @brief       CVar declarations for the codegen pipeline
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>

// Codegen/Output
REXCVAR_DECLARE(uint32_t, functions_per_file);
REXCVAR_DECLARE(uint32_t, progress_log_frequency);

// Codegen/Analysis
REXCVAR_DECLARE(uint32_t, max_discovery_iterations);
REXCVAR_DECLARE(uint32_t, max_vtable_iterations);
REXCVAR_DECLARE(uint32_t, max_resolve_iterations);
REXCVAR_DECLARE(uint32_t, max_eh_states);
REXCVAR_DECLARE(uint32_t, max_eh_try_blocks);
REXCVAR_DECLARE(uint32_t, max_eh_ip_map_entries);
REXCVAR_DECLARE(uint32_t, max_seh_scope_entries);

// Codegen/Discovery
REXCVAR_DECLARE(uint32_t, backward_scan_limit);
REXCVAR_DECLARE(uint32_t, max_jump_table_entries);
REXCVAR_DECLARE(uint32_t, max_blocks_per_function);

// Codegen (moved from main.cpp)
REXCVAR_DECLARE(bool, force);
REXCVAR_DECLARE(bool, enable_exception_handlers);
