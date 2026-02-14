/**
 * @file        rex/codegen/recompiled_function.h
 * @brief       Function representation for recompiler
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>  // For FunctionAuthority, TargetKind
#include <rex/result.h>
#include <rex/types.h>

namespace rex::codegen {

// Forward declarations
struct CodeRegion;
class BinaryView;

// FunctionAuthority is defined in function_graph.h

//=============================================================================
// Block-Based Discovery Types
//=============================================================================

/**
 * Basic block discovered during recursive block discovery.
 * Used as temporary scanner state during discovery when function extent is unknown.
 * Converted to FunctionNode::Block when added to the graph.
 *
 * projectedSize: Size limit for conditional branch fall-through.
 * When a conditional branch is taken, the fall-through block gets a projectedSize
 * equal to the distance to the branch target. This prevents the fall-through from
 * consuming unrelated code beyond the branch target.
 */
struct DiscoveredBlock {
  rex::guest_addr_t base = 0;   // Start address
  rex::guest_addr_t end = 0;    // End address (exclusive, points after last instruction)
  bool has_terminator = false;  // Ends with blr/bctr/unconditional branch
  int64_t projectedSize =
      -1;  // Size limit (-1 = unlimited), set on conditional branch fall-through
  std::vector<rex::guest_addr_t> successors;  // Branch targets (for CFG building)
};

/**
 * Result of block-based function discovery.
 * Contains all blocks reachable from entry point.
 */
struct FunctionBlocks {
  rex::guest_addr_t entry = 0;                    // Function entry point
  std::vector<DiscoveredBlock> blocks;            // All discovered blocks
  rex::u32 pdata_size = 0;                        // From pdata (0 if unknown)
  std::vector<JumpTable> jump_tables;             // Detected jump tables
  std::vector<rex::guest_addr_t> external_calls;  // bl targets outside this function
  std::vector<rex::guest_addr_t> tail_calls;      // Unconditional branches to other functions
};

//=============================================================================
// Function Scanner - TODO move to dedicated header to match source file
//=============================================================================

/**
 * PowerPC function scanner
 *
 * Implements heuristics for function boundary detection:
 * - Linear sweep from entry point
 * - Furthest branch target tracking
 * - Return/indirect branch detection
 * - Prologue/epilogue pattern matching
 *
 * Uses BinaryView for binary introspection.
 */
class FunctionScanner {
 public:
  explicit FunctionScanner(const BinaryView& binary);

  /**
   * Detect jump table pattern at bctr instruction
   * @param bctr_address Address of the bctr instruction
   * @return JumpTable on success, nullopt if no jump table detected
   */
  std::optional<JumpTable> detect_jump_table(rex::guest_addr_t bctr_address);

  /**
   * Discover all reachable blocks from entry point.
   * Uses recursive block discovery with pending stack.
   * @param entry_point Function entry point address
   * @param pdata_size Optional size from .pdata (0 if unknown)
   * @return FunctionBlocks containing all discovered blocks
   */
  FunctionBlocks discover_blocks(rex::guest_addr_t entry_point, rex::u32 pdata_size = 0);

  // Address translation via Module's Memory
  template <typename T>
  const T* translate_address(rex::guest_addr_t guest_addr) const;

  // Check if address is in an executable section
  bool isExecutableSection(rex::guest_addr_t address) const;

  // Set known switch table addresses (from config) to skip auto-detection
  void setKnownSwitchTables(const std::unordered_set<uint32_t>& addresses) {
    known_switch_tables_ = addresses;
  }

  // Set discontinuous function chunks (chunk_addr -> FunctionConfig with parent set)
  void setChunks(const std::unordered_map<uint32_t, FunctionConfig>& chunks) { chunks_ = chunks; }

  // Set bl targets that need resolution (for Fix 4: shared helper promotion)
  void setBlTargets(const std::unordered_set<uint32_t>& targets) { bl_targets_ = targets; }

  // Get collected bl targets from discovery
  const std::unordered_set<uint32_t>& getBlTargets() const { return bl_targets_; }

  void setKnownCallables(const std::unordered_set<uint32_t>& callables) {
    known_callables_ = callables;
  }

  bool isKnownCallable(uint32_t address) const { return known_callables_.contains(address); }

  void setDataRegions(const std::vector<std::pair<uint32_t, uint32_t>>& regions) {
    data_regions_ = regions;
  }

  bool isInDataRegion(uint32_t address) const {
    for (const auto& [start, end] : data_regions_) {
      if (address >= start && address < end)
        return true;
    }
    return false;
  }

  // Set code regions for boundary checking (prevents cross-region merges)
  void setCodeRegions(const std::vector<CodeRegion>* regions) { code_regions_ = regions; }

  // Find which code region contains the given address (nullptr if not found)
  const CodeRegion* findRegionContaining(uint32_t address) const;

  /**
   * Check if a branch from currentAddr to targetAddr stays within the same
   * code region OR targets a configured chunk of the current function.
   * Used to determine if a branch is internal or a tail call.
   *
   * @param currentAddr Address of the branch instruction
   * @param targetAddr Branch target address
   * @param functionEntry Entry point of the current function being discovered
   * @return true if the branch should be treated as internal
   */
  bool isInternalBranch(uint32_t currentAddr, uint32_t targetAddr, uint32_t functionEntry) const;

  // Check if an address is within a chunk belonging to the given function
  bool isWithinChunk(uint32_t address, uint32_t function_entry) const {
    for (const auto& [chunk_start, cfg] : chunks_) {
      if (cfg.parent == function_entry) {
        uint32_t chunk_end =
            cfg.end ? cfg.end : (cfg.size ? chunk_start + cfg.size : chunk_start + 0x1000);
        if (address >= chunk_start && address < chunk_end) {
          return true;
        }
      }
    }
    return false;
  }

  // Find chunk parent for an address (returns 0 if not in any chunk)
  uint32_t findChunkParent(uint32_t address) const {
    // First check exact chunk start matches
    auto it = chunks_.find(address);
    if (it != chunks_.end()) {
      return it->second.parent;
    }
    // Then check if address is within any chunk range
    for (const auto& [chunk_start, cfg] : chunks_) {
      uint32_t chunk_end =
          cfg.end ? cfg.end : (cfg.size ? chunk_start + cfg.size : chunk_start + 0x1000);
      if (address >= chunk_start && address < chunk_end) {
        return cfg.parent;
      }
    }
    return 0;
  }

 private:
  const BinaryView* binary_ = nullptr;
  std::unordered_set<uint32_t> known_switch_tables_;
  std::unordered_map<uint32_t, FunctionConfig> chunks_;
  std::unordered_set<uint32_t> bl_targets_;
  std::unordered_set<uint32_t> known_callables_;
  std::vector<std::pair<uint32_t, uint32_t>> data_regions_;
  const std::vector<CodeRegion>* code_regions_ = nullptr;

  // Helper methods for function boundary detection
  bool is_prologue_pattern(rex::guest_addr_t address);
  bool is_epilogue_pattern(rex::guest_addr_t address);
  bool is_restgprlr_function(rex::guest_addr_t address);
};

}  // namespace rex::codegen
