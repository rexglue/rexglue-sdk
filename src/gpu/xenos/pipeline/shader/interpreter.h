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

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include <rex/assert.h>
#include <rex/xenos/ucode.h>
#include "gpu/xenos/pipeline/shader/shader.h"
#include <rex/xenos/register_file.h>
#include <rex/xenos/xenos.h>
#include <rex/runtime/xmemory.h>

namespace rex::gpu {

class ShaderInterpreter {
 public:
  ShaderInterpreter(const xenos::RegisterFile& register_file, const runtime::Memory& memory)
      : register_file_(register_file), memory_(memory) {}

  class ExportSink {
   public:
    virtual ~ExportSink() = default;
    virtual void AllocExport(xenos::ucode::AllocType /*type*/, u32 /*size*/) {}
    virtual void Export(xenos::ucode::ExportRegister /*export_register*/, const f32* /*value*/,
                        u32 /*value_mask*/) {}
  };

  void SetExportSink(ExportSink* new_export_sink) { export_sink_ = new_export_sink; }

  const f32* GetTempRegisters() const { return &temp_registers_[0][0]; }
  f32* GetTempRegisters() { return &temp_registers_[0][0]; }

  /**
   * Reports whether the shader can be interpreted.
   *
   * Texture fetch instructions are not supported.
   *
   * @param shader Analyzed shader to check.
   * @return True when the shader can be interpreted.
   */
  static bool CanInterpretShader(const Shader& shader) {
    REX_ASSERT(shader.IsUcodeAnalyzed());
    if (shader.HasTextureFetchInstructionResults()) {
      return false;
    }
    return true;
  }
  void SetShader(xenos::ShaderType shader_type, const u32* ucode) {
    shader_type_ = shader_type;
    ucode_ = ucode;
  }
  void SetShader(const Shader& shader) {
    REX_ASSERT(CanInterpretShader(shader));
    SetShader(shader.GetType(), shader.GetUcodeDwords());
  }

  void Execute();

 private:
  struct State {
    xenos::ucode::VertexFetchInstruction vfetch_full_last;
    u32 vfetch_address_dwords;
    f32 previous_scalar;
    u32 call_stack_depth;
    u32 call_return_addresses[4];
    u32 loop_stack_depth;
    xenos::LoopConstant loop_constants[4];
    u32 loop_iterators[4];
    i32 address_register;
    bool predicate;

    void Reset() { *this = {}; }

    /**
     * Returns the loop address clamped to the sequencer range of [-256, 256] from the IPR2015-00325
     * specification.
     *
     * @return Clamped loop address.
     */
    i32 GetLoopAddress() const {
      REX_ASSERT(loop_stack_depth && loop_stack_depth < 4);
      if (!loop_stack_depth || loop_stack_depth >= 4) {
        return 0;
      }
      xenos::LoopConstant loop_constant = loop_constants[loop_stack_depth];
      return std::min(INT32_C(256),
                      std::max(INT32_C(-256),
                               static_cast<i32>(static_cast<i32>(loop_iterators[loop_stack_depth]) *
                                                    loop_constant.step +
                                                loop_constant.start)));
    }
  };

  static f32 FlushDenormal(f32 value) {
    u32 bits = std::bit_cast<u32>(value);
    bits &= (bits & UINT32_C(0x7F800000)) ? ~UINT32_C(0) : (UINT32_C(1) << 31);
    return std::bit_cast<f32>(bits);
  }

  u32 GetTempRegisterIndex(u32 address, bool is_relative) const {
    return (static_cast<i32>(address) + (is_relative ? state_.GetLoopAddress() : 0)) &
           ((UINT32_C(1) << xenos::kMaxShaderTempRegistersLog2) - 1);
  }
  const f32* GetTempRegister(u32 address, bool is_relative) const {
    return temp_registers_[GetTempRegisterIndex(address, is_relative)];
  }
  f32* GetTempRegister(u32 address, bool is_relative) {
    return temp_registers_[GetTempRegisterIndex(address, is_relative)];
  }
  const std::array<f32, 4> GetFloatConstant(u32 address, bool is_relative,
                                            bool relative_address_is_a0) const;

  void ExecuteALUInstruction(xenos::ucode::AluInstruction instr);
  void StoreFetchResult(u32 dest, bool is_dest_relative, u32 swizzle, const f32* value);
  void ExecuteVertexFetchInstruction(xenos::ucode::VertexFetchInstruction instr);

  const xenos::RegisterFile& register_file_;
  const runtime::Memory& memory_;

  ExportSink* export_sink_ = nullptr;

  xenos::ShaderType shader_type_ = xenos::ShaderType::kVertex;
  const u32* ucode_ = nullptr;

  f32 temp_registers_[xenos::kMaxShaderTempRegisters][4];

  State state_;
};

}  // namespace rex::gpu
