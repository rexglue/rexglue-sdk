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

#include <rex/xenos/ucode.h>
#include <rex/xenos/registers.h>
#include <rex/xenos/xenos.h>
#include <rex/math.h>
#include <rex/string/buffer.h>
#include <rex/types.h>

namespace rex::gpu {

// Shared with disassembly, so parsing may generalize but not skip nops, for XNA round trips.

enum class InstructionStorageTarget : u8 {
  kNone,
  /** Temporary register, storage_index in [0-63]. */
  kRegister,
  /** Vertex shader interpolator export, storage_index in [0-15]. */
  kInterpolator,
  kPosition,
  /** Vertex shader misc export, components described at
     ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex. */
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
 * Returns the number of components of the target that are used.
 *
 * Must be used only in translation to skip unused components, not in disassembly, since oPts.x000
 * assembles but oPts.x00_, with skipped components and zeros, cannot be encoded.
 *
 * @param target Storage target.
 * @return Number of used components.
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
constexpr SwizzleSource GetSwizzledAluSourceComponent(u32 swizzle, u32 component_index) {
  return GetSwizzleFromComponentIndex(
      xenos::ucode::AluInstruction::GetSwizzledComponentIndex(swizzle, component_index));
}
inline char GetCharForSwizzle(SwizzleSource swizzle_source) {
  const static char kChars[] = {'x', 'y', 'z', 'w', '0', '1'};
  return kChars[static_cast<u32>(swizzle_source)];
}

struct InstructionResult {
  InstructionStorageTarget storage_target = InstructionStorageTarget::kNone;
  u32 storage_index = 0;
  InstructionStorageAddressingMode storage_addressing_mode =
      InstructionStorageAddressingMode::kAbsolute;
  /** Clamps the result to [0-1]. */
  bool is_clamped = false;
  /** Taken from the original microcode, so components missing in the target are included. */
  u32 original_write_mask = 0b0000;
  SwizzleSource components[4] = {SwizzleSource::kX, SwizzleSource::kY, SwizzleSource::kZ,
                                 SwizzleSource::kW};
  /**
   * Returns the write mask limited to components present in the target.
   *
   * @return Write mask limited to the target components.
   */
  u32 GetUsedWriteMask() const {
    u32 target_component_count = GetInstructionStorageTargetUsedComponentCount(storage_target);
    return original_write_mask & ((1 << target_component_count) - 1);
  }
  /**
   * Reports whether the components are in xyzw order.
   *
   * @return True when the components are in xyzw order.
   */
  bool IsStandardSwizzle() const {
    return (GetUsedWriteMask() == 0b1111) && components[0] == SwizzleSource::kX &&
           components[1] == SwizzleSource::kY && components[2] == SwizzleSource::kZ &&
           components[3] == SwizzleSource::kW;
  }
  /**
   * Returns the result components, before swizzling, that are not discarded or replaced with a
   * constant.
   *
   * @return Mask of the used result components.
   */
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
   * Returns which components of the used write mask are constant.
   *
   * @param constant_values_out Receives the values of the constant components.
   * @return Mask of constant components.
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

struct InstructionOperand {
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
   * are fewer than 4, as the console shader compiler does for unspecified components.
   *
   * @param index Component index.
   * @return Swizzle source of the component.
   */
  SwizzleSource GetComponent(u32 index) const {
    return components[std::min(index, component_count - 1)];
  }
  /**
   * Reports whether the components are in xyzw order.
   *
   * @return True when there are 4 components in xyzw order.
   */
  bool IsStandardSwizzle() const {
    if (component_count != 4) {
      return false;
    }
    return components[0] == SwizzleSource::kX && components[1] == SwizzleSource::kY &&
           components[2] == SwizzleSource::kZ && components[3] == SwizzleSource::kW;
  }

  /**
   * Returns which components of two operands are always bitwise equal, ignoring component_count by
   * treating the rightmost component as replicated.
   *
   * Must be used with all its conditions when emulating the Shader Model 3 +-0 * x = +0
   * multiplication with IEEE multiplication, since -0 * |-0| or -0 * +0 is -0 while the result must
   * be +0.
   *
   * @param other Operand to compare with.
   * @return Mask of the components that are identical.
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

struct ParsedExecInstruction {
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

  void Disassemble(StringBuffer* out) const;
};

struct ParsedLoopStartInstruction {
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

  void Disassemble(StringBuffer* out) const;
};

struct ParsedLoopEndInstruction {
  u32 dword_index = 0;

  /** Breaks from the loop if the predicate matches the expected value. */
  bool is_predicated_break = false;
  bool predicate_condition = false;

  /**
   * Integer constant register with the loop parameters.
   *
   * Bits 0 to 7 are the uint8 loop count, 8 to 15 the uint8 start aL and 16 to 23 the int8 aL step.
   */
  u32 loop_constant_index = 0;

  u32 loop_body_address = 0;

  void Disassemble(StringBuffer* out) const;
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

  void Disassemble(StringBuffer* out) const;
};

struct ParsedReturnInstruction {
  u32 dword_index = 0;

  void Disassemble(StringBuffer* out) const;
};

struct ParsedJumpInstruction {
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

  void Disassemble(StringBuffer* out) const;
};

struct ParsedAllocInstruction {
  u32 dword_index = 0;

  xenos::ucode::AllocType type = xenos::ucode::AllocType::kNone;
  int count = 0;

  bool is_vertex_shader = false;

  void Disassemble(StringBuffer* out) const;
};

struct ParsedVertexFetchInstruction {
  xenos::ucode::FetchOpcode opcode;
  const char* opcode_name = nullptr;

  /** Reuses the previous full fetch, whose source and constant data are populated. */
  bool is_mini_fetch = false;

  bool is_predicated = false;
  bool predicate_condition = false;

  /**
   * Describes how the result is stored.
   *
   * A vfetch_full with no components to write the fetched value to needs its address calculation,
   * since it may set up addressing for a vfetch_mini.
   */
  InstructionResult result;

  size_t operand_count = 0;
  /**
   * Describes each source operand.
   *
   * A vfetch_mini inherits the operands of its vfetch_full. The index register may be overwritten
   * between them without changing the fetch index, so the vfetch_full must store a copy of the
   * index. The base address, stride and rounding may be pre applied to it since they are the same
   * for the vfetch_full and all its vfetch_mini instructions.
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

  void Disassemble(StringBuffer* out) const;
};

struct ParsedTextureFetchInstruction {
  xenos::ucode::FetchOpcode opcode;
  const char* opcode_name = nullptr;
  /** Texture dimension for opcodes that have multiple dimension forms. */
  xenos::FetchOpDimension dimension = xenos::FetchOpDimension::k1D;

  bool is_predicated = false;
  bool predicate_condition = false;

  bool has_result() const { return result.storage_target != InstructionStorageTarget::kNone; }
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

  /**
   * Returns which components of the result may have a value other than zero, considering the
   * operation, dimensions, filter overrides and result components.
   *
   * @return Mask of the possibly non zero result components.
   */
  u32 GetNonZeroResultComponents() const;

  void Disassemble(StringBuffer* out) const;
};

struct ParsedAluInstruction {
  xenos::ucode::AluVectorOpcode vector_opcode = xenos::ucode::AluVectorOpcode::kAdd;
  xenos::ucode::AluScalarOpcode scalar_opcode = xenos::ucode::AluScalarOpcode::kAdds;
  const char* vector_opcode_name = nullptr;
  const char* scalar_opcode_name = nullptr;

  bool is_predicated = false;
  bool predicate_condition = false;

  /**
   * Describes how the vector operation result and, for exports, the constant 0/1 writes are stored.
   *
   * Constant 0/1 writes are treated as part of the vector operation, since they must be expressed
   * in the disassembly with a disassembled instruction even when only constants are exported. The
   * XNA disassembler displays the whole vector operation when the scalar operation is a nop or the
   * vector operation changes a0, p0 or kills pixels, and otherwise puts the entire constant mask in
   * the scalar destination. It normally outputs the mask in both operations, but the assembler
   * accepts it in the vector operation, the scalar operation or both.
   */
  InstructionResult vector_and_constant_result;
  InstructionResult scalar_result;
  // Both paired ops run before any result is stored, so a result cannot feed the other's operand.

  u32 vector_operand_count = 0;
  InstructionOperand vector_operands[3];
  u32 scalar_operand_count = 0;
  InstructionOperand scalar_operands[2];

  /**
   * Reports whether the vector part is the same as if omitted in the assembly (compiled or
   * assembled with the Xenos shader compiler), so reassembling without it gives the same microcode,
   * since instructions with only an empty write mask may differ in other fields.
   *
   * For disassembly only. Translators should use the write masks and the changed state bits of the
   * opcode info to skip operations, since this covers 1 nop format.
   *
   * @return True when the vector part matches the omitted form.
   */
  bool IsVectorOpDefaultNop() const;
  /**
   * Reports whether the scalar part is the same as if omitted in the assembly (compiled or
   * assembled with the Xenos shader compiler), so reassembling without it gives the same microcode,
   * since instructions with only an empty write mask may differ in other fields.
   *
   * @return True when the scalar part matches the omitted form.
   */
  bool IsScalarOpDefaultNop() const;

  /**
   * Reports whether the instruction has no effect, for translation and not disassembly.
   *
   * @return True when the instruction has no effect.
   */
  bool IsNop() const;

  /**
   * Returns the index of the stream float constant if this is a normal eA write (MAD with a stream
   * constant), otherwise UINT32_MAX.
   *
   * @return Stream float constant index, or UINT32_MAX.
   */
  u32 GetMemExportStreamConstant() const;

  void Disassemble(StringBuffer* out) const;
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
 * Parses a vertex fetch instruction.
 *
 * @param op Instruction to parse.
 * @param previous_full_op Previous full fetch inherited by a mini fetch.
 * @param instr Receives the parsed instruction.
 * @return Whether the fetch is a full one, so the next parsed mini fetch inherits most of its
 * parameters.
 */
bool ParseVertexFetchInstruction(const xenos::ucode::VertexFetchInstruction& op,
                                 const xenos::ucode::VertexFetchInstruction& previous_full_op,
                                 ParsedVertexFetchInstruction& instr);
void ParseTextureFetchInstruction(const xenos::ucode::TextureFetchInstruction& op,
                                  ParsedTextureFetchInstruction& instr);
void ParseAluInstruction(const xenos::ucode::AluInstruction& op, xenos::ShaderType shader_type,
                         ParsedAluInstruction& instr);

class Shader {
 public:
  /**
   * Type of the vertex shader on the host.
   *
   * The shader interface depends on it, so it must be known at translation time. Changing the
   * values requires increasing the version of the shader storages that hold it and checking the bit
   * count where it is packed. The underlying type is uint32_t for packing in bit fields.
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

    /** Vertex shader converted to a compute shader that only performs memory export, for
       implementations without unconditional vertex shader memory writes. */
    kMemExportCompute,

    /** 4 host vertices per guest vertex, for implementations without unconditional geometry shader
       support. */
    kPointListAsTriangleStrip,
    /** 3 guest vertices processed per host shader invocation to choose the strip orientation, for
       implementations without unconditional geometry shader support. */
    kRectangleListAsTriangleStrip,
  };
  /** Bit count for packing HostVertexShaderType in bit fields. */
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
    /** Stride of the entire binding in words. */
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
    /** Each shader reads up to 256 of the 512 float constants, based on the shader type and the
     * SQ_VS/PS_CONST registers. Each bit is a storage index from that base. */
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
     * Returns the index of the float4 constant as if all referenced float4 constant registers were
     * tightly packed in a buffer.
     *
     * @param float_constant Float constant index.
     * @return Packed index, or UINT32_MAX if not found.
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
        offset += rex::bit_count(float_bitmap[i]);
      }
      return offset +
             rex::bit_count(float_bitmap[block_index] & ((static_cast<u64>(1) << bit_index) - 1));
    }
  };

  struct ControlFlowMemExportInfo {
    /** Which eM elements are potentially written, regardless of conditionals, loop iteration counts
     * and predication, earlier in the predecessor graph of the instruction since an `alloc export`.
     */
    u8 eM_potentially_written_before = 0;
    /** For exec sequences, which eM elements are potentially written by the instructions,
     * regardless of predication. 0 for other control flow instructions. */
    u8 eM_potentially_written_by_exec = 0;
  };

  class Translation {
   public:
    Translation(Shader& shader, u64 modification) : shader_(shader), modification_(modification) {}
    virtual ~Translation() {}

    Shader& shader() const { return shader_; }

    u64 modification() const { return modification_; }

    bool is_valid() const { return is_valid_; }

    bool is_translated() const { return is_translated_; }

    const std::vector<Error>& errors() const { return errors_; }

    const std::vector<u8>& translated_binary() const { return translated_binary_; }

    /**
     * Sets the host disassembly when it depends on the GPU backend.
     *
     * @param disassembly Host disassembly text.
     */
    void set_host_disassembly(std::string disassembly) {
      host_disassembly_ = std::move(disassembly);
    }

    /**
     * Dumps the translated code and, if available, the translated disassembly to files in the given
     * directory named by the ucode hash.
     *
     * @param base_path Output directory.
     * @param path_prefix Prefix of the file names.
     * @return Binary path and disassembly path, the latter empty if not written.
     */
    std::pair<std::filesystem::path, std::filesystem::path> Dump(
        const std::filesystem::path& base_path, const char* path_prefix) const;

   protected:
    void MakeInvalid() { is_valid_ = false; }

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
   * Constructs the shader. The ucode is stored in the native byte order.
   *
   * @param shader_type Shader type.
   * @param ucode_data_hash Hash of the ucode.
   * @param ucode_dwords Source ucode dwords.
   * @param ucode_dword_count Number of ucode dwords.
   * @param ucode_source_endian Endianness of ucode_dwords.
   */
  Shader(xenos::ShaderType shader_type, u64 ucode_data_hash, const u32* ucode_dwords,
         size_t ucode_dword_count, std::endian ucode_source_endian = std::endian::big);
  virtual ~Shader();

  xenos::ShaderType type() const { return shader_type_; }

  /** Microcode dwords in host endianness. */
  const std::vector<u32>& ucode_data() const { return ucode_data_; }
  u64 ucode_data_hash() const { return ucode_data_hash_; }
  const u32* ucode_dwords() const { return ucode_data_.data(); }
  size_t ucode_dword_count() const { return ucode_data_.size(); }

  bool is_ucode_analyzed() const { return is_ucode_analyzed_; }
  /**
   * Analyzes the microcode and gathers the information below.
   *
   * @param ucode_disasm_buffer Scratch storage for disassembly, provided externally to avoid
   * reallocating it for every shader.
   */
  void AnalyzeUcode(StringBuffer& ucode_disasm_buffer);

  // The accessors below are valid only after AnalyzeUcode.

  /** Microcode disassembly in D3D format. */
  const std::string& ucode_disassembly() const { return ucode_disassembly_; }

  const std::vector<TextureBinding>& texture_bindings() const { return texture_bindings_; }

  const ConstantRegisterMap& constant_register_map() const { return constant_register_map_; }

  /**
   * Memory export state at each control flow instruction, empty when there are no eM# writes.
   *
   * @return Memory export state per control flow instruction.
   */
  const std::vector<ControlFlowMemExportInfo>& cf_memexport_info() const {
    return cf_memexport_info_;
  }

  u8 memexport_eM_written() const { return memexport_eM_written_; }
  u8 memexport_eM_potentially_written_before_end() const {
    return memexport_eM_potentially_written_before_end_;
  }

  /**
   * The c# registers used as the addend in MAD operations to eA.
   *
   * @return Set of c# register indices.
   */
  const std::set<u32>& memexport_stream_constants() const { return memexport_stream_constants_; }

  const std::set<u32>& label_addresses() const { return label_addresses_; }

  /** Exclusive upper bound of the indexes of paired control flow instructions, each corresponding
   * to 3 dwords. */
  u32 cf_pair_index_bound() const { return cf_pair_index_bound_; }

  /**
   * Returns the upper bound of temporary registers addressed statically, the highest static
   * register address + 1 or 0 if none.
   *
   * SQ_PROGRAM_CNTL is not always reliable. Some draws that cover no pixels, such as single point
   * draws with oPos = 0001, use an invalid zero SQ_PROGRAM_CNTL with an outdated pixel shader
   * loaded, so it may hold less than the pixel shader needs. SQ_PROGRAM_CNTL should be used to go
   * above this count only if uses_register_dynamic_addressing is true.
   */
  u32 register_static_address_bound() const { return register_static_address_bound_; }

  /** Reports whether temporary registers are addressed dynamically, so SQ_PROGRAM_CNTL determines
   * the register count in addition to register_static_address_bound. */
  bool uses_register_dynamic_addressing() const { return uses_register_dynamic_addressing_; }

  /**
   * Returns the number of temporary registers to allocate, for building and normalizing shader
   * modification bits.
   *
   * Without dynamic register addressing the translator uses register_static_address_bound as is and
   * this returns 0.
   *
   * @param program_cntl_num_reg Register count field of SQ_PROGRAM_CNTL.
   * @return Number of dynamically addressable registers.
   */
  u32 GetDynamicAddressableRegisterCount(u32 program_cntl_num_reg) const {
    if (!uses_register_dynamic_addressing()) {
      return 0;
    }
    return std::max(program_cntl_num_reg + static_cast<u32>(1), register_static_address_bound());
  }

  bool kills_pixels() const { return kills_pixels_; }

  /**
   * Reports whether any fetch instruction other than vertex fetch writes non constant components.
   *
   * @return True when such a fetch instruction exists.
   */
  bool uses_texture_fetch_instruction_results() const {
    return uses_texture_fetch_instruction_results_;
  }

  /** Mask of interpolators written on any execution path. */
  u32 writes_interpolators() const { return writes_interpolators_; }

  /**
   * Mask of system vertex shader exports written on any execution path.
   *
   * @return Mask of written system exports.
   */
  u32 writes_point_size_edge_flag_kill_vertex() const {
    return writes_point_size_edge_flag_kill_vertex_;
  }

  /**
   * Returns the mask of interpolators the pixel shader potentially requires from the vertex shader.
   *
   * @param sq_program_cntl SQ_PROGRAM_CNTL value.
   * @param sq_context_misc SQ_CONTEXT_MISC value.
   * @param param_gen_pos_out Receives the PsParamGen register, or UINT32_MAX when unused.
   * @return Mask of the required interpolators.
   */
  u32 GetInterpolatorInputMask(xenos::reg::SQ_PROGRAM_CNTL sq_program_cntl,
                               xenos::reg::SQ_CONTEXT_MISC sq_context_misc,
                               u32& param_gen_pos_out) const;

  bool writes_depth() const { return writes_depth_; }

  /**
   * Reports whether the shader can have early depth and stencil writing enabled, unless alpha test
   * or alpha to coverage is enabled.
   *
   * @return True when early depth and stencil writing is allowed.
   */
  bool implicit_early_z_write_allowed() const {
    return !kills_pixels() && !writes_depth() && !memexport_eM_written();
  }

  /** Mask of color render targets written on any execution path. */
  u32 writes_color_targets() const { return writes_color_targets_; }
  bool writes_color_target(u32 i) const {
    return (writes_color_targets() & (static_cast<u32>(1) << i)) != 0;
  }

  /**
   * Host translations by modification bits. Not thread safe with respect to translation creation
   * and destruction.
   *
   * @return Translations keyed by modification bits.
   */
  const std::unordered_map<u64, std::unique_ptr<Translation>>& translations() const {
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
  /**
   * Removes a modification after a translation failure during shader storage loading. Not thread
   * safe.
   *
   * @param modification Modification bits of the translation to remove.
   */
  void DestroyTranslation(u64 modification);

  /** Externally managed identifier of the shader storage the microcode was last written to or
   * loaded from, so the microcode is written to the storage once. UINT32_MAX by default. */
  u32 ucode_storage_index() const { return ucode_storage_index_; }
  void set_ucode_storage_index(u32 storage_index) { ucode_storage_index_ = storage_index; }

  /**
   * Dumps the microcode binary and, if analyzed, the disassembly to files in the given directory
   * named by the ucode hash. Can be called at any time and does not require translation.
   *
   * @param base_path Output directory.
   * @return Binary path and disassembly path, the latter empty if not written.
   */
  std::pair<std::filesystem::path, std::filesystem::path> DumpUcode(
      const std::filesystem::path& base_path) const;

 protected:
  friend class ShaderTranslator;

  virtual std::unique_ptr<Translation> CreateTranslationInstance(u64 modification);

  xenos::ShaderType shader_type_;
  std::vector<u32> ucode_data_;
  u64 ucode_data_hash_;

  /**
   * Reports whether the info needed before translating has been gathered. It determines which
   * modifications are needed and is used for drawing.
   *
   * Draws that cover nothing and allocate no pixel shader registers in SQ_PROGRAM_CNTL use the
   * previous pixel shader, so every shader before such a draw is translated again with a different
   * dynamically addressed register count. That would make host pipeline compilation depend on draw
   * order, so the effect is limited to shaders with dynamic register addressing, which are rare.
   * Depth format related translation modifications need care in this case.
   */
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

  /** Memory export eM write info for each control flow instruction, if the shader has any eM
   * writes. */
  std::vector<ControlFlowMemExportInfo> cf_memexport_info_;
  /** Which memexport elements (eM#) are written by any memexport in the shader. */
  u8 memexport_eM_written_ = 0;
  /** Equivalent of ControlFlowMemExportInfo::eM_potentially_written_before for the end of the
   * shader, covering the last memory export or exports if the end has multiple predecessor chains
   * exporting to memory. */
  u8 memexport_eM_potentially_written_before_end_ = 0;
  std::set<u32> memexport_stream_constants_;

  std::unordered_map<u64, std::unique_ptr<Translation>> translations_;

  u32 ucode_storage_index_ = UINT32_MAX;

 private:
  void GatherExecInformation(const ParsedExecInstruction& instr,
                             xenos::ucode::VertexFetchInstruction& previous_vfetch_full,
                             u32& unique_texture_bindings, StringBuffer& ucode_disasm_buffer);
  void GatherVertexFetchInformation(const xenos::ucode::VertexFetchInstruction& op,
                                    xenos::ucode::VertexFetchInstruction& previous_vfetch_full,
                                    StringBuffer& ucode_disasm_buffer);
  void GatherTextureFetchInformation(const xenos::ucode::TextureFetchInstruction& op,
                                     u32& unique_texture_bindings,
                                     StringBuffer& ucode_disasm_buffer);
  void GatherAluInstructionInformation(const xenos::ucode::AluInstruction& op, u32 exec_cf_index,
                                       StringBuffer& ucode_disasm_buffer);
  void GatherOperandInformation(const InstructionOperand& operand);
  void GatherFetchResultInformation(const InstructionResult& result);
  void GatherAluResultInformation(const InstructionResult& result, u32 exec_cf_index);
};

}  // namespace rex::gpu
