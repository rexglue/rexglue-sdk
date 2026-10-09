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

#include <rex/codegen/function_types.h>

namespace rex::codegen {

/**
 * Core object representing a function in the graph. Manages its own state transitions and
 * resolution. Only FunctionGraph mutates it, and the public accessors are read only.
 */
class FunctionNode {
  friend class FunctionGraph;

 public:
  /**
   * Creates a registered node named after its address, such as sub_82010000.
   *
   * @param base Entry point address
   * @param size Size in bytes
   * @param authority Authority of the registration
   */
  FunctionNode(uint32_t base, uint32_t size, FunctionAuthority authority);

  uint32_t base() const { return base_; }
  uint32_t size() const { return size_; }
  uint32_t end() const { return base_ + size_; }
  const std::string& name() const { return name_; }

  /** Returns the cached pointer to the instruction bytes. */
  const uint8_t* code() const { return code_; }
  bool hasCode() const { return code_ != nullptr; }

  FunctionAuthority authority() const { return authority_; }
  FunctionState state() const { return state_; }

  bool isRegistered() const { return state_ == FunctionState::kRegistered; }
  bool isDiscovered() const { return state_ == FunctionState::kDiscovered; }
  bool isSealed() const { return state_ == FunctionState::kSealed; }

  /**
   * Reports whether the function is PENDING, which covers kRegistered and kDiscovered.
   *
   * @return True when not sealed
   */
  bool isPending() const { return state_ != FunctionState::kSealed; }

  bool isImport() const { return authority_ == FunctionAuthority::IMPORT; }
  bool isHelper() const { return authority_ == FunctionAuthority::HELPER; }

  /**
   * Reports whether the function can transition from kRegistered to kDiscovered.
   *
   * @return True when state is kRegistered
   */
  bool canDiscover() const { return state_ == FunctionState::kRegistered; }

  /**
   * Transitions kRegistered to kDiscovered with blocks and instructions.
   * Requires canDiscover() and, for non imports, non empty blocks.
   *
   * @param blocks Discovered blocks
   * @param instructions Instructions of the function
   * @param labels Internal branch target addresses
   */
  void discover(std::vector<Block> blocks,
                std::vector<rex::codegen::ppc::Instruction*> instructions,
                std::set<uint32_t> labels);

  /** Transitions kRegistered to kDiscovered for an import function, which has no blocks. */
  void discoverAsImport();

  /**
   * Reports whether the function can transition from kDiscovered to kSealed.
   * Imports always can. Other functions need non empty blocks and no unresolved branches.
   *
   * @return True when state is kDiscovered and the conditions hold
   */
  bool canSeal() const;

  /** Transitions kDiscovered to kSealed, computing FunctionAnalysis and sorting blocks. */
  void seal();

  /**
   * Returns the analysis result. Valid only after seal().
   *
   * @return Analysis of the sealed function
   */
  const FunctionAnalysis& analysis() const;

  /**
   * Emits C++ code for this function. Requires state() == kSealed. Imports emit REX_IMPORT and
   * normal functions emit REX_FUNC with blocks and instructions.
   *
   * @param ctx Emit context
   * @return Generated C++ source
   */
  std::string emitCpp(const EmitContext& ctx) const;

  /**
   * Returns the owned instructions, which point into DecodedBinary. Valid after discover().
   *
   * @return Instruction pointers
   */
  std::span<rex::codegen::ppc::Instruction* const> instructions() const { return instructions_; }

  const std::vector<Block>& blocks() const { return blocks_; }
  /**
   * Reports whether an address belongs to the function. Gaps between blocks match only for CONFIG
   * and PDATA functions.
   *
   * @param addr Address to test
   * @return True when addr is inside the function
   */
  bool containsAddress(uint32_t addr) const;

  /**
   * Reports whether an address is within the overall function bounds, ignoring blocks. Used for
   * branch target detection where the address may be in a gap between blocks.
   *
   * @param addr Address to test
   * @return True when addr is within bounds
   */
  bool isWithinBounds(uint32_t addr) const { return addr >= base_ && addr < base_ + size_; }

  /** Returns the internal branch targets of this function. */
  const std::set<uint32_t>& labels() const { return labels_; }
  bool isLabel(uint32_t addr) const { return labels_.contains(addr); }

  /** Returns the resolved calls, the bl instructions. */
  const std::vector<CallEdge>& calls() const { return calls_; }

  /** Returns the resolved tail calls, the b instructions to other functions. */
  const std::vector<CallEdge>& tailCalls() const { return tailCalls_; }

  const std::vector<JumpTable>& jumpTables() const { return jumpTables_; }

  const std::vector<UnresolvedJump>& unresolvedJumps() const { return unresolvedJumps_; }

  bool hasUnresolvedJumps() const { return !unresolvedJumps_.empty(); }

  bool hasExceptionHandler() const { return hasExceptionHandler_; }

  /** Returns the SEH or C++ EH info. */
  const std::optional<ExceptionInfo>& exceptionInfo() const { return exceptionInfo_; }
  bool hasExceptionInfo() const { return exceptionInfo_.has_value() && exceptionInfo_->hasInfo(); }

  void setName(std::string name) { name_ = std::move(name); }

  /**
   * Reports whether this is an SEH funclet. Its non volatiles stay in ctx so it sees what its
   * owner left live, and callers sync their localized copies around the call.
   *
   * @return True for a register sharing funclet
   */
  bool sharesRegisters() const { return sharesRegisters_; }
  void setSharesRegisters(bool val) { sharesRegisters_ = val; }

 private:
  void setCode(const uint8_t* ptr) { code_ = ptr; }
  void setHasExceptionHandler(bool val) { hasExceptionHandler_ = val; }
  void setExceptionInfo(ExceptionInfo info) { exceptionInfo_ = std::move(info); }

  void addBlock(Block block);
  void addLabel(uint32_t addr);

  void addCall(uint32_t site, CallTarget target);
  void addTailCall(uint32_t site, CallTarget target);
  void addJumpTable(JumpTable jt);
  void addUnresolvedJump(uint32_t site, uint32_t target, bool isCall, bool conditional);

  bool tryResolveAgainst(FunctionNode* newFunction);
  bool tryResolveAgainstImport(uint32_t importAddr, const std::string& importName);
  bool tryResolveAsInternalLabel(uint32_t target);

  void absorbRegion(uint32_t regionBase, uint32_t regionSize);

  void removeUnresolvedJump(uint32_t site);

 private:
  uint32_t base_;
  uint32_t size_;
  std::string name_;
  const uint8_t* code_ = nullptr;
  FunctionAuthority authority_;
  FunctionState state_ = FunctionState::kRegistered;
  bool hasExceptionHandler_ = false;
  bool sharesRegisters_ = false;

  std::vector<Block> blocks_;
  std::vector<rex::codegen::ppc::Instruction*> instructions_; /**< Point into DecodedBinary */
  std::set<uint32_t> labels_;

  std::vector<CallEdge> calls_;
  std::vector<CallEdge> tailCalls_;
  std::vector<JumpTable> jumpTables_;

  std::vector<UnresolvedJump> unresolvedJumps_;

  std::optional<ExceptionInfo> exceptionInfo_;

  std::optional<FunctionAnalysis> analysis_;
};

}  // namespace rex::codegen
