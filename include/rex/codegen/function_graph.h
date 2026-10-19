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
#include <rex/types.h>

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
class FunctionGraph : public rex::MoveOnly {
 public:
  FunctionGraph() = default;

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
   * @param has_xrefs True when this is a known call target such as a bl target
   * @return The new node, or the existing node when present
   */
  FunctionNode* AddFunction(u32 base, u32 size, FunctionAuthority authority,
                            bool has_xrefs = false);

  /**
   * Adds a named function to the graph.
   *
   * @param base Entry point address
   * @param size Size in bytes
   * @param authority Authority of the registration
   * @param name Function name
   * @param has_xrefs True when this is a known call target
   * @return The new node, or the existing node when present
   */
  FunctionNode* AddFunction(u32 base, u32 size, FunctionAuthority authority, std::string_view name,
                            bool has_xrefs = false);

  /**
   * Adds a resolved import as a callable function with an __imp__ name.
   *
   * @param address Thunk address that bl instructions target
   * @param resolved_name Resolved import name
   * @param size Size of the thunk in bytes
   * @return The new node
   */
  FunctionNode* AddImportFunction(u32 address, std::string_view resolved_name, u32 size = 4);

  /**
   * Declares that a call to an address never returns, as for an import that ends the thread or a
   * longjmp routine.
   *
   * @param address Entry point address
   */
  void DeclareNoReturn(u32 address);

  /**
   * Reports whether an address was declared as never returning.
   *
   * @param address Entry point address
   * @return True after DeclareNoReturn for the address
   */
  bool IsDeclaredNoReturn(u32 address) const {
    return tables_->declared_no_return.contains(address);
  }

  /**
   * Reports whether a function never returns to its caller. A declared address never returns.
   * Any other function never returns when no path from its entry reaches a return, an
   * unresolved target, or a function that returns, where a path ends at a trap, an infinite loop
   * or a call that never returns. The set is a fixed point over the graph, so a cycle with no
   * exit to a return counts as never returning, and an unknown address as returning.
   *
   * @param entry Entry point address
   * @return True when no path leads back to the caller
   */
  bool IsNoReturn(u32 entry) const;

  /**
   * Returns the function at an entry point in O(1).
   *
   * @param entry_point Entry point address
   * @return The node, or null when absent
   */
  FunctionNode* GetFunction(u32 entry_point);
  /**
   * Returns the function at an entry point in O(1).
   *
   * @param entry_point Entry point address
   * @return The node, or null when absent
   */
  const FunctionNode* GetFunction(u32 entry_point) const;

  /**
   * Removes a function from the graph, used to clean up absorbed GAP_FILLs.
   *
   * @param entry_point Entry point address
   * @return True when a function was removed
   */
  bool RemoveFunction(u32 entry_point);

  /**
   * Returns the function containing an address in O(log f) through the sorted base index.
   *
   * @param addr Address to look up
   * @return The node, or null when no function contains addr
   */
  FunctionNode* GetFunctionContaining(u32 addr);
  /**
   * Returns the function containing an address in O(log f) through the sorted base index.
   *
   * @param addr Address to look up
   * @return The node, or null when no function contains addr
   */
  const FunctionNode* GetFunctionContaining(u32 addr) const;

  /**
   * Reports whether an address is a known entry point.
   *
   * @param addr Address to test
   * @return True when a function starts at addr
   */
  bool IsEntryPoint(u32 addr) const;

  /**
   * Reports whether an address is an import, a FunctionNode with IMPORT authority.
   *
   * @param addr Address to test
   * @return True when addr is an import
   */
  bool IsImport(u32 addr) const;

  /**
   * Returns all functions including imports with IMPORT authority.
   *
   * @return Functions keyed by entry point
   */
  const std::unordered_map<u32, std::unique_ptr<FunctionNode>>& GetFunctions() const {
    return tables_->functions;
  }

  /**
   * Returns all PENDING functions.
   *
   * @return Pending nodes
   */
  std::vector<FunctionNode*> GetPendingFunctions();

  /**
   * Returns all SEALED functions.
   *
   * @return Sealed nodes
   */
  std::vector<FunctionNode*> GetSealedFunctions();

  /** Returns the number of functions. */
  size_t GetFunctionCount() const { return tables_->functions.size(); }
  /**
   * Counts the functions that are not sealed.
   *
   * @return Number of PENDING functions
   */
  size_t GetPendingCount() const;
  /**
   * Counts the sealed functions.
   *
   * @return Number of SEALED functions
   */
  size_t GetSealedCount() const;

  /**
   * Sets a function name.
   *
   * @param entry Entry point address
   * @param name Function name
   */
  void SetFunctionName(u32 entry, std::string name);

  /**
   * Sets the exception handler flag.
   *
   * @param entry Entry point address
   * @param val Flag value
   */
  void SetFunctionHasExceptionHandler(u32 entry, bool val);

  /**
   * Marks a function as an entry of a millicode run.
   *
   * @param entry Entry point address
   * @param millicode Kind and register of the entry
   */
  void SetFunctionMillicode(u32 entry, MillicodeEntry millicode);

  /**
   * Sets the parsed SEH or C++ EH info.
   *
   * @param entry Entry point address
   * @param info Parsed exception info
   */
  void SetFunctionExceptionInfo(u32 entry, ExceptionInfo info);

  /**
   * Adds a block to a function.
   *
   * @param entry Entry point address
   * @param block Block to add
   */
  void AddBlockToFunction(u32 entry, Block block);

  /**
   * Adds a label, an internal branch target, to a function.
   *
   * @param entry Entry point address
   * @param label Label address
   */
  void AddLabelToFunction(u32 entry, u32 label);

  /**
   * Adds a resolved call to a function.
   *
   * @param entry Entry point address
   * @param site Call site address
   * @param target Resolved target
   */
  void AddCallToFunction(u32 entry, u32 site, CallTarget target);

  /**
   * Adds a resolved tail call to a function.
   *
   * @param entry Entry point address
   * @param site Call site address
   * @param target Resolved target
   */
  void AddTailCallToFunction(u32 entry, u32 site, CallTarget target);

  /**
   * Adds a jump table to a function.
   *
   * @param entry Entry point address
   * @param jt Jump table to add
   */
  void AddJumpTableToFunction(u32 entry, JumpTable jt);

  /**
   * Adds an unresolved jump to a function.
   *
   * @param entry Entry point address
   * @param site Jump site address
   * @param target Jump target address
   * @param is_call True for bl (call), false for b (tail call)
   * @param conditional True for a conditional branch
   */
  void AddUnresolvedJumpToFunction(u32 entry, u32 site, u32 target, bool is_call, bool conditional);

  /**
   * Tries to resolve all unresolved jumps of a function against known functions, imports and
   * internal labels.
   *
   * @param entry Entry point address
   * @return Number of jumps resolved
   */
  size_t TryResolveFunction(u32 entry);

  /**
   * Absorbs a region into a function for vacancy expansion.
   *
   * @param entry Entry point address
   * @param region_base Region start address
   * @param region_size Region size in bytes
   */
  void AbsorbRegionIntoFunction(u32 entry, u32 region_base, u32 region_size);

  /**
   * Seals a function if it can be sealed.
   *
   * @param entry Entry point address
   * @return True when the function was sealed
   */
  bool TrySealFunction(u32 entry);

  /**
   * Seals all functions that can be sealed.
   *
   * @return Number of functions sealed
   */
  size_t SealAllReady();

  /** Seals all functions and throws if any cannot be sealed, enforcing that discovery is done. */
  void SealAll();

  /**
   * Sets the memory reader for null dword checking.
   *
   * @param reader Reader returning the dword at an address
   */
  void SetMemoryReader(MemoryReader reader) { memory_reader_ = std::move(reader); }

  /**
   * Registers a chunk, an address range claimed by config that blocks vacancy.
   *
   * @param base Range start address
   * @param size Range size in bytes
   */
  void RegisterChunk(u32 base, u32 size);

  /**
   * Reports whether a region is vacant for absorption.
   *
   * @param from_addr Address being expanded from, used to check for a null boundary
   * @param target_addr Start of the region to absorb
   * @return True when the region is vacant
   */
  bool IsVacant(u32 from_addr, u32 target_addr) const;

  /**
   * Reports whether a target is a mergeable entry point, a DISCOVERED function with xrefs.
   *
   * @param addr Address to test
   * @return True when addr is mergeable
   */
  bool IsMergeableEntryPoint(u32 addr) const;

  /**
   * Flags every SEH funclet as register sharing. Runs after discovery, once funclet extents are
   * known.
   *
   * @return Number of funclets flagged
   */
  size_t MarkFuncletRegisterSharing();

  /**
   * Classifies a branch target for code generation.
   *
   * @param target Address being branched to
   * @param caller_addr Address of the branch instruction
   * @param is_call_instruction True for bl (expects return), false for b (no return)
   * @return How the target is treated during code generation
   */
  TargetKind ClassifyTarget(u32 target, u32 caller_addr, bool is_call_instruction) const;

 private:
  /** The containers, held behind a pointer so that moving the graph moves a pointer. */
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
