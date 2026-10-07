#pragma once
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
#include <rex/memory.h>

namespace rex::gpu {

class ShaderInterpreter {
 public:
  ShaderInterpreter(const xenos::RegisterFile& register_file, const runtime::Memory& memory)
      : register_file_(register_file), memory_(memory) {}

  class ExportSink {
   public:
    virtual ~ExportSink() = default;
    virtual void AllocExport(xenos::ucode::AllocType /*type*/, uint32_t /*size*/) {}
    virtual void Export(xenos::ucode::ExportRegister /*export_register*/, const float* /*value*/,
                        uint32_t /*value_mask*/) {}
  };

  void SetExportSink(ExportSink* new_export_sink) { export_sink_ = new_export_sink; }

  const float* temp_registers() const { return &temp_registers_[0][0]; }
  float* temp_registers() { return &temp_registers_[0][0]; }

  static bool CanInterpretShader(const Shader& shader) {
    REX_ASSERT(shader.is_ucode_analyzed());
    // Texture instructions are not common in vertex shaders (and not used
    // in Direct3D 9's internal rectangles such as clears) and are extremely
    // complex, not implemented.
    if (shader.uses_texture_fetch_instruction_results()) {
      return false;
    }
    return true;
  }
  void SetShader(xenos::ShaderType shader_type, const uint32_t* ucode) {
    shader_type_ = shader_type;
    ucode_ = ucode;
  }
  void SetShader(const Shader& shader) {
    REX_ASSERT(CanInterpretShader(shader));
    SetShader(shader.type(), shader.ucode_dwords());
  }

  void Execute();

 private:
  struct State {
    xenos::ucode::VertexFetchInstruction vfetch_full_last;
    uint32_t vfetch_address_dwords;
    float previous_scalar;
    uint32_t call_stack_depth;
    uint32_t call_return_addresses[4];
    uint32_t loop_stack_depth;
    xenos::LoopConstant loop_constants[4];
    uint32_t loop_iterators[4];
    int32_t address_register;
    bool predicate;

    void Reset() { *this = {}; }

    int32_t GetLoopAddress() const {
      REX_ASSERT(loop_stack_depth && loop_stack_depth < 4);
      if (!loop_stack_depth || loop_stack_depth >= 4) {
        return 0;
      }
      xenos::LoopConstant loop_constant = loop_constants[loop_stack_depth];
      // Clamp to the real range specified in the IPR2015-00325 sequencer
      // specification.
      // https://portal.unifiedpatents.com/ptab/case/IPR2015-00325
      return std::min(
          INT32_C(256),
          std::max(INT32_C(-256),
                   static_cast<int32_t>(static_cast<int32_t>(loop_iterators[loop_stack_depth]) *
                                            loop_constant.step +
                                        loop_constant.start)));
    }
  };

  static float FlushDenormal(float value) {
    uint32_t bits = std::bit_cast<uint32_t>(value);
    bits &= (bits & UINT32_C(0x7F800000)) ? ~UINT32_C(0) : (UINT32_C(1) << 31);
    return std::bit_cast<float>(bits);
  }

  uint32_t GetTempRegisterIndex(uint32_t address, bool is_relative) const {
    return (static_cast<int32_t>(address) + (is_relative ? state_.GetLoopAddress() : 0)) &
           ((UINT32_C(1) << xenos::kMaxShaderTempRegistersLog2) - 1);
  }
  const float* GetTempRegister(uint32_t address, bool is_relative) const {
    return temp_registers_[GetTempRegisterIndex(address, is_relative)];
  }
  float* GetTempRegister(uint32_t address, bool is_relative) {
    return temp_registers_[GetTempRegisterIndex(address, is_relative)];
  }
  const std::array<float, 4> GetFloatConstant(uint32_t address, bool is_relative,
                                              bool relative_address_is_a0) const;

  void ExecuteAluInstruction(xenos::ucode::AluInstruction instr);
  void StoreFetchResult(uint32_t dest, bool is_dest_relative, uint32_t swizzle, const float* value);
  void ExecuteVertexFetchInstruction(xenos::ucode::VertexFetchInstruction instr);

  const xenos::RegisterFile& register_file_;
  const runtime::Memory& memory_;

  ExportSink* export_sink_ = nullptr;

  xenos::ShaderType shader_type_ = xenos::ShaderType::kVertex;
  const uint32_t* ucode_ = nullptr;

  // For both inputs and locals.
  float temp_registers_[xenos::kMaxShaderTempRegisters][4];

  State state_;
};

}  // namespace rex::gpu
