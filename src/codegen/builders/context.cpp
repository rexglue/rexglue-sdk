/**
 * @file        rexcodegen/builders/context.cpp
 * @brief       PPC context instruction code generation
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
#include <iterator>

#include <fmt/format.h>

#include <rex/codegen/binary_view.h>
#include <rex/logging.h>

#include "../codegen_logging.h"
#include <rex/system/export_resolver.h>

namespace rex::codegen {

/// eieio instruction encoding (big-endian). Used for MMIO detection:
/// if the next instruction after a load/store is eieio, the access is MMIO.
static constexpr uint32_t kEieioEncoding = 0xAC06007C;

//=============================================================================
// Convenience Accessors
//=============================================================================

const RecompilerConfig& BuilderContext::config() const {
  return emitCtx.config;
}

const FunctionGraph& BuilderContext::graph() const {
  return emitCtx.graph;
}

//=============================================================================
// Register Accessors
//=============================================================================

namespace {
bool IsRegisterHelper(const std::string& name) {
  return name.rfind("__save", 0) == 0 || name.rfind("__rest", 0) == 0;
}
}  // namespace

// Localizing a non-volatile assumes the function owns it across its whole body.
// An SEH funclet does not: it inherits its owner's live registers through ctx.
// Two more kinds keep their non-volatiles in ctx:
// - The register save/restore helpers (__savegprlr_N, __restgprlr_N, ...).
//   Localized code never calls them (the calls are elided), but code that
//   keeps registers in ctx does: hand-written overrides copied from
//   ctx-based generated code, and the functions below. Their bodies must keep
//   saving and restoring ctx, or one such function calling another loses its
//   non-volatile registers.
// - Functions that call setjmp: it is emitted as a copy of ctx that longjmp
//   restores, and registers held in C++ locals would not be part of it.
bool BuilderContext::localizeNonVolatiles() const {
  if (localizeNonVolatilesCache >= 0) {
    return localizeNonVolatilesCache != 0;
  }
  bool localize = config().nonVolatileRegistersAsLocalVariables && !fn.sharesRegisters();
  if (localize) {
    if (IsRegisterHelper(fn.name())) {
      localize = false;
    }
  }
  if (localize && config().setJmpAddress) {
    for (const auto& edge : fn.calls()) {
      uint32_t target = 0;
      if (const auto* f = edge.target.asFunction()) {
        target = f->base();
      } else if (const auto* u = std::get_if<CallTarget::Unresolved>(&edge.target.value)) {
        target = u->address;
      } else if (const auto* i = std::get_if<CallTarget::ToImport>(&edge.target.value)) {
        target = i->address;
      }
      if (target == config().setJmpAddress) {
        localize = false;
        break;
      }
    }
  }
  localizeNonVolatilesCache = localize ? 1 : 0;
  return localize;
}

// A function that shares registers (an SEH funclet or a split-off fragment,
// see share_registers_functions) reads and writes its caller's registers
// through ctx. A caller that keeps registers in C++ locals hands them over
// around the call. Which registers are local is only known once the whole
// function is emitted, so this writes markers that the function emitter
// expands (ExpandRegisterHandovers): before the call every local register goes
// to ctx, the non-volatile ones after saving the ctx value (it belongs to this
// function's caller, which may keep its registers in ctx); after the call the
// locals are read back and the saved non-volatiles restored, as the epilogue
// would have done.
void BuilderContext::emit_shared_call(const FunctionNode* targetFn, std::string_view indent) {
  const auto& name = targetFn->name();
  emitCtx.reference(name);
  bool handover = targetFn->sharesRegisters() &&
                  (config().nonVolatileRegistersAsLocalVariables ||
                   config().nonArgumentRegistersAsLocalVariables) &&
                  !fn.sharesRegisters();
  if (handover)
    println("{}{}", indent, kHandoverToMarker);
  println("{}{}(ctx, base);", indent, name);
  if (handover)
    println("{}{}", indent, kHandoverBackMarker);
}

namespace {
bool IsNonVolatileGprFpr(size_t i) {
  return i >= 14;
}
bool IsNonVolatileVr(size_t i) {
  return (i >= 14 && i < 32) || i >= 64;
}
}  // namespace

bool ExpandRegisterHandovers(std::string& body, const RecompilerLocalVariables& locals) {
  if (body.find(kHandoverToMarker) == std::string::npos &&
      body.find(kHandoverBackMarker) == std::string::npos) {
    return false;
  }
  std::string out;
  out.reserve(body.size() + body.size() / 8);
  size_t pos = 0;
  while (pos < body.size()) {
    size_t eol = body.find('\n', pos);
    size_t next = eol == std::string::npos ? body.size() : eol + 1;
    std::string_view line(body.data() + pos, next - pos);
    size_t to = line.find(kHandoverToMarker);
    size_t back = to == std::string_view::npos ? line.find(kHandoverBackMarker)
                                               : std::string_view::npos;
    if (to == std::string_view::npos && back == std::string_view::npos) {
      out.append(line);
      pos = next;
      continue;
    }
    std::string_view indent = line.substr(0, to != std::string_view::npos ? to : back);
    auto copy = [&](char kind, size_t i, bool nonVolatile) {
      out.append(indent);
      if (to != std::string_view::npos) {
        if (nonVolatile)
          fmt::format_to(std::back_inserter(out), "ho_{0}{1} = ctx.{0}{1}; ", kind, i);
        fmt::format_to(std::back_inserter(out), "ctx.{0}{1} = {0}{1};\n", kind, i);
      } else {
        fmt::format_to(std::back_inserter(out), "{0}{1} = ctx.{0}{1};", kind, i);
        if (nonVolatile)
          fmt::format_to(std::back_inserter(out), " ctx.{0}{1} = ho_{0}{1};", kind, i);
        out += '\n';
      }
    };
    for (size_t i = 0; i < 32; ++i) {
      if (locals.r[i])
        copy('r', i, IsNonVolatileGprFpr(i));
    }
    for (size_t i = 0; i < 32; ++i) {
      if (locals.f[i])
        copy('f', i, IsNonVolatileGprFpr(i));
    }
    for (size_t i = 0; i < 128; ++i) {
      if (locals.v[i])
        copy('v', i, IsNonVolatileVr(i));
    }
    pos = next;
  }
  body = std::move(out);
  return true;
}

bool HandoverSavesRegister(char kind, size_t i) {
  return kind == 'v' ? IsNonVolatileVr(i) : IsNonVolatileGprFpr(i);
}

bool BuilderContext::localizeNonArguments() const {
  return config().nonArgumentRegistersAsLocalVariables && !fn.sharesRegisters() &&
         !IsRegisterHelper(fn.name());
}

std::string BuilderContext::r(size_t index) {
  if ((localizeNonArguments() &&
       (index == 0 || index == 2 || index == 11 || index == 12)) ||
      (localizeNonVolatiles() && index >= 14)) {
    locals.r[index] = true;
    return fmt::format("r{}", index);
  }
  return fmt::format("ctx.r{}", index);
}

std::string BuilderContext::f(size_t index) {
  if ((localizeNonArguments() && index == 0) ||
      (localizeNonVolatiles() && index >= 14)) {
    locals.f[index] = true;
    return fmt::format("f{}", index);
  }
  return fmt::format("ctx.f{}", index);
}

std::string BuilderContext::v(size_t index) {
  if ((localizeNonArguments() && (index >= 32 && index <= 63)) ||
      (localizeNonVolatiles() && ((index >= 14 && index <= 31) || (index >= 64 && index <= 127)))) {
    locals.v[index] = true;
    return fmt::format("v{}", index);
  }
  return fmt::format("ctx.v{}", index);
}

std::string BuilderContext::cr(size_t index) {
  if (config().crRegistersAsLocalVariables) {
    locals.cr[index] = true;
    return fmt::format("cr{}", index);
  }
  return fmt::format("ctx.cr{}", index);
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

//=============================================================================
// Output Helpers
//=============================================================================

// Template implementations in header, but we need explicit instantiations
// for common format strings to avoid link errors in some cases

//=============================================================================
// Code Generation Helpers
//=============================================================================

bool BuilderContext::mmio_check_d_form() {
  if (base + 4 < fn.end() && *(data + 1) == kEieioEncoding)
    return true;
  return locals.is_mmio_base(insn.operands[2]);
}

bool BuilderContext::mmio_check_x_form() {
  if (base + 4 < fn.end() && *(data + 1) == kEieioEncoding)
    return true;
  return locals.is_mmio_base(insn.operands[1]) || locals.is_mmio_base(insn.operands[2]);
}

const CallTarget* BuilderContext::findCallTarget(uint32_t site) const {
  // Search in calls (bl instructions)
  for (const auto& edge : fn.calls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  // Search in tail calls (b instructions to other functions)
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
    // Use custom ppc_longjmp that uses guest address as key (not for storage)
    println("\tppc_longjmp({}.u32, {}.s32);", r(3), r(4));
    return;
  }

  if (address == cfg.setJmpAddress) {
    // Save PPCContext for restoration after longjmp
    println("\t{} = ctx;", env());
    // Use custom ppc_setjmp that uses guest address as key
    println("\t{}.s64 = ppc_setjmp({}.u32);", temp(), r(3));
    // Restore PPCContext if returning from longjmp
    println("\tif ({}.s64 != 0) ctx = {};", temp(), env());
    println("\t{} = {};", r(3), temp());
    return;
  }

  // Try to use pre-resolved call target from FunctionGraph
  if (const auto* target = findCallTarget(base)) {
    if (target->isFunction()) {
      auto* targetFn = target->asFunction();
      const auto& name = targetFn->name();

      // Handle save/restore helpers: a function with localized non-volatiles
      // keeps them in C++ locals, so it has nothing to save or restore. A
      // function that keeps its non-volatiles in ctx (setjmp callers, functions
      // that share registers) calls them, and their bodies save and restore ctx
      // (see localizeNonVolatiles): many share_registers functions are whole
      // functions whose callers read their registers back from ctx afterwards.
      if (cfg.nonVolatileRegistersAsLocalVariables && localizeNonVolatiles() &&
          (name.find("__rest") == 0 || name.find("__save") == 0)) {
        // print nothing - these are handled by local variable tracking
        return;
      }

      // An SEH funclet or split-off fragment runs on its owner's frame and
      // reads whatever registers the owner left live (emit_shared_call).
      emit_shared_call(targetFn, "\t");
      return;
    }

    if (target->isImport()) {
      const auto& importTarget = std::get<CallTarget::ToImport>(target->value);
      std::string func_name;

      // Try to resolve ordinal to actual function name
      auto at_pos = importTarget.name.find('@');
      if (at_pos != std::string::npos && emitCtx.resolver) {
        auto lib_name = importTarget.name.substr(0, at_pos);
        auto ordinal_str = importTarget.name.substr(at_pos + 1);
        uint16_t ordinal = static_cast<uint16_t>(std::stoul(ordinal_str));

        auto* exp = emitCtx.resolver->GetExportByOrdinal(lib_name + ".xex", ordinal);
        if (!exp)
          exp = emitCtx.resolver->GetExportByOrdinal(lib_name, ordinal);

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

    // Unresolved target from graph
    REXCODEGEN_ERROR("Unresolved function 0x{:08X} from 0x{:08X}", address, base);
    println("\t// FATAL: unresolved function 0x{:08X}", address);
    println("\tREX_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
    return;
  }

  // No pre-resolved target found - this is an error
  REXCODEGEN_ERROR("Unresolved function 0x{:08X} from 0x{:08X} (no CallTarget in FunctionNode)",
                   address, base);
  println("\t// FATAL: unresolved function 0x{:08X} (no CallTarget in FunctionNode)", address);
  println("\tREX_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
}

void BuilderContext::emit_conditional_branch(bool not_, std::string_view cond) {
  uint32_t target = insn.operands[1];

  // Use classifyTarget for consistent branch classification
  // false = branch instruction (not a call), so own-base means loop back
  auto kind = graph().classifyTarget(target, base, false);

  switch (kind) {
    case TargetKind::InternalLabel:
      // Target is within this function - local goto
      println("\tif ({}{}.{}) goto loc_{:08X};", not_ ? "!" : "", cr(insn.operands[0]), cond,
              target);
      break;

    case TargetKind::Function:
    case TargetKind::Import:
      // Conditional tail call to another function - check pre-resolved call target
      if (const auto* callTarget = findCallTarget(base)) {
        if (callTarget->isFunction()) {
          auto* targetFn = callTarget->asFunction();
          println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
          emit_shared_call(targetFn, "\t\t");
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
        REXCODEGEN_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X} (no CallTarget)",
                         target, base);
        println("\tif ({}{}.{}) REX_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
                not_ ? "!" : "", cr(insn.operands[0]), cond, base, target);
      }
      break;

    case TargetKind::Unknown:
      REXCODEGEN_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X}", target, base);
      println("\t// ERROR: conditional branch to unknown address 0x{:08X}", target);
      println("\tif ({}{}.{}) REX_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
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

  print("\t");
  if (returnsBool)
    print("if (");

  // Build call -- no ctx/base prefix, just register arguments resolved through accessors
  print("{}(", midAsmHook->second.name);
  for (auto& reg : midAsmHook->second.registers) {
    if (out.back() != '(')
      out += ", ";

    switch (reg[0]) {
      case 'c':
        if (reg == "ctr")
          out += ctr();
        else
          out += cr(std::atoi(reg.c_str() + 2));
        break;
      case 'x':
        out += xer();
        break;
      case 'r':
        if (reg == "reserved")
          out += reserved();
        else
          out += r(std::atoi(reg.c_str() + 1));
        break;
      case 'f':
        if (reg == "fpscr")
          out += "ctx.fpscr";
        else
          out += f(std::atoi(reg.c_str() + 1));
        break;
      case 'v':
        out += v(std::atoi(reg.c_str() + 1));
        break;
    }
  }

  if (returnsBool) {
    println(")) {{");

    if (midAsmHook->second.returnOnTrue)
      println("\t\treturn;");
    else if (midAsmHook->second.jumpAddressOnTrue != 0)
      println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnTrue);

    println("\t}}");

    println("\telse {{");

    if (midAsmHook->second.returnOnFalse)
      println("\t\treturn;");
    else if (midAsmHook->second.jumpAddressOnFalse != 0)
      println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnFalse);

    println("\t}}");
  } else {
    println(");");

    if (midAsmHook->second.ret)
      println("\treturn;");
    else if (midAsmHook->second.jumpAddress != 0)
      println("\tgoto loc_{:X};", midAsmHook->second.jumpAddress);
  }
}

//=============================================================================
// Vector (SIMD) Code Generation Helpers
//=============================================================================

void BuilderContext::emit_vec_fp_binary(const char* simd_op) {
  println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_{}_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)));",
      v(insn.operands[0]), simd_op, v(insn.operands[1]), v(insn.operands[2]));
}

void BuilderContext::emit_vec_fp_unary_expr(std::string_view simd_expr) {
  auto vD = v(insn.operands[0]);
  auto vA = v(insn.operands[1]);

  // Replace {vA} placeholder in expression with actual register
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
  // Swapped: op(vB, vA) instead of op(vA, vB) - useful for andnot which has reversed operand
  // semantics
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

//=============================================================================
// Memory (Load/Store) Code Generation Helpers
//=============================================================================

void BuilderContext::emit_load_d_form(const char* load_macro, const char* dest_type,
                                      bool check_mmio) {
  // D-form: rD = LOAD(rA + D) where operands[0]=rD, operands[1]=D, operands[2]=rA
  // load_macro should be like "REX_LOAD_U8" - we replace REX_LOAD with REX_MM_LOAD for MMIO
  const char* macro = load_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_d_form()) {
    // Replace "REX_LOAD_" with "REX_MM_LOAD_"
    if (strncmp(load_macro, "REX_LOAD_", 9) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_LOAD_%s", load_macro + 9);
      macro = mm_macro;
    }
  }

  print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, macro);
  if (insn.operands[2] != 0)
    print("{}.u32 + ", r(insn.operands[2]));
  println("{});", static_cast<int32_t>(insn.operands[1]));
}

void BuilderContext::emit_load_x_form(const char* load_macro, const char* dest_type,
                                      bool check_mmio) {
  // X-form: rD = LOAD(rA + rB) where operands[0]=rD, operands[1]=rA, operands[2]=rB
  // load_macro should be like "REX_LOAD_U8" - we replace REX_LOAD with REX_MM_LOAD for MMIO
  const char* macro = load_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_x_form()) {
    // Replace "REX_LOAD_" with "REX_MM_LOAD_"
    if (strncmp(load_macro, "REX_LOAD_", 9) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_LOAD_%s", load_macro + 9);
      macro = mm_macro;
    }
  }

  print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, macro);
  if (insn.operands[1] != 0)
    print("{}.u32 + ", r(insn.operands[1]));
  println("{}.u32);", r(insn.operands[2]));
}

void BuilderContext::emit_store_d_form(const char* store_macro, const char* src_type,
                                       bool check_mmio) {
  // D-form: STORE(rA + D, rS) where operands[0]=rS, operands[1]=D, operands[2]=rA
  // store_macro should be like "REX_STORE_U8" - we replace REX_STORE with REX_MM_STORE for MMIO
  const char* macro = store_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_d_form()) {
    // Replace "REX_STORE_" with "REX_MM_STORE_"
    if (strncmp(store_macro, "REX_STORE_", 10) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_STORE_%s", store_macro + 10);
      macro = mm_macro;
    }
  }

  print("\t{}(", macro);
  if (insn.operands[2] != 0)
    print("{}.u32 + ", r(insn.operands[2]));
  println("{}, {}.{});", static_cast<int32_t>(insn.operands[1]), r(insn.operands[0]), src_type);
}

void BuilderContext::emit_store_x_form(const char* store_macro, const char* src_type,
                                       bool check_mmio) {
  // X-form: STORE(rA + rB, rS) where operands[0]=rS, operands[1]=rA, operands[2]=rB
  // store_macro should be like "REX_STORE_U8" - we replace REX_STORE with REX_MM_STORE for MMIO
  const char* macro = store_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_x_form()) {  // Use X-form specific check (operands[1] is base)
    // Replace "REX_STORE_" with "REX_MM_STORE_"
    if (strncmp(store_macro, "REX_STORE_", 10) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_STORE_%s", store_macro + 10);
      macro = mm_macro;
    }
  }

  print("\t{}(", macro);
  if (insn.operands[1] != 0)
    print("{}.u32 + ", r(insn.operands[1]));
  println("{}.u32, {}.{});", r(insn.operands[2]), r(insn.operands[0]), src_type);
}

}  // namespace rex::codegen
