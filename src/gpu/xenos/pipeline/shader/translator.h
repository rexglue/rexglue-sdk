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

#include <rex/xenos/ucode.h>
#include "gpu/xenos/pipeline/shader/shader.h"
#include <rex/xenos/registers.h>
#include <rex/xenos/xenos.h>
#include <rex/math.h>
#include <rex/string/buffer.h>

namespace rex::gpu {

class ShaderTranslator {
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
   * @return Whether translation succeeded.
   */
  bool TranslateAnalyzedShader(Shader::Translation& translation);

 protected:
  ShaderTranslator();

  /** Resets translator state before beginning translation. */
  virtual void Reset();

  /** Shader and modification being translated. */
  Shader::Translation& current_translation() const { return *translation_; }
  Shader& current_shader() const { return current_translation().shader(); }

  /** Register count from SQ_PROGRAM_CNTL, stored by the implementation in its modification bits. */
  virtual u32 GetModificationRegisterCount() const { return xenos::kMaxShaderTempRegisters; }

  bool is_vertex_shader() const { return current_shader().type() == xenos::ShaderType::kVertex; }
  bool is_pixel_shader() const { return current_shader().type() == xenos::ShaderType::kPixel; }

  /** Temporary register count, accessible via static and dynamic addressing. */
  u32 register_count() const { return register_count_; }

  /**
   * Emits a translation error that is passed back in the result.
   *
   * @param message Error text.
   * @param is_fatal Whether the error fails the translation.
   */
  virtual void EmitTranslationError(const char* message, bool is_fatal = true);

  /** Called at the start of translation after the vertex and texture bindings are gathered. */
  virtual void StartTranslation() {}

  /**
   * Called at the end of translation after all ucode is processed.
   *
   * @return The translated shader binary.
   */
  virtual std::vector<u8> CompleteTranslation() { return std::vector<u8>(); }

  virtual void PostTranslation() {}
  void set_host_disassembly(Shader::Translation& translation, std::string value) {
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
  /** Memory exports for the eM# in export_eM must be performed regardless of the alloc type. */
  virtual void ProcessAllocInstruction(const ParsedAllocInstruction& /*instr*/, u8 /*export_eM*/) {}

  virtual void ProcessVertexFetchInstruction(const ParsedVertexFetchInstruction& /*instr*/) {}
  virtual void ProcessTextureFetchInstruction(const ParsedTextureFetchInstruction& /*instr*/) {}
  /** memexport_eM_potentially_written_before must be handled by `kill` so exports of eM# written in
   * earlier execs and the current exec are done before the invocation becomes inactive. */
  virtual void ProcessAluInstruction(const ParsedAluInstruction& /*instr*/,
                                     u8 /*memexport_eM_potentially_written_before*/) {}

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
