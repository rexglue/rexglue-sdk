/**
 * @file        codegen/builders/builder_context.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>

#include "mmio_analysis.h"
#include "stack_forwarding.h"

struct ppc_insn;

namespace rex::codegen {

class FunctionNode;

struct RecompilerLocalVariables {
  bool ctr{};
  bool xer{};
  bool reserved{};
  bool cr[8]{};
  bool r[32]{};
  bool f[32]{};
  bool v[128]{};
  bool env{};
  bool temp{};
  bool v_temp{};
  bool ea{};
  u8 fwd{};
};

/**
 * Text of the lines that stand for the copy of every localized nonvolatile into ctx or back out of
 * it.
 */
inline constexpr std::string_view kSyncToCtx = "@sync_to_ctx";
inline constexpr std::string_view kSyncFromCtx = "@sync_from_ctx";

/**
 * CSR (Control/Status Register) flush mode state.
 *
 * Tracks the current MXCSR configuration for floating-point operations.
 * - **Unknown** is the initial state and the state after a function call. The next FP/VMX
 *   instruction will emit a conditional mode check.
 * - **FPU** preserves denormals (flush-to-zero disabled). Used by scalar
 *   floating-point instructions (fadd, fmul and others)
 * - **VMX** sets the host flush mode to the VSCR non Java bit. Used by vector floating-point
 *   instructions (vaddfp, vmaddfp and others)
 */
enum class CSRState : u8 { kUnknown, kFPU, kVMX };

/** Context passed to instruction builders during code generation. */
struct BuilderContext {
  /** Raw output buffer for code generation. */
  std::string& out;

  /** Emission context (binary, config, graph). */
  const EmitContext& emit_ctx;

  /** The function being recompiled (FunctionNode from graph). */
  const FunctionNode& fn;

  /** The decoded instruction being processed (opcode, operands, disassembly). */
  const ppc_insn& insn;

  /** Address of the current instruction in guest memory. */
  u32 base;

  /** Pointer to raw instruction data in the image. */
  const u32* data;

  /** Tracks which registers need local variable declarations. */
  RecompilerLocalVariables& locals;

  /** Current CSR state for flush mode (FPU vs VMX). */
  CSRState& csr_state;

  /** Pointer to active jump table for bctr dispatch, or null. */
  const JumpTable* active_jump_table = nullptr;

  /** MMIO classification of the current function, or null. */
  const MMIOClassification* mmio = nullptr;

  /** The decoded instruction that follows in the same block, or null when it may not be fused. */
  const ppc_insn* next = nullptr;

  /** How many instruction words the builder emitted code for. */
  u32 consumed = 1;

  /** True when the function wraps its body in an SEH scope. */
  bool seh_scoped = false;

  /** The stack slots stored in this basic block, or null when forwarding is off. */
  StackForwarding* forwarding = nullptr;

  /**
   * Marks the next instruction as emitted together with the current one.
   *
   * Prints its comment line and sets consumed to 2. Requires next to be non null.
   */
  void ConsumeNext();

  /**
   * Returns the recompiler configuration.
   *
   * @return The configuration.
   */
  const RecompilerConfig& GetConfig() const;

  /**
   * Returns the function graph.
   *
   * @return The graph.
   */
  const FunctionGraph& GetGraph() const;

  /**
   * Reports whether nonvolatiles may be localized in the current function.
   *
   * False when the function shares registers with an intra-function bl partner.
   * An SEH funclet inherits its owner's live registers through ctx, so it never localizes them.
   * A save or restore helper moves registers between ctx and the frame, so it never does either.
   *
   * @return True when localizing is allowed.
   */
  bool CanLocalizeNonVolatiles() const;

  /**
   * Returns the expression for general purpose register access.
   *
   * @param index Register index (0-31)
   * @return "rN" for local variables, "ctx.rN" for context access
   */
  std::string R(size_t index);

  /**
   * Returns the expression for floating point register access.
   *
   * @param index Register index (0-31)
   * @return "fN" for local variables, "ctx.fN" for context access
   */
  std::string F(size_t index);

  /**
   * Returns the expression for vector register access.
   *
   * @param index Register index (0-127, extended VMX128)
   * @return "vN" for local variables, "ctx.vN" for context access
   */
  std::string V(size_t index);

  /**
   * Returns the expression for condition register field access.
   *
   * @param index CR field index (0-7)
   * @return "crN" for local variables, "ctx.crN" for context access
   */
  std::string CR(size_t index);

  /**
   * Returns the count register expression.
   *
   * @return "ctr" or "ctx.ctr".
   */
  const char* CTR();

  /**
   * Returns the XER register expression.
   *
   * @return "xer" or "ctx.xer".
   */
  const char* XER();

  /**
   * Returns the reservation register expression, used by lwarx and stwcx.
   *
   * @return The reservation expression.
   */
  const char* Reserved();

  /**
   * Returns the scalar temporary expression.
   *
   * @return "temp".
   */
  const char* Temp();

  /**
   * Returns the vector temporary expression.
   *
   * @return "vTemp".
   */
  const char* VTemp();

  /**
   * Returns the setjmp environment expression.
   *
   * @return "env".
   */
  const char* Env();

  /**
   * Returns the effective address temporary expression.
   *
   * @return "ea".
   */
  const char* EA();

  /**
   * Prints formatted text to the output buffer (no newline).
   *
   * @tparam Args Format argument types (deduced)
   * @param fmt Format string with {} placeholders
   * @param args Values to substitute into placeholders
   */
  template <class... Args>
  void Print(std::format_string<Args...> fmt, Args&&... args) {
    std::vformat_to(std::back_inserter(out), fmt.get(), std::make_format_args(args...));
  }

  /**
   * Prints formatted text to the output buffer with a newline.
   *
   * @tparam Args Format argument types (deduced)
   * @param fmt Format string with {} placeholders
   * @param args Values to substitute into placeholders
   */
  template <class... Args>
  void Println(std::format_string<Args...> fmt, Args&&... args) {
    std::vformat_to(std::back_inserter(out), fmt.get(), std::make_format_args(args...));
    out += '\n';
  }

  /**
   * Maps a plain memory macro to the macro the current instruction needs.
   *
   * @param plain A REX_LOAD_ or REX_STORE_ macro name.
   * @return The REX_GPU_ or REX_APU_ macro for a proven 32 bit access, the REX_MM_ macro of the
   *         same width for any other proven or guarded access, otherwise plain.
   */
  std::string_view MMIOMacro(std::string_view plain) const;

  /**
   * Picks the direct stack form of an access macro for an instruction that addresses through r1.
   * It never routes to a register block, for accesses that are not classified.
   *
   * @param plain The REX_LOAD_* or REX_STORE_* macro.
   * @return The REX_STACK_* macro for an access through r1, otherwise plain.
   */
  std::string_view StackMacro(std::string_view plain) const;

  /**
   * Tells whether the memory instruction addresses through r1.
   *
   * @return True when the base register of the instruction is r1.
   */
  bool IsStackAccess() const;

  /**
   * Returns the pre resolved call target of an instruction site.
   *
   * Searches the FunctionNode's calls and tailCalls for a matching site.
   *
   * @param site Address of the call/branch instruction
   * @return The call target, or null when none matches.
   */
  const CallTarget* FindCallTarget(u32 site) const;

  /**
   * Emits C++ code for a function call.
   *
   * Uses pre-resolved CallTarget from FunctionNode when available.
   * Logs an error and emits REX_FATAL when the FunctionNode has no CallTarget.
   * Handles special cases like setjmp/longjmp and __restgprlr_N functions.
   * Helper bodies keep nonvolatiles in ctx, so a kept helper call saves and restores real values.
   * Save and restore helpers are elided in a function that localizes its nonvolatiles. An SEH
   * funclet runs on its owner's frame, so the localized registers are copied through ctx before
   * the call and back after it. A setjmp call copies them into ctx before the snapshot and back
   * out of ctx after the call, so a return through longjmp shows the values from the setjmp call.
   *
   * @param address Target function address
   */
  void EmitFunctionCall(u32 address);

  /**
   * Emits the call to a function.
   *
   * @param target_fn The function to call.
   */
  void EmitCallToFunction(const FunctionNode& target_fn);

  /**
   * Tells whether a jump out of the function can be a tail call. SEH scopes, a hook after the
   * instruction, register sharing and millicode targets keep the call and return form.
   *
   * @return True when no state of the current function outlives the jump.
   */
  bool CanTailCall() const;

  /**
   * Emits the tail call to the function a branch at the current address reaches.
   *
   * @param address The branch target.
   * @return True when a tail call was emitted, false when the call and return form is needed.
   */
  bool EmitTailCall(u32 address);

  /**
   * Emits C++ code for a conditional branch.
   *
   * Emits either `goto loc_X` for intra-function branches or a function
   * call for inter-function branches.
   *
   * @param negate If true, invert the condition
   * @param cond Condition field name ("eq", "lt", "gt")
   */
  void EmitConditionalBranch(bool negate, std::string_view cond);

  /**
   * Emits the CSR flush mode change if needed.
   *
   * @param enable true for VMX mode (flush follows the VSCR NJ bit), false for FPU mode
   */
  void EmitSetFlushMode(bool enable);

  /** Emits the mid asm hook if configured for the current address. */
  void EmitMidAsmHook();

  /**
   * Reports whether a mid asm hook exists for the current address.
   *
   * @return True when a hook exists.
   */
  bool HasMidAsmHook() const;

  /** Clears the active jump table pointer (used after processing a switch). */
  void ResetJumpTable() { active_jump_table = nullptr; }

  /**
   * Emits binary float vector operation `vD = op(vA, vB)`.
   *
   * Emits `simde_mm_store_ps(vD.f32, simde_mm_OP(load(vA.f32), load(vB.f32)));`
   * Uses operands[0]=vD, operands[1]=vA, operands[2]=vB from current instruction.
   *
   * @param simd_op The SIMDE function name (e.g., "add_ps", "sub_ps", "mul_ps")
   */
  void EmitVecFPBinary(const char* simd_op);

  /**
   * Emits unary float vector operation `vD = op(vA)`.
   *
   * Emits `simde_mm_store_ps(vD.f32, EXPR);`
   * Uses operands[0]=vD, operands[1]=vA from current instruction.
   * Example `EmitVecFPUnaryExpr("simde_mm_sqrt_ps(simde_mm_load_ps({vA}.f32))")`
   *
   * @param simd_expr SIMDE expression for the operation (will be wrapped in store)
   */
  void EmitVecFPUnaryExpr(std::string_view simd_expr);

  /**
   * Emits binary integer vector operation `vD = op(vA, vB)`.
   *
   * Emits `simde_mm_store_si128((simde__m128i*)vD.TYPE, simde_mm_OP(load(vA), load(vB)));`
   * Uses operands[0]=vD, operands[1]=vA, operands[2]=vB from current instruction.
   *
   * @param simd_op The SIMDE function name (e.g., "add_epi16", "and_si128")
   * @param element_type The vector element type suffix (e.g., "u8", "s16", "u32")
   */
  void EmitVecIntBinary(const char* simd_op, const char* element_type);

  /**
   * Emits binary integer vector operation with swapped operands `vD = op(vB, vA)`.
   *
   * Same as EmitVecIntBinary but swaps vA and vB order (useful for andnot)
   *
   * @param simd_op The SIMDE function name
   * @param element_type The vector element type suffix
   */
  void EmitVecIntBinarySwapped(const char* simd_op, const char* element_type);

  /**
   * Emits variable shift `vD = rex::ppc::simd::{function}(vA, vB & mask)`.
   *
   * Uses the rex::ppc::simd variable shift helpers.
   * Uses operands[0]=vD, operands[1]=vA, operands[2]=vB from current instruction.
   *
   * @param function The rex::ppc::simd shift function ("ShiftLeftLanes<u16>", ...)
   * @param element_type The SIMDE element type suffix ("epi16")
   * @param mask_value Shift amount mask (e.g., 0xF for 16-bit)
   */
  void EmitVecVarShift(const char* function, const char* element_type, u32 mask_value);

  /**
   * Holds the write of the current instruction as a forwardable stack slot.
   *
   * @param width The width of the store in bytes.
   * @return The local that holds the stored value, empty when the store is not a plain access
   *         through r1 or forwarding is off.
   */
  std::optional<std::string> HoldStackWrite(u8 width);

  /**
   * Finds the local that holds the value a load of the current instruction would read.
   *
   * @param width The width of the load in bytes.
   * @return The local, empty when no earlier store of the same slot is live.
   */
  std::optional<std::string> FindStackWrite(u8 width) const;

  /**
   * Emits load with D-form addressing `rD = LOAD(rA + offset)`.
   *
   * Uses operands[0]=rD, operands[1]=offset (D), operands[2]=rA from current instruction.
   * If rA (operands[2]) is 0, omits the base register addition.
   *
   * @param load_macro The load macro name (e.g., "REX_LOAD_U8", "REX_LOAD_U32")
   * @param dest_type The destination type suffix (e.g., "u64", "s64")
   */
  void EmitLoadDForm(const char* load_macro, const char* dest_type);

  /**
   * Emits load with X-form addressing `rD = LOAD(rA + rB)`.
   *
   * Uses operands[0]=rD, operands[1]=rA, operands[2]=rB from current instruction.
   * If rA (operands[1]) is 0, omits the first register addition.
   *
   * @param load_macro The load macro name
   * @param dest_type The destination type suffix
   */
  void EmitLoadXForm(const char* load_macro, const char* dest_type);

  /**
   * Emits store with D-form addressing `STORE(rA + offset, rS)`.
   *
   * Uses operands[0]=rS, operands[1]=offset (D), operands[2]=rA from current instruction.
   *
   * @param store_macro The store macro name (e.g., "REX_STORE_U8")
   * @param src_type The source type suffix (e.g., "u8", "u32")
   */
  void EmitStoreDForm(const char* store_macro, const char* src_type);

  /**
   * Emits store with X-form addressing `STORE(rA + rB, rS)`.
   *
   * Uses operands[0]=rS, operands[1]=rA, operands[2]=rB from current instruction.
   *
   * @param store_macro The store macro name
   * @param src_type The source type suffix
   */
  void EmitStoreXForm(const char* store_macro, const char* src_type);
};

/**
 * Returns the CRRegister member name of a BI field bit.
 *
 * @param bi BI field bit index, 0 to 3.
 * @return "lt", "gt", "eq" or "so".
 */
const char* GetCRBitName(u32 bi);

/**
 * Reports whether the current instruction is a record form (has '.' suffix).
 *
 * Rc forms update CR0 from the result.
 *
 * @param insn The ppc_insn being processed
 * @return True when the instruction name contains '.' (record form)
 */
bool IsRecordForm(const ppc_insn& insn);

/**
 * Emits the CR0 comparison for Rc forms.
 *
 * Rc forms (the '.' suffix, such as add. and and.)
 * update CR0 based on the result compared to zero.
 *   CR0[LT] = result < 0
 *   CR0[GT] = result > 0
 *   CR0[EQ] = result == 0
 *   CR0[SO] = XER[SO]
 *
 * @param ctx The builder context containing the instruction being processed
 */
void EmitRecordFormCompare(BuilderContext& ctx);

}  // namespace rex::codegen