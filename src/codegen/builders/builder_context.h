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
#include <string>
#include <string_view>

#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>

#include "mmio_analysis.h"

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
};

/**
 * @brief CSR (Control/Status Register) flush mode state.
 *
 * Tracks the current MXCSR configuration for floating-point operations.
 * - **Unknown** is the initial state and the state after a function call. The next FP/VMX
 *   instruction will emit a conditional mode check.
 * - **FPU** preserves denormals (flush-to-zero disabled). Used by scalar
 *   floating-point instructions (fadd, fmul and others)
 * - **VMX** flushes denormals to zero. Used by vector floating-point
 *   instructions (vaddfp, vmaddfp and others)
 */
enum class CSRState { Unknown, FPU, VMX };

/**
 * @brief Context passed to instruction builders during code generation.
 */
struct BuilderContext {
  /** Raw output buffer for code generation. */
  std::string& out;

  /** Emission context (binary, config, graph). */
  const EmitContext& emitCtx;

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
  CSRState& csrState;

  /** Pointer to active jump table for bctr dispatch, or nullptr. */
  const JumpTable* activeJumpTable = nullptr;

  /** MMIO classification of the current function, or nullptr. */
  const MMIOClassification* mmio = nullptr;

  /**
   * Returns the recompiler configuration.
   *
   * @return The configuration.
   */
  const RecompilerConfig& config() const;

  /**
   * Returns the function graph.
   *
   * @return The graph.
   */
  const FunctionGraph& graph() const;

  /**
   * @brief Reports whether non-volatiles may be localized in the current function.
   * False when the function shares registers with an intra-function bl partner.
   * An SEH funclet inherits its owner's live registers through ctx, so it never localizes them.
   *
   * @return True when localizing is allowed.
   */
  bool localizeNonVolatiles() const;

  /**
   * @brief Returns the expression for general purpose register access.
   * @param index Register index (0-31)
   * @return "rN" for local variables, "ctx.rN" for context access
   */
  std::string r(size_t index);

  /**
   * @brief Returns the expression for floating point register access.
   * @param index Register index (0-31)
   * @return "fN" for local variables, "ctx.fN" for context access
   */
  std::string f(size_t index);

  /**
   * @brief Returns the expression for vector register access.
   * @param index Register index (0-127, extended VMX128)
   * @return "vN" for local variables, "ctx.vN" for context access
   */
  std::string v(size_t index);

  /**
   * @brief Returns the expression for condition register field access.
   * @param index CR field index (0-7)
   * @return "crN" for local variables, "ctx.crN" for context access
   */
  std::string cr(size_t index);

  /**
   * Returns the count register expression.
   *
   * @return "ctr" or "ctx.ctr".
   */
  const char* ctr();

  /**
   * Returns the XER register expression.
   *
   * @return "xer" or "ctx.xer".
   */
  const char* xer();

  /**
   * Returns the reservation register expression, used by lwarx and stwcx.
   *
   * @return The reservation expression.
   */
  const char* reserved();

  /**
   * Returns the scalar temporary expression.
   *
   * @return "temp".
   */
  const char* temp();

  /**
   * Returns the vector temporary expression.
   *
   * @return "vTemp".
   */
  const char* v_temp();

  /**
   * Returns the setjmp environment expression.
   *
   * @return "env".
   */
  const char* env();

  /**
   * Returns the effective address temporary expression.
   *
   * @return "ea".
   */
  const char* ea();

  /**
   * @brief Print formatted text to the output buffer (no newline).
   * @tparam Args Format argument types (deduced)
   * @param fmt Format string with {} placeholders
   * @param args Values to substitute into placeholders
   */
  template <class... Args>
  void print(std::format_string<Args...> fmt, Args&&... args) {
    std::vformat_to(std::back_inserter(out), fmt.get(), std::make_format_args(args...));
  }

  /**
   * @brief Print formatted text to output buffer with newline.
   * @tparam Args Format argument types (deduced)
   * @param fmt Format string with {} placeholders
   * @param args Values to substitute into placeholders
   */
  template <class... Args>
  void println(std::format_string<Args...> fmt, Args&&... args) {
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
   * @brief Find pre-resolved call target for an instruction site.
   * @param site Address of the call/branch instruction
   * @return Pointer to CallTarget if found, nullptr otherwise
   *
   * Searches the FunctionNode's calls and tailCalls for a matching site.
   */
  const CallTarget* findCallTarget(u32 site) const;

  /**
   * @brief Emit C++ code for a function call.
   * @param address Target function address
   *
   * Uses pre-resolved CallTarget from FunctionNode when available.
   * Falls back to symbol lookup for backward compatibility.
   * Handles special cases like setjmp/longjmp and __restgprlr_N functions.
   * Save and restore helpers are elided under the global non volatile setting and not the
   * function's, because the helper bodies are emitted under it and would write zeros over the
   * caller's frame once spilled to locals. An SEH funclet runs on its owner's frame, so the
   * localized registers are copied through ctx before the call and back after it.
   */
  void emit_function_call(u32 address);

  /**
   * @brief Emit C++ code for a conditional branch.
   * @param not_ If true, invert the condition
   * @param cond Condition field name ("eq", "lt", "gt")
   *
   * Emits either `goto loc_X` for intra-function branches or a function
   * call for inter-function branches.
   */
  void emit_conditional_branch(bool not_, std::string_view cond);

  /**
   * @brief Emit CSR flush mode change if needed.
   * @param enable true for VMX mode (flush-to-zero), false for FPU mode
   */
  void emit_set_flush_mode(bool enable);

  /** Emit mid-asm hook if configured for current address. */
  void emit_mid_asm_hook();

  /**
   * Reports whether a mid asm hook exists for the current address.
   *
   * @return True when a hook exists.
   */
  bool has_mid_asm_hook() const;

  /** Clear active jump table pointer (used after processing a switch). */
  void reset_switch_table() { activeJumpTable = nullptr; }

  /**
   * @brief Emit binary float vector operation: vD = op(vA, vB)
   * @param simd_op The SIMDE function name (e.g., "add_ps", "sub_ps", "mul_ps")
   *
   * Emits `simde_mm_store_ps(vD.f32, simde_mm_OP(load(vA.f32), load(vB.f32)));`
   * Uses operands[0]=vD, operands[1]=vA, operands[2]=vB from current instruction.
   */
  void emit_vec_fp_binary(const char* simd_op);

  /**
   * @brief Emit unary float vector operation: vD = op(vA)
   * @param simd_expr SIMDE expression for the operation (will be wrapped in store)
   *
   * Use when the operation is more complex than a single function call.
   * Emits `simde_mm_store_ps(vD.f32, EXPR);`
   * Uses operands[0]=vD, operands[1]=vA from current instruction.
   * Example `emit_vec_fp_unary_expr("simde_mm_sqrt_ps(simde_mm_load_ps({vA}.f32))")`
   */
  void emit_vec_fp_unary_expr(std::string_view simd_expr);

  /**
   * @brief Emit binary integer vector operation: vD = op(vA, vB)
   * @param simd_op The SIMDE function name (e.g., "add_epi16", "and_si128")
   * @param element_type The vector element type suffix (e.g., "u8", "s16", "u32")
   *
   * Emits `simde_mm_store_si128((simde__m128i*)vD.TYPE, simde_mm_OP(load(vA), load(vB)));`
   * Uses operands[0]=vD, operands[1]=vA, operands[2]=vB from current instruction.
   */
  void emit_vec_int_binary(const char* simd_op, const char* element_type);

  /**
   * @brief Emit binary integer vector operation with swapped operands: vD = op(vB, vA)
   * @param simd_op The SIMDE function name
   * @param element_type The vector element type suffix
   *
   * Same as emit_vec_int_binary but swaps vA and vB order (useful for andnot)
   */
  void emit_vec_int_binary_swapped(const char* simd_op, const char* element_type);

  /**
   * @brief Emit variable shift: vD = rex::ppc::simde_mm_{shift_dir}_{element_type}(vA, vB & mask)
   * @param shift_dir The shift direction ("sllv", "srlv", or "srav")
   * @param element_type The SIMDE element type suffix ("epi16")
   * @param mask_value Shift amount mask (e.g., 0xF for 16-bit)
   *
   * Uses the custom rex:: variable shift helpers (simde_mm_{sllv,srlv,srav}_epi16).
   * Uses operands[0]=vD, operands[1]=vA, operands[2]=vB from current instruction.
   */
  void emit_vec_var_shift(const char* shift_dir, const char* element_type, u32 mask_value);

  /**
   * @brief Emit load with D-form addressing: rD = LOAD(rA + offset)
   * @param load_macro The load macro name (e.g., "REX_LOAD_U8", "REX_LOAD_U32")
   * @param dest_type The destination type suffix (e.g., "u64", "s64")
   *
   * Uses operands[0]=rD, operands[1]=offset (D), operands[2]=rA from current instruction.
   * If rA (operands[2]) is 0, omits the base register addition.
   */
  void emit_load_d_form(const char* load_macro, const char* dest_type);

  /**
   * @brief Emit load with X-form addressing: rD = LOAD(rA + rB)
   * @param load_macro The load macro name
   * @param dest_type The destination type suffix
   *
   * Uses operands[0]=rD, operands[1]=rA, operands[2]=rB from current instruction.
   * If rA (operands[1]) is 0, omits the first register addition.
   */
  void emit_load_x_form(const char* load_macro, const char* dest_type);

  /**
   * @brief Emit store with D-form addressing: STORE(rA + offset, rS)
   * @param store_macro The store macro name (e.g., "REX_STORE_U8")
   * @param src_type The source type suffix (e.g., "u8", "u32")
   *
   * Uses operands[0]=rS, operands[1]=offset (D), operands[2]=rA from current instruction.
   */
  void emit_store_d_form(const char* store_macro, const char* src_type);

  /**
   * @brief Emit store with X-form addressing: STORE(rA + rB, rS)
   * @param store_macro The store macro name
   * @param src_type The source type suffix
   *
   * Uses operands[0]=rS, operands[1]=rA, operands[2]=rB from current instruction.
   */
  void emit_store_x_form(const char* store_macro, const char* src_type);
};
}  // namespace rex::codegen