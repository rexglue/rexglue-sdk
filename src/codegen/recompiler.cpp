/**
 * @file        rexcodegen/recompiler.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// TODO(tomc): This file should probably be refactored away. Its quite old and very fragmented from
// newer components.

#include "builder_context.h"
#include "builders.h"
#include "codegen_flags.h"
#include "ppc/disasm.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <rex/codegen/recompiled_function.h>
#include <rex/codegen/recompiler.h>
#include <rex/logging.h>

#include "codegen_logging.h"

#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/types.h>

// Import types
using rex::codegen::ppc::Disassemble;
using rex::memory::load_and_swap;

// Xbox 360/Windows CE Runtime Function (.pdata section)
// Used for exception handling and function discovery
namespace {
#pragma pack(push, 1)
struct IMAGE_CE_RUNTIME_FUNCTION {
  uint32_t BeginAddress;
  union {
    uint32_t Data;
    struct {
      uint32_t PrologLength : 8;
      uint32_t FunctionLength : 22;
      uint32_t ThirtyTwoBit : 1;
      uint32_t ExceptionFlag : 1;
    };
  };
};
#pragma pack(pop)
static_assert(sizeof(IMAGE_CE_RUNTIME_FUNCTION) == 8);
}  // namespace

#include <algorithm>
#include <set>
#include <sstream>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/math.h>

#include <dis-asm.h>
#include <ppc.h>
#include <xxhash.h>

using rex::X_STATUS;

namespace rex::codegen {

// Output configuration constants
constexpr size_t kOutputBufferReserveSize = 32 * 1024 * 1024;  // 32 MB

Recompiler::Recompiler() = default;
Recompiler::~Recompiler() = default;

bool Recompiler::recompile(const FunctionNode& fn, uint32_t base, const ppc_insn& insn,
                           const uint32_t* data,
                           std::unordered_map<uint32_t, JumpTable>::iterator& switchTable,
                           RecompilerLocalVariables& localVariables, CSRState& csrState) {
  println("\t// {} {}", insn.opcode->name, insn.op_str);

  // TODO: we could cache these formats in an array
  auto r = [&](size_t index) {
    if ((config().nonArgumentRegistersAsLocalVariables &&
         (index == 0 || index == 2 || index == 11 || index == 12)) ||
        (config().nonVolatileRegistersAsLocalVariables && index >= 14)) {
      localVariables.r[index] = true;
      return fmt::format("r{}", index);
    }
    return fmt::format("ctx.r{}", index);
  };

  auto f = [&](size_t index) {
    if ((config().nonArgumentRegistersAsLocalVariables && index == 0) ||
        (config().nonVolatileRegistersAsLocalVariables && index >= 14)) {
      localVariables.f[index] = true;
      return fmt::format("f{}", index);
    }
    return fmt::format("ctx.f{}", index);
  };

  auto v = [&](size_t index) {
    if ((config().nonArgumentRegistersAsLocalVariables && (index >= 32 && index <= 63)) ||
        (config().nonVolatileRegistersAsLocalVariables &&
         ((index >= 14 && index <= 31) || (index >= 64 && index <= 127)))) {
      localVariables.v[index] = true;
      return fmt::format("v{}", index);
    }
    return fmt::format("ctx.v{}", index);
  };

  auto cr = [&](size_t index) {
    if (config().crRegistersAsLocalVariables) {
      localVariables.cr[index] = true;
      return fmt::format("cr{}", index);
    }
    return fmt::format("ctx.cr{}", index);
  };

  auto ctr = [&]() {
    if (config().ctrAsLocalVariable) {
      localVariables.ctr = true;
      return "ctr";
    }
    return "ctx.ctr";
  };

  auto xer = [&]() {
    if (config().xerAsLocalVariable) {
      localVariables.xer = true;
      return "xer";
    }
    return "ctx.xer";
  };

  auto reserved = [&]() {
    if (config().reservedRegisterAsLocalVariable) {
      localVariables.reserved = true;
      return "reserved";
    }
    return "ctx.reserved";
  };

  auto temp = [&]() {
    localVariables.temp = true;
    return "temp";
  };

  [[maybe_unused]] auto vTemp = [&]() {
    localVariables.v_temp = true;
    return "vTemp";
  };

  auto env = [&]() {
    localVariables.env = true;
    return "env";
  };

  [[maybe_unused]] auto ea = [&]() {
    localVariables.ea = true;
    return "ea";
  };

  [[maybe_unused]] auto mmioStore = [&]() -> bool {
    // Check that data+1 is within the function's instruction range
    // base is current instruction address, base+4 is next instruction
    if (base + 4 >= fn.end()) {
      return false;  // Out of bounds, cannot check for eieio
    }
    return *(data + 1) == c_eieio;
  };

  auto printFunctionCall = [&](uint32_t address) {
    if (address == config().longJmpAddress) {
      // Native longjmp: use guest buffer directly
      println("\tlongjmp(*reinterpret_cast<jmp_buf*>(base + {}.u32), {}.s32);", r(3), r(4));
    } else if (address == config().setJmpAddress) {
      // Native setjmp: save ctx and use guest buffer directly
      // Returns 0 on first call, non-zero when longjmp returns here
      println("\t{} = ctx;", env());
      println("\t{}.s64 = setjmp(*reinterpret_cast<jmp_buf*>(base + {}.u32));", temp(), r(3));
      println("\tif ({}.s64 != 0) ctx = {};", temp(), env());
      println("\t{} = {};", r(3), temp());
    } else {
      // Look up target in FunctionGraph
      if (auto* targetNode = graph().getFunction(address)) {
        const auto& name = targetNode->name();
        if (config().nonVolatileRegistersAsLocalVariables &&
            (name.find("__rest") == 0 || name.find("__save") == 0)) {
          // print nothing - handled by local variable tracking
        } else {
          println("\t{}(ctx, base);", name);
        }
      } else {
        REXCODEGEN_ERROR("Unresolved function 0x{:08X} from 0x{:08X}", address, base);
        println("\t// ERROR: unresolved function 0x{:08X}", address);
      }
    }
  };

  [[maybe_unused]] auto printConditionalBranch = [&](bool not_, const std::string_view& cond) {
    if (insn.operands[1] < fn.base() || insn.operands[1] >= fn.end()) {
      println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
      print("\t");
      printFunctionCall(insn.operands[1]);
      println("\t\treturn;");
      println("\t}}");
    } else {
      println("\tif ({}{}.{}) goto loc_{:X};", not_ ? "!" : "", cr(insn.operands[0]), cond,
              insn.operands[1]);
    }
  };

  [[maybe_unused]] auto printSetFlushMode = [&](bool enable) {
    auto newState = enable ? CSRState::VMX : CSRState::FPU;
    if (csrState != newState) {
      auto prefix = enable ? "enable" : "disable";
      auto suffix = csrState != CSRState::Unknown ? "Unconditional" : "";
      println("\tctx.fpscr.{}FlushMode{}();", prefix, suffix);

      csrState = newState;
    }
  };

  auto midAsmHook = config().midAsmHooks.find(base);

  auto printMidAsmHook = [&]() {
    bool returnsBool = midAsmHook->second.returnOnFalse || midAsmHook->second.returnOnTrue ||
                       midAsmHook->second.jumpAddressOnFalse != 0 ||
                       midAsmHook->second.jumpAddressOnTrue != 0;

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
  };

  if (midAsmHook != config().midAsmHooks.end() && !midAsmHook->second.afterInstruction)
    printMidAsmHook();

  int id = insn.opcode->id;

  // Handling instructions that don't disassemble correctly for some reason here
  if (id == PPC_INST_VUPKHSB128 && insn.operands[2] == 0x60)
    id = PPC_INST_VUPKHSH128;
  else if (id == PPC_INST_VUPKLSB128 && insn.operands[2] == 0x60)
    id = PPC_INST_VUPKLSH128;

  // Create builder context and dispatch to appropriate builder
  BuilderContext ctx{*this, fn, insn, base, data, localVariables, csrState, switchTable};

  if (!DispatchInstruction(id, ctx))
    return false;

  // Validate that RC bit instructions generate condition register updates
  if (strchr(insn.opcode->name, '.')) {
    int lastLine = out.find_last_of('\n', out.size() - 2);
    if (out.find("cr0", lastLine + 1) == std::string::npos &&
        out.find("cr6", lastLine + 1) == std::string::npos)
      REXCODEGEN_WARN("{} at {:X} has RC bit enabled but no comparison was generated",
                      insn.opcode->name, base);
  }

  if (midAsmHook != config().midAsmHooks.end() && midAsmHook->second.afterInstruction)
    printMidAsmHook();

  return true;
}

bool Recompiler::recompile(const FunctionNode& fn) {
  // Iterate over discovered blocks, not raw address range
  // This ensures we only process actual code, not PDATA/metadata
  if (fn.blocks().empty()) {
    // Generate empty stub for functions with no blocks (e.g., exception handler data)
    REXCODEGEN_WARN("Function 0x{:08X} has no blocks - generating stub", fn.base());

    std::string name;
    if (fn.base() == analysisState().entryPoint) {
      name = "xstart";
    } else if (!fn.name().empty()) {
      name = fn.name();
    } else {
      name = fmt::format("sub_{:08X}", fn.base());
    }

    // Generate stub with weak/alias pattern
    println("// STUB: Function at 0x{:08X} has no discovered code blocks", fn.base());
    println("__attribute__((alias(\"__imp__{}\"))) PPC_WEAK_FUNC({});", name, name);
    println("PPC_FUNC_IMPL(__imp__{}) {{", name);
    println("\tPPC_FUNC_PROLOGUE();");
    println("}}\n");
    return true;
  }

  // Check for SEH exception info
  const SehExceptionInfo* sehInfo = nullptr;
  if (fn.hasExceptionInfo()) {
    sehInfo = fn.exceptionInfo()->asSeh();
    if (sehInfo && !sehInfo->scopes.empty()) {
      REXCODEGEN_TRACE("Function 0x{:08X} has {} SEH scopes", fn.base(), sehInfo->scopes.size());
    }
  }

  std::unordered_set<size_t> labels;
  labels.reserve(64);  // Pre-allocate for typical function

  // First pass: collect labels from all blocks
  for (const auto& block : fn.blocks()) {
    auto* blockData = reinterpret_cast<const uint32_t*>(binary().translate(block.base));
    if (!blockData)
      continue;

    for (size_t addr = block.base; addr < block.end(); addr += 4) {
      const uint32_t instruction =
          load_and_swap<uint32_t>((const uint8_t*)blockData + addr - block.base);
      if (!PPC_BL(instruction)) {
        const size_t op = PPC_OP(instruction);
        if (op == PPC_OP_B)
          labels.emplace(addr + PPC_BI(instruction));
        else if (op == PPC_OP_BC)
          labels.emplace(addr + PPC_BD(instruction));
      }

      auto switchTable = config().switchTables.find(addr);
      if (switchTable != config().switchTables.end()) {
        for (auto label : switchTable->second.targets)
          labels.emplace(label);
      }

      auto midAsmHook = config().midAsmHooks.find(addr);
      if (midAsmHook != config().midAsmHooks.end()) {
        if (midAsmHook->second.returnOnFalse || midAsmHook->second.returnOnTrue ||
            midAsmHook->second.jumpAddressOnFalse != 0 ||
            midAsmHook->second.jumpAddressOnTrue != 0) {
          print("extern bool ");
        } else {
          print("extern void ");
        }

        print("{}(", midAsmHook->second.name);
        for (auto& reg : midAsmHook->second.registers) {
          if (out.back() != '(')
            out += ", ";

          switch (reg[0]) {
            case 'c':
              if (reg == "ctr")
                print("PPCRegister& ctr");
              else
                print("PPCCRRegister& {}", reg);
              break;

            case 'x':
              print("PPCXERRegister& xer");
              break;

            case 'r':
              print("PPCRegister& {}", reg);
              break;

            case 'f':
              if (reg == "fpscr")
                print("PPCFPSCRRegister& fpscr");
              else
                print("PPCRegister& {}", reg);
              break;

            case 'v':
              print("PPCVRegister& {}", reg);
              break;
          }
        }

        println(");\n");

        if (midAsmHook->second.jumpAddress != 0)
          labels.emplace(midAsmHook->second.jumpAddress);
        if (midAsmHook->second.jumpAddressOnTrue != 0)
          labels.emplace(midAsmHook->second.jumpAddressOnTrue);
        if (midAsmHook->second.jumpAddressOnFalse != 0)
          labels.emplace(midAsmHook->second.jumpAddressOnFalse);
      }
    }
  }

  // Collect labels from auto-detected jump tables
  for (const auto& jt : fn.jumpTables()) {
    for (auto label : jt.targets) {
      labels.emplace(label);
    }
  }

  // Determine function name from fn.name() (already populated in FunctionGraph)
  std::string name;
  bool is_entry_point = (fn.base() == analysisState().entryPoint);

  if (is_entry_point) {
    name = "xstart";  // Entry point is always named xstart
  } else if (!fn.name().empty()) {
    name = fn.name();  // Use name from FunctionGraph (includes helper names)
  } else {
    name = fmt::format("sub_{:08X}", fn.base());
  }

  // Use weak/alias pattern - allows functions to be overridden at link time
  // The weak symbol 'name' aliases to '__imp__name', so overriding 'name' takes precedence
  println("__attribute__((alias(\"__imp__{}\"))) PPC_WEAK_FUNC({});", name, name);
  println("PPC_FUNC_IMPL(__imp__{}) {{", name);
  println("\tPPC_FUNC_PROLOGUE();");

  auto switchTable = config().switchTables.end();
  bool allRecompiled = true;
  CSRState csrState = CSRState::Unknown;

  RecompilerLocalVariables localVariables;
  std::string tempString;
  tempString.reserve(4096);    // Pre-allocate for typical function body
  std::swap(out, tempString);  // Save current output, body will be written to out

  ppc_insn insn;
  std::unordered_set<size_t> emittedLabels;  // Track emitted labels to avoid duplicates

  // Second pass: recompile all blocks
  for (const auto& block : fn.blocks()) {
    auto base = block.base;
    auto end = block.end();
    auto* data = reinterpret_cast<const uint32_t*>(binary().translate(block.base));
    if (!data) {
      REXCODEGEN_WARN("Block 0x{:08X} in function 0x{:08X} has no mapped data - skipping",
                      block.base, fn.base());
      continue;
    }

    while (base < end) {
      // Only emit each label once
      if (labels.find(base) != labels.end() && emittedLabels.insert(base).second) {
        println("loc_{:X}:", base);

        // Anyone could jump to this label so we wouldn't know what the CSR state would be.
        csrState = CSRState::Unknown;
      }

      if (switchTable == config().switchTables.end())
        switchTable = config().switchTables.find(base);

      Disassemble(data, 4, base, insn);

      if (insn.opcode == nullptr) {
        println("\t// {}", insn.op_str);
        // Warn about undecoded non-zero instructions (likely unimplemented opcodes)
        if (*data != 0)
          REXCODEGEN_WARN("Unable to decode instruction {:X} at {:X}", *data, base);
      } else {
        // Check for potential jump table that wasn't detected during analysis
        if (insn.opcode->id == PPC_INST_BCTR && switchTable == config().switchTables.end() &&
            ctx_ != nullptr) {
          // Look for mtctr within 3 instructions before bctr
          // mtctr rX = 0x7CXX03A6 (where XX = RS << 5)
          // nop = 0x60000000
          // Pattern can have 0, 1, or 2 nops between mtctr and bctr
          bool is_switch_pattern = false;
          constexpr uint32_t MTCTR_MASK = 0xFC1FFFFF;
          constexpr uint32_t MTCTR_OPCODE = 0x7C0003A6;
          constexpr uint32_t NOP = 0x60000000;

          for (int i = 1; i <= 3 && !is_switch_pattern; i++) {
            uint32_t prev_insn = load_and_swap<uint32_t>(data - i);
            if ((prev_insn & MTCTR_MASK) == MTCTR_OPCODE) {
              // Found mtctr - verify all instructions between are nops
              is_switch_pattern = true;
              for (int j = 1; j < i; j++) {
                if (load_and_swap<uint32_t>(data - j) != NOP) {
                  is_switch_pattern = false;
                  break;
                }
              }
            } else if (prev_insn != NOP) {
              break;  // Non-nop, non-mtctr - stop searching
            }
          }

          if (is_switch_pattern) {
            // Try to detect jump table
            FunctionScanner scanner(binary());
            auto jt_opt = scanner.detect_jump_table(base);
            if (jt_opt.has_value()) {
              // Add to config and use it
              config().switchTables.emplace(base, std::move(*jt_opt));
              switchTable = config().switchTables.find(base);
              // Also add labels for code generation
              for (auto label : switchTable->second.targets) {
                labels.emplace(label);
              }
              REXCODEGEN_INFO("Late-detected jump table at 0x{:08X} with {} entries", base,
                              switchTable->second.targets.size());
            } else {
            }
          }
        }

        if (!recompile(fn, base, insn, data, switchTable, localVariables, csrState)) {
          REXCODEGEN_WARN("Unrecognized instruction at 0x{:X}: {}", base, insn.opcode->name);
          allRecompiled = false;
        }
      }

      base += 4;
      ++data;
    }
  }

  // Close the function body (or SEH try block)
  bool generateSeh = sehInfo && !sehInfo->scopes.empty() && config().generateExceptionHandlers;
  if (generateSeh) {
    // Close the SEH try block and emit catch + finally handlers
    // For each SEH scope with filter==0 (meaning __finally), call the handler
    println("\t\t}} SEH_CATCH_ALL {{");
    println("\t\t\tREXLOG_WARN(\"SEH exception caught in sub_{:08X}\");", fn.base());

    // Set up frame pointer for finally handler (r12 = establisher frame)
    // The finally handler computes r31 = r12 - frameSize, then accesses data via r31
    if (sehInfo->frameSize > 0) {
      println("\t\t\tctx.r12.s64 = ctx.r31.s64 + {};  // Establisher frame pointer",
              sehInfo->frameSize);
    }

    // Run finally handlers in reverse order
    for (auto it = sehInfo->scopes.rbegin(); it != sehInfo->scopes.rend(); ++it) {
      const auto& scope = *it;
      if (scope.filter == 0 && scope.handler != 0) {
        // This is a __finally handler - call it
        // Handler is a separate function discovered during analysis
        println("\t\t\tsub_{:08X}(ctx, base);  // __finally handler", scope.handler);
      }
    }

    // Call restore helper to restore caller's registers before returning
    if (sehInfo->restoreHelper != 0) {
      auto* restoreFn = graph().getFunction(sehInfo->restoreHelper);
      if (restoreFn && !restoreFn->name().empty()) {
        println("\t\t\t{}(ctx, base);  // Restore caller registers", restoreFn->name());
      }
    }

    // After running finally handlers, rethrow the exception to propagate it
    println("\t\t\tSEH_RETHROW;");
    println("\t\t}} SEH_END");
    // NO duplicate finally call after SEH_END - normal path already calls them inline at tryEnd
    println("\t}}\n");
  } else {
    println("}}\n");
  }

  // Swap back: tempString now has function body, out restored to pre-function state
  // Now write local variable declarations to out, then append the body
  std::swap(out, tempString);
  if (localVariables.ctr)
    println("\tPPCRegister ctr{{}};");
  if (localVariables.xer)
    println("\tPPCXERRegister xer{{}};");
  if (localVariables.reserved)
    println("\tPPCRegister reserved{{}};");

  for (size_t i = 0; i < 8; i++) {
    if (localVariables.cr[i])
      println("\tPPCCRRegister cr{}{{}};", i);
  }

  for (size_t i = 0; i < 32; i++) {
    if (localVariables.r[i])
      println("\tPPCRegister r{}{{}};", i);
  }

  for (size_t i = 0; i < 32; i++) {
    if (localVariables.f[i])
      println("\tPPCRegister f{}{{}};", i);
  }

  for (size_t i = 0; i < 128; i++) {
    if (localVariables.v[i])
      println("\tPPCVRegister v{}{{}};", i);
  }

  if (localVariables.env)
    println("\tPPCContext env{{}};");

  if (localVariables.temp)
    println("\tPPCRegister temp{{}};");

  if (localVariables.v_temp)
    println("\tPPCVRegister vTemp{{}};");

  if (localVariables.ea)
    println("\tuint32_t ea{{}};");

  // If we have SEH scopes and exception handlers are enabled, emit the SEH_TRY opening brace and
  // indent body
  if (generateSeh) {
    println("\tSEH_TRY {{");
    // Add extra indentation to body content for SEH blocks
    // Replace each newline+tab with newline+tab+tab
    std::string indentedBody;
    indentedBody.reserve(tempString.size() + tempString.size() / 20);  // ~5% overhead
    for (size_t i = 0; i < tempString.size(); ++i) {
      indentedBody += tempString[i];
      if (tempString[i] == '\n' && i + 1 < tempString.size() && tempString[i + 1] == '\t') {
        indentedBody += '\t';  // Add extra tab after newline before existing tab
      }
    }
    out += indentedBody;
  } else {
    // Append the function body after the declarations
    out += tempString;
  }

  return allRecompiled;
}

bool Recompiler::recompile(bool force) {
  // Block code generation if validation failed (unless --force)
  if (validationFailed_ && !force) {
    REXCODEGEN_ERROR(
        "Code generation blocked: validation errors detected. Use --force to override.");
    return false;
  }

  REXCODEGEN_TRACE("Recompile: starting");
  out.reserve(kOutputBufferReserveSize);

  // Build sorted function list from graph for code generation
  std::vector<const FunctionNode*> functions;
  functions.reserve(graph().functionCount());
  for (const auto& [addr, node] : graph().functions()) {
    functions.push_back(node.get());
  }
  std::sort(functions.begin(), functions.end(),
            [](const auto* a, const auto* b) { return a->base() < b->base(); });

  // Build rexcrt reverse map and rename graph nodes so all call sites
  // emit the rexcrt_ name instead of sub_XXXXXXXX.
  std::unordered_map<uint32_t, std::string> rexcrtByAddr;
  for (const auto& [name, addr] : config().rexcrtFunctions) {
    auto crtName = fmt::format("rexcrt_{}", name);
    rexcrtByAddr[addr] = crtName;
    if (auto* node = graph().getFunction(addr)) {
      node->setName(std::move(crtName));
    }
  }

  // Use project name for all output file naming (default: "rex")
  const std::string& projectName = config().projectName;

  REXCODEGEN_TRACE("Recompile: generating {}_config.h", projectName);
  {
    REXCODEGEN_TRACE("  {}_config.h: step 1", projectName);
    println("#pragma once");

    println("#ifndef PPC_CONFIG_H_INCLUDED");
    println("#define PPC_CONFIG_H_INCLUDED\n");
    REXCODEGEN_TRACE("  {}_config.h: step 2", projectName);

    if (config().skipLr)
      println("#define PPC_CONFIG_SKIP_LR");
    if (config().ctrAsLocalVariable)
      println("#define PPC_CONFIG_CTR_AS_LOCAL");
    if (config().xerAsLocalVariable)
      println("#define PPC_CONFIG_XER_AS_LOCAL");
    if (config().reservedRegisterAsLocalVariable)
      println("#define PPC_CONFIG_RESERVED_AS_LOCAL");
    if (config().skipMsr)
      println("#define PPC_CONFIG_SKIP_MSR");
    if (config().crRegistersAsLocalVariables)
      println("#define PPC_CONFIG_CR_AS_LOCAL");
    if (config().nonArgumentRegistersAsLocalVariables)
      println("#define PPC_CONFIG_NON_ARGUMENT_AS_LOCAL");
    if (config().nonVolatileRegistersAsLocalVariables)
      println("#define PPC_CONFIG_NON_VOLATILE_AS_LOCAL");

    println("");

    REXCODEGEN_TRACE(
        "  ppc_config.h: step 3 - binary().baseAddress()=0x{:X}, binary().imageSize()={}",
        binary().baseAddress(), binary().imageSize());
    println("#define PPC_IMAGE_BASE 0x{:X}ull", binary().baseAddress());
    println("#define PPC_IMAGE_SIZE 0x{:X}ull", binary().imageSize());

    REXCODEGEN_TRACE("  ppc_config.h: step 4 - iterating sections");
    // Extract the address of the minimum code segment to store the function table at.
    size_t codeMin = ~0;
    size_t codeMax = 0;

    for (const auto& section : binary().sections()) {
      if (section.executable) {
        if (section.baseAddress < codeMin)
          codeMin = section.baseAddress;

        if ((section.baseAddress + section.size) > codeMax)
          codeMax = (section.baseAddress + section.size);
      }
    }

    println("#define PPC_CODE_BASE 0x{:X}ull", codeMin);
    println("#define PPC_CODE_SIZE 0x{:X}ull", codeMax - codeMin);

    // Emit rexcrt heap flag -- set to 1 when [rexcrt] contains heap functions,
    // 0 otherwise. Consumed by PPCImageInfo to auto-init the heap after
    // LoadXexImage (originals are stripped so init is required).
    bool hasRexcrtHeap = config().rexcrtFunctions.contains("RtlAllocateHeap");
    println("");
    println("#define REXCRT_HEAP {}", hasRexcrtHeap ? 1 : 0);

    println("");

    // Prebuilt PPCImageInfo (defined in {project}_init.cpp)
    println("#include <rex/ppc/image_info.h>");
    println("extern const rex::PPCImageInfo PPCImageConfig;");

    println("\n#endif");

    REXCODEGEN_TRACE("  {}_config.h: step 5 - saving", projectName);
    SaveCurrentOutData(fmt::format("{}_config.h", projectName));
    REXCODEGEN_TRACE("  {}_config.h: done", projectName);
  }

  REXCODEGEN_TRACE("Recompile: generating {}_init.h", projectName);
  {
    println("#pragma once\n");
    println("#include \"{}_config.h\"", projectName);
    println("#include <rex/ppc.h>");
    println("#include <rex/logging.h>  // For REX_FATAL on unresolved calls");

    for (const auto* fn : functions) {
      auto crtIt = rexcrtByAddr.find(static_cast<uint32_t>(fn->base()));
      if (crtIt != rexcrtByAddr.end()) {
        println("PPC_EXTERN_FUNC({});", crtIt->second);
        continue;
      }

      std::string func_name;
      if (fn->base() == analysisState().entryPoint) {
        func_name = "xstart";
      } else if (!fn->name().empty()) {
        func_name = fn->name();
      } else {
        func_name = fmt::format("sub_{:08X}", fn->base());
      }

      println("PPC_EXTERN_IMPORT({});", func_name);
    }

    // Import declarations (kernel/system functions)
    println("\n// Import function declarations");
    for (const auto& [addr, node] : graph().functions()) {
      if (node->authority() != FunctionAuthority::IMPORT)
        continue;
      println("PPC_EXTERN_IMPORT({});", node->name());
    }

    // Function mapping table (defined in {project}_init.cpp)
    println("\n// Function mapping table - iterate to register functions with processor");

    SaveCurrentOutData(fmt::format("{}_init.h", projectName));
  }

  REXCODEGEN_TRACE("Recompile: generating {}_init.cpp (function mapping table)", projectName);
  {
    println("//=============================================================================");
    println("// ReXGlue Generated - {} Function Mapping Table", projectName);
    println("//=============================================================================\n");
    println("#include \"{}_init.h\"\n", projectName);

    size_t funcMappingCodeMin = ~0ull;
    for (const auto& section : binary().sections()) {
      if (section.executable) {
        if (section.baseAddress < funcMappingCodeMin)
          funcMappingCodeMin = section.baseAddress;
      }
    }

    println("PPCFuncMapping PPCFuncMappings[] = {{");

    for (const auto* fn : functions) {
      if (fn->base() < funcMappingCodeMin)
        continue;

      auto crtIt = rexcrtByAddr.find(static_cast<uint32_t>(fn->base()));
      if (crtIt != rexcrtByAddr.end()) {
        println("\t{{ 0x{:X}, {} }},", fn->base(), crtIt->second);
        continue;
      }

      std::string func_name;
      if (fn->base() == analysisState().entryPoint) {
        func_name = "xstart";
      } else if (!fn->name().empty()) {
        func_name = fn->name();
      } else {
        func_name = fmt::format("sub_{:08X}", fn->base());
      }

      println("\t{{ 0x{:X}, {} }},", fn->base(), func_name);
    }

    // add import thunks to mapping table for indirect call support
    for (const auto& [addr, node] : graph().functions()) {
      if (node->authority() != FunctionAuthority::IMPORT)
        continue;
      println("\t{{ 0x{:X}, {} }},", addr, node->name());
    }

    println("\t{{ 0, nullptr }}");
    println("}};");

    SaveCurrentOutData(fmt::format("{}_init.cpp", projectName));
  }

  REXCODEGEN_TRACE("Recompile: generating {}_config.cpp (PPCImageConfig)", projectName);
  {
    println("//=============================================================================");
    println("// ReXGlue Generated - {} Image Configuration", projectName);
    println("//=============================================================================\n");
    println("#include \"{}_init.h\"\n", projectName);
    println("#include <rex/ppc/image_info.h>");
    println("");
    println("const rex::PPCImageInfo PPCImageConfig = {{");
    println("    PPC_CODE_BASE,      // code_base");
    println("    PPC_CODE_SIZE,      // code_size");
    println("    PPC_IMAGE_BASE,     // image_base");
    println("    PPC_IMAGE_SIZE,     // image_size");
    println("    PPCFuncMappings,    // func_mappings");
    println("    REXCRT_HEAP,        // rexcrt_heap");
    println("}};");

    SaveCurrentOutData(fmt::format("{}_config.cpp", projectName));
  }

  std::erase_if(functions, [](const FunctionNode* fn) {
    return fn->authority() == FunctionAuthority::IMPORT;
  });

  // Remove rexcrt functions -- SDK provides these, no need to recompile
  std::erase_if(functions, [&rexcrtByAddr](const FunctionNode* fn) {
    return rexcrtByAddr.contains(static_cast<uint32_t>(fn->base()));
  });

  // TODO: Add fancy single-line progress indicator
  REXCODEGEN_INFO("Recompiling {} functions...", functions.size());
  for (size_t i = 0; i < functions.size(); i++) {
    if ((i % REXCVAR_GET(functions_per_file)) == 0) {
      SaveCurrentOutData();
      println("#include \"{}_init.h\"\n", projectName);
    }

    recompile(*functions[i]);
  }

  SaveCurrentOutData();
  REXCODEGEN_INFO("Recompilation complete.");

  // Generate sources.cmake for inclusion by parent projects
  // (CMakeLists.txt is NOT generated - parent project handles library creation)
  REXCODEGEN_TRACE("Recompile: generating sources.cmake");
  {
    println("# Auto-generated by rexglue codegen - DO NOT EDIT");
    println("#");
    println("# IMPORTANT: For SEH (Structured Exception Handling) support on Windows,");
    println("# add /EHa to your compile options:");
    println("#   target_compile_options(your_target PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHa>)");
    println("#");
    println("set(GENERATED_SOURCES");
    println("    ${{CMAKE_CURRENT_LIST_DIR}}/{}_config.cpp", projectName);
    println("    ${{CMAKE_CURRENT_LIST_DIR}}/{}_init.cpp", projectName);
    for (size_t i = 0; i < cppFileIndex; ++i) {
      println("    ${{CMAKE_CURRENT_LIST_DIR}}/{}_recomp.{}.cpp", projectName, i);
    }
    println(")");

    SaveCurrentOutData("sources.cmake");
  }

  // Write all buffered files to disk
  FlushPendingWrites();
  return true;
}

void Recompiler::SaveCurrentOutData(const std::string_view& name) {
  if (!out.empty()) {
    std::string filename;

    if (name.empty()) {
      filename = fmt::format("{}_recomp.{}.cpp", config().projectName, cppFileIndex);
      ++cppFileIndex;
    } else {
      filename = std::string(name);
    }

    pendingWrites.emplace_back(std::move(filename), std::move(out));
    out.clear();
  }
}

void Recompiler::FlushPendingWrites() {
  std::filesystem::path outputPath = ctx_->configDir() / config().outDirectoryPath;

  for (const auto& [filename, content] : pendingWrites) {
    std::string filePath = (outputPath / filename).string();
    REXCODEGEN_TRACE("flush_pending_writes: filePath={}", filePath);

    bool shouldWrite = true;

    FILE* f = fopen(filePath.c_str(), "rb");
    if (f) {
      std::vector<uint8_t> temp;

      fseek(f, 0, SEEK_END);
      long fileSize = ftell(f);
      if (fileSize == static_cast<long>(content.size())) {
        fseek(f, 0, SEEK_SET);
        temp.resize(fileSize);
        fread(temp.data(), 1, fileSize, f);

        shouldWrite = !XXH128_isEqual(XXH3_128bits(temp.data(), temp.size()),
                                      XXH3_128bits(content.data(), content.size()));
      }
      fclose(f);
    }

    if (shouldWrite) {
      f = fopen(filePath.c_str(), "wb");
      if (!f) {
        REXCODEGEN_ERROR("Failed to open file for writing: {}", filePath);
        continue;
      }
      fwrite(content.data(), 1, content.size(), f);
      fclose(f);
      REXCODEGEN_TRACE("Wrote {} bytes to {}", content.size(), filePath);
    }
  }

  pendingWrites.clear();
}

}  // namespace rex::codegen
