/**
 * @file        codegen/builders/memory.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "builders.h"
#include "ppc/opcode.h"

#include <dis-asm.h>
#include <ppc-inst.h>

namespace rex::codegen {

namespace {

/**
 * Emit D-form load with update instruction.
 *
 * Pattern `EA = (rA) + d; rD = MEM[EA]; rA = EA`
 * Used by lbzu, lwzu, ldu and others
 *
 * @param ctx The builder context
 * @param load_macro The REX_LOAD_* macro to use (e.g., "REX_LOAD_U8")
 */
void EmitLoadWithUpdate(BuilderContext& ctx, const char* load_macro) {
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.u64 = {}({});", ctx.R(ctx.insn.operands[0]), ctx.MMIOMacro(load_macro),
              ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
}

/**
 * Emit X-form load with update instruction.
 *
 * Pattern `EA = rA + rB; rD = MEM[EA]; rA = EA`
 * Used by lbzux, lhzux, lwzux, ldux
 *
 * @param ctx The builder context
 * @param load_macro The REX_LOAD_* macro to use (e.g., "REX_LOAD_U8")
 */
void EmitLoadXFormWithUpdate(BuilderContext& ctx, const char* load_macro) {
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.u64 = {}({});", ctx.R(ctx.insn.operands[0]), ctx.MMIOMacro(load_macro),
              ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
}

/**
 * Emit D-form store with update instruction.
 *
 * Pattern `EA = (rA) + d; MEM[EA] = rS; rA = EA`
 * Used by stbu, stwu, stdu and others
 *
 * @param ctx The builder context
 * @param store_macro The REX_STORE_* macro to use (e.g., "REX_STORE_U8")
 * @param field The register field to store (e.g., "u8", "u32", "u64")
 */
void EmitStoreWithUpdate(BuilderContext& ctx, const char* store_macro, const char* field) {
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}({}, {}.{});", ctx.MMIOMacro(store_macro), ctx.EA(), ctx.R(ctx.insn.operands[0]),
              field);
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
}

/**
 * Emit X-form store with update instruction.
 *
 * Pattern `EA = rA + rB; MEM[EA] = rS; rA = EA`
 * Used by stbux, sthux, stwux, stdux
 *
 * @param ctx The builder context
 * @param store_macro The REX_STORE_* macro (e.g., "REX_STORE_U8")
 * @param field The register field to store (e.g., "u8", "u32", "u64")
 */
void EmitStoreXFormWithUpdate(BuilderContext& ctx, const char* store_macro, const char* field) {
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}({}, {}.{});", ctx.MMIOMacro(store_macro), ctx.EA(), ctx.R(ctx.insn.operands[0]),
              field);
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
}

/**
 * Emit atomic load-and-reserve instruction (lwarx/ldarx pattern).
 *
 * Pattern `EA = rA + rB; reserved = *(T*)REX_RAW_ADDR(EA); rD = bswap(reserved)`
 *
 * @param ctx The builder context
 * @param ptr_type The pointer type (e.g., "u32", "u64")
 * @param bswap_func The byte-swap builtin (e.g., "__builtin_bswap32")
 * @param reserved_field The reserved register field (e.g., "u32", "u64")
 */
void EmitAtomicLoadReserve(BuilderContext& ctx, const char* ptr_type, const char* bswap_func,
                           const char* reserved_field) {
  ctx.Print("\t{} = ", ctx.EA());
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32;", ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.{} = *({}*)REX_RAW_ADDR({});", ctx.Reserved(), reserved_field, ptr_type,
              ctx.EA());
  ctx.Println("\t{}.u64 = {}({}.{});", ctx.R(ctx.insn.operands[0]), bswap_func, ctx.Reserved(),
              reserved_field);
}

/**
 * Emit atomic store-conditional instruction (stwcx./stdcx. pattern).
 *
 * Pattern `EA = rA + rB; cr0 = CAS(EA, reserved, bswap(rS))`
 *
 * @param ctx The builder context
 * @param ptr_type The pointer type (e.g., "u32", "u64")
 * @param bswap_func The byte-swap builtin (e.g., "__builtin_bswap32")
 * @param field The register field (e.g., "u32", "u64")
 */
void EmitAtomicStoreConditional(BuilderContext& ctx, const char* ptr_type, const char* bswap_func,
                                const char* field) {
  ctx.Print("\t{} = ", ctx.EA());
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32;", ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.lt = 0;", ctx.CR(0));
  ctx.Println("\t{}.gt = 0;", ctx.CR(0));
  ctx.Println(
      "\t{}.eq = reinterpret_cast<std::atomic<{}>*>(REX_RAW_ADDR({}))"
      "->compare_exchange_strong({}.{}, {}({}.{}), "
      "std::memory_order_acq_rel, std::memory_order_acquire);",
      ctx.CR(0), ptr_type, ctx.EA(), ctx.Reserved(), field, bswap_func, ctx.R(ctx.insn.operands[0]),
      field);
}

/**
 * Emit D-form sign-extending load instruction.
 *
 * Pattern `rD = sign_extend(LOAD(rA + d))`
 * Used by lha, lwa (halfword/word algebraic loads)
 *
 * @param ctx The builder context
 * @param cast_type The cast for sign extension (e.g., "i16", "i32")
 * @param load_macro The REX_LOAD_* macro to use
 */
void EmitSignExtendLoadDForm(BuilderContext& ctx, const char* cast_type, const char* load_macro) {
  const u8 width = std::string_view(load_macro).ends_with("U16") ? 2 : 4;
  if (const auto held = ctx.FindStackWrite(width)) {
    ctx.Println("\t{}.s64 = {}({}.u{});", ctx.R(ctx.insn.operands[0]), cast_type, *held, width * 8);
    return;
  }
  ctx.Print("\t{}.s64 = {}({}(", ctx.R(ctx.insn.operands[0]), cast_type, ctx.MMIOMacro(load_macro));
  if (ctx.insn.operands[2] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[2]));
  ctx.Println("{}));", static_cast<i32>(ctx.insn.operands[1]));
}

/**
 * Emit X-form sign-extending load instruction.
 *
 * Pattern `rD = sign_extend(LOAD(rA + rB))`
 * Used by lhax, lwax (halfword/word algebraic indexed loads)
 *
 * @param ctx The builder context
 * @param cast_type The cast for sign extension (e.g., "i16", "i32")
 * @param load_macro The REX_LOAD_* macro to use
 */
void EmitSignExtendLoadXForm(BuilderContext& ctx, const char* cast_type, const char* load_macro) {
  ctx.Print("\t{}.s64 = {}({}(", ctx.R(ctx.insn.operands[0]), cast_type, ctx.MMIOMacro(load_macro));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32));", ctx.R(ctx.insn.operands[2]));
}

/**
 * Emit aligned or unaligned vector effective address calculation to ea.
 *
 * Pattern `ea = (opt_rA + rB) [& ~align_mask]`
 * Uses operands[1] as optional base register and operands[2] as index register.
 *
 * @param ctx The builder context
 * @param align_mask Alignment mask string (e.g., "0xF"), or null for no alignment
 */
void EmitVectorEA(BuilderContext& ctx, const char* align_mask = nullptr) {
  if (align_mask)
    ctx.Print("\t{} = (", ctx.EA());
  else
    ctx.Print("\t{} = ", ctx.EA());
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  if (align_mask)
    ctx.Println("{}.u32) & ~{};", ctx.R(ctx.insn.operands[2]), align_mask);
  else
    ctx.Println("{}.u32;", ctx.R(ctx.insn.operands[2]));
}

/**
 * Emit unaligned vector effective address calculation to temp.
 *
 * Pattern `temp.u32 = opt_rA + rB`
 * Uses operands[1] as optional base register and operands[2] as index register.
 *
 * @param ctx The builder context
 */
void EmitVectorTempEA(BuilderContext& ctx) {
  ctx.Print("\t{}.u32 = ", ctx.Temp());
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32;", ctx.R(ctx.insn.operands[2]));
}

}  // namespace

bool BuildLi(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = {};", ctx.R(ctx.insn.operands[0]),
              static_cast<i32>(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_LIS>(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = {};", ctx.R(ctx.insn.operands[0]),
              static_cast<i32>(static_cast<u32>(ctx.insn.operands[1]) << 16));
  return true;
}

template <>
bool Build<PPC_INST_LBZ>(BuilderContext& ctx) {
  ctx.EmitLoadDForm("REX_LOAD_U8", "u64");
  return true;
}

template <>
bool Build<PPC_INST_LBZU>(BuilderContext& ctx) {
  EmitLoadWithUpdate(ctx, "REX_LOAD_U8");
  return true;
}

template <>
bool Build<PPC_INST_LBZX>(BuilderContext& ctx) {
  ctx.EmitLoadXForm("REX_LOAD_U8", "u64");
  return true;
}

template <>
bool Build<PPC_INST_LBZUX>(BuilderContext& ctx) {
  EmitLoadXFormWithUpdate(ctx, "REX_LOAD_U8");
  return true;
}

template <>
bool Build<PPC_INST_LHA>(BuilderContext& ctx) {
  EmitSignExtendLoadDForm(ctx, "i16", "REX_LOAD_U16");
  return true;
}

template <>
bool Build<PPC_INST_LHAX>(BuilderContext& ctx) {
  EmitSignExtendLoadXForm(ctx, "i16", "REX_LOAD_U16");
  return true;
}

template <>
bool Build<PPC_INST_LHZ>(BuilderContext& ctx) {
  ctx.EmitLoadDForm("REX_LOAD_U16", "u64");
  return true;
}

template <>
bool Build<PPC_INST_LHZX>(BuilderContext& ctx) {
  ctx.EmitLoadXForm("REX_LOAD_U16", "u64");
  return true;
}

template <>
bool Build<PPC_INST_LHZU>(BuilderContext& ctx) {
  EmitLoadWithUpdate(ctx, "REX_LOAD_U16");
  return true;
}

template <>
bool Build<PPC_INST_LHZUX>(BuilderContext& ctx) {
  EmitLoadXFormWithUpdate(ctx, "REX_LOAD_U16");
  return true;
}

template <>
bool Build<PPC_INST_LHAU>(BuilderContext& ctx) {
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.s64 = i16({}({}));", ctx.R(ctx.insn.operands[0]), ctx.MMIOMacro("REX_LOAD_U16"),
              ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_LHAUX>(BuilderContext& ctx) {
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.s64 = i16({}({}));", ctx.R(ctx.insn.operands[0]), ctx.MMIOMacro("REX_LOAD_U16"),
              ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_LHBRX>(BuilderContext& ctx) {
  ctx.Print("\t{}.u64 = __builtin_bswap16({}(", ctx.R(ctx.insn.operands[0]),
            ctx.MMIOMacro("REX_LOAD_U16"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32));", ctx.R(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_LWA>(BuilderContext& ctx) {
  EmitSignExtendLoadDForm(ctx, "i32", "REX_LOAD_U32");
  return true;
}

template <>
bool Build<PPC_INST_LWAUX>(BuilderContext& ctx) {
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.s64 = i32({}({}));", ctx.R(ctx.insn.operands[0]), ctx.MMIOMacro("REX_LOAD_U32"),
              ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_LWAX>(BuilderContext& ctx) {
  EmitSignExtendLoadXForm(ctx, "i32", "REX_LOAD_U32");
  return true;
}

template <>
bool Build<PPC_INST_LWZ>(BuilderContext& ctx) {
  ctx.EmitLoadDForm("REX_LOAD_U32", "u64");
  return true;
}

template <>
bool Build<PPC_INST_LWZU>(BuilderContext& ctx) {
  EmitLoadWithUpdate(ctx, "REX_LOAD_U32");
  return true;
}

template <>
bool Build<PPC_INST_LWZX>(BuilderContext& ctx) {
  ctx.EmitLoadXForm("REX_LOAD_U32", "u64");
  return true;
}

template <>
bool Build<PPC_INST_LWZUX>(BuilderContext& ctx) {
  EmitLoadXFormWithUpdate(ctx, "REX_LOAD_U32");
  return true;
}

template <>
bool Build<PPC_INST_LWBRX>(BuilderContext& ctx) {
  ctx.Print("\t{}.u64 = __builtin_bswap32({}(", ctx.R(ctx.insn.operands[0]),
            ctx.MMIOMacro("REX_LOAD_U32"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32));", ctx.R(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_LD>(BuilderContext& ctx) {
  ctx.EmitLoadDForm("REX_LOAD_U64", "u64");
  return true;
}

template <>
bool Build<PPC_INST_LDU>(BuilderContext& ctx) {
  EmitLoadWithUpdate(ctx, "REX_LOAD_U64");
  return true;
}

template <>
bool Build<PPC_INST_LDX>(BuilderContext& ctx) {
  ctx.EmitLoadXForm("REX_LOAD_U64", "u64");
  return true;
}

template <>
bool Build<PPC_INST_LDUX>(BuilderContext& ctx) {
  EmitLoadXFormWithUpdate(ctx, "REX_LOAD_U64");
  return true;
}

template <>
bool Build<PPC_INST_LWARX>(BuilderContext& ctx) {
  EmitAtomicLoadReserve(ctx, "u32", "__builtin_bswap32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_LDARX>(BuilderContext& ctx) {
  EmitAtomicLoadReserve(ctx, "u64", "__builtin_bswap64", "u64");
  return true;
}

template <>
bool Build<PPC_INST_LFD>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  if (const auto held = ctx.FindStackWrite(8)) {
    ctx.Println("\t{}.u64 = {}.u64;", ctx.F(ctx.insn.operands[0]), *held);
    return true;
  }
  ctx.Print("\t{}.u64 = {}(", ctx.F(ctx.insn.operands[0]), ctx.StackMacro("REX_LOAD_U64"));
  if (ctx.insn.operands[2] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[2]));
  ctx.Println("{});", static_cast<i32>(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_LFDX>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Print("\t{}.u64 = {}(", ctx.F(ctx.insn.operands[0]), ctx.StackMacro("REX_LOAD_U64"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32);", ctx.R(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_LFS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  if (const auto held = ctx.FindStackWrite(4)) {
    ctx.Println("\t{}.f64 = f64({}.f32);", ctx.F(ctx.insn.operands[0]), *held);
    return true;
  }
  ctx.Print("\t{}.u32 = {}(", ctx.Temp(), ctx.StackMacro("REX_LOAD_U32"));
  if (ctx.insn.operands[2] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[2]));
  ctx.Println("{});", static_cast<i32>(ctx.insn.operands[1]));
  ctx.Println("\t{}.f64 = f64({}.f32);", ctx.F(ctx.insn.operands[0]), ctx.Temp());
  return true;
}

template <>
bool Build<PPC_INST_LFSX>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Print("\t{}.u32 = {}(", ctx.Temp(), ctx.StackMacro("REX_LOAD_U32"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32);", ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.f64 = f64({}.f32);", ctx.F(ctx.insn.operands[0]), ctx.Temp());
  return true;
}

template <>
bool Build<PPC_INST_LFDU>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.u64 = {}({});", ctx.F(ctx.insn.operands[0]), ctx.StackMacro("REX_LOAD_U64"),
              ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_LFDUX>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.u64 = {}({});", ctx.F(ctx.insn.operands[0]), ctx.StackMacro("REX_LOAD_U64"),
              ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_LFSU>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.u32 = {}({});", ctx.Temp(), ctx.StackMacro("REX_LOAD_U32"), ctx.EA());
  ctx.Println("\t{}.f64 = f64({}.f32);", ctx.F(ctx.insn.operands[0]), ctx.Temp());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_LFSUX>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.u32 = {}({});", ctx.Temp(), ctx.StackMacro("REX_LOAD_U32"), ctx.EA());
  ctx.Println("\t{}.f64 = f64({}.f32);", ctx.F(ctx.insn.operands[0]), ctx.Temp());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_STB>(BuilderContext& ctx) {
  ctx.EmitStoreDForm("REX_STORE_U8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_STBU>(BuilderContext& ctx) {
  EmitStoreWithUpdate(ctx, "REX_STORE_U8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_STBX>(BuilderContext& ctx) {
  ctx.EmitStoreXForm("REX_STORE_U8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_STBUX>(BuilderContext& ctx) {
  EmitStoreXFormWithUpdate(ctx, "REX_STORE_U8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_STH>(BuilderContext& ctx) {
  ctx.EmitStoreDForm("REX_STORE_U16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_STHBRX>(BuilderContext& ctx) {
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U16"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32, __builtin_bswap16({}.u16));", ctx.R(ctx.insn.operands[2]),
              ctx.R(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_STHX>(BuilderContext& ctx) {
  ctx.EmitStoreXForm("REX_STORE_U16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_STHU>(BuilderContext& ctx) {
  EmitStoreWithUpdate(ctx, "REX_STORE_U16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_STHUX>(BuilderContext& ctx) {
  EmitStoreXFormWithUpdate(ctx, "REX_STORE_U16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_STW>(BuilderContext& ctx) {
  ctx.EmitStoreDForm("REX_STORE_U32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_STWU>(BuilderContext& ctx) {
  EmitStoreWithUpdate(ctx, "REX_STORE_U32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_STWUX>(BuilderContext& ctx) {
  EmitStoreXFormWithUpdate(ctx, "REX_STORE_U32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_STWX>(BuilderContext& ctx) {
  ctx.EmitStoreXForm("REX_STORE_U32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_STMW>(BuilderContext& ctx) {
  u32 r_s = ctx.insn.operands[0];
  i32 offset = static_cast<i32>(ctx.insn.operands[1]);
  u32 r_a = ctx.insn.operands[2];
  if (r_a != 0)
    ctx.Println("\t{} = {}.u32 + {};", ctx.EA(), ctx.R(r_a), offset);
  else
    ctx.Println("\t{} = {};", ctx.EA(), offset);
  for (u32 i = r_s; i <= 31; ++i) {
    ctx.Println("\tREX_STORE_U32({} + {}, {}.u32);", ctx.EA(), (i - r_s) * 4, ctx.R(i));
  }
  return true;
}

template <>
bool Build<PPC_INST_STWBRX>(BuilderContext& ctx) {
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U32"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32, __builtin_bswap32({}.u32));", ctx.R(ctx.insn.operands[2]),
              ctx.R(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_STWCX>(BuilderContext& ctx) {
  EmitAtomicStoreConditional(ctx, "u32", "__builtin_bswap32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_STDCX>(BuilderContext& ctx) {
  EmitAtomicStoreConditional(ctx, "u64", "__builtin_bswap64", "u64");
  return true;
}

template <>
bool Build<PPC_INST_STD>(BuilderContext& ctx) {
  ctx.EmitStoreDForm("REX_STORE_U64", "u64");
  return true;
}

template <>
bool Build<PPC_INST_STDU>(BuilderContext& ctx) {
  EmitStoreWithUpdate(ctx, "REX_STORE_U64", "u64");
  return true;
}

template <>
bool Build<PPC_INST_STDX>(BuilderContext& ctx) {
  ctx.EmitStoreXForm("REX_STORE_U64", "u64");
  return true;
}

template <>
bool Build<PPC_INST_STDUX>(BuilderContext& ctx) {
  EmitStoreXFormWithUpdate(ctx, "REX_STORE_U64", "u64");
  return true;
}

template <>
bool Build<PPC_INST_STFD>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  if (const auto held = ctx.HoldStackWrite(8)) {
    ctx.Println("\t{}.u64 = {}.u64;", *held, ctx.F(ctx.insn.operands[0]));
    ctx.Println("\t{}({}.u32 + {}, {}.u64);", ctx.MMIOMacro("REX_STORE_U64"), ctx.R(1),
                static_cast<i32>(ctx.insn.operands[1]), *held);
    return true;
  }
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U64"));
  if (ctx.insn.operands[2] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[2]));
  ctx.Println("{}, {}.u64);", static_cast<i32>(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_STFDX>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U64"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32, {}.u64);", ctx.R(ctx.insn.operands[2]), ctx.F(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_STFDUX>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}({}, {}.u64);", ctx.MMIOMacro("REX_STORE_U64"), ctx.EA(),
              ctx.F(ctx.insn.operands[0]));
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_STFIWX>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U32"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32, {}.u32);", ctx.R(ctx.insn.operands[2]), ctx.F(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_STFS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  if (const auto held = ctx.HoldStackWrite(4)) {
    ctx.Println("\t{}.f32 = f32({}.f64);", *held, ctx.F(ctx.insn.operands[0]));
    ctx.Println("\t{}({}.u32 + {}, {}.u32);", ctx.MMIOMacro("REX_STORE_U32"), ctx.R(1),
                static_cast<i32>(ctx.insn.operands[1]), *held);
    return true;
  }
  ctx.Println("\t{}.f32 = f32({}.f64);", ctx.Temp(), ctx.F(ctx.insn.operands[0]));
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U32"));
  if (ctx.insn.operands[2] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[2]));
  ctx.Println("{}, {}.u32);", static_cast<i32>(ctx.insn.operands[1]), ctx.Temp());
  return true;
}

template <>
bool Build<PPC_INST_STFSX>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f32 = f32({}.f64);", ctx.Temp(), ctx.F(ctx.insn.operands[0]));
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U32"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32, {}.u32);", ctx.R(ctx.insn.operands[2]), ctx.Temp());
  return true;
}

template <>
bool Build<PPC_INST_STFDU>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}({}, {}.u64);", ctx.MMIOMacro("REX_STORE_U64"), ctx.EA(),
              ctx.F(ctx.insn.operands[0]));
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_STFSU>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.f32 = f32({}.f64);", ctx.Temp(), ctx.F(ctx.insn.operands[0]));
  ctx.Println("\t{}({}, {}.u32);", ctx.MMIOMacro("REX_STORE_U32"), ctx.EA(), ctx.Temp());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_STFSUX>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f32 = f32({}.f64);", ctx.Temp(), ctx.F(ctx.insn.operands[0]));
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}({}, {}.u32);", ctx.MMIOMacro("REX_STORE_U32"), ctx.EA(), ctx.Temp());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_LVX>(BuilderContext& ctx) {
  // Endian swap reverses the whole vector, so builders account for it (dp3 sums yzw, not xyz)
  EmitVectorEA(ctx, "0xF");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR({})), "
      "simde_mm_load_si128((simde__m128i*)kVectorMaskL)));",
      ctx.V(ctx.insn.operands[0]), ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_LVLX>(BuilderContext& ctx) {
  EmitVectorTempEA(ctx);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR({}.u32 & ~0xF)), "
      "simde_mm_load_si128((simde__m128i*)&kVectorMaskL[({}.u32 & 0xF) * 16])));",
      ctx.V(ctx.insn.operands[0]), ctx.Temp(), ctx.Temp());
  return true;
}

template <>
bool Build<PPC_INST_LVRX>(BuilderContext& ctx) {
  EmitVectorTempEA(ctx);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, {}.u32 & 0xF ? "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR({}.u32 & ~0xF)), "
      "simde_mm_load_si128((simde__m128i*)&kVectorMaskR[({}.u32 & 0xF) * 16])) : "
      "simde_mm_setzero_si128());",
      ctx.V(ctx.insn.operands[0]), ctx.Temp(), ctx.Temp(), ctx.Temp());
  return true;
}

template <>
bool Build<PPC_INST_LVSL>(BuilderContext& ctx) {
  EmitVectorTempEA(ctx);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_load_si128((simde__m128i*)&kVectorShiftTableL[({}.u32 & 0xF) * 16]));",
      ctx.V(ctx.insn.operands[0]), ctx.Temp());
  return true;
}

template <>
bool Build<PPC_INST_LVSR>(BuilderContext& ctx) {
  EmitVectorTempEA(ctx);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_load_si128((simde__m128i*)&kVectorShiftTableR[({}.u32 & 0xF) * 16]));",
      ctx.V(ctx.insn.operands[0]), ctx.Temp());
  return true;
}

template <>
bool Build<PPC_INST_STVEBX>(BuilderContext& ctx) {
  EmitVectorEA(ctx);
  ctx.Println("\tREX_STORE_U8({}, {}.u8[15 - ({} & 0xF)]);", ctx.EA(), ctx.V(ctx.insn.operands[0]),
              ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_STVEHX>(BuilderContext& ctx) {
  EmitVectorEA(ctx, "0x1");
  ctx.Println("\tREX_STORE_U16(ea, {}.u16[7 - (({} & 0xF) >> 1)]);", ctx.V(ctx.insn.operands[0]),
              ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_STVEWX>(BuilderContext& ctx) {
  EmitVectorEA(ctx, "0x3");
  ctx.Println("\tREX_STORE_U32(ea, {}.u32[3 - (({} & 0xF) >> 2)]);", ctx.V(ctx.insn.operands[0]),
              ctx.EA());
  return true;
}

template <>
bool Build<PPC_INST_STVLX>(BuilderContext& ctx) {
  if (ctx.next != nullptr) {
    const int next_id = rex::codegen::detail::ResolveID(*ctx.next);
    if ((next_id == PPC_INST_STVRX || next_id == PPC_INST_STVRX128) &&
        ctx.next->operands[0] == ctx.insn.operands[0]) {
      ctx.ConsumeNext();
      EmitVectorEA(ctx);
      ctx.Print("\t{}.u32 = ", ctx.Temp());
      if (ctx.next->operands[1] != 0)
        ctx.Print("{}.u32 + ", ctx.R(ctx.next->operands[1]));
      ctx.Println("{}.u32;", ctx.R(ctx.next->operands[2]));
      ctx.Println(
          "\trex::ppc::simd::StoreVectorPair(REX_RAW_ADDR({}), {}, REX_RAW_ADDR({}.u32 & ~0xF), "
          "{}.u32, simde_mm_load_si128((simde__m128i*){}.u8));",
          ctx.EA(), ctx.EA(), ctx.Temp(), ctx.Temp(), ctx.V(ctx.insn.operands[0]));
      return true;
    }
  }

  EmitVectorEA(ctx);

  ctx.Println(
      "\trex::ppc::simd::StoreVectorLeft(REX_RAW_ADDR({}), {}, "
      "simde_mm_load_si128((simde__m128i*){}.u8));",
      ctx.EA(), ctx.EA(), ctx.V(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_STVRX>(BuilderContext& ctx) {
  EmitVectorEA(ctx);

  ctx.Println(
      "\trex::ppc::simd::StoreVectorRight(REX_RAW_ADDR({} & ~0xF), {}, "
      "simde_mm_load_si128((simde__m128i*){}.u8));",
      ctx.EA(), ctx.EA(), ctx.V(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_STVX>(BuilderContext& ctx) {
  EmitVectorEA(ctx, "0xF");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*)REX_RAW_ADDR({}), "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*)kVectorMaskL)));",
      ctx.EA(), ctx.V(ctx.insn.operands[0]));
  return true;
}

}  // namespace rex::codegen
