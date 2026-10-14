/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>

#include <rex/assert.h>
#include "gpu/xenos/pipeline/shader/interpreter.h"
#include <rex/xenos/registers.h>
#include <rex/xenos/xenos.h>
#include <rex/math.h>
#include <rex/types.h>

namespace rex::gpu {

void ShaderInterpreter::Execute() {
  state_.Reset();

  const u32* bool_constants =
      &register_file_[std::to_underlying(xenos::Register::SHADER_CONSTANT_BOOL_000_031)];

  bool exec_ended = false;
  u32 cf_index_next = 1;
  for (u32 cf_index = 0; !exec_ended; cf_index = cf_index_next) {
    cf_index_next = cf_index + 1;

    const u32* cf_pair = &ucode_[size_t{3} * (cf_index >> 1)];
    xenos::ucode::ControlFlowInstruction cf_instr;
    if (cf_index & 1) {
      cf_instr.dword_0 = (cf_pair[1] >> 16) | (cf_pair[2] << 16);
      cf_instr.dword_1 = cf_pair[2] >> 16;
    } else {
      cf_instr.dword_0 = cf_pair[0];
      cf_instr.dword_1 = cf_pair[1] & 0xFFFF;
    }

    xenos::ucode::ControlFlowOpcode cf_opcode = cf_instr.GetOpcode();
    switch (cf_opcode) {
      case xenos::ucode::ControlFlowOpcode::kNop: {
      } break;

      case xenos::ucode::ControlFlowOpcode::kExec:
      case xenos::ucode::ControlFlowOpcode::kExecEnd:
      case xenos::ucode::ControlFlowOpcode::kCondExec:
      case xenos::ucode::ControlFlowOpcode::kCondExecEnd:
      case xenos::ucode::ControlFlowOpcode::kCondExecPred:
      case xenos::ucode::ControlFlowOpcode::kCondExecPredEnd:
      case xenos::ucode::ControlFlowOpcode::kCondExecPredClean:
      case xenos::ucode::ControlFlowOpcode::kCondExecPredCleanEnd: {
        xenos::ucode::ControlFlowExecInstruction cf_exec =
            std::bit_cast<xenos::ucode::ControlFlowExecInstruction>(cf_instr);

        switch (cf_opcode) {
          case xenos::ucode::ControlFlowOpcode::kCondExec:
          case xenos::ucode::ControlFlowOpcode::kCondExecEnd:
          case xenos::ucode::ControlFlowOpcode::kCondExecPredClean:
          case xenos::ucode::ControlFlowOpcode::kCondExecPredCleanEnd: {
            const xenos::ucode::ControlFlowCondExecInstruction cf_cond_exec =
                std::bit_cast<xenos::ucode::ControlFlowCondExecInstruction>(cf_exec);
            u32 bool_address = cf_cond_exec.GetBoolAddress();
            if (cf_cond_exec.GetCondition() !=
                ((bool_constants[bool_address >> 5] & (UINT32_C(1) << (bool_address & 31))) != 0)) {
              continue;
            }
          } break;
          case xenos::ucode::ControlFlowOpcode::kCondExecPred:
          case xenos::ucode::ControlFlowOpcode::kCondExecPredEnd: {
            const xenos::ucode::ControlFlowCondExecPredInstruction cf_cond_exec_pred =
                *reinterpret_cast<const xenos::ucode::ControlFlowCondExecPredInstruction*>(
                    &cf_exec);
            if (cf_cond_exec_pred.GetCondition() != state_.predicate) {
              continue;
            }
          } break;
          default:
            break;
        }

        for (u32 exec_index = 0; exec_index < cf_exec.GetCount(); ++exec_index) {
          const u32* exec_instruction = &ucode_[size_t{3} * (cf_exec.GetAddress() + exec_index)];
          if ((cf_exec.GetSequence() >> (exec_index << 1)) & 0b01) {
            const xenos::ucode::FetchInstruction& fetch_instr =
                *reinterpret_cast<const xenos::ucode::FetchInstruction*>(exec_instruction);
            if (fetch_instr.IsPredicated() &&
                fetch_instr.GetPredicateCondition() != state_.predicate) {
              continue;
            }
            if (fetch_instr.GetOpcode() == xenos::ucode::FetchOpcode::kVertexFetch) {
              ExecuteVertexFetchInstruction(fetch_instr.GetVertexFetch());
            } else {
              f32 zero_result[4] = {};
              StoreFetchResult(fetch_instr.GetDest(), fetch_instr.IsDestRelative(),
                               fetch_instr.GetDestSwizzle(), zero_result);
            }
          } else {
            const xenos::ucode::AluInstruction& alu_instr =
                *reinterpret_cast<const xenos::ucode::AluInstruction*>(exec_instruction);
            if (alu_instr.IsPredicated() && alu_instr.GetPredicateCondition() != state_.predicate) {
              continue;
            }
            ExecuteAluInstruction(alu_instr);
          }
        }

        if (xenos::ucode::DoesControlFlowOpcodeEndShader(cf_opcode)) {
          exec_ended = true;
        }
      } break;

      case xenos::ucode::ControlFlowOpcode::kLoopStart: {
        xenos::ucode::ControlFlowLoopStartInstruction cf_loop_start =
            std::bit_cast<xenos::ucode::ControlFlowLoopStartInstruction>(cf_instr);
        REX_ASSERT(state_.loop_stack_depth < 4);
        if (++state_.loop_stack_depth > 4) {
          cf_index_next = cf_loop_start.GetAddress();
          continue;
        }
        auto loop_constant = register_file_.Get<xenos::LoopConstant>(
            std::to_underlying(xenos::Register::SHADER_CONSTANT_LOOP_00) +
            cf_loop_start.GetLoopId());
        state_.loop_constants[state_.loop_stack_depth] = loop_constant;
        u32& loop_iterator_ref = state_.loop_iterators[state_.loop_stack_depth];
        if (!cf_loop_start.IsRepeat()) {
          loop_iterator_ref = 0;
        }
        if (loop_iterator_ref >= loop_constant.count) {
          cf_index_next = cf_loop_start.GetAddress();
          continue;
        }
        ++state_.loop_stack_depth;
      } break;

      case xenos::ucode::ControlFlowOpcode::kLoopEnd: {
        REX_ASSERT(state_.loop_stack_depth != 0);
        if (!state_.loop_stack_depth) {
          continue;
        }
        REX_ASSERT(state_.loop_stack_depth <= 4);
        if (state_.loop_stack_depth > 4) {
          --state_.loop_stack_depth;
          continue;
        }
        xenos::ucode::ControlFlowLoopEndInstruction cf_loop_end =
            std::bit_cast<xenos::ucode::ControlFlowLoopEndInstruction>(cf_instr);
        xenos::LoopConstant loop_constant = state_.loop_constants[state_.loop_stack_depth - 1];
        REX_ASSERT(loop_constant.value ==
                   register_file_[std::to_underlying(xenos::Register::SHADER_CONSTANT_LOOP_00) +
                                  cf_loop_end.GetLoopId()]);
        u32 loop_iterator = ++state_.loop_iterators[state_.loop_stack_depth - 1];
        if (loop_iterator < loop_constant.count &&
            (!cf_loop_end.IsPredicatedBreak() || cf_loop_end.GetCondition() != state_.predicate)) {
          cf_index_next = cf_loop_end.GetAddress();
          continue;
        }
        --state_.loop_stack_depth;
      } break;

      case xenos::ucode::ControlFlowOpcode::kCondCall: {
        REX_ASSERT(state_.call_stack_depth < 4);
        if (state_.call_stack_depth >= 4) {
          continue;
        }
        const xenos::ucode::ControlFlowCondCallInstruction cf_cond_call =
            std::bit_cast<xenos::ucode::ControlFlowCondCallInstruction>(cf_instr);
        if (!cf_cond_call.IsUnconditional()) {
          if (cf_cond_call.IsPredicated()) {
            if (cf_cond_call.GetCondition() != state_.predicate) {
              continue;
            }
          } else {
            u32 bool_address = cf_cond_call.GetBoolAddress();
            if (cf_cond_call.GetCondition() !=
                ((bool_constants[bool_address >> 5] & (UINT32_C(1) << (bool_address & 31))) != 0)) {
              continue;
            }
          }
        }
        state_.call_return_addresses[state_.call_stack_depth++] = cf_index + 1;
        cf_index_next = cf_cond_call.GetAddress();
      } break;

      case xenos::ucode::ControlFlowOpcode::kReturn: {
        // Skipping a return outside a function call is well defined, so no stack depth assertion.
        if (!state_.call_stack_depth) {
          continue;
        }
        cf_index_next = state_.call_return_addresses[--state_.call_stack_depth];
      } break;

      case xenos::ucode::ControlFlowOpcode::kCondJmp: {
        const xenos::ucode::ControlFlowCondJmpInstruction cf_cond_jmp =
            std::bit_cast<xenos::ucode::ControlFlowCondJmpInstruction>(cf_instr);
        if (!cf_cond_jmp.IsUnconditional()) {
          if (cf_cond_jmp.IsPredicated()) {
            if (cf_cond_jmp.GetCondition() != state_.predicate) {
              continue;
            }
          } else {
            u32 bool_address = cf_cond_jmp.GetBoolAddress();
            if (cf_cond_jmp.GetCondition() !=
                ((bool_constants[bool_address >> 5] & (UINT32_C(1) << (bool_address & 31))) != 0)) {
              continue;
            }
          }
        }
        cf_index_next = cf_cond_jmp.GetAddress();
      } break;

      case xenos::ucode::ControlFlowOpcode::kAlloc: {
        if (export_sink_) {
          const auto cf_alloc = std::bit_cast<xenos::ucode::ControlFlowAllocInstruction>(cf_instr);
          export_sink_->AllocExport(cf_alloc.GetAllocType(), cf_alloc.GetSize());
        }
      } break;

      case xenos::ucode::ControlFlowOpcode::kMarkVsFetchDone: {
      } break;

      default:
        REX_UNHANDLED_CASE(cf_opcode);
    }
  }
}

const std::array<f32, 4> ShaderInterpreter::GetFloatConstant(u32 address, bool is_relative,
                                                             bool relative_address_is_a0) const {
  i32 index = static_cast<i32>(address);
  if (is_relative) {
    index += relative_address_is_a0 ? state_.address_register : state_.GetLoopAddress();
  }
  if (index < 0) {
    return std::array<f32, 4>();
  }
  auto base_and_size_minus_1 = register_file_.Get<xenos::reg::SQ_VS_CONST>(
      shader_type_ == xenos::ShaderType::kVertex ? xenos::Register::SQ_VS_CONST
                                                 : xenos::Register::SQ_PS_CONST);
  if (static_cast<u32>(index) > base_and_size_minus_1.size) {
    return std::array<f32, 4>();
  }
  index += base_and_size_minus_1.base;
  if (index >= 512) {
    return std::array<f32, 4>();
  }
  std::array<f32, 4> value;
  std::memcpy(
      value.data(),
      &register_file_[std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X) + 4 * index],
      sizeof(f32) * 4);
  return value;
}

void ShaderInterpreter::ExecuteAluInstruction(xenos::ucode::AluInstruction instr) {
  f32 vector_result[4] = {};
  xenos::ucode::AluVectorOpcode vector_opcode = instr.GetVectorOpcode();
  const xenos::ucode::AluVectorOpcodeInfo& vector_opcode_info =
      xenos::ucode::GetAluVectorOpcodeInfo(vector_opcode);
  u32 vector_result_write_mask = instr.GetVectorOpResultWriteMask();
  if (vector_result_write_mask || vector_opcode_info.changed_state) {
    f32 vector_operands[3][4];
    for (u32 i = 0; i < 3; ++i) {
      if (!vector_opcode_info.operand_components_used[i]) {
        continue;
      }
      const f32* vector_src_ptr;
      u32 vector_src_register = instr.GetSrcReg(1 + i);
      bool vector_src_absolute = false;
      std::array<f32, 4> vector_src_float_constant;
      if (instr.IsSrcTemp(1 + i)) {
        vector_src_ptr =
            GetTempRegister(xenos::ucode::AluInstruction::GetSrcTempReg(vector_src_register),
                            xenos::ucode::AluInstruction::IsSrcTempRelative(vector_src_register));
        vector_src_absolute =
            xenos::ucode::AluInstruction::IsSrcTempValueAbsolute(vector_src_register);
      } else {
        vector_src_float_constant =
            GetFloatConstant(vector_src_register, instr.IsSrcConstAddressed(1 + i),
                             instr.IsConstAddressRegisterRelative());
        vector_src_ptr = vector_src_float_constant.data();
      }
      u32 vector_src_absolute_mask = ~(static_cast<u32>(vector_src_absolute) << 31);
      u32 vector_src_negate_bit = static_cast<u32>(instr.IsSrcNegated(1 + i)) << 31;
      u32 vector_src_swizzle = instr.GetSrcSwizzle(1 + i);
      for (u32 j = 0; j < 4; ++j) {
        f32 vector_src_component =
            FlushDenormal(vector_src_ptr[xenos::ucode::AluInstruction::GetSwizzledComponentIndex(
                vector_src_swizzle, j)]);
        vector_src_component = std::bit_cast<f32>(
            (std::bit_cast<u32>(vector_src_component) & vector_src_absolute_mask) ^
            vector_src_negate_bit);
        vector_operands[i][j] = vector_src_component;
      }
    }

    bool replicate_vector_result_x = false;
    switch (vector_opcode) {
      case xenos::ucode::AluVectorOpcode::kAdd: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] = vector_operands[0][i] + vector_operands[1][i];
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kMul: {
        for (u32 i = 0; i < 4; ++i) {
          // Direct3D 9 gives +0 for 0 or denormal * anything.
          vector_result[i] = (vector_operands[0][i] != 0.0f && vector_operands[1][i] != 0.0f)
                                 ? vector_operands[0][i] * vector_operands[1][i]
                                 : 0.0f;
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kMax: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] = std::isgreaterequal(vector_operands[0][i], vector_operands[1][i])
                                 ? vector_operands[0][i]
                                 : vector_operands[1][i];
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kMin: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] = std::isless(vector_operands[0][i], vector_operands[1][i])
                                 ? vector_operands[0][i]
                                 : vector_operands[1][i];
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kSeq: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] = static_cast<f32>(vector_operands[0][i] == vector_operands[1][i]);
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kSgt: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] =
              static_cast<f32>(std::isgreater(vector_operands[0][i], vector_operands[1][i]));
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kSge: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] =
              static_cast<f32>(std::isgreaterequal(vector_operands[0][i], vector_operands[1][i]));
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kSne: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] = static_cast<f32>(vector_operands[0][i] != vector_operands[1][i]);
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kFrc: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] = vector_operands[0][i] - std::floor(vector_operands[0][i]);
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kTrunc: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] = std::trunc(vector_operands[0][i]);
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kFloor: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] = std::floor(vector_operands[0][i]);
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kMad: {
        for (u32 i = 0; i < 4; ++i) {
          // D3D9 0 or denormal * anything is +0 and +0 + -0 must be +0, so add is unconditional.
          vector_result[i] = ((vector_operands[0][i] != 0.0f && vector_operands[1][i] != 0.0f)
                                  ? vector_operands[0][i] * vector_operands[1][i]
                                  : 0.0f) +
                             vector_operands[2][i];
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kCndEq: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] =
              vector_operands[0][i] == 0.0f ? vector_operands[1][i] : vector_operands[2][i];
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kCndGe: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] = std::isgreaterequal(vector_operands[0][i], 0.0f)
                                 ? vector_operands[1][i]
                                 : vector_operands[2][i];
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kCndGt: {
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] = std::isgreater(vector_operands[0][i], 0.0f) ? vector_operands[1][i]
                                                                         : vector_operands[2][i];
        }
      } break;
      case xenos::ucode::AluVectorOpcode::kDp4: {
        vector_result[0] = 0.0f;
        for (u32 i = 0; i < 4; ++i) {
          // D3D9 0 or denormal * anything is +0 and +0 + -0 must be +0, so add is unconditional.
          vector_result[0] += (vector_operands[0][i] != 0.0f && vector_operands[1][i] != 0.0f)
                                  ? vector_operands[0][i] * vector_operands[1][i]
                                  : 0.0f;
        }
        replicate_vector_result_x = true;
      } break;
      case xenos::ucode::AluVectorOpcode::kDp3: {
        vector_result[0] = 0.0f;
        for (u32 i = 0; i < 3; ++i) {
          // D3D9 0 or denormal * anything is +0 and +0 + -0 must be +0, so add is unconditional.
          vector_result[0] += (vector_operands[0][i] != 0.0f && vector_operands[1][i] != 0.0f)
                                  ? vector_operands[0][i] * vector_operands[1][i]
                                  : 0.0f;
        }
        replicate_vector_result_x = true;
      } break;
      case xenos::ucode::AluVectorOpcode::kDp2Add: {
        // Add even for zero operands because +0 + -0 must be +0.
        vector_result[0] = 0.0f;
        for (u32 i = 0; i < 2; ++i) {
          // Direct3D 9 gives +0 for 0 or denormal * anything.
          vector_result[0] += (vector_operands[0][i] != 0.0f && vector_operands[1][i] != 0.0f)
                                  ? vector_operands[0][i] * vector_operands[1][i]
                                  : 0.0f;
        }
        vector_result[0] += vector_operands[2][0];
        replicate_vector_result_x = true;
      } break;
      case xenos::ucode::AluVectorOpcode::kCube: {
        // Operand [0] is .z_xy.
        f32 x = vector_operands[0][2];
        f32 y = vector_operands[0][3];
        f32 z = vector_operands[0][0];
        f32 x_abs = std::abs(x), y_abs = std::abs(y), z_abs = std::abs(z);
        // Result is T coordinate, S coordinate, 2 * major axis, face ID.
        if (z_abs >= x_abs && z_abs >= y_abs) {
          bool z_negative = std::isless(z, 0.0f);
          vector_result[0] = -y;
          vector_result[1] = z_negative ? -x : x;
          vector_result[2] = z;
          vector_result[3] = z_negative ? 5.0f : 4.0f;
        } else if (y_abs >= x_abs) {
          bool y_negative = std::isless(y, 0.0f);
          vector_result[0] = y_negative ? -z : z;
          vector_result[1] = x;
          vector_result[2] = y;
          vector_result[3] = y_negative ? 3.0f : 2.0f;
        } else {
          bool x_negative = std::isless(x, 0.0f);
          vector_result[0] = -y;
          vector_result[1] = x_negative ? z : -z;
          vector_result[2] = x;
          vector_result[3] = x_negative ? 1.0f : 0.0f;
        }
        vector_result[2] *= 2.0f;
      } break;
      case xenos::ucode::AluVectorOpcode::kMax4: {
        if (std::isgreaterequal(vector_operands[0][0], vector_operands[0][1]) &&
            std::isgreaterequal(vector_operands[0][0], vector_operands[0][2]) &&
            std::isgreaterequal(vector_operands[0][0], vector_operands[0][3])) {
          vector_result[0] = vector_operands[0][0];
        } else if (std::isgreaterequal(vector_operands[0][1], vector_operands[0][2]) &&
                   std::isgreaterequal(vector_operands[0][1], vector_operands[0][3])) {
          vector_result[0] = vector_operands[0][1];
        } else if (std::isgreaterequal(vector_operands[0][2], vector_operands[0][3])) {
          vector_result[0] = vector_operands[0][2];
        } else {
          vector_result[0] = vector_operands[0][3];
        }
        replicate_vector_result_x = true;
      } break;
      case xenos::ucode::AluVectorOpcode::kSetpEqPush: {
        state_.predicate = vector_operands[0][3] == 0.0f && vector_operands[1][3] == 0.0f;
        vector_result[0] = (vector_operands[0][0] == 0.0f && vector_operands[1][0] == 0.0f)
                               ? 0.0f
                               : vector_operands[0][0] + 1.0f;
        replicate_vector_result_x = true;
      } break;
      case xenos::ucode::AluVectorOpcode::kSetpNePush: {
        state_.predicate = vector_operands[0][3] == 0.0f && vector_operands[1][3] != 0.0f;
        vector_result[0] = (vector_operands[0][0] == 0.0f && vector_operands[1][0] != 0.0f)
                               ? 0.0f
                               : vector_operands[0][0] + 1.0f;
        replicate_vector_result_x = true;
      } break;
      case xenos::ucode::AluVectorOpcode::kSetpGtPush: {
        state_.predicate =
            vector_operands[0][3] == 0.0f && std::isgreater(vector_operands[1][3], 0.0f);
        vector_result[0] =
            (vector_operands[0][0] == 0.0f && std::isgreater(vector_operands[1][0], 0.0f))
                ? 0.0f
                : vector_operands[0][0] + 1.0f;
        replicate_vector_result_x = true;
      } break;
      case xenos::ucode::AluVectorOpcode::kSetpGePush: {
        state_.predicate =
            vector_operands[0][3] == 0.0f && std::isgreaterequal(vector_operands[1][3], 0.0f);
        vector_result[0] =
            (vector_operands[0][0] == 0.0f && std::isgreaterequal(vector_operands[1][0], 0.0f))
                ? 0.0f
                : vector_operands[0][0] + 1.0f;
        replicate_vector_result_x = true;
      } break;
      case xenos::ucode::AluVectorOpcode::kKillEq: {
        vector_result[0] = static_cast<f32>(vector_operands[0][0] == vector_operands[1][0] ||
                                            vector_operands[0][1] == vector_operands[1][1] ||
                                            vector_operands[0][2] == vector_operands[1][2] ||
                                            vector_operands[0][3] == vector_operands[1][3]);
        replicate_vector_result_x = true;
      } break;
      case xenos::ucode::AluVectorOpcode::kKillGt: {
        vector_result[0] =
            static_cast<f32>(std::isgreater(vector_operands[0][0], vector_operands[1][0]) ||
                             std::isgreater(vector_operands[0][1], vector_operands[1][1]) ||
                             std::isgreater(vector_operands[0][2], vector_operands[1][2]) ||
                             std::isgreater(vector_operands[0][3], vector_operands[1][3]));
        replicate_vector_result_x = true;
      } break;
      case xenos::ucode::AluVectorOpcode::kKillGe: {
        vector_result[0] =
            static_cast<f32>(std::isgreaterequal(vector_operands[0][0], vector_operands[1][0]) ||
                             std::isgreaterequal(vector_operands[0][1], vector_operands[1][1]) ||
                             std::isgreaterequal(vector_operands[0][2], vector_operands[1][2]) ||
                             std::isgreaterequal(vector_operands[0][3], vector_operands[1][3]));
        replicate_vector_result_x = true;
      } break;
      case xenos::ucode::AluVectorOpcode::kKillNe: {
        vector_result[0] = static_cast<f32>(vector_operands[0][0] != vector_operands[1][0] ||
                                            vector_operands[0][1] != vector_operands[1][1] ||
                                            vector_operands[0][2] != vector_operands[1][2] ||
                                            vector_operands[0][3] != vector_operands[1][3]);
        replicate_vector_result_x = true;
      } break;
      case xenos::ucode::AluVectorOpcode::kDst: {
        vector_result[0] = 1.0f;
        // Direct3D 9 gives +0 for 0 or denormal * anything.
        vector_result[1] = (vector_operands[0][1] != 0.0f && vector_operands[1][1] != 0.0f)
                               ? vector_operands[0][1] * vector_operands[1][1]
                               : 0.0f;
        vector_result[2] = vector_operands[0][2];
        vector_result[3] = vector_operands[1][3];
      } break;
      case xenos::ucode::AluVectorOpcode::kMaxA: {
        state_.address_register = static_cast<i32>(
            std::floor(rex::clamp_float(vector_operands[0][3], -256.0f, 255.0f) + 0.5f));
        for (u32 i = 0; i < 4; ++i) {
          vector_result[i] = std::isgreaterequal(vector_operands[0][i], vector_operands[1][i])
                                 ? vector_operands[0][i]
                                 : vector_operands[1][i];
        }
      } break;
      default: {
        REX_UNHANDLED_CASE(vector_opcode);
      }
    }
    if (replicate_vector_result_x) {
      for (u32 i = 1; i < 4; ++i) {
        vector_result[i] = vector_result[0];
      }
    }
  }

  xenos::ucode::AluScalarOpcode scalar_opcode = instr.GetScalarOpcode();
  const xenos::ucode::AluScalarOpcodeInfo& scalar_opcode_info =
      xenos::ucode::GetAluScalarOpcodeInfo(scalar_opcode);
  f32 scalar_operands[2];
  u32 scalar_operand_component_count = 0;
  bool scalar_src_absolute = false;
  switch (scalar_opcode_info.operand_count) {
    case 1: {
      // r#/c#.w or r#/c#.wx.
      const f32* scalar_src_ptr;
      u32 scalar_src_register = instr.GetSrcReg(3);
      std::array<f32, 4> scalar_src_float_constant;
      if (instr.IsSrcTemp(3)) {
        scalar_src_ptr =
            GetTempRegister(xenos::ucode::AluInstruction::GetSrcTempReg(scalar_src_register),
                            xenos::ucode::AluInstruction::IsSrcTempRelative(scalar_src_register));
        scalar_src_absolute =
            xenos::ucode::AluInstruction::IsSrcTempValueAbsolute(scalar_src_register);
      } else {
        scalar_src_float_constant =
            GetFloatConstant(scalar_src_register, instr.IsSrcConstAddressed(3),
                             instr.IsConstAddressRegisterRelative());
        scalar_src_ptr = scalar_src_float_constant.data();
      }
      u32 scalar_src_swizzle = instr.GetSrcSwizzle(3);
      scalar_operand_component_count = scalar_opcode_info.single_operand_is_two_component ? 2 : 1;
      for (u32 i = 0; i < scalar_operand_component_count; ++i) {
        scalar_operands[i] = scalar_src_ptr[xenos::ucode::AluInstruction::GetSwizzledComponentIndex(
            scalar_src_swizzle, (3 + i) & 3)];
      }
    } break;
    case 2: {
      scalar_operand_component_count = 2;
      scalar_src_absolute = instr.GetAbsConstants();
      u32 scalar_src_swizzle = instr.GetSrcSwizzle(3);
      // c#.w.
      scalar_operands[0] = GetFloatConstant(instr.GetSrcReg(3), instr.IsSrcConstAddressed(3),
                                            instr.IsConstAddressRegisterRelative())
          [xenos::ucode::AluInstruction::GetSwizzledComponentIndex(scalar_src_swizzle, 3)];
      // r#.x.
      scalar_operands[1] = GetTempRegister(
          instr.GetScalarConstRegOpSrcTempReg(),
          false)[xenos::ucode::AluInstruction::GetSwizzledComponentIndex(scalar_src_swizzle, 0)];
    } break;
    default:
      break;
  }
  if (scalar_operand_component_count) {
    u32 scalar_src_absolute_mask = ~(static_cast<u32>(scalar_src_absolute) << 31);
    u32 scalar_src_negate_bit = static_cast<u32>(instr.IsSrcNegated(3)) << 31;
    for (u32 i = 0; i < scalar_operand_component_count; ++i) {
      f32 scalar_operand = FlushDenormal(scalar_operands[i]);
      scalar_operand = std::bit_cast<f32>(
          (std::bit_cast<u32>(scalar_operand) & scalar_src_absolute_mask) ^ scalar_src_negate_bit);
      scalar_operands[i] = scalar_operand;
    }
  }
  switch (scalar_opcode) {
    case xenos::ucode::AluScalarOpcode::kAdds:
    case xenos::ucode::AluScalarOpcode::kAddsc0:
    case xenos::ucode::AluScalarOpcode::kAddsc1: {
      state_.previous_scalar = scalar_operands[0] + scalar_operands[1];
    } break;
    case xenos::ucode::AluScalarOpcode::kAddsPrev: {
      state_.previous_scalar = scalar_operands[0] + state_.previous_scalar;
    } break;
    case xenos::ucode::AluScalarOpcode::kMuls:
    case xenos::ucode::AluScalarOpcode::kMulsc0:
    case xenos::ucode::AluScalarOpcode::kMulsc1: {
      // Direct3D 9 gives +0 for 0 or denormal * anything.
      state_.previous_scalar = (scalar_operands[0] != 0.0f && scalar_operands[1] != 0.0f)
                                   ? scalar_operands[0] * scalar_operands[1]
                                   : 0.0f;
    } break;
    case xenos::ucode::AluScalarOpcode::kMulsPrev: {
      // Direct3D 9 gives +0 for 0 or denormal * anything.
      state_.previous_scalar = (scalar_operands[0] != 0.0f && state_.previous_scalar != 0.0f)
                                   ? scalar_operands[0] * state_.previous_scalar
                                   : 0.0f;
    } break;
    case xenos::ucode::AluScalarOpcode::kMulsPrev2: {
      if (state_.previous_scalar == -FLT_MAX || !std::isfinite(state_.previous_scalar) ||
          !std::isfinite(scalar_operands[1]) || std::islessequal(scalar_operands[1], 0.0f)) {
        state_.previous_scalar = -FLT_MAX;
      } else {
        // Direct3D 9 gives +0 for 0 or denormal * anything.
        state_.previous_scalar = (scalar_operands[0] != 0.0f && state_.previous_scalar != 0.0f)
                                     ? scalar_operands[0] * state_.previous_scalar
                                     : 0.0f;
      }
    } break;
    case xenos::ucode::AluScalarOpcode::kMaxs: {
      state_.previous_scalar = std::isgreaterequal(scalar_operands[0], scalar_operands[1])
                                   ? scalar_operands[0]
                                   : scalar_operands[1];
    } break;
    case xenos::ucode::AluScalarOpcode::kMins: {
      state_.previous_scalar = std::isless(scalar_operands[0], scalar_operands[1])
                                   ? scalar_operands[0]
                                   : scalar_operands[1];
    } break;
    case xenos::ucode::AluScalarOpcode::kSeqs: {
      state_.previous_scalar = static_cast<f32>(scalar_operands[0] == 0.0f);
    } break;
    case xenos::ucode::AluScalarOpcode::kSgts: {
      state_.previous_scalar = static_cast<f32>(std::isgreater(scalar_operands[0], 0.0f));
    } break;
    case xenos::ucode::AluScalarOpcode::kSges: {
      state_.previous_scalar = static_cast<f32>(std::isgreaterequal(scalar_operands[0], 0.0f));
    } break;
    case xenos::ucode::AluScalarOpcode::kSnes: {
      state_.previous_scalar = static_cast<f32>(scalar_operands[0] != 0.0f);
    } break;
    case xenos::ucode::AluScalarOpcode::kFrcs: {
      state_.previous_scalar = scalar_operands[0] - std::floor(scalar_operands[0]);
    } break;
    case xenos::ucode::AluScalarOpcode::kTruncs: {
      state_.previous_scalar = std::trunc(scalar_operands[0]);
    } break;
    case xenos::ucode::AluScalarOpcode::kFloors: {
      state_.previous_scalar = std::floor(scalar_operands[0]);
    } break;
    case xenos::ucode::AluScalarOpcode::kExp: {
      state_.previous_scalar = std::exp2(scalar_operands[0]);
    } break;
    case xenos::ucode::AluScalarOpcode::kLogc: {
      state_.previous_scalar = std::log2(scalar_operands[0]);
      if (state_.previous_scalar == -INFINITY) {
        state_.previous_scalar = -FLT_MAX;
      }
    } break;
    case xenos::ucode::AluScalarOpcode::kLog: {
      state_.previous_scalar = std::log2(scalar_operands[0]);
    } break;
    case xenos::ucode::AluScalarOpcode::kRcpc: {
      state_.previous_scalar = 1.0f / scalar_operands[0];
      if (state_.previous_scalar == -INFINITY) {
        state_.previous_scalar = -FLT_MAX;
      } else if (state_.previous_scalar == INFINITY) {
        state_.previous_scalar = FLT_MAX;
      }
    } break;
    case xenos::ucode::AluScalarOpcode::kRcpf: {
      state_.previous_scalar = 1.0f / scalar_operands[0];
      if (state_.previous_scalar == -INFINITY) {
        state_.previous_scalar = -0.0f;
      } else if (state_.previous_scalar == INFINITY) {
        state_.previous_scalar = 0.0f;
      }
    } break;
    case xenos::ucode::AluScalarOpcode::kRcp: {
      state_.previous_scalar = 1.0f / scalar_operands[0];
    } break;
    case xenos::ucode::AluScalarOpcode::kRsqc: {
      state_.previous_scalar = 1.0f / std::sqrt(scalar_operands[0]);
      if (state_.previous_scalar == -INFINITY) {
        state_.previous_scalar = -FLT_MAX;
      } else if (state_.previous_scalar == INFINITY) {
        state_.previous_scalar = FLT_MAX;
      }
    } break;
    case xenos::ucode::AluScalarOpcode::kRsqf: {
      state_.previous_scalar = 1.0f / std::sqrt(scalar_operands[0]);
      if (state_.previous_scalar == -INFINITY) {
        state_.previous_scalar = -0.0f;
      } else if (state_.previous_scalar == INFINITY) {
        state_.previous_scalar = 0.0f;
      }
    } break;
    case xenos::ucode::AluScalarOpcode::kRsq: {
      state_.previous_scalar = 1.0f / std::sqrt(scalar_operands[0]);
    } break;
    case xenos::ucode::AluScalarOpcode::kMaxAs: {
      state_.address_register = static_cast<i32>(
          std::floor(rex::clamp_float(scalar_operands[0], -256.0f, 255.0f) + 0.5f));
      state_.previous_scalar = std::isgreaterequal(scalar_operands[0], scalar_operands[1])
                                   ? scalar_operands[0]
                                   : scalar_operands[1];
    } break;
    case xenos::ucode::AluScalarOpcode::kMaxAsf: {
      state_.address_register =
          static_cast<i32>(std::floor(rex::clamp_float(scalar_operands[0], -256.0f, 255.0f)));
      state_.previous_scalar = std::isgreaterequal(scalar_operands[0], scalar_operands[1])
                                   ? scalar_operands[0]
                                   : scalar_operands[1];
    } break;
    case xenos::ucode::AluScalarOpcode::kSubs:
    case xenos::ucode::AluScalarOpcode::kSubsc0:
    case xenos::ucode::AluScalarOpcode::kSubsc1: {
      state_.previous_scalar = scalar_operands[0] - scalar_operands[1];
    } break;
    case xenos::ucode::AluScalarOpcode::kSubsPrev: {
      state_.previous_scalar = scalar_operands[0] - state_.previous_scalar;
    } break;
    case xenos::ucode::AluScalarOpcode::kSetpEq: {
      state_.predicate = scalar_operands[0] == 0.0f;
      state_.previous_scalar = static_cast<f32>(!state_.predicate);
    } break;
    case xenos::ucode::AluScalarOpcode::kSetpNe: {
      state_.predicate = scalar_operands[0] != 0.0f;
      state_.previous_scalar = static_cast<f32>(!state_.predicate);
    } break;
    case xenos::ucode::AluScalarOpcode::kSetpGt: {
      state_.predicate = std::isgreater(scalar_operands[0], 0.0f);
      state_.previous_scalar = static_cast<f32>(!state_.predicate);
    } break;
    case xenos::ucode::AluScalarOpcode::kSetpGe: {
      state_.predicate = std::isgreaterequal(scalar_operands[0], 0.0f);
      state_.previous_scalar = static_cast<f32>(!state_.predicate);
    } break;
    case xenos::ucode::AluScalarOpcode::kSetpInv: {
      state_.predicate = scalar_operands[0] == 1.0f;
      state_.previous_scalar =
          state_.predicate ? 0.0f : (scalar_operands[0] == 0.0f ? 1.0f : scalar_operands[0]);
    } break;
    case xenos::ucode::AluScalarOpcode::kSetpPop: {
      f32 new_counter = scalar_operands[0] - 1.0f;
      state_.predicate = std::islessequal(new_counter, 0.0f);
      state_.previous_scalar = state_.predicate ? 0.0f : new_counter;
    } break;
    case xenos::ucode::AluScalarOpcode::kSetpClr: {
      state_.predicate = false;
      state_.previous_scalar = FLT_MAX;
    } break;
    case xenos::ucode::AluScalarOpcode::kSetpRstr: {
      state_.predicate = scalar_operands[0] == 0.0f;
      state_.previous_scalar = state_.predicate ? 0.0f : scalar_operands[0];
    } break;
    case xenos::ucode::AluScalarOpcode::kKillsEq: {
      state_.previous_scalar = static_cast<f32>(scalar_operands[0] == 0.0f);
    } break;
    case xenos::ucode::AluScalarOpcode::kKillsGt: {
      state_.previous_scalar = static_cast<f32>(std::isgreater(scalar_operands[0], 0.0f));
    } break;
    case xenos::ucode::AluScalarOpcode::kKillsGe: {
      state_.previous_scalar = static_cast<f32>(std::isgreaterequal(scalar_operands[0], 0.0f));
    } break;
    case xenos::ucode::AluScalarOpcode::kKillsNe: {
      state_.previous_scalar = static_cast<f32>(scalar_operands[0] != 0.0f);
    } break;
    case xenos::ucode::AluScalarOpcode::kKillsOne: {
      state_.previous_scalar = static_cast<f32>(scalar_operands[0] == 1.0f);
    } break;
    case xenos::ucode::AluScalarOpcode::kSqrt: {
      state_.previous_scalar = std::sqrt(scalar_operands[0]);
    } break;
    case xenos::ucode::AluScalarOpcode::kSin: {
      state_.previous_scalar = std::sin(scalar_operands[0]);
    } break;
    case xenos::ucode::AluScalarOpcode::kCos: {
      state_.previous_scalar = std::cos(scalar_operands[0]);
    } break;
    case xenos::ucode::AluScalarOpcode::kRetainPrev: {
    } break;
    default: {
      REX_UNHANDLED_CASE(scalar_opcode);
    }
  }

  if (instr.GetVectorClamp()) {
    for (u32 i = 0; i < 4; ++i) {
      vector_result[i] = rex::saturate(vector_result[i]);
    }
  }
  f32 scalar_result =
      instr.GetScalarClamp() ? rex::saturate(state_.previous_scalar) : state_.previous_scalar;

  u32 scalar_result_write_mask = instr.GetScalarOpResultWriteMask();
  if (instr.IsExport()) {
    if (export_sink_) {
      f32 export_value[4];
      u32 export_constant_1_mask = instr.GetConstant1WriteMask();
      u32 export_mask = vector_result_write_mask | scalar_result_write_mask |
                        instr.GetConstant0WriteMask() | export_constant_1_mask;
      for (u32 i = 0; i < 4; ++i) {
        u32 export_component_bit = UINT32_C(1) << i;
        f32 export_component = 0.0f;
        if (vector_result_write_mask & export_component_bit) {
          export_component = vector_result[i];
        } else if (scalar_result_write_mask & export_component_bit) {
          export_component = scalar_result;
        } else if (export_constant_1_mask & export_component_bit) {
          export_component = 1.0f;
        } else {
          export_component = 0.0f;
        }
        export_value[i] = export_component;
      }
      export_sink_->Export(xenos::ucode::ExportRegister(instr.GetVectorDest()), export_value,
                           export_mask);
    }
  } else {
    if (vector_result_write_mask) {
      f32* vector_dest = GetTempRegister(instr.GetVectorDest(), instr.IsVectorDestRelative());
      for (u32 i = 0; i < 4; ++i) {
        if (vector_result_write_mask & (UINT32_C(1) << i)) {
          vector_dest[i] = vector_result[i];
        }
      }
    }
    if (scalar_result_write_mask) {
      f32* scalar_dest = GetTempRegister(instr.GetScalarDest(), instr.IsScalarDestRelative());
      for (u32 i = 0; i < 4; ++i) {
        if (scalar_result_write_mask & (UINT32_C(1) << i)) {
          scalar_dest[i] = scalar_result;
        }
      }
    }
  }
}

void ShaderInterpreter::StoreFetchResult(u32 dest, bool is_dest_relative, u32 swizzle,
                                         const f32* value) {
  f32* dest_data = GetTempRegister(dest, is_dest_relative);
  for (u32 i = 0; i < 4; ++i) {
    xenos::ucode::FetchDestinationSwizzle component_swizzle =
        xenos::ucode::GetFetchDestinationComponentSwizzle(swizzle, i);
    switch (component_swizzle) {
      case xenos::ucode::FetchDestinationSwizzle::kX:
        dest_data[i] = value[0];
        break;
      case xenos::ucode::FetchDestinationSwizzle::kY:
        dest_data[i] = value[1];
        break;
      case xenos::ucode::FetchDestinationSwizzle::kZ:
        dest_data[i] = value[2];
        break;
      case xenos::ucode::FetchDestinationSwizzle::kW:
        dest_data[i] = value[3];
        break;
      case xenos::ucode::FetchDestinationSwizzle::k1:
        dest_data[i] = 1.0f;
        break;
      case xenos::ucode::FetchDestinationSwizzle::kKeep:
        break;
      default:
        // k0 or the invalid swizzle 6.
        REX_ASSERT(component_swizzle == xenos::ucode::FetchDestinationSwizzle::k0);
        dest_data[i] = 0.0f;
        break;
    }
  }
}

void ShaderInterpreter::ExecuteVertexFetchInstruction(xenos::ucode::VertexFetchInstruction instr) {
  if (!instr.IsMiniFetch()) {
    state_.vfetch_full_last = instr;
  }

  xenos::VertexFetch fetch_constant =
      register_file_.GetVertexFetch(state_.vfetch_full_last.GetFetchConstantIndex());

  if (!instr.IsMiniFetch()) {
    u32 vertex_index = static_cast<u32>(
        std::floor(GetTempRegister(instr.GetSrc(), instr.IsSrcRelative())[instr.GetSrcSwizzle()] +
                   (instr.IsIndexRounded() ? 0.5f : 0.0f)));
    state_.vfetch_address_dwords = instr.GetStride() * vertex_index + fetch_constant.address;
  }

  f32 result[4] = {};
  u32 dest_swizzle = instr.GetDestSwizzle();
  u32 used_result_components = 0b0000;
  for (u32 i = 0; i < 4; ++i) {
    u32 dest_component_swizzle = (dest_swizzle >> (3 * i)) & 0b111;
    if (dest_component_swizzle <= 3) {
      used_result_components |= UINT32_C(1) << dest_component_swizzle;
    }
  }
  u32 needed_dwords =
      xenos::GetVertexFormatNeededWords(instr.GetDataFormat(), used_result_components);
  if (needed_dwords) {
    u32 data[4] = {};
    const u32* memory_dwords = reinterpret_cast<const u32*>(memory_.GetPhysicalMembase());
    u32 buffer_end_dwords = fetch_constant.address + fetch_constant.size;
    u32 dword_0_address_dwords =
        static_cast<u32>(static_cast<i32>(state_.vfetch_address_dwords) + instr.GetOffset());
    for (u32 i = 0; i < 4; ++i) {
      if (!(needed_dwords & (UINT32_C(1) << i))) {
        continue;
      }
      u32 dword_value = 0;
      u32 dword_address_dwords = dword_0_address_dwords + i;
      if (dword_address_dwords >= fetch_constant.address &&
          dword_address_dwords < buffer_end_dwords) {
        dword_value = xenos::GPUSwap(memory_dwords[dword_address_dwords], fetch_constant.endian);
      }
      data[i] = dword_value;
    }

    u32 packed_components = 0b0000;
    u32 packed_widths[4], packed_offsets[4];
    u32 packed_dwords[] = {data[0], data[0]};
    switch (instr.GetDataFormat()) {
      case xenos::VertexFormat::k_8_8_8_8: {
        packed_components = 0b1111;
        packed_widths[0] = packed_widths[1] = packed_widths[2] = packed_widths[3] = 8;
        packed_offsets[1] = 8;
        packed_offsets[2] = 16;
        packed_offsets[3] = 24;
      } break;
      case xenos::VertexFormat::k_2_10_10_10: {
        packed_components = 0b1111;
        packed_widths[0] = packed_widths[1] = packed_widths[2] = 10;
        packed_widths[3] = 2;
        packed_offsets[1] = 10;
        packed_offsets[2] = 20;
        packed_offsets[3] = 30;
      } break;
      case xenos::VertexFormat::k_10_11_11: {
        packed_components = 0b0111;
        packed_widths[0] = packed_widths[1] = 11;
        packed_widths[2] = 10;
        packed_offsets[1] = 11;
        packed_offsets[2] = 22;
      } break;
      case xenos::VertexFormat::k_11_11_10: {
        packed_components = 0b0111;
        packed_widths[0] = 10;
        packed_widths[1] = packed_widths[2] = 11;
        packed_offsets[1] = 10;
        packed_offsets[2] = 21;
      } break;
      case xenos::VertexFormat::k_16_16: {
        packed_components = 0b0011;
        packed_widths[0] = packed_widths[1] = 16;
        packed_offsets[1] = 16;
      } break;
      case xenos::VertexFormat::k_16_16_16_16: {
        packed_components = 0b1111;
        packed_widths[0] = packed_widths[1] = packed_widths[2] = packed_widths[3] = 16;
        packed_offsets[1] = packed_offsets[3] = 16;
        packed_dwords[1] = data[1];
      } break;
      case xenos::VertexFormat::k_16_16_16_16_FLOAT: {
        if (used_result_components & 0b1000) {
          result[3] = rex::xenos::HalfToFloat(static_cast<u16>(data[1] >> 16));
        }
        if (used_result_components & 0b0100) {
          result[2] = rex::xenos::HalfToFloat(static_cast<u16>(data[1]));
        }
      }
        [[fallthrough]];
      case xenos::VertexFormat::k_16_16_FLOAT: {
        if (used_result_components & 0b0010) {
          result[1] = rex::xenos::HalfToFloat(static_cast<u16>(data[0] >> 16));
        }
        if (used_result_components & 0b0001) {
          result[0] = rex::xenos::HalfToFloat(static_cast<u16>(data[0]));
        }
      } break;
      case xenos::VertexFormat::k_32:
      case xenos::VertexFormat::k_32_32:
      case xenos::VertexFormat::k_32_32_32_32: {
        if (instr.IsSigned()) {
          for (u32 i = 0; i < 4; ++i) {
            result[i] = static_cast<f32>(static_cast<i32>(data[i]));
          }
          if (instr.IsNormalized()) {
            if (instr.GetSignedRfMode() == xenos::SignedRepeatingFractionMode::kNoZero) {
              for (u32 i = 0; i < 4; ++i) {
                result[i] = (result[i] + 0.5f) / 2147483647.5f;
              }
            } else {
              for (u32 i = 0; i < 4; ++i) {
                result[i] /= 2147483647.0f;
                // No clamp to -1 for signed as the smallest value is -2^23 / 2^23 after rounding.
              }
            }
          }
        } else {
          for (u32 i = 0; i < 4; ++i) {
            result[i] = static_cast<f32>(data[i]);
          }
          if (instr.IsNormalized()) {
            for (u32 i = 0; i < 4; ++i) {
              result[i] /= 4294967295.0f;
            }
          }
        }
      } break;
      case xenos::VertexFormat::k_32_FLOAT:
      case xenos::VertexFormat::k_32_32_FLOAT:
      case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      case xenos::VertexFormat::k_32_32_32_FLOAT: {
        for (u32 i = 0; i < 4; ++i) {
          result[i] = std::bit_cast<f32>(data[i]);
        }
      } break;
      default:
        REX_UNHANDLED_CASE(instr.data_format());
        break;
    }

    packed_components &= used_result_components;
    if (packed_components) {
      if (instr.IsSigned()) {
        for (u32 i = 0; i < 4; ++i) {
          if (!(packed_components & (UINT32_C(1) << i))) {
            continue;
          }
          u32 packed_width = packed_widths[i];
          result[i] = static_cast<f32>(static_cast<i32>(packed_dwords[i >> 1])
                                           << (32 - (packed_width + packed_offsets[i])) >>
                                       (32 - packed_width));
        }
        if (instr.IsNormalized()) {
          if (instr.GetSignedRfMode() == xenos::SignedRepeatingFractionMode::kNoZero) {
            for (u32 i = 0; i < 4; ++i) {
              if (!(packed_components & (UINT32_C(1) << i))) {
                continue;
              }
              result[i] = (result[i] + 0.5f) * 2.0f /
                          static_cast<f32>((UINT32_C(1) << packed_widths[i]) - 1);
            }
          } else {
            for (u32 i = 0; i < 4; ++i) {
              if (!(packed_components & (UINT32_C(1) << i))) {
                continue;
              }
              result[i] = std::max(
                  -1.0f, result[i] / static_cast<f32>((UINT32_C(1) << (packed_widths[i] - 1)) - 1));
            }
          }
        }
      } else {
        for (u32 i = 0; i < 4; ++i) {
          if (!(packed_components & (UINT32_C(1) << i))) {
            continue;
          }
          u32 packed_width = packed_widths[i];
          result[i] = static_cast<f32>(packed_dwords[i >> 1] & ((UINT32_C(1) << packed_width) - 1));
        }
        if (instr.IsNormalized()) {
          for (u32 i = 0; i < 4; ++i) {
            if (!(packed_components & (UINT32_C(1) << i))) {
              continue;
            }
            result[i] /= static_cast<f32>((UINT32_C(1) << packed_widths[i]) - 1);
          }
        }
      }
    }
  }

  i32 exp_adjust = instr.GetExpAdjust();
  if (exp_adjust) {
    f32 exp_adjust_factor = std::ldexp(1.0f, exp_adjust);
    for (u32 i = 0; i < 4; ++i) {
      result[i] *= exp_adjust_factor;
    }
  }

  StoreFetchResult(instr.GetDest(), instr.IsDestRelative(), instr.GetDestSwizzle(), result);
}

}  // namespace rex::gpu
