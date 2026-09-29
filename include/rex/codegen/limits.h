/**
 * @file        codegen/limits.h
 * @brief       Recompiler analysis limits
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

namespace rex::codegen {

/**
 * Analysis limits. Passed by the tool, hashed into the input fingerprint.
 */
struct Limits {
  /** Largest generated source file in bytes before the writer starts a new one. */
  uint32_t max_file_size_bytes = 1048576;
  /** Discovery passes before giving up on a fixed point. */
  uint32_t max_discovery_iterations = 1000;
  /** Vtable resolution passes. */
  uint32_t max_vtable_iterations = 100;
  /** Merge resolution passes. */
  uint32_t max_resolve_iterations = 100;
  /** C++ exception states a function may declare. */
  uint32_t max_eh_states = 100;
  /** C++ try blocks a function may declare. */
  uint32_t max_eh_try_blocks = 50;
  /** C++ IP to state map entries a function may declare. */
  uint32_t max_eh_ip_map_entries = 200;
  /** SEH scope entries a function may declare. */
  uint32_t max_seh_scope_entries = 100;
  /** Instructions scanned backwards for a jump table base. */
  uint32_t backward_scan_limit = 64;
  /** Jump table entries accepted. */
  uint32_t max_jump_table_entries = 512;
  /** Basic blocks a function may have before it is skipped. */
  uint32_t max_blocks_per_function = 10000;
};

}  // namespace rex::codegen
