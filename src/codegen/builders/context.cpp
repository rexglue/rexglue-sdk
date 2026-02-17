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

#include "../builder_context.h"
#include <rex/codegen/recompiler.h>
#include <rex/runtime/export_resolver.h>
#include <rex/runtime.h>
#include <rex/logging.h>
#include "helpers.h"
#include <algorithm>

namespace rex::codegen {

//=============================================================================
// Convenience Accessors
//=============================================================================

const RecompilerConfig& BuilderContext::config() const
{
    return recompiler.ctx_->Config();
}

const FunctionGraph& BuilderContext::graph() const
{
    return recompiler.ctx_->graph;
}

std::string& BuilderContext::out()
{
    return recompiler.out;
}

//=============================================================================
// Register Accessors
//=============================================================================

std::string BuilderContext::r(size_t index)
{
    const auto& cfg = config();
    if ((cfg.nonArgumentRegistersAsLocalVariables && (index == 0 || index == 2 || index == 11 || index == 12)) ||
        (cfg.nonVolatileRegistersAsLocalVariables && index >= 14))
    {
        locals.r[index] = true;
        return fmt::format("r{}", index);
    }
    return fmt::format("ctx.r{}", index);
}

std::string BuilderContext::f(size_t index)
{
    const auto& cfg = config();
    if ((cfg.nonArgumentRegistersAsLocalVariables && index == 0) ||
        (cfg.nonVolatileRegistersAsLocalVariables && index >= 14))
    {
        locals.f[index] = true;
        return fmt::format("f{}", index);
    }
    return fmt::format("ctx.f{}", index);
}

std::string BuilderContext::v(size_t index)
{
    const auto& cfg = config();
    if ((cfg.nonArgumentRegistersAsLocalVariables && (index >= 32 && index <= 63)) ||
        (cfg.nonVolatileRegistersAsLocalVariables && ((index >= 14 && index <= 31) || (index >= 64 && index <= 127))))
    {
        locals.v[index] = true;
        return fmt::format("v{}", index);
    }
    return fmt::format("ctx.v{}", index);
}

std::string BuilderContext::cr(size_t index)
{
    if (config().crRegistersAsLocalVariables)
    {
        locals.cr[index] = true;
        return fmt::format("cr{}", index);
    }
    return fmt::format("ctx.cr{}", index);
}

const char* BuilderContext::ctr()
{
    if (config().ctrAsLocalVariable)
    {
        locals.ctr = true;
        return "ctr";
    }
    return "ctx.ctr";
}

const char* BuilderContext::xer()
{
    if (config().xerAsLocalVariable)
    {
        locals.xer = true;
        return "xer";
    }
    return "ctx.xer";
}

const char* BuilderContext::reserved()
{
    if (config().reservedRegisterAsLocalVariable)
    {
        locals.reserved = true;
        return "reserved";
    }
    return "ctx.reserved";
}

const char* BuilderContext::temp()
{
    locals.temp = true;
    return "temp";
}

const char* BuilderContext::v_temp()
{
    locals.v_temp = true;
    return "vTemp";
}

const char* BuilderContext::env()
{
    locals.env = true;
    return "env";
}

const char* BuilderContext::ea()
{
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

bool BuilderContext::mmio_check_d_form()
{
    if (base + 4 < fn.end() && *(data + 1) == Recompiler::c_eieio)
        return true;
    return locals.is_mmio_base(insn.operands[2]);
}

bool BuilderContext::mmio_check_x_form()
{
    if (base + 4 < fn.end() && *(data + 1) == Recompiler::c_eieio)
        return true;
    return locals.is_mmio_base(insn.operands[1]) || locals.is_mmio_base(insn.operands[2]);
}

const CallTarget* BuilderContext::findCallTarget(uint32_t site) const
{
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

void BuilderContext::emit_function_call(uint32_t address)
{
    const auto& cfg = config();

    if (address == cfg.longJmpAddress)
    {
        // Use custom ppc_longjmp that uses guest address as key (not for storage)
        println("\t::rex::runtime::guest::ppc_longjmp({}.u32, {}.s32);", r(3), r(4));
        return;
    }

    if (address == cfg.setJmpAddress)
    {
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

            // Handle save/restore helpers
            if (cfg.nonVolatileRegistersAsLocalVariables &&
                (name.find("__rest") == 0 || name.find("__save") == 0))
            {
                // print nothing - these are handled by local variable tracking
                return;
            }

            println("\t{}(ctx, base);", name);
            return;
        }

        if (target->isImport()) {
            const auto& importTarget = std::get<CallTarget::ToImport>(target->value);
            std::string func_name;

            // Try to resolve ordinal to actual function name
            auto at_pos = importTarget.name.find('@');
            if (at_pos != std::string::npos && recompiler.runtime)
            {
                auto lib_name = importTarget.name.substr(0, at_pos);
                auto ordinal_str = importTarget.name.substr(at_pos + 1);
                uint16_t ordinal = static_cast<uint16_t>(std::stoul(ordinal_str));

                if (auto* resolver = recompiler.runtime->export_resolver())
                {
                    auto* exp = resolver->GetExportByOrdinal(lib_name + ".xex", ordinal);
                    if (!exp) exp = resolver->GetExportByOrdinal(lib_name, ordinal);

                    if (exp)
                    {
                        func_name = "__imp__" + std::string(exp->name);
                    }
                }
            }

            if (func_name.empty())
            {
                func_name = "__imp__" + importTarget.name;
                std::replace(func_name.begin(), func_name.end(), '@', '_');
                std::replace(func_name.begin(), func_name.end(), '.', '_');
            }

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
    REXCODEGEN_ERROR("Unresolved function 0x{:08X} from 0x{:08X} (no CallTarget in FunctionNode)", address, base);
    println("\t// FATAL: unresolved function 0x{:08X} (no CallTarget in FunctionNode)", address);
    println("\tREX_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
}

void BuilderContext::emit_conditional_branch(bool not_, std::string_view cond)
{
    uint32_t target = insn.operands[1];

    // Use classifyTarget for consistent branch classification
    // false = branch instruction (not a call), so own-base means loop back
    auto kind = graph().classifyTarget(target, base, false);

    switch (kind) {
    case TargetKind::InternalLabel:
        // Target is within this function - local goto
        println("\tif ({}{}.{}) goto loc_{:08X};",
                not_ ? "!" : "", cr(insn.operands[0]), cond, target);
        break;

    case TargetKind::Function:
    case TargetKind::Import:
        // Conditional tail call to another function - check pre-resolved call target
        if (const auto* callTarget = findCallTarget(base)) {
            if (callTarget->isFunction()) {
                auto* targetFn = callTarget->asFunction();
                println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
                println("\t\t{}(ctx, base);", targetFn->name());
                println("\t\treturn;");
                println("\t}}");
            } else if (callTarget->isImport()) {
                const auto& importTarget = std::get<CallTarget::ToImport>(callTarget->value);
                std::string func_name = "__imp__" + importTarget.name;
                std::replace(func_name.begin(), func_name.end(), '@', '_');
                std::replace(func_name.begin(), func_name.end(), '.', '_');
                println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
                println("\t\t{}(ctx, base);", func_name);
                println("\t\treturn;");
                println("\t}}");
            }
        } else {
            REXCODEGEN_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X} (no CallTarget)", target, base);
            println("\tif ({}{}.{}) REX_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
                    not_ ? "!" : "", cr(insn.operands[0]), cond, base, target);
        }
        break;

    case TargetKind::Unknown:
        REXCODEGEN_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X}", target, base);
        println("\t// ERROR: conditional branch to unknown address 0x{:08X}",
                target);
        println("\tif ({}{}.{}) REX_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X}\");",
                not_ ? "!" : "", cr(insn.operands[0]), cond, base, target);
        break;
    }
}

void BuilderContext::emit_set_flush_mode(bool enable)
{
    auto newState = enable ? CSRState::VMX : CSRState::FPU;
    if (csrState != newState)
    {
        auto prefix = enable ? "enable" : "disable";
        auto suffix = csrState != CSRState::Unknown ? "Unconditional" : "";
        println("\tctx.fpscr.{}FlushMode{}();", prefix, suffix);

        csrState = newState;
    }
}

bool BuilderContext::has_mid_asm_hook() const
{
    return config().midAsmHooks.find(base) != config().midAsmHooks.end();
}

void BuilderContext::emit_mid_asm_hook()
{
    auto midAsmHook = config().midAsmHooks.find(base);
    if (midAsmHook == config().midAsmHooks.end())
    {
        return;
    }

    bool returnsBool = midAsmHook->second.returnOnFalse || midAsmHook->second.returnOnTrue ||
        midAsmHook->second.jumpAddressOnFalse != 0 || midAsmHook->second.jumpAddressOnTrue != 0;

    print("\t");
    if (returnsBool)
        print("if (");

    print("{}(ctx, base", midAsmHook->second.name);
    for (const auto& reg : midAsmHook->second.registers)
    {
        print(", {}", reg);
    }
    print(")");

    if (returnsBool)
        print(") ");

    if (midAsmHook->second.returnOnTrue || midAsmHook->second.returnOnFalse)
    {
        if (midAsmHook->second.returnOnFalse)
            print("{{ ");
        println("return;");
        if (midAsmHook->second.returnOnFalse)
            println("\t}}");
    }
    else if (midAsmHook->second.jumpAddress != 0)
    {
        println("goto loc_{:X};", midAsmHook->second.jumpAddress);
    }
    else if (midAsmHook->second.jumpAddressOnTrue != 0)
    {
        println("goto loc_{:X};", midAsmHook->second.jumpAddressOnTrue);
    }
    else if (midAsmHook->second.jumpAddressOnFalse != 0)
    {
        println("{{ goto loc_{:X}; }}", midAsmHook->second.jumpAddressOnFalse);
    }
    else if (midAsmHook->second.ret)
    {
        println("return;");
    }
    else
    {
        println(";");
    }
}

void BuilderContext::reset_switch_table()
{
    switchTable = recompiler.ctx_->Config().switchTables.end();
}

//=============================================================================
// Vector (SIMD) Code Generation Helpers
//=============================================================================

void BuilderContext::emit_vec_fp_binary(const char* simd_op)
{
    println("\tsimde_mm_store_ps({}.f32, simde_mm_{}_ps(simde_mm_load_ps({}.f32), simde_mm_load_ps({}.f32)));",
        v(insn.operands[0]), simd_op, v(insn.operands[1]), v(insn.operands[2]));
}

void BuilderContext::emit_vec_fp_unary_expr(std::string_view simd_expr)
{
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

void BuilderContext::emit_vec_int_binary(const char* simd_op, const char* element_type)
{
    println("\tsimde_mm_store_si128((simde__m128i*){}.{}, simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), simde_mm_load_si128((simde__m128i*){}.{})));",
        v(insn.operands[0]), element_type, simd_op,
        v(insn.operands[1]), element_type,
        v(insn.operands[2]), element_type);
}

void BuilderContext::emit_vec_int_binary_swapped(const char* simd_op, const char* element_type)
{
    // Swapped: op(vB, vA) instead of op(vA, vB) - useful for andnot which has reversed operand semantics
    println("\tsimde_mm_store_si128((simde__m128i*){}.{}, simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), simde_mm_load_si128((simde__m128i*){}.{})));",
        v(insn.operands[0]), element_type, simd_op,
        v(insn.operands[2]), element_type,
        v(insn.operands[1]), element_type);
}

//=============================================================================
// Memory (Load/Store) Code Generation Helpers
//=============================================================================

void BuilderContext::emit_load_d_form(const char* load_macro, const char* dest_type, bool check_mmio)
{
    // D-form: rD = LOAD(rA + D) where operands[0]=rD, operands[1]=D, operands[2]=rA
    // load_macro should be like "PPC_LOAD_U8" - we replace PPC_LOAD with PPC_MM_LOAD for MMIO
    const char* macro = load_macro;
    static char mm_macro[64];
    if (check_mmio && mmio_check_d_form()) {
        // Replace "PPC_LOAD_" with "PPC_MM_LOAD_"
        if (strncmp(load_macro, "PPC_LOAD_", 9) == 0) {
            snprintf(mm_macro, sizeof(mm_macro), "PPC_MM_LOAD_%s", load_macro + 9);
            macro = mm_macro;
        }
    }

    print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, macro);
    if (insn.operands[2] != 0)
        print("{}.u32 + ", r(insn.operands[2]));
    println("{});", static_cast<int32_t>(insn.operands[1]));
}

void BuilderContext::emit_load_x_form(const char* load_macro, const char* dest_type, bool check_mmio)
{
    // X-form: rD = LOAD(rA + rB) where operands[0]=rD, operands[1]=rA, operands[2]=rB
    // load_macro should be like "PPC_LOAD_U8" - we replace PPC_LOAD with PPC_MM_LOAD for MMIO
    const char* macro = load_macro;
    static char mm_macro[64];
    if (check_mmio && mmio_check_x_form()) {
        // Replace "PPC_LOAD_" with "PPC_MM_LOAD_"
        if (strncmp(load_macro, "PPC_LOAD_", 9) == 0) {
            snprintf(mm_macro, sizeof(mm_macro), "PPC_MM_LOAD_%s", load_macro + 9);
            macro = mm_macro;
        }
    }

    print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, macro);
    if (insn.operands[1] != 0)
        print("{}.u32 + ", r(insn.operands[1]));
    println("{}.u32);", r(insn.operands[2]));
}

void BuilderContext::emit_store_d_form(const char* store_macro, const char* src_type, bool check_mmio)
{
    // D-form: STORE(rA + D, rS) where operands[0]=rS, operands[1]=D, operands[2]=rA
    // store_macro should be like "PPC_STORE_U8" - we replace PPC_STORE with PPC_MM_STORE for MMIO
    const char* macro = store_macro;
    static char mm_macro[64];
    if (check_mmio && mmio_check_d_form()) {
        // Replace "PPC_STORE_" with "PPC_MM_STORE_"
        if (strncmp(store_macro, "PPC_STORE_", 10) == 0) {
            snprintf(mm_macro, sizeof(mm_macro), "PPC_MM_STORE_%s", store_macro + 10);
            macro = mm_macro;
        }
    }

    print("\t{}(", macro);
    if (insn.operands[2] != 0)
        print("{}.u32 + ", r(insn.operands[2]));
    println("{}, {}.{});", static_cast<int32_t>(insn.operands[1]), r(insn.operands[0]), src_type);
}

void BuilderContext::emit_store_x_form(const char* store_macro, const char* src_type, bool check_mmio)
{
    // X-form: STORE(rA + rB, rS) where operands[0]=rS, operands[1]=rA, operands[2]=rB
    // store_macro should be like "PPC_STORE_U8" - we replace PPC_STORE with PPC_MM_STORE for MMIO
    const char* macro = store_macro;
    static char mm_macro[64];
    if (check_mmio && mmio_check_x_form()) {  // Use X-form specific check (operands[1] is base)
        // Replace "PPC_STORE_" with "PPC_MM_STORE_"
        if (strncmp(store_macro, "PPC_STORE_", 10) == 0) {
            snprintf(mm_macro, sizeof(mm_macro), "PPC_MM_STORE_%s", store_macro + 10);
            macro = mm_macro;
        }
    }

    print("\t{}(", macro);
    if (insn.operands[1] != 0)
        print("{}.u32 + ", r(insn.operands[1]));
    println("{}.u32, {}.{});", r(insn.operands[2]), r(insn.operands[0]), src_type);
}

} // namespace rex::codegen
