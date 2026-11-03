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

#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <rex/types.h>
#include <rex/utility.h>
#include <rex/xenos/ucode.h>
#include "gpu/xenos/pipeline/shader/shader.h"
#include <rex/xenos/register.h>
#include <rex/xenos/state.h>
#include <rex/math.h>
#include <rex/string/text.h>
#include "gpu/xenos/xenos_export.h"

namespace rex::gpu {

class REX_GPU_XENOS_API ShaderTranslator : public rex::NonCopyable {
 public:
  virtual ~ShaderTranslator();

  virtual u64 GetDefaultVertexShaderModification(
      u32 /*dynamic_addressable_register_count*/,
      Shader::HostVertexShaderType /*host_vertex_shader_type*/ =
          Shader::HostVertexShaderType::kVertex) const {
    return 0;
  }
  virtual u64 GetDefaultPixelShaderModification(u32 /*dynamic_addressable_register_count*/) const {
    return 0;
  }

  /**
   * Translates an analyzed shader.
   *
   * AnalyzeUcode must be done on the shader before translating.
   *
   * @param translation Translation to fill.
   */
  bool TranslateAnalyzedShader(Shader::Translation& translation);

 protected:
  ShaderTranslator();

  /** Resets translator state before beginning translation. */
  virtual void Reset();

  /** Shader and modification being translated. */
  Shader::Translation& GetCurrentTranslation() const { return *translation_; }
  Shader& GetCurrentShader() const { return GetCurrentTranslation().GetShader(); }

  /** Register count from SQ_PROGRAM_CNTL, stored by the implementation in its modification bits. */
  virtual u32 GetModificationRegisterCount() const { return xenos::kMaxShaderTempRegisters; }

  bool IsVertexShader() const { return GetCurrentShader().GetType() == xenos::ShaderType::kVertex; }
  bool IsPixelShader() const { return GetCurrentShader().GetType() == xenos::ShaderType::kPixel; }

  /** Temporary register count, accessible via static and dynamic addressing. */
  u32 GetRegisterCount() const { return register_count_; }

  /**
   * Emits a translation error that is passed back in the result.
   *
   * @param is_fatal True when the error fails the translation.
   */
  virtual void EmitTranslationError(const char* message, bool is_fatal = true);

  /** Called at the start of translation after the vertex and texture bindings are gathered. */
  virtual void StartTranslation() {}

  /** Called at the end of translation after all ucode is processed. */
  virtual std::vector<u8> CompleteTranslation() { return std::vector<u8>(); }

  virtual void PostTranslation() {}
  void SetHostDisassembly(Shader::Translation& translation, std::string value) {
    translation.host_disassembly_ = std::move(value);
  }

  /** Called once per label required by control flow, before the instructions of the target exec. */
  virtual void ProcessLabel(u32 /*cf_index*/) {}

  virtual void ProcessControlFlowNopInstruction(u32 /*cf_index*/) {}
  virtual void ProcessControlFlowInstructionBegin(u32 /*cf_index*/) {}
  virtual void ProcessControlFlowInstructionEnd(u32 /*cf_index*/) {}
  virtual void ProcessExecInstructionBegin(const ParsedExecInstruction& /*instr*/) {}
  virtual void ProcessExecInstructionEnd(const ParsedExecInstruction& /*instr*/) {}
  virtual void ProcessLoopStartInstruction(const ParsedLoopStartInstruction& /*instr*/) {}
  virtual void ProcessLoopEndInstruction(const ParsedLoopEndInstruction& /*instr*/) {}
  virtual void ProcessCallInstruction(const ParsedCallInstruction& /*instr*/) {}
  virtual void ProcessReturnInstruction(const ParsedReturnInstruction& /*instr*/) {}
  virtual void ProcessJumpInstruction(const ParsedJumpInstruction& /*instr*/) {}
  /**
   * Memory exports for the eM# in export_eM must be performed regardless of the alloc type.
   *
   * @param export_em Bit mask of eM# elements to export.
   */
  virtual void ProcessAllocInstruction(const ParsedAllocInstruction& /*instr*/, u8 /*export_em*/) {}

  virtual void ProcessVertexFetchInstruction(const ParsedVertexFetchInstruction& /*instr*/) {}
  virtual void ProcessTextureFetchInstruction(const ParsedTextureFetchInstruction& /*instr*/) {}
  /**
   * @param memexport_em_potentially_written_before Must be handled by `kill` so exports of eM#
   * written in earlier execs and the current exec are done before the invocation becomes inactive.
   */
  virtual void ProcessALUInstruction(const ParsedALUInstruction& /*instr*/,
                                     u8 /*memexport_em_potentially_written_before*/) {}

 private:
  void TranslateControlFlowInstruction(const xenos::ucode::ControlFlowInstruction& cf);
  void TranslateExecInstructions(const ParsedExecInstruction& instr);

  Shader::Translation* translation_ = nullptr;

  std::vector<Shader::Error> errors_;

  u32 register_count_ = 0;

  u32 cf_index_ = 0;

  xenos::ucode::VertexFetchInstruction previous_vfetch_full_;
};

}  // namespace rex::gpu
