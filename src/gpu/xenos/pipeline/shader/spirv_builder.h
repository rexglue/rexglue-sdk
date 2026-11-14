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
#include <rex/utility.h>

namespace rex::gpu {

class SPIRVBuilder : public spv::Builder {
 public:
  SPIRVBuilder(unsigned int spv_version, unsigned int user_number,
               spv::SpvBuildLogger* build_logger)
      : spv::Builder(spv_version, user_number, build_logger) {}

  using spv::Builder::createSelectionMerge;

  spv::Id CreateQuadOp(spv::Op op_code, spv::Id type_id, spv::Id operand1, spv::Id operand2,
                       spv::Id operand3, spv::Id operand4);

  spv::Id CreateNoContractionUnaryOp(spv::Op op_code, spv::Id type_id, spv::Id operand);
  spv::Id CreateNoContractionBinOp(spv::Op op_code, spv::Id type_id, spv::Id operand1,
                                   spv::Id operand2);

  spv::Id CreateUnaryBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                                 spv::Id operand);
  spv::Id CreateBinBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                               spv::Id operand1, spv::Id operand2);
  spv::Id CreateTriBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                               spv::Id operand1, spv::Id operand2, spv::Id operand3);

  /** Builds nested if-then-else control flow, extending SpvBuilder::If. */
  class IfBuilder : public rex::NonCopyable {
   public:
    IfBuilder(spv::Id condition, unsigned int control, SPIRVBuilder& builder,
              unsigned int then_weight = 0, unsigned int else_weight = 0);

    ~IfBuilder() {
#ifndef NDEBUG
      REX_ASSERT(current_branch_ == Branch::kMerge);
#endif
    }

    void MakeBeginElse(bool branch_to_merge = true);
    void MakeEndIf(bool branch_to_merge = true);

    spv::Id GetThenPhiParent() const { return then_phi_parent_; }
    spv::Id GetElsePhiParent() const { return else_phi_parent_; }

    spv::Id CreateMergePhi(spv::Id then_variable, spv::Id else_variable) const;

   private:
    enum class Branch : u8 {
      kThen,
      kElse,
      kMerge,
    };

    SPIRVBuilder& builder_;
    spv::Id condition_;
    unsigned int control_;
    unsigned int then_weight_;
    unsigned int else_weight_;

    spv::Function& function_;

    spv::Block* header_block_;
    spv::Block* then_block_;
    spv::Block* else_block_;
    spv::Block* merge_block_;

    spv::Id then_phi_parent_;
    spv::Id else_phi_parent_;

#ifndef NDEBUG
    Branch current_branch_ = Branch::kThen;
#endif
  };

  /** Switch builder allowing multiple cases to share a block, unlike makeSwitch. */
  class SwitchBuilder {
   public:
    SwitchBuilder(spv::Id selector, unsigned int selection_control, SPIRVBuilder& builder);
    ~SwitchBuilder() { REX_ASSERT(current_branch_ == Branch::kMerge); }

    void MakeBeginDefault();
    void MakeBeginCase(unsigned int literal);
    void AddCurrentCaseLiteral(unsigned int literal);
    void MakeEndSwitch();

    spv::Id GetDefaultPhiParent() const { return default_phi_parent_; }

   private:
    enum class Branch : u8 {
      kSelection,
      kDefault,
      kCase,
      kMerge,
    };

    void EndSegment();

    SPIRVBuilder& builder_;
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
