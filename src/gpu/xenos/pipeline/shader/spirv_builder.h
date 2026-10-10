/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <SPIRV/SpvBuilder.h>

#include <rex/assert.h>
#include <rex/types.h>

namespace rex::gpu {

/** SpvBuilder with extra helpers. */
class SpirvBuilder : public spv::Builder {
 public:
  SpirvBuilder(unsigned int spv_version, unsigned int user_number, spv::SpvBuildLogger* logger)
      : spv::Builder(spv_version, user_number, logger) {}

  using spv::Builder::createSelectionMerge;

  spv::Id createQuadOp(spv::Op op_code, spv::Id type_id, spv::Id operand1, spv::Id operand2,
                       spv::Id operand3, spv::Id operand4);

  spv::Id createNoContractionUnaryOp(spv::Op op_code, spv::Id type_id, spv::Id operand);
  spv::Id createNoContractionBinOp(spv::Op op_code, spv::Id type_id, spv::Id operand1,
                                   spv::Id operand2);

  spv::Id createUnaryBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                                 spv::Id operand);
  spv::Id createBinBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                               spv::Id operand1, spv::Id operand2);
  spv::Id createTriBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                               spv::Id operand1, spv::Id operand2, spv::Id operand3);

  /** Builds nested if-then-else control flow, extending SpvBuilder::If. */
  class IfBuilder : public rex::NonCopyable {
   public:
    IfBuilder(spv::Id condition, unsigned int control, SpirvBuilder& builder,
              unsigned int thenWeight = 0, unsigned int elseWeight = 0);

    ~IfBuilder() {
#ifndef NDEBUG
      REX_ASSERT(currentBranch == Branch::kMerge);
#endif
    }

    void makeBeginElse(bool branchToMerge = true);
    void makeEndIf(bool branchToMerge = true);

    spv::Id getThenPhiParent() const { return thenPhiParent; }
    spv::Id getElsePhiParent() const { return elsePhiParent; }

    spv::Id createMergePhi(spv::Id then_variable, spv::Id else_variable) const;

   private:
    enum class Branch : u8 {
      kThen,
      kElse,
      kMerge,
    };

    SpirvBuilder& builder;
    spv::Id condition;
    unsigned int control;
    unsigned int thenWeight;
    unsigned int elseWeight;

    spv::Function& function;

    spv::Block* headerBlock;
    spv::Block* thenBlock;
    spv::Block* elseBlock;
    spv::Block* mergeBlock;

    spv::Id thenPhiParent;
    spv::Id elsePhiParent;

#ifndef NDEBUG
    Branch currentBranch = Branch::kThen;
#endif
  };

  /** Switch builder allowing multiple cases to share a block, unlike makeSwitch. */
  class SwitchBuilder {
   public:
    SwitchBuilder(spv::Id selector, unsigned int selection_control, SpirvBuilder& builder);
    ~SwitchBuilder() { REX_ASSERT(current_branch_ == Branch::kMerge); }

    void makeBeginDefault();
    void makeBeginCase(unsigned int literal);
    void addCurrentCaseLiteral(unsigned int literal);
    void makeEndSwitch();

    spv::Id getDefaultPhiParent() const { return default_phi_parent_; }

   private:
    enum class Branch : u8 {
      kSelection,
      kDefault,
      kCase,
      kMerge,
    };

    void endSegment();

    SpirvBuilder& builder_;
    spv::Id selector_;
    unsigned int selection_control_;

    spv::Function& function_;

    spv::Block* header_block_;
    spv::Block* merge_block_;
    spv::Block* default_block_ = nullptr;

    std::vector<std::pair<unsigned int, spv::Id>> cases_;

    spv::Id default_phi_parent_;

    Branch current_branch_ = Branch::kSelection;
  };
};

}  // namespace rex::gpu
