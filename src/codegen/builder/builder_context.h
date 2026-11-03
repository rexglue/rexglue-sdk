/**
 * @file        codegen/builder/builder_context.h
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

#include "config.h"
#include "function_graph.h"

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

/** Placeholder lines that stand for copying every localized nonvolatile into ctx or back. */
inline constexpr std::string_view kSyncToCtx = "@sync_to_ctx";
inline constexpr std::string_view kSyncFromCtx = "@sync_from_ctx";

/**
 * Flush mode state of the host floating point control register. kUnknown is the initial state and
 * the state after a function call, so the next FP or VMX instruction emits a conditional mode
 * check. kFPU preserves denormals. kVMX follows the VSCR non Java bit.
 */
enum class CSRState : u8 { kUnknown, kFPU, kVMX };

struct BuilderContext {
  std::string& out;

  const EmitContext& emit_ctx;

  const FunctionNode& fn;

  const ppc_insn& insn;

  /** Guest address of the current instruction. */
  u32 base;

  /** Pointer to the raw instruction words in the image. */
  const u32* data;

  /** Tracks which registers need local variable declarations. */
  RecompilerLocalVariables& locals;

  CSRState& csr_state;

  /** Active jump table for bctr dispatch, or null. */
  const JumpTable* active_jump_table = nullptr;

  /** MMIO classification of the current function, or null. */
  const MMIOClassification* mmio = nullptr;

  /** The decoded instruction that follows in the same block, or null when it may not be fused. */
  const ppc_insn* next = nullptr;

  /** How many instruction words the builder emitted code for. */
  u32 consumed = 1;

  bool seh_scoped = false;

  /** The stack slots stored in this basic block, or null when forwarding is off. */
  StackForwarding* forwarding = nullptr;

  /** Marks the next instruction as emitted with the current one. Requires next to be non null. */
  void ConsumeNext();

  const RecompilerConfig& GetConfig() const;

  const FunctionGraph& GetGraph() const;

  /**
   * Reports whether nonvolatiles may be localized in the current function. False for a function
   * that shares registers with an intra function bl partner, an SEH funclet and a save or restore
   * helper.
   *
   * @return True when localizing is allowed.
   */
  bool CanLocalizeNonVolatiles() const;

  /**
   * Returns the expression for general purpose register access. The expression is "rN" for local
   * variables and "ctx.rN" for context access.
   *
   * @param index Register index, 0 to 31
   */
  std::string R(size_t index);

  /**
   * Returns the expression for floating point register access. The expression is "fN" for local
   * variables and "ctx.fN" for context access.
   *
   * @param index Register index, 0 to 31
   */
  std::string F(size_t index);

  /**
   * Returns the expression for vector register access. The expression is "vN" for local variables
   * and "ctx.vN" for context access.
   *
   * @param index Register index, 0 to 127 with the extended VMX128 range
   */
  std::string V(size_t index);

  /**
   * Returns the expression for condition register field access. The expression is "crN" for local
   * variables and "ctx.crN" for context access.
   *
   * @param index CR field index, 0 to 7
   */
  std::string CR(size_t index);

  /** Returns "ctr" when CTR is kept in a local, otherwise "ctx.ctr". */
  const char* CTR();

  /** Returns "xer" when XER is kept in a local, otherwise "ctx.xer". */
  const char* XER();

  /** Returns the reservation register expression that lwarx and stwcx use. */
  const char* Reserved();

  const char* Temp();

  const char* VTemp();

  const char* Env();

  const char* EA();

  template <class... Args>
  void Print(std::format_string<Args...> fmt, Args&&... args) {
    std::vformat_to(std::back_inserter(out), fmt.get(), std::make_format_args(args...));
  }

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
   * Picks the direct stack form of an access macro. It never routes to a register block.
   *
   * @param plain The REX_LOAD_* or REX_STORE_* macro.
   * @return The REX_STACK_* macro for an access through r1, otherwise plain.
   */
  std::string_view StackMacro(std::string_view plain) const;

  /** Reports whether the memory instruction addresses through r1. */
  bool IsStackAccess() const;

  /**
   * Returns the pre resolved call target of an instruction site, or null when none matches.
   *
   * @param site Address of the call or branch instruction
   */
  const CallTarget* FindCallTarget(u32 site) const;

  /**
   * Logs an error and emits REX_FATAL when the function node has no call target. Save and restore
   * helper calls are elided in a function that localizes its nonvolatiles. An SEH funclet and a
   * setjmp call copy the localized registers into ctx before the call and back after it.
   */
  void EmitFunctionCall(u32 address);

  void EmitCallToFunction(const FunctionNode& target_fn);

  /**
   * Reports whether a jump out of the function can be a tail call. SEH scopes, a hook after the
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
   * Emits a conditional branch as `goto loc_X` within the function or as a call out of it.
   *
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

  /** Reports whether a mid asm hook exists for the current address. */
  bool HasMidAsmHook() const;

  /** Clears the active jump table after a switch. */
  void ResetJumpTable() { active_jump_table = nullptr; }

  /**
   * Emits binary float vector operation `vD = op(vA, vB)` from operands 0 to 2.
   *
   * @param simd_op The SIMDE function name (e.g., "add_ps", "sub_ps", "mul_ps")
   */
  void EmitVecFPBinary(const char* simd_op);

  /**
   * Emits unary float vector operation `vD = op(vA)` from operands 0 and 1.
   *
   * @param simd_expr SIMDE expression for the operation wrapped in a vector store
   */
  void EmitVecFPUnaryExpr(std::string_view simd_expr);

  /**
   * Emits binary integer vector operation `vD = op(vA, vB)` from operands 0 to 2.
   *
   * @param simd_op The SIMDE function name (e.g., "add_epi16", "and_si128")
   * @param element_type The vector element type suffix (e.g., "u8", "s16", "u32")
   */
  void EmitVecIntBinary(const char* simd_op, const char* element_type);

  /**
   * Emits binary integer vector operation with swapped operands `vD = op(vB, vA)`.
   *
   * @param simd_op The SIMDE function name
   * @param element_type The vector element type suffix
   */
  void EmitVecIntBinarySwapped(const char* simd_op, const char* element_type);

  /**
   * Emits variable shift `vD = rex::ppc::simd::{function}(vA, vB & mask)` from operands 0 to 2.
   *
   * @param function The rex::ppc::simd shift function ("ShiftLeftLanes<u16>", ...)
   * @param element_type The SIMDE element type suffix ("epi16")
   * @param mask_value Shift amount mask, such as 0xF for 16 bit lanes
   */
  void EmitVecVarShift(const char* function, const char* element_type, u32 mask_value);

  /**
   * Holds the write of the current instruction as a forwardable stack slot.
   *
   * @return The local that holds the stored value, empty when the store is not a plain access
   *         through r1 or forwarding is off.
   */
  std::optional<std::string> HoldStackWrite(u8 width);

  /**
   * Finds the local that holds the value a load of the current instruction would read.
   *
   * @return The local, empty when no earlier store of the same slot is live.
   */
  std::optional<std::string> FindStackWrite(u8 width) const;

  /**
   * Emits load with D-form addressing `rD = LOAD(rA + offset)`. Operand 2 equal to 0 omits rA.
   *
   * @param load_macro The load macro name (e.g., "REX_LOAD_U8", "REX_LOAD_U32")
   * @param dest_type The destination type suffix (e.g., "u64", "s64")
   */
  void EmitLoadDForm(const char* load_macro, const char* dest_type);

  /**
   * Emits load with X-form addressing `rD = LOAD(rA + rB)`. Operand 1 equal to 0 omits rA.
   *
   * @param dest_type The destination type suffix
   */
  void EmitLoadXForm(const char* load_macro, const char* dest_type);

  /**
   * Emits store with D-form addressing `STORE(rA + offset, rS)`.
   *
   * @param store_macro The store macro name (e.g., "REX_STORE_U8")
   * @param src_type The source type suffix (e.g., "u8", "u32")
   */
  void EmitStoreDForm(const char* store_macro, const char* src_type);

  /**
   * Emits store with X-form addressing `STORE(rA + rB, rS)`.
   *
   * @param src_type The source type suffix
   */
  void EmitStoreXForm(const char* store_macro, const char* src_type);
};

/**
 * Returns the CRRegister member name of a BI field bit. The name is "lt", "gt", "eq" or "so".
 *
 * @param bi BI field bit index, 0 to 3.
 */
const char* GetCRBitName(u32 bi);

/**
 * Returns the width in bytes of the memory access a load or store macro makes. The width is 1, 2, 4
 * or 8, taken from the width suffix of the macro name.
 *
 * @param macro A REX_LOAD_U* or REX_STORE_U* macro name, or one of its routed forms.
 */
u8 AccessWidth(std::string_view macro);

/**
 * Reports whether an instruction is a record form, which has a '.' suffix.
 *
 * @return True when the instruction name contains '.'
 */
bool IsRecordForm(const ppc_insn& insn);

/** Emits the CR0 update of a record form from the comparison of the result with 0. */
void EmitRecordFormCompare(BuilderContext& ctx);

}  // namespace rex::codegen