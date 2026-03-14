/**
 * @file        rex/codegen/function_graph.h
 * @brief       Function graph - reactive model for function discovery and resolution
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/codegen/function_node.h>

namespace rex::codegen {

//=============================================================================
// Function Graph
//=============================================================================
// Container for all function nodes. Manages resolution notifications.
// Also handles vacancy checking for merge eligibility.
//
// Vacancy Rules - A region is vacant if ALL are true:
//   1. No null dword at the boundary
//   2. No chunk claims the region
//   3. Target does not fall within a protected function's range:
//      - PDATA/CONFIG/HELPER: always protected (cannot merge into)
//      - DISCOVERED with xrefs: CAN be merged (treated as potential internal label)

class FunctionGraph {
 public:
  using MemoryReader = std::function<std::optional<uint32_t>(uint32_t addr)>;

  //=========================================================================
  // Code Buffer Management
  //=========================================================================

  // Add a code buffer (copies executable section data into graph)
  void addCodeBuffer(uint32_t baseAddress, const uint8_t* data, size_t size);

  // Translate guest address to host pointer (searches all code buffers)
  const uint8_t* translateCode(uint32_t addr) const;

  // Get all code buffers (for iteration/debugging)
  const std::vector<CodeBuffer>& codeBuffers() const { return codeBuffers_; }

  // Update all function code pointers (call after loading code buffers)
  void updateFunctionCodePointers();

  //=========================================================================
  // Function Management
  //=========================================================================

  // Add a function to the graph.
  // Returns the new node, or existing node if already present (higher authority wins).
  // Notifies all PENDING functions to try resolution against the new entry.
  // hasXrefs: true if this is a known call target (bl target, etc.)
  FunctionNode* addFunction(uint32_t base, uint32_t size, FunctionAuthority authority,
                            bool hasXrefs = false);

  // Add a named function to the graph (convenience overload)
  FunctionNode* addFunction(uint32_t base, uint32_t size, FunctionAuthority authority,
                            std::string_view name, bool hasXrefs = false);

  // Add a resolved import as a callable function with __imp__ name
  // Address is the thunk address that bl instructions target
  FunctionNode* addImportFunction(uint32_t address, std::string_view resolvedName);

  // Get function by entry point (O(1))
  FunctionNode* getFunction(uint32_t entryPoint);
  const FunctionNode* getFunction(uint32_t entryPoint) const;

  // Remove function from graph (for cleanup of absorbed GAP_FILLs)
  bool removeFunction(uint32_t entryPoint);

  // Get function containing address (O(log f) via sorted base index)
  FunctionNode* getFunctionContaining(uint32_t addr);
  const FunctionNode* getFunctionContaining(uint32_t addr) const;

  // Check if address is a known entry point
  bool isEntryPoint(uint32_t addr) const;

  // Check if address is an import (FunctionNode with IMPORT authority)
  bool isImport(uint32_t addr) const;

  // Iterate all functions (includes imports with IMPORT authority)
  const std::unordered_map<uint32_t, std::unique_ptr<FunctionNode>>& functions() const {
    return functions_;
  }

  // Get all PENDING functions
  std::vector<FunctionNode*> getPendingFunctions();

  // Get all SEALED functions
  std::vector<FunctionNode*> getSealedFunctions();

  // Statistics
  size_t functionCount() const { return functions_.size(); }
  size_t pendingCount() const;
  size_t sealedCount() const;

  //=========================================================================
  // Function Setup (called during Discover phase)
  //=========================================================================

  // Set function name
  void setFunctionName(uint32_t entry, std::string name);

  // Set exception handler flag
  void setFunctionHasExceptionHandler(uint32_t entry, bool val);

  // Set parsed exception info (SEH or C++ EH)
  void setFunctionExceptionInfo(uint32_t entry, ExceptionInfo info);

  // Add a block to a function
  void addBlockToFunction(uint32_t entry, Block block);

  // Add a label (internal branch target) to a function
  void addLabelToFunction(uint32_t entry, uint32_t label);

  // Add a resolved call to a function
  void addCallToFunction(uint32_t entry, uint32_t site, CallTarget target);

  // Add a resolved tail call to a function
  void addTailCallToFunction(uint32_t entry, uint32_t site, CallTarget target);

  // Add a jump table to a function
  void addJumpTableToFunction(uint32_t entry, JumpTable jt);

  // Add an unresolved jump to a function
  // isCall: true for bl (call), false for b (tail call)
  void addUnresolvedJumpToFunction(uint32_t entry, uint32_t site, uint32_t target, bool isCall,
                                   bool conditional);

  //=========================================================================
  // Resolution and Expansion (called during Merge phase)
  //=========================================================================

  // Try to resolve all unresolved jumps for a function
  // Checks against known functions, imports, and internal labels
  // Returns number of jumps resolved
  size_t tryResolveFunction(uint32_t entry);

  // Absorb a region into a function (for vacancy expansion)
  void absorbRegionIntoFunction(uint32_t entry, uint32_t regionBase, uint32_t regionSize);

  // Seal a function if it can be sealed
  // Returns true if function was sealed
  bool trySealFunction(uint32_t entry);

  // Seal all functions that can be sealed
  // Returns number of functions sealed
  size_t sealAllReady();

  // Seal all functions, throwing if any cannot be sealed
  // Use after discovery is complete to enforce all functions are ready
  void sealAll();

  //=========================================================================
  // Vacancy Checking
  //=========================================================================

  // Set the memory reader for null-dword checking
  void setMemoryReader(MemoryReader reader) { memoryReader_ = std::move(reader); }

  // Register a chunk (address range claimed by config, blocks vacancy)
  void registerChunk(uint32_t base, uint32_t size);

  // Check if a region is vacant for absorption
  // fromAddr: the address we're expanding from (to check for null boundary)
  // targetAddr: the start of the region we want to absorb
  bool isVacant(uint32_t fromAddr, uint32_t targetAddr) const;

  // Check if target is a mergeable entry point (DISCOVERED with xrefs)
  bool isMergeableEntryPoint(uint32_t addr) const;

  //=========================================================================
  // Target Classification (for code generation)
  //=========================================================================

  // Classify a branch target for code generation.
  // target: address being branched to
  // callerAddr: address of the branch instruction
  // isCallInstruction: true for bl (expects return), false for b (no return)
  // Returns how the target should be treated during code generation.
  TargetKind classifyTarget(uint32_t target, uint32_t callerAddr, bool isCallInstruction) const;

 private:
  std::vector<CodeBuffer> codeBuffers_;
  std::unordered_map<uint32_t, std::unique_ptr<FunctionNode>> functions_;
  std::map<uint32_t, FunctionNode*>
      functionsByBase_;  // sorted by base for O(log f) interval lookup
  std::unordered_map<uint32_t, bool> functionHasXrefs_;  // entry -> hasXrefs
  std::vector<std::pair<uint32_t, uint32_t>> chunks_;    // base, size pairs
  MemoryReader memoryReader_;

  // Notify all PENDING functions that a new function was added
  void notifyFunctionAdded(FunctionNode* newFunction);
};

}  // namespace rex::codegen
