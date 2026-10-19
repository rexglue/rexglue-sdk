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

#include <cstdint>

#include <rex/types.h>
#include <rex/assert.h>
#include <rex/export.h>
#include <rex/xenos/xenos.h>
#include <rex/math.h>
#include <rex/platform.h>

/*
 * Parts of this code come from the freedreno project:
 * https://github.com/freedreno/freedreno/blob/master/includes/instr-a2xx.h
 *
 * Copyright (c) 2012 Rob Clark <robdclark@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * Microcode instruction layouts. Bit fields use only 32 bit types (uint32_t, int32_t or enums with
 * uint32_t or int32_t as the underlying type) because Visual C++ restarts packing when a field
 * requires a different alignment than the previous one.
 */
namespace rex::xenos::ucode {

/** Control flow opcodes used to schedule instructions. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class ControlFlowOpcode : u32 {
  /** No operation, used to fill space. */
  kNop = 0,
  /** Executes fetch or ALU instructions. */
  kExec = 1,
  /** Executes fetch or ALU instructions, then ends execution. */
  kExecEnd = 2,
  /** Executes conditionally on a bool constant. PredicateClean is false. */
  kCondExec = 3,
  /**
   * Executes conditionally on a bool constant, then ends execution. PredicateClean is false.
   *
   * Execution ends only if the condition is met, unlike on R600 where it ends unconditionally when
   * END_OF_PROGRAM is set in the control flow instruction. Shaders rely on this for an if / else in
   * their tail done via cexece (cexece b130, then cexece !b130, then an empty exece), where
   * ignoring the condition would never reach the second cexece.
   */
  kCondExecEnd = 4,
  /**
   * Executes conditionally on the current predicate.
   *
   * The hardware processes 64 vertices or pixels per sequencer, so the condition is the AND of the
   * predicate values of all active (not killed) invocations for (!p0) exec and the OR of them for
   * (p0) exec. If any invocation passes the predicate check, all of them enter the exec. Because of
   * this the compiler predicates the ALU and fetch instructions inside a predicated exec, and
   * mismatched control flow and ALU / fetch predication is invalid.
   */
  kCondExecPred = 5,
  /**
   * Executes conditionally on the current predicate, then ends execution.
   *
   * Execution ends only if the condition is met for any of the invocations, unlike on R600 where it
   * ends unconditionally when END_OF_PROGRAM is set in the control flow instruction.
   */
  kCondExecPredEnd = 6,
  /** Starts a loop that must be terminated with kLoopEnd, with a depth of up to 4. */
  kLoopStart = 7,
  /**
   * Continues or breaks out of a loop started with kLoopStart.
   *
   * The loop iteration is incremented before the count and predicated break checks.
   */
  kLoopEnd = 8,
  /**
   * Conditionally calls a function, with a depth of up to 4. Pushes a return address to the stack
   * for kReturn.
   */
  kCondCall = 9,
  /** Returns from the current function called by kCondCall. No operation outside a function. */
  kReturn = 10,
  /** Conditionally jumps to an arbitrary address based on a bool constant. */
  kCondJmp = 11,
  /** Allocates output values. */
  kAlloc = 12,
  /**
   * Executes conditionally on a bool constant. PredicateClean is true.
   *
   * This is cexec with a bool constant (kCondExec), not kCondExecPred. kCondExec has no predicate
   * clean field because the bool constant index occupies that space, while kCondExecPred has one.
   * An early description of this opcode as kCondExecPred with PredicateClean false predates
   * predicate clean fields in exec instructions.
   */
  kCondExecPredClean = 13,
  /** Executes conditionally on a bool constant, then ends execution. PredicateClean is true. */
  kCondExecPredCleanEnd = 14,
  /** Hints that no more vertex fetches will be performed. */
  kMarkVsFetchDone = 15,
};

/**
 * Reports whether a control flow opcode executes ALU or fetch instructions.
 *
 * @param opcode Control flow opcode to classify.
 * @return True when the opcode executes instructions.
 */
constexpr bool IsControlFlowOpcodeExec(ControlFlowOpcode opcode) {
  return opcode == ControlFlowOpcode::kExec || opcode == ControlFlowOpcode::kExecEnd ||
         opcode == ControlFlowOpcode::kCondExec || opcode == ControlFlowOpcode::kCondExecEnd ||
         opcode == ControlFlowOpcode::kCondExecPred ||
         opcode == ControlFlowOpcode::kCondExecPredEnd ||
         opcode == ControlFlowOpcode::kCondExecPredClean ||
         opcode == ControlFlowOpcode::kCondExecPredCleanEnd;
}

/**
 * Reports whether a control flow opcode terminates the shader after executing.
 *
 * @param opcode Control flow opcode to classify.
 * @return True when the opcode ends the shader.
 */
constexpr bool DoesControlFlowOpcodeEndShader(ControlFlowOpcode opcode) {
  return opcode == ControlFlowOpcode::kExecEnd || opcode == ControlFlowOpcode::kCondExecEnd ||
         opcode == ControlFlowOpcode::kCondExecPredEnd ||
         opcode == ControlFlowOpcode::kCondExecPredCleanEnd;
}

/**
 * Reports whether a conditional exec opcode has a clean predicate, see
 * ControlFlowOpcode::kCondExecPredClean.
 *
 * @param opcode Control flow opcode to classify.
 * @return True when the opcode has a clean predicate.
 */
constexpr bool DoesControlFlowCondExecHaveCleanPredicate(ControlFlowOpcode opcode) {
  return opcode == ControlFlowOpcode::kCondExecPredClean ||
         opcode == ControlFlowOpcode::kCondExecPredCleanEnd;
}

/** Selects whether addressing is based on a0 or aL. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class AddressingMode : u32 {
  /** Indexing into register sets is based on aL, allowing forms like c[aL + 5]. */
  kRelative = 0,
  /** Indexing into register sets is based on a0, allowing forms like c[a0 + 5]. */
  kAbsolute = 1,
};

/**
 * Type of a ControlFlowOpcode::kAlloc instruction. An allocation is a size reservation and a shader
 * may contain several.
 */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class AllocType : u32 {
  /** Allocation type a shader holds before any alloc instruction sets one. */
  kNone = 0,
  /** Vertex shader exports a position. */
  kVsPosition = 1,
  /** Vertex shader exports interpolators. */
  kVsInterpolators = 2,
  /** Pixel shader exports colors. */
  kPsColors = 2,
  /** Memory export. */
  kMemory = 3,
};

/** Instruction data for ControlFlowOpcode::kExec and kExecEnd. */
struct ControlFlowExecInstruction {
  ControlFlowOpcode GetOpcode() const { return opcode_; }
  AddressingMode GetAddressingMode() const { return address_mode_; }
  u32 GetAddress() const { return address_; }
  u32 GetCount() const { return count_; }
  /**
   * Returns the sequence bits, 2 per instruction.
   *
   * @return Per instruction, bit 0 is ALU (0) or fetch (1) and bit 1 is serialize.
   */
  u32 GetSequence() const { return sequence_; }
  bool IsPredicateClean() const { return is_predicate_clean_ == 1; }
  bool IsYield() const { return is_yield_ == 1; }

 private:
  /** Word 0 (32 bits). */
  u32 address_ : 12;
  u32 count_ : 3;
  u32 is_yield_ : 1;
  u32 sequence_ : 12;
  [[maybe_unused]] u32 vc_hi_ : 4;

  /** Word 1 (16 bits). */
  [[maybe_unused]] u32 vc_lo_ : 2;
  u32 : 7;
  /**
   * The compiler specifies PredicateClean false for the exec if the instructions inside it modify
   * the predicate, but may keep it true if the predicate set only refines the current predicate,
   * like in a nested `if`, because the optimization works.
   */
  u32 is_predicate_clean_ : 1;
  u32 : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
REX_STATIC_ASSERT_SIZE(ControlFlowExecInstruction, sizeof(u32) * 2);

/**
 * Instruction data for ControlFlowOpcode::kCondExec, kCondExecEnd, kCondExecPredClean and
 * kCondExecPredCleanEnd.
 */
struct ControlFlowCondExecInstruction {
  ControlFlowOpcode GetOpcode() const { return opcode_; }
  AddressingMode GetAddressingMode() const { return address_mode_; }
  u32 GetAddress() const { return address_; }
  u32 GetCount() const { return count_; }
  /**
   * Returns the sequence bits, 2 per instruction.
   *
   * @return Per instruction, bit 0 is ALU (0) or fetch (1) and bit 1 is serialize.
   */
  u32 GetSequence() const { return sequence_; }
  u32 GetBoolAddress() const { return bool_address_; }
  bool GetCondition() const { return condition_ == 1; }
  bool IsYield() const { return is_yield_ == 1; }

 private:
  /** Word 0 (32 bits). */
  u32 address_ : 12;
  u32 count_ : 3;
  u32 is_yield_ : 1;
  u32 sequence_ : 12;
  [[maybe_unused]] u32 vc_hi_ : 4;

  /** Word 1 (16 bits). */
  [[maybe_unused]] u32 vc_lo_ : 2;
  u32 bool_address_ : 8;
  u32 condition_ : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
REX_STATIC_ASSERT_SIZE(ControlFlowCondExecInstruction, sizeof(u32) * 2);

/** Instruction data for ControlFlowOpcode::kCondExecPred and kCondExecPredEnd. */
struct ControlFlowCondExecPredInstruction {
  ControlFlowOpcode GetOpcode() const { return opcode_; }
  AddressingMode GetAddressingMode() const { return address_mode_; }
  u32 GetAddress() const { return address_; }
  u32 GetCount() const { return count_; }
  /**
   * Returns the sequence bits, 2 per instruction.
   *
   * @return Per instruction, bit 0 is ALU (0) or fetch (1) and bit 1 is serialize.
   */
  u32 GetSequence() const { return sequence_; }
  bool IsPredicateClean() const { return is_predicate_clean_ == 1; }
  bool GetCondition() const { return condition_ == 1; }
  bool IsYield() const { return is_yield_ == 1; }

 private:
  /** Word 0 (32 bits). */
  u32 address_ : 12;
  u32 count_ : 3;
  u32 is_yield_ : 1;
  u32 sequence_ : 12;
  [[maybe_unused]] u32 vc_hi_ : 4;

  /** Word 1 (16 bits). */
  [[maybe_unused]] u32 vc_lo_ : 2;
  u32 : 7;
  u32 is_predicate_clean_ : 1;
  u32 condition_ : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
REX_STATIC_ASSERT_SIZE(ControlFlowCondExecPredInstruction, sizeof(u32) * 2);

/** Instruction data for ControlFlowOpcode::kLoopStart. */
struct ControlFlowLoopStartInstruction {
  ControlFlowOpcode GetOpcode() const { return opcode_; }
  AddressingMode GetAddressingMode() const { return address_mode_; }
  /**
   * Returns the address to jump to when skipping the loop, normally the instruction right after the
   * `endloop` instruction.
   *
   * @return Skip target address.
   */
  u32 GetAddress() const { return address_; }
  /**
   * Reports whether the loop reuses the current aL instead of resetting it to the loop start.
   *
   * @return True when aL is reused.
   */
  bool IsRepeat() const { return is_repeat_; }
  /**
   * Returns the integer constant register that holds the loop parameters (xenos::LoopConstant).
   *
   * @return Loop constant register index.
   */
  u32 GetLoopId() const { return loop_id_; }

 private:
  /** Word 0 (32 bits). */
  u32 address_ : 13;
  u32 is_repeat_ : 1;
  u32 : 2;
  u32 loop_id_ : 5;
  u32 : 11;

  /** Word 1 (16 bits). */
  u32 : 11;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
REX_STATIC_ASSERT_SIZE(ControlFlowLoopStartInstruction, sizeof(u32) * 2);

/** Instruction data for ControlFlowOpcode::kLoopEnd. */
struct ControlFlowLoopEndInstruction {
  ControlFlowOpcode GetOpcode() const { return opcode_; }
  AddressingMode GetAddressingMode() const { return address_mode_; }
  /**
   * Returns the address of the start of the loop body, normally the instruction right after the
   * `loop` instruction.
   *
   * @return Loop body address.
   */
  u32 GetAddress() const { return address_; }
  /**
   * Returns the integer constant register that holds the loop parameters (xenos::LoopConstant).
   *
   * @return Loop constant register index.
   */
  u32 GetLoopId() const { return loop_id_; }
  /**
   * Reports whether the loop breaks if the predicate in all 64 invocations matches the expected
   * value.
   *
   * @return True when the break is predicated.
   */
  bool IsPredicatedBreak() const { return is_predicated_break_; }
  bool GetCondition() const { return condition_ == 1; }

 private:
  /** Word 0 (32 bits). */
  u32 address_ : 13;
  u32 : 3;
  u32 loop_id_ : 5;
  u32 is_predicated_break_ : 1;
  u32 : 10;

  /** Word 1 (16 bits). */
  u32 : 10;
  u32 condition_ : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
REX_STATIC_ASSERT_SIZE(ControlFlowLoopEndInstruction, sizeof(u32) * 2);

/** Instruction data for ControlFlowOpcode::kCondCall. */
struct ControlFlowCondCallInstruction {
  ControlFlowOpcode GetOpcode() const { return opcode_; }
  AddressingMode GetAddressingMode() const { return address_mode_; }
  u32 GetAddress() const { return address_; }
  bool IsUnconditional() const { return is_unconditional_; }
  bool IsPredicated() const { return is_predicated_; }
  u32 GetBoolAddress() const { return bool_address_; }
  bool GetCondition() const { return condition_ == 1; }

 private:
  /** Word 0 (32 bits). */
  u32 address_ : 13;
  u32 is_unconditional_ : 1;
  u32 is_predicated_ : 1;
  u32 : 17;

  /** Word 1 (16 bits). */
  u32 : 2;
  u32 bool_address_ : 8;
  u32 condition_ : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
REX_STATIC_ASSERT_SIZE(ControlFlowCondCallInstruction, sizeof(u32) * 2);

/** Instruction data for ControlFlowOpcode::kReturn. */
struct ControlFlowReturnInstruction {
  ControlFlowOpcode GetOpcode() const { return opcode_; }
  AddressingMode GetAddressingMode() const { return address_mode_; }

 private:
  /** Word 0 (32 bits). */
  u32 : 32;

  /** Word 1 (16 bits). */
  u32 : 11;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
REX_STATIC_ASSERT_SIZE(ControlFlowReturnInstruction, sizeof(u32) * 2);

/** Instruction data for ControlFlowOpcode::kCondJmp. */
struct ControlFlowCondJmpInstruction {
  ControlFlowOpcode GetOpcode() const { return opcode_; }
  AddressingMode GetAddressingMode() const { return address_mode_; }
  u32 GetAddress() const { return address_; }
  bool IsUnconditional() const { return is_unconditional_; }
  bool IsPredicated() const { return is_predicated_; }
  u32 GetBoolAddress() const { return bool_address_; }
  bool GetCondition() const { return condition_ == 1; }

 private:
  /** Word 0 (32 bits). */
  u32 address_ : 13;
  u32 is_unconditional_ : 1;
  u32 is_predicated_ : 1;
  u32 : 17;

  /** Word 1 (16 bits). */
  u32 : 1;
  [[maybe_unused]] u32 direction_ : 1;
  u32 bool_address_ : 8;
  u32 condition_ : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
REX_STATIC_ASSERT_SIZE(ControlFlowCondJmpInstruction, sizeof(u32) * 2);

/** Instruction data for ControlFlowOpcode::kAlloc. */
struct ControlFlowAllocInstruction {
  ControlFlowOpcode GetOpcode() const { return opcode_; }
  u32 GetSize() const { return size_; }
  AllocType GetAllocType() const { return alloc_type_; }

 private:
  /** Word 0 (32 bits). */
  u32 size_ : 3;
  u32 : 29;

  /** Word 1 (16 bits). */
  u32 : 8;
  u32 : 1; /**< is_unserialized_ */
  AllocType alloc_type_ : 2;
  u32 : 1;
  ControlFlowOpcode opcode_ : 4;
};
REX_STATIC_ASSERT_SIZE(ControlFlowAllocInstruction, sizeof(u32) * 2);

/**
 * Control flow instruction of 48 bits in 2 dwords, viewed through the layout matching its opcode.
 */
union ControlFlowInstruction {
  ControlFlowOpcode GetOpcode() const { return opcode_value; }

  ControlFlowExecInstruction exec;                   /**< kExec* */
  ControlFlowCondExecInstruction cond_exec;          /**< kCondExec* */
  ControlFlowCondExecPredInstruction cond_exec_pred; /**< kCondExecPred* */
  ControlFlowLoopStartInstruction loop_start;        /**< kLoopStart */
  ControlFlowLoopEndInstruction loop_end;            /**< kLoopEnd */
  ControlFlowCondCallInstruction cond_call;          /**< kCondCall */
  ControlFlowReturnInstruction ret;                  /**< kReturn */
  ControlFlowCondJmpInstruction cond_jmp;            /**< kCondJmp */
  ControlFlowAllocInstruction alloc;                 /**< kAlloc */

  struct {
    u32 dword_0; /**< Low dword of the instruction. */
    u32 dword_1; /**< High dword of the instruction. */
  };
  struct {
    u32 unused_0 : 32;                  /**< Bits 0 to 31 of the instruction. No code reads it. */
    u32 unused_1 : 12;                  /**< Bits 32 to 43 of the instruction. No code reads it. */
    ControlFlowOpcode opcode_value : 4; /**< Bits 44 to 47 of the instruction, the opcode. */
  };
};
REX_STATIC_ASSERT_SIZE(ControlFlowInstruction, sizeof(u32) * 2);

/**
 * Splits 3 dwords of microcode into the 2 control flow instructions packed in them.
 *
 * @param dwords 3 dwords holding a pair of instructions.
 * @param out_ab Receives the first and the second instruction.
 */
inline void UnpackControlFlowInstructions(const u32* dwords, ControlFlowInstruction* out_ab) {
  u32 dword_0 = dwords[0];
  u32 dword_1 = dwords[1];
  u32 dword_2 = dwords[2];
  out_ab[0].dword_0 = dword_0;
  out_ab[0].dword_1 = dword_1 & 0xFFFF;
  out_ab[1].dword_0 = (dword_1 >> 16) | (dword_2 << 16);
  out_ab[1].dword_1 = dword_2 >> 16;
}

/** Opcode of a fetch instruction, either a vertex fetch or a texture operation. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class FetchOpcode : u32 {
  /** Fetches vertex data through a vertex fetch constant. */
  kVertexFetch = 0,

  /**
   * Fetches a texture sample.
   *
   * Parameters and their defaults are as follows.
   * - UnnormalizedTextureCoords is false (default) or true. It is only taken into account if
   *   AddressU (1D), AddressU/V (2D) or AddressU/V/W (3D, cube) are all set to a non wrapping mode.
   *   Unnormalized coordinates are required to access 1D textures wider than 8192 texels.
   * - MagFilter and MinFilter are point, linear or keep (default).
   * - MipFilter is point, linear, basemap (undocumented but assembled) or keep (default).
   * - VolMagFilter (3D only) is the filter used when the volume is magnified, "volume" as opposed
   *   to "texture". VolMinFilter (3D only) is its minification counterpart. Both are point, linear
   *   or keep (default).
   * - AnisoFilter is disabled, max1to1, max2to1, max4to1, max8to1, max16to1 or keep (default).
   * - UseComputedLOD is false or true (default).
   * - UseRegisterLOD is false (default) or true.
   * - LODBias is -4.0 to 3.9375 in 1/16 increments, default 0.
   * - OffsetX is a value added to the x component of the texel address right before sampling, -8.0
   *   to 7.5 in 1/2 increments, default 0. OffsetY (2D, 3D and cube only) and OffsetZ (3D and cube
   *   only) are similar.
   * - FetchValidOnly (default true) fetches data only for pixels inside the current primitive in a
   *   2x2 quad, as a performance optimization. It must be false if the result is used to calculate
   *   gradients.
   *
   * Coordinates by dimension are as follows.
   * - 1D is U (normalized or unnormalized).
   * - 2D is U, V (normalized or unnormalized).
   * - 3D, used for both 3D and stacked 2D textures, is U, V, W (normalized or unnormalized, the
   *   same for 3D W and stack layer). VolMagFilter and VolMinFilter between stack layers are
   *   supported.
   * - Cube is SC, TC (between 1 and 2 for normalized) and face ID (0.0 to 5.0), computed by the
   *   cube vector ALU instruction. The 1.5 constant is designed so that the output face coordinate
   *   range is {1.0 <= x < 2.0}, which keeps the upper mantissa bits constant throughout the output
   *   range.
   *
   * The total LOD for a sample is additive and based on what is enabled.
   *
   * For cube maps, register gradients are in cube space, not in SC/TC space like the coordinates
   * themselves.
   *
   * Cube maps are sampled through host cube views, so filtering crosses faces unless the Vulkan
   * non_seamless_cube_map option is set.
   *
   * Offsets are applied in texels of the base level, whatever LOD is sampled.
   */
  kTextureFetch = 1,

  /**
   * Writes the fraction of border color that would be blended into the texture data at the given
   * coordinates to the X component of the destination.
   *
   * May take all the parameters that tfetch takes.
   */
  kGetTextureBorderColorFrac = 16,

  /**
   * Writes the LOD for all of the pixels in the quad at the given coordinates to the X component
   * of the destination.
   *
   * The valid parameters are UnnormalizedTextureCoords, AnisoFilter, VolMagFilter and VolMinFilter.
   * LODBias is rejected, while UseComputedLOD, UseRegisterLOD and UseRegisterGradients assemble.
   * The translators ignore the LOD bias and grad_exp_adjust_h/v. MipFilter cannot be overridden
   * and does not assemble, and the translators query the LOD with a linear mip filter.
   *
   * Valid only in pixel shaders. The LOD is computed for all of the pixels in the quad (with
   * explicit gradients this may diverge) and the instruction does not assemble in vertex shaders
   * even with UseRegisterGradients=true. Both translators report a translation error for
   * UseRegisterGradients and for explicit LOD, so only implicit gradients are used.
   *
   * The translators return the unclamped LOD.
   */
  kGetTextureComputedLOD = 17,

  /**
   * Source is 2 component. XZ = ddx(source.xy), YW = ddy(source.xy).
   *
   * This uses the texture unit, where the LOD is computed for the whole quad. The translators
   * use coarse derivatives.
   */
  kGetTextureGradients = 18,

  /**
   * Writes the weights used in a bilinear fetch.
   * - X is the horizontal lerp factor.
   * - Y is the vertical lerp factor.
   * - Z is the depth lerp factor.
   * - W is the mip lerp factor.
   *
   * May take all the parameters that tfetch takes. Takes the filtering mode into account, and is
   * used with MagFilter=linear and MinFilter=linear. Offsets apply. The translators
   * compute the factors from the base level size and ignore the LOD, and write 0 to W.
   *
   * For cube maps, the translators compute the X and Y factors in SC/TC space from the face size.
   */
  kGetTextureWeights = 19,

  /** Source is 1 component. */
  kSetTextureLOD = 24,
  /** Source is 3 components. */
  kSetTextureGradientsHorz = 25,
  /** Source is 3 components. */
  kSetTextureGradientsVert = 26,
};

/**
 * Fetch destination swizzle. The component indices are absolute, not relative to the component
 * itself like in ALU operation sources.
 */
enum class FetchDestinationSwizzle : u8 {
  kX = 0, /**< Fetched X component. */
  kY = 1, /**< Fetched Y component. */
  kZ = 2, /**< Fetched Z component. */
  kW = 3, /**< Fetched W component. */
  k0 = 4, /**< Constant 0. */
  k1 = 5, /**< Constant 1. */
  /** Keeps the current value of the destination register without writing. */
  kKeep = 7,
};

/**
 * Extracts the swizzle of 1 destination component from a packed destination swizzle.
 *
 * @param swizzle Packed destination swizzle, 3 bits per component.
 * @param component Destination component index, 0 for X to 3 for W.
 * @return Swizzle of the component.
 */
constexpr FetchDestinationSwizzle GetFetchDestinationComponentSwizzle(u32 swizzle, u32 component) {
  return static_cast<FetchDestinationSwizzle>((swizzle >> (3 * component)) & 0b111);
}

/** Vertex fetch instruction of 3 dwords. */
struct alignas(u32) VertexFetchInstruction {
  FetchOpcode GetOpcode() const { return data_.opcode_value; }

  bool IsPredicated() const { return data_.is_predicated; }
  bool GetPredicateCondition() const { return data_.pred_condition == 1; }
  /**
   * Returns the vertex fetch constant index [0-95]. Applicable only to vfetch_full, as vfetch_mini
   * reuses its address.
   *
   * @return Vertex fetch constant index.
   */
  u32 GetFetchConstantIndex() const { return data_.const_index * 3 + data_.const_index_sel; }

  u32 GetDest() const { return data_.dst_reg; }
  u32 GetDestSwizzle() const { return data_.dst_swiz; }
  bool IsDestRelative() const { return data_.dst_reg_am; }
  /**
   * Returns the source register. Applicable only to vfetch_full, as vfetch_mini reuses its address.
   *
   * @return Source register index.
   */
  u32 GetSrc() const { return data_.src_reg; }
  u32 GetSrcSwizzle() const { return data_.src_swiz; }
  bool IsSrcRelative() const { return data_.src_reg_am; }

  u32 GetPrefetchCount() const { return data_.prefetch_count; }
  bool IsMiniFetch() const { return data_.is_mini_fetch == 1; }

  xenos::VertexFormat GetDataFormat() const { return data_.format; }
  /**
   * Returns the exponent adjustment, [-32, 31].
   *
   * @return Exponent adjustment.
   */
  int GetExpAdjust() const { return data_.exp_adjust; }
  bool IsSigned() const { return data_.fomat_comp_all == 1; }
  bool IsNormalized() const { return data_.num_format_all == 0; }
  xenos::SignedRepeatingFractionMode GetSignedRfMode() const { return data_.signed_rf_mode_all; }
  /**
   * Reports whether the floating point index is rounded to the nearest integer, as
   * floor(index + 0.5), because rounding to the nearest even makes no sense for addressing (both
   * 1.5 and 2.5 would be 2). Otherwise it is floored, rounded towards negative infinity. Applicable
   * only to vfetch_full, as vfetch_mini reuses its address.
   *
   * @return True when the index is rounded.
   */
  bool IsIndexRounded() const { return data_.is_index_rounded == 1; }
  /**
   * Returns the dword stride, [0, 255]. Applicable only to vfetch_full, as vfetch_mini reuses its
   * address.
   *
   * @return Dword stride.
   */
  u32 GetStride() const { return data_.stride; }
  /**
   * Returns the dword offset, [-4194304, 4194303].
   *
   * @return Dword offset.
   */
  i32 GetOffset() const { return data_.offset; }

 private:
  struct Data {
    struct {
      FetchOpcode opcode_value : 5;
      u32 src_reg : 6;
      u32 src_reg_am : 1;
      u32 dst_reg : 6;
      u32 dst_reg_am : 1;
      u32 must_be_one : 1;
      u32 const_index : 5;
      u32 const_index_sel : 2;
      /** Prefetch count minus 1. */
      u32 prefetch_count : 3;
      /** Absolute, 1 component. */
      u32 src_swiz : 2;
    };
    struct {
      u32 dst_swiz : 12;
      u32 fomat_comp_all : 1;
      u32 num_format_all : 1;
      xenos::SignedRepeatingFractionMode signed_rf_mode_all : 1;
      u32 is_index_rounded : 1;
      xenos::VertexFormat format : 6;
      u32 reserved2 : 2;
      i32 exp_adjust : 6;
      u32 is_mini_fetch : 1;
      u32 is_predicated : 1;
    };
    struct {
      u32 stride : 8;
      i32 offset : 23;
      u32 pred_condition : 1;
    };
  };
  Data data_;
};
REX_STATIC_ASSERT_SIZE(VertexFetchInstruction, sizeof(u32) * 3);

/** Texture fetch instruction of 3 dwords. */
struct alignas(u32) TextureFetchInstruction {
  FetchOpcode GetOpcode() const { return data_.opcode_value; }

  bool IsPredicated() const { return data_.is_predicated; }
  bool GetPredicateCondition() const { return data_.pred_condition == 1; }
  /**
   * Returns the texture fetch constant index [0-31].
   *
   * @return Texture fetch constant index.
   */
  u32 GetFetchConstantIndex() const { return data_.const_index; }

  u32 GetDest() const { return data_.dst_reg; }
  u32 GetDestSwizzle() const { return data_.dst_swiz; }
  bool IsDestRelative() const { return data_.dst_reg_am; }
  u32 GetSrc() const { return data_.src_reg; }
  u32 GetSrcSwizzle() const { return data_.src_swiz; }
  bool IsSrcRelative() const { return data_.src_reg_am; }

  xenos::FetchOpDimension GetDimension() const { return data_.dimension; }
  bool GetFetchValidOnly() const { return data_.fetch_valid_only == 1; }
  bool GetUnnormalizedCoordinates() const { return data_.tx_coord_denorm == 1; }
  bool HasMagFilter() const { return data_.mag_filter != xenos::TextureFilter::kUseFetchConst; }
  xenos::TextureFilter GetMagFilter() const { return data_.mag_filter; }
  bool HasMinFilter() const { return data_.min_filter != xenos::TextureFilter::kUseFetchConst; }
  xenos::TextureFilter GetMinFilter() const { return data_.min_filter; }
  bool HasMipFilter() const { return data_.mip_filter != xenos::TextureFilter::kUseFetchConst; }
  xenos::TextureFilter GetMipFilter() const { return data_.mip_filter; }
  bool HasAnisoFilter() const { return data_.aniso_filter != xenos::AnisoFilter::kUseFetchConst; }
  xenos::AnisoFilter GetAnisoFilter() const { return data_.aniso_filter; }
  /**
   * Reports whether the instruction overrides the volume magnification filter of the fetch
   * constant.
   *
   * @return True when the filter is not `kUseFetchConst`.
   */
  bool HasVolMagFilter() const {
    return data_.vol_mag_filter != xenos::TextureFilter::kUseFetchConst;
  }
  xenos::TextureFilter GetVolMagFilter() const { return data_.vol_mag_filter; }
  /**
   * Reports whether the instruction overrides the volume minification filter of the fetch constant.
   *
   * @return True when the filter is not `kUseFetchConst`.
   */
  bool HasVolMinFilter() const {
    return data_.vol_min_filter != xenos::TextureFilter::kUseFetchConst;
  }
  xenos::TextureFilter GetVolMinFilter() const { return data_.vol_min_filter; }
  bool UsesComputedLOD() const { return data_.use_comp_lod == 1; }
  bool UsesRegisterLOD() const { return data_.use_reg_lod == 1; }
  bool UsesRegisterGradients() const { return data_.use_reg_gradients == 1; }
  xenos::SampleLocation GetSampleLocation() const { return data_.sample_location; }
  f32 GetLODBias() const { return static_cast<f32>(data_.lod_bias) * (1.0f / 16.0f); }
  f32 GetOffsetX() const { return static_cast<f32>(data_.offset_x) * 0.5f; }
  f32 GetOffsetY() const { return static_cast<f32>(data_.offset_y) * 0.5f; }
  f32 GetOffsetZ() const { return static_cast<f32>(data_.offset_z) * 0.5f; }

 private:
  struct Data {
    struct {
      FetchOpcode opcode_value : 5;
      u32 src_reg : 6;
      u32 src_reg_am : 1;
      u32 dst_reg : 6;
      u32 dst_reg_am : 1;
      u32 fetch_valid_only : 1;
      u32 const_index : 5;
      u32 tx_coord_denorm : 1;
      /** Absolute, 3 components. */
      u32 src_swiz : 6;
    };
    struct {
      u32 dst_swiz : 12;
      xenos::TextureFilter mag_filter : 2;
      xenos::TextureFilter min_filter : 2;
      xenos::TextureFilter mip_filter : 2;
      xenos::AnisoFilter aniso_filter : 3;
      xenos::ArbitraryFilter arbitrary_filter : 3;
      xenos::TextureFilter vol_mag_filter : 2;
      xenos::TextureFilter vol_min_filter : 2;
      u32 use_comp_lod : 1;
      u32 use_reg_lod : 1;
      u32 unk : 1;
      u32 is_predicated : 1;
    };
    struct {
      u32 use_reg_gradients : 1;
      xenos::SampleLocation sample_location : 1;
      i32 lod_bias : 7;
      u32 unused : 5;
      xenos::FetchOpDimension dimension : 2;
      i32 offset_x : 5;
      i32 offset_y : 5;
      i32 offset_z : 5;
      u32 pred_condition : 1;
    };
  };
  Data data_;
};
REX_STATIC_ASSERT_SIZE(TextureFetchInstruction, sizeof(u32) * 3);

/** Fetch instruction of 3 dwords, viewed as a vertex or texture fetch according to its opcode. */
union alignas(u32) FetchInstruction {
 public:
  FetchOpcode GetOpcode() const { return data_.opcode_value; }

  bool IsPredicated() const { return data_.is_predicated; }
  bool GetPredicateCondition() const { return data_.pred_condition == 1; }

  u32 GetDest() const { return data_.dst_reg; }
  u32 GetDestSwizzle() const { return data_.dst_swiz; }
  bool IsDestRelative() const { return data_.dst_reg_am; }
  u32 GetSrc() const { return data_.src_reg; }
  bool IsSrcRelative() const { return data_.src_reg_am; }

  /**
   * Returns the vertex fetch data, valid for FetchOpcode::kVertexFetch.
   *
   * @return Vertex fetch instruction.
   */
  const VertexFetchInstruction& GetVertexFetch() const { return vertex_fetch_; }
  /**
   * Returns the texture fetch data, valid for operations other than FetchOpcode::kVertexFetch.
   *
   * @return Texture fetch instruction.
   */
  const TextureFetchInstruction& GetTextureFetch() const { return texture_fetch_; }

 private:
  struct Data {
    struct {
      FetchOpcode opcode_value : 5;
      u32 src_reg : 6;
      u32 src_reg_am : 1;
      u32 dst_reg : 6;
      u32 dst_reg_am : 1;
      /** Defined by the vertex or texture fetch layout. */
      u32 : 1;
      /** [0-31], points to 1 tf# or 3 vf# constants. */
      u32 const_index : 5;
      /** Defined by the vertex or texture fetch layout. */
      u32 : 7;
    };
    struct {
      u32 dst_swiz : 12;
      /** Defined by the vertex or texture fetch layout. */
      u32 : 19;
      u32 is_predicated : 1;
    };
    struct {
      /** Defined by the vertex or texture fetch layout. */
      u32 : 31;
      u32 pred_condition : 1;
    };
  };
  Data data_;
  VertexFetchInstruction vertex_fetch_;
  TextureFetchInstruction texture_fetch_;
};
REX_STATIC_ASSERT_SIZE(FetchInstruction, sizeof(u32) * 3);

/**
 * State changes of an ALU operation, used to analyze shaders and skip instructions that write
 * nothing.
 */
enum AluOpChangedState : u8 {
  /** The operation changes no state besides its destination. */
  kAluOpChangedStateNone = 0,
  /** The operation writes the address register, set for maxas and maxasf. */
  kAluOpChangedStateAddressRegister = 1 << 0,
  /** The operation writes the predicate, set for the setp opcodes. */
  kAluOpChangedStatePredicate = 1 << 1,
  /** The operation can kill the pixel, set for the kills opcodes. */
  kAluOpChangedStatePixelKill = 1 << 2,
};

/**
 * Scalar ALU opcodes.
 *
 * The scalar and vector opcode descriptions follow these conventions.
 * - All temporary registers are vec4s.
 * - Most scalar operations work with 1 or 2 components of the source register or of the float
 *   constant passed as the third operand of the whole co issued ALU instruction, denoted a (left
 *   operand) and b (right operand). a is component (3 + src3_swizzle[6:7]) & 3 (W, alpha). b is
 *   component (0 + src3_swizzle[0:1]) & 3 (X, red).
 * - mulsc, addsc and subsc take 2 operands. The left operand is a float constant with the a (W)
 *   swizzle, addressed by the third operand index and addressing mode. The right operand is a
 *   temporary register with the b (X) swizzle, with the index constructed from bit 0 of
 *   scalar_opcode as bit 0, bit 0 of src3_sel as bit 1 and src3_swizzle[2:5] as bits 2 to 5. The 2
 *   variants of each opcode are the same instruction, with 1 bit of the register index stored in
 *   the opcode field. abs_constants and the negation of the third source apply to both the constant
 *   and the temporary register.
 * - Some scalar operations have no operands.
 * - Scalar operations replicate the result into all masked components.
 * - ps is the previous result of a scalar operation. It is not modified when the instruction that
 *   would write it fails its predication check.
 * - Multiplication follows Direct3D 9 rules (as GCN v_*_legacy_f32 instructions) wherever it is
 *   present (mul, mad, dp and others), and so does NaN handling in min and max. A zero or denormal
 *   operand makes the product +0 whatever the signs of the other operands, as Infinity * 0
 *   resulting in NaN breaks vector normalization (rcp and mul). Therefore +0 * -0 is +0 and not -0,
 *   and multiply add must not be replaced with a conditional move of the third operand for zero
 *   multiplicands, because +0 + -0 is +0 while the term may be -0.
 * - Multiply add is treated as not fused, like GCN v_mad_legacy_f32 rather than v_fma_legacy_f32,
 *   so translators must not use instructions that the host GPU may interpret as fused multiply
 *   add. The DXBC translator emits separate mul and add instead of mad, and the SPIR-V translator
 *   marks them NoContraction.
 */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class AluScalarOpcode : u32 {
  /**
   * Floating Point Add.
   *
   * @code
   * adds/ADDs dest, src0.ab
   *     dest.xyzw = src0.a + src0.b;
   * @endcode
   */
  kAdds = 0,

  /**
   * Floating Point Add (with Previous).
   *
   * @code
   * adds_prev/ADD_PREVs dest, src0.a
   *     dest.xyzw = src0.a + ps;
   * @endcode
   */
  kAddsPrev = 1,

  /**
   * Floating Point Multiply.
   *
   * @code
   * muls/MULs dest, src0.ab
   *     dest.xyzw = src0.a * src0.b;
   * @endcode
   */
  kMuls = 2,

  /**
   * Floating Point Multiply (with Previous).
   *
   * @code
   * muls_prev/MUL_PREVs dest, src0.a
   *     dest.xyzw = src0.a * ps;
   * @endcode
   */
  kMulsPrev = 3,

  /**
   * Scalar Multiply Emulating LIT Operation.
   *
   * @code
   * muls_prev2/MUL_PREV2s dest, src0.ab
   *    dest.xyzw =
   *        ps == -FLT_MAX || !isfinite(ps) || !isfinite(src0.b) || src0.b <= 0
   *        ? -FLT_MAX : src0.a * ps;
   * @endcode
   */
  kMulsPrev2 = 4,

  /**
   * Floating Point Maximum.
   *
   * @code
   * maxs/MAXs dest, src0.ab
   *     dest.xyzw = src0.a >= src0.b ? src0.a : src0.b;
   * @endcode
   */
  kMaxs = 5,

  /**
   * Floating Point Minimum.
   *
   * @code
   * mins/MINs dest, src0.ab
   *     dest.xyzw = src0.a < src0.b ? src0.a : src0.b;
   * @endcode
   */
  kMins = 6,

  /**
   * Floating Point Set If Equal.
   *
   * @code
   * seqs/SETEs dest, src0.a
   *     dest.xyzw = src0.a == 0.0 ? 1.0 : 0.0;
   * @endcode
   */
  kSeqs = 7,

  /**
   * Floating Point Set If Greater Than.
   *
   * @code
   * sgts/SETGTs dest, src0.a
   *     dest.xyzw = src0.a > 0.0 ? 1.0 : 0.0;
   * @endcode
   */
  kSgts = 8,

  /**
   * Floating Point Set If Greater Than Or Equal.
   *
   * @code
   * sges/SETGTEs dest, src0.a
   *     dest.xyzw = src0.a >= 0.0 ? 1.0 : 0.0;
   * @endcode
   */
  kSges = 9,

  /**
   * Floating Point Set If Not Equal.
   *
   * @code
   * snes/SETNEs dest, src0.a
   *     dest.xyzw = src0.a != 0.0 ? 1.0 : 0.0;
   * @endcode
   */
  kSnes = 10,

  /**
   * Floating Point Fractional.
   *
   * @code
   * frcs/FRACs dest, src0.a
   *     dest.xyzw = src0.a - floor(src0.a);
   * @endcode
   */
  kFrcs = 11,

  /**
   * Floating Point Truncate.
   *
   * @code
   * truncs/TRUNCs dest, src0.a
   *     dest.xyzw = src0.a >= 0 ? floor(src0.a) : -floor(-src0.a);
   * @endcode
   */
  kTruncs = 12,

  /**
   * Floating Point Floor.
   *
   * @code
   * floors/FLOORs dest, src0.a
   *     dest.xyzw = floor(src0.a);
   * @endcode
   */
  kFloors = 13,

  /**
   * Scalar Base 2 Exponent, IEEE.
   *
   * @code
   * exp/EXP_IEEE dest, src0.a
   *     dest.xyzw = src0.a == 0.0 ? 1.0 : pow(2, src0.a);
   * @endcode
   */
  kExp = 14,

  /**
   * Scalar Base 2 Log.
   *
   * @code
   * logc/LOG_CLAMP dest, src0.a
   *     float t = src0.a == 1.0 ? 0.0 : log(src0.a) / log(2.0);
   *     dest.xyzw = t == -INF ? -FLT_MAX : t;
   * @endcode
   */
  kLogc = 15,

  /**
   * Scalar Base 2 IEEE Log.
   *
   * @code
   * log/LOG_IEEE dest, src0.a
   *     dest.xyzw = src0.a == 1.0 ? 0.0 : log(src0.a) / log(2.0);
   * @endcode
   */
  kLog = 16,

  /**
   * Scalar Reciprocal, Clamp to Maximum.
   *
   * @code
   * rcpc/RECIP_CLAMP dest, src0.a
   *     float t = src0.a == 1.0 ? 1.0 : 1.0 / src0.a;
   *     if (t == -INF) t = -FLT_MAX;
   *     else if (t == INF) t = FLT_MAX;
   *     dest.xyzw = t;
   * @endcode
   */
  kRcpc = 17,

  /**
   * Scalar Reciprocal, Clamp to Zero.
   *
   * Mimicking the behavior of the fixed function pipeline.
   *
   * @code
   * rcpf/RECIP_FF dest, src0.a
   *     float t = src0.a == 1.0 ? 1.0 : 1.0 / src0.a;
   *     if (t == -INF) t = -0.0;
   *     else if (t == INF) t = 0.0;
   *     dest.xyzw = t;
   * @endcode
   */
  kRcpf = 18,

  /**
   * Scalar Reciprocal, IEEE Approximation.
   *
   * @code
   * rcp/RECIP_IEEE dest, src0.a
   *     dest.xyzw = src0.a == 1.0 ? 1.0 : 1.0 / src0.a;
   * @endcode
   */
  kRcp = 19,

  /**
   * Scalar Reciprocal Square Root, Clamp to Maximum.
   *
   * @code
   * rsqc/RECIPSQ_CLAMP dest, src0.a
   *     float t = src0.a == 1.0 ? 1.0 : 1.0 / sqrt(src0.a);
   *     if (t == -INF) t = -FLT_MAX;
   *     else if (t == INF) t = FLT_MAX;
   *     dest.xyzw = t;
   * @endcode
   */
  kRsqc = 20,

  /**
   * Scalar Reciprocal Square Root, Clamp to Zero.
   *
   * @code
   * rsqf/RECIPSQ_FF dest, src0.a
   *     float t = src0.a == 1.0 ? 1.0 : 1.0 / sqrt(src0.a);
   *     if (t == -INF) t = -0.0;
   *     else if (t == INF) t = 0.0;
   *     dest.xyzw = t;
   * @endcode
   */
  kRsqf = 21,

  /**
   * Scalar Reciprocal Square Root, IEEE Approximation.
   *
   * @code
   * rsq/RECIPSQ_IEEE dest, src0.a
   *     dest.xyzw = src0.a == 1.0 ? 1.0 : 1.0 / sqrt(src0.a);
   * @endcode
   */
  kRsq = 22,

  /**
   * Floating Point Maximum with Copy To Integer in AR.
   *
   * @code
   * maxas dest, src0.ab
   * movas/MOVAs dest, src0.aa
   *     a0 = (int)clamp(floor(src0.a + 0.5), -256.0, 255.0);
   *     dest.xyzw = src0.a >= src0.b ? src0.a : src0.b;
   * @endcode
   */
  kMaxAs = 23,

  /**
   * Floating Point Maximum with Copy Truncated To Integer in AR.
   *
   * @code
   * maxasf dest, src0.ab
   * movasf/MOVA_FLOORs dest, src0.aa
   *     a0 = (int)clamp(floor(src0.a), -256.0, 255.0);
   *     dest.xyzw = src0.a >= src0.b ? src0.a : src0.b;
   * @endcode
   */
  kMaxAsf = 24,

  /**
   * Floating Point Subtract.
   *
   * @code
   * subs/SUBs dest, src0.ab
   *     dest.xyzw = src0.a - src0.b;
   * @endcode
   */
  kSubs = 25,

  /**
   * Floating Point Subtract (with Previous).
   *
   * @code
   * subs_prev/SUB_PREVs dest, src0.a
   *     dest.xyzw = src0.a - ps;
   * @endcode
   */
  kSubsPrev = 26,

  /**
   * Floating Point Predicate Set If Equal.
   *
   * @code
   * setp_eq/PRED_SETEs dest, src0.a
   *     if (src0.a == 0.0) {
   *       dest.xyzw = 0.0;
   *       p0 = 1;
   *     } else {
   *       dest.xyzw = 1.0;
   *       p0 = 0;
   *     }
   * @endcode
   */
  kSetpEq = 27,

  /**
   * Floating Point Predicate Set If Not Equal.
   *
   * @code
   * setp_ne/PRED_SETNEs dest, src0.a
   *     if (src0.a != 0.0) {
   *       dest.xyzw = 0.0;
   *       p0 = 1;
   *     } else {
   *       dest.xyzw = 1.0;
   *       p0 = 0;
   *     }
   * @endcode
   */
  kSetpNe = 28,

  /**
   * Floating Point Predicate Set If Greater Than.
   *
   * @code
   * setp_gt/PRED_SETGTs dest, src0.a
   *     if (src0.a > 0.0) {
   *       dest.xyzw = 0.0;
   *       p0 = 1;
   *     } else {
   *       dest.xyzw = 1.0;
   *       p0 = 0;
   *     }
   * @endcode
   */
  kSetpGt = 29,

  /**
   * Floating Point Predicate Set If Greater Than Or Equal.
   *
   * @code
   * setp_ge/PRED_SETGTEs dest, src0.a
   *     if (src0.a >= 0.0) {
   *       dest.xyzw = 0.0;
   *       p0 = 1;
   *     } else {
   *       dest.xyzw = 1.0;
   *       p0 = 0;
   *     }
   * @endcode
   */
  kSetpGe = 30,

  /**
   * Predicate Counter Invert.
   *
   * @code
   * setp_inv/PRED_SET_INVs dest, src0.a
   *     if (src0.a == 1.0) {
   *       dest.xyzw = 0.0;
   *       p0 = 1;
   *     } else {
   *       if (src0.a == 0.0) {
   *         dest.xyzw = 1.0;
   *       } else {
   *         dest.xyzw = src0.a;
   *       }
   *       p0 = 0;
   *     }
   * @endcode
   */
  kSetpInv = 31,

  /**
   * Predicate Counter Pop.
   *
   * @code
   * setp_pop/PRED_SET_POPs dest, src0.a
   *     if (src0.a - 1.0 <= 0.0) {
   *       dest.xyzw = 0.0;
   *       p0 = 1;
   *     } else {
   *       dest.xyzw = src0.a - 1.0;
   *       p0 = 0;
   *     }
   * @endcode
   */
  kSetpPop = 32,

  /**
   * Predicate Counter Clear.
   *
   * @code
   * setp_clr/PRED_SET_CLRs dest
   *     dest.xyzw = FLT_MAX;
   *     p0 = 0;
   * @endcode
   */
  kSetpClr = 33,

  /**
   * Predicate Counter Restore.
   *
   * @code
   * setp_rstr/PRED_SET_RESTOREs dest, src0.a
   *     if (src0.a == 0.0) {
   *       dest.xyzw = 0.0;
   *       p0 = 1;
   *     } else {
   *       dest.xyzw = src0.a;
   *       p0 = 0;
   *     }
   * @endcode
   */
  kSetpRstr = 34,

  /**
   * Floating Point Pixel Kill If Equal.
   *
   * @code
   * kills_eq/KILLEs dest, src0.a
   *     if (src0.a == 0.0) {
   *       dest.xyzw = 1.0;
   *       discard;
   *     } else {
   *       dest.xyzw = 0.0;
   *     }
   * @endcode
   */
  kKillsEq = 35,

  /**
   * Floating Point Pixel Kill If Greater Than.
   *
   * @code
   * kills_gt/KILLGTs dest, src0.a
   *     if (src0.a > 0.0) {
   *       dest.xyzw = 1.0;
   *       discard;
   *     } else {
   *       dest.xyzw = 0.0;
   *     }
   * @endcode
   */
  kKillsGt = 36,

  /**
   * Floating Point Pixel Kill If Greater Than Or Equal.
   *
   * @code
   * kills_ge/KILLGTEs dest, src0.a
   *     if (src0.a >= 0.0) {
   *       dest.xyzw = 1.0;
   *       discard;
   *     } else {
   *       dest.xyzw = 0.0;
   *     }
   * @endcode
   */
  kKillsGe = 37,

  /**
   * Floating Point Pixel Kill If Not Equal.
   *
   * @code
   * kills_ne/KILLNEs dest, src0.a
   *     if (src0.a != 0.0) {
   *       dest.xyzw = 1.0;
   *       discard;
   *     } else {
   *       dest.xyzw = 0.0;
   *     }
   * @endcode
   */
  kKillsNe = 38,

  /**
   * Floating Point Pixel Kill If One.
   *
   * @code
   * kills_one/KILLONEs dest, src0.a
   *     if (src0.a == 1.0) {
   *       dest.xyzw = 1.0;
   *       discard;
   *     } else {
   *       dest.xyzw = 0.0;
   *     }
   * @endcode
   */
  kKillsOne = 39,

  /**
   * Scalar Square Root, IEEE Approximation.
   *
   * @code
   * sqrt/SQRT_IEEE dest, src0.a
   *     dest.xyzw = sqrt(src0.a);
   * @endcode
   */
  kSqrt = 40,

  /** mulsc/MUL_CONST_0 dest, src0.a, src1.a */
  kMulsc0 = 42,
  /** mulsc/MUL_CONST_1 dest, src0.a, src1.a */
  kMulsc1 = 43,
  /** addsc/ADD_CONST_0 dest, src0.a, src1.a */
  kAddsc0 = 44,
  /** addsc/ADD_CONST_1 dest, src0.a, src1.a */
  kAddsc1 = 45,
  /** subsc/SUB_CONST_0 dest, src0.a, src1.a */
  kSubsc0 = 46,
  /** subsc/SUB_CONST_1 dest, src0.a, src1.a */
  kSubsc1 = 47,

  /**
   * Scalar Sin.
   *
   * @code
   * sin/SIN dest, src0.a
   *     dest.xyzw = sin(src0.a);
   * @endcode
   */
  kSin = 48,

  /**
   * Scalar Cos.
   *
   * @code
   * cos/COS dest, src0.a
   *     dest.xyzw = cos(src0.a);
   * @endcode
   */
  kCos = 49,

  /**
   * retain_prev/RETAIN_PREV dest
   *
   * @code
   *     dest.xyzw = ps;
   * @endcode
   */
  kRetainPrev = 50,
};

/** Static description of a scalar ALU opcode. */
struct AluScalarOpcodeInfo {
  const char* name; /**< Lowercase mnemonic of the opcode. */
  /**
   * 0 means no operands. 1 means 1 single component (W) or 2 component (WX) r# or c#. 2 means c#.w
   * and r#.x.
   */
  u32 operand_count;
  /** If operand_count is 1, true when both W and X of the operand are used rather than only W. */
  bool single_operand_is_two_component;
  /**
   * All scalar instructions except for retain_prev modify the previous scalar register, so they
   * must be executed even if they do not write any result and do not perform any other state
   * changes.
   */
  AluOpChangedState changed_state;
};

/** Indexed by the 6 bit scalar opcode, 64 entries. */
extern REX_API const AluScalarOpcodeInfo kAluScalarOpcodeInfos[64];

/**
 * Looks up the description of a scalar ALU opcode.
 *
 * @param opcode Scalar opcode.
 * @return Description of the opcode.
 */
inline const AluScalarOpcodeInfo& GetAluScalarOpcodeInfo(AluScalarOpcode opcode) {
  REX_ASSERT(static_cast<u32>(opcode) < rex::CountOf(kAluScalarOpcodeInfos));
  return kAluScalarOpcodeInfos[static_cast<u32>(opcode)];
}

/** Vector ALU opcodes. */
// NOLINTNEXTLINE(performance-enum-size): Xenos bit field, MSVC packs per type
enum class AluVectorOpcode : u32 {
  /**
   * Per Component Floating Point Add.
   *
   * @code
   * add/ADDv dest, src0, src1
   *     dest.x = src0.x + src1.x;
   *     dest.y = src0.y + src1.y;
   *     dest.z = src0.z + src1.z;
   *     dest.w = src0.w + src1.w;
   * @endcode
   */
  kAdd = 0,

  /**
   * Per Component Floating Point Multiply.
   *
   * @code
   * mul/MULv dest, src0, src1
   *     dest.x = src0.x * src1.x;
   *     dest.y = src0.y * src1.y;
   *     dest.z = src0.z * src1.z;
   *     dest.w = src0.w * src1.w;
   * @endcode
   */
  kMul = 1,

  /**
   * Per Component Floating Point Maximum.
   *
   * @code
   * max/MAXv dest, src0, src1
   *     dest.x = src0.x >= src1.x ? src0.x : src1.x;
   *     dest.y = src0.y >= src1.y ? src0.y : src1.y;
   *     dest.z = src0.z >= src1.z ? src0.z : src1.z;
   *     dest.w = src0.w >= src1.w ? src0.w : src1.w;
   * @endcode
   */
  kMax = 2,

  /**
   * Per Component Floating Point Minimum.
   *
   * @code
   * min/MINv dest, src0, src1
   *     dest.x = src0.x < src1.x ? src0.x : src1.x;
   *     dest.y = src0.y < src1.y ? src0.y : src1.y;
   *     dest.z = src0.z < src1.z ? src0.z : src1.z;
   *     dest.w = src0.w < src1.w ? src0.w : src1.w;
   * @endcode
   */
  kMin = 3,

  /**
   * Per Component Floating Point Set If Equal.
   *
   * @code
   * seq/SETEv dest, src0, src1
   *     dest.x = src0.x == src1.x ? 1.0 : 0.0;
   *     dest.y = src0.y == src1.y ? 1.0 : 0.0;
   *     dest.z = src0.z == src1.z ? 1.0 : 0.0;
   *     dest.w = src0.w == src1.w ? 1.0 : 0.0;
   * @endcode
   */
  kSeq = 4,

  /**
   * Per Component Floating Point Set If Greater Than.
   *
   * @code
   * sgt/SETGTv dest, src0, src1
   *     dest.x = src0.x > src1.x ? 1.0 : 0.0;
   *     dest.y = src0.y > src1.y ? 1.0 : 0.0;
   *     dest.z = src0.z > src1.z ? 1.0 : 0.0;
   *     dest.w = src0.w > src1.w ? 1.0 : 0.0;
   * @endcode
   */
  kSgt = 5,

  /**
   * Per Component Floating Point Set If Greater Than Or Equal.
   *
   * @code
   * sge/SETGTEv dest, src0, src1
   *     dest.x = src0.x >= src1.x ? 1.0 : 0.0;
   *     dest.y = src0.y >= src1.y ? 1.0 : 0.0;
   *     dest.z = src0.z >= src1.z ? 1.0 : 0.0;
   *     dest.w = src0.w >= src1.w ? 1.0 : 0.0;
   * @endcode
   */
  kSge = 6,

  /**
   * Per Component Floating Point Set If Not Equal.
   *
   * @code
   * sne/SETNEv dest, src0, src1
   *     dest.x = src0.x != src1.x ? 1.0 : 0.0;
   *     dest.y = src0.y != src1.y ? 1.0 : 0.0;
   *     dest.z = src0.z != src1.z ? 1.0 : 0.0;
   *     dest.w = src0.w != src1.w ? 1.0 : 0.0;
   * @endcode
   */
  kSne = 7,

  /**
   * Per Component Floating Point Fractional.
   *
   * @code
   * frc/FRACv dest, src0
   *     dest.x = src0.x - floor(src0.x);
   *     dest.y = src0.y - floor(src0.y);
   *     dest.z = src0.z - floor(src0.z);
   *     dest.w = src0.w - floor(src0.w);
   * @endcode
   */
  kFrc = 8,

  /**
   * Per Component Floating Point Truncate.
   *
   * @code
   * trunc/TRUNCv dest, src0
   *     dest.x = src0.x >= 0 ? floor(src0.x) : -floor(-src0.x);
   *     dest.y = src0.y >= 0 ? floor(src0.y) : -floor(-src0.y);
   *     dest.z = src0.z >= 0 ? floor(src0.z) : -floor(-src0.z);
   *     dest.w = src0.w >= 0 ? floor(src0.w) : -floor(-src0.w);
   * @endcode
   */
  kTrunc = 9,

  /**
   * Per Component Floating Point Floor.
   *
   * @code
   * floor/FLOORv dest, src0
   *     dest.x = floor(src0.x);
   *     dest.y = floor(src0.y);
   *     dest.z = floor(src0.z);
   *     dest.w = floor(src0.w);
   * @endcode
   */
  kFloor = 10,

  /**
   * Per Component Floating Point Multiply Add.
   *
   * @code
   * mad/MULADDv dest, src0, src1, src2
   *     dest.x = src0.x * src1.x + src2.x;
   *     dest.y = src0.y * src1.y + src2.y;
   *     dest.z = src0.z * src1.z + src2.z;
   *     dest.w = src0.w * src1.w + src2.w;
   * @endcode
   *
   * An early reference model computes this fused, rounding to single precision only after the
   * addition.
   */
  kMad = 11,

  /**
   * Per Component Floating Point Conditional Move If Equal.
   *
   * @code
   * cndeq/CNDEv dest, src0, src1, src2
   *     dest.x = src0.x == 0.0 ? src1.x : src2.x;
   *     dest.y = src0.y == 0.0 ? src1.y : src2.y;
   *     dest.z = src0.z == 0.0 ? src1.z : src2.z;
   *     dest.w = src0.w == 0.0 ? src1.w : src2.w;
   * @endcode
   */
  kCndEq = 12,

  /**
   * Per Component Floating Point Conditional Move If Greater Than Or Equal.
   *
   * @code
   * cndge/CNDGTEv dest, src0, src1, src2
   *     dest.x = src0.x >= 0.0 ? src1.x : src2.x;
   *     dest.y = src0.y >= 0.0 ? src1.y : src2.y;
   *     dest.z = src0.z >= 0.0 ? src1.z : src2.z;
   *     dest.w = src0.w >= 0.0 ? src1.w : src2.w;
   * @endcode
   */
  kCndGe = 13,

  /**
   * Per Component Floating Point Conditional Move If Greater Than.
   *
   * @code
   * cndgt/CNDGTv dest, src0, src1, src2
   *     dest.x = src0.x > 0.0 ? src1.x : src2.x;
   *     dest.y = src0.y > 0.0 ? src1.y : src2.y;
   *     dest.z = src0.z > 0.0 ? src1.z : src2.z;
   *     dest.w = src0.w > 0.0 ? src1.w : src2.w;
   * @endcode
   */
  kCndGt = 14,

  /**
   * 4 Element Dot Product.
   *
   * @code
   * dp4/DOT4v dest, src0, src1
   *     dest.xyzw = src0.x * src1.x + src0.y * src1.y + src0.z * src1.z +
   *                 src0.w * src1.w;
   * @endcode
   */
  kDp4 = 15,

  /**
   * 3 Element Dot Product.
   *
   * @code
   * dp3/DOT3v dest, src0, src1
   *     dest.xyzw = src0.x * src1.x + src0.y * src1.y + src0.z * src1.z;
   * @endcode
   */
  kDp3 = 16,

  /**
   * 2 Element Dot Product and Add.
   *
   * @code
   * dp2add/DOT2ADDv dest, src0, src1, src2
   *     dest.xyzw = src0.x * src1.x + src0.y * src1.y + src2.x;
   * @endcode
   *
   * An early reference model implements this as src0.x * src1.x + src0.z * src1.z + src2.y, which
   * is outdated and unreliable. The order above is correct, as Shader Model 2 dp2add translates
   * into this instruction without additional swizzling.
   */
  kDp2Add = 17,

  /**
   * Cube Map.
   *
   * @code
   * cube/CUBEv dest, src0, src1
   *     dest.x = T cube coordinate;
   *     dest.y = S cube coordinate;
   *     dest.z = 2.0 * major axis;
   *     dest.w = FaceID;
   * @endcode
   *
   * The coordinates are computed as follows.
   * @code
   *     if (abs(z) >= abs(x) && abs(z) >= abs(y)) {
   *       tc = -y;
   *       sc = z < 0.0 ? -x : x;
   *       ma = 2.0 * z;
   *       id = z < 0.0 ? 5.0 : 4.0;
   *     } else if (abs(y) >= abs(x)) {
   *       tc = y < 0.0 ? -z : z;
   *       sc = x;
   *       ma = 2.0 * y;
   *       id = y < 0.0 ? 3.0 : 2.0;
   *     } else {
   *       tc = -y;
   *       sc = x < 0.0 ? z : -z;
   *       ma = 2.0 * x;
   *       id = x < 0.0 ? 1.0 : 0.0;
   *     }
   * @endcode
   *
   * Expects src0.zzxy and src1.yxzz swizzles. FaceID is D3DCUBEMAP_FACES. Used like the following.
   * @code
   *     cube r0, source.zzxy, source.yxz
   *     rcp r0.z, r0_abs.z
   *     mad r0.xy, r0, r0.zzzw, 1.5f
   *     tfetchCube r0, r0.yxw, tf0
   * @endcode
   *
   * tfetchCube does not accept the ST as if the texture was a 2D array in XY exactly, so to get
   * texture array ST, 1 must be subtracted from its XY inputs. The 1.5 constant is designed so that
   * the output face coordinate range is {1.0 <= x < 2.0}, which keeps the upper mantissa bits
   * constant throughout the output range.
   */
  kCube = 18,

  /**
   * 4 Element Maximum.
   *
   * @code
   * max4/MAX4v dest, src0
   *     if (src0.x > src0.y && src0.x > src0.z && src0.x > src0.w) {
   *       dest.xyzw = src0.x;
   *     } else if (src0.y > src0.z && src0.y > src0.w) {
   *       dest.xyzw = src0.y;
   *     } else if (src0.z > src0.w) {
   *       dest.xyzw = src0.z;
   *     } else {
   *       dest.xyzw = src0.w;
   *     }
   * @endcode
   *
   * The interpreter compares with >= in the order xyzw, and the translators take the maximum of
   * the components with max.
   */
  kMax4 = 19,

  /**
   * Floating Point Predicate Counter Increment If Equal.
   *
   * @code
   * setp_eq_push/PRED_SETE_PUSHv dest, src0, src1
   *     if (src0.w == 0.0 && src1.w == 0.0) {
   *       p0 = 1;
   *     } else {
   *       p0 = 0;
   *     }
   *     if (src0.x == 0.0 && src1.x == 0.0) {
   *       dest.xyzw = 0.0;
   *     } else {
   *       dest.xyzw = src0.x + 1.0;
   *     }
   * @endcode
   */
  kSetpEqPush = 20,

  /**
   * Floating Point Predicate Counter Increment If Not Equal.
   *
   * @code
   * setp_ne_push/PRED_SETNE_PUSHv dest, src0, src1
   *     if (src0.w == 0.0 && src1.w != 0.0) {
   *       p0 = 1;
   *     } else {
   *       p0 = 0;
   *     }
   *     if (src0.x == 0.0 && src1.x != 0.0) {
   *       dest.xyzw = 0.0;
   *     } else {
   *       dest.xyzw = src0.x + 1.0;
   *     }
   * @endcode
   */
  kSetpNePush = 21,

  /**
   * Floating Point Predicate Counter Increment If Greater Than.
   *
   * @code
   * setp_gt_push/PRED_SETGT_PUSHv dest, src0, src1
   *     if (src0.w == 0.0 && src1.w > 0.0) {
   *       p0 = 1;
   *     } else {
   *       p0 = 0;
   *     }
   *     if (src0.x == 0.0 && src1.x > 0.0) {
   *       dest.xyzw = 0.0;
   *     } else {
   *       dest.xyzw = src0.x + 1.0;
   *     }
   * @endcode
   */
  kSetpGtPush = 22,

  /**
   * Floating Point Predicate Counter Increment If Greater Than Or Equal.
   *
   * @code
   * setp_ge_push/PRED_SETGTE_PUSHv dest, src0, src1
   *     if (src0.w == 0.0 && src1.w >= 0.0) {
   *       p0 = 1;
   *     } else {
   *       p0 = 0;
   *     }
   *     if (src0.x == 0.0 && src1.x >= 0.0) {
   *       dest.xyzw = 0.0;
   *     } else {
   *       dest.xyzw = src0.x + 1.0;
   *     }
   * @endcode
   */
  kSetpGePush = 23,

  /**
   * Floating Point Pixel Kill If Equal.
   *
   * @code
   * kill_eq/KILLEv dest, src0, src1
   *     if (src0.x == src1.x ||
   *         src0.y == src1.y ||
   *         src0.z == src1.z ||
   *         src0.w == src1.w) {
   *       dest.xyzw = 1.0;
   *       discard;
   *     } else {
   *       dest.xyzw = 0.0;
   *     }
   * @endcode
   */
  kKillEq = 24,

  /**
   * Floating Point Pixel Kill If Greater Than.
   *
   * @code
   * kill_gt/KILLGTv dest, src0, src1
   *     if (src0.x > src1.x ||
   *         src0.y > src1.y ||
   *         src0.z > src1.z ||
   *         src0.w > src1.w) {
   *       dest.xyzw = 1.0;
   *       discard;
   *     } else {
   *       dest.xyzw = 0.0;
   *     }
   * @endcode
   */
  kKillGt = 25,

  /**
   * Floating Point Pixel Kill If Greater Than Or Equal.
   *
   * @code
   * kill_ge/KILLGTEv dest, src0, src1
   *     if (src0.x >= src1.x ||
   *         src0.y >= src1.y ||
   *         src0.z >= src1.z ||
   *         src0.w >= src1.w) {
   *       dest.xyzw = 1.0;
   *       discard;
   *     } else {
   *       dest.xyzw = 0.0;
   *     }
   * @endcode
   */
  kKillGe = 26,

  /**
   * Floating Point Pixel Kill If Not Equal.
   *
   * @code
   * kill_ne/KILLNEv dest, src0, src1
   *     if (src0.x != src1.x ||
   *         src0.y != src1.y ||
   *         src0.z != src1.z ||
   *         src0.w != src1.w) {
   *       dest.xyzw = 1.0;
   *       discard;
   *     } else {
   *       dest.xyzw = 0.0;
   *     }
   * @endcode
   */
  kKillNe = 27,

  /**
   * dst/DSTv dest, src0, src1
   *
   * @code
   *     dest.x = 1.0;
   *     dest.y = src0.y * src1.y;
   *     dest.z = src0.z;
   *     dest.w = src1.w;
   * @endcode
   */
  kDst = 28,

  /**
   * Per Component Floating Point Maximum with Copy To Integer in AR.
   *
   * This is a combined max + mova/MOVAv.
   * @code
   * maxa dest, src0, src1
   *     a0 = (int)clamp(floor(src0.w + 0.5), -256.0, 255.0);
   *     dest.x = src0.x >= src1.x ? src0.x : src1.x;
   *     dest.y = src0.y >= src1.y ? src0.y : src1.y;
   *     dest.z = src0.z >= src1.z ? src0.z : src1.z;
   *     dest.w = src0.w >= src1.w ? src0.w : src1.w;
   * @endcode
   *
   * The clamp may instead be the following, which differs only for NaN. Using NaN as an address
   * would be unusual.
   * @code
   * if (!(SQResultF >= -256.0)) {
   *   SQResultF = -256.0;
   * }
   * if (SQResultF > 255.0) {
   *   SQResultF = 255.0;
   * }
   * @endcode
   */
  kMaxA = 29,
};

/** Static description of a vector ALU opcode. */
struct AluVectorOpcodeInfo {
  const char* name; /**< Lowercase mnemonic of the opcode. */
  /** Bit mask of the X, Y, Z and W components read from each of the 3 sources, X in bit 0. */
  u32 operand_components_used[3];
  /** State the operation changes besides its destination, as AluOpChangedState bits. */
  AluOpChangedState changed_state;

  /**
   * Counts the source operands the opcode reads.
   *
   * @return Operand count, from 0 to 3.
   */
  u32 GetOperandCount() const {
    if (!operand_components_used[2]) {
      if (!operand_components_used[1]) {
        if (!operand_components_used[0]) {
          return 0;
        }
        return 1;
      }
      return 2;
    }
    return 3;
  }
};

/** Indexed by the 5 bit vector opcode, 32 entries. */
extern REX_API const AluVectorOpcodeInfo kAluVectorOpcodeInfos[32];

/**
 * Looks up the description of a vector ALU opcode.
 *
 * @param opcode Vector opcode.
 * @return Description of the opcode.
 */
inline const AluVectorOpcodeInfo& GetAluVectorOpcodeInfo(AluVectorOpcode opcode) {
  REX_ASSERT(static_cast<u32>(opcode) < rex::CountOf(kAluVectorOpcodeInfos));
  return kAluVectorOpcodeInfos[static_cast<u32>(opcode)];
}

/**
 * Returns the components of a source operand needed by the instruction when executed with the given
 * write mask, which therefore cannot be thrown away or be undefined in translation. For per
 * component operations only the components in the write mask are needed, but some instructions have
 * special behavior for some components. kDst is per component, but not all components are used, and
 * operand_components_used filters out the unused ones.
 *
 * @param vector_opcode Vector opcode of the instruction.
 * @param src_index 1 based source operand index.
 * @param used_result_components Write mask of the used result components.
 * @return Mask of the needed source components.
 */
inline u32 GetAluVectorOpNeededSourceComponents(AluVectorOpcode vector_opcode, u32 src_index,
                                                u32 used_result_components) {
  REX_ASSERT(src_index != 0);
  REX_ASSERT((used_result_components & ~static_cast<u32>(0b1111)) == 0);
  u32 components = used_result_components;
  switch (vector_opcode) {
    case AluVectorOpcode::kDp4:
    case AluVectorOpcode::kMax4:
      components = used_result_components ? 0b1111 : 0;
      break;
    case AluVectorOpcode::kDp3:
      components = used_result_components ? 0b0111 : 0;
      break;
    case AluVectorOpcode::kDp2Add:
      components = used_result_components ? (src_index == 3 ? 0b0001 : 0b0011) : 0;
      break;
    case AluVectorOpcode::kCube:
      components = used_result_components ? 0b1111 : 0;
      break;
    case AluVectorOpcode::kSetpEqPush:
    case AluVectorOpcode::kSetpNePush:
    case AluVectorOpcode::kSetpGtPush:
    case AluVectorOpcode::kSetpGePush:
      components = used_result_components ? 0b1001 : 0b1000;
      break;
    case AluVectorOpcode::kKillEq:
    case AluVectorOpcode::kKillGt:
    case AluVectorOpcode::kKillGe:
    case AluVectorOpcode::kKillNe:
      components = 0b1111;
      break;
    case AluVectorOpcode::kMaxA:
      if (src_index == 1) {
        components |= 0b1000;
      }
      break;
    default:
      break;
  }
  return components & GetAluVectorOpcodeInfo(vector_opcode).operand_components_used[src_index - 1];
}

/** eM# (kExportData) register count. */
constexpr u32 kMaxMemExportElementCount = 5;

/**
 * Destination of an export ALU instruction. Vertex and pixel shaders use different subsets of the
 * values.
 */
enum class ExportRegister : u8 {
  /** @name Vertex shader interpolators 0 to 15, export registers 0 to 15. */
  /** @{ */
  kVSInterpolator0 = 0,
  kVSInterpolator1,
  kVSInterpolator2,
  kVSInterpolator3,
  kVSInterpolator4,
  kVSInterpolator5,
  kVSInterpolator6,
  kVSInterpolator7,
  kVSInterpolator8,
  kVSInterpolator9,
  kVSInterpolator10,
  kVSInterpolator11,
  kVSInterpolator12,
  kVSInterpolator13,
  kVSInterpolator14,
  kVSInterpolator15,
  /** @} */

  /** Vertex shader position output, mapped to the position storage target by the translator. */
  kVSPosition = 62,

  /**
   * Vertex shader misc export (USE_VTX_POINT_SIZE, USE_VTX_EDGE_FLAG and USE_VTX_KILL_FLAG on
   * R6xx/R7xx).
   * - X is PSIZE (gl_PointSize). It is the full width and height of the point sprite, not half, as
   *   gl_PointSize goes directly to oPts.x. It is clamped to PA_SU_POINT_MINMAX as a signed integer
   *   in rasterization.
   *   - -NaN gives min.
   *   - -Infinity gives min.
   *   - -Normal gives min.
   *   - -0 (0x80000000, the smallest signed integer) gives min.
   *   - +0 gives min.
   *   - +Infinity gives max.
   *   - +NaN gives max.
   * - Y is EDGEFLAG (glEdgeFlag) for PrimitiveType::kPolygon wireframe or point drawing.
   * - Z is the KILLVERTEX flag, set for killing primitives based on the
   *   PA_CL_CLIP_CNTL::VTX_KILL_OR condition if bits 0:30 of this export value (the sign bit is
   *   ignored) are not zero.
   */
  kVSPointSizeEdgeFlagKillVertex = 63,

  /** @name Pixel shader color outputs for render targets 0 to 3. */
  /** @{ */
  kPSColor0 = 0,
  kPSColor1,
  kPSColor2,
  kPSColor3,
  /** @} */

  /** Exported in X. */
  kPSDepth = 61,

  /** Memory export address, index.?y?? * 0100 + MemExportStream.xyzw. */
  kExportAddress = 32,
  /** @name Memory export values for texels [index+0] to [index+4], 1 register each. */
  /** @{ */
  kExportData0 = 33,
  kExportData1,
  kExportData2,
  kExportData3,
  kExportData4,
  /** @} */
};

/** ALU instruction of 3 dwords holding a vector operation and a scalar operation. */
struct alignas(u32) AluInstruction {
  /**
   * Reports whether data is exported, rather than written to local registers.
   *
   * @return True when the instruction exports.
   */
  bool IsExport() const { return data_.export_data == 1; }

  bool IsPredicated() const { return data_.is_predicated; }
  bool GetPredicateCondition() const { return data_.pred_condition == 1; }

  bool GetAbsConstants() const { return data_.abs_constants == 1; }
  bool IsConst0Addressed() const { return data_.const_0_rel_abs == 1; }
  bool IsConst1Addressed() const { return data_.const_1_rel_abs == 1; }
  /**
   * Reports whether relatively addressed constants are indexed by the address register a0 rather
   * than the loop counter aL.
   *
   * @return True when the address register is used.
   */
  bool IsConstAddressRegisterRelative() const { return data_.const_address_register_relative == 1; }

  AluVectorOpcode GetVectorOpcode() const { return data_.vector_opc; }
  u32 GetVectorWriteMask() const { return data_.vector_write_mask; }
  u32 GetVectorDest() const { return data_.vector_dest; }
  bool IsVectorDestRelative() const { return data_.vector_dest_rel == 1; }
  bool GetVectorClamp() const { return data_.vector_clamp == 1; }

  AluScalarOpcode GetScalarOpcode() const { return data_.scalar_opc; }
  u32 GetScalarWriteMask() const { return data_.scalar_write_mask; }
  u32 GetScalarDest() const { return data_.scalar_dest; }
  bool IsScalarDestRelative() const { return data_.scalar_dest_rel == 1; }
  bool GetScalarClamp() const { return data_.scalar_clamp == 1; }

  static constexpr u32 GetSrcTempReg(u32 src_reg) { return src_reg & 0x3F; }
  static constexpr bool IsSrcTempRelative(u32 src_reg) { return (src_reg & 0x40) != 0; }
  /**
   * Reports whether a temporary register source takes the absolute value.
   *
   * @param src_reg Packed temporary register from `src_reg`.
   * @return True when the absolute value is taken.
   */
  static constexpr bool IsSrcTempValueAbsolute(u32 src_reg) { return (src_reg & 0x80) != 0; }
  /**
   * Returns the full register index for constants, or the packed structure for temporary registers
   * (unpack using GetSrcTempReg, IsSrcTempRelative and IsSrcTempValueAbsolute).
   *
   * @param i 1 based source operand index.
   * @return Register index or packed temporary register.
   */
  u32 GetSrcReg(size_t i) const {
    switch (i) {
      case 1:
        return data_.src1_reg;
      case 2:
        return data_.src2_reg;
      case 3:
        return data_.src3_reg;
      default:
        REX_UNHANDLED_CASE(i);
        return 0;
    }
  }
  /**
   * Reports whether a source operand is a temporary register rather than a constant.
   *
   * @param i 1 based source operand index.
   * @return True when the operand is a temporary register.
   */
  bool IsSrcTemp(size_t i) const {
    switch (i) {
      case 1:
        return static_cast<bool>(data_.src1_sel);
      case 2:
        return static_cast<bool>(data_.src2_sel);
      case 3:
        return static_cast<bool>(data_.src3_sel);
      default:
        REX_UNHANDLED_CASE(i);
        return 0;
    }
  }
  /**
   * Reports whether a constant operand uses relative addressing. Whether the operand is a constant
   * is disregarded, so the scope is limited to parsing the structure layout.
   *
   * Operand index 3 is the constant operand in mulsc, addsc and subsc. Its addressing
   * mode is taken the same way as for the third vector operand, from const_0_rel_abs if there are
   * no constant vector operands or from const_1_rel_abs if there is at least one. Taking it
   * unconditionally from const_1_rel_abs gives different microcode when the disassembly is
   * assembled again.
   *
   * When 3 constants are used in 1 instruction, the second and third constant must both be non
   * relative or both be relative. const_0_rel_abs applies when no previous operand is a constant
   * and const_1_rel_abs otherwise.
   *
   * @param i 1 based source operand index.
   * @return True when the constant operand is relatively addressed.
   */
  bool IsSrcConstAddressed(size_t i) const {
    switch (i) {
      case 1:
        return static_cast<bool>(data_.const_0_rel_abs);
      case 2:
        return static_cast<bool>(IsSrcTemp(1) ? data_.const_0_rel_abs : data_.const_1_rel_abs);
      case 3:
        return static_cast<bool>((IsSrcTemp(1) && IsSrcTemp(2)) ? data_.const_0_rel_abs
                                                                : data_.const_1_rel_abs);
      default:
        REX_UNHANDLED_CASE(i);
        return false;
    }
  }
  /**
   * Returns the component relative swizzle of a source operand, 2 bits per component.
   *
   * @param i 1 based source operand index.
   * @return Packed swizzle.
   */
  u32 GetSrcSwizzle(size_t i) const {
    switch (i) {
      case 1:
        return data_.src1_swiz;
      case 2:
        return data_.src2_swiz;
      case 3:
        return data_.src3_swiz;
      default:
        REX_UNHANDLED_CASE(i);
        return 0;
    }
  }
  /**
   * Reports whether a source operand is negated.
   *
   * @param i 1 based source operand index.
   * @return True when the operand is negated.
   */
  bool IsSrcNegated(size_t i) const {
    switch (i) {
      case 1:
        return data_.src1_reg_negate == 1;
      case 2:
        return data_.src2_reg_negate == 1;
      case 3:
        return data_.src3_reg_negate == 1;
      default:
        REX_UNHANDLED_CASE(i);
        return 0;
    }
  }

  /**
   * Returns the temporary register of the register operand of a scalar operation that takes a
   * constant and a register, rebuilt from the opcode, the third source select and the third source
   * swizzle.
   *
   * @return Temporary register index.
   */
  u32 GetScalarConstRegOpSrcTempReg() const {
    return (static_cast<u32>(data_.scalar_opc) & 1) | (data_.src3_sel << 1) |
           (data_.src3_swiz & 0x3C);
  }

  /**
   * Returns the absolute component index calculated from the relative swizzle in an ALU
   * instruction.
   *
   * @param swizzle Component relative swizzle.
   * @param component_index Result component index.
   * @return Absolute component index.
   */
  static constexpr u32 GetSwizzledComponentIndex(u32 swizzle, u32 component_index) {
    return ((swizzle >> (2 * component_index)) + component_index) & 3;
  }

  /**
   * Returns the write mask of the vector operation result. An unused export component, like W of
   * the vertex shader misc register or YZW of pixel shader depth, must not be excluded, as
   * disassembly with constant 0 writes in the export may not reassemble. For example oPts.x000 is
   * assembled, but oPts.x00_ is not, even though W has no effect.
   *
   * @return Write mask of the vector result.
   */
  u32 GetVectorOpResultWriteMask() const {
    u32 mask = GetVectorWriteMask();
    if (IsExport()) {
      mask &= ~GetScalarWriteMask();
    }
    return mask;
  }
  /**
   * Returns the write mask of the scalar operation result. In an export it excludes the components
   * also in the vector write mask, which export constant 1.
   *
   * @return Write mask of the scalar result.
   */
  u32 GetScalarOpResultWriteMask() const {
    u32 mask = GetScalarWriteMask();
    if (IsExport()) {
      mask &= ~GetVectorWriteMask();
    }
    return mask;
  }
  /**
   * Returns the export components that receive constant 0, which are those neither operation writes
   * when the scalar destination is relative.
   *
   * @return Mask of components exporting 0, empty when the instruction is not an export.
   */
  u32 GetConstant0WriteMask() const {
    if (!IsExport() || !IsScalarDestRelative()) {
      return 0b0000;
    }
    return 0b1111 & ~(GetVectorWriteMask() | GetScalarWriteMask());
  }
  /**
   * Returns the export components that receive constant 1, which are those both operations write.
   *
   * @return Mask of components exporting 1, empty when the instruction is not an export.
   */
  u32 GetConstant1WriteMask() const {
    if (!IsExport()) {
      return 0b0000;
    }
    return GetVectorWriteMask() & GetScalarWriteMask();
  }

 private:
  struct Data {
    struct {
      /**
       * If exporting, both vector and scalar operations use the vector destination, which cannot be
       * relative in this case. If both operations exporting something have an empty write mask, the
       * assembler forces vector_dest to 0 (interpolator 0 or color 0) in the microcode.
       */
      u32 vector_dest : 6;
      u32 vector_dest_rel : 1;
      u32 abs_constants : 1;
      u32 scalar_dest : 6;
      u32 scalar_dest_rel : 1;
      /**
       * Exports have different write masking. Export is done to vector_dest by both the vector and
       * the scalar operation, and exports can write constant 0 and 1. For each component the result
       * is as follows.
       * - vector_write_mask 0, scalar_write_mask 0 is unchanged for scalar_dest_rel 0 and constant
       *   0 for scalar_dest_rel 1 (all components must be written).
       * - vector_write_mask 1, scalar_write_mask 0 is from the vector operation.
       * - vector_write_mask 0, scalar_write_mask 1 is from the scalar operation.
       * - vector_write_mask 1, scalar_write_mask 1 is constant 1.
       */
      u32 export_data : 1;
      u32 vector_write_mask : 4;
      u32 scalar_write_mask : 4;
      u32 vector_clamp : 1;
      u32 scalar_clamp : 1;
      AluScalarOpcode scalar_opc : 6;
    };
    struct {
      /** Swizzles are component relative. */
      u32 src3_swiz : 8;
      u32 src2_swiz : 8;
      u32 src1_swiz : 8;
      u32 src3_reg_negate : 1;
      u32 src2_reg_negate : 1;
      u32 src1_reg_negate : 1;
      u32 pred_condition : 1;
      u32 is_predicated : 1;
      /** Temporary registers can have only absolute and aL relative indices, not a0 relative. */
      u32 const_address_register_relative : 1;
      u32 const_1_rel_abs : 1;
      u32 const_0_rel_abs : 1;
    };
    struct {
      u32 src3_reg : 8;
      u32 src2_reg : 8;
      u32 src1_reg : 8;
      AluVectorOpcode vector_opc : 5;
      u32 src3_sel : 1;
      u32 src2_sel : 1;
      u32 src1_sel : 1;
    };
  };
  Data data_;
};
REX_STATIC_ASSERT_SIZE(AluInstruction, sizeof(u32) * 3);

}  // namespace rex::xenos::ucode
