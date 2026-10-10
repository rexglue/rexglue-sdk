/**
 * @file        codegen/function_graph.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/codegen/function_node.h>
#include <rex/move_only_function.h>

namespace rex::codegen {

/**
 * Container for all function nodes. Manages resolution notifications and vacancy checking for
 * merge eligibility.
 *
 * A region is vacant when all of these hold.
 * - No null dword at the boundary
 * - No chunk claims the region
 * - The target does not fall within a protected function range, where PDATA, CONFIG and HELPER
 *   functions are always protected and DISCOVERED functions with xrefs can be merged as a
 *   potential internal label
 */
class FunctionGraph {
 public:
  /**
   * Reads the dword stored at a guest address.
   *
   * @param addr Guest address
   * @return The dword, or nullopt when addr is unreadable
   */
  using MemoryReader = rex::MoveOnlyFunction<std::optional<u32>(u32 addr) const>;

  /**
   * Adds a function to the graph and notifies all PENDING functions to try resolution against
   * the new entry.
   *
   * @param base Entry point address
   * @param size Size in bytes
   * @param authority Authority of the registration, the higher authority wins on a repeat
   * @param hasXrefs True when this is a known call target such as a bl target
   * @return The new node, or the existing node when already present
   */
  FunctionNode* addFunction(u32 base, u32 size, FunctionAuthority authority, bool hasXrefs = false);

  /**
   * Adds a named function to the graph.
   *
   * @param base Entry point address
   * @param size Size in bytes
   * @param authority Authority of the registration
   * @param name Function name
   * @param hasXrefs True when this is a known call target
   * @return The new node, or the existing node when already present
   */
  FunctionNode* addFunction(u32 base, u32 size, FunctionAuthority authority, std::string_view name,
                            bool hasXrefs = false);

  /**
   * Adds a resolved import as a callable function with an __imp__ name.
   *
   * @param address Thunk address that bl instructions target
   * @param resolvedName Resolved import name
   * @return The new node
   */
  FunctionNode* addImportFunction(u32 address, std::string_view resolvedName);

  /**
   * Returns the function at an entry point in O(1).
   *
   * @param entryPoint Entry point address
   * @return The node, or null when absent
   */
  FunctionNode* getFunction(u32 entryPoint);
  /**
   * Returns the function at an entry point in O(1).
   *
   * @param entryPoint Entry point address
   * @return The node, or null when absent
   */
  const FunctionNode* getFunction(u32 entryPoint) const;

  /**
   * Removes a function from the graph, used to clean up absorbed GAP_FILLs.
   *
   * @param entryPoint Entry point address
   * @return True when a function was removed
   */
  bool removeFunction(u32 entryPoint);

  /**
   * Returns the function containing an address in O(log f) through the sorted base index.
   *
   * @param addr Address to look up
   * @return The node, or null when no function contains addr
   */
  FunctionNode* getFunctionContaining(u32 addr);
  /**
   * Returns the function containing an address in O(log f) through the sorted base index.
   *
   * @param addr Address to look up
   * @return The node, or null when no function contains addr
   */
  const FunctionNode* getFunctionContaining(u32 addr) const;

  /**
   * Reports whether an address is a known entry point.
   *
   * @param addr Address to test
   * @return True when a function starts at addr
   */
  bool isEntryPoint(u32 addr) const;

  /**
   * Reports whether an address is an import, a FunctionNode with IMPORT authority.
   *
   * @param addr Address to test
   * @return True when addr is an import
   */
  bool isImport(u32 addr) const;

  /**
   * Returns all functions including imports with IMPORT authority.
   *
   * @return Functions keyed by entry point
   */
  const std::unordered_map<u32, std::unique_ptr<FunctionNode>>& functions() const {
    return functions_;
  }

  /**
   * Returns all PENDING functions.
   *
   * @return Pending nodes
   */
  std::vector<FunctionNode*> getPendingFunctions();

  /**
   * Returns all SEALED functions.
   *
   * @return Sealed nodes
   */
  std::vector<FunctionNode*> getSealedFunctions();

  /** Returns the number of functions. */
  size_t functionCount() const { return functions_.size(); }
  /**
   * Counts the functions that are not sealed.
   *
   * @return Number of PENDING functions
   */
  size_t pendingCount() const;
  /**
   * Counts the sealed functions.
   *
   * @return Number of SEALED functions
   */
  size_t sealedCount() const;

  /**
   * Sets a function name. Called during the Discover phase.
   *
   * @param entry Entry point address
   * @param name Function name
   */
  void setFunctionName(u32 entry, std::string name);

  /**
   * Sets the exception handler flag. Called during the Discover phase.
   *
   * @param entry Entry point address
   * @param val Flag value
   */
  void setFunctionHasExceptionHandler(u32 entry, bool val);

  /**
   * Sets the parsed SEH or C++ EH info. Called during the Discover phase.
   *
   * @param entry Entry point address
   * @param info Parsed exception info
   */
  void setFunctionExceptionInfo(u32 entry, ExceptionInfo info);

  /**
   * Adds a block to a function. Called during the Discover phase.
   *
   * @param entry Entry point address
   * @param block Block to add
   */
  void addBlockToFunction(u32 entry, Block block);

  /**
   * Adds a label, an internal branch target, to a function. Called during the Discover phase.
   *
   * @param entry Entry point address
   * @param label Label address
   */
  void addLabelToFunction(u32 entry, u32 label);

  /**
   * Adds a resolved call to a function. Called during the Discover phase.
   *
   * @param entry Entry point address
   * @param site Call site address
   * @param target Resolved target
   */
  void addCallToFunction(u32 entry, u32 site, CallTarget target);

  /**
   * Adds a resolved tail call to a function. Called during the Discover phase.
   *
   * @param entry Entry point address
   * @param site Call site address
   * @param target Resolved target
   */
  void addTailCallToFunction(u32 entry, u32 site, CallTarget target);

  /**
   * Adds a jump table to a function. Called during the Discover phase.
   *
   * @param entry Entry point address
   * @param jt Jump table to add
   */
  void addJumpTableToFunction(u32 entry, JumpTable jt);

  /**
   * Adds an unresolved jump to a function. Called during the Discover phase.
   *
   * @param entry Entry point address
   * @param site Jump site address
   * @param target Jump target address
   * @param isCall True for bl (call), false for b (tail call)
   * @param conditional True for a conditional branch
   */
  void addUnresolvedJumpToFunction(u32 entry, u32 site, u32 target, bool isCall, bool conditional);

  /**
   * Tries to resolve all unresolved jumps of a function against known functions, imports and
   * internal labels. Called during the Merge phase.
   *
   * @param entry Entry point address
   * @return Number of jumps resolved
   */
  size_t tryResolveFunction(u32 entry);

  /**
   * Absorbs a region into a function for vacancy expansion. Called during the Merge phase.
   *
   * @param entry Entry point address
   * @param regionBase Region start address
   * @param regionSize Region size in bytes
   */
  void absorbRegionIntoFunction(u32 entry, u32 regionBase, u32 regionSize);

  /**
   * Seals a function if it can be sealed. Called during the Merge phase.
   *
   * @param entry Entry point address
   * @return True when the function was sealed
   */
  bool trySealFunction(u32 entry);

  /**
   * Seals all functions that can be sealed. Called during the Merge phase.
   *
   * @return Number of functions sealed
   */
  size_t sealAllReady();

  /** Seals all functions and throws if any cannot be sealed, enforcing that discovery is done. */
  void sealAll();

  /**
   * Sets the memory reader for null dword checking.
   *
   * @param reader Reader returning the dword at an address
   */
  void setMemoryReader(MemoryReader reader) { memoryReader_ = std::move(reader); }

  /**
   * Registers a chunk, an address range claimed by config that blocks vacancy.
   *
   * @param base Range start address
   * @param size Range size in bytes
   */
  void registerChunk(u32 base, u32 size);

  /**
   * Reports whether a region is vacant for absorption.
   *
   * @param fromAddr Address being expanded from, used to check for a null boundary
   * @param targetAddr Start of the region to absorb
   * @return True when the region is vacant
   */
  bool isVacant(u32 fromAddr, u32 targetAddr) const;

  /**
   * Reports whether a target is a mergeable entry point, a DISCOVERED function with xrefs.
   *
   * @param addr Address to test
   * @return True when addr is mergeable
   */
  bool isMergeableEntryPoint(u32 addr) const;

  /**
   * Flags every SEH funclet as register sharing. Runs after discovery, once funclet extents are
   * known.
   *
   * @return Number of funclets flagged
   */
  size_t markFuncletRegisterSharing();

  /**
   * Classifies a branch target for code generation.
   *
   * @param target Address being branched to
   * @param callerAddr Address of the branch instruction
   * @param isCallInstruction True for bl (expects return), false for b (no return)
   * @return How the target is treated during code generation
   */
  TargetKind classifyTarget(u32 target, u32 callerAddr, bool isCallInstruction) const;

 private:
  std::unordered_map<u32, std::unique_ptr<FunctionNode>> functions_;
  std::map<u32, FunctionNode*> functionsByBase_;
  std::unordered_map<u32, bool> functionHasXrefs_;
  std::vector<std::pair<u32, u32>> chunks_;
  MemoryReader memoryReader_;

  void notifyFunctionAdded(FunctionNode* newFunction);
};

}  // namespace rex::codegen
