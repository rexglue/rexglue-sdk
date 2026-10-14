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

#include <memory>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include "gpu/xenos/pipeline/shader/spirv_builder.h"

namespace rex::gpu {

spv::Id SPIRVBuilder::CreateQuadOp(spv::Op op_code, spv::Id type_id, spv::Id operand1,
                                   spv::Id operand2, spv::Id operand3, spv::Id operand4) {
  if (generatingOpCodeForSpecConst) {
    std::vector<spv::Id> operands(4);
    operands[0] = operand1;
    operands[1] = operand2;
    operands[2] = operand3;
    operands[3] = operand4;
    return createSpecConstantOp(op_code, type_id, operands, std::vector<spv::Id>());
  }
  std::unique_ptr<spv::Instruction> op =
      std::make_unique<spv::Instruction>(getUniqueId(), type_id, op_code);
  op->addIdOperand(operand1);
  op->addIdOperand(operand2);
  op->addIdOperand(operand3);
  op->addIdOperand(operand4);
  spv::Id result = op->getResultId();
  buildPoint->addInstruction(std::move(op));
  return result;
}

spv::Id SPIRVBuilder::CreateNoContractionUnaryOp(spv::Op op_code, spv::Id type_id,
                                                 spv::Id operand) {
  spv::Id result = createUnaryOp(op_code, type_id, operand);
  addDecoration(result, spv::DecorationNoContraction);
  return result;
}

spv::Id SPIRVBuilder::CreateNoContractionBinOp(spv::Op op_code, spv::Id type_id, spv::Id operand1,
                                               spv::Id operand2) {
  spv::Id result = createBinOp(op_code, type_id, operand1, operand2);
  addDecoration(result, spv::DecorationNoContraction);
  return result;
}

spv::Id SPIRVBuilder::CreateUnaryBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                                             spv::Id operand) {
  std::unique_ptr<spv::Instruction> instruction =
      std::make_unique<spv::Instruction>(getUniqueId(), result_type, spv::OpExtInst);
  instruction->addIdOperand(builtins);
  instruction->addImmediateOperand(entry_point);
  instruction->addIdOperand(operand);
  spv::Id result = instruction->getResultId();
  getBuildPoint()->addInstruction(std::move(instruction));
  return result;
}

spv::Id SPIRVBuilder::CreateBinBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                                           spv::Id operand1, spv::Id operand2) {
  std::unique_ptr<spv::Instruction> instruction =
      std::make_unique<spv::Instruction>(getUniqueId(), result_type, spv::OpExtInst);
  instruction->addIdOperand(builtins);
  instruction->addImmediateOperand(entry_point);
  instruction->addIdOperand(operand1);
  instruction->addIdOperand(operand2);
  spv::Id result = instruction->getResultId();
  getBuildPoint()->addInstruction(std::move(instruction));
  return result;
}

spv::Id SPIRVBuilder::CreateTriBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                                           spv::Id operand1, spv::Id operand2, spv::Id operand3) {
  std::unique_ptr<spv::Instruction> instruction =
      std::make_unique<spv::Instruction>(getUniqueId(), result_type, spv::OpExtInst);
  instruction->addIdOperand(builtins);
  instruction->addImmediateOperand(entry_point);
  instruction->addIdOperand(operand1);
  instruction->addIdOperand(operand2);
  instruction->addIdOperand(operand3);
  spv::Id result = instruction->getResultId();
  getBuildPoint()->addInstruction(std::move(instruction));
  return result;
}

SPIRVBuilder::IfBuilder::IfBuilder(spv::Id condition, unsigned int control, SPIRVBuilder& builder,
                                   unsigned int then_weight, unsigned int else_weight)
    : builder_(builder),
      condition_(condition),
      control_(control),
      then_weight_(then_weight),
      else_weight_(else_weight),
      function_(builder.getBuildPoint()->getParent()) {
  then_block_ = new spv::Block(builder.getUniqueId(), function_);
  else_block_ = nullptr;
  merge_block_ = new spv::Block(builder.getUniqueId(), function_);

  header_block_ = builder.getBuildPoint();

  spv::Id header_block_id = header_block_->getId();
  then_phi_parent_ = header_block_id;
  else_phi_parent_ = header_block_id;

  function_.addBlock(then_block_);
  builder.setBuildPoint(then_block_);
}

void SPIRVBuilder::IfBuilder::MakeBeginElse(bool branch_to_merge) {
#ifndef NDEBUG
  REX_ASSERT(current_branch_ == Branch::kThen);
#endif

  if (branch_to_merge && !builder_.getBuildPoint()->isTerminated()) {
    then_phi_parent_ = builder_.getBuildPoint()->getId();
    builder_.createBranch(merge_block_);
  }

  else_block_ = new spv::Block(builder_.getUniqueId(), function_);
  function_.addBlock(else_block_);

  builder_.setBuildPoint(else_block_);

#ifndef NDEBUG
  current_branch_ = Branch::kElse;
#endif
}

void SPIRVBuilder::IfBuilder::MakeEndIf(bool branch_to_merge) {
#ifndef NDEBUG
  REX_ASSERT(current_branch_ == Branch::kThen || current_branch_ == Branch::kElse);
#endif

  if (branch_to_merge && !builder_.getBuildPoint()->isTerminated()) {
    (else_block_ ? else_phi_parent_ : then_phi_parent_) = builder_.getBuildPoint()->getId();
    builder_.createBranch(merge_block_);
  }

  builder_.setBuildPoint(header_block_);
  builder_.createSelectionMerge(merge_block_, control_);
  {
    spv::Block* false_block = else_block_ ? else_block_ : merge_block_;
    std::unique_ptr<spv::Instruction> branch =
        std::make_unique<spv::Instruction>(spv::OpBranchConditional);
    branch->addIdOperand(condition_);
    branch->addIdOperand(then_block_->getId());
    branch->addIdOperand(false_block->getId());
    if (then_weight_ || else_weight_) {
      branch->addImmediateOperand(then_weight_);
      branch->addImmediateOperand(else_weight_);
    }
    builder_.getBuildPoint()->addInstruction(std::move(branch));
    then_block_->addPredecessor(builder_.getBuildPoint());
    false_block->addPredecessor(builder_.getBuildPoint());
  }

  function_.addBlock(merge_block_);
  builder_.setBuildPoint(merge_block_);

#ifndef NDEBUG
  current_branch_ = Branch::kMerge;
#endif
}

spv::Id SPIRVBuilder::IfBuilder::CreateMergePhi(spv::Id then_variable,
                                                spv::Id else_variable) const {
  REX_ASSERT(builder_.getBuildPoint() == merge_block_);
  return builder_.CreateQuadOp(spv::OpPhi, builder_.getTypeId(then_variable), then_variable,
                               GetThenPhiParent(), else_variable, GetElsePhiParent());
}

SPIRVBuilder::SwitchBuilder::SwitchBuilder(spv::Id selector, unsigned int selection_control,
                                           SPIRVBuilder& builder)
    : builder_(builder),
      selector_(selector),
      selection_control_(selection_control),
      function_(builder.getBuildPoint()->getParent()),
      header_block_(builder.getBuildPoint()),
      default_phi_parent_(builder.getBuildPoint()->getId()) {
  merge_block_ = new spv::Block(builder_.getUniqueId(), function_);
}

void SPIRVBuilder::SwitchBuilder::MakeBeginDefault() {
  REX_ASSERT(default_block_ == nullptr);

  EndSegment();

  default_block_ = new spv::Block(builder_.getUniqueId(), function_);
  function_.addBlock(default_block_);
  default_block_->addPredecessor(header_block_);
  builder_.setBuildPoint(default_block_);

  current_branch_ = Branch::kDefault;
}

void SPIRVBuilder::SwitchBuilder::MakeBeginCase(unsigned int literal) {
  EndSegment();

  auto case_block = new spv::Block(builder_.getUniqueId(), function_);
  function_.addBlock(case_block);
  cases_.emplace_back(literal, case_block->getId());
  case_block->addPredecessor(header_block_);
  builder_.setBuildPoint(case_block);

  current_branch_ = Branch::kCase;
}

void SPIRVBuilder::SwitchBuilder::AddCurrentCaseLiteral(unsigned int literal) {
  REX_ASSERT(current_branch_ == Branch::kCase);

  cases_.emplace_back(literal, cases_.back().second);
}

void SPIRVBuilder::SwitchBuilder::MakeEndSwitch() {
  EndSegment();

  builder_.setBuildPoint(header_block_);

  builder_.createSelectionMerge(merge_block_, selection_control_);

  std::unique_ptr<spv::Instruction> switch_instruction =
      std::make_unique<spv::Instruction>(spv::OpSwitch);
  switch_instruction->addIdOperand(selector_);
  if (default_block_) {
    switch_instruction->addIdOperand(default_block_->getId());
  } else {
    switch_instruction->addIdOperand(merge_block_->getId());
    merge_block_->addPredecessor(header_block_);
  }
  for (const std::pair<unsigned int, spv::Id>& case_pair : cases_) {
    switch_instruction->addImmediateOperand(case_pair.first);
    switch_instruction->addIdOperand(case_pair.second);
  }
  builder_.getBuildPoint()->addInstruction(std::move(switch_instruction));

  function_.addBlock(merge_block_);
  builder_.setBuildPoint(merge_block_);

  current_branch_ = Branch::kMerge;
}

void SPIRVBuilder::SwitchBuilder::EndSegment() {
  REX_ASSERT(current_branch_ == Branch::kSelection || current_branch_ == Branch::kDefault ||
             current_branch_ == Branch::kCase);

  if (current_branch_ == Branch::kSelection) {
    return;
  }

  if (!builder_.getBuildPoint()->isTerminated()) {
    builder_.createBranch(merge_block_);
    if (current_branch_ == Branch::kDefault) {
      default_phi_parent_ = builder_.getBuildPoint()->getId();
    }
  }

  current_branch_ = Branch::kSelection;
}

}  // namespace rex::gpu
