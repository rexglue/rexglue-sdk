#include "builder_context.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string_view>

#include "binary_view.h"
#include <rex/logging/log_category.h>
#include "codegen_categories.h"

#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/export.h>

#include <dis-asm.h>
#include <ppc-inst.h>

namespace rex::codegen {

namespace {

struct MMIOMacroRow {
  std::string_view plain;
  std::string_view gpu;
  std::string_view apu;
  std::string_view guarded;
  std::string_view stack;
};

constexpr MMIOMacroRow kMMIOMacros[] = {
    {"REX_LOAD_U8", "REX_MM_LOAD_U8", "REX_MM_LOAD_U8", "REX_MM_LOAD_U8", "REX_STACK_LOAD_U8"},
    {"REX_LOAD_U16", "REX_MM_LOAD_U16", "REX_MM_LOAD_U16", "REX_MM_LOAD_U16", "REX_STACK_LOAD_U16"},
    {"REX_LOAD_U32", "REX_GPU_LOAD32", "REX_APU_LOAD32", "REX_MM_LOAD_U32", "REX_STACK_LOAD_U32"},
    {"REX_LOAD_U64", "REX_MM_LOAD_U64", "REX_MM_LOAD_U64", "REX_MM_LOAD_U64", "REX_STACK_LOAD_U64"},
    {"REX_STORE_U8", "REX_MM_STORE_U8", "REX_MM_STORE_U8", "REX_MM_STORE_U8", "REX_STACK_STORE_U8"},
    {"REX_STORE_U16", "REX_MM_STORE_U16", "REX_MM_STORE_U16", "REX_MM_STORE_U16",
     "REX_STACK_STORE_U16"},
    {"REX_STORE_U32", "REX_GPU_STORE32", "REX_APU_STORE32", "REX_MM_STORE_U32",
     "REX_STACK_STORE_U32"},
    {"REX_STORE_U64", "REX_MM_STORE_U64", "REX_MM_STORE_U64", "REX_MM_STORE_U64",
     "REX_STACK_STORE_U64"},
};

size_t RegisterIndex(std::string_view name, size_t prefix) {
  size_t index = 0;
  (void)std::from_chars(name.data() + prefix, name.data() + name.size(), index);
  return index;
}

}  // namespace

u8 AccessWidth(std::string_view macro) {
  if (macro.ends_with("U8"))
    return 1;
  if (macro.ends_with("U16"))
    return 2;
  if (macro.ends_with("U32"))
    return 4;
  return 8;
}

std::string_view BuilderContext::MMIOMacro(std::string_view plain) const {
  const MMIOAccess access = mmio ? mmio->At(base) : MMIOAccess::kPlain;
  const auto* row = std::ranges::find(kMMIOMacros, plain, &MMIOMacroRow::plain);
  if (row == std::end(kMMIOMacros)) {
    return plain;
  }
  if (access == MMIOAccess::kPlain) {
    return IsStackAccess() ? row->stack : plain;
  }
  switch (access) {
    case MMIOAccess::kGPU:
      return row->gpu;
    case MMIOAccess::kAPU:
      return row->apu;
    default:
      return row->guarded;
  }
}

std::string_view BuilderContext::StackMacro(std::string_view plain) const {
  const auto* row = std::ranges::find(kMMIOMacros, plain, &MMIOMacroRow::plain);
  if (row == std::end(kMMIOMacros) || !IsStackAccess()) {
    return plain;
  }
  return row->stack;
}

bool BuilderContext::IsStackAccess() const {
  const std::string_view name = insn.opcode->name;
  const bool indexed = name.ends_with('x') || name.ends_with("x.");
  return insn.operands[indexed ? 1 : 2] == 1;
}

const RecompilerConfig& BuilderContext::GetConfig() const {
  return emit_ctx.config;
}

const FunctionGraph& BuilderContext::GetGraph() const {
  return emit_ctx.graph;
}

std::optional<std::string> BuilderContext::HoldStackWrite(u8 width) {
  if (forwarding == nullptr || insn.operands[2] != 1) {
    return std::nullopt;
  }
  if (mmio && mmio->At(base) != MMIOAccess::kPlain) {
    forwarding->Invalidate();
    return std::nullopt;
  }
  const u8 slot = forwarding->HoldSlot(static_cast<i32>(insn.operands[1]), width);
  locals.fwd = std::max<u8>(locals.fwd, static_cast<u8>(slot + 1));
  return std::format("fwd{}", slot);
}

std::optional<std::string> BuilderContext::FindStackWrite(u8 width) const {
  if (forwarding == nullptr || insn.operands[2] != 1) {
    return std::nullopt;
  }
  if (mmio && mmio->At(base) != MMIOAccess::kPlain) {
    return std::nullopt;
  }
  if (const auto slot = forwarding->FindSlot(static_cast<i32>(insn.operands[1]), width)) {
    return std::format("fwd{}", *slot);
  }
  return std::nullopt;
}

bool BuilderContext::CanLocalizeNonVolatiles() const {
  const auto& info = fn.GetExceptionInfo();
  const auto* seh = info ? info->AsSEH() : nullptr;
  const bool seh_scope = GetConfig().generate_exception_handlers && seh && !seh->scopes.empty();
  return GetConfig().non_volatile_registers_as_local_variables && !fn.GetMillicode() &&
         !fn.IsRegisterSharing() && !seh_scope;
}

std::string BuilderContext::R(size_t index) {
  const auto& cfg = GetConfig();
  if (!fn.GetMillicode() && ((cfg.non_argument_registers_as_local_variables &&
                              (index == 0 || index == 11 || index == 12)) ||
                             (CanLocalizeNonVolatiles() && index >= 14))) {
    locals.r[index] = true;
    return std::format("r{}", index);
  }
  return std::format("ctx.r{}", index);
}

std::string BuilderContext::F(size_t index) {
  const auto& cfg = GetConfig();
  if (!fn.GetMillicode() && ((cfg.non_argument_registers_as_local_variables && index == 0) ||
                             (CanLocalizeNonVolatiles() && index >= 14))) {
    locals.f[index] = true;
    return std::format("f{}", index);
  }
  return std::format("ctx.f{}", index);
}

std::string BuilderContext::V(size_t index) {
  const auto& cfg = GetConfig();
  if (!fn.GetMillicode() &&
      ((cfg.non_argument_registers_as_local_variables && (index >= 32 && index <= 63)) ||
       (CanLocalizeNonVolatiles() &&
        ((index >= 14 && index <= 31) || (index >= 64 && index <= 127))))) {
    locals.v[index] = true;
    return std::format("v{}", index);
  }
  return std::format("ctx.v{}", index);
}

std::string BuilderContext::CR(size_t index) {
  if (GetConfig().cr_registers_as_local_variables) {
    locals.cr[index] = true;
    return std::format("cr{}", index);
  }
  return std::format("ctx.cr{}", index);
}

const char* BuilderContext::CTR() {
  if (GetConfig().ctr_as_local_variable) {
    locals.ctr = true;
    return "ctr";
  }
  return "ctx.ctr";
}

const char* BuilderContext::XER() {
  if (GetConfig().xer_as_local_variable) {
    locals.xer = true;
    return "xer";
  }
  return "ctx.xer";
}

const char* BuilderContext::Reserved() {
  if (GetConfig().reserved_register_as_local_variable) {
    locals.reserved = true;
    return "reserved";
  }
  return "ctx.reserved";
}

const char* BuilderContext::Temp() {
  locals.temp = true;
  return "temp";
}

const char* BuilderContext::VTemp() {
  locals.v_temp = true;
  return "vTemp";
}

const char* BuilderContext::Env() {
  locals.env = true;
  return "env";
}

const char* BuilderContext::EA() {
  locals.ea = true;
  return "ea";
}

const CallTarget* BuilderContext::FindCallTarget(u32 site) const {
  for (const auto& edge : fn.GetCalls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  for (const auto& edge : fn.GetTailCalls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  return nullptr;
}

void BuilderContext::EmitCallToFunction(const FunctionNode& target_fn) {
  const auto& name = target_fn.GetName();

  if (target_fn.GetMillicode()) {
    if (CanLocalizeNonVolatiles())
      return;
    for (size_t i : {11, 12}) {
      if (locals.r[i])
        Println("\tctx.r{} = r{};", i, i);
    }
  }

  if (target_fn.IsRegisterSharing()) {
    Println("\t{}", kSyncToCtx);
    emit_ctx.Reference(name);
    Println("\t{}(ctx, base);", name);
    Println("\t{}", kSyncFromCtx);
    return;
  }

  emit_ctx.Reference(name);
  Println("\t{}(ctx, base);", name);
}

bool BuilderContext::CanTailCall() const {
  if (seh_scoped) {
    return false;
  }
  const auto hook = emit_ctx.config.mid_asm_hooks.find(base);
  return hook == emit_ctx.config.mid_asm_hooks.end() || !hook->second.after_instruction;
}

bool BuilderContext::EmitTailCall(u32 address) {
  const auto& cfg = GetConfig();
  if (!CanTailCall() || address == cfg.longjmp_address || address == cfg.setjmp_address) {
    return false;
  }
  const auto* target = FindCallTarget(base);
  if (!target || !target->IsFunction()) {
    return false;
  }
  const FunctionNode& target_fn = *target->AsFunction();
  if (target_fn.GetMillicode() || target_fn.IsRegisterSharing()) {
    return false;
  }
  emit_ctx.Reference(target_fn.GetName());
  Println("\tREX_MUSTTAIL return {}(ctx, base);", target_fn.GetName());
  return true;
}

void BuilderContext::EmitFunctionCall(u32 address) {
  const auto& cfg = GetConfig();

  if (address == cfg.longjmp_address) {
    Println("\tppc_longjmp({}.u32, {}.s32);", R(3), R(4));
    return;
  }

  if (address == cfg.setjmp_address) {
    Println("\t{}", kSyncToCtx);
    Println("\t{} = ctx;", Env());
    Println("\t{}.s64 = ppc_setjmp({}.u32);", Temp(), R(3));
    Println("\tif ({}.s64 != 0) ctx = {};", Temp(), Env());
    Println("\t{}", kSyncFromCtx);
    Println("\t{} = {};", R(3), Temp());
    return;
  }

  if (const auto* target = FindCallTarget(base)) {
    if (target->IsFunction()) {
      EmitCallToFunction(*target->AsFunction());
      return;
    }

    if (target->IsImport()) {
      const auto& import_target = std::get<CallTarget::ToImport>(target->value);
      std::string func_name;

      auto at_pos = import_target.name.find('@');
      if (at_pos != std::string::npos) {
        auto lib_name = import_target.name.substr(0, at_pos);
        auto ordinal_str = import_target.name.substr(at_pos + 1);
        u16 ordinal = static_cast<u16>(std::stoul(ordinal_str));

        const auto export_module = kernel::ModuleFromName(lib_name);
        const auto* exp = export_module ? kernel::FindByOrdinal(*export_module, ordinal) : nullptr;
        if (exp) {
          func_name = "__imp__" + std::string(exp->name);
        }
      }

      if (func_name.empty()) {
        func_name = "__imp__" + import_target.name;
        std::replace(func_name.begin(), func_name.end(), '@', '_');
        std::replace(func_name.begin(), func_name.end(), '.', '_');
      }

      emit_ctx.Reference(func_name);
      Println("\t{}(ctx, base);", func_name);
      return;
    }

    REXCG_ERROR("Unresolved function 0x{:08X} from 0x{:08X}", address, base);
    Println("\t// FATAL: unresolved function 0x{:08X}", address);
    Println("\tREXCPU_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
    return;
  }

  REXCG_ERROR("Unresolved function 0x{:08X} from 0x{:08X} (no CallTarget in FunctionNode)", address,
              base);
  Println("\t// FATAL: unresolved function 0x{:08X} (no CallTarget in FunctionNode)", address);
  Println("\tREXCPU_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
}

void BuilderContext::EmitConditionalBranch(bool negate, std::string_view cond) {
  u32 target = insn.operands[1];

  auto type = GetGraph().ClassifyTarget(target, base, false);

  switch (type) {
    case TargetType::kInternalLabel:
      Println("\tif ({}{}.{}) goto loc_{:08X};", negate ? "!" : "", CR(insn.operands[0]), cond,
              target);
      break;

    case TargetType::kFunction:
    case TargetType::kImport:
      if (const auto* call_target = FindCallTarget(base)) {
        if (call_target->IsFunction()) {
          auto* target_fn = call_target->AsFunction();
          emit_ctx.Reference(target_fn->GetName());
          Println("\tif ({}{}.{}) {{", negate ? "!" : "", CR(insn.operands[0]), cond);
          Println("\t\t{}(ctx, base);", target_fn->GetName());
          Println("\t\treturn;");
          Println("\t}}");
        } else if (call_target->IsImport()) {
          const auto& import_target = std::get<CallTarget::ToImport>(call_target->value);
          std::string func_name = "__imp__" + import_target.name;
          std::replace(func_name.begin(), func_name.end(), '@', '_');
          std::replace(func_name.begin(), func_name.end(), '.', '_');
          emit_ctx.Reference(func_name);
          Println("\tif ({}{}.{}) {{", negate ? "!" : "", CR(insn.operands[0]), cond);
          Println("\t\t{}(ctx, base);", func_name);
          Println("\t\treturn;");
          Println("\t}}");
        }
      } else {
        REXCG_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X} (no CallTarget)",
                    target, base);
        Println("\tif ({}{}.{}) REXCPU_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
                negate ? "!" : "", CR(insn.operands[0]), cond, base, target);
      }
      break;

    case TargetType::kUnknown:
      REXCG_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X}", target, base);
      Println("\t// ERROR: conditional branch to unknown address 0x{:08X}", target);
      Println("\tif ({}{}.{}) REXCPU_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
              negate ? "!" : "", CR(insn.operands[0]), cond, base, target);
      break;
  }
}

void BuilderContext::EmitSetFlushMode(bool enable) {
  auto new_state = enable ? CSRState::kVMX : CSRState::kFPU;
  if (csr_state != new_state) {
    if (enable) {
      Println("\tctx.fpscr.SetFlushMode(ctx.vscr.nj);");
    } else {
      auto suffix = csr_state != CSRState::kUnknown ? "Unconditional" : "";
      Println("\tctx.fpscr.DisableFlushMode{}();", suffix);
    }

    csr_state = new_state;
  }
}

void BuilderContext::ConsumeNext() {
  Println("\t// {} {}", next->opcode->name, std::string_view{next->op_str});
  consumed = 2;
}

bool BuilderContext::HasMidAsmHook() const {
  return GetConfig().mid_asm_hooks.find(base) != GetConfig().mid_asm_hooks.end();
}

void BuilderContext::EmitMidAsmHook() {
  auto mid_asm_hook = GetConfig().mid_asm_hooks.find(base);
  if (mid_asm_hook == GetConfig().mid_asm_hooks.end()) {
    return;
  }

  bool returns_bool = mid_asm_hook->second.return_on_false || mid_asm_hook->second.return_on_true ||
                      mid_asm_hook->second.jump_address_on_false != 0 ||
                      mid_asm_hook->second.jump_address_on_true != 0;

  const std::string r1 = R(1);
  Println("\t{}.u64 -= rex::ppc::kRedZone;", r1);
  Print("\t");
  if (returns_bool)
    Print("if (");

  Print("{}(", mid_asm_hook->second.name);
  size_t parameter_index = 0;
  for (auto& reg : mid_asm_hook->second.registers) {
    if (out.back() != '(')
      out += ", ";

    std::string_view value_suffix;
    if (mid_asm_hook->second.from_source) {
      const std::string& parameter = mid_asm_hook->second.parameters[parameter_index++];
      if (parameter == "void*" || parameter == "rex::memory::MappedPtr<u8>") {
        const bool mapped = parameter != "void*";
        out +=
            std::format("rex::ppc::ArgTranslator::Get{}Value<{}>({}.u64, base)",
                        mapped ? "MappedPointer" : "Pointer", parameter, R(RegisterIndex(reg, 1)));
        continue;
      }
      if (parameter == "u32")
        value_suffix = ".u32";
      else if (parameter == "u64")
        value_suffix = ".u64";
      else if (parameter == "f64")
        value_suffix = ".f64";
    }
    if (reg == "ctx") {
      out += "ctx";
      continue;
    }

    switch (reg[0]) {
      case 'c':
        if (reg == "ctr")
          out += CTR();
        else
          out += CR(RegisterIndex(reg, 2));
        break;
      case 'x':
        out += XER();
        break;
      case 'r':
        if (reg == "reserved")
          out += Reserved();
        else
          out += R(RegisterIndex(reg, 1));
        break;
      case 'f':
        if (reg == "fpscr")
          out += "ctx.fpscr";
        else
          out += F(RegisterIndex(reg, 1));
        break;
      case 'v':
        out += V(RegisterIndex(reg, 1));
        break;
      default:
        break;
    }
    out += value_suffix;
  }

  if (returns_bool) {
    Println(")) {{");
    Println("\t\t{}.u64 += rex::ppc::kRedZone;", r1);

    if (mid_asm_hook->second.return_on_true)
      Println("\t\treturn;");
    else if (mid_asm_hook->second.jump_address_on_true != 0)
      Println("\t\tgoto loc_{:X};", mid_asm_hook->second.jump_address_on_true);

    Println("\t}}");

    Println("\telse {{");
    Println("\t\t{}.u64 += rex::ppc::kRedZone;", r1);

    if (mid_asm_hook->second.return_on_false)
      Println("\t\treturn;");
    else if (mid_asm_hook->second.jump_address_on_false != 0)
      Println("\t\tgoto loc_{:X};", mid_asm_hook->second.jump_address_on_false);

    Println("\t}}");
  } else {
    Println(");");
    Println("\t{}.u64 += rex::ppc::kRedZone;", r1);

    if (mid_asm_hook->second.ret)
      Println("\treturn;");
    else if (mid_asm_hook->second.jump_address != 0)
      Println("\tgoto loc_{:X};", mid_asm_hook->second.jump_address);
  }
}

void BuilderContext::EmitVecFPBinary(const char* simd_op) {
  Println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_{}_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)));",
      V(insn.operands[0]), simd_op, V(insn.operands[1]), V(insn.operands[2]));
}

void BuilderContext::EmitVecFPUnaryExpr(std::string_view simd_expr) {
  auto v_d = V(insn.operands[0]);
  auto v_a = V(insn.operands[1]);

  std::string expr(simd_expr);
  size_t pos;
  while ((pos = expr.find("{vA}")) != std::string::npos) {
    expr.replace(pos, 4, v_a);
  }

  Println("\tsimde_mm_store_ps({}.f32, {});", v_d, expr);
}

void BuilderContext::EmitVecIntBinary(const char* simd_op, const char* element_type) {
  Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.{}, "
      "simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), "
      "simde_mm_load_si128((simde__m128i*){}.{})));",
      V(insn.operands[0]), element_type, simd_op, V(insn.operands[1]), element_type,
      V(insn.operands[2]), element_type);
}

void BuilderContext::EmitVecIntBinarySwapped(const char* simd_op, const char* element_type) {
  Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.{}, "
      "simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), "
      "simde_mm_load_si128((simde__m128i*){}.{})));",
      V(insn.operands[0]), element_type, simd_op, V(insn.operands[2]), element_type,
      V(insn.operands[1]), element_type);
}

void BuilderContext::EmitVecVarShift(const char* function, const char* element_type,
                                     u32 mask_value) {
  auto v_d = V(insn.operands[0]);
  auto v_a = V(insn.operands[1]);
  auto v_b = V(insn.operands[2]);
  Println("\t{{");
  Println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", v_a);
  Println("\t\tsimde__m128i b = simde_mm_load_si128((simde__m128i*){}.u8);", v_b);
  Println("\t\tsimde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_{}(0x{:X}));", element_type,
          mask_value);
  Println(
      "\t\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simd::{}"
      "(a, shift));",
      v_d, function);
  Println("\t}}");
}

void BuilderContext::EmitLoadDForm(const char* load_macro, const char* dest_type) {
  if (const auto held = FindStackWrite(AccessWidth(load_macro))) {
    Println("\t{}.{} = {}.u{};", R(insn.operands[0]), dest_type, *held,
            AccessWidth(load_macro) * 8);
    return;
  }
  Print("\t{}.{} = {}(", R(insn.operands[0]), dest_type, MMIOMacro(load_macro));
  if (insn.operands[2] != 0)
    Print("{}.u32 + ", R(insn.operands[2]));
  Println("{});", static_cast<i32>(insn.operands[1]));
}

void BuilderContext::EmitLoadXForm(const char* load_macro, const char* dest_type) {
  Print("\t{}.{} = {}(", R(insn.operands[0]), dest_type, MMIOMacro(load_macro));
  if (insn.operands[1] != 0)
    Print("{}.u32 + ", R(insn.operands[1]));
  Println("{}.u32);", R(insn.operands[2]));
}

void BuilderContext::EmitStoreDForm(const char* store_macro, const char* src_type) {
  if (const auto held = HoldStackWrite(AccessWidth(store_macro))) {
    Println("\t{}.{} = {}.{};", *held, src_type, R(insn.operands[0]), src_type);
    Println("\t{}({}.u32 + {}, {}.{});", MMIOMacro(store_macro), R(1),
            static_cast<i32>(insn.operands[1]), *held, src_type);
    return;
  }
  Print("\t{}(", MMIOMacro(store_macro));
  if (insn.operands[2] != 0)
    Print("{}.u32 + ", R(insn.operands[2]));
  Println("{}, {}.{});", static_cast<i32>(insn.operands[1]), R(insn.operands[0]), src_type);
}

void BuilderContext::EmitStoreXForm(const char* store_macro, const char* src_type) {
  Print("\t{}(", MMIOMacro(store_macro));
  if (insn.operands[1] != 0)
    Print("{}.u32 + ", R(insn.operands[1]));
  Println("{}.u32, {}.{});", R(insn.operands[2]), R(insn.operands[0]), src_type);
}

const char* GetCRBitName(u32 bi) {
  static constexpr const char* kNames[] = {"lt", "gt", "eq", "so"};
  return kNames[bi & 3];
}

bool IsRecordForm(const ppc_insn& insn) {
  return std::strchr(insn.opcode->name, '.') != nullptr;
}

void EmitRecordFormCompare(BuilderContext& ctx) {
  if (IsRecordForm(ctx.insn)) {
    ctx.Println("\t{}.Compare<i32>({}.s32, 0, {});", ctx.CR(0), ctx.R(ctx.insn.operands[0]),
                ctx.XER());
  }
}

}  // namespace rex::codegen
