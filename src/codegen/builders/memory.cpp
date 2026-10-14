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
#include "helpers.h"

namespace rex::codegen {

bool BuildLi(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = {};", ctx.R(ctx.insn.operands[0]),
              static_cast<i32>(ctx.insn.operands[1]));
  return true;
}

bool BuildLis(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = {};", ctx.R(ctx.insn.operands[0]),
              static_cast<i32>(static_cast<u32>(ctx.insn.operands[1]) << 16));
  return true;
}

bool BuildLbz(BuilderContext& ctx) {
  ctx.EmitLoadDForm("REX_LOAD_U8", "u64");
  return true;
}

bool BuildLbzu(BuilderContext& ctx) {
  EmitLoadWithUpdate(ctx, "REX_LOAD_U8");
  return true;
}

bool BuildLbzx(BuilderContext& ctx) {
  ctx.EmitLoadXForm("REX_LOAD_U8", "u64");
  return true;
}

bool BuildLbzux(BuilderContext& ctx) {
  EmitLoadXFormWithUpdate(ctx, "REX_LOAD_U8");
  return true;
}

bool BuildLha(BuilderContext& ctx) {
  EmitSignExtendLoadDForm(ctx, "i16", "REX_LOAD_U16");
  return true;
}

bool BuildLhax(BuilderContext& ctx) {
  EmitSignExtendLoadXForm(ctx, "i16", "REX_LOAD_U16");
  return true;
}

bool BuildLhz(BuilderContext& ctx) {
  ctx.EmitLoadDForm("REX_LOAD_U16", "u64");
  return true;
}

bool BuildLhzx(BuilderContext& ctx) {
  ctx.EmitLoadXForm("REX_LOAD_U16", "u64");
  return true;
}

bool BuildLhzu(BuilderContext& ctx) {
  EmitLoadWithUpdate(ctx, "REX_LOAD_U16");
  return true;
}

bool BuildLhzux(BuilderContext& ctx) {
  EmitLoadXFormWithUpdate(ctx, "REX_LOAD_U16");
  return true;
}

bool BuildLhau(BuilderContext& ctx) {
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.s64 = i16({}({}));", ctx.R(ctx.insn.operands[0]), ctx.MMIOMacro("REX_LOAD_U16"),
              ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
  return true;
}

bool BuildLhaux(BuilderContext& ctx) {
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.s64 = i16({}({}));", ctx.R(ctx.insn.operands[0]), ctx.MMIOMacro("REX_LOAD_U16"),
              ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
  return true;
}

bool BuildLhbrx(BuilderContext& ctx) {
  ctx.Print("\t{}.u64 = __builtin_bswap16({}(", ctx.R(ctx.insn.operands[0]),
            ctx.MMIOMacro("REX_LOAD_U16"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32));", ctx.R(ctx.insn.operands[2]));
  return true;
}

bool BuildLwa(BuilderContext& ctx) {
  EmitSignExtendLoadDForm(ctx, "i32", "REX_LOAD_U32");
  return true;
}

bool BuildLwaux(BuilderContext& ctx) {
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.s64 = i32({}({}));", ctx.R(ctx.insn.operands[0]), ctx.MMIOMacro("REX_LOAD_U32"),
              ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
  return true;
}

bool BuildLwax(BuilderContext& ctx) {
  EmitSignExtendLoadXForm(ctx, "i32", "REX_LOAD_U32");
  return true;
}

bool BuildLwz(BuilderContext& ctx) {
  ctx.EmitLoadDForm("REX_LOAD_U32", "u64");
  return true;
}

bool BuildLwzu(BuilderContext& ctx) {
  EmitLoadWithUpdate(ctx, "REX_LOAD_U32");
  return true;
}

bool BuildLwzx(BuilderContext& ctx) {
  ctx.EmitLoadXForm("REX_LOAD_U32", "u64");
  return true;
}

bool BuildLwzux(BuilderContext& ctx) {
  EmitLoadXFormWithUpdate(ctx, "REX_LOAD_U32");
  return true;
}

bool BuildLwbrx(BuilderContext& ctx) {
  ctx.Print("\t{}.u64 = __builtin_bswap32({}(", ctx.R(ctx.insn.operands[0]),
            ctx.MMIOMacro("REX_LOAD_U32"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32));", ctx.R(ctx.insn.operands[2]));
  return true;
}

bool BuildLd(BuilderContext& ctx) {
  ctx.EmitLoadDForm("REX_LOAD_U64", "u64");
  return true;
}

bool BuildLdu(BuilderContext& ctx) {
  EmitLoadWithUpdate(ctx, "REX_LOAD_U64");
  return true;
}

bool BuildLdx(BuilderContext& ctx) {
  ctx.EmitLoadXForm("REX_LOAD_U64", "u64");
  return true;
}

bool BuildLdux(BuilderContext& ctx) {
  EmitLoadXFormWithUpdate(ctx, "REX_LOAD_U64");
  return true;
}

bool BuildLwarx(BuilderContext& ctx) {
  EmitAtomicLoadReserve(ctx, "u32", "__builtin_bswap32", "u32");
  return true;
}

bool BuildLdarx(BuilderContext& ctx) {
  EmitAtomicLoadReserve(ctx, "u64", "__builtin_bswap64", "u64");
  return true;
}

bool BuildLfd(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Print("\t{}.u64 = REX_LOAD_U64(", ctx.F(ctx.insn.operands[0]));
  if (ctx.insn.operands[2] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[2]));
  ctx.Println("{});", static_cast<i32>(ctx.insn.operands[1]));
  return true;
}

bool BuildLfdx(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Print("\t{}.u64 = REX_LOAD_U64(", ctx.F(ctx.insn.operands[0]));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32);", ctx.R(ctx.insn.operands[2]));
  return true;
}

bool BuildLfs(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Print("\t{}.u32 = REX_LOAD_U32(", ctx.Temp());
  if (ctx.insn.operands[2] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[2]));
  ctx.Println("{});", static_cast<i32>(ctx.insn.operands[1]));
  ctx.Println("\t{}.f64 = f64({}.f32);", ctx.F(ctx.insn.operands[0]), ctx.Temp());
  return true;
}

bool BuildLfsx(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Print("\t{}.u32 = REX_LOAD_U32(", ctx.Temp());
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32);", ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.f64 = f64({}.f32);", ctx.F(ctx.insn.operands[0]), ctx.Temp());
  return true;
}

bool BuildLfdu(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.u64 = REX_LOAD_U64({});", ctx.F(ctx.insn.operands[0]), ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
  return true;
}

bool BuildLfdux(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.u64 = REX_LOAD_U64({});", ctx.F(ctx.insn.operands[0]), ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
  return true;
}

bool BuildLfsu(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.u32 = REX_LOAD_U32({});", ctx.Temp(), ctx.EA());
  ctx.Println("\t{}.f64 = f64({}.f32);", ctx.F(ctx.insn.operands[0]), ctx.Temp());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
  return true;
}

bool BuildLfsux(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.u32 = REX_LOAD_U32({});", ctx.Temp(), ctx.EA());
  ctx.Println("\t{}.f64 = f64({}.f32);", ctx.F(ctx.insn.operands[0]), ctx.Temp());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
  return true;
}

bool BuildStb(BuilderContext& ctx) {
  ctx.EmitStoreDForm("REX_STORE_U8", "u8");
  return true;
}

bool BuildStbu(BuilderContext& ctx) {
  EmitStoreWithUpdate(ctx, "REX_STORE_U8", "u8");
  return true;
}

bool BuildStbx(BuilderContext& ctx) {
  ctx.EmitStoreXForm("REX_STORE_U8", "u8");
  return true;
}

bool BuildStbux(BuilderContext& ctx) {
  EmitStoreXFormWithUpdate(ctx, "REX_STORE_U8", "u8");
  return true;
}

bool BuildSth(BuilderContext& ctx) {
  ctx.EmitStoreDForm("REX_STORE_U16", "u16");
  return true;
}

bool BuildSthbrx(BuilderContext& ctx) {
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U16"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32, __builtin_bswap16({}.u16));", ctx.R(ctx.insn.operands[2]),
              ctx.R(ctx.insn.operands[0]));
  return true;
}

bool BuildSthx(BuilderContext& ctx) {
  ctx.EmitStoreXForm("REX_STORE_U16", "u16");
  return true;
}

bool BuildSthu(BuilderContext& ctx) {
  EmitStoreWithUpdate(ctx, "REX_STORE_U16", "u16");
  return true;
}

bool BuildSthux(BuilderContext& ctx) {
  EmitStoreXFormWithUpdate(ctx, "REX_STORE_U16", "u16");
  return true;
}

bool BuildStw(BuilderContext& ctx) {
  ctx.EmitStoreDForm("REX_STORE_U32", "u32");
  return true;
}

bool BuildStwu(BuilderContext& ctx) {
  EmitStoreWithUpdate(ctx, "REX_STORE_U32", "u32");
  return true;
}

bool BuildStwux(BuilderContext& ctx) {
  EmitStoreXFormWithUpdate(ctx, "REX_STORE_U32", "u32");
  return true;
}

bool BuildStwx(BuilderContext& ctx) {
  ctx.EmitStoreXForm("REX_STORE_U32", "u32");
  return true;
}

bool BuildStmw(BuilderContext& ctx) {
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

bool BuildStwbrx(BuilderContext& ctx) {
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U32"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32, __builtin_bswap32({}.u32));", ctx.R(ctx.insn.operands[2]),
              ctx.R(ctx.insn.operands[0]));
  return true;
}

bool BuildStwcx(BuilderContext& ctx) {
  EmitAtomicStoreConditional(ctx, "u32", "__builtin_bswap32", "u32");
  return true;
}

bool BuildStdcx(BuilderContext& ctx) {
  EmitAtomicStoreConditional(ctx, "u64", "__builtin_bswap64", "u64");
  return true;
}

bool BuildStd(BuilderContext& ctx) {
  ctx.EmitStoreDForm("REX_STORE_U64", "u64");
  return true;
}

bool BuildStdu(BuilderContext& ctx) {
  EmitStoreWithUpdate(ctx, "REX_STORE_U64", "u64");
  return true;
}

bool BuildStdx(BuilderContext& ctx) {
  ctx.EmitStoreXForm("REX_STORE_U64", "u64");
  return true;
}

bool BuildStdux(BuilderContext& ctx) {
  EmitStoreXFormWithUpdate(ctx, "REX_STORE_U64", "u64");
  return true;
}

bool BuildStfd(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U64"));
  if (ctx.insn.operands[2] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[2]));
  ctx.Println("{}, {}.u64);", static_cast<i32>(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[0]));
  return true;
}

bool BuildStfdx(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U64"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32, {}.u64);", ctx.R(ctx.insn.operands[2]), ctx.F(ctx.insn.operands[0]));
  return true;
}

bool BuildStfdux(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}({}, {}.u64);", ctx.MMIOMacro("REX_STORE_U64"), ctx.EA(),
              ctx.F(ctx.insn.operands[0]));
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
  return true;
}

bool BuildStfiwx(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U32"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32, {}.u32);", ctx.R(ctx.insn.operands[2]), ctx.F(ctx.insn.operands[0]));
  return true;
}

bool BuildSTFS(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f32 = f32({}.f64);", ctx.Temp(), ctx.F(ctx.insn.operands[0]));
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U32"));
  if (ctx.insn.operands[2] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[2]));
  ctx.Println("{}, {}.u32);", static_cast<i32>(ctx.insn.operands[1]), ctx.Temp());
  return true;
}

bool BuildStfsx(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f32 = f32({}.f64);", ctx.Temp(), ctx.F(ctx.insn.operands[0]));
  ctx.Print("\t{}(", ctx.MMIOMacro("REX_STORE_U32"));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32, {}.u32);", ctx.R(ctx.insn.operands[2]), ctx.Temp());
  return true;
}

bool BuildStfdu(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}({}, {}.u64);", ctx.MMIOMacro("REX_STORE_U64"), ctx.EA(),
              ctx.F(ctx.insn.operands[0]));
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
  return true;
}

bool BuildStfsu(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.f32 = f32({}.f64);", ctx.Temp(), ctx.F(ctx.insn.operands[0]));
  ctx.Println("\t{}({}, {}.u32);", ctx.MMIOMacro("REX_STORE_U32"), ctx.EA(), ctx.Temp());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
  return true;
}

bool BuildStfsux(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f32 = f32({}.f64);", ctx.Temp(), ctx.F(ctx.insn.operands[0]));
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}({}, {}.u32);", ctx.MMIOMacro("REX_STORE_U32"), ctx.EA(), ctx.Temp());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
  return true;
}

bool BuildLvx(BuilderContext& ctx) {
  // Endian swap reverses the whole vector, so builders account for it (dp3 sums yzw, not xyz)
  EmitVectorEA(ctx, "0xF");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR({})), "
      "simde_mm_load_si128((simde__m128i*)VectorMaskL)));",
      ctx.V(ctx.insn.operands[0]), ctx.EA());
  return true;
}

bool BuildLvlx(BuilderContext& ctx) {
  EmitVectorTempEA(ctx);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR({}.u32 & ~0xF)), "
      "simde_mm_load_si128((simde__m128i*)&VectorMaskL[({}.u32 & 0xF) * 16])));",
      ctx.V(ctx.insn.operands[0]), ctx.Temp(), ctx.Temp());
  return true;
}

bool BuildLvrx(BuilderContext& ctx) {
  EmitVectorTempEA(ctx);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, {}.u32 & 0xF ? "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR({}.u32 & ~0xF)), "
      "simde_mm_load_si128((simde__m128i*)&VectorMaskR[({}.u32 & 0xF) * 16])) : "
      "simde_mm_setzero_si128());",
      ctx.V(ctx.insn.operands[0]), ctx.Temp(), ctx.Temp(), ctx.Temp());
  return true;
}

bool BuildLvsl(BuilderContext& ctx) {
  EmitVectorTempEA(ctx);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_load_si128((simde__m128i*)&VectorShiftTableL[({}.u32 & 0xF) * 16]));",
      ctx.V(ctx.insn.operands[0]), ctx.Temp());
  return true;
}

bool BuildLvsr(BuilderContext& ctx) {
  EmitVectorTempEA(ctx);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_load_si128((simde__m128i*)&VectorShiftTableR[({}.u32 & 0xF) * 16]));",
      ctx.V(ctx.insn.operands[0]), ctx.Temp());
  return true;
}

bool BuildStvebx(BuilderContext& ctx) {
  EmitVectorEA(ctx);
  ctx.Println("\tREX_STORE_U8({}, {}.u8[15 - ({} & 0xF)]);", ctx.EA(), ctx.V(ctx.insn.operands[0]),
              ctx.EA());
  return true;
}

bool BuildStvehx(BuilderContext& ctx) {
  EmitVectorEA(ctx, "0x1");
  ctx.Println("\tREX_STORE_U16(ea, {}.u16[7 - (({} & 0xF) >> 1)]);", ctx.V(ctx.insn.operands[0]),
              ctx.EA());
  return true;
}

bool BuildStvewx(BuilderContext& ctx) {
  EmitVectorEA(ctx, "0x3");
  ctx.Println("\tREX_STORE_U32(ea, {}.u32[3 - (({} & 0xF) >> 2)]);", ctx.V(ctx.insn.operands[0]),
              ctx.EA());
  return true;
}

bool BuildStvlx(BuilderContext& ctx) {
  EmitVectorEA(ctx);

  ctx.Println("\tfor (size_t i = 0; i < (16 - ({} & 0xF)); i++)", ctx.EA());
  ctx.Println("\t\tREX_STORE_U8({} + i, {}.u8[15 - i]);", ctx.EA(), ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildStvrx(BuilderContext& ctx) {
  EmitVectorEA(ctx);

  ctx.Println("\tfor (size_t i = 0; i < ({} & 0xF); i++)", ctx.EA());
  ctx.Println("\t\tREX_STORE_U8({} - i - 1, {}.u8[i]);", ctx.EA(), ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildStvx(BuilderContext& ctx) {
  EmitVectorEA(ctx, "0xF");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*)REX_RAW_ADDR({}), "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*)VectorMaskL)));",
      ctx.EA(), ctx.V(ctx.insn.operands[0]));
  return true;
}

}  // namespace rex::codegen
