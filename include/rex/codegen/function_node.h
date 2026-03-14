/**
 * @file        rex/codegen/function_node.h
 * @brief       FunctionNode - core object representing a function in the graph
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/codegen/function_types.h>

namespace rex::codegen {

//=============================================================================
// Function Node
//=============================================================================
// Core object representing a function in the graph.
// Manages its own state transitions and resolution.

class FunctionNode {
  friend class FunctionGraph;  // Graph manages all mutations

 public:
  FunctionNode(uint32_t base, uint32_t size, FunctionAuthority authority);

  //=========================================================================
  // Read-only accessors - external code can inspect but not modify
  //=========================================================================

  // Identity
  uint32_t base() const { return base_; }
  uint32_t size() const { return size_; }
  uint32_t end() const { return base_ + size_; }
  const std::string& name() const { return name_; }

  // Code access - cached pointer to instruction bytes
  const uint8_t* code() const { return code_; }
  bool hasCode() const { return code_ != nullptr; }

  // Authority and state
  FunctionAuthority authority() const { return authority_; }
  FunctionState state() const { return state_; }

  // State queries
  bool isRegistered() const { return state_ == FunctionState::kRegistered; }
  bool isDiscovered() const { return state_ == FunctionState::kDiscovered; }
  bool isSealed() const { return state_ == FunctionState::kSealed; }

  // Legacy aliases (PENDING maps to kRegistered OR kDiscovered - not sealed)
  bool isPending() const { return state_ != FunctionState::kSealed; }

  // Special type checks
  bool isImport() const { return authority_ == FunctionAuthority::IMPORT; }
  bool isHelper() const { return authority_ == FunctionAuthority::HELPER; }

  //=========================================================================
  // State Machine - New 3-state model
  //=========================================================================

  /// Can transition from kRegistered to kDiscovered?
  bool canDiscover() const { return state_ == FunctionState::kRegistered; }

  /// Transition kRegistered -> kDiscovered with blocks and instructions
  /// Precondition: canDiscover() returns true
  /// For non-imports: blocks must not be empty
  void discover(std::vector<Block> blocks,
                std::vector<rex::codegen::ppc::Instruction*> instructions,
                std::set<uint32_t> labels);

  /// Transition kRegistered -> kDiscovered for import functions (no blocks)
  void discoverAsImport();

  /// Can transition from kDiscovered to kSealed?
  /// Returns true if:
  /// - state == kDiscovered
  /// - imports: always OK (no blocks required)
  /// - non-imports: blocks not empty AND no unresolved branches
  bool canSeal() const;

  /// Transition kDiscovered -> kSealed
  /// Computes FunctionAnalysis and sorts blocks
  void seal();

  /// Get analysis result (only valid after seal)
  const FunctionAnalysis& analysis() const;

  //=========================================================================
  // Code Emission (valid after seal)
  //=========================================================================

  /// Emit C++ code for this function
  /// Requires: state() == kSealed
  /// For imports: emits PPC_IMPORT macro
  /// For normal functions: emits PPC_FUNC with blocks and instructions
  std::string emitCpp(const EmitContext& ctx) const;

  //=========================================================================
  // Instruction access (valid after discover)
  //=========================================================================

  /// Get owned instructions (pointers into DecodedBinary)
  std::span<rex::codegen::ppc::Instruction* const> instructions() const { return instructions_; }

  // Blocks
  const std::vector<Block>& blocks() const { return blocks_; }
  bool containsAddress(uint32_t addr) const;

  // Check if address is within overall function bounds (ignores blocks)
  // Use this for branch target detection where address may be in a gap between blocks
  bool isWithinBounds(uint32_t addr) const { return addr >= base_ && addr < base_ + size_; }

  // Labels (internal branch targets within this function)
  const std::set<uint32_t>& labels() const { return labels_; }
  bool isLabel(uint32_t addr) const { return labels_.contains(addr); }

  // Resolved calls (bl instructions)
  const std::vector<CallEdge>& calls() const { return calls_; }

  // Resolved tail calls (b instructions to other functions)
  const std::vector<CallEdge>& tailCalls() const { return tailCalls_; }

  // Jump tables
  const std::vector<JumpTable>& jumpTables() const { return jumpTables_; }

  // Unresolved jumps (pending resolution)
  const std::vector<UnresolvedJump>& unresolvedJumps() const { return unresolvedJumps_; }

  // Sealing state (legacy - use canSeal() with new semantics)
  bool hasUnresolvedJumps() const { return !unresolvedJumps_.empty(); }

  // Validation
  bool hasExceptionHandler() const { return hasExceptionHandler_; }

  // Exception info (SEH or C++ EH)
  const std::optional<ExceptionInfo>& exceptionInfo() const { return exceptionInfo_; }
  bool hasExceptionInfo() const { return exceptionInfo_.has_value() && exceptionInfo_->hasInfo(); }

  void setName(std::string name) { name_ = std::move(name); }

 private:
  //=========================================================================
  // Mutation methods - only FunctionGraph can call these
  //=========================================================================

  void setCode(const uint8_t* ptr) { code_ = ptr; }
  void setHasExceptionHandler(bool val) { hasExceptionHandler_ = val; }
  void setExceptionInfo(ExceptionInfo info) { exceptionInfo_ = std::move(info); }

  // Block/label management
  void addBlock(Block block);
  void addLabel(uint32_t addr);

  // Call tracking
  void addCall(uint32_t site, CallTarget target);
  void addTailCall(uint32_t site, CallTarget target);
  void addJumpTable(JumpTable jt);
  void addUnresolvedJump(uint32_t site, uint32_t target, bool isCall, bool conditional);

  // Resolution (reactive - called by graph on events)
  bool tryResolveAgainst(FunctionNode* newFunction);
  bool tryResolveAgainstImport(uint32_t importAddr, const std::string& importName);
  bool tryResolveAsInternalLabel(uint32_t target);

  // Expansion
  void absorbRegion(uint32_t regionBase, uint32_t regionSize);

  // Internal helper
  void removeUnresolvedJump(uint32_t site);

  //=========================================================================
  // State
  //=========================================================================
 private:
  uint32_t base_;
  uint32_t size_;
  std::string name_;
  const uint8_t* code_ = nullptr;  // Cached pointer to instruction bytes
  FunctionAuthority authority_;
  FunctionState state_ = FunctionState::kRegistered;
  bool hasExceptionHandler_ = false;

  // Populated at discover()
  std::vector<Block> blocks_;
  std::vector<rex::codegen::ppc::Instruction*> instructions_;  // Pointers into DecodedBinary
  std::set<uint32_t> labels_;  // Branch targets within this function

  std::vector<CallEdge> calls_;
  std::vector<CallEdge> tailCalls_;
  std::vector<JumpTable> jumpTables_;

  std::vector<UnresolvedJump> unresolvedJumps_;

  std::optional<ExceptionInfo> exceptionInfo_;

  // Computed at seal()
  std::optional<FunctionAnalysis> analysis_;
};

}  // namespace rex::codegen
