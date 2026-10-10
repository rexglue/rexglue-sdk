/**
 * @file        codegen/function_scanner.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>
#include <rex/codegen/limits.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::codegen {

struct CodeRegion;
class BinaryView;
class DecodedBinary;

/**
 * Basic block discovered during recursive block discovery.
 * Temporary scanner state while the function extent is unknown, converted to
 * FunctionNode::Block when added to the graph.
 *
 * projectedSize limits a conditional branch fall through. When a conditional branch is taken,
 * the fall through block gets a projectedSize equal to the distance to the branch target, so it
 * cannot consume unrelated code beyond that target.
 */
struct DiscoveredBlock {
  rex::guest_addr_t base = 0;  /**< Guest address of the first instruction of the block. */
  rex::guest_addr_t end = 0;   /**< Exclusive, points after the last instruction */
  bool has_terminator = false; /**< Ends with blr, bctr or an unconditional branch */
  i64 projectedSize = -1;      /**< Size limit, -1 for unlimited */
  /** Addresses of the blocks control flow can reach next, including fall through and branch
   * targets. */
  std::vector<rex::guest_addr_t> successors;
};

/**
 * Result of block based function discovery.
 * Contains all blocks reachable from the entry point.
 */
struct FunctionBlocks {
  rex::guest_addr_t entry = 0; /**< Guest address of the function entry the scan started from. */
  std::vector<DiscoveredBlock> blocks; /**< Blocks reachable from the entry. */
  rex::u32 pdata_size = 0;             /**< Size from pdata, 0 when unknown */
  /** Jump tables found behind bctr instructions in the blocks. */
  std::vector<JumpTable> jump_tables;
  std::vector<rex::guest_addr_t> external_calls; /**< bl targets outside this function */
  std::vector<rex::guest_addr_t> tail_calls;     /**< Unconditional branches to other functions */
};

/**
 * PowerPC function scanner.
 *
 * Implements heuristics for function boundary detection.
 * - Linear sweep from the entry point
 * - Furthest branch target tracking
 * - Return and indirect branch detection
 * - Prologue and epilogue pattern matching
 *
 * Uses BinaryView for binary introspection.
 */
class FunctionScanner {
 public:
  /**
   * Creates a scanner over a binary.
   *
   * @param binary Binary to scan, must outlive the scanner
   * @param limits Analysis limits, must outlive the scanner
   */
  FunctionScanner(const BinaryView& binary, const Limits& limits);

  /**
   * Detect jump table pattern at bctr instruction
   *
   * @param bctr_address Address of the bctr instruction
   * @return JumpTable on success, nullopt if no jump table detected
   */
  std::optional<JumpTable> detect_jump_table(rex::guest_addr_t bctr_address);

  /**
   * Discover all reachable blocks from entry point.
   * Uses recursive block discovery with pending stack.
   *
   * @param entry_point Function entry point address
   * @param pdata_size Optional size from .pdata (0 if unknown)
   * @return FunctionBlocks containing all discovered blocks
   */
  FunctionBlocks discover_blocks(rex::guest_addr_t entry_point, rex::u32 pdata_size = 0);

  /**
   * Translates an address through the Module Memory.
   *
   * @tparam T Pointee type
   * @param guest_addr Address to translate
   * @return Pointer to the data
   */
  template <typename T>
  const T* translate_address(rex::guest_addr_t guest_addr) const;

  /**
   * Reports whether an address is in an executable section.
   *
   * @param address Address to test
   * @return True when executable
   */
  bool isExecutableSection(rex::guest_addr_t address) const;

  /**
   * Sets the known switch table addresses from config, which skip auto detection.
   *
   * @param addresses Switch table addresses
   */
  void setKnownSwitchTables(const std::unordered_set<u32>& addresses) {
    known_switch_tables_ = addresses;
  }

  /**
   * Sets the discontinuous function chunks.
   *
   * @param chunks Chunk address mapped to a FunctionConfig with parent set
   */
  void setChunks(const std::unordered_map<u32, FunctionConfig>& chunks) { chunks_ = chunks; }

  /**
   * Sets the bl targets that need resolution.
   *
   * @param targets bl target addresses
   */
  void setBlTargets(const std::unordered_set<u32>& targets) { bl_targets_ = targets; }

  /** Returns the bl targets collected by discovery. */
  const std::unordered_set<u32>& getBlTargets() const { return bl_targets_; }

  /**
   * Sets the addresses of known functions. A branch to one is a tail call and a sweep stops at one.
   *
   * @param callables Entry point addresses
   */
  void setKnownCallables(const std::unordered_set<u32>& callables) { known_callables_ = callables; }

  bool isKnownCallable(u32 address) const { return known_callables_.contains(address); }

  /**
   * Sets the address ranges that hold data instead of code.
   *
   * @param regions Ranges of start address up to but excluding end address
   */
  void setDataRegions(const std::vector<std::pair<u32, u32>>& regions) { data_regions_ = regions; }

  /**
   * Reports whether an address is in a data region.
   *
   * @param address Address to test
   * @return True when a data region contains address
   */
  bool isInDataRegion(u32 address) const {
    for (const auto& [start, end] : data_regions_) {
      if (address >= start && address < end)
        return true;
    }
    return false;
  }

  /**
   * Sets the code regions used for boundary checking, which prevents cross region merges.
   *
   * @param regions Code regions, not owned
   */
  void setCodeRegions(const std::vector<CodeRegion>* regions) { code_regions_ = regions; }

  /**
   * Finds the code region containing an address.
   *
   * @param address Address to look up
   * @return The region, or null when none contains address
   */
  const CodeRegion* findRegionContaining(u32 address) const;

  /**
   * Reports whether a branch from currentAddr to targetAddr stays within the same code region
   * or targets a configured chunk of the current function. Separates an internal branch from a
   * tail call.
   *
   * @param currentAddr Address of the branch instruction
   * @param targetAddr Branch target address
   * @param functionEntry Entry point of the function being discovered
   * @return True when the branch is treated as internal
   */
  bool isInternalBranch(u32 currentAddr, u32 targetAddr, u32 functionEntry) const;

  /**
   * Reports whether an address is within a chunk belonging to a function.
   *
   * @param address Address to test
   * @param function_entry Entry point of the function
   * @return True when address is inside one of its chunks
   */
  bool isWithinChunk(u32 address, u32 function_entry) const {
    for (const auto& [chunk_start, cfg] : chunks_) {
      if (cfg.parent == function_entry) {
        u32 chunk_end =
            cfg.end ? cfg.end : (cfg.size ? chunk_start + cfg.size : chunk_start + 0x1000);
        if (address >= chunk_start && address < chunk_end) {
          return true;
        }
      }
    }
    return false;
  }

  /**
   * Finds the chunk parent for an address. An exact chunk start match is checked first, then
   * every chunk range.
   *
   * @param address Address to look up
   * @return Parent function entry, 0 when address is in no chunk
   */
  u32 findChunkParent(u32 address) const {
    auto it = chunks_.find(address);
    if (it != chunks_.end()) {
      return it->second.parent;
    }
    for (const auto& [chunk_start, cfg] : chunks_) {
      u32 chunk_end =
          cfg.end ? cfg.end : (cfg.size ? chunk_start + cfg.size : chunk_start + 0x1000);
      if (address >= chunk_start && address < chunk_end) {
        return cfg.parent;
      }
    }
    return 0;
  }

 private:
  const BinaryView* binary_ = nullptr;
  const Limits* limits_ = nullptr;
  std::unordered_set<u32> known_switch_tables_;
  std::unordered_map<u32, FunctionConfig> chunks_;
  std::unordered_set<u32> bl_targets_;
  std::unordered_set<u32> known_callables_;
  std::vector<std::pair<u32, u32>> data_regions_;
  const std::vector<CodeRegion>* code_regions_ = nullptr;

  bool is_prologue_pattern(rex::guest_addr_t address) const;
  bool is_epilogue_pattern(rex::guest_addr_t address) const;
  bool is_restgprlr_function(rex::guest_addr_t address) const;
};

/** Branch whose target the function graph must still resolve. */
struct UnresolvedBranch {
  u32 site;           /**< Address of the branch instruction */
  u32 target;         /**< Branch target address */
  bool isCall;        /**< True for bl, false for b */
  bool isConditional; /**< True for bc, beq and other conditional branches */
};

/** Blocks, branches and instructions found by discoverBlocks() for 1 function. */
struct BlockDiscoveryResult {
  BlockDiscoveryResult() = default;
  BlockDiscoveryResult(const BlockDiscoveryResult&) = default;
  BlockDiscoveryResult& operator=(const BlockDiscoveryResult&) = default;
  BlockDiscoveryResult(BlockDiscoveryResult&&) noexcept(false) = default;
  BlockDiscoveryResult& operator=(BlockDiscoveryResult&&) noexcept(false) = default;
  ~BlockDiscoveryResult() = default;

  std::vector<Block> blocks;                        /**< Discovered blocks */
  std::vector<UnresolvedBranch> unresolvedBranches; /**< Branches left for the graph to resolve */
  std::vector<JumpTable> jumpTables;                /**< Detected jump tables */
  std::set<u32> labels;                             /**< Internal branch target addresses */

  std::vector<rex::codegen::ppc::Instruction*> instructions; /**< Handed to FunctionNode */

  std::vector<u32> externalCalls; /**< bl to unknown targets */
  std::vector<u32> tailCalls;     /**< b to external targets */
};

/**
 * Discover all blocks belonging to a function starting at entryPoint.
 *
 * The scan runs in these steps.
 * - Worklist-based block discovery
 * - Linear sweep until terminator (blr, bctr, unconditional b)
 * - Follow both paths for conditional branches
 * - Detect jump tables at bctr instructions
 * - Stop at code region boundaries (null padding)
 *
 * @param decoded The decoded binary (single-pass decoded instructions)
 * @param limits Analysis limits
 * @param entryPoint Starting address of the function
 * @param containingRegion Code region containing the entry point
 * @param knownFunctions Set of known function entry points (to detect tail calls)
 * @param pdataSize Function size in bytes from PDATA, or 0 when unknown
 * @param manualSwitchTables Configured switch tables keyed by bctr address, or nullptr
 * @return BlockDiscoveryResult containing blocks, branches, and jump tables
 */
BlockDiscoveryResult discoverBlocks(
    DecodedBinary& decoded, const Limits& limits, u32 entryPoint,
    const CodeRegion& containingRegion, const std::unordered_set<u32>& knownFunctions,
    u32 pdataSize = 0, const std::unordered_map<u32, JumpTable>* manualSwitchTables = nullptr);

/**
 * Detect jump table at a bctr instruction.
 *
 * Detected patterns.
 * - ABSOLUTE, lwzx loads full 32-bit addresses
 * - COMPUTED, lbzx + rlwinm (byte offset with shift)
 * - BYTEOFFSET, lbzx + add (byte offset direct)
 * - SHORTOFFSET, lhzx + add (16-bit offset)
 *
 * @param decoded The decoded binary
 * @param limits Analysis limits
 * @param bctrAddr Address of the bctr instruction
 * @param containingRegion Code region for validation
 * @param funcStart Function start address, lower bound for table targets
 * @param funcEnd Function end address
 * @return JumpTable if detected, empty optional otherwise
 */
std::optional<JumpTable> detectJumpTable(DecodedBinary& decoded, const Limits& limits, u32 bctrAddr,
                                         const CodeRegion& containingRegion, u32 funcStart,
                                         u32 funcEnd);

}  // namespace rex::codegen
