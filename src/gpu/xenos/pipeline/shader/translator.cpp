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

  cf_pair_index_bound_ = static_cast<uint32_t>(ucode_data_.size() / 3);
  for (uint32_t i = 0; i < cf_pair_index_bound_; ++i) {
    ControlFlowInstruction cf_ab[2];
    UnpackControlFlowInstructions(ucode_data_.data() + i * 3, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      const ControlFlowInstruction& cf = cf_ab[j];
      if (IsControlFlowOpcodeExec(cf.opcode())) {
        cf_pair_index_bound_ = std::min(cf_pair_index_bound_, cf.exec.address());
      }
      switch (cf.opcode()) {
        case ControlFlowOpcode::kCondCall:
          label_addresses_.insert(cf.cond_call.address());
          break;
        case ControlFlowOpcode::kCondJmp:
          label_addresses_.insert(cf.cond_jmp.address());
          break;
        case ControlFlowOpcode::kLoopStart:
          label_addresses_.insert(cf.loop_start.address());
          break;
        case ControlFlowOpcode::kLoopEnd:
          label_addresses_.insert(cf.loop_end.address());
          break;
        default:
          break;
      }
    }
  }

  ucode_disasm_buffer.Reset();
  VertexFetchInstruction previous_vfetch_full{};
  uint32_t unique_texture_bindings = 0;
  for (uint32_t i = 0; i < cf_pair_index_bound_; ++i) {
    ControlFlowInstruction cf_ab[2];
    UnpackControlFlowInstructions(ucode_data_.data() + i * 3, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      uint32_t cf_index = i * 2 + j;
      if (label_addresses_.find(cf_index) != label_addresses_.end()) {
        ucode_disasm_buffer.AppendFormat("                label L{}\n", cf_index);
      }
      ucode_disasm_buffer.AppendFormat("/* {:4d}.{} */ ", i, j);

      const ControlFlowInstruction& cf = cf_ab[j];
      uint32_t bool_constant_index = UINT32_MAX;
      switch (cf.opcode()) {
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
          bool_constant_index = cf.cond_exec.bool_address();
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
          constant_register_map_.loop_bitmap |= static_cast<uint32_t>(1)
                                                << instr.loop_constant_index;
        } break;
        case ControlFlowOpcode::kLoopEnd: {
          ParsedLoopEndInstruction instr;
          ParseControlFlowLoopEnd(cf.loop_end, cf_index, instr);
          instr.Disassemble(&ucode_disasm_buffer);
          constant_register_map_.loop_bitmap |= static_cast<uint32_t>(1)
                                                << instr.loop_constant_index;
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
          ParseControlFlowAlloc(cf.alloc, cf_index, type() == xenos::ShaderType::kVertex, instr);
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
            static_cast<uint32_t>(1) << (bool_constant_index % 32);
      }
    }
  }
  ucode_disassembly_ = ucode_disasm_buffer.to_string();

  if (constant_register_map_.float_dynamic_addressing) {
    constant_register_map_.float_count = 256;
    memset(constant_register_map_.float_bitmap, UINT8_MAX,
           sizeof(constant_register_map_.float_bitmap));
  } else {
    constant_register_map_.float_count = 0;
    for (int i = 0; i < 4; ++i) {
      // Each bit is 1 vec4 of 4 floats.
      constant_register_map_.float_count += rex::bit_count(constant_register_map_.float_bitmap[i]);
    }
  }

  if (!cf_memexport_info_.empty()) {
    std::vector<uint32_t> successor_stack;
    for (uint32_t i = 0; i < cf_pair_index_bound_; ++i) {
      ControlFlowInstruction eM_writing_cf_ab[2];
      UnpackControlFlowInstructions(ucode_data_.data() + i * 3, eM_writing_cf_ab);
      for (uint32_t j = 0; j < 2; ++j) {
        uint32_t eM_writing_cf_index = i * 2 + j;
        uint32_t eM_written_by_cf_instr =
            cf_memexport_info_[eM_writing_cf_index].eM_potentially_written_by_exec;
        if (eM_writing_cf_ab[j].opcode() == ControlFlowOpcode::kCondCall) {
          eM_written_by_cf_instr = memexport_eM_written_;
        }
        if (!eM_written_by_cf_instr) {
          continue;
        }

        bool is_successor_graph_head = true;
        successor_stack.push_back(eM_writing_cf_index);
        while (!successor_stack.empty()) {
          uint32_t successor_cf_index = successor_stack.back();
          successor_stack.pop_back();

          ControlFlowMemExportInfo& successor_memexport_info =
              cf_memexport_info_[successor_cf_index];
          if ((successor_memexport_info.eM_potentially_written_before & eM_written_by_cf_instr) ==
              eM_written_by_cf_instr) {
            break;
          }
          if (!is_successor_graph_head) {
            successor_memexport_info.eM_potentially_written_before |= eM_written_by_cf_instr;
          }
          is_successor_graph_head = false;

          ControlFlowInstruction successor_cf_ab[2];
          UnpackControlFlowInstructions(ucode_data_.data() + (successor_cf_index >> 1) * 3,
                                        successor_cf_ab);
          const ControlFlowInstruction& successor_cf = successor_cf_ab[successor_cf_index & 1];

          bool next_instr_is_new_successor = true;
          switch (successor_cf.opcode()) {
            case ControlFlowOpcode::kExecEnd:
              memexport_eM_potentially_written_before_end_ |= eM_written_by_cf_instr;
              next_instr_is_new_successor = false;
              break;
            case ControlFlowOpcode::kCondExecEnd:
            case ControlFlowOpcode::kCondExecPredEnd:
            case ControlFlowOpcode::kCondExecPredCleanEnd:
              memexport_eM_potentially_written_before_end_ |= eM_written_by_cf_instr;
              break;
            case ControlFlowOpcode::kLoopStart:
              successor_stack.push_back(successor_cf.loop_start.address());
              break;
            case ControlFlowOpcode::kLoopEnd:
              successor_stack.push_back(successor_cf.loop_end.address());
              break;
            case ControlFlowOpcode::kCondCall:
              successor_stack.push_back(successor_cf.cond_call.address());
              break;
            case ControlFlowOpcode::kReturn:
              next_instr_is_new_successor = false;
              break;
            case ControlFlowOpcode::kCondJmp:
              successor_stack.push_back(successor_cf.cond_jmp.address());
              if (successor_cf.cond_jmp.is_unconditional()) {
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
              memexport_eM_potentially_written_before_end_ |= eM_written_by_cf_instr;
            }
          }
        }
      }
    }
  }

  is_ucode_analyzed_ = true;

  if (!REXCVAR_GET(::rex::gpu::cvars::dump_shaders).empty() && !ucode_data().empty()) {
    DumpUcode(REXCVAR_GET(::rex::gpu::cvars::dump_shaders));
  }
}

uint32_t Shader::GetInterpolatorInputMask(xenos::reg::SQ_PROGRAM_CNTL sq_program_cntl,
                                          xenos::reg::SQ_CONTEXT_MISC sq_context_misc,
                                          uint32_t& param_gen_pos_out) const {
  REX_ASSERT(type() == xenos::ShaderType::kPixel);
  uint32_t interpolator_count =
      std::min(xenos::kMaxInterpolators,
               std::max(register_static_address_bound(),
                        GetDynamicAddressableRegisterCount(sq_program_cntl.ps_num_reg)));
  uint32_t interpolator_mask = (UINT32_C(1) << interpolator_count) - 1;
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
                                   uint32_t& unique_texture_bindings,
                                   StringBuffer& ucode_disasm_buffer) {
  instr.Disassemble(&ucode_disasm_buffer);
  uint32_t sequence = instr.sequence;
  for (uint32_t instr_offset = instr.instruction_address;
       instr_offset < instr.instruction_address + instr.instruction_count;
       ++instr_offset, sequence >>= 2) {
    ucode_disasm_buffer.AppendFormat("/* {:4d}   */ ", instr_offset);
    if (sequence & 0b10) {
      ucode_disasm_buffer.Append("         serialize\n             ");
    }
    const uint32_t* op_ptr = ucode_data_.data() + instr_offset * 3;
    if (sequence & 0b01) {
      auto& op = *reinterpret_cast<const FetchInstruction*>(op_ptr);
      if (op.opcode() == FetchOpcode::kVertexFetch) {
        GatherVertexFetchInformation(op.vertex_fetch(), previous_vfetch_full, ucode_disasm_buffer);
      } else {
        GatherTextureFetchInformation(op.texture_fetch(), unique_texture_bindings,
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
    if (vertex_binding.fetch_constant == op.fetch_constant_index()) {
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
    vertex_binding.fetch_constant = op.fetch_constant_index();
    vertex_binding.stride_words = fetch_instr.attributes.stride;
    vertex_binding.attributes.push_back({});
    vertex_bindings_.emplace_back(std::move(vertex_binding));
    attrib = &vertex_bindings_.back().attributes.back();
  }

  attrib->fetch_instr = fetch_instr;
}

void Shader::GatherTextureFetchInformation(const TextureFetchInstruction& op,
                                           uint32_t& unique_texture_bindings,
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

  switch (op.opcode()) {
    case FetchOpcode::kSetTextureLod:
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

  texture_bindings_.emplace_back(std::move(binding));
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
void Shader::GatherAluInstructionInformation(const AluInstruction& op, uint32_t exec_cf_index,
                                             StringBuffer& ucode_disasm_buffer) {
  ParsedAluInstruction instr;
  ParseAluInstruction(op, type(), instr);
  instr.Disassemble(&ucode_disasm_buffer);

  kills_pixels_ = kills_pixels_ ||
                  (xenos::ucode::GetAluVectorOpcodeInfo(op.vector_opcode()).changed_state &
                   xenos::ucode::kAluOpChangedStatePixelKill) ||
                  (xenos::ucode::GetAluScalarOpcodeInfo(op.scalar_opcode()).changed_state &
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
    uint32_t memexport_stream_constant = instr.GetMemExportStreamConstant();
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
        register_static_address_bound_ = std::max(register_static_address_bound_,
                                                  operand.storage_index + static_cast<uint32_t>(1));
      } else {
        uses_register_dynamic_addressing_ = true;
      }
      break;
    case InstructionStorageSource::kConstantFloat:
      if (operand.storage_addressing_mode == InstructionStorageAddressingMode::kAbsolute) {
        // Gathered before translation so it can pack indices unless dynamically indexed.
        constant_register_map_.float_bitmap[operand.storage_index >> 6] |=
            static_cast<uint64_t>(1) << (operand.storage_index & 63);
      } else {
        constant_register_map_.float_dynamic_addressing = true;
      }
      break;
    case InstructionStorageSource::kVertexFetchConstant:
      constant_register_map_.vertex_fetch_bitmap[operand.storage_index >> 5] |=
          static_cast<uint32_t>(1) << (operand.storage_index & 31);
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
        std::max(register_static_address_bound_, result.storage_index + static_cast<uint32_t>(1));
  } else {
    uses_register_dynamic_addressing_ = true;
  }
}

void Shader::GatherAluResultInformation(const InstructionResult& result, uint32_t exec_cf_index) {
  uint32_t used_write_mask = result.GetUsedWriteMask();
  if (!used_write_mask) {
    return;
  }
  switch (result.storage_target) {
    case InstructionStorageTarget::kRegister:
      if (result.storage_addressing_mode == InstructionStorageAddressingMode::kAbsolute) {
        register_static_address_bound_ = std::max(register_static_address_bound_,
                                                  result.storage_index + static_cast<uint32_t>(1));
      } else {
        uses_register_dynamic_addressing_ = true;
      }
      break;
    case InstructionStorageTarget::kInterpolator:
      writes_interpolators_ |= static_cast<uint32_t>(1) << result.storage_index;
      break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      writes_point_size_edge_flag_kill_vertex_ |= used_write_mask;
      break;
    case InstructionStorageTarget::kExportData:
      memexport_eM_written_ |= static_cast<uint8_t>(1) << result.storage_index;
      if (cf_memexport_info_.empty()) {
        cf_memexport_info_.resize(2 * cf_pair_index_bound_);
      }
      cf_memexport_info_[exec_cf_index].eM_potentially_written_by_exec |= static_cast<uint32_t>(1)
                                                                          << result.storage_index;
      break;
    case InstructionStorageTarget::kColor:
      writes_color_targets_ |= static_cast<uint32_t>(1) << result.storage_index;
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
  const Shader& shader = translation.shader();
  REX_ASSERT(shader.is_ucode_analyzed());
  if (!shader.is_ucode_analyzed()) {
    REX_FAIL(gpu, false, "AnalyzeUcode must be done on the shader before translation");
  }
  translation_ = &translation;

  Reset();

  register_count_ = shader.register_static_address_bound();
  if (shader.uses_register_dynamic_addressing()) {
    // End of r# space registers may be dynamically addressed, so allocate SQ_PROGRAM_CNTL count.
    register_count_ = std::max(register_count_, GetModificationRegisterCount());
  }

  StartTranslation();

  const uint32_t* ucode_dwords = shader.ucode_data().data();

  uint32_t cf_pair_index_bound = shader.cf_pair_index_bound();
  const std::set<uint32_t>& label_addresses = shader.label_addresses();
  for (uint32_t i = 0; i < cf_pair_index_bound; ++i) {
    ControlFlowInstruction cf_ab[2];
    UnpackControlFlowInstructions(ucode_dwords + i * 3, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      uint32_t cf_index = i * 2 + j;
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
  switch (cf.opcode()) {
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
      ParseControlFlowAlloc(cf.alloc, cf_index_, is_vertex_shader(), instr);
      const std::vector<Shader::ControlFlowMemExportInfo>& cf_memexport_info =
          current_shader().cf_memexport_info();
      ProcessAllocInstruction(
          instr, instr.dword_index < cf_memexport_info.size()
                     ? cf_memexport_info[instr.dword_index].eM_potentially_written_before
                     : 0);
    } break;
    case ControlFlowOpcode::kMarkVsFetchDone:
      break;
    default:
      REX_UNHANDLED_CASE(cf.opcode);
      break;
  }
}

void ParseControlFlowExec(const ControlFlowExecInstruction& cf, uint32_t cf_index,
                          ParsedExecInstruction& instr) {
  instr.dword_index = cf_index;
  instr.opcode = cf.opcode();
  instr.opcode_name = cf.opcode() == ControlFlowOpcode::kExecEnd ? "exece" : "exec";
  instr.instruction_address = cf.address();
  instr.instruction_count = cf.count();
  instr.type = ParsedExecInstruction::Type::kUnconditional;
  instr.is_end = cf.opcode() == ControlFlowOpcode::kExecEnd;
  instr.is_predicate_clean = cf.is_predicate_clean();
  instr.is_yield = cf.is_yield();
  instr.sequence = cf.sequence();
}

void ParseControlFlowCondExec(const ControlFlowCondExecInstruction& cf, uint32_t cf_index,
                              ParsedExecInstruction& instr) {
  instr.dword_index = cf_index;
  instr.opcode = cf.opcode();
  instr.opcode_name = "cexec";
  switch (cf.opcode()) {
    case ControlFlowOpcode::kCondExecEnd:
    case ControlFlowOpcode::kCondExecPredCleanEnd:
      instr.opcode_name = "cexece";
      instr.is_end = true;
      break;
    default:
      break;
  }
  instr.instruction_address = cf.address();
  instr.instruction_count = cf.count();
  instr.type = ParsedExecInstruction::Type::kConditional;
  instr.bool_constant_index = cf.bool_address();
  instr.condition = cf.condition();
  switch (cf.opcode()) {
    case ControlFlowOpcode::kCondExec:
    case ControlFlowOpcode::kCondExecEnd:
      instr.is_predicate_clean = false;
      break;
    default:
      break;
  }
  instr.is_yield = cf.is_yield();
  instr.sequence = cf.sequence();
}

void ParseControlFlowCondExecPred(const ControlFlowCondExecPredInstruction& cf, uint32_t cf_index,
                                  ParsedExecInstruction& instr) {
  instr.dword_index = cf_index;
  instr.opcode = cf.opcode();
  instr.opcode_name = cf.opcode() == ControlFlowOpcode::kCondExecPredEnd ? "exece" : "exec";
  instr.instruction_address = cf.address();
  instr.instruction_count = cf.count();
  instr.type = ParsedExecInstruction::Type::kPredicated;
  instr.condition = cf.condition();
  instr.is_end = cf.opcode() == ControlFlowOpcode::kCondExecPredEnd;
  instr.is_predicate_clean = cf.is_predicate_clean();
  instr.is_yield = cf.is_yield();
  instr.sequence = cf.sequence();
}

void ParseControlFlowLoopStart(const ControlFlowLoopStartInstruction& cf, uint32_t cf_index,
                               ParsedLoopStartInstruction& instr) {
  instr.dword_index = cf_index;
  instr.loop_constant_index = cf.loop_id();
  instr.is_repeat = cf.is_repeat();
  instr.loop_skip_address = cf.address();
}

void ParseControlFlowLoopEnd(const ControlFlowLoopEndInstruction& cf, uint32_t cf_index,
                             ParsedLoopEndInstruction& instr) {
  instr.dword_index = cf_index;
  instr.is_predicated_break = cf.is_predicated_break();
  instr.predicate_condition = cf.condition();
  instr.loop_constant_index = cf.loop_id();
  instr.loop_body_address = cf.address();
}

void ParseControlFlowCondCall(const ControlFlowCondCallInstruction& cf, uint32_t cf_index,
                              ParsedCallInstruction& instr) {
  instr.dword_index = cf_index;
  instr.target_address = cf.address();
  if (cf.is_unconditional()) {
    instr.type = ParsedCallInstruction::Type::kUnconditional;
  } else if (cf.is_predicated()) {
    instr.type = ParsedCallInstruction::Type::kPredicated;
    instr.condition = cf.condition();
  } else {
    instr.type = ParsedCallInstruction::Type::kConditional;
    instr.bool_constant_index = cf.bool_address();
    instr.condition = cf.condition();
  }
}

void ParseControlFlowReturn([[maybe_unused]] const ControlFlowReturnInstruction& cf,
                            uint32_t cf_index, ParsedReturnInstruction& instr) {
  instr.dword_index = cf_index;
}

void ParseControlFlowCondJmp(const ControlFlowCondJmpInstruction& cf, uint32_t cf_index,
                             ParsedJumpInstruction& instr) {
  instr.dword_index = cf_index;
  instr.target_address = cf.address();
  if (cf.is_unconditional()) {
    instr.type = ParsedJumpInstruction::Type::kUnconditional;
  } else if (cf.is_predicated()) {
    instr.type = ParsedJumpInstruction::Type::kPredicated;
    instr.condition = cf.condition();
  } else {
    instr.type = ParsedJumpInstruction::Type::kConditional;
    instr.bool_constant_index = cf.bool_address();
    instr.condition = cf.condition();
  }
}

void ParseControlFlowAlloc(const ControlFlowAllocInstruction& cf, uint32_t cf_index,
                           bool is_vertex_shader, ParsedAllocInstruction& instr) {
  instr.dword_index = cf_index;
  instr.type = cf.alloc_type();
  instr.count = cf.size();
  instr.is_vertex_shader = is_vertex_shader;
}

void ShaderTranslator::TranslateExecInstructions(const ParsedExecInstruction& instr) {
  ProcessExecInstructionBegin(instr);

  const std::vector<Shader::ControlFlowMemExportInfo>& cf_memexport_info =
      current_shader().cf_memexport_info();
  uint8_t eM_potentially_written_before =
      instr.dword_index < cf_memexport_info.size()
          ? cf_memexport_info[instr.dword_index].eM_potentially_written_before
          : 0;

  const uint32_t* ucode_dwords = current_shader().ucode_data().data();
  uint32_t sequence = instr.sequence;
  for (uint32_t instr_offset = instr.instruction_address;
       instr_offset < instr.instruction_address + instr.instruction_count;
       ++instr_offset, sequence >>= 2) {
    const uint32_t* op_ptr = ucode_dwords + instr_offset * 3;
    if (sequence & 0b01) {
      auto& op = *reinterpret_cast<const FetchInstruction*>(op_ptr);
      if (op.opcode() == FetchOpcode::kVertexFetch) {
        const VertexFetchInstruction& vfetch_op = op.vertex_fetch();
        ParsedVertexFetchInstruction vfetch_instr;
        if (ParseVertexFetchInstruction(vfetch_op, previous_vfetch_full_, vfetch_instr)) {
          previous_vfetch_full_ = vfetch_op;
        }
        ProcessVertexFetchInstruction(vfetch_instr);
      } else {
        ParsedTextureFetchInstruction tfetch_instr;
        ParseTextureFetchInstruction(op.texture_fetch(), tfetch_instr);
        ProcessTextureFetchInstruction(tfetch_instr);
      }
    } else {
      auto& op = *reinterpret_cast<const AluInstruction*>(op_ptr);
      ParsedAluInstruction alu_instr;
      ParseAluInstruction(op, current_shader().type(), alu_instr);
      ProcessAluInstruction(alu_instr, eM_potentially_written_before);
      if (alu_instr.vector_and_constant_result.storage_target ==
              InstructionStorageTarget::kExportData &&
          alu_instr.vector_and_constant_result.GetUsedWriteMask()) {
        eM_potentially_written_before |= static_cast<uint8_t>(1)
                                         << alu_instr.vector_and_constant_result.storage_index;
      }
      if (alu_instr.scalar_result.storage_target == InstructionStorageTarget::kExportData &&
          alu_instr.scalar_result.GetUsedWriteMask()) {
        eM_potentially_written_before |= static_cast<uint8_t>(1)
                                         << alu_instr.scalar_result.storage_index;
      }
    }
  }

  ProcessExecInstructionEnd(instr);
}

static void ParseFetchInstructionResult(uint32_t dest, uint32_t swizzle, bool is_relative,
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
  instr.opcode_name = op.is_mini_fetch() ? "vfetch_mini" : "vfetch_full";
  instr.is_mini_fetch = op.is_mini_fetch();
  instr.is_predicated = op.is_predicated();
  instr.predicate_condition = op.predicate_condition();

  ParseFetchInstructionResult(op.dest(), op.dest_swizzle(), op.is_dest_relative(), instr.result);

  const auto& full_op = op.is_mini_fetch() ? previous_full_op : op;
  auto& src_op = instr.operands[instr.operand_count++];
  src_op.storage_source = InstructionStorageSource::kRegister;
  src_op.storage_index = full_op.src();
  src_op.storage_addressing_mode = full_op.is_src_relative()
                                       ? InstructionStorageAddressingMode::kLoopRelative
                                       : InstructionStorageAddressingMode::kAbsolute;
  src_op.is_negated = false;
  src_op.is_absolute_value = false;
  src_op.component_count = 1;
  uint32_t swizzle = full_op.src_swizzle();
  for (uint32_t j = 0; j < src_op.component_count; ++j, swizzle >>= 2) {
    src_op.components[j] = GetSwizzleFromComponentIndex(swizzle & 0x3);
  }

  auto& const_op = instr.operands[instr.operand_count++];
  const_op.storage_source = InstructionStorageSource::kVertexFetchConstant;
  const_op.storage_index = full_op.fetch_constant_index();

  instr.attributes.data_format = op.data_format();
  instr.attributes.offset = op.offset();
  instr.attributes.stride = full_op.stride();
  instr.attributes.exp_adjust = op.exp_adjust();
  instr.attributes.prefetch_count = op.prefetch_count();
  instr.attributes.is_index_rounded = full_op.is_index_rounded();
  instr.attributes.is_signed = op.is_signed();
  instr.attributes.is_integer = !op.is_normalized();
  instr.attributes.signed_rf_mode = op.signed_rf_mode();

  return !op.is_mini_fetch();
}

void ParseTextureFetchInstruction(const TextureFetchInstruction& op,
                                  ParsedTextureFetchInstruction& instr) {
  struct TextureFetchOpcodeInfo {
    const char* name;
    bool has_dest;
    bool has_const;
    bool has_attributes;
    uint32_t override_component_count;
  } opcode_info;
  switch (op.opcode()) {
    case FetchOpcode::kTextureFetch: {
      static const char* kNames[] = {"tfetch1D", "tfetch2D", "tfetch3D", "tfetchCube"};
      opcode_info = {kNames[static_cast<int>(op.dimension())], true, true, true, 0};
    } break;
    case FetchOpcode::kGetTextureBorderColorFrac: {
      static const char* kNames[] = {"getBCF1D", "getBCF2D", "getBCF3D", "getBCFCube"};
      opcode_info = {kNames[static_cast<int>(op.dimension())], true, true, true, 0};
    } break;
    case FetchOpcode::kGetTextureComputedLod: {
      static const char* kNames[] = {"getCompTexLOD1D", "getCompTexLOD2D", "getCompTexLOD3D",
                                     "getCompTexLODCube"};
      opcode_info = {kNames[static_cast<int>(op.dimension())], true, true, true, 0};
    } break;
    case FetchOpcode::kGetTextureGradients:
      opcode_info = {"getGradients", true, true, true, 2};
      break;
    case FetchOpcode::kGetTextureWeights: {
      static const char* kNames[] = {"getWeights1D", "getWeights2D", "getWeights3D",
                                     "getWeightsCube"};
      opcode_info = {kNames[static_cast<int>(op.dimension())], true, true, true, 0};
    } break;
    case FetchOpcode::kSetTextureLod:
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

  instr.opcode = op.opcode();
  instr.opcode_name = opcode_info.name;
  instr.dimension = op.dimension();
  instr.is_predicated = op.is_predicated();
  instr.predicate_condition = op.predicate_condition();

  if (opcode_info.has_dest) {
    ParseFetchInstructionResult(op.dest(), op.dest_swizzle(), op.is_dest_relative(), instr.result);
  } else {
    instr.result.storage_target = InstructionStorageTarget::kNone;
  }

  auto& src_op = instr.operands[instr.operand_count++];
  src_op.storage_source = InstructionStorageSource::kRegister;
  src_op.storage_index = op.src();
  src_op.storage_addressing_mode = op.is_src_relative()
                                       ? InstructionStorageAddressingMode::kLoopRelative
                                       : InstructionStorageAddressingMode::kAbsolute;
  src_op.is_negated = false;
  src_op.is_absolute_value = false;
  src_op.component_count = opcode_info.override_component_count
                               ? opcode_info.override_component_count
                               : xenos::GetFetchOpDimensionComponentCount(op.dimension());
  uint32_t swizzle = op.src_swizzle();
  for (uint32_t j = 0; j < src_op.component_count; ++j, swizzle >>= 2) {
    src_op.components[j] = GetSwizzleFromComponentIndex(swizzle & 0x3);
  }

  if (opcode_info.has_const) {
    auto& const_op = instr.operands[instr.operand_count++];
    const_op.storage_source = InstructionStorageSource::kTextureFetchConstant;
    const_op.storage_index = op.fetch_constant_index();
  }

  if (opcode_info.has_attributes) {
    instr.attributes.fetch_valid_only = op.fetch_valid_only();
    instr.attributes.unnormalized_coordinates = op.unnormalized_coordinates();
    instr.attributes.mag_filter = op.mag_filter();
    instr.attributes.min_filter = op.min_filter();
    instr.attributes.mip_filter = op.mip_filter();
    instr.attributes.aniso_filter = op.aniso_filter();
    instr.attributes.vol_mag_filter = op.vol_mag_filter();
    instr.attributes.vol_min_filter = op.vol_min_filter();
    instr.attributes.use_computed_lod = op.use_computed_lod();
    instr.attributes.use_register_lod = op.use_register_lod();
    instr.attributes.use_register_gradients = op.use_register_gradients();
    instr.attributes.lod_bias = op.lod_bias();
    instr.attributes.offset_x = op.offset_x();
    instr.attributes.offset_y = op.offset_y();
    instr.attributes.offset_z = op.offset_z();
  }
}

uint32_t ParsedTextureFetchInstruction::GetNonZeroResultComponents() const {
  uint32_t components = 0b0000;
  switch (opcode) {
    case FetchOpcode::kTextureFetch:
    case FetchOpcode::kGetTextureGradients:
      components = 0b1111;
      break;
    case FetchOpcode::kGetTextureBorderColorFrac:
      components = 0b0001;
      break;
    case FetchOpcode::kGetTextureComputedLod:
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
        components &= ~static_cast<uint32_t>(0b1000);
      }
      break;
    case FetchOpcode::kSetTextureLod:
    case FetchOpcode::kSetTextureGradientsHorz:
    case FetchOpcode::kSetTextureGradientsVert:
      components = 0b0000;
      break;
    default:
      REX_UNHANDLED_CASE(opcode);
  }
  return result.GetUsedResultComponents() & components;
}

static void ParseAluInstructionOperand(const AluInstruction& op, uint32_t i,
                                       uint32_t swizzle_component_count,
                                       InstructionOperand& out_op) {
  out_op.is_negated = op.src_negate(i);
  uint32_t reg = op.src_reg(i);
  if (op.src_is_temp(i)) {
    out_op.storage_source = InstructionStorageSource::kRegister;
    out_op.storage_index = AluInstruction::src_temp_reg(reg);
    out_op.is_absolute_value = AluInstruction::is_src_temp_value_absolute(reg);
    out_op.storage_addressing_mode = AluInstruction::is_src_temp_relative(reg)
                                         ? InstructionStorageAddressingMode::kLoopRelative
                                         : InstructionStorageAddressingMode::kAbsolute;
  } else {
    out_op.storage_source = InstructionStorageSource::kConstantFloat;
    out_op.storage_index = reg;
    if (op.src_const_is_addressed(i)) {
      if (op.is_const_address_register_relative()) {
        out_op.storage_addressing_mode = InstructionStorageAddressingMode::kAddressRegisterRelative;
      } else {
        out_op.storage_addressing_mode = InstructionStorageAddressingMode::kLoopRelative;
      }
    } else {
      out_op.storage_addressing_mode = InstructionStorageAddressingMode::kAbsolute;
    }
    out_op.is_absolute_value = op.abs_constants();
  }
  out_op.component_count = swizzle_component_count;
  uint32_t swizzle = op.src_swizzle(i);
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
    for (uint32_t j = 0; j < swizzle_component_count; ++j) {
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
  instr.is_predicated = op.is_predicated();
  instr.predicate_condition = op.predicate_condition();

  bool is_export = op.is_export();

  InstructionStorageTarget storage_target = InstructionStorageTarget::kRegister;
  uint32_t storage_index_export = 0;
  if (is_export) {
    storage_target = InstructionStorageTarget::kNone;
    // Both vector and scalar operation export to vector_dest.
    ExportRegister export_register = ExportRegister(op.vector_dest());
    if (export_register == ExportRegister::kExportAddress) {
      storage_target = InstructionStorageTarget::kExportAddress;
    } else if (export_register >= ExportRegister::kExportData0 &&
               export_register <= ExportRegister::kExportData4) {
      storage_target = InstructionStorageTarget::kExportData;
      storage_index_export =
          static_cast<uint32_t>(export_register) - std::to_underlying(ExportRegister::kExportData0);
    } else if (shader_type == xenos::ShaderType::kVertex) {
      if (export_register >= ExportRegister::kVSInterpolator0 &&
          export_register <= ExportRegister::kVSInterpolator15) {
        storage_target = InstructionStorageTarget::kInterpolator;
        storage_index_export = static_cast<uint32_t>(export_register) -
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
            static_cast<uint32_t>(export_register) - std::to_underlying(ExportRegister::kPSColor0);
      } else if (export_register == ExportRegister::kPSDepth) {
        storage_target = InstructionStorageTarget::kDepth;
      }
    }
    if (storage_target == InstructionStorageTarget::kNone) {
      REX_ASSERT_ALWAYS();
      REXLOG_ERROR(gpu,
                   "ShaderTranslator::ParseAluInstruction: Unsupported write to export "
                   "{}",
                   static_cast<uint32_t>(export_register));
    }
  }

  xenos::ucode::AluVectorOpcode vector_opcode = op.vector_opcode();
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
    instr.vector_and_constant_result.storage_index = op.vector_dest();
    if (op.is_vector_dest_relative()) {
      instr.vector_and_constant_result.storage_addressing_mode =
          InstructionStorageAddressingMode::kLoopRelative;
    }
  }
  instr.vector_and_constant_result.is_clamped = op.vector_clamp();
  uint32_t constant_0_mask = op.GetConstant0WriteMask();
  uint32_t constant_1_mask = op.GetConstant1WriteMask();
  instr.vector_and_constant_result.original_write_mask =
      op.GetVectorOpResultWriteMask() | constant_0_mask | constant_1_mask;
  for (uint32_t i = 0; i < 4; ++i) {
    SwizzleSource component = GetSwizzleFromComponentIndex(i);
    if (constant_0_mask & (1 << i)) {
      component = SwizzleSource::k0;
    } else if (constant_1_mask & (1 << i)) {
      component = SwizzleSource::k1;
    }
    instr.vector_and_constant_result.components[i] = component;
  }

  instr.vector_operand_count = vector_opcode_info.GetOperandCount();
  for (uint32_t i = 0; i < instr.vector_operand_count; ++i) {
    InstructionOperand& vector_operand = instr.vector_operands[i];
    ParseAluInstructionOperand(op, i + 1, 4, vector_operand);
  }

  xenos::ucode::AluScalarOpcode scalar_opcode = op.scalar_opcode();
  instr.scalar_opcode = scalar_opcode;
  const xenos::ucode::AluScalarOpcodeInfo& scalar_opcode_info =
      xenos::ucode::GetAluScalarOpcodeInfo(scalar_opcode);
  instr.scalar_opcode_name = scalar_opcode_info.name;

  instr.scalar_result.storage_target = storage_target;
  instr.scalar_result.storage_addressing_mode = InstructionStorageAddressingMode::kAbsolute;
  if (is_export) {
    instr.scalar_result.storage_index = storage_index_export;
  } else {
    instr.scalar_result.storage_index = op.scalar_dest();
    if (op.is_scalar_dest_relative()) {
      instr.scalar_result.storage_addressing_mode = InstructionStorageAddressingMode::kLoopRelative;
    }
  }
  instr.scalar_result.is_clamped = op.scalar_clamp();
  instr.scalar_result.original_write_mask = op.GetScalarOpResultWriteMask();
  for (uint32_t i = 0; i < 4; ++i) {
    instr.scalar_result.components[i] = GetSwizzleFromComponentIndex(i);
  }

  instr.scalar_operand_count = scalar_opcode_info.operand_count;
  if (instr.scalar_operand_count) {
    if (instr.scalar_operand_count == 1) {
      ParseAluInstructionOperand(op, 3, scalar_opcode_info.single_operand_is_two_component ? 2 : 1,
                                 instr.scalar_operands[0]);
    } else {
      bool src3_negate = op.src_negate(3);
      uint32_t src3_swizzle = op.src_swizzle(3);

      // Left-hand constant operand (`a`, W swizzle).
      InstructionOperand& const_op = instr.scalar_operands[0];
      const_op.is_negated = src3_negate;
      const_op.is_absolute_value = op.abs_constants();
      const_op.storage_source = InstructionStorageSource::kConstantFloat;
      const_op.storage_index = op.src_reg(3);
      if (op.src_const_is_addressed(3)) {
        if (op.is_const_address_register_relative()) {
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
      temp_op.is_absolute_value = op.abs_constants();
      temp_op.storage_source = InstructionStorageSource::kRegister;
      temp_op.storage_index = op.scalar_const_reg_op_src_temp_reg();
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

uint32_t ParsedAluInstruction::GetMemExportStreamConstant() const {
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
