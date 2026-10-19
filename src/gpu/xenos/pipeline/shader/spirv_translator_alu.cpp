/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <memory>

#include <SPIRV/GLSL.std.450.h>

#include <rex/assert.h>
#include "gpu/xenos/pipeline/shader/spirv_translator.h"
#include <rex/math.h>

namespace rex::gpu {

spv::Id SPIRVShaderTranslator::ZeroIfAnyOperandIsZero(spv::Id value, spv::Id operand_0_abs,
                                                      spv::Id operand_1_abs) {
  EnsureBuildPointAvailable();
  int num_components = builder_->getNumComponents(value);
  REX_ASSERT(builder_->getNumComponents(operand_0_abs) == num_components);
  REX_ASSERT(builder_->getNumComponents(operand_1_abs) == num_components);
  return builder_->createTriOp(
      spv::OpSelect, type_float,
      builder_->createBinOp(spv::OpFOrdEqual, type_bool_vectors[num_components - 1],
                            builder_->CreateBinBuiltinCall(type_float_vectors[num_components - 1],
                                                           ext_inst_glsl_std_450_, GLSLstd450NMin,
                                                           operand_0_abs, operand_1_abs),
                            const_float_vectors_0[num_components - 1]),
      const_float_vectors_0[num_components - 1], value);
}

void SPIRVShaderTranslator::KillPixel(spv::Id condition,
                                      u8 memexport_em_potentially_written_before) {
  SPIRVBuilder::IfBuilder kill_if(condition, spv::SelectionControlMaskNone, *builder_);
  {
    ExportToMemory(memexport_em_potentially_written_before);
    if (var_main_kill_pixel_ != spv::NoResult) {
      builder_->createStore(builder_->makeBoolConstant(true), var_main_kill_pixel_);
    }
    if (features_.demote_to_helper_invocation) {
      builder_->createNoResultOp(spv::OpDemoteToHelperInvocationEXT);
    }
  }
  kill_if.MakeEndIf();
}

void SPIRVShaderTranslator::ProcessAluInstruction(const ParsedAluInstruction& instr,
                                                  u8 memexport_em_potentially_written_before) {
  if (instr.IsNop()) {
    return;
  }

  UpdateInstructionPredication(instr.is_predicated, instr.predicate_condition);

  bool predicate_written_vector = false;
  spv::Id vector_result = ProcessVectorAluOperation(instr, memexport_em_potentially_written_before,
                                                    predicate_written_vector);

  bool predicate_written_scalar = false;
  spv::Id scalar_result = ProcessScalarAluOperation(instr, memexport_em_potentially_written_before,
                                                    predicate_written_scalar);
  if (scalar_result != spv::NoResult) {
    EnsureBuildPointAvailable();
    builder_->createStore(scalar_result, var_main_previous_scalar_);
  } else {
    if (instr.scalar_result.GetUsedWriteMask()) {
      EnsureBuildPointAvailable();
      scalar_result = builder_->createLoad(var_main_previous_scalar_, spv::NoPrecision);
    }
  }

  StoreResult(instr.vector_and_constant_result, vector_result,
              instr.GetMemExportStreamConstant() != UINT32_MAX);
  StoreResult(instr.scalar_result, scalar_result);

  if (predicate_written_vector || predicate_written_scalar) {
    cf_exec_predicate_written_ = true;
    CloseInstructionPredication();
  }
}

spv::Id SPIRVShaderTranslator::ProcessVectorAluOperation(const ParsedAluInstruction& instr,
                                                         u8 memexport_em_potentially_written_before,
                                                         bool& predicate_written) {
  predicate_written = false;

  u32 used_result_components = instr.vector_and_constant_result.GetUsedResultComponents();
  if (!used_result_components &&
      !xenos::ucode::GetAluVectorOpcodeInfo(instr.vector_opcode).changed_state) {
    return spv::NoResult;
  }
  u32 used_result_component_count = rex::BitCount(used_result_components);

  u32 operand_count;
  if (instr.vector_opcode == xenos::ucode::AluVectorOpcode::kCube) {
    operand_count = 1;
  } else {
    operand_count = instr.vector_operand_count;
  }
  spv::Id operand_storage[3] = {};
  for (u32 i = 0; i < operand_count; ++i) {
    operand_storage[i] = LoadOperandStorage(instr.vector_operands[i]);
  }
  spv::Id result_type = used_result_component_count
                            ? type_float_vectors[used_result_component_count - 1]
                            : spv::NoType;

  EnsureBuildPointAvailable();

  static const unsigned int kOps[] = {
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),
      static_cast<unsigned int>(spv::OpFOrdLessThan),
      static_cast<unsigned int>(spv::OpFOrdEqual),
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),
      static_cast<unsigned int>(spv::OpFUnordNotEqual),
      static_cast<unsigned int>(GLSLstd450Fract),
      static_cast<unsigned int>(GLSLstd450Trunc),
      static_cast<unsigned int>(GLSLstd450Floor),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpFOrdEqual),
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpFOrdEqual),
      static_cast<unsigned int>(spv::OpFUnordNotEqual),
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),
      static_cast<unsigned int>(spv::OpFOrdEqual),
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),
      static_cast<unsigned int>(spv::OpFUnordNotEqual),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),
  };

  switch (instr.vector_opcode) {
    case xenos::ucode::AluVectorOpcode::kAdd: {
      return builder_->CreateNoContractionBinOp(
          spv::OpFAdd, result_type,
          GetOperandComponents(operand_storage[0], instr.vector_operands[0],
                               used_result_components),
          GetOperandComponents(operand_storage[1], instr.vector_operands[1],
                               used_result_components));
    }
    case xenos::ucode::AluVectorOpcode::kMul:
    case xenos::ucode::AluVectorOpcode::kMad: {
      spv::Id multiplicands[2];
      for (u32 i = 0; i < 2; ++i) {
        multiplicands[i] = GetOperandComponents(operand_storage[i], instr.vector_operands[i],
                                                used_result_components);
      }
      spv::Id result = builder_->CreateNoContractionBinOp(spv::OpFMul, result_type,
                                                          multiplicands[0], multiplicands[1]);
      u32 multiplicands_different =
          used_result_components &
          ~instr.vector_operands[0].GetIdenticalComponents(instr.vector_operands[1]);
      if (multiplicands_different) {
        spv::Id different_operands[2] = {multiplicands[0], multiplicands[1]};
        spv::Id different_result = result;
        u32 different_count = rex::BitCount(multiplicands_different);
        spv::Id different_type = type_float_vectors[different_count - 1];
        if (multiplicands_different != used_result_components) {
          uint_vector_temp_.clear();
          u32 components_remaining = used_result_components;
          for (u32 i = 0; i < used_result_component_count; ++i) {
            u32 component;
            rex::bit_scan_forward(components_remaining, &component);
            components_remaining &= ~(static_cast<u32>(1) << component);
            if (multiplicands_different & (1 << component)) {
              uint_vector_temp_.push_back(i);
            }
          }
          REX_ASSERT(uint_vector_temp_.size() == different_count);
          if (different_count > 1) {
            for (u32 i = 0; i < 2; ++i) {
              different_operands[i] = builder_->createRvalueSwizzle(
                  spv::NoPrecision, different_type, different_operands[i], uint_vector_temp_);
            }
            different_result = builder_->createRvalueSwizzle(spv::NoPrecision, different_type,
                                                             different_result, uint_vector_temp_);
          } else {
            for (u32 i = 0; i < 2; ++i) {
              different_operands[i] = builder_->createCompositeExtract(
                  different_operands[i], different_type, uint_vector_temp_[0]);
            }
            different_result = builder_->createCompositeExtract(different_result, different_type,
                                                                uint_vector_temp_[0]);
          }
        }
        for (u32 i = 0; i < 2; ++i) {
          different_operands[i] =
              GetAbsoluteOperand(different_operands[i], instr.vector_operands[i]);
        }
        spv::Id different_zero = builder_->createBinOp(
            spv::OpFOrdEqual, type_bool_vectors[different_count - 1],
            builder_->CreateBinBuiltinCall(different_type, ext_inst_glsl_std_450_, GLSLstd450NMin,
                                           different_operands[0], different_operands[1]),
            const_float_vectors_0[different_count - 1]);
        different_result =
            builder_->createTriOp(spv::OpSelect, different_type, different_zero,
                                  const_float_vectors_0[different_count - 1], different_result);
        if (multiplicands_different != used_result_components) {
          if (different_count > 1) {
            std::unique_ptr<spv::Instruction> shuffle_op = std::make_unique<spv::Instruction>(
                builder_->getUniqueId(), result_type, spv::OpVectorShuffle);
            shuffle_op->addIdOperand(result);
            shuffle_op->addIdOperand(different_result);
            u32 components_remaining = used_result_components;
            unsigned int different_shuffle_index = used_result_component_count;
            for (u32 i = 0; i < used_result_component_count; ++i) {
              u32 component;
              rex::bit_scan_forward(components_remaining, &component);
              components_remaining &= ~(static_cast<u32>(1) << component);
              shuffle_op->addImmediateOperand(
                  (multiplicands_different & (1 << component)) ? different_shuffle_index++ : i);
            }
            result = shuffle_op->getResultId();
            builder_->getBuildPoint()->addInstruction(std::move(shuffle_op));
          } else {
            result = builder_->createCompositeInsert(
                different_result, result, result_type,
                rex::BitCount(used_result_components & (multiplicands_different - 1)));
          }
        } else {
          result = different_result;
        }
      }
      if (instr.vector_opcode == xenos::ucode::AluVectorOpcode::kMad) {
        result = builder_->CreateNoContractionBinOp(
            spv::OpFAdd, result_type, result,
            GetOperandComponents(operand_storage[2], instr.vector_operands[2],
                                 used_result_components));
      }
      return result;
    }

    case xenos::ucode::AluVectorOpcode::kMax:
    case xenos::ucode::AluVectorOpcode::kMin:
    case xenos::ucode::AluVectorOpcode::kMaxA: {
      bool is_maxa = instr.vector_opcode == xenos::ucode::AluVectorOpcode::kMaxA;
      spv::Id operand_0 =
          GetOperandComponents(operand_storage[0], instr.vector_operands[0],
                               used_result_components | (is_maxa ? 0b1000 : 0b0000));
      spv::Id maxa_operand_0_w = spv::NoResult;
      if (is_maxa) {
        int operand_0_num_components = builder_->getNumComponents(operand_0);
        if (operand_0_num_components > 1) {
          maxa_operand_0_w = builder_->createCompositeExtract(
              operand_0, type_float, static_cast<unsigned int>(operand_0_num_components - 1));
        } else {
          maxa_operand_0_w = operand_0;
        }
        builder_->createStore(
            builder_->createUnaryOp(
                spv::OpConvertFToS, type_int,
                builder_->CreateTriBuiltinCall(
                    type_float, ext_inst_glsl_std_450_, GLSLstd450NClamp,
                    builder_->CreateUnaryBuiltinCall(type_float, ext_inst_glsl_std_450_,
                                                     GLSLstd450Floor,
                                                     builder_->CreateNoContractionBinOp(
                                                         spv::OpFAdd, type_float, maxa_operand_0_w,
                                                         builder_->makeFloatConstant(0.5f))),
                    builder_->makeFloatConstant(-256.0f), builder_->makeFloatConstant(255.0f))),
            var_main_address_register_);
      }
      if (!used_result_components) {
        return spv::NoResult;
      }
      u32 identical = instr.vector_operands[0].GetIdenticalComponents(instr.vector_operands[1]) &
                      used_result_components;
      spv::Id operand_0_per_component;
      if (is_maxa && !(used_result_components & 0b1000) &&
          (identical == used_result_components || !identical)) {
        if (used_result_component_count > 1) {
          uint_vector_temp_.clear();
          for (unsigned int i = 0; i < used_result_component_count; ++i) {
            uint_vector_temp_.push_back(i);
          }
          operand_0_per_component = builder_->createRvalueSwizzle(
              spv::NoPrecision, type_float_vectors[used_result_component_count - 1], operand_0,
              uint_vector_temp_);
        } else {
          operand_0_per_component = builder_->createCompositeExtract(operand_0, type_float, 0);
        }
      } else {
        operand_0_per_component = operand_0;
      }
      if (identical == used_result_components) {
        REX_ASSERT(static_cast<u32>(builder_->getNumComponents(operand_0_per_component)) ==
                   used_result_component_count);
        return operand_0_per_component;
      }
      spv::Id operand_1 = GetOperandComponents(operand_storage[1], instr.vector_operands[1],
                                               used_result_components);
      spv::Op op = static_cast<spv::Op>(kOps[static_cast<size_t>(instr.vector_opcode)]);
      if (!identical) {
        REX_ASSERT(static_cast<u32>(builder_->getNumComponents(operand_0_per_component)) ==
                   used_result_component_count);
        return builder_->createTriOp(
            spv::OpSelect, result_type,
            builder_->createBinOp(op, type_bool_vectors[used_result_component_count - 1],
                                  operand_0_per_component, operand_1),
            operand_0_per_component, operand_1);
      }
      REX_ASSERT(used_result_component_count > 1);
      id_vector_temp_.clear();
      u32 components_remaining = used_result_components;
      for (u32 i = 0; i < used_result_component_count; ++i) {
        spv::Id result_component =
            ((used_result_components & 0b1000) && i + 1 >= used_result_component_count &&
             maxa_operand_0_w != spv::NoResult)
                ? maxa_operand_0_w
                : builder_->createCompositeExtract(operand_0, type_float, i);
        u32 component_index;
        rex::bit_scan_forward(components_remaining, &component_index);
        components_remaining &= ~(static_cast<u32>(1) << component_index);
        if (!(identical & (1 << component_index))) {
          spv::Id operand_1_component = builder_->createCompositeExtract(operand_1, type_float, i);
          result_component = builder_->createTriOp(
              spv::OpSelect, type_float,
              builder_->createBinOp(op, type_bool, result_component, operand_1_component),
              result_component, operand_1_component);
        }
        id_vector_temp_.push_back(result_component);
      }
      return builder_->createCompositeConstruct(result_type, id_vector_temp_);
    }

    case xenos::ucode::AluVectorOpcode::kSeq:
    case xenos::ucode::AluVectorOpcode::kSgt:
    case xenos::ucode::AluVectorOpcode::kSge:
    case xenos::ucode::AluVectorOpcode::kSne:
      return builder_->createTriOp(
          spv::OpSelect, result_type,
          builder_->createBinOp(
              static_cast<spv::Op>(kOps[static_cast<size_t>(instr.vector_opcode)]),
              type_bool_vectors[used_result_component_count - 1],
              GetOperandComponents(operand_storage[0], instr.vector_operands[0],
                                   used_result_components),
              GetOperandComponents(operand_storage[1], instr.vector_operands[1],
                                   used_result_components)),
          const_float_vectors_1[used_result_component_count - 1],
          const_float_vectors_0[used_result_component_count - 1]);

    case xenos::ucode::AluVectorOpcode::kFrc:
    case xenos::ucode::AluVectorOpcode::kTrunc:
    case xenos::ucode::AluVectorOpcode::kFloor:
      return builder_->CreateUnaryBuiltinCall(
          result_type, ext_inst_glsl_std_450_,
          static_cast<GLSLstd450>(kOps[static_cast<size_t>(instr.vector_opcode)]),
          GetOperandComponents(operand_storage[0], instr.vector_operands[0],
                               used_result_components));

    case xenos::ucode::AluVectorOpcode::kCndEq:
    case xenos::ucode::AluVectorOpcode::kCndGe:
    case xenos::ucode::AluVectorOpcode::kCndGt:
      return builder_->createTriOp(
          spv::OpSelect, result_type,
          builder_->createBinOp(
              static_cast<spv::Op>(kOps[static_cast<size_t>(instr.vector_opcode)]),
              type_bool_vectors[used_result_component_count - 1],
              GetOperandComponents(operand_storage[0], instr.vector_operands[0],
                                   used_result_components),
              const_float_vectors_0[used_result_component_count - 1]),
          GetOperandComponents(operand_storage[1], instr.vector_operands[1],
                               used_result_components),
          GetOperandComponents(operand_storage[2], instr.vector_operands[2],
                               used_result_components));

    case xenos::ucode::AluVectorOpcode::kDp4:
    case xenos::ucode::AluVectorOpcode::kDp3:
    case xenos::ucode::AluVectorOpcode::kDp2Add: {
      u32 component_count;
      if (instr.vector_opcode == xenos::ucode::AluVectorOpcode::kDp2Add) {
        component_count = 2;
      } else if (instr.vector_opcode == xenos::ucode::AluVectorOpcode::kDp3) {
        component_count = 3;
      } else {
        component_count = 4;
      }
      u32 component_mask = (1 << component_count) - 1;
      spv::Id operands[2];
      for (u32 i = 0; i < 2; ++i) {
        operands[i] =
            GetOperandComponents(operand_storage[i], instr.vector_operands[i], component_mask);
      }
      u32 different = component_mask &
                      ~instr.vector_operands[0].GetIdenticalComponents(instr.vector_operands[1]);
      spv::Id result = spv::NoResult;
      for (u32 i = 0; i < component_count; ++i) {
        spv::Id operand_components[2];
        for (unsigned int j = 0; j < 2; ++j) {
          operand_components[j] = builder_->createCompositeExtract(operands[j], type_float, i);
        }
        spv::Id product = builder_->CreateNoContractionBinOp(
            spv::OpFMul, type_float, operand_components[0], operand_components[1]);
        if (different & (1 << i)) {
          product = ZeroIfAnyOperandIsZero(
              product, GetAbsoluteOperand(operand_components[0], instr.vector_operands[0]),
              GetAbsoluteOperand(operand_components[1], instr.vector_operands[1]));
        }
        if (!i) {
          result = product;
          continue;
        }
        result = builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float, result, product);
      }
      if (instr.vector_opcode == xenos::ucode::AluVectorOpcode::kDp2Add) {
        result = builder_->CreateNoContractionBinOp(
            spv::OpFAdd, type_float, result,
            GetOperandComponents(operand_storage[2], instr.vector_operands[2], 0b0001));
      }
      return result;
    }

    case xenos::ucode::AluVectorOpcode::kCube: {
      spv::Id operand_vector =
          GetOperandComponents(operand_storage[0], instr.vector_operands[0], 0b1101);
      spv::Id operand[3];
      for (unsigned int i = 0; i < 3; ++i) {
        operand[i] = builder_->createCompositeExtract(operand_vector, type_float, (i + 1) % 3);
      }
      spv::Id operand_abs[3];
      if (!instr.vector_operands[0].is_absolute_value || instr.vector_operands[0].is_negated) {
        for (unsigned int i = 0; i < 3; ++i) {
          operand_abs[i] = builder_->CreateUnaryBuiltinCall(type_float, ext_inst_glsl_std_450_,
                                                            GLSLstd450FAbs, operand[i]);
        }
      } else {
        for (unsigned int i = 0; i < 3; ++i) {
          operand_abs[i] = operand[i];
        }
      }
      spv::Id operand_neg[3] = {};
      if (used_result_components & 0b0001) {
        operand_neg[1] =
            builder_->CreateNoContractionUnaryOp(spv::OpFNegate, type_float, operand[1]);
      }
      if (used_result_components & 0b0010) {
        operand_neg[0] =
            builder_->CreateNoContractionUnaryOp(spv::OpFNegate, type_float, operand[0]);
        operand_neg[2] =
            builder_->CreateNoContractionUnaryOp(spv::OpFNegate, type_float, operand[2]);
      }

      spv::Id ma_z_result[4] = {}, ma_yx_result[4] = {};

      SPIRVBuilder::IfBuilder ma_z_if(
          builder_->createBinOp(spv::OpLogicalAnd, type_bool,
                                builder_->createBinOp(spv::OpFOrdGreaterThanEqual, type_bool,
                                                      operand_abs[2], operand_abs[0]),
                                builder_->createBinOp(spv::OpFOrdGreaterThanEqual, type_bool,
                                                      operand_abs[2], operand_abs[1])),
          spv::SelectionControlMaskNone, *builder_);
      {
        ma_z_result[0] = operand_neg[1];
        ma_z_result[2] = operand[2];
        if (used_result_components & 0b1010) {
          spv::Id z_is_neg =
              builder_->createBinOp(spv::OpFOrdLessThan, type_bool, operand[2], const_float_0);
          if (used_result_components & 0b0010) {
            ma_z_result[1] = builder_->createTriOp(spv::OpSelect, type_float, z_is_neg,
                                                   operand_neg[0], operand[0]);
          }
          if (used_result_components & 0b1000) {
            ma_z_result[3] = builder_->createTriOp(spv::OpSelect, type_float, z_is_neg,
                                                   builder_->makeFloatConstant(5.0f),
                                                   builder_->makeFloatConstant(4.0f));
          }
        }
      }
      ma_z_if.MakeBeginElse();
      {
        spv::Id ma_y_result[4] = {}, ma_x_result[4] = {};

        SPIRVBuilder::IfBuilder ma_y_if(
            builder_->createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, operand_abs[1],
                                  operand_abs[0]),
            spv::SelectionControlMaskNone, *builder_);
        {
          ma_y_result[1] = operand[0];
          ma_y_result[2] = operand[1];
          if (used_result_components & 0b1001) {
            spv::Id y_is_neg =
                builder_->createBinOp(spv::OpFOrdLessThan, type_bool, operand[1], const_float_0);
            if (used_result_components & 0b0001) {
              ma_y_result[0] = builder_->createTriOp(spv::OpSelect, type_float, y_is_neg,
                                                     operand_neg[2], operand[2]);
              ma_y_result[3] = builder_->createTriOp(spv::OpSelect, type_float, y_is_neg,
                                                     builder_->makeFloatConstant(3.0f),
                                                     builder_->makeFloatConstant(2.0f));
            }
          }
        }
        ma_y_if.MakeBeginElse();
        {
          ma_x_result[0] = operand_neg[1];
          ma_x_result[2] = operand[0];
          if (used_result_components & 0b1010) {
            spv::Id x_is_neg =
                builder_->createBinOp(spv::OpFOrdLessThan, type_bool, operand[0], const_float_0);
            if (used_result_components & 0b0010) {
              ma_x_result[1] = builder_->createTriOp(spv::OpSelect, type_float, x_is_neg,
                                                     operand[2], operand_neg[2]);
            }
            if (used_result_components & 0b1000) {
              ma_x_result[3] = builder_->createTriOp(spv::OpSelect, type_float, x_is_neg,
                                                     const_float_1, const_float_0);
            }
          }
        }
        ma_y_if.MakeEndIf();

        for (u32 i = 0; i < 4; ++i) {
          if (!(used_result_components & (1 << i))) {
            continue;
          }
          ma_yx_result[i] = ma_y_if.CreateMergePhi(ma_y_result[i], ma_x_result[i]);
        }
      }
      ma_z_if.MakeEndIf();

      id_vector_temp_.clear();
      for (u32 i = 0; i < 4; ++i) {
        if (!(used_result_components & (1 << i))) {
          continue;
        }
        id_vector_temp_.push_back(ma_z_if.CreateMergePhi(ma_z_result[i], ma_yx_result[i]));
      }
      REX_ASSERT(id_vector_temp_.size() == used_result_component_count);
      if (used_result_components & 0b0100) {
        spv::Id& ma2 = id_vector_temp_[rex::BitCount(used_result_components & ((1 << 2) - 1))];
        ma2 = builder_->CreateNoContractionBinOp(spv::OpFMul, type_float,
                                                 builder_->makeFloatConstant(2.0f), ma2);
      }
      if (used_result_component_count == 1) {
        return id_vector_temp_[0];
      }
      return builder_->createCompositeConstruct(type_float_vectors[used_result_component_count - 1],
                                                id_vector_temp_);
    }

    case xenos::ucode::AluVectorOpcode::kMax4: {
      u32 components_remaining = 0b0000;
      for (u32 i = 0; i < 4; ++i) {
        SwizzleSource swizzle_source = instr.vector_operands[0].GetComponent(i);
        REX_ASSERT(swizzle_source >= SwizzleSource::kX && swizzle_source <= SwizzleSource::kW);
        components_remaining |=
            1 << (static_cast<u32>(swizzle_source) - std::to_underlying(SwizzleSource::kX));
      }
      REX_ASSERT(components_remaining != 0);
      spv::Id operand = ApplyOperandModifiers(operand_storage[0], instr.vector_operands[0]);
      u32 component;
      rex::bit_scan_forward(components_remaining, &component);
      components_remaining &= ~(static_cast<u32>(1) << component);
      spv::Id result = builder_->createCompositeExtract(operand, type_float,
                                                        static_cast<unsigned int>(component));
      while (rex::bit_scan_forward(components_remaining, &component)) {
        components_remaining &= ~(static_cast<u32>(1) << component);
        result = builder_->CreateBinBuiltinCall(
            type_float, ext_inst_glsl_std_450_, GLSLstd450NMax, result,
            builder_->createCompositeExtract(operand, type_float,
                                             static_cast<unsigned int>(component)));
      }
      return result;
    }

    case xenos::ucode::AluVectorOpcode::kSetpEqPush:
    case xenos::ucode::AluVectorOpcode::kSetpNePush:
    case xenos::ucode::AluVectorOpcode::kSetpGtPush:
    case xenos::ucode::AluVectorOpcode::kSetpGePush: {
      spv::Id operands[2];
      spv::Id operands_w[2];
      for (u32 i = 0; i < 2; ++i) {
        operands[i] = GetOperandComponents(operand_storage[i], instr.vector_operands[i],
                                           used_result_components ? 0b1001 : 0b1000);
        if (used_result_components) {
          operands_w[i] = builder_->createCompositeExtract(operands[i], type_float, 1);
        } else {
          operands_w[i] = operands[i];
        }
      }
      spv::Op op = static_cast<spv::Op>(kOps[static_cast<size_t>(instr.vector_opcode)]);
      builder_->createStore(
          builder_->createBinOp(
              spv::OpLogicalAnd, type_bool,
              builder_->createBinOp(spv::OpFOrdEqual, type_bool, operands_w[0], const_float_0),
              builder_->createBinOp(op, type_bool, operands_w[1], const_float_0)),
          var_main_predicate_);
      predicate_written = true;
      if (!used_result_components) {
        return spv::NoResult;
      }
      spv::Id operands_x[2];
      for (u32 i = 0; i < 2; ++i) {
        operands_x[i] = builder_->createCompositeExtract(operands[i], type_float, 0);
      }
      spv::Id condition = builder_->createBinOp(
          spv::OpLogicalAnd, type_bool,
          builder_->createBinOp(spv::OpFOrdEqual, type_bool, operands_x[0], const_float_0),
          builder_->createBinOp(op, type_bool, operands_x[1], const_float_0));
      return builder_->CreateNoContractionBinOp(
          spv::OpFAdd, type_float,
          builder_->createTriOp(spv::OpSelect, type_float, condition,
                                builder_->makeFloatConstant(-1.0f), operands_x[0]),
          const_float_1);
    }

    case xenos::ucode::AluVectorOpcode::kKillEq:
    case xenos::ucode::AluVectorOpcode::kKillGt:
    case xenos::ucode::AluVectorOpcode::kKillGe:
    case xenos::ucode::AluVectorOpcode::kKillNe: {
      KillPixel(
          builder_->createUnaryOp(
              spv::OpAny, type_bool,
              builder_->createBinOp(
                  static_cast<spv::Op>(kOps[static_cast<size_t>(instr.vector_opcode)]), type_bool4,
                  GetOperandComponents(operand_storage[0], instr.vector_operands[0], 0b1111),
                  GetOperandComponents(operand_storage[1], instr.vector_operands[1], 0b1111))),
          memexport_em_potentially_written_before);
      return const_float_0;
    }

    case xenos::ucode::AluVectorOpcode::kDst: {
      spv::Id operands[2] = {};
      if (used_result_components & 0b0110) {
        operands[0] = GetOperandComponents(operand_storage[0], instr.vector_operands[0],
                                           used_result_components & 0b0110);
      }
      if (used_result_components & 0b1010) {
        operands[1] = GetOperandComponents(operand_storage[1], instr.vector_operands[1],
                                           used_result_components & 0b1010);
      }
      spv::Id result_y = spv::NoResult;
      if (used_result_components & 0b0010) {
        spv::Id operands_y[2];
        operands_y[0] = (used_result_components & 0b0100)
                            ? builder_->createCompositeExtract(operands[0], type_float, 0)
                            : operands[0];
        operands_y[1] = (used_result_components & 0b1000)
                            ? builder_->createCompositeExtract(operands[1], type_float, 0)
                            : operands[1];
        result_y = builder_->CreateNoContractionBinOp(spv::OpFMul, type_float, operands_y[0],
                                                      operands_y[1]);
        if (!(instr.vector_operands[0].GetIdenticalComponents(instr.vector_operands[1]) & 0b0010)) {
          result_y = ZeroIfAnyOperandIsZero(
              result_y, GetAbsoluteOperand(operands_y[0], instr.vector_operands[0]),
              GetAbsoluteOperand(operands_y[1], instr.vector_operands[1]));
        }
      }
      id_vector_temp_.clear();
      if (used_result_components & 0b0001) {
        id_vector_temp_.push_back(const_float_1);
      }
      if (used_result_components & 0b0010) {
        id_vector_temp_.push_back(result_y);
      }
      if (used_result_components & 0b0100) {
        id_vector_temp_.push_back((used_result_components & 0b0010)
                                      ? builder_->createCompositeExtract(operands[0], type_float, 1)
                                      : operands[0]);
      }
      if (used_result_components & 0b1000) {
        id_vector_temp_.push_back((used_result_components & 0b0010)
                                      ? builder_->createCompositeExtract(operands[1], type_float, 1)
                                      : operands[1]);
      }
      REX_ASSERT(id_vector_temp_.size() == used_result_component_count);
      if (used_result_component_count == 1) {
        return id_vector_temp_[0];
      }
      return builder_->createCompositeConstruct(type_float_vectors[used_result_component_count - 1],
                                                id_vector_temp_);
    }
  }

  REX_UNHANDLED_CASE(instr.vector_opcode);
  EmitTranslationError("Unknown ALU vector operation");
  return spv::NoResult;
}

spv::Id SPIRVShaderTranslator::ProcessScalarAluOperation(const ParsedAluInstruction& instr,
                                                         u8 memexport_em_potentially_written_before,
                                                         bool& predicate_written) {
  predicate_written = false;

  spv::Id operand_storage[2] = {};
  for (u32 i = 0; i < instr.scalar_operand_count; ++i) {
    operand_storage[i] = LoadOperandStorage(instr.scalar_operands[i]);
  }

  EnsureBuildPointAvailable();

  static const unsigned int kOps[] = {
      static_cast<unsigned int>(spv::OpFAdd),
      static_cast<unsigned int>(spv::OpFAdd),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),
      static_cast<unsigned int>(spv::OpFOrdLessThan),
      static_cast<unsigned int>(spv::OpFOrdEqual),
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),
      static_cast<unsigned int>(spv::OpFUnordNotEqual),
      static_cast<unsigned int>(GLSLstd450Fract),
      static_cast<unsigned int>(GLSLstd450Trunc),
      static_cast<unsigned int>(GLSLstd450Floor),
      static_cast<unsigned int>(GLSLstd450Exp2),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(GLSLstd450Log2),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(GLSLstd450InverseSqrt),
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),
      static_cast<unsigned int>(spv::OpFSub),
      static_cast<unsigned int>(spv::OpFSub),
      static_cast<unsigned int>(spv::OpFOrdEqual),
      static_cast<unsigned int>(spv::OpFUnordNotEqual),
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpFOrdEqual),
      static_cast<unsigned int>(spv::OpFOrdGreaterThan),
      static_cast<unsigned int>(spv::OpFOrdGreaterThanEqual),
      static_cast<unsigned int>(spv::OpFUnordNotEqual),
      static_cast<unsigned int>(spv::OpFOrdEqual),
      static_cast<unsigned int>(GLSLstd450Sqrt),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpNop),
      static_cast<unsigned int>(spv::OpFAdd),
      static_cast<unsigned int>(spv::OpFAdd),
      static_cast<unsigned int>(spv::OpFSub),
      static_cast<unsigned int>(spv::OpFSub),
      static_cast<unsigned int>(GLSLstd450Sin),
      static_cast<unsigned int>(GLSLstd450Cos),
      static_cast<unsigned int>(spv::OpNop),
  };

  switch (instr.scalar_opcode) {
    case xenos::ucode::AluScalarOpcode::kAdds:
    case xenos::ucode::AluScalarOpcode::kSubs: {
      spv::Id a, b;
      GetOperandScalarXY(operand_storage[0], instr.scalar_operands[0], a, b);
      return builder_->CreateNoContractionBinOp(
          static_cast<spv::Op>(kOps[static_cast<size_t>(instr.scalar_opcode)]), type_float, a, b);
    }
    case xenos::ucode::AluScalarOpcode::kAddsPrev:
    case xenos::ucode::AluScalarOpcode::kSubsPrev: {
      return builder_->CreateNoContractionBinOp(
          static_cast<spv::Op>(kOps[static_cast<size_t>(instr.scalar_opcode)]), type_float,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001),
          builder_->createLoad(var_main_previous_scalar_, spv::NoPrecision));
    }
    case xenos::ucode::AluScalarOpcode::kMuls: {
      spv::Id a, b;
      GetOperandScalarXY(operand_storage[0], instr.scalar_operands[0], a, b);
      spv::Id result = builder_->CreateNoContractionBinOp(spv::OpFMul, type_float, a, b);
      if (a != b) {
        result = ZeroIfAnyOperandIsZero(result, GetAbsoluteOperand(a, instr.scalar_operands[0]),
                                        GetAbsoluteOperand(b, instr.scalar_operands[0]));
      }
      return result;
    }
    case xenos::ucode::AluScalarOpcode::kMulsPrev: {
      spv::Id a = GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001);
      spv::Id ps = builder_->createLoad(var_main_previous_scalar_, spv::NoPrecision);
      spv::Id result = builder_->CreateNoContractionBinOp(spv::OpFMul, type_float, a, ps);
      return ZeroIfAnyOperandIsZero(
          result, GetAbsoluteOperand(a, instr.scalar_operands[0]),
          builder_->CreateUnaryBuiltinCall(type_float, ext_inst_glsl_std_450_, GLSLstd450FAbs, ps));
    }
    case xenos::ucode::AluScalarOpcode::kMulsPrev2: {
      // Compute the condition before OpSelectionMerge, which must be the penultimate instruction.
      spv::Id ps = builder_->createLoad(var_main_previous_scalar_, spv::NoPrecision);
      spv::Id const_float_max_neg = builder_->makeFloatConstant(-FLT_MAX);
      spv::Id condition =
          builder_->createBinOp(spv::OpFUnordNotEqual, type_bool, ps, const_float_max_neg);
      spv::Id ps_abs =
          builder_->CreateUnaryBuiltinCall(type_float, ext_inst_glsl_std_450_, GLSLstd450FAbs, ps);
      spv::Id ps_abs_neg = builder_->CreateNoContractionUnaryOp(spv::OpFNegate, type_float, ps_abs);
      condition =
          builder_->createBinOp(spv::OpLogicalAnd, type_bool, condition,
                                builder_->createBinOp(spv::OpFOrdGreaterThanEqual, type_bool,
                                                      ps_abs_neg, const_float_max_neg));
      spv::Id b = GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0010);
      spv::Id b_abs_neg = b;
      if (!instr.scalar_operands[0].is_absolute_value) {
        b_abs_neg = builder_->CreateUnaryBuiltinCall(type_float, ext_inst_glsl_std_450_,
                                                     GLSLstd450FAbs, b_abs_neg);
      }
      if (!instr.scalar_operands[0].is_absolute_value || !instr.scalar_operands[0].is_negated) {
        b_abs_neg = builder_->CreateNoContractionUnaryOp(spv::OpFNegate, type_float, b_abs_neg);
      }
      condition =
          builder_->createBinOp(spv::OpLogicalAnd, type_bool, condition,
                                builder_->createBinOp(spv::OpFOrdGreaterThanEqual, type_bool,
                                                      b_abs_neg, const_float_max_neg));
      condition = builder_->createBinOp(
          spv::OpLogicalAnd, type_bool, condition,
          builder_->createBinOp(spv::OpFOrdGreaterThan, type_bool, b, const_float_0));
      SPIRVBuilder::IfBuilder multiply_if(condition, spv::SelectionControlMaskNone, *builder_);
      spv::Id product;
      {
        spv::Id a =
            instr.scalar_operands[0].GetComponent(0) != instr.scalar_operands[0].GetComponent(1)
                ? GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001)
                : b;
        product = builder_->CreateNoContractionBinOp(spv::OpFMul, type_float, a, ps);
        product = ZeroIfAnyOperandIsZero(product, GetAbsoluteOperand(a, instr.scalar_operands[0]),
                                         ps_abs);
      }
      multiply_if.MakeEndIf();
      return multiply_if.CreateMergePhi(product, const_float_max_neg);
    }

    case xenos::ucode::AluScalarOpcode::kMaxs:
    case xenos::ucode::AluScalarOpcode::kMins:
    case xenos::ucode::AluScalarOpcode::kMaxAs:
    case xenos::ucode::AluScalarOpcode::kMaxAsf: {
      spv::Id a, b;
      GetOperandScalarXY(operand_storage[0], instr.scalar_operands[0], a, b);
      if (instr.scalar_opcode == xenos::ucode::AluScalarOpcode::kMaxAs ||
          instr.scalar_opcode == xenos::ucode::AluScalarOpcode::kMaxAsf) {
        spv::Id maxa_address;
        if (instr.scalar_opcode == xenos::ucode::AluScalarOpcode::kMaxAs) {
          maxa_address = builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float, a,
                                                            builder_->makeFloatConstant(0.5f));
        } else {
          maxa_address = a;
        }
        builder_->createStore(
            builder_->createUnaryOp(
                spv::OpConvertFToS, type_int,
                builder_->CreateTriBuiltinCall(
                    type_float, ext_inst_glsl_std_450_, GLSLstd450NClamp,
                    builder_->CreateUnaryBuiltinCall(type_float, ext_inst_glsl_std_450_,
                                                     GLSLstd450Floor, maxa_address),
                    builder_->makeFloatConstant(-256.0f), builder_->makeFloatConstant(255.0f))),
            var_main_address_register_);
      }
      if (a == b) {
        return a;
      }
      return builder_->createTriOp(
          spv::OpSelect, type_float,
          builder_->createBinOp(
              static_cast<spv::Op>(kOps[static_cast<size_t>(instr.scalar_opcode)]), type_bool, a,
              b),
          a, b);
    }

    case xenos::ucode::AluScalarOpcode::kSeqs:
    case xenos::ucode::AluScalarOpcode::kSgts:
    case xenos::ucode::AluScalarOpcode::kSges:
    case xenos::ucode::AluScalarOpcode::kSnes:
      return builder_->createTriOp(
          spv::OpSelect, type_float,
          builder_->createBinOp(
              static_cast<spv::Op>(kOps[static_cast<size_t>(instr.scalar_opcode)]), type_bool,
              GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001),
              const_float_0),
          const_float_1, const_float_0);

    case xenos::ucode::AluScalarOpcode::kFrcs:
    case xenos::ucode::AluScalarOpcode::kTruncs:
    case xenos::ucode::AluScalarOpcode::kFloors:
    case xenos::ucode::AluScalarOpcode::kExp:
    case xenos::ucode::AluScalarOpcode::kLog:
    case xenos::ucode::AluScalarOpcode::kRsq:
    case xenos::ucode::AluScalarOpcode::kSqrt:
    case xenos::ucode::AluScalarOpcode::kSin:
    case xenos::ucode::AluScalarOpcode::kCos:
      return builder_->CreateUnaryBuiltinCall(
          type_float, ext_inst_glsl_std_450_,
          static_cast<GLSLstd450>(kOps[static_cast<size_t>(instr.scalar_opcode)]),
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001));
    case xenos::ucode::AluScalarOpcode::kLogc: {
      spv::Id result = builder_->CreateUnaryBuiltinCall(
          type_float, ext_inst_glsl_std_450_, GLSLstd450Log2,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001));
      return builder_->createTriOp(spv::OpSelect, type_float,
                                   builder_->createBinOp(spv::OpFOrdEqual, type_bool, result,
                                                         builder_->makeFloatConstant(-INFINITY)),
                                   builder_->makeFloatConstant(-FLT_MAX), result);
    }
    case xenos::ucode::AluScalarOpcode::kRcpc: {
      spv::Id result = builder_->CreateNoContractionBinOp(
          spv::OpFDiv, type_float, const_float_1,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001));
      result = builder_->createTriOp(spv::OpSelect, type_float,
                                     builder_->createBinOp(spv::OpFOrdEqual, type_bool, result,
                                                           builder_->makeFloatConstant(-INFINITY)),
                                     builder_->makeFloatConstant(-FLT_MAX), result);
      return builder_->createTriOp(spv::OpSelect, type_float,
                                   builder_->createBinOp(spv::OpFOrdEqual, type_bool, result,
                                                         builder_->makeFloatConstant(INFINITY)),
                                   builder_->makeFloatConstant(FLT_MAX), result);
    }
    case xenos::ucode::AluScalarOpcode::kRcpf: {
      spv::Id result = builder_->CreateNoContractionBinOp(
          spv::OpFDiv, type_float, const_float_1,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001));
      result = builder_->createTriOp(spv::OpSelect, type_float,
                                     builder_->createBinOp(spv::OpFOrdEqual, type_bool, result,
                                                           builder_->makeFloatConstant(INFINITY)),
                                     const_float_0, result);
      // Cannot create -0.0f with makeFloatConstant (float comparison), use the bit pattern.
      result = builder_->createTriOp(spv::OpSelect, type_uint,
                                     builder_->createBinOp(spv::OpFOrdEqual, type_bool, result,
                                                           builder_->makeFloatConstant(-INFINITY)),
                                     builder_->makeUintConstant(static_cast<u32>(INT32_MIN)),
                                     builder_->createUnaryOp(spv::OpBitcast, type_uint, result));
      return builder_->createUnaryOp(spv::OpBitcast, type_float, result);
    }
    case xenos::ucode::AluScalarOpcode::kRcp: {
      return builder_->CreateNoContractionBinOp(
          spv::OpFDiv, type_float, const_float_1,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001));
    }
    case xenos::ucode::AluScalarOpcode::kRsqc: {
      spv::Id result = builder_->CreateUnaryBuiltinCall(
          type_float, ext_inst_glsl_std_450_, GLSLstd450InverseSqrt,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001));
      result = builder_->createTriOp(spv::OpSelect, type_float,
                                     builder_->createBinOp(spv::OpFOrdEqual, type_bool, result,
                                                           builder_->makeFloatConstant(-INFINITY)),
                                     builder_->makeFloatConstant(-FLT_MAX), result);
      return builder_->createTriOp(spv::OpSelect, type_float,
                                   builder_->createBinOp(spv::OpFOrdEqual, type_bool, result,
                                                         builder_->makeFloatConstant(INFINITY)),
                                   builder_->makeFloatConstant(FLT_MAX), result);
    }
    case xenos::ucode::AluScalarOpcode::kRsqf: {
      spv::Id result = builder_->CreateUnaryBuiltinCall(
          type_float, ext_inst_glsl_std_450_, GLSLstd450InverseSqrt,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001));
      result = builder_->createTriOp(spv::OpSelect, type_float,
                                     builder_->createBinOp(spv::OpFOrdEqual, type_bool, result,
                                                           builder_->makeFloatConstant(INFINITY)),
                                     const_float_0, result);
      // Cannot create -0.0f with makeFloatConstant (float comparison), use the bit pattern.
      result = builder_->createTriOp(spv::OpSelect, type_uint,
                                     builder_->createBinOp(spv::OpFOrdEqual, type_bool, result,
                                                           builder_->makeFloatConstant(-INFINITY)),
                                     builder_->makeUintConstant(static_cast<u32>(INT32_MIN)),
                                     builder_->createUnaryOp(spv::OpBitcast, type_uint, result));
      return builder_->createUnaryOp(spv::OpBitcast, type_float, result);
    }

    case xenos::ucode::AluScalarOpcode::kSetpEq:
    case xenos::ucode::AluScalarOpcode::kSetpNe:
    case xenos::ucode::AluScalarOpcode::kSetpGt:
    case xenos::ucode::AluScalarOpcode::kSetpGe: {
      spv::Id predicate = builder_->createBinOp(
          static_cast<spv::Op>(kOps[static_cast<size_t>(instr.scalar_opcode)]), type_bool,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001),
          const_float_0);
      builder_->createStore(predicate, var_main_predicate_);
      predicate_written = true;
      return builder_->createTriOp(spv::OpSelect, type_float, predicate, const_float_0,
                                   const_float_1);
    }
    case xenos::ucode::AluScalarOpcode::kSetpInv: {
      spv::Id a = GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001);
      spv::Id predicate = builder_->createBinOp(spv::OpFOrdEqual, type_bool, a, const_float_1);
      builder_->createStore(predicate, var_main_predicate_);
      predicate_written = true;
      return builder_->createTriOp(
          spv::OpSelect, type_float, predicate, const_float_0,
          builder_->createTriOp(
              spv::OpSelect, type_float,
              builder_->createBinOp(spv::OpFOrdEqual, type_bool, a, const_float_0), const_float_1,
              a));
    }
    case xenos::ucode::AluScalarOpcode::kSetpPop: {
      spv::Id a_minus_1 = builder_->CreateNoContractionBinOp(
          spv::OpFSub, type_float,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001),
          const_float_1);
      spv::Id predicate =
          builder_->createBinOp(spv::OpFOrdLessThanEqual, type_bool, a_minus_1, const_float_0);
      builder_->createStore(predicate, var_main_predicate_);
      predicate_written = true;
      return builder_->createTriOp(spv::OpSelect, type_float, predicate, const_float_0, a_minus_1);
    }
    case xenos::ucode::AluScalarOpcode::kSetpClr:
      builder_->createStore(builder_->makeBoolConstant(false), var_main_predicate_);
      return builder_->makeFloatConstant(FLT_MAX);
    case xenos::ucode::AluScalarOpcode::kSetpRstr: {
      spv::Id a = GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001);
      spv::Id predicate = builder_->createBinOp(spv::OpFOrdEqual, type_bool, a, const_float_0);
      builder_->createStore(predicate, var_main_predicate_);
      predicate_written = true;
      return builder_->createTriOp(spv::OpSelect, type_float, predicate, const_float_0, a);
    }

    case xenos::ucode::AluScalarOpcode::kKillsEq:
    case xenos::ucode::AluScalarOpcode::kKillsGt:
    case xenos::ucode::AluScalarOpcode::kKillsGe:
    case xenos::ucode::AluScalarOpcode::kKillsNe:
    case xenos::ucode::AluScalarOpcode::kKillsOne: {
      KillPixel(
          builder_->createBinOp(
              static_cast<spv::Op>(kOps[static_cast<size_t>(instr.scalar_opcode)]), type_bool,
              GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001),
              instr.scalar_opcode == xenos::ucode::AluScalarOpcode::kKillsOne ? const_float_1
                                                                              : const_float_0),
          memexport_em_potentially_written_before);
      return const_float_0;
    }

    case xenos::ucode::AluScalarOpcode::kMulsc0:
    case xenos::ucode::AluScalarOpcode::kMulsc1: {
      spv::Id operand_0 =
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001);
      spv::Id operand_1 =
          GetOperandComponents(operand_storage[1], instr.scalar_operands[1], 0b0001);
      spv::Id result =
          builder_->CreateNoContractionBinOp(spv::OpFMul, type_float, operand_0, operand_1);
      if (!(instr.scalar_operands[0].GetIdenticalComponents(instr.scalar_operands[1]) & 0b0001)) {
        result =
            ZeroIfAnyOperandIsZero(result, GetAbsoluteOperand(operand_0, instr.scalar_operands[0]),
                                   GetAbsoluteOperand(operand_1, instr.scalar_operands[1]));
      }
      return result;
    }
    case xenos::ucode::AluScalarOpcode::kAddsc0:
    case xenos::ucode::AluScalarOpcode::kAddsc1:
    case xenos::ucode::AluScalarOpcode::kSubsc0:
    case xenos::ucode::AluScalarOpcode::kSubsc1: {
      return builder_->CreateNoContractionBinOp(
          static_cast<spv::Op>(kOps[static_cast<size_t>(instr.scalar_opcode)]), type_float,
          GetOperandComponents(operand_storage[0], instr.scalar_operands[0], 0b0001),
          GetOperandComponents(operand_storage[1], instr.scalar_operands[1], 0b0001));
    }

    case xenos::ucode::AluScalarOpcode::kRetainPrev:
      return spv::NoResult;
  }

  REX_UNHANDLED_CASE(instr.scalar_opcode);
  EmitTranslationError("Unknown ALU scalar operation");
  return spv::NoResult;
}

}  // namespace rex::gpu
