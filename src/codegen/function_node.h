/**
 * @file        codegen/function_node.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <set>
#include <utility>

#include "function_types.h"

namespace rex::codegen {

/** A function in the graph, which manages its own state transitions and resolution. */
class FunctionNode {
  friend class FunctionGraph;

 public:
  /** Creates a registered node named after its address, such as sub_82010000. */
  FunctionNode(u32 base, u32 size, FunctionAuthority authority);

  u32 GetBase() const { return base_; }
  u32 GetSize() const { return size_; }
  /** Returns the address 1 past the function. */
  u32 GetEnd() const { return base_ + size_; }
  const std::string& GetName() const { return name_; }

  /** Returns the cached pointer to the instruction bytes. */
  const u8* GetCode() const { return code_; }
  bool HasCode() const { return code_ != nullptr; }

  FunctionAuthority GetAuthority() const { return authority_; }
  FunctionState GetState() const { return state_; }

  bool IsRegistered() const { return state_ == FunctionState::kRegistered; }
  bool IsDiscovered() const { return state_ == FunctionState::kDiscovered; }
  bool IsSealed() const { return state_ == FunctionState::kSealed; }

  /** Reports whether the function is PENDING, which covers kRegistered and kDiscovered. */
  bool IsPending() const { return state_ != FunctionState::kSealed; }

  bool IsImport() const { return authority_ == FunctionAuthority::kImport; }
  /** Reports whether the function is a save or restore helper. */
  bool IsHelper() const { return authority_ == FunctionAuthority::kHelper; }

  bool CanDiscover() const { return state_ == FunctionState::kRegistered; }

  /**
   * Transitions kRegistered to kDiscovered. Requires CanDiscover() and, for non imports, non
   * empty blocks.
   *
   * @param labels Internal branch target addresses
   */
  void Discover(std::vector<Block> blocks,
                std::vector<rex::codegen::detail::Instruction*> instructions, std::set<u32> labels);

  /** Transitions kRegistered to kDiscovered for an import function, which has no blocks. */
  void DiscoverAsImport();

  /**
   * Reports whether the function can transition from kDiscovered to kSealed. Imports always can.
   * Other functions need non empty blocks and no unresolved branches.
   */
  bool CanSeal() const;

  /** Transitions kDiscovered to kSealed, sorting blocks. */
  void Seal();

  /**
   * Emits C++ code for this function. Requires GetState() == kSealed. Imports emit REX_IMPORT and
   * normal functions emit REX_FUNC with blocks and instructions.
   *
   * @return Generated C++ source
   */
  std::string EmitCpp(const EmitContext& ctx) const;

  /** Returns the instructions, which point into DecodedBinary. Valid after Discover(). */
  std::span<rex::codegen::detail::Instruction* const> GetInstructions() const {
    return instructions_;
  }

  const std::vector<Block>& GetBlocks() const { return blocks_; }
  /**
   * Reports whether an address belongs to the function. Gaps between blocks match only for CONFIG
   * and PDATA functions.
   */
  bool ContainsAddress(u32 addr) const;

  /**
   * Reports whether an address is within the overall function bounds, ignoring blocks. Used for
   * branch target detection where the address may be in a gap between blocks.
   *
   * @return True when addr is within bounds
   */
  bool IsWithinBounds(u32 addr) const { return addr >= base_ && addr < base_ + size_; }

  /** Returns the internal branch targets. */
  const std::set<u32>& GetLabels() const { return labels_; }
  bool IsLabel(u32 addr) const { return labels_.contains(addr); }

  /** Returns the resolved calls, the bl instructions. */
  const std::vector<CallEdge>& GetCalls() const { return calls_; }

  /** Returns the resolved tail calls, the b instructions to other functions. */
  const std::vector<CallEdge>& GetTailCalls() const { return tail_calls_; }

  /** Returns the entries of the functions that its blocks run into without a terminator. */
  const std::set<u32>& GetFallThroughs() const { return fall_throughs_; }

  void SetFallThroughs(std::set<u32> targets) { fall_throughs_ = std::move(targets); }

  const std::vector<JumpTable>& GetJumpTables() const { return jump_tables_; }

  /** Returns the jumps awaiting resolution. */
  const std::vector<UnresolvedJump>& GetUnresolvedJumps() const { return unresolved_jumps_; }

  bool HasUnresolvedJumps() const { return !unresolved_jumps_.empty(); }

  /** Reports whether the .pdata entry of the function is of the exception handler type. */
  bool HasExceptionHandler() const { return has_exception_handler_; }

  /** Returns the millicode role, or no value when the function is not a millicode entry. */
  const std::optional<MillicodeEntry>& GetMillicode() const { return millicode_; }

  /** Returns the SEH or C++ EH info. */
  const std::optional<ExceptionInfo>& GetExceptionInfo() const { return exception_data_; }
  bool HasExceptionInfo() const {
    return exception_data_.has_value() && exception_data_->HasInfo();
  }

  void SetName(std::string name) { name_ = std::move(name); }

  /**
   * Reports whether this is an SEH funclet. Its non volatiles stay in ctx so it sees what its
   * owner left live.
   */
  bool IsRegisterSharing() const { return shares_registers_; }
  void SetSharesRegisters(bool val) { shares_registers_ = val; }

 private:
  void SetCode(const u8* ptr) { code_ = ptr; }
  void SetHasExceptionHandler(bool val) { has_exception_handler_ = val; }
  void SetMillicode(MillicodeEntry entry) { millicode_ = entry; }

  void SetExceptionInfo(ExceptionInfo info) { exception_data_ = std::move(info); }

  void AddBlock(Block block);
  void AddLabel(u32 addr);

  void AddCall(u32 site, CallTarget target);
  void AddTailCall(u32 site, CallTarget target);
  void AddJumpTable(JumpTable jt);
  void AddUnresolvedJump(u32 site, u32 target, bool is_call, bool conditional);

  bool TryResolveAgainst(FunctionNode* new_function);
  bool TryResolveAgainstImport(u32 import_addr, const std::string& import_name);
  bool TryResolveAsInternalLabel(u32 target);

  void AbsorbRegion(u32 region_base, u32 region_size);

  void RemoveUnresolvedJump(u32 site);

  u32 base_;
  u32 size_;
  std::string name_;
  const u8* code_ = nullptr;
  FunctionAuthority authority_;
  FunctionState state_ = FunctionState::kRegistered;
  bool has_exception_handler_ = false;
  bool shares_registers_ = false;

  std::vector<Block> blocks_;
  std::vector<rex::codegen::detail::Instruction*> instructions_;
  std::set<u32> labels_;

  std::vector<CallEdge> calls_;
  std::vector<CallEdge> tail_calls_;
  std::set<u32> fall_throughs_;
  std::vector<JumpTable> jump_tables_;

  std::vector<UnresolvedJump> unresolved_jumps_;

  std::optional<ExceptionInfo> exception_data_;
  std::optional<MillicodeEntry> millicode_;
};

}  // namespace rex::codegen
