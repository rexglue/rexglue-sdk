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

#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/utility.h>
#include <rex/xenos/ucode.h>
#include <rex/xenos/register.h>
#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include <rex/xenos/state.h>
#include <rex/bit.h>
#include <rex/string/text.h>
#include <rex/types.h>
#include "gpu/xenos/xenos_export.h"

namespace rex::gpu {

// Shared with disassembly, so parsing keeps nops for XNA round trips.

enum class InstructionStorageTarget : u8 {
  kNone,
  /** Temporary register, storage_index in [0-63]. */
  kRegister,
  /** Vertex shader interpolator export, storage_index in [0-15]. */
  kInterpolator,
  kPosition,
  /** Vertex shader misc export, see ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex. */
  kPointSizeEdgeFlagKillVertex,
  /** Memexport destination address, see xenos::MemExportStream. */
  kExportAddress,
  kExportData,
  /** Color target export, storage_index in [0-3]. */
  kColor,
  /** Only X is stored. */
  kDepth,
};

/**
 * Only for translation, not disassembly, since oPts.x000 assembles but oPts.x00_ cannot be encoded.
 */
constexpr u32 GetInstructionStorageTargetUsedComponentCount(InstructionStorageTarget target) {
  switch (target) {
    case InstructionStorageTarget::kNone:
      return 0;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      return 3;
    case InstructionStorageTarget::kDepth:
      return 1;
    default:
      return 4;
  }
}

enum class InstructionStorageAddressingMode : u8 {
  kAbsolute,
  /** Addressed by a0, float constants only. */
  kAddressRegisterRelative,
  /** Addressed by aL, float constants and temporary registers only. */
  kLoopRelative,
};

enum class SwizzleSource : u8 {
  kX,
  kY,
  kZ,
  kW,
  k0,
  k1,
};

constexpr SwizzleSource GetSwizzleFromComponentIndex(u32 i) {
  return static_cast<SwizzleSource>(i);
}
constexpr SwizzleSource GetSwizzledALUSourceComponent(u32 swizzle, u32 component_index) {
  return GetSwizzleFromComponentIndex(
      xenos::ucode::AluInstruction::GetSwizzledComponentIndex(swizzle, component_index));
}
inline char GetCharForSwizzle(SwizzleSource swizzle_source) {
  const static char kChars[] = {'x', 'y', 'z', 'w', '0', '1'};
  return kChars[static_cast<u32>(swizzle_source)];
}

struct REX_GPU_XENOS_API InstructionResult {
  InstructionStorageTarget storage_target = InstructionStorageTarget::kNone;
  u32 storage_index = 0;
  InstructionStorageAddressingMode storage_addressing_mode =
      InstructionStorageAddressingMode::kAbsolute;
  /** Saturates the result to [0-1]. */
  bool is_clamped = false;
  /** Taken from the original microcode, so components missing in the target are included. */
  u32 original_write_mask = 0b0000;
  SwizzleSource components[4] = {SwizzleSource::kX, SwizzleSource::kY, SwizzleSource::kZ,
                                 SwizzleSource::kW};
  /** Returns the write mask limited to the components present in the target. */
  u32 GetUsedWriteMask() const {
    u32 target_component_count = GetInstructionStorageTargetUsedComponentCount(storage_target);
    return original_write_mask & ((1 << target_component_count) - 1);
  }
  bool IsStandardSwizzle() const {
    return (GetUsedWriteMask() == 0b1111) && components[0] == SwizzleSource::kX &&
           components[1] == SwizzleSource::kY && components[2] == SwizzleSource::kZ &&
           components[3] == SwizzleSource::kW;
  }
  /** Returns the result components, before swizzling, that are not discarded or constant. */
  u32 GetUsedResultComponents() const {
    u32 used_write_mask = GetUsedWriteMask();
    u32 used_components = 0b0000;
    for (u32 i = 0; i < 4; ++i) {
      if ((used_write_mask & (1 << i)) && components[i] >= SwizzleSource::kX &&
          components[i] <= SwizzleSource::kW) {
        used_components |=
            1 << (static_cast<u32>(components[i]) - std::to_underlying(SwizzleSource::kX));
      }
    }
    return used_components;
  }
  /**
   * Returns the constant components of the used write mask, with their values in
   * constant_values_out.
   *
   * @param constant_values_out Receives the values of the constant components.
   */
  u32 GetUsedConstantComponents(u32& constant_values_out) const {
    u32 constant_components = 0;
    u32 constant_values = 0;
    u32 used_write_mask = GetUsedWriteMask();
    for (u32 i = 0; i < 4; ++i) {
      if (!(used_write_mask & (1 << i))) {
        continue;
      }
      SwizzleSource component = components[i];
      if (component >= SwizzleSource::kX && component <= SwizzleSource::kW) {
        continue;
      }
      constant_components |= 1 << i;
      if (component == SwizzleSource::k1) {
        constant_values |= 1 << i;
      }
    }
    constant_values_out = constant_values;
    return constant_components;
  }
};

enum class InstructionStorageSource : u8 {
  /** Temporary register, storage_index in [0-63]. */
  kRegister,
  /** Float constant, storage_index in [0-255]. */
  kConstantFloat,
  /** Vertex fetch constant, storage_index in [0-95]. */
  kVertexFetchConstant,
  /** Texture fetch constant, storage_index in [0-31]. */
  kTextureFetchConstant,
};

struct REX_GPU_XENOS_API InstructionOperand {
  InstructionStorageSource storage_source = InstructionStorageSource::kRegister;
  u32 storage_index = 0;
  InstructionStorageAddressingMode storage_addressing_mode =
      InstructionStorageAddressingMode::kAbsolute;
  bool is_negated = false;
  /** Applied before any negation. */
  bool is_absolute_value = false;
  u32 component_count = 4;
  SwizzleSource components[4] = {SwizzleSource::kX, SwizzleSource::kY, SwizzleSource::kZ,
                                 SwizzleSource::kW};
  /**
   * Returns the swizzle source for the component, replicating the rightmost component when there
   * are fewer than 4, as the console shader compiler does.
   */
  SwizzleSource GetComponent(u32 index) const {
    return components[std::min(index, component_count - 1)];
  }
  bool IsStandardSwizzle() const {
    if (component_count != 4) {
      return false;
    }
    return components[0] == SwizzleSource::kX && components[1] == SwizzleSource::kY &&
           components[2] == SwizzleSource::kZ && components[3] == SwizzleSource::kW;
  }

  /**
   * Returns which components of two operands are always bitwise equal, treating the rightmost
   * component as replicated.
   *
   * Used to emulate the Shader Model 3 multiplication rule +-0 * x = +0 with IEEE multiplication,
   * since -0 * |-0| or -0 * +0 is -0 while the result must be +0.
   */
  u32 GetIdenticalComponents(const InstructionOperand& other) const {
    if (storage_source != other.storage_source || storage_index != other.storage_index ||
        storage_addressing_mode != other.storage_addressing_mode ||
        is_negated != other.is_negated || is_absolute_value != other.is_absolute_value) {
      return 0;
    }
    u32 identical_components = 0;
    for (u32 i = 0; i < 4; ++i) {
      identical_components |= static_cast<u32>(GetComponent(i) == other.GetComponent(i)) << i;
    }
    return identical_components;
  }
};

struct REX_GPU_XENOS_API ParsedExecInstruction {
  u32 dword_index = 0;

  xenos::ucode::ControlFlowOpcode opcode;
  const char* opcode_name = nullptr;

  u32 instruction_address = 0;
  u32 instruction_count = 0;

  enum class Type : u8 {
    kUnconditional,
    kConditional,
    kPredicated,
  };
  Type type = Type::kUnconditional;
  u32 bool_constant_index = 0;
  bool condition = false;

  bool is_end = false;
  /** True when the hardware does not wait for the predicate update after this exec. */
  bool is_predicate_clean = true;
  bool is_yield = false;

  /** 2 bits per instruction indicating ALU or fetch. */
  u32 sequence = 0;

  void Disassemble(string::StringBuffer* out) const;
};

struct REX_GPU_XENOS_API ParsedLoopStartInstruction {
  u32 dword_index = 0;

  /**
   * Integer constant register with the loop parameters.
   *
   * Bits 0 to 7 are the uint8 loop count, 8 to 15 the uint8 start aL and 16 to 23 the int8 aL step.
   */
  u32 loop_constant_index = 0;
  /** Reuses the current aL instead of resetting it to the loop start. */
  bool is_repeat = false;

  u32 loop_skip_address = 0;

  void Disassemble(string::StringBuffer* out) const;
};

struct REX_GPU_XENOS_API ParsedLoopEndInstruction {
  u32 dword_index = 0;

  bool is_predicated_break = false;
  bool predicate_condition = false;

  /**
   * Integer constant register with the loop parameters.
   *
   * Bits 0 to 7 are the uint8 loop count, 8 to 15 the uint8 start aL and 16 to 23 the int8 aL step.
   */
  u32 loop_constant_index = 0;

  u32 loop_body_address = 0;

  void Disassemble(string::StringBuffer* out) const;
};

struct ParsedCallInstruction {
  u32 dword_index = 0;

  u32 target_address = 0;

  enum class Type : u8 {
    kUnconditional,
    kConditional,
    kPredicated,
  };
  Type type = Type::kUnconditional;
  u32 bool_constant_index = 0;
  bool condition = false;

  void Disassemble(string::StringBuffer* out) const;
};

struct ParsedReturnInstruction {
  u32 dword_index = 0;

  void Disassemble(string::StringBuffer* out) const;
};

struct REX_GPU_XENOS_API ParsedJumpInstruction {
  u32 dword_index = 0;

  u32 target_address = 0;

  enum class Type : u8 {
    kUnconditional,
    kConditional,
    kPredicated,
  };
  Type type = Type::kUnconditional;
  u32 bool_constant_index = 0;
  bool condition = false;

  void Disassemble(string::StringBuffer* out) const;
};

struct REX_GPU_XENOS_API ParsedAllocInstruction {
  u32 dword_index = 0;

  xenos::ucode::AllocType type = xenos::ucode::AllocType::kNone;
  int count = 0;

  bool is_vertex_shader = false;

  void Disassemble(string::StringBuffer* out) const;
};

struct REX_GPU_XENOS_API ParsedVertexFetchInstruction {
  xenos::ucode::FetchOpcode opcode;
  const char* opcode_name = nullptr;

  /** Reuses the previous full fetch, whose source and constant data are populated. */
  bool is_mini_fetch = false;

  bool is_predicated = false;
  bool predicate_condition = false;

  /** A vfetch_full with no components to write still needs its address for a vfetch_mini. */
  InstructionResult result;

  size_t operand_count = 0;
  /**
   * A vfetch_mini inherits the operands of its vfetch_full. The index register may be overwritten
   * between them, so the vfetch_full must store a copy of the index. The base address, stride and
   * rounding are the same for every vfetch_mini, so the vfetch_full may apply them to the index.
   */
  InstructionOperand operands[2];

  struct Attributes {
    xenos::VertexFormat data_format = xenos::VertexFormat::kUndefined;
    i32 offset = 0;
    u32 stride = 0; /**< Stride in dwords. */
    i32 exp_adjust = 0;
    /** Prefetch count minus 1. */
    u32 prefetch_count = 0;
    xenos::SignedRepeatingFractionMode signed_rf_mode =
        xenos::SignedRepeatingFractionMode::kZeroClampMinusOne;
    bool is_index_rounded = false;
    bool is_signed = false;
    bool is_integer = false;
  };
  Attributes attributes;

  void Disassemble(string::StringBuffer* out) const;
};

struct REX_GPU_XENOS_API ParsedTextureFetchInstruction {
  xenos::ucode::FetchOpcode opcode;
  const char* opcode_name = nullptr;
  /** Texture dimension for opcodes that have multiple dimension forms. */
  xenos::FetchOpDimension dimension = xenos::FetchOpDimension::k1D;

  bool is_predicated = false;
  bool predicate_condition = false;

  bool HasResult() const { return result.storage_target != InstructionStorageTarget::kNone; }
  InstructionResult result;

  size_t operand_count = 0;
  InstructionOperand operands[2];

  struct Attributes {
    bool fetch_valid_only = true;
    bool unnormalized_coordinates = false;
    xenos::TextureFilter mag_filter = xenos::TextureFilter::kUseFetchConst;
    xenos::TextureFilter min_filter = xenos::TextureFilter::kUseFetchConst;
    xenos::TextureFilter mip_filter = xenos::TextureFilter::kUseFetchConst;
    xenos::AnisoFilter aniso_filter = xenos::AnisoFilter::kUseFetchConst;
    xenos::TextureFilter vol_mag_filter = xenos::TextureFilter::kUseFetchConst;
    xenos::TextureFilter vol_min_filter = xenos::TextureFilter::kUseFetchConst;
    bool use_computed_lod = true;
    bool use_register_lod = false;
    bool use_register_gradients = false;
    f32 lod_bias = 0.0f;
    f32 offset_x = 0.0f;
    f32 offset_y = 0.0f;
    f32 offset_z = 0.0f;
  };
  Attributes attributes;

  /** Returns which components of the result may be non zero, given the operation and filters. */
  u32 GetNonZeroResultComponents() const;

  void Disassemble(string::StringBuffer* out) const;
};

struct REX_GPU_XENOS_API ParsedALUInstruction {
  xenos::ucode::AluVectorOpcode vector_opcode = xenos::ucode::AluVectorOpcode::kAdd;
  xenos::ucode::AluScalarOpcode scalar_opcode = xenos::ucode::AluScalarOpcode::kAdds;
  const char* vector_opcode_name = nullptr;
  const char* scalar_opcode_name = nullptr;

  bool is_predicated = false;
  bool predicate_condition = false;

  /**
   * How the vector operation result and, for exports, the constant 0/1 writes are stored.
   *
   * The constant writes are part of the vector operation so the disassembly can express them even
   * when only constants are exported. The XNA disassembler shows the whole vector operation when
   * the scalar operation is a nop or the vector operation changes a0, p0 or kills pixels, and
   * otherwise puts the constant mask in the scalar destination. The assembler accepts the
   * mask in the vector operation, the scalar operation or both.
   */
  InstructionResult vector_and_constant_result;
  InstructionResult scalar_result;
  // Both paired ops run before any result is stored, so a result cannot feed the other's operand.

  u32 vector_operand_count = 0;
  InstructionOperand vector_operands[3];
  u32 scalar_operand_count = 0;
  InstructionOperand scalar_operands[2];

  /**
   * Reports whether the vector part matches what the assembler produces when it is omitted, so
   * reassembling without it gives the same microcode.
   *
   * For disassembly only. Translators should use the write masks and the changed state bits of the
   * opcode info to skip operations, since this covers one nop format.
   */
  bool IsVectorOpDefaultNop() const;
  /**
   * Reports whether the scalar part matches what the assembler produces when it is omitted, so
   * reassembling without it gives the same microcode.
   */
  bool IsScalarOpDefaultNop() const;

  /** Reports whether the instruction has no effect, for translation and not disassembly. */
  bool IsNop() const;

  /** Returns the stream float constant index if this is a normal eA write, otherwise UINT32_MAX. */
  u32 GetMemExportStreamConstant() const;

  void Disassemble(string::StringBuffer* out) const;
};

void ParseControlFlowExec(const xenos::ucode::ControlFlowExecInstruction& cf, u32 cf_index,
                          ParsedExecInstruction& instr);
void ParseControlFlowCondExec(const xenos::ucode::ControlFlowCondExecInstruction& cf, u32 cf_index,
                              ParsedExecInstruction& instr);
void ParseControlFlowCondExecPred(const xenos::ucode::ControlFlowCondExecPredInstruction& cf,
                                  u32 cf_index, ParsedExecInstruction& instr);
void ParseControlFlowLoopStart(const xenos::ucode::ControlFlowLoopStartInstruction& cf,
                               u32 cf_index, ParsedLoopStartInstruction& instr);
void ParseControlFlowLoopEnd(const xenos::ucode::ControlFlowLoopEndInstruction& cf, u32 cf_index,
                             ParsedLoopEndInstruction& instr);
void ParseControlFlowCondCall(const xenos::ucode::ControlFlowCondCallInstruction& cf, u32 cf_index,
                              ParsedCallInstruction& instr);
void ParseControlFlowReturn(const xenos::ucode::ControlFlowReturnInstruction& cf, u32 cf_index,
                            ParsedReturnInstruction& instr);
void ParseControlFlowCondJmp(const xenos::ucode::ControlFlowCondJmpInstruction& cf, u32 cf_index,
                             ParsedJumpInstruction& instr);
void ParseControlFlowAlloc(const xenos::ucode::ControlFlowAllocInstruction& cf, u32 cf_index,
                           bool is_vertex_shader, ParsedAllocInstruction& instr);

/**
 * @param previous_full_op Previous full fetch inherited by a mini fetch.
 * @param instr Receives the parsed instruction.
 * @return True when the fetch is a full one, so the next mini fetch inherits its parameters.
 */
bool ParseVertexFetchInstruction(const xenos::ucode::VertexFetchInstruction& op,
                                 const xenos::ucode::VertexFetchInstruction& previous_full_op,
                                 ParsedVertexFetchInstruction& instr);
void ParseTextureFetchInstruction(const xenos::ucode::TextureFetchInstruction& op,
                                  ParsedTextureFetchInstruction& instr);
void ParseAluInstruction(const xenos::ucode::AluInstruction& op, xenos::ShaderType shader_type,
                         ParsedALUInstruction& instr);

class REX_GPU_XENOS_API Shader : public rex::NonCopyable {
 public:
  /**
   * Type of the vertex shader on the host.
   *
   * The shader interface depends on it, so it must be known at translation time. Changing the
   * values requires increasing the shader storage version and checking the bit
   * count where it is packed. The underlying type is u32 for packing in bit fields.
   */
  // NOLINTNEXTLINE(performance-enum-size): packed key bit field, MSVC packs per type
  enum class HostVertexShaderType : u32 {
    kVertex,

    kDomainStart,
    kLineDomainCPIndexed = kDomainStart,
    kLineDomainPatchIndexed,
    kTriangleDomainCPIndexed,
    kTriangleDomainPatchIndexed,
    kQuadDomainCPIndexed,
    kQuadDomainPatchIndexed,
    kDomainEnd,

    /** Vertex shader converted to a compute shader that only performs memory export. */
    kMemExportCompute,

    /** Four host vertices per guest vertex. */
    kPointListAsTriangleStrip,
    /** Three guest vertices processed per host shader invocation to choose the orientation. */
    kRectangleListAsTriangleStrip,
  };
  static constexpr u32 kHostVertexShaderTypeBitCount = 4;

  static constexpr bool IsHostVertexShaderTypeDomain(HostVertexShaderType host_vertex_shader_type) {
    return host_vertex_shader_type >= HostVertexShaderType::kDomainStart &&
           host_vertex_shader_type < HostVertexShaderType::kDomainEnd;
  }

  struct Error {
    bool is_fatal = false;
    std::string message;
  };

  struct VertexBinding {
    struct Attribute {
      ParsedVertexFetchInstruction fetch_instr;
    };

    int binding_index;
    /** Fetch constant index in [0-95]. */
    u32 fetch_constant;
    /** Stride of the binding in words. */
    u32 stride_words;
    std::vector<Attribute> attributes;
  };

  struct TextureBinding {
    size_t binding_index;
    /** Fetch constant index in [0-31]. */
    u32 fetch_constant;
    ParsedTextureFetchInstruction fetch_instr;
  };

  struct ConstantRegisterMap {
    /**
     * Each shader reads up to 256 of the 512 float constants, based on the shader type and the
     * SQ_VS/PS_CONST registers. Each bit is a storage index from that base.
     */
    u64 float_bitmap[256 / 64];
    /** Each bit is a storage index in [0-31]. */
    u32 loop_bitmap;
    /** Each bit is a storage index in [0-255]. */
    u32 bool_bitmap[256 / 32];
    /** Each bit is a storage index in [0-95]. */
    u32 vertex_fetch_bitmap[96 / 32];

    u32 float_count;

    /** When set, float_bitmap is all 1 and tight packing must not be done. */
    bool float_dynamic_addressing;

    /**
     * Returns the index of the float4 constant as if all referenced float4 constants were tightly
     * packed in a buffer. It is UINT32_MAX if not found.
     */
    u32 GetPackedFloatConstantIndex(u32 float_constant) const {
      if (float_constant >= 256) {
        return UINT32_MAX;
      }
      if (float_dynamic_addressing) {
        return float_constant;
      }
      u32 block_index = float_constant / 64;
      u32 bit_index = float_constant % 64;
      if (!(float_bitmap[block_index] & (static_cast<u64>(1) << bit_index))) {
        return UINT32_MAX;
      }
      u32 offset = 0;
      for (u32 i = 0; i < block_index; ++i) {
        offset += rex::BitCount(float_bitmap[i]);
      }
      return offset +
             rex::BitCount(float_bitmap[block_index] & ((static_cast<u64>(1) << bit_index) - 1));
    }
  };

  struct ControlFlowMemExportState {
    /**
     * Which eM elements may have been written by earlier instructions since the last `alloc
     * export`, regardless of conditionals, loop iteration counts and predication.
     */
    u8 em_potentially_written_before = 0;
    /**
     * For exec sequences, which eM elements are potentially written by the instructions,
     * regardless of predication. 0 for other control flow instructions.
     */
    u8 em_potentially_written_by_exec = 0;
  };

  class REX_GPU_XENOS_API Translation {
   public:
    Translation(Shader& shader, u64 modification) : shader_(shader), modification_(modification) {}
    virtual ~Translation() {}

    Shader& GetShader() const { return shader_; }

    u64 GetModification() const { return modification_; }

    bool IsValid() const { return is_valid_; }

    bool IsTranslated() const { return is_translated_; }

    const std::vector<Error>& GetErrors() const { return errors_; }

    const std::vector<u8>& GetTranslatedBinary() const { return translated_binary_; }

    /** Sets the host disassembly when it depends on the GPU driver. */
    void SetHostDisassembly(std::string disassembly) { host_disassembly_ = std::move(disassembly); }

    /**
     * Dumps the translated code and, if available, the disassembly to files in the directory named
     * by the ucode hash.
     *
     * @param base_path Output directory.
     * @param path_prefix Prefix of the file names.
     * @return Binary path and disassembly path, the latter empty if not written.
     */
    std::pair<std::filesystem::path, std::filesystem::path> DumpTranslation(
        const std::filesystem::path& base_path, const char* path_prefix) const;

   protected:
    void Invalidate() { is_valid_ = false; }

   private:
    friend class Shader;
    friend class ShaderTranslator;

    Shader& shader_;
    u64 modification_;

    bool is_valid_ = false;
    bool is_translated_ = false;
    std::vector<Error> errors_;
    std::vector<u8> translated_binary_;
    std::string host_disassembly_;
  };

  /**
   * Constructs the shader, storing the ucode in the native byte order.
   *
   * @param ucode_dwords Source ucode dwords.
   * @param ucode_source_endian Endianness of ucode_dwords.
   */
  Shader(xenos::ShaderType shader_type, u64 ucode_data_hash, const u32* ucode_dwords,
         size_t ucode_dword_count, std::endian ucode_source_endian = std::endian::big);
  virtual ~Shader();

  xenos::ShaderType GetType() const { return shader_type_; }

  /** Microcode dwords in host endianness. */
  const std::vector<u32>& GetUcodeData() const { return ucode_dwords_; }
  u64 GetUcodeDataHash() const { return ucode_data_hash_; }
  const u32* GetUcodeDwords() const { return ucode_dwords_.data(); }
  size_t GetUcodeDwordCount() const { return ucode_dwords_.size(); }

  bool IsUcodeAnalyzed() const { return is_ucode_analyzed_; }
  /**
   * Analyzes the microcode and gathers the information below.
   *
   * @param ucode_disasm_buffer Scratch storage for disassembly.
   * @param dump_path Directory the microcode is dumped to, or empty to skip the dump.
   */
  void AnalyzeUcode(string::StringBuffer& ucode_disasm_buffer, const std::string& dump_path);

  // The accessors below are valid only after AnalyzeUcode.

  /** Microcode disassembly in D3D format. */
  const std::string& GetUcodeDisassembly() const { return ucode_disassembly_; }

  const std::vector<TextureBinding>& GetTextureBindings() const { return texture_bindings_; }

  const ConstantRegisterMap& GetConstantRegisterMap() const { return constant_register_map_; }

  /** Memory export state per control flow instruction, empty when there are no eM# writes. */
  const std::vector<ControlFlowMemExportState>& GetControlFlowMemExportState() const {
    return cf_memexport_info_;
  }

  u8 GetMemExportEMWritten() const { return memexport_em_written_; }
  u8 GetMemExportEMPotentiallyWrittenBeforeEnd() const {
    return memexport_em_potentially_written_before_end_;
  }

  /** The c# registers used as the addend in MAD operations to eA. */
  const std::set<u32>& GetMemExportStreamConstants() const { return memexport_stream_constants_; }

  const std::set<u32>& GetLabelAddresses() const { return label_addresses_; }

  /** Returns the exclusive upper bound of the indexes of paired control flow instructions. */
  u32 GetCFPairIndexBound() const { return cf_pair_index_bound_; }

  /**
   * Returns the upper bound of statically addressed temporary registers, the highest address + 1 or
   * 0 if none. SQ_PROGRAM_CNTL is not always reliable, so it should raise this count only if
   * uses_register_dynamic_addressing is true.
   */
  u32 GetRegisterStaticAddressBound() const { return register_static_address_bound_; }

  /** Reports whether temporary registers are addressed dynamically, so SQ_PROGRAM_CNTL matters. */
  bool HasRegisterDynamicAddressing() const { return uses_register_dynamic_addressing_; }

  /**
   * Returns the number of temporary registers to allocate, for building and normalizing shader
   * modification bits. 0 without dynamic register addressing, where the translator uses
   * register_static_address_bound as is.
   *
   * @param program_cntl_num_reg Register count field of SQ_PROGRAM_CNTL.
   */
  u32 GetDynamicAddressableRegisterCount(u32 program_cntl_num_reg) const {
    if (!HasRegisterDynamicAddressing()) {
      return 0;
    }
    return std::max(program_cntl_num_reg + static_cast<u32>(1), GetRegisterStaticAddressBound());
  }

  bool DoesKillPixels() const { return kills_pixels_; }

  /** Reports whether any fetch other than vertex fetch writes non constant components. */
  bool HasTextureFetchInstructionResults() const { return uses_texture_fetch_instruction_results_; }

  /** Mask of interpolators written on any execution path. */
  u32 GetWritesInterpolators() const { return writes_interpolators_; }

  /** Mask of system vertex shader exports written on any execution path. */
  u32 GetWritesPointSizeEdgeFlagKillVertex() const {
    return writes_point_size_edge_flag_kill_vertex_;
  }

  /**
   * Returns the mask of interpolators the pixel shader may require from the vertex shader.
   *
   * @param param_gen_pos_out Receives the PsParamGen register, or UINT32_MAX when unused.
   */
  u32 GetInterpolatorInputMask(xenos::reg::SQ_PROGRAM_CNTL sq_program_cntl,
                               xenos::reg::SQ_CONTEXT_MISC sq_context_misc,
                               u32& param_gen_pos_out) const;

  bool IsDepthWritten() const { return writes_depth_; }

  /** Reports whether early depth and stencil writes are allowed absent alpha test and A2C. */
  bool IsImplicitEarlyZWriteAllowed() const {
    return !DoesKillPixels() && !IsDepthWritten() && !GetMemExportEMWritten();
  }

  /** Mask of color render targets written on any execution path. */
  u32 GetWritesColorTargets() const { return writes_color_targets_; }
  bool IsColorTargetWritten(u32 i) const {
    return (GetWritesColorTargets() & (static_cast<u32>(1) << i)) != 0;
  }

  /** Host translations by modification bits. Not thread safe against creation and destruction. */
  const std::unordered_map<u64, std::unique_ptr<Translation>>& GetTranslations() const {
    return translations_;
  }
  Translation* GetTranslation(u64 modification) const {
    auto it = translations_.find(modification);
    if (it != translations_.cend()) {
      return it->second.get();
    }
    return nullptr;
  }
  Translation* GetOrCreateTranslation(u64 modification, bool* is_new = nullptr);
  /** Removes a modification after a translation failure while loading shader storage. */
  void DestroyTranslation(u64 modification);

  /**
   * Returns the externally managed identifier of the shader storage the microcode was last written
   * to or loaded from, so it is written once, or UINT32_MAX by default.
   */
  u32 GetUcodeStorageIndex() const { return ucode_storage_index_; }
  void SetUcodeStorageIndex(u32 storage_index) { ucode_storage_index_ = storage_index; }

  /**
   * Dumps the microcode binary and, if analyzed, the disassembly to files in the directory named by
   * the ucode hash. Needs no translation.
   *
   * @return Binary path and disassembly path, the latter empty if not written.
   */
  std::pair<std::filesystem::path, std::filesystem::path> DumpUcode(
      const std::filesystem::path& base_path) const;

 protected:
  friend class ShaderTranslator;

  virtual std::unique_ptr<Translation> CreateTranslationInstance(u64 modification);

  xenos::ShaderType shader_type_;
  std::vector<u32> ucode_dwords_;
  u64 ucode_data_hash_;

  bool is_ucode_analyzed_ = false;

  std::string ucode_disassembly_;
  std::vector<VertexBinding> vertex_bindings_;
  std::vector<TextureBinding> texture_bindings_;
  ConstantRegisterMap constant_register_map_ = {};
  std::set<u32> label_addresses_;
  u32 cf_pair_index_bound_ = 0;
  u32 register_static_address_bound_ = 0;
  u32 writes_interpolators_ = 0;
  u32 writes_point_size_edge_flag_kill_vertex_ = 0;
  u32 writes_color_targets_ = 0b0000;
  bool uses_register_dynamic_addressing_ = false;
  bool kills_pixels_ = false;
  bool uses_texture_fetch_instruction_results_ = false;
  bool writes_depth_ = false;

  /** Per control flow instruction memory export eM write info, if the shader has any eM writes. */
  std::vector<ControlFlowMemExportState> cf_memexport_info_;
  /** Which memexport elements (eM#) are written by any memexport in the shader. */
  u8 memexport_em_written_ = 0;
  /**
   * Equivalent of ControlFlowMemExportState::em_potentially_written_before for the end of the
   * shader, covering the last memory export or exports if the end has multiple predecessor chains.
   */
  u8 memexport_em_potentially_written_before_end_ = 0;
  std::set<u32> memexport_stream_constants_;

  std::unordered_map<u64, std::unique_ptr<Translation>> translations_;

  u32 ucode_storage_index_ = UINT32_MAX;

 private:
  void GatherExecInformation(const ParsedExecInstruction& instr,
                             xenos::ucode::VertexFetchInstruction& previous_vfetch_full,
                             u32& unique_texture_bindings,
                             string::StringBuffer& ucode_disasm_buffer);
  void GatherVertexFetchInformation(const xenos::ucode::VertexFetchInstruction& op,
                                    xenos::ucode::VertexFetchInstruction& previous_vfetch_full,
                                    string::StringBuffer& ucode_disasm_buffer);
  void GatherTextureFetchInformation(const xenos::ucode::TextureFetchInstruction& op,
                                     u32& unique_texture_bindings,
                                     string::StringBuffer& ucode_disasm_buffer);
  void GatherALUInstructionInformation(const xenos::ucode::AluInstruction& op, u32 exec_cf_index,
                                       string::StringBuffer& ucode_disasm_buffer);
  void GatherOperandInformation(const InstructionOperand& operand);
  void GatherFetchResultInformation(const InstructionResult& result);
  void GatherAluResultInformation(const InstructionResult& result, u32 exec_cf_index);
};

}  // namespace rex::gpu
