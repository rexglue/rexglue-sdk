/**
 * @file        codegen/builders/context.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

#include <algorithm>
#include <charconv>
#include <string_view>

#include <rex/codegen/binary_view.h>
#include <rex/logging.h>

#include <rex/runtime/exports.h>

namespace rex::codegen {

namespace {

struct MMIOMacroRow {
  std::string_view plain;
  std::string_view gpu;
  std::string_view apu;
  std::string_view guarded;
};

constexpr MMIOMacroRow kMMIOMacros[] = {
    {"REX_LOAD_U8", "REX_MM_LOAD_U8", "REX_MM_LOAD_U8", "REX_MM_LOAD_U8"},
    {"REX_LOAD_U16", "REX_MM_LOAD_U16", "REX_MM_LOAD_U16", "REX_MM_LOAD_U16"},
    {"REX_LOAD_U32", "REX_GPU_LOAD32", "REX_APU_LOAD32", "REX_MM_LOAD_U32"},
    {"REX_LOAD_U64", "REX_MM_LOAD_U64", "REX_MM_LOAD_U64", "REX_MM_LOAD_U64"},
    {"REX_STORE_U8", "REX_MM_STORE_U8", "REX_MM_STORE_U8", "REX_MM_STORE_U8"},
    {"REX_STORE_U16", "REX_MM_STORE_U16", "REX_MM_STORE_U16", "REX_MM_STORE_U16"},
    {"REX_STORE_U32", "REX_GPU_STORE32", "REX_APU_STORE32", "REX_MM_STORE_U32"},
    {"REX_STORE_U64", "REX_MM_STORE_U64", "REX_MM_STORE_U64", "REX_MM_STORE_U64"},
};

size_t RegisterIndex(std::string_view name, size_t prefix) {
  size_t index = 0;
  (void)std::from_chars(name.data() + prefix, name.data() + name.size(), index);
  return index;
}

}  // namespace

std::string_view BuilderContext::MMIOMacro(std::string_view plain) const {
  const MMIOAccess access = mmio ? mmio->At(base) : MMIOAccess::Plain;
  const auto* row = std::ranges::find(kMMIOMacros, plain, &MMIOMacroRow::plain);
  if (access == MMIOAccess::Plain || row == std::end(kMMIOMacros)) {
    return plain;
  }
  switch (access) {
    case MMIOAccess::GPU:
      return row->gpu;
    case MMIOAccess::APU:
      return row->apu;
    default:
      return row->guarded;
  }
}

const RecompilerConfig& BuilderContext::config() const {
  return emitCtx.config;
}

const FunctionGraph& BuilderContext::graph() const {
  return emitCtx.graph;
}

bool BuilderContext::localizeNonVolatiles() const {
  const auto& info = fn.exceptionInfo();
  const auto* seh = info ? info->asSeh() : nullptr;
  const bool sehScope = config().generateExceptionHandlers && seh && !seh->scopes.empty();
  return config().nonVolatileRegistersAsLocalVariables && !fn.sharesRegisters() && !sehScope;
}

std::string BuilderContext::r(size_t index) {
  const auto& cfg = config();
  if ((cfg.nonArgumentRegistersAsLocalVariables &&
       (index == 0 || index == 2 || index == 11 || index == 12)) ||
      (localizeNonVolatiles() && index >= 14)) {
    locals.r[index] = true;
    return std::format("r{}", index);
  }
  return std::format("ctx.r{}", index);
}

std::string BuilderContext::f(size_t index) {
  const auto& cfg = config();
  if ((cfg.nonArgumentRegistersAsLocalVariables && index == 0) ||
      (localizeNonVolatiles() && index >= 14)) {
    locals.f[index] = true;
    return std::format("f{}", index);
  }
  return std::format("ctx.f{}", index);
}

std::string BuilderContext::v(size_t index) {
  const auto& cfg = config();
  if ((cfg.nonArgumentRegistersAsLocalVariables && (index >= 32 && index <= 63)) ||
      (localizeNonVolatiles() && ((index >= 14 && index <= 31) || (index >= 64 && index <= 127)))) {
    locals.v[index] = true;
    return std::format("v{}", index);
  }
  return std::format("ctx.v{}", index);
}

std::string BuilderContext::cr(size_t index) {
  if (config().crRegistersAsLocalVariables) {
    locals.cr[index] = true;
    return std::format("cr{}", index);
  }
  return std::format("ctx.cr{}", index);
}

const char* BuilderContext::ctr() {
  if (config().ctrAsLocalVariable) {
    locals.ctr = true;
    return "ctr";
  }
  return "ctx.ctr";
}

const char* BuilderContext::xer() {
  if (config().xerAsLocalVariable) {
    locals.xer = true;
    return "xer";
  }
  return "ctx.xer";
}

const char* BuilderContext::reserved() {
  if (config().reservedRegisterAsLocalVariable) {
    locals.reserved = true;
    return "reserved";
  }
  return "ctx.reserved";
}

const char* BuilderContext::temp() {
  locals.temp = true;
  return "temp";
}

const char* BuilderContext::v_temp() {
  locals.v_temp = true;
  return "vTemp";
}

const char* BuilderContext::env() {
  locals.env = true;
  return "env";
}

const char* BuilderContext::ea() {
  locals.ea = true;
  return "ea";
}

const CallTarget* BuilderContext::findCallTarget(uint32_t site) const {
  for (const auto& edge : fn.calls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  for (const auto& edge : fn.tailCalls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  return nullptr;
}

void BuilderContext::emit_function_call(uint32_t address) {
  const auto& cfg = config();

  if (address == cfg.longJmpAddress) {
    println("\tppc_longjmp({}.u32, {}.s32);", r(3), r(4));
    return;
  }

  if (address == cfg.setJmpAddress) {
    println("\t{} = ctx;", env());
    println("\t{}.s64 = ppc_setjmp({}.u32);", temp(), r(3));
    println("\tif ({}.s64 != 0) ctx = {};", temp(), env());
    println("\t{} = {};", r(3), temp());
    return;
  }

  if (const auto* target = findCallTarget(base)) {
    if (target->isFunction()) {
      auto* targetFn = target->asFunction();
      const auto& name = targetFn->name();

      if (cfg.nonVolatileRegistersAsLocalVariables &&
          (name.find("__rest") == 0 || name.find("__save") == 0)) {
        return;
      }

      if (targetFn->sharesRegisters() && localizeNonVolatiles()) {
        for (size_t i = 14; i < 32; ++i) {
          if (locals.r[i])
            println("\tctx.r{} = r{};", i, i);
        }
        emitCtx.reference(name);
        println("\t{}(ctx, base);", name);
        for (size_t i = 14; i < 32; ++i) {
          if (locals.r[i])
            println("\tr{} = ctx.r{};", i, i);
        }
        return;
      }

      emitCtx.reference(name);
      println("\t{}(ctx, base);", name);
      return;
    }

    if (target->isImport()) {
      const auto& importTarget = std::get<CallTarget::ToImport>(target->value);
      std::string func_name;

      auto at_pos = importTarget.name.find('@');
      if (at_pos != std::string::npos) {
        auto lib_name = importTarget.name.substr(0, at_pos);
        auto ordinal_str = importTarget.name.substr(at_pos + 1);
        uint16_t ordinal = static_cast<uint16_t>(std::stoul(ordinal_str));

        const auto exportModule = runtime::ModuleFromName(lib_name);
        const auto* exp = exportModule ? runtime::FindByOrdinal(*exportModule, ordinal) : nullptr;
        if (exp) {
          func_name = "__imp__" + std::string(exp->name);
        }
      }

      if (func_name.empty()) {
        func_name = "__imp__" + importTarget.name;
        std::replace(func_name.begin(), func_name.end(), '@', '_');
        std::replace(func_name.begin(), func_name.end(), '.', '_');
      }

      emitCtx.reference(func_name);
      println("\t{}(ctx, base);", func_name);
      return;
    }

    REXLOG_ERROR(codegen, "Unresolved function 0x{:08X} from 0x{:08X}", address, base);
    println("\t// FATAL: unresolved function 0x{:08X}", address);
    println("\tREX_FATAL(cpu, \"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
    return;
  }

  REXLOG_ERROR(codegen,
               "Unresolved function 0x{:08X} from 0x{:08X} (no CallTarget in FunctionNode)",
               address, base);
  println("\t// FATAL: unresolved function 0x{:08X} (no CallTarget in FunctionNode)", address);
  println("\tREX_FATAL(cpu, \"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
}

void BuilderContext::emit_conditional_branch(bool not_, std::string_view cond) {
  uint32_t target = insn.operands[1];

  auto kind = graph().classifyTarget(target, base, false);

  switch (kind) {
    case TargetKind::InternalLabel:
      println("\tif ({}{}.{}) goto loc_{:08X};", not_ ? "!" : "", cr(insn.operands[0]), cond,
              target);
      break;

    case TargetKind::Function:
    case TargetKind::Import:
      if (const auto* callTarget = findCallTarget(base)) {
        if (callTarget->isFunction()) {
          auto* targetFn = callTarget->asFunction();
          emitCtx.reference(targetFn->name());
          println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
          println("\t\t{}(ctx, base);", targetFn->name());
          println("\t\treturn;");
          println("\t}}");
        } else if (callTarget->isImport()) {
          const auto& importTarget = std::get<CallTarget::ToImport>(callTarget->value);
          std::string func_name = "__imp__" + importTarget.name;
          std::replace(func_name.begin(), func_name.end(), '@', '_');
          std::replace(func_name.begin(), func_name.end(), '.', '_');
          emitCtx.reference(func_name);
          println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
          println("\t\t{}(ctx, base);", func_name);
          println("\t\treturn;");
          println("\t}}");
        }
      } else {
        REXLOG_ERROR(codegen,
                     "Unresolved conditional branch to 0x{:08X} from 0x{:08X} (no CallTarget)",
                     target, base);
        println("\tif ({}{}.{}) REX_FATAL(cpu, \"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
                not_ ? "!" : "", cr(insn.operands[0]), cond, base, target);
      }
      break;

    case TargetKind::Unknown:
      REXLOG_ERROR(codegen, "Unresolved conditional branch to 0x{:08X} from 0x{:08X}", target,
                   base);
      println("\t// ERROR: conditional branch to unknown address 0x{:08X}", target);
      println("\tif ({}{}.{}) REX_FATAL(cpu, \"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
              not_ ? "!" : "", cr(insn.operands[0]), cond, base, target);
      break;
  }
}

void BuilderContext::emit_set_flush_mode(bool enable) {
  auto newState = enable ? CSRState::VMX : CSRState::FPU;
  if (csrState != newState) {
    auto prefix = enable ? "enable" : "disable";
    auto suffix = csrState != CSRState::Unknown ? "Unconditional" : "";
    println("\tctx.fpscr.{}FlushMode{}();", prefix, suffix);

    csrState = newState;
  }
}

bool BuilderContext::has_mid_asm_hook() const {
  return config().midAsmHooks.find(base) != config().midAsmHooks.end();
}

void BuilderContext::emit_mid_asm_hook() {
  auto midAsmHook = config().midAsmHooks.find(base);
  if (midAsmHook == config().midAsmHooks.end()) {
    return;
  }

  bool returnsBool = midAsmHook->second.returnOnFalse || midAsmHook->second.returnOnTrue ||
                     midAsmHook->second.jumpAddressOnFalse != 0 ||
                     midAsmHook->second.jumpAddressOnTrue != 0;

  const std::string r1 = r(1);
  println("\t{}.u64 -= rex::ppc::kRedZone;", r1);
  print("\t");
  if (returnsBool)
    print("if (");

  print("{}(", midAsmHook->second.name);
  for (auto& reg : midAsmHook->second.registers) {
    if (out.back() != '(')
      out += ", ";

    switch (reg[0]) {
      case 'c':
        if (reg == "ctr")
          out += ctr();
        else
          out += cr(RegisterIndex(reg, 2));
        break;
      case 'x':
        out += xer();
        break;
      case 'r':
        if (reg == "reserved")
          out += reserved();
        else
          out += r(RegisterIndex(reg, 1));
        break;
      case 'f':
        if (reg == "fpscr")
          out += "ctx.fpscr";
        else
          out += f(RegisterIndex(reg, 1));
        break;
      case 'v':
        out += v(RegisterIndex(reg, 1));
        break;
      default:
        break;
    }
  }

  if (returnsBool) {
    println(")) {{");
    println("\t\t{}.u64 += rex::ppc::kRedZone;", r1);

    if (midAsmHook->second.returnOnTrue)
      println("\t\treturn;");
    else if (midAsmHook->second.jumpAddressOnTrue != 0)
      println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnTrue);

    println("\t}}");

    println("\telse {{");
    println("\t\t{}.u64 += rex::ppc::kRedZone;", r1);

    if (midAsmHook->second.returnOnFalse)
      println("\t\treturn;");
    else if (midAsmHook->second.jumpAddressOnFalse != 0)
      println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnFalse);

    println("\t}}");
  } else {
    println(");");
    println("\t{}.u64 += rex::ppc::kRedZone;", r1);

    if (midAsmHook->second.ret)
      println("\treturn;");
    else if (midAsmHook->second.jumpAddress != 0)
      println("\tgoto loc_{:X};", midAsmHook->second.jumpAddress);
  }
}

void BuilderContext::emit_vec_fp_binary(const char* simd_op) {
  println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_{}_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)));",
      v(insn.operands[0]), simd_op, v(insn.operands[1]), v(insn.operands[2]));
}

void BuilderContext::emit_vec_fp_unary_expr(std::string_view simd_expr) {
  auto vD = v(insn.operands[0]);
  auto vA = v(insn.operands[1]);

  std::string expr(simd_expr);
  size_t pos;
  while ((pos = expr.find("{vA}")) != std::string::npos) {
    expr.replace(pos, 4, vA);
  }

  println("\tsimde_mm_store_ps({}.f32, {});", vD, expr);
}

void BuilderContext::emit_vec_int_binary(const char* simd_op, const char* element_type) {
  println(
      "\tsimde_mm_store_si128((simde__m128i*){}.{}, "
      "simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), "
      "simde_mm_load_si128((simde__m128i*){}.{})));",
      v(insn.operands[0]), element_type, simd_op, v(insn.operands[1]), element_type,
      v(insn.operands[2]), element_type);
}

void BuilderContext::emit_vec_int_binary_swapped(const char* simd_op, const char* element_type) {
  println(
      "\tsimde_mm_store_si128((simde__m128i*){}.{}, "
      "simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), "
      "simde_mm_load_si128((simde__m128i*){}.{})));",
      v(insn.operands[0]), element_type, simd_op, v(insn.operands[2]), element_type,
      v(insn.operands[1]), element_type);
}

void BuilderContext::emit_vec_var_shift(const char* shift_dir, const char* element_type,
                                        uint32_t mask_value) {
  auto vD = v(insn.operands[0]);
  auto vA = v(insn.operands[1]);
  auto vB = v(insn.operands[2]);
  println("\t{{");
  println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", vA);
  println("\t\tsimde__m128i b = simde_mm_load_si128((simde__m128i*){}.u8);", vB);
  println("\t\tsimde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_{}(0x{:X}));", element_type,
          mask_value);
  println(
      "\t\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_{}_{}"
      "(a, shift));",
      vD, shift_dir, element_type);
  println("\t}}");
}

void BuilderContext::emit_load_d_form(const char* load_macro, const char* dest_type) {
  print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, MMIOMacro(load_macro));
  if (insn.operands[2] != 0)
    print("{}.u32 + ", r(insn.operands[2]));
  println("{});", static_cast<int32_t>(insn.operands[1]));
}

void BuilderContext::emit_load_x_form(const char* load_macro, const char* dest_type) {
  print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, MMIOMacro(load_macro));
  if (insn.operands[1] != 0)
    print("{}.u32 + ", r(insn.operands[1]));
  println("{}.u32);", r(insn.operands[2]));
}

void BuilderContext::emit_store_d_form(const char* store_macro, const char* src_type) {
  print("\t{}(", MMIOMacro(store_macro));
  if (insn.operands[2] != 0)
    print("{}.u32 + ", r(insn.operands[2]));
  println("{}, {}.{});", static_cast<int32_t>(insn.operands[1]), r(insn.operands[0]), src_type);
}

void BuilderContext::emit_store_x_form(const char* store_macro, const char* src_type) {
  print("\t{}(", MMIOMacro(store_macro));
  if (insn.operands[1] != 0)
    print("{}.u32 + ", r(insn.operands[1]));
  println("{}.u32, {}.{});", r(insn.operands[2]), r(insn.operands[0]), src_type);
}

}  // namespace rex::codegen
