/**
 * @file        rex/codegen/builder_context.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <fmt/core.h>

#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>

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

  /// Tracks which GPRs contain MMIO base addresses (bit N = rN is MMIO base)
  /// Set when lis loads a value with upper 16 bits >= 0x7F00 (address >= 0x7F000000)
  /// or when oris sets upper bits >= 0xC800 (address >= 0xC8000000)
  uint32_t mmio_base_regs{0};

  void set_mmio_base(size_t reg) {
    if (reg < 32)
      mmio_base_regs |= (1u << reg);
  }
  void clear_mmio_base(size_t reg) {
    if (reg < 32)
      mmio_base_regs &= ~(1u << reg);
  }
  bool is_mmio_base(size_t reg) const { return reg < 32 && (mmio_base_regs & (1u << reg)); }
};

/**
 * @brief CSR (Control/Status Register) flush mode state.
 *
 * Tracks the current MXCSR configuration for floating-point operations:
 * - **Unknown**: Initial state or after function call. Next FP/VMX instruction
 *   will emit a conditional mode check.
 * - **FPU**: Denormals preserved (flush-to-zero disabled). Used by scalar
 *   floating-point instructions (fadd, fmul, etc.)
 * - **VMX**: Denormals flushed to zero. Used by vector floating-point
 *   instructions (vaddfp, vmaddfp, etc.)
 */
enum class CSRState { Unknown, FPU, VMX };

/**
 * @brief Context passed to instruction builders during code generation.
 */
struct BuilderContext {
  /// Raw output buffer for code generation
  std::string& out;

  /// Emission context (binary, config, graph, resolver)
  const EmitContext& emitCtx;

  /// The function currently being recompiled (FunctionNode from graph)
  const FunctionNode& fn;

  /// The decoded instruction being processed (opcode, operands, disassembly)
  const ppc_insn& insn;

  /// Address of the current instruction in guest memory
  uint32_t base;

  /// Pointer to raw instruction data in the image
  const uint32_t* data;

  /// Tracks which registers need local variable declarations
  RecompilerLocalVariables& locals;

  /// Current CSR state for flush mode (FPU vs VMX)
  CSRState& csrState;

  /// Pointer to active jump table for bctr dispatch, or nullptr
  const JumpTable* activeJumpTable = nullptr;

  /// Get the recompiler configuration
  const RecompilerConfig& config() const;

  /// Get the function graph (single source of truth for function info)
  const FunctionGraph& graph() const;

  //=========================================================================
  // Register Accessors
  //=========================================================================

  /**
   * @brief Get expression for general-purpose register access.
   * @param index Register index (0-31)
   * @return "rN" for local variables, "ctx.rN" for context access
   */
  std::string r(size_t index);

  /**
   * @brief Get expression for floating-point register access.
   * @param index Register index (0-31)
   * @return "fN" for local variables, "ctx.fN" for context access
   */
  std::string f(size_t index);

  /**
   * @brief Get expression for vector register access.
   * @param index Register index (0-127, Xbox 360 extended VMX128)
   * @return "vN" for local variables, "ctx.vN" for context access
   */
  std::string v(size_t index);

  /**
   * @brief Get expression for condition register field access.
   * @param index CR field index (0-7)
   * @return "crN" for local variables, "ctx.crN" for context access
   */
  std::string cr(size_t index);

  /// Get expression for count register ("ctr" or "ctx.ctr")
  const char* ctr();

  /// Get expression for XER register ("xer" or "ctx.xer")
  const char* xer();

  /// Get expression for reservation register (used by lwarx/stwcx)
  const char* reserved();

  /// Get expression for scalar temporary variable (always "temp")
  const char* temp();

  /// Get expression for vector temporary variable (always "vTemp")
  const char* v_temp();

  /// Get expression for setjmp environment storage (always "env")
  const char* env();

  /// Get expression for effective address temporary (always "ea")
  const char* ea();

  //=========================================================================
  // Output Helpers
  //=========================================================================

  /**
   * @brief Print formatted text to the output buffer (no newline).
   * @tparam Args Format argument types (deduced)
   * @param fmt Format string with {} placeholders
   * @param args Values to substitute into placeholders
   */
  template <class... Args>
  void print(fmt::format_string<Args...> fmt, Args&&... args) {
    fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
  }

  /**
   * @brief Print formatted text to output buffer with newline.
   * @tparam Args Format argument types (deduced)
   * @param fmt Format string with {} placeholders
   * @param args Values to substitute into placeholders
   */
  template <class... Args>
  void println(fmt::format_string<Args...> fmt, Args&&... args) {
    fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    out += '\n';
  }

  //=========================================================================
  // Code Generation Helpers
  //=========================================================================

  /**
   * @brief Check if current D-form load/store targets MMIO address.
   *
   * Checks: next instruction is eieio, or operands[2] (base register) is MMIO base.
   *
   * @return true if this is an MMIO access
   */
  bool mmio_check_d_form();

  /**
   * @brief Check if current X-form load/store targets MMIO address.
   *
   * Checks: next instruction is eieio, or operands[1]/operands[2] is MMIO base.
   *
   * @return true if this is an MMIO access
   */
  bool mmio_check_x_form();

  /**
   * @brief Find pre-resolved call target for an instruction site.
   * @param site Address of the call/branch instruction
   * @return Pointer to CallTarget if found, nullptr otherwise
   *
   * Searches the FunctionNode's calls and tailCalls for a matching site.
   */
  const CallTarget* findCallTarget(uint32_t site) const;

  /**
   * @brief Emit C++ code for a function call.
   * @param address Target function address
   *
   * Uses pre-resolved CallTarget from FunctionNode when available.
   * Falls back to symbol lookup for backward compatibility.
   * Handles special cases like setjmp/longjmp and __restgprlr_N functions.
   */
  void emit_function_call(uint32_t address);

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

  /// Emit mid-asm hook if configured for current address
  void emit_mid_asm_hook();

  /// Check if mid-asm hook exists for current address
  bool has_mid_asm_hook() const;

  /// Clear active jump table pointer (used after processing a switch)
  void reset_switch_table() { activeJumpTable = nullptr; }

  //=========================================================================
  // Vector (SIMD) Code Generation Helpers
  //=========================================================================

  /**
   * @brief Emit binary float vector operation: vD = op(vA, vB)
   * @param simd_op The SIMDE function name (e.g., "add_ps", "sub_ps", "mul_ps")
   *
   * Emits: simde_mm_store_ps(vD.f32, simde_mm_OP(load(vA.f32), load(vB.f32)));
   * Uses operands[0]=vD, operands[1]=vA, operands[2]=vB from current instruction.
   */
  void emit_vec_fp_binary(const char* simd_op);

  /**
   * @brief Emit unary float vector operation: vD = op(vA)
   * @param simd_expr SIMDE expression for the operation (will be wrapped in store)
   *
   * Use when the operation is more complex than a single function call.
   * Emits: simde_mm_store_ps(vD.f32, EXPR);
   * Uses operands[0]=vD, operands[1]=vA from current instruction.
   * Example: emit_vec_fp_unary_expr("simde_mm_sqrt_ps(simde_mm_load_ps({vA}.f32))")
   */
  void emit_vec_fp_unary_expr(std::string_view simd_expr);

  /**
   * @brief Emit binary integer vector operation: vD = op(vA, vB)
   * @param simd_op The SIMDE function name (e.g., "add_epi16", "and_si128")
   * @param element_type The vector element type suffix (e.g., "u8", "s16", "u32")
   *
   * Emits: simde_mm_store_si128((simde__m128i*)vD.TYPE, simde_mm_OP(load(vA), load(vB)));
   * Uses operands[0]=vD, operands[1]=vA, operands[2]=vB from current instruction.
   */
  void emit_vec_int_binary(const char* simd_op, const char* element_type);

  /**
   * @brief Emit binary integer vector operation with swapped operands: vD = op(vB, vA)
   * @param simd_op The SIMDE function name
   * @param element_type The vector element type suffix
   *
   * Same as emit_vec_int_binary but swaps vA and vB order (useful for andnot, etc.)
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
  void emit_vec_var_shift(const char* shift_dir, const char* element_type, uint32_t mask_value);

  //=========================================================================
  // Memory (Load/Store) Code Generation Helpers
  //=========================================================================

  /**
   * @brief Emit load with D-form addressing: rD = LOAD(rA + offset)
   * @param load_macro The load macro name (e.g., "REX_LOAD_U8", "REX_LOAD_U32")
   * @param dest_type The destination type suffix (e.g., "u64", "s64")
   * @param check_mmio If true, uses mmio_load() to detect memory-mapped I/O
   *
   * Uses operands[0]=rD, operands[1]=offset (D), operands[2]=rA from current instruction.
   * If rA (operands[2]) is 0, omits the base register addition.
   */
  void emit_load_d_form(const char* load_macro, const char* dest_type, bool check_mmio = true);

  /**
   * @brief Emit load with X-form addressing: rD = LOAD(rA + rB)
   * @param load_macro The load macro name
   * @param dest_type The destination type suffix
   * @param check_mmio If true, uses mmio_load_x_form() to detect memory-mapped I/O
   *
   * Uses operands[0]=rD, operands[1]=rA, operands[2]=rB from current instruction.
   * If rA (operands[1]) is 0, omits the first register addition.
   */
  void emit_load_x_form(const char* load_macro, const char* dest_type, bool check_mmio = true);

  /**
   * @brief Emit store with D-form addressing: STORE(rA + offset, rS)
   * @param store_macro The store macro name (e.g., "REX_STORE_U8")
   * @param src_type The source type suffix (e.g., "u8", "u32")
   * @param check_mmio If true, uses mmio_store() to detect memory-mapped I/O
   *
   * Uses operands[0]=rS, operands[1]=offset (D), operands[2]=rA from current instruction.
   */
  void emit_store_d_form(const char* store_macro, const char* src_type, bool check_mmio = true);

  /**
   * @brief Emit store with X-form addressing: STORE(rA + rB, rS)
   * @param store_macro The store macro name
   * @param src_type The source type suffix
   * @param check_mmio If true, uses mmio_store() to detect memory-mapped I/O
   *
   * Uses operands[0]=rS, operands[1]=rA, operands[2]=rB from current instruction.
   */
  void emit_store_x_form(const char* store_macro, const char* src_type, bool check_mmio = true);
};
}  // namespace rex::codegen