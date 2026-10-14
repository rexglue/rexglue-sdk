/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <set>
#include <string>
#include <utility>

#include <rex/assert.h>
#include "gpu/xenos/pipeline/shader/translator.h"
#include <rex/logging.h>
#include <rex/math.h>

#include "gpu/xenos/cvar_table.h"

namespace rex::gpu {

using namespace xenos::ucode;

/**
 * Disassembles the microcode and gathers bindings, register and constant usage and memory export
 * state.
 *
 * Xenos microcode follows the Adreno A2XX ISA with a slightly different format, so freedreno
 * behavior can not be assumed to match.
 *
 * Control flow instructions come in pairs in blocks of 3 dwords listed at the top of the ucode and
 * execute sequentially until the ending instruction. The upper bound of the control flow program is
 * the address of the first exec, as in freedreno.
 *
 * Any `alloc`, not only `alloc export`, flushes the previous memory export. Yielding and
 * serializing end exports on hardware but are not modeled. Each eM# write is propagated as
 * potentially written before every successor in the control flow graph.
 * - exece has only the end as successor.
 * - Conditional exec ends have the next instruction and the end.
 * - Loop start has the next instruction and the skip target, loop end has the next instruction and
 * the repeat target.
 * - Conditional call has the next instruction and the target, and return stops the walk since a
 * subroutine call is treated as writing every eM#.
 * - Conditional jump has the target and, unless unconditional, the next instruction.
 * - alloc ends the previous export.
 *
 * @param ucode_disasm_buffer Buffer receiving the disassembly.
 */
void Shader::AnalyzeUcode(StringBuffer& ucode_disasm_buffer) {
  if (is_ucode_analyzed_) {
    return;
  }

  cf_pair_index_bound_ = static_cast<u32>(ucode_data_.size() / 3);
  for (u32 i = 0; i < cf_pair_index_bound_; ++i) {
    ControlFlowInstruction cf_ab[2];
    UnpackControlFlowInstructions(ucode_data_.data() + size_t{i} * 3, cf_ab);
    for (u32 j = 0; j < 2; ++j) {
      const ControlFlowInstruction& cf = cf_ab[j];
      if (IsControlFlowOpcodeExec(cf.GetOpcode())) {
        cf_pair_index_bound_ = std::min(cf_pair_index_bound_, cf.exec.GetAddress());
      }
      switch (cf.GetOpcode()) {
        case ControlFlowOpcode::kCondCall:
          label_addresses_.insert(cf.cond_call.GetAddress());
          break;
        case ControlFlowOpcode::kCondJmp:
          label_addresses_.insert(cf.cond_jmp.GetAddress());
          break;
        case ControlFlowOpcode::kLoopStart:
          label_addresses_.insert(cf.loop_start.GetAddress());
          break;
        case ControlFlowOpcode::kLoopEnd:
          label_addresses_.insert(cf.loop_end.GetAddress());
          break;
        default:
          break;
      }
    }
  }

  ucode_disasm_buffer.Reset();
  VertexFetchInstruction previous_vfetch_full{};
  u32 unique_texture_bindings = 0;
  for (u32 i = 0; i < cf_pair_index_bound_; ++i) {
    ControlFlowInstruction cf_ab[2];
    UnpackControlFlowInstructions(ucode_data_.data() + size_t{i} * 3, cf_ab);
    for (u32 j = 0; j < 2; ++j) {
      u32 cf_index = i * 2 + j;
      if (label_addresses_.find(cf_index) != label_addresses_.end()) {
        ucode_disasm_buffer.AppendFormat("                label L{}\n", cf_index);
      }
      ucode_disasm_buffer.AppendFormat("/* {:4d}.{} */ ", i, j);

      const ControlFlowInstruction& cf = cf_ab[j];
      u32 bool_constant_index = UINT32_MAX;
      switch (cf.GetOpcode()) {
        case ControlFlowOpcode::kNop:
          ucode_disasm_buffer.Append("      cnop\n");
          break;
        case ControlFlowOpcode::kExec:
        case ControlFlowOpcode::kExecEnd: {
          ParsedExecInstruction instr;
          ParseControlFlowExec(cf.exec, cf_index, instr);
          GatherExecInformation(instr, previous_vfetch_full, unique_texture_bindings,
                                ucode_disasm_buffer);
        } break;
        case ControlFlowOpcode::kCondExec:
        case ControlFlowOpcode::kCondExecEnd:
        case ControlFlowOpcode::kCondExecPredClean:
        case ControlFlowOpcode::kCondExecPredCleanEnd: {
          bool_constant_index = cf.cond_exec.GetBoolAddress();
          ParsedExecInstruction instr;
          ParseControlFlowCondExec(cf.cond_exec, cf_index, instr);
          GatherExecInformation(instr, previous_vfetch_full, unique_texture_bindings,
                                ucode_disasm_buffer);
        } break;
        case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredEnd: {
          ParsedExecInstruction instr;
          ParseControlFlowCondExecPred(cf.cond_exec_pred, cf_index, instr);
          GatherExecInformation(instr, previous_vfetch_full, unique_texture_bindings,
                                ucode_disasm_buffer);
        } break;
        case ControlFlowOpcode::kLoopStart: {
          ParsedLoopStartInstruction instr;
          ParseControlFlowLoopStart(cf.loop_start, cf_index, instr);
          instr.Disassemble(&ucode_disasm_buffer);
          constant_register_map_.loop_bitmap |= static_cast<u32>(1) << instr.loop_constant_index;
        } break;
        case ControlFlowOpcode::kLoopEnd: {
          ParsedLoopEndInstruction instr;
          ParseControlFlowLoopEnd(cf.loop_end, cf_index, instr);
          instr.Disassemble(&ucode_disasm_buffer);
          constant_register_map_.loop_bitmap |= static_cast<u32>(1) << instr.loop_constant_index;
        } break;
        case ControlFlowOpcode::kCondCall: {
          ParsedCallInstruction instr;
          ParseControlFlowCondCall(cf.cond_call, cf_index, instr);
          instr.Disassemble(&ucode_disasm_buffer);
          if (instr.type == ParsedCallInstruction::Type::kConditional) {
            bool_constant_index = instr.bool_constant_index;
          }
        } break;
        case ControlFlowOpcode::kReturn: {
          ParsedReturnInstruction instr;
          ParseControlFlowReturn(cf.ret, cf_index, instr);
          instr.Disassemble(&ucode_disasm_buffer);
        } break;
        case ControlFlowOpcode::kCondJmp: {
          ParsedJumpInstruction instr;
          ParseControlFlowCondJmp(cf.cond_jmp, cf_index, instr);
          instr.Disassemble(&ucode_disasm_buffer);
          if (instr.type == ParsedJumpInstruction::Type::kConditional) {
            bool_constant_index = instr.bool_constant_index;
          }
        } break;
        case ControlFlowOpcode::kAlloc: {
          ParsedAllocInstruction instr;
          ParseControlFlowAlloc(cf.alloc, cf_index, GetType() == xenos::ShaderType::kVertex, instr);
          instr.Disassemble(&ucode_disasm_buffer);
        } break;
        case ControlFlowOpcode::kMarkVsFetchDone:
          break;
        default:
          REX_UNHANDLED_CASE(cf.opcode);
          break;
      }
      if (bool_constant_index != UINT32_MAX) {
        constant_register_map_.bool_bitmap[bool_constant_index / 32] |=
            static_cast<u32>(1) << (bool_constant_index % 32);
      }
    }
  }
  ucode_disassembly_ = ucode_disasm_buffer.ToString();

  if (constant_register_map_.float_dynamic_addressing) {
    constant_register_map_.float_count = 256;
    memset(constant_register_map_.float_bitmap, UINT8_MAX,
           sizeof(constant_register_map_.float_bitmap));
  } else {
    constant_register_map_.float_count = 0;
    for (int i = 0; i < 4; ++i) {
      // Each bit is 1 vec4 of 4 floats.
      constant_register_map_.float_count += rex::BitCount(constant_register_map_.float_bitmap[i]);
    }
  }

  if (!cf_memexport_info_.empty()) {
    std::vector<u32> successor_stack;
    for (u32 i = 0; i < cf_pair_index_bound_; ++i) {
      ControlFlowInstruction em_writing_cf_ab[2];
      UnpackControlFlowInstructions(ucode_data_.data() + size_t{i} * 3, em_writing_cf_ab);
      for (u32 j = 0; j < 2; ++j) {
        u32 em_writing_cf_index = i * 2 + j;
        u32 em_written_by_cf_instr =
            cf_memexport_info_[em_writing_cf_index].em_potentially_written_by_exec;
        if (em_writing_cf_ab[j].GetOpcode() == ControlFlowOpcode::kCondCall) {
          em_written_by_cf_instr = memexport_em_written_;
        }
        if (!em_written_by_cf_instr) {
          continue;
        }

        bool is_successor_graph_head = true;
        successor_stack.push_back(em_writing_cf_index);
        while (!successor_stack.empty()) {
          u32 successor_cf_index = successor_stack.back();
          successor_stack.pop_back();

          ControlFlowMemExportInfo& successor_memexport_info =
              cf_memexport_info_[successor_cf_index];
          if ((successor_memexport_info.em_potentially_written_before & em_written_by_cf_instr) ==
              em_written_by_cf_instr) {
            break;
          }
          if (!is_successor_graph_head) {
            successor_memexport_info.em_potentially_written_before |= em_written_by_cf_instr;
          }
          is_successor_graph_head = false;

          ControlFlowInstruction successor_cf_ab[2];
          UnpackControlFlowInstructions(ucode_data_.data() + size_t{successor_cf_index >> 1} * 3,
                                        successor_cf_ab);
          const ControlFlowInstruction& successor_cf = successor_cf_ab[successor_cf_index & 1];

          bool next_instr_is_new_successor = true;
          switch (successor_cf.GetOpcode()) {
            case ControlFlowOpcode::kExecEnd:
              memexport_em_potentially_written_before_end_ |= em_written_by_cf_instr;
              next_instr_is_new_successor = false;
              break;
            case ControlFlowOpcode::kCondExecEnd:
            case ControlFlowOpcode::kCondExecPredEnd:
            case ControlFlowOpcode::kCondExecPredCleanEnd:
              memexport_em_potentially_written_before_end_ |= em_written_by_cf_instr;
              break;
            case ControlFlowOpcode::kLoopStart:
              successor_stack.push_back(successor_cf.loop_start.GetAddress());
              break;
            case ControlFlowOpcode::kLoopEnd:
              successor_stack.push_back(successor_cf.loop_end.GetAddress());
              break;
            case ControlFlowOpcode::kCondCall:
              successor_stack.push_back(successor_cf.cond_call.GetAddress());
              break;
            case ControlFlowOpcode::kReturn:
              next_instr_is_new_successor = false;
              break;
            case ControlFlowOpcode::kCondJmp:
              successor_stack.push_back(successor_cf.cond_jmp.GetAddress());
              if (successor_cf.cond_jmp.IsUnconditional()) {
                next_instr_is_new_successor = false;
              }
              break;
            case ControlFlowOpcode::kAlloc:
              next_instr_is_new_successor = false;
              break;
            default:
              break;
          }
          if (next_instr_is_new_successor) {
            if (successor_cf_index < (cf_pair_index_bound_ << 1)) {
              successor_stack.push_back(successor_cf_index + 1);
            } else {
              memexport_em_potentially_written_before_end_ |= em_written_by_cf_instr;
            }
          }
        }
      }
    }
  }

  is_ucode_analyzed_ = true;

  if (!REXCVAR_GET(::rex::gpu::cvars::dump_shaders).empty() && !GetUcodeData().empty()) {
    DumpUcode(REXCVAR_GET(::rex::gpu::cvars::dump_shaders));
  }
}

u32 Shader::GetInterpolatorInputMask(xenos::reg::SQ_PROGRAM_CNTL sq_program_cntl,
                                     xenos::reg::SQ_CONTEXT_MISC sq_context_misc,
                                     u32& param_gen_pos_out) const {
  REX_ASSERT(GetType() == xenos::ShaderType::kPixel);
  u32 interpolator_count =
      std::min(xenos::kMaxInterpolators,
               std::max(GetRegisterStaticAddressBound(),
                        GetDynamicAddressableRegisterCount(sq_program_cntl.ps_num_reg)));
  u32 interpolator_mask = (UINT32_C(1) << interpolator_count) - 1;
  if (sq_program_cntl.param_gen && sq_context_misc.param_gen_pos < interpolator_count) {
    interpolator_mask &= ~(UINT32_C(1) << sq_context_misc.param_gen_pos);
    param_gen_pos_out = sq_context_misc.param_gen_pos;
  } else {
    param_gen_pos_out = UINT32_MAX;
  }
  return interpolator_mask;
}

void Shader::GatherExecInformation(const ParsedExecInstruction& instr,
                                   xenos::ucode::VertexFetchInstruction& previous_vfetch_full,
                                   u32& unique_texture_bindings,
                                   StringBuffer& ucode_disasm_buffer) {
  instr.Disassemble(&ucode_disasm_buffer);
  u32 sequence = instr.sequence;
  for (u32 instr_offset = instr.instruction_address;
       instr_offset < instr.instruction_address + instr.instruction_count;
       ++instr_offset, sequence >>= 2) {
    ucode_disasm_buffer.AppendFormat("/* {:4d}   */ ", instr_offset);
    if (sequence & 0b10) {
      ucode_disasm_buffer.Append("         serialize\n             ");
    }
    const u32* op_ptr = ucode_data_.data() + size_t{instr_offset} * 3;
    if (sequence & 0b01) {
      auto& op = *reinterpret_cast<const FetchInstruction*>(op_ptr);
      if (op.GetOpcode() == FetchOpcode::kVertexFetch) {
        GatherVertexFetchInformation(op.GetVertexFetch(), previous_vfetch_full,
                                     ucode_disasm_buffer);
      } else {
        GatherTextureFetchInformation(op.GetTextureFetch(), unique_texture_bindings,
                                      ucode_disasm_buffer);
      }
    } else {
      auto& op = *reinterpret_cast<const AluInstruction*>(op_ptr);
      GatherAluInstructionInformation(op, instr.dword_index, ucode_disasm_buffer);
    }
  }
}

void Shader::GatherVertexFetchInformation(const VertexFetchInstruction& op,
                                          VertexFetchInstruction& previous_vfetch_full,
                                          StringBuffer& ucode_disasm_buffer) {
  ParsedVertexFetchInstruction fetch_instr;
  if (ParseVertexFetchInstruction(op, previous_vfetch_full, fetch_instr)) {
    previous_vfetch_full = op;
  }
  fetch_instr.Disassemble(&ucode_disasm_buffer);

  GatherFetchResultInformation(fetch_instr.result);

  // Mini fetches inherit the operands from full fetches.
  if (!fetch_instr.is_mini_fetch) {
    for (size_t i = 0; i < fetch_instr.operand_count; ++i) {
      GatherOperandInformation(fetch_instr.operands[i]);
    }
  }

  // A vfetch_full with no used results sets addressing for the next vfetch_mini, so gather first.
  if (!fetch_instr.result.GetUsedResultComponents()) {
    return;
  }

  using VertexBinding = Shader::VertexBinding;
  VertexBinding::Attribute* attrib = nullptr;
  for (auto& vertex_binding : vertex_bindings_) {
    if (vertex_binding.fetch_constant == op.GetFetchConstantIndex()) {
      REX_ASSERT(!fetch_instr.attributes.stride ||
                 vertex_binding.stride_words == fetch_instr.attributes.stride);
      vertex_binding.attributes.push_back({});
      attrib = &vertex_binding.attributes.back();
      break;
    }
  }
  if (!attrib) {
    REX_ASSERT(fetch_instr.attributes.stride != 0);
    VertexBinding vertex_binding;
    vertex_binding.binding_index = int(vertex_bindings_.size());
    vertex_binding.fetch_constant = op.GetFetchConstantIndex();
    vertex_binding.stride_words = fetch_instr.attributes.stride;
    vertex_binding.attributes.push_back({});
    vertex_bindings_.emplace_back(std::move(vertex_binding));
    attrib = &vertex_bindings_.back().attributes.back();
  }

  attrib->fetch_instr = fetch_instr;
}

void Shader::GatherTextureFetchInformation(const TextureFetchInstruction& op,
                                           u32& unique_texture_bindings,
                                           StringBuffer& ucode_disasm_buffer) {
  TextureBinding binding;
  ParseTextureFetchInstruction(op, binding.fetch_instr);
  binding.fetch_instr.Disassemble(&ucode_disasm_buffer);

  GatherFetchResultInformation(binding.fetch_instr.result);
  for (size_t i = 0; i < binding.fetch_instr.operand_count; ++i) {
    GatherOperandInformation(binding.fetch_instr.operands[i]);
  }

  if (binding.fetch_instr.result.GetUsedResultComponents()) {
    uses_texture_fetch_instruction_results_ = true;
  }

  switch (op.GetOpcode()) {
    case FetchOpcode::kSetTextureLOD:
    case FetchOpcode::kSetTextureGradientsHorz:
    case FetchOpcode::kSetTextureGradientsVert:
      return;
    default:
      break;
  }
  binding.binding_index = SIZE_MAX;
  binding.fetch_constant = binding.fetch_instr.operands[1].storage_index;

  for (auto& tex_binding : texture_bindings_) {
    if (tex_binding.fetch_constant == binding.fetch_constant) {
      binding.binding_index = tex_binding.binding_index;
      break;
    }
  }

  if (binding.binding_index == SIZE_MAX) {
    binding.binding_index = unique_texture_bindings++;
  }

  texture_bindings_.emplace_back(binding);
}

/**
 * Gathers the disassembly, operands, results and kill state of an ALU instruction.
 *
 * Used memexport stream constants are stored because the CPU code needs addresses and sizes. eA is
 * expected to be written by `mad eA, r#, const0100, c#`, and export goes to vector_dest for both
 * vector and scalar operations so there is 1 check.
 *
 * @param op ALU instruction.
 * @param exec_cf_index Control flow index of the containing exec.
 * @param ucode_disasm_buffer Buffer receiving the disassembly.
 */
void Shader::GatherAluInstructionInformation(const AluInstruction& op, u32 exec_cf_index,
                                             StringBuffer& ucode_disasm_buffer) {
  ParsedAluInstruction instr;
  ParseAluInstruction(op, GetType(), instr);
  instr.Disassemble(&ucode_disasm_buffer);

  kills_pixels_ = kills_pixels_ ||
                  (xenos::ucode::GetAluVectorOpcodeInfo(op.GetVectorOpcode()).changed_state &
                   xenos::ucode::kAluOpChangedStatePixelKill) ||
                  (xenos::ucode::GetAluScalarOpcodeInfo(op.GetScalarOpcode()).changed_state &
                   xenos::ucode::kAluOpChangedStatePixelKill);

  GatherAluResultInformation(instr.vector_and_constant_result, exec_cf_index);
  GatherAluResultInformation(instr.scalar_result, exec_cf_index);
  for (size_t i = 0; i < instr.vector_operand_count; ++i) {
    GatherOperandInformation(instr.vector_operands[i]);
  }
  for (size_t i = 0; i < instr.scalar_operand_count; ++i) {
    GatherOperandInformation(instr.scalar_operands[i]);
  }

  if (instr.vector_and_constant_result.storage_target == InstructionStorageTarget::kExportAddress) {
    u32 memexport_stream_constant = instr.GetMemExportStreamConstant();
    if (memexport_stream_constant != UINT32_MAX) {
      memexport_stream_constants_.insert(memexport_stream_constant);
    } else {
      REXLOG_ERROR(gpu,
                   "ShaderTranslator::GatherAluInstructionInformation: Couldn't extract "
                   "memexport stream constant index");
    }
  }
}

void Shader::GatherOperandInformation(const InstructionOperand& operand) {
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister:
      if (operand.storage_addressing_mode == InstructionStorageAddressingMode::kAbsolute) {
        register_static_address_bound_ =
            std::max(register_static_address_bound_, operand.storage_index + static_cast<u32>(1));
      } else {
        uses_register_dynamic_addressing_ = true;
      }
      break;
    case InstructionStorageSource::kConstantFloat:
      if (operand.storage_addressing_mode == InstructionStorageAddressingMode::kAbsolute) {
        // Gathered before translation so it can pack indices unless dynamically indexed.
        constant_register_map_.float_bitmap[operand.storage_index >> 6] |=
            static_cast<u64>(1) << (operand.storage_index & 63);
      } else {
        constant_register_map_.float_dynamic_addressing = true;
      }
      break;
    case InstructionStorageSource::kVertexFetchConstant:
      constant_register_map_.vertex_fetch_bitmap[operand.storage_index >> 5] |=
          static_cast<u32>(1) << (operand.storage_index & 31);
      break;
    default:
      break;
  }
}

void Shader::GatherFetchResultInformation(const InstructionResult& result) {
  if (!result.GetUsedWriteMask()) {
    return;
  }
  REX_ASSERT(result.storage_target == InstructionStorageTarget::kRegister);
  if (result.storage_addressing_mode == InstructionStorageAddressingMode::kAbsolute) {
    register_static_address_bound_ =
        std::max(register_static_address_bound_, result.storage_index + static_cast<u32>(1));
  } else {
    uses_register_dynamic_addressing_ = true;
  }
}

void Shader::GatherAluResultInformation(const InstructionResult& result, u32 exec_cf_index) {
  u32 used_write_mask = result.GetUsedWriteMask();
  if (!used_write_mask) {
    return;
  }
  switch (result.storage_target) {
    case InstructionStorageTarget::kRegister:
      if (result.storage_addressing_mode == InstructionStorageAddressingMode::kAbsolute) {
        register_static_address_bound_ =
            std::max(register_static_address_bound_, result.storage_index + static_cast<u32>(1));
      } else {
        uses_register_dynamic_addressing_ = true;
      }
      break;
    case InstructionStorageTarget::kInterpolator:
      writes_interpolators_ |= static_cast<u32>(1) << result.storage_index;
      break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      writes_point_size_edge_flag_kill_vertex_ |= used_write_mask;
      break;
    case InstructionStorageTarget::kExportData:
      memexport_em_written_ |= static_cast<u8>(1) << result.storage_index;
      if (cf_memexport_info_.empty()) {
        cf_memexport_info_.resize(size_t{2} * cf_pair_index_bound_);
      }
      cf_memexport_info_[exec_cf_index].em_potentially_written_by_exec |= static_cast<u32>(1)
                                                                          << result.storage_index;
      break;
    case InstructionStorageTarget::kColor:
      writes_color_targets_ |= static_cast<u32>(1) << result.storage_index;
      break;
    case InstructionStorageTarget::kDepth:
      writes_depth_ = true;
      break;
    default:
      break;
  }
}

ShaderTranslator::ShaderTranslator() = default;

ShaderTranslator::~ShaderTranslator() = default;

void ShaderTranslator::Reset() {
  errors_.clear();
  previous_vfetch_full_ = {};
}

bool ShaderTranslator::TranslateAnalyzedShader(Shader::Translation& translation) {
  const Shader& shader = translation.GetShader();
  REX_ASSERT(shader.IsUcodeAnalyzed());
  if (!shader.IsUcodeAnalyzed()) {
    REX_FAIL(gpu, false, "AnalyzeUcode must be done on the shader before translation");
  }
  translation_ = &translation;

  Reset();

  register_count_ = shader.GetRegisterStaticAddressBound();
  if (shader.UsesRegisterDynamicAddressing()) {
    // End of r# space registers may be dynamically addressed, so allocate SQ_PROGRAM_CNTL count.
    register_count_ = std::max(register_count_, GetModificationRegisterCount());
  }

  StartTranslation();

  const u32* ucode_dwords = shader.GetUcodeData().data();

  u32 cf_pair_index_bound = shader.GetCfPairIndexBound();
  const std::set<u32>& label_addresses = shader.GetLabelAddresses();
  for (u32 i = 0; i < cf_pair_index_bound; ++i) {
    ControlFlowInstruction cf_ab[2];
    UnpackControlFlowInstructions(ucode_dwords + size_t{i} * 3, cf_ab);
    for (u32 j = 0; j < 2; ++j) {
      u32 cf_index = i * 2 + j;
      cf_index_ = cf_index;
      if (label_addresses.find(cf_index) != label_addresses.end()) {
        ProcessLabel(cf_index);
      }
      ProcessControlFlowInstructionBegin(cf_index);
      TranslateControlFlowInstruction(cf_ab[j]);
      ProcessControlFlowInstructionEnd(cf_index);
    }
  }

  translation.errors_ = std::move(errors_);
  translation.translated_binary_ = CompleteTranslation();
  translation.is_translated_ = true;

  bool is_valid = true;
  for (const auto& error : translation.errors_) {
    if (error.is_fatal) {
      is_valid = false;
      break;
    }
  }
  translation.is_valid_ = is_valid;

  PostTranslation();

  // PostTranslation may change is_valid_, so reload it.
  return translation.is_valid_;
}

void ShaderTranslator::EmitTranslationError(const char* message, bool is_fatal) {
  Shader::Error error;
  error.is_fatal = is_fatal;
  error.message = message;
  errors_.push_back(std::move(error));
  REXLOG_ERROR(gpu, "Shader translation {}error: {}", is_fatal ? "fatal " : "", message);
}

void ShaderTranslator::TranslateControlFlowInstruction(const ControlFlowInstruction& cf) {
  switch (cf.GetOpcode()) {
    case ControlFlowOpcode::kNop:
      ProcessControlFlowNopInstruction(cf_index_);
      break;
    case ControlFlowOpcode::kExec:
    case ControlFlowOpcode::kExecEnd: {
      ParsedExecInstruction instr;
      ParseControlFlowExec(cf.exec, cf_index_, instr);
      TranslateExecInstructions(instr);
    } break;
    case ControlFlowOpcode::kCondExec:
    case ControlFlowOpcode::kCondExecEnd:
    case ControlFlowOpcode::kCondExecPredClean:
    case ControlFlowOpcode::kCondExecPredCleanEnd: {
      ParsedExecInstruction instr;
      ParseControlFlowCondExec(cf.cond_exec, cf_index_, instr);
      TranslateExecInstructions(instr);
    } break;
    case ControlFlowOpcode::kCondExecPred:
    case ControlFlowOpcode::kCondExecPredEnd: {
      ParsedExecInstruction instr;
      ParseControlFlowCondExecPred(cf.cond_exec_pred, cf_index_, instr);
      TranslateExecInstructions(instr);
    } break;
    case ControlFlowOpcode::kLoopStart: {
      ParsedLoopStartInstruction instr;
      ParseControlFlowLoopStart(cf.loop_start, cf_index_, instr);
      ProcessLoopStartInstruction(instr);
    } break;
    case ControlFlowOpcode::kLoopEnd: {
      ParsedLoopEndInstruction instr;
      ParseControlFlowLoopEnd(cf.loop_end, cf_index_, instr);
      ProcessLoopEndInstruction(instr);
    } break;
    case ControlFlowOpcode::kCondCall: {
      ParsedCallInstruction instr;
      ParseControlFlowCondCall(cf.cond_call, cf_index_, instr);
      ProcessCallInstruction(instr);
    } break;
    case ControlFlowOpcode::kReturn: {
      ParsedReturnInstruction instr;
      ParseControlFlowReturn(cf.ret, cf_index_, instr);
      ProcessReturnInstruction(instr);
    } break;
    case ControlFlowOpcode::kCondJmp: {
      ParsedJumpInstruction instr;
      ParseControlFlowCondJmp(cf.cond_jmp, cf_index_, instr);
      ProcessJumpInstruction(instr);
    } break;
    case ControlFlowOpcode::kAlloc: {
      ParsedAllocInstruction instr;
      ParseControlFlowAlloc(cf.alloc, cf_index_, IsVertexShader(), instr);
      const std::vector<Shader::ControlFlowMemExportInfo>& cf_memexport_info =
          GetCurrentShader().GetCfMemexportInfo();
      ProcessAllocInstruction(
          instr, instr.dword_index < cf_memexport_info.size()
                     ? cf_memexport_info[instr.dword_index].em_potentially_written_before
                     : 0);
    } break;
    case ControlFlowOpcode::kMarkVsFetchDone:
      break;
    default:
      REX_UNHANDLED_CASE(cf.opcode);
      break;
  }
}

void ParseControlFlowExec(const ControlFlowExecInstruction& cf, u32 cf_index,
                          ParsedExecInstruction& instr) {
  instr.dword_index = cf_index;
  instr.opcode = cf.GetOpcode();
  instr.opcode_name = cf.GetOpcode() == ControlFlowOpcode::kExecEnd ? "exece" : "exec";
  instr.instruction_address = cf.GetAddress();
  instr.instruction_count = cf.GetCount();
  instr.type = ParsedExecInstruction::Type::kUnconditional;
  instr.is_end = cf.GetOpcode() == ControlFlowOpcode::kExecEnd;
  instr.is_predicate_clean = cf.IsPredicateClean();
  instr.is_yield = cf.IsYield();
  instr.sequence = cf.GetSequence();
}

void ParseControlFlowCondExec(const ControlFlowCondExecInstruction& cf, u32 cf_index,
                              ParsedExecInstruction& instr) {
  instr.dword_index = cf_index;
  instr.opcode = cf.GetOpcode();
  instr.opcode_name = "cexec";
  switch (cf.GetOpcode()) {
    case ControlFlowOpcode::kCondExecEnd:
    case ControlFlowOpcode::kCondExecPredCleanEnd:
      instr.opcode_name = "cexece";
      instr.is_end = true;
      break;
    default:
      break;
  }
  instr.instruction_address = cf.GetAddress();
  instr.instruction_count = cf.GetCount();
  instr.type = ParsedExecInstruction::Type::kConditional;
  instr.bool_constant_index = cf.GetBoolAddress();
  instr.condition = cf.GetCondition();
  switch (cf.GetOpcode()) {
    case ControlFlowOpcode::kCondExec:
    case ControlFlowOpcode::kCondExecEnd:
      instr.is_predicate_clean = false;
      break;
    default:
      break;
  }
  instr.is_yield = cf.IsYield();
  instr.sequence = cf.GetSequence();
}

void ParseControlFlowCondExecPred(const ControlFlowCondExecPredInstruction& cf, u32 cf_index,
                                  ParsedExecInstruction& instr) {
  instr.dword_index = cf_index;
  instr.opcode = cf.GetOpcode();
  instr.opcode_name = cf.GetOpcode() == ControlFlowOpcode::kCondExecPredEnd ? "exece" : "exec";
  instr.instruction_address = cf.GetAddress();
  instr.instruction_count = cf.GetCount();
  instr.type = ParsedExecInstruction::Type::kPredicated;
  instr.condition = cf.GetCondition();
  instr.is_end = cf.GetOpcode() == ControlFlowOpcode::kCondExecPredEnd;
  instr.is_predicate_clean = cf.IsPredicateClean();
  instr.is_yield = cf.IsYield();
  instr.sequence = cf.GetSequence();
}

void ParseControlFlowLoopStart(const ControlFlowLoopStartInstruction& cf, u32 cf_index,
                               ParsedLoopStartInstruction& instr) {
  instr.dword_index = cf_index;
  instr.loop_constant_index = cf.GetLoopId();
  instr.is_repeat = cf.IsRepeat();
  instr.loop_skip_address = cf.GetAddress();
}

void ParseControlFlowLoopEnd(const ControlFlowLoopEndInstruction& cf, u32 cf_index,
                             ParsedLoopEndInstruction& instr) {
  instr.dword_index = cf_index;
  instr.is_predicated_break = cf.IsPredicatedBreak();
  instr.predicate_condition = cf.GetCondition();
  instr.loop_constant_index = cf.GetLoopId();
  instr.loop_body_address = cf.GetAddress();
}

void ParseControlFlowCondCall(const ControlFlowCondCallInstruction& cf, u32 cf_index,
                              ParsedCallInstruction& instr) {
  instr.dword_index = cf_index;
  instr.target_address = cf.GetAddress();
  if (cf.IsUnconditional()) {
    instr.type = ParsedCallInstruction::Type::kUnconditional;
  } else if (cf.IsPredicated()) {
    instr.type = ParsedCallInstruction::Type::kPredicated;
    instr.condition = cf.GetCondition();
  } else {
    instr.type = ParsedCallInstruction::Type::kConditional;
    instr.bool_constant_index = cf.GetBoolAddress();
    instr.condition = cf.GetCondition();
  }
}

void ParseControlFlowReturn([[maybe_unused]] const ControlFlowReturnInstruction& cf, u32 cf_index,
                            ParsedReturnInstruction& instr) {
  instr.dword_index = cf_index;
}

void ParseControlFlowCondJmp(const ControlFlowCondJmpInstruction& cf, u32 cf_index,
                             ParsedJumpInstruction& instr) {
  instr.dword_index = cf_index;
  instr.target_address = cf.GetAddress();
  if (cf.IsUnconditional()) {
    instr.type = ParsedJumpInstruction::Type::kUnconditional;
  } else if (cf.IsPredicated()) {
    instr.type = ParsedJumpInstruction::Type::kPredicated;
    instr.condition = cf.GetCondition();
  } else {
    instr.type = ParsedJumpInstruction::Type::kConditional;
    instr.bool_constant_index = cf.GetBoolAddress();
    instr.condition = cf.GetCondition();
  }
}

void ParseControlFlowAlloc(const ControlFlowAllocInstruction& cf, u32 cf_index,
                           bool is_vertex_shader, ParsedAllocInstruction& instr) {
  instr.dword_index = cf_index;
  instr.type = cf.GetAllocType();
  instr.count = static_cast<int>(cf.GetSize());
  instr.is_vertex_shader = is_vertex_shader;
}

void ShaderTranslator::TranslateExecInstructions(const ParsedExecInstruction& instr) {
  ProcessExecInstructionBegin(instr);

  const std::vector<Shader::ControlFlowMemExportInfo>& cf_memexport_info =
      GetCurrentShader().GetCfMemexportInfo();
  u8 em_potentially_written_before =
      instr.dword_index < cf_memexport_info.size()
          ? cf_memexport_info[instr.dword_index].em_potentially_written_before
          : 0;

  const u32* ucode_dwords = GetCurrentShader().GetUcodeData().data();
  u32 sequence = instr.sequence;
  for (u32 instr_offset = instr.instruction_address;
       instr_offset < instr.instruction_address + instr.instruction_count;
       ++instr_offset, sequence >>= 2) {
    const u32* op_ptr = ucode_dwords + size_t{instr_offset} * 3;
    if (sequence & 0b01) {
      auto& op = *reinterpret_cast<const FetchInstruction*>(op_ptr);
      if (op.GetOpcode() == FetchOpcode::kVertexFetch) {
        const VertexFetchInstruction& vfetch_op = op.GetVertexFetch();
        ParsedVertexFetchInstruction vfetch_instr;
        if (ParseVertexFetchInstruction(vfetch_op, previous_vfetch_full_, vfetch_instr)) {
          previous_vfetch_full_ = vfetch_op;
        }
        ProcessVertexFetchInstruction(vfetch_instr);
      } else {
        ParsedTextureFetchInstruction tfetch_instr;
        ParseTextureFetchInstruction(op.GetTextureFetch(), tfetch_instr);
        ProcessTextureFetchInstruction(tfetch_instr);
      }
    } else {
      auto& op = *reinterpret_cast<const AluInstruction*>(op_ptr);
      ParsedAluInstruction alu_instr;
      ParseAluInstruction(op, GetCurrentShader().GetType(), alu_instr);
      ProcessAluInstruction(alu_instr, em_potentially_written_before);
      if (alu_instr.vector_and_constant_result.storage_target ==
              InstructionStorageTarget::kExportData &&
          alu_instr.vector_and_constant_result.GetUsedWriteMask()) {
        em_potentially_written_before |= static_cast<u8>(1)
                                         << alu_instr.vector_and_constant_result.storage_index;
      }
      if (alu_instr.scalar_result.storage_target == InstructionStorageTarget::kExportData &&
          alu_instr.scalar_result.GetUsedWriteMask()) {
        em_potentially_written_before |= static_cast<u8>(1)
                                         << alu_instr.scalar_result.storage_index;
      }
    }
  }

  ProcessExecInstructionEnd(instr);
}

static void ParseFetchInstructionResult(u32 dest, u32 swizzle, bool is_relative,
                                        InstructionResult& result) {
  result.storage_target = InstructionStorageTarget::kRegister;
  result.storage_index = dest;
  result.is_clamped = false;
  result.storage_addressing_mode = is_relative ? InstructionStorageAddressingMode::kLoopRelative
                                               : InstructionStorageAddressingMode::kAbsolute;
  result.original_write_mask = 0b1111;
  for (int i = 0; i < 4; ++i) {
    SwizzleSource component_source = SwizzleSource::k0;
    xenos::ucode::FetchDestinationSwizzle component_swizzle =
        xenos::ucode::GetFetchDestinationComponentSwizzle(swizzle, i);
    switch (component_swizzle) {
      case xenos::ucode::FetchDestinationSwizzle::kX:
        component_source = SwizzleSource::kX;
        break;
      case xenos::ucode::FetchDestinationSwizzle::kY:
        component_source = SwizzleSource::kY;
        break;
      case xenos::ucode::FetchDestinationSwizzle::kZ:
        component_source = SwizzleSource::kZ;
        break;
      case xenos::ucode::FetchDestinationSwizzle::kW:
        component_source = SwizzleSource::kW;
        break;
      case xenos::ucode::FetchDestinationSwizzle::k1:
        component_source = SwizzleSource::k1;
        break;
      case xenos::ucode::FetchDestinationSwizzle::kKeep:
        result.original_write_mask &= ~(UINT32_C(1) << i);
        break;
      default:
        // k0 or the invalid swizzle 6.
        REX_ASSERT(component_swizzle == xenos::ucode::FetchDestinationSwizzle::k0);
        component_source = SwizzleSource::k0;
        break;
    }
    result.components[i] = component_source;
  }
}

bool ParseVertexFetchInstruction(const VertexFetchInstruction& op,
                                 const VertexFetchInstruction& previous_full_op,
                                 ParsedVertexFetchInstruction& instr) {
  instr.opcode = FetchOpcode::kVertexFetch;
  instr.opcode_name = op.IsMiniFetch() ? "vfetch_mini" : "vfetch_full";
  instr.is_mini_fetch = op.IsMiniFetch();
  instr.is_predicated = op.IsPredicated();
  instr.predicate_condition = op.GetPredicateCondition();

  ParseFetchInstructionResult(op.GetDest(), op.GetDestSwizzle(), op.IsDestRelative(), instr.result);

  const auto& full_op = op.IsMiniFetch() ? previous_full_op : op;
  auto& src_op = instr.operands[instr.operand_count++];
  src_op.storage_source = InstructionStorageSource::kRegister;
  src_op.storage_index = full_op.GetSrc();
  src_op.storage_addressing_mode = full_op.IsSrcRelative()
                                       ? InstructionStorageAddressingMode::kLoopRelative
                                       : InstructionStorageAddressingMode::kAbsolute;
  src_op.is_negated = false;
  src_op.is_absolute_value = false;
  src_op.component_count = 1;
  u32 swizzle = full_op.GetSrcSwizzle();
  for (u32 j = 0; j < src_op.component_count; ++j, swizzle >>= 2) {
    src_op.components[j] = GetSwizzleFromComponentIndex(swizzle & 0x3);
  }

  auto& const_op = instr.operands[instr.operand_count++];
  const_op.storage_source = InstructionStorageSource::kVertexFetchConstant;
  const_op.storage_index = full_op.GetFetchConstantIndex();

  instr.attributes.data_format = op.GetDataFormat();
  instr.attributes.offset = op.GetOffset();
  instr.attributes.stride = full_op.GetStride();
  instr.attributes.exp_adjust = op.GetExpAdjust();
  instr.attributes.prefetch_count = op.GetPrefetchCount();
  instr.attributes.is_index_rounded = full_op.IsIndexRounded();
  instr.attributes.is_signed = op.IsSigned();
  instr.attributes.is_integer = !op.IsNormalized();
  instr.attributes.signed_rf_mode = op.GetSignedRfMode();

  return !op.IsMiniFetch();
}

void ParseTextureFetchInstruction(const TextureFetchInstruction& op,
                                  ParsedTextureFetchInstruction& instr) {
  struct TextureFetchOpcodeInfo {
    const char* name;
    bool has_dest;
    bool has_const;
    bool has_attributes;
    u32 override_component_count;
  } opcode_info;
  switch (op.GetOpcode()) {
    case FetchOpcode::kTextureFetch: {
      static constexpr const char* kNames[] = {"tfetch1D", "tfetch2D", "tfetch3D", "tfetchCube"};
      opcode_info = {kNames[static_cast<int>(op.GetDimension())], true, true, true, 0};
    } break;
    case FetchOpcode::kGetTextureBorderColorFrac: {
      static constexpr const char* kNames[] = {"getBCF1D", "getBCF2D", "getBCF3D", "getBCFCube"};
      opcode_info = {kNames[static_cast<int>(op.GetDimension())], true, true, true, 0};
    } break;
    case FetchOpcode::kGetTextureComputedLOD: {
      static constexpr const char* kNames[] = {"getCompTexLOD1D", "getCompTexLOD2D",
                                               "getCompTexLOD3D", "getCompTexLODCube"};
      opcode_info = {kNames[static_cast<int>(op.GetDimension())], true, true, true, 0};
    } break;
    case FetchOpcode::kGetTextureGradients:
      opcode_info = {"getGradients", true, true, true, 2};
      break;
    case FetchOpcode::kGetTextureWeights: {
      static constexpr const char* kNames[] = {"getWeights1D", "getWeights2D", "getWeights3D",
                                               "getWeightsCube"};
      opcode_info = {kNames[static_cast<int>(op.GetDimension())], true, true, true, 0};
    } break;
    case FetchOpcode::kSetTextureLOD:
      opcode_info = {"setTexLOD", false, false, false, 1};
      break;
    case FetchOpcode::kSetTextureGradientsHorz:
      opcode_info = {"setGradientH", false, false, false, 3};
      break;
    case FetchOpcode::kSetTextureGradientsVert:
      opcode_info = {"setGradientV", false, false, false, 3};
      break;
    default:
      REX_UNHANDLED_CASE(fetch_opcode);
      return;
  }

  instr.opcode = op.GetOpcode();
  instr.opcode_name = opcode_info.name;
  instr.dimension = op.GetDimension();
  instr.is_predicated = op.IsPredicated();
  instr.predicate_condition = op.GetPredicateCondition();

  if (opcode_info.has_dest) {
    ParseFetchInstructionResult(op.GetDest(), op.GetDestSwizzle(), op.IsDestRelative(),
                                instr.result);
  } else {
    instr.result.storage_target = InstructionStorageTarget::kNone;
  }

  auto& src_op = instr.operands[instr.operand_count++];
  src_op.storage_source = InstructionStorageSource::kRegister;
  src_op.storage_index = op.GetSrc();
  src_op.storage_addressing_mode = op.IsSrcRelative()
                                       ? InstructionStorageAddressingMode::kLoopRelative
                                       : InstructionStorageAddressingMode::kAbsolute;
  src_op.is_negated = false;
  src_op.is_absolute_value = false;
  src_op.component_count = opcode_info.override_component_count
                               ? opcode_info.override_component_count
                               : xenos::GetFetchOpDimensionComponentCount(op.GetDimension());
  u32 swizzle = op.GetSrcSwizzle();
  for (u32 j = 0; j < src_op.component_count; ++j, swizzle >>= 2) {
    src_op.components[j] = GetSwizzleFromComponentIndex(swizzle & 0x3);
  }

  if (opcode_info.has_const) {
    auto& const_op = instr.operands[instr.operand_count++];
    const_op.storage_source = InstructionStorageSource::kTextureFetchConstant;
    const_op.storage_index = op.GetFetchConstantIndex();
  }

  if (opcode_info.has_attributes) {
    instr.attributes.fetch_valid_only = op.GetFetchValidOnly();
    instr.attributes.unnormalized_coordinates = op.GetUnnormalizedCoordinates();
    instr.attributes.mag_filter = op.GetMagFilter();
    instr.attributes.min_filter = op.GetMinFilter();
    instr.attributes.mip_filter = op.GetMipFilter();
    instr.attributes.aniso_filter = op.GetAnisoFilter();
    instr.attributes.vol_mag_filter = op.GetVolMagFilter();
    instr.attributes.vol_min_filter = op.GetVolMinFilter();
    instr.attributes.use_computed_lod = op.UsesComputedLOD();
    instr.attributes.use_register_lod = op.UsesRegisterLOD();
    instr.attributes.use_register_gradients = op.UsesRegisterGradients();
    instr.attributes.lod_bias = op.GetLODBias();
    instr.attributes.offset_x = op.GetOffsetX();
    instr.attributes.offset_y = op.GetOffsetY();
    instr.attributes.offset_z = op.GetOffsetZ();
  }
}

u32 ParsedTextureFetchInstruction::GetNonZeroResultComponents() const {
  u32 components = 0b0000;
  switch (opcode) {
    case FetchOpcode::kTextureFetch:
    case FetchOpcode::kGetTextureGradients:
      components = 0b1111;
      break;
    case FetchOpcode::kGetTextureBorderColorFrac:
    case FetchOpcode::kGetTextureComputedLOD:
      // XNA does not accept MipFilter for getCompTexLOD, so the MipFilter is not checked.
      components = 0b0001;
      break;
    case FetchOpcode::kGetTextureWeights:
      switch (dimension) {
        case xenos::FetchOpDimension::k1D:
          components = 0b1001;
          break;
        case xenos::FetchOpDimension::k2D:
        case xenos::FetchOpDimension::kCube:
          components = 0b1011;
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          components = 0b1111;
          break;
      }
      if (attributes.mip_filter == xenos::TextureFilter::kBaseMap ||
          attributes.mip_filter == xenos::TextureFilter::kPoint) {
        components &= ~static_cast<u32>(0b1000);
      }
      break;
    case FetchOpcode::kSetTextureLOD:
    case FetchOpcode::kSetTextureGradientsHorz:
    case FetchOpcode::kSetTextureGradientsVert:
      components = 0b0000;
      break;
    default:
      REX_UNHANDLED_CASE(opcode);
  }
  return result.GetUsedResultComponents() & components;
}

static void ParseAluInstructionOperand(const AluInstruction& op, u32 i, u32 swizzle_component_count,
                                       InstructionOperand& out_op) {
  out_op.is_negated = op.IsSrcNegated(i);
  u32 reg = op.GetSrcReg(i);
  if (op.IsSrcTemp(i)) {
    out_op.storage_source = InstructionStorageSource::kRegister;
    out_op.storage_index = AluInstruction::GetSrcTempReg(reg);
    out_op.is_absolute_value = AluInstruction::IsSrcTempValueAbsolute(reg);
    out_op.storage_addressing_mode = AluInstruction::IsSrcTempRelative(reg)
                                         ? InstructionStorageAddressingMode::kLoopRelative
                                         : InstructionStorageAddressingMode::kAbsolute;
  } else {
    out_op.storage_source = InstructionStorageSource::kConstantFloat;
    out_op.storage_index = reg;
    if (op.IsSrcConstAddressed(i)) {
      if (op.IsConstAddressRegisterRelative()) {
        out_op.storage_addressing_mode = InstructionStorageAddressingMode::kAddressRegisterRelative;
      } else {
        out_op.storage_addressing_mode = InstructionStorageAddressingMode::kLoopRelative;
      }
    } else {
      out_op.storage_addressing_mode = InstructionStorageAddressingMode::kAbsolute;
    }
    out_op.is_absolute_value = op.GetAbsConstants();
  }
  out_op.component_count = swizzle_component_count;
  u32 swizzle = op.GetSrcSwizzle(i);
  if (swizzle_component_count == 1) {
    // Scalar `a` (W).
    out_op.components[0] = GetSwizzledAluSourceComponent(swizzle, 3);
  } else if (swizzle_component_count == 2) {
    // Scalar left-hand `a` (W) and right-hand `b` (X).
    out_op.components[0] = GetSwizzledAluSourceComponent(swizzle, 3);
    out_op.components[1] = GetSwizzledAluSourceComponent(swizzle, 0);
  } else if (swizzle_component_count == 3) {
    REX_ASSERT_ALWAYS();
  } else if (swizzle_component_count == 4) {
    for (u32 j = 0; j < swizzle_component_count; ++j) {
      out_op.components[j] = GetSwizzledAluSourceComponent(swizzle, j);
    }
  }
}

/**
 * Reports whether the vector operation is the default nop.
 *
 * When the vector and scalar operations are both nop, an export is marked as such rather than as
 * `mov r0._, r0 + retain_prev r0._`. The round trip is exact only for the o0 or oC0 target, since
 * the XNA assembler forces o0 or oC0 when the total write mask is empty.
 *
 * @return True when the vector operation is the default nop.
 */
bool ParsedAluInstruction::IsVectorOpDefaultNop() const {
  if (vector_opcode != xenos::ucode::AluVectorOpcode::kMax ||
      vector_and_constant_result.original_write_mask || vector_and_constant_result.is_clamped ||
      vector_operands[0].storage_source != InstructionStorageSource::kRegister ||
      vector_operands[0].storage_index != 0 ||
      vector_operands[0].storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute ||
      vector_operands[0].is_negated || vector_operands[0].is_absolute_value ||
      !vector_operands[0].IsStandardSwizzle() ||
      vector_operands[1].storage_source != InstructionStorageSource::kRegister ||
      vector_operands[1].storage_index != 0 ||
      vector_operands[1].storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute ||
      vector_operands[1].is_negated || vector_operands[1].is_absolute_value ||
      !vector_operands[1].IsStandardSwizzle()) {
    return false;
  }
  if (vector_and_constant_result.storage_target == InstructionStorageTarget::kRegister) {
    if (vector_and_constant_result.storage_index != 0 ||
        vector_and_constant_result.storage_addressing_mode !=
            InstructionStorageAddressingMode::kAbsolute) {
      return false;
    }
  } else {
    if (IsScalarOpDefaultNop()) {
      return false;
    }
  }
  return true;
}

void ParseAluInstruction(const AluInstruction& op, xenos::ShaderType shader_type,
                         ParsedAluInstruction& instr) {
  instr.is_predicated = op.IsPredicated();
  instr.predicate_condition = op.GetPredicateCondition();

  bool is_export = op.IsExport();

  InstructionStorageTarget storage_target = InstructionStorageTarget::kRegister;
  u32 storage_index_export = 0;
  if (is_export) {
    storage_target = InstructionStorageTarget::kNone;
    // Both vector and scalar operation export to vector_dest.
    ExportRegister export_register = ExportRegister(op.GetVectorDest());
    if (export_register == ExportRegister::kExportAddress) {
      storage_target = InstructionStorageTarget::kExportAddress;
    } else if (export_register >= ExportRegister::kExportData0 &&
               export_register <= ExportRegister::kExportData4) {
      storage_target = InstructionStorageTarget::kExportData;
      storage_index_export =
          static_cast<u32>(export_register) - std::to_underlying(ExportRegister::kExportData0);
    } else if (shader_type == xenos::ShaderType::kVertex) {
      if (export_register >= ExportRegister::kVSInterpolator0 &&
          export_register <= ExportRegister::kVSInterpolator15) {
        storage_target = InstructionStorageTarget::kInterpolator;
        storage_index_export = static_cast<u32>(export_register) -
                               std::to_underlying(ExportRegister::kVSInterpolator0);
      } else if (export_register == ExportRegister::kVSPosition) {
        storage_target = InstructionStorageTarget::kPosition;
      } else if (export_register == ExportRegister::kVSPointSizeEdgeFlagKillVertex) {
        storage_target = InstructionStorageTarget::kPointSizeEdgeFlagKillVertex;
      }
    } else if (shader_type == xenos::ShaderType::kPixel) {
      if (export_register >= ExportRegister::kPSColor0 &&
          export_register <= ExportRegister::kPSColor3) {
        storage_target = InstructionStorageTarget::kColor;
        storage_index_export =
            static_cast<u32>(export_register) - std::to_underlying(ExportRegister::kPSColor0);
      } else if (export_register == ExportRegister::kPSDepth) {
        storage_target = InstructionStorageTarget::kDepth;
      }
    }
    if (storage_target == InstructionStorageTarget::kNone) {
      REX_ASSERT_ALWAYS();
      REXLOG_ERROR(gpu,
                   "ShaderTranslator::ParseAluInstruction: Unsupported write to export "
                   "{}",
                   static_cast<u32>(export_register));
    }
  }

  xenos::ucode::AluVectorOpcode vector_opcode = op.GetVectorOpcode();
  instr.vector_opcode = vector_opcode;
  const xenos::ucode::AluVectorOpcodeInfo& vector_opcode_info =
      xenos::ucode::GetAluVectorOpcodeInfo(vector_opcode);
  instr.vector_opcode_name = vector_opcode_info.name;

  instr.vector_and_constant_result.storage_target = storage_target;
  instr.vector_and_constant_result.storage_addressing_mode =
      InstructionStorageAddressingMode::kAbsolute;
  if (is_export) {
    instr.vector_and_constant_result.storage_index = storage_index_export;
  } else {
    instr.vector_and_constant_result.storage_index = op.GetVectorDest();
    if (op.IsVectorDestRelative()) {
      instr.vector_and_constant_result.storage_addressing_mode =
          InstructionStorageAddressingMode::kLoopRelative;
    }
  }
  instr.vector_and_constant_result.is_clamped = op.GetVectorClamp();
  u32 constant_0_mask = op.GetConstant0WriteMask();
  u32 constant_1_mask = op.GetConstant1WriteMask();
  instr.vector_and_constant_result.original_write_mask =
      op.GetVectorOpResultWriteMask() | constant_0_mask | constant_1_mask;
  for (u32 i = 0; i < 4; ++i) {
    SwizzleSource component = GetSwizzleFromComponentIndex(i);
    if (constant_0_mask & (1 << i)) {
      component = SwizzleSource::k0;
    } else if (constant_1_mask & (1 << i)) {
      component = SwizzleSource::k1;
    }
    instr.vector_and_constant_result.components[i] = component;
  }

  instr.vector_operand_count = vector_opcode_info.GetOperandCount();
  for (u32 i = 0; i < instr.vector_operand_count; ++i) {
    InstructionOperand& vector_operand = instr.vector_operands[i];
    ParseAluInstructionOperand(op, i + 1, 4, vector_operand);
  }

  xenos::ucode::AluScalarOpcode scalar_opcode = op.GetScalarOpcode();
  instr.scalar_opcode = scalar_opcode;
  const xenos::ucode::AluScalarOpcodeInfo& scalar_opcode_info =
      xenos::ucode::GetAluScalarOpcodeInfo(scalar_opcode);
  instr.scalar_opcode_name = scalar_opcode_info.name;

  instr.scalar_result.storage_target = storage_target;
  instr.scalar_result.storage_addressing_mode = InstructionStorageAddressingMode::kAbsolute;
  if (is_export) {
    instr.scalar_result.storage_index = storage_index_export;
  } else {
    instr.scalar_result.storage_index = op.GetScalarDest();
    if (op.IsScalarDestRelative()) {
      instr.scalar_result.storage_addressing_mode = InstructionStorageAddressingMode::kLoopRelative;
    }
  }
  instr.scalar_result.is_clamped = op.GetScalarClamp();
  instr.scalar_result.original_write_mask = op.GetScalarOpResultWriteMask();
  for (u32 i = 0; i < 4; ++i) {
    instr.scalar_result.components[i] = GetSwizzleFromComponentIndex(i);
  }

  instr.scalar_operand_count = scalar_opcode_info.operand_count;
  if (instr.scalar_operand_count) {
    if (instr.scalar_operand_count == 1) {
      ParseAluInstructionOperand(op, 3, scalar_opcode_info.single_operand_is_two_component ? 2 : 1,
                                 instr.scalar_operands[0]);
    } else {
      bool src3_negate = op.IsSrcNegated(3);
      u32 src3_swizzle = op.GetSrcSwizzle(3);

      // Left-hand constant operand (`a`, W swizzle).
      InstructionOperand& const_op = instr.scalar_operands[0];
      const_op.is_negated = src3_negate;
      const_op.is_absolute_value = op.GetAbsConstants();
      const_op.storage_source = InstructionStorageSource::kConstantFloat;
      const_op.storage_index = op.GetSrcReg(3);
      if (op.IsSrcConstAddressed(3)) {
        if (op.IsConstAddressRegisterRelative()) {
          const_op.storage_addressing_mode =
              InstructionStorageAddressingMode::kAddressRegisterRelative;
        } else {
          const_op.storage_addressing_mode = InstructionStorageAddressingMode::kLoopRelative;
        }
      } else {
        const_op.storage_addressing_mode = InstructionStorageAddressingMode::kAbsolute;
      }
      const_op.component_count = 1;
      const_op.components[0] = GetSwizzledAluSourceComponent(src3_swizzle, 3);

      // Right-hand temporary register operand (`b`, X swizzle).
      InstructionOperand& temp_op = instr.scalar_operands[1];
      temp_op.is_negated = src3_negate;
      temp_op.is_absolute_value = op.GetAbsConstants();
      temp_op.storage_source = InstructionStorageSource::kRegister;
      temp_op.storage_index = op.GetScalarConstRegOpSrcTempReg();
      temp_op.storage_addressing_mode = InstructionStorageAddressingMode::kAbsolute;
      temp_op.component_count = 1;
      temp_op.components[0] = GetSwizzledAluSourceComponent(src3_swizzle, 0);
    }
  }
}

bool ParsedAluInstruction::IsScalarOpDefaultNop() const {
  if (scalar_opcode != xenos::ucode::AluScalarOpcode::kRetainPrev ||
      scalar_result.original_write_mask || scalar_result.is_clamped) {
    return false;
  }
  if (scalar_result.storage_target == InstructionStorageTarget::kRegister) {
    if (scalar_result.storage_index != 0 ||
        scalar_result.storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute) {
      return false;
    }
  }
  // For exports with both nop, the vector operation is kept to mark the destination as an export.
  return true;
}

bool ParsedAluInstruction::IsNop() const {
  return scalar_opcode == xenos::ucode::AluScalarOpcode::kRetainPrev &&
         !scalar_result.GetUsedWriteMask() && !vector_and_constant_result.GetUsedWriteMask() &&
         !xenos::ucode::GetAluVectorOpcodeInfo(vector_opcode).changed_state;
}

u32 ParsedAluInstruction::GetMemExportStreamConstant() const {
  if (vector_and_constant_result.storage_target == InstructionStorageTarget::kExportAddress &&
      vector_opcode == xenos::ucode::AluVectorOpcode::kMad &&
      vector_and_constant_result.GetUsedResultComponents() == 0b1111 &&
      !vector_and_constant_result.is_clamped &&
      vector_operands[2].storage_source == InstructionStorageSource::kConstantFloat &&
      vector_operands[2].storage_addressing_mode == InstructionStorageAddressingMode::kAbsolute &&
      vector_operands[2].IsStandardSwizzle() && !vector_operands[2].is_negated &&
      !vector_operands[2].is_absolute_value) {
    return vector_operands[2].storage_index;
  }
  return UINT32_MAX;
}

}  // namespace rex::gpu
