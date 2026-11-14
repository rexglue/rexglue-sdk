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

#include "function_node.h"
#include <rex/utility.h>
#include <rex/types.h>

namespace rex::codegen {

/**
 * Container for all function nodes. Notifies pending functions of new entries and checks vacancy
 * for merge eligibility. A region is vacant when it has no null dword at the boundary, no chunk
 * claims it and the target is outside a protected function range. PDATA, CONFIG and HELPER
 * functions are always protected, and DISCOVERED functions with xrefs can be merged as an
 * internal label.
 */
class FunctionGraph : public rex::MoveOnly {
 public:
  FunctionGraph() = default;

  /** Reads the dword at a guest address, nullopt when it is unreadable. */
  using MemoryReader = rex::MoveOnlyFunction<std::optional<u32>(u32 addr) const>;

  /**
   * Adds a function and notifies all PENDING functions to try resolution against it.
   *
   * @param authority Authority of the registration, the higher authority wins on a repeat
   * @param has_xrefs True when this is a known call target such as a bl target
   * @return The new node, or the existing node when present
   */
  FunctionNode* AddFunction(u32 base, u32 size, FunctionAuthority authority,
                            bool has_xrefs = false);

  /**
   * Adds a named function and notifies all PENDING functions to try resolution against it.
   *
   * @param authority Authority of the registration, the higher authority wins on a repeat
   * @param has_xrefs True when this is a known call target such as a bl target
   * @return The new node, or the existing node when present
   */
  FunctionNode* AddFunction(u32 base, u32 size, FunctionAuthority authority, std::string_view name,
                            bool has_xrefs = false);

  /**
   * Adds a resolved import as a callable function with an __imp__ name.
   *
   * @param address Thunk address that bl instructions target
   */
  FunctionNode* AddImportFunction(u32 address, std::string_view resolved_name, u32 size = 4);

  /** Declares that a call to an address never returns, such as an import that ends the thread. */
  void DeclareNoReturn(u32 address);

  /** Reports whether DeclareNoReturn was called for the address. */
  bool IsDeclaredNoReturn(u32 address) const {
    return tables_->declared_no_return.contains(address);
  }

  /**
   * Reports whether a function never returns to its caller. A declared address never returns. Any
   * other function never returns when no path from its entry reaches a return, an unresolved
   * target or a function that returns. A cycle with no exit to a return never returns, and an
   * unknown address returns.
   */
  bool IsNoReturn(u32 entry) const;

  /** Returns the function at an entry point in O(1), or null when absent. */
  FunctionNode* GetFunction(u32 entry_point);
  const FunctionNode* GetFunction(u32 entry_point) const;

  /** Removes a function, to clean up absorbed GAP_FILLs. */
  bool RemoveFunction(u32 entry_point);

  /**
   * Returns the function containing an address in O(log f) through the sorted base index, or null
   * when no function contains addr.
   */
  FunctionNode* GetFunctionContaining(u32 addr);
  const FunctionNode* GetFunctionContaining(u32 addr) const;

  bool IsEntryPoint(u32 addr) const;

  /** Reports whether an address is an import, a node with IMPORT authority. */
  bool IsImport(u32 addr) const;

  /** Returns all functions including imports, keyed by entry point. */
  const std::unordered_map<u32, std::unique_ptr<FunctionNode>>& GetFunctions() const {
    return tables_->functions;
  }

  std::vector<FunctionNode*> GetPendingFunctions();

  std::vector<FunctionNode*> GetSealedFunctions();

  size_t GetFunctionCount() const { return tables_->functions.size(); }
  size_t GetPendingCount() const;
  size_t GetSealedCount() const;

  void SetFunctionName(u32 entry, std::string name);

  void SetFunctionHasExceptionHandler(u32 entry, bool val);

  /** Marks a function as an entry of a millicode run. */
  void SetFunctionMillicode(u32 entry, MillicodeEntry millicode);

  void SetFunctionExceptionInfo(u32 entry, ExceptionInfo info);

  void AddBlockToFunction(u32 entry, Block block);

  /** Adds a label, an internal branch target, to a function. */
  void AddLabelToFunction(u32 entry, u32 label);

  void AddCallToFunction(u32 entry, u32 site, CallTarget target);

  void AddTailCallToFunction(u32 entry, u32 site, CallTarget target);

  void AddJumpTableToFunction(u32 entry, JumpTable jt);

  /**
   * Adds an unresolved jump to a function.
   *
   * @param is_call True for bl (call), false for b (tail call)
   * @param conditional True for a conditional branch
   */
  void AddUnresolvedJumpToFunction(u32 entry, u32 site, u32 target, bool is_call, bool conditional);

  /**
   * Tries to resolve all unresolved jumps of a function against known functions, imports and
   * internal labels.
   */
  size_t TryResolveFunction(u32 entry);

  void AbsorbRegionIntoFunction(u32 entry, u32 region_base, u32 region_size);

  /** Seals a function if it can be sealed, returning true when it was. */
  bool TrySealFunction(u32 entry);

  /** Seals all functions that can be sealed and returns how many it sealed. */
  size_t SealAllReady();

  /** Seals all functions and throws if any cannot be sealed, enforcing that discovery is done. */
  void SealAll();

  /** Sets the reader for null dword checks. */
  void SetMemoryReader(MemoryReader reader) { memory_reader_ = std::move(reader); }

  /** Registers a chunk, an address range claimed by config that blocks vacancy. */
  void RegisterChunk(u32 base, u32 size);

  /**
   * Reports whether a region is vacant for absorption.
   *
   * @param from_addr Address being expanded from, used to check for a null boundary
   * @param target_addr Start of the region to absorb
   */
  bool IsVacant(u32 from_addr, u32 target_addr) const;

  /** Reports whether a target is a mergeable entry point, a DISCOVERED function with xrefs. */
  bool IsMergeableEntryPoint(u32 addr) const;

  /** Flags every SEH funclet as register sharing once discovery knows their extents. */
  size_t MarkFuncletRegisterSharing();

  /**
   * Classifies a branch target for code generation.
   *
   * @param is_call_instruction True for bl (expects return), false for b (no return)
   * @return How the target is treated during code generation
   */
  TargetType ClassifyTarget(u32 target, u32 caller_addr, bool is_call_instruction) const;

 private:
  /** The containers, held behind a pointer so moving the graph moves one pointer. */
  struct Tables {
    std::unordered_map<u32, std::unique_ptr<FunctionNode>> functions;
    std::map<u32, FunctionNode*> functions_by_base;
    std::unordered_map<u32, bool> function_has_xrefs;
    std::unordered_set<u32> declared_no_return;
    std::optional<std::unordered_set<u32>> no_return;
  };

  std::unique_ptr<Tables> tables_ = std::make_unique<Tables>();
  std::vector<std::pair<u32, u32>> chunks_;
  MemoryReader memory_reader_;

  void NotifyFunctionAdded(FunctionNode* new_function);
};

}  // namespace rex::codegen
