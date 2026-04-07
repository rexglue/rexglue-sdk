/**
 * @file        rexcodegen/function_graph.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc/instruction.h"
#include "ppc/disasm.h"
#include "builders/builder_context.h"
#include "builders.h"

#include <algorithm>
#include <cassert>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/code_emitter.h>
#include <rex/codegen/function_graph.h>
#include <rex/codegen/function_scanner.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include "codegen_logging.h"

#include <dis-asm.h>
#include <ppc.h>

using rex::codegen::ppc::Disassemble;
using rex::memory::load_and_swap;

namespace rex::codegen {

//=============================================================================
// Utility
//=============================================================================

const char* AuthorityName(FunctionAuthority auth) {
  switch (auth) {
    case FunctionAuthority::GAP_FILL:
      return "gap_fill";
    case FunctionAuthority::DISCOVERED:
      return "discovered";
    case FunctionAuthority::VTABLE:
      return "vtable";
    case FunctionAuthority::HELPER:
      return "helper";
    case FunctionAuthority::PDATA:
      return "pdata";
    case FunctionAuthority::CONFIG:
      return "config";
    case FunctionAuthority::IMPORT:
      return "import";
    default:
      return "unknown";
  }
}

//=============================================================================
// FunctionNode
//=============================================================================

FunctionNode::FunctionNode(uint32_t base, uint32_t size, FunctionAuthority authority)
    : base_(base), size_(size), authority_(authority), state_(FunctionState::kRegistered) {
  // Generate default name
  char buf[32];
  snprintf(buf, sizeof(buf), "sub_%08X", base);
  name_ = buf;

  // All functions start kRegistered - waiting for discover() to assign blocks.
  // Authority prevents merging via vacancy checks, not via initial state.
}

//=============================================================================
// State Machine Methods
//=============================================================================

void FunctionNode::discover(std::vector<Block> blocks,
                            std::vector<rex::codegen::ppc::Instruction*> instructions,
                            std::set<uint32_t> labels) {
  assert(canDiscover() && "Invalid state transition: must be kRegistered");

  // Non-imports must have blocks
  if (!isImport()) {
    assert(!blocks.empty() && "Non-import function must have blocks");
  }

  blocks_ = std::move(blocks);
  instructions_ = std::move(instructions);
  labels_ = std::move(labels);

  // Update size based on blocks
  for (const auto& block : blocks_) {
    uint32_t blockEnd = block.base + block.size;
    if (blockEnd > base_ + size_) {
      size_ = blockEnd - base_;
    }
  }

  state_ = FunctionState::kDiscovered;
  REXCODEGEN_DEBUG(
      "FunctionNode 0x{:08X} ({}): DISCOVERED with {} blocks, {} instructions, {} labels", base_,
      name_, blocks_.size(), instructions_.size(), labels_.size());
}

void FunctionNode::discoverAsImport() {
  assert(canDiscover() && "Invalid state transition: must be kRegistered");
  assert(isImport() && "Only imports can use discoverAsImport()");

  // blocks_ and instructions_ remain empty for imports
  state_ = FunctionState::kDiscovered;
  REXCODEGEN_DEBUG("FunctionNode 0x{:08X} ({}): DISCOVERED as import", base_, name_);
}

bool FunctionNode::canSeal() const {
  if (state_ != FunctionState::kDiscovered)
    return false;
  if (isImport())
    return true;  // Imports can seal without blocks
  if (blocks_.empty())
    return false;  // Non-imports must have blocks
  if (!unresolvedJumps_.empty())
    return false;  // All branches must be resolved
  return true;
}

const FunctionAnalysis& FunctionNode::analysis() const {
  assert(state_ == FunctionState::kSealed && "Analysis only valid after seal");
  assert(analysis_.has_value() && "Analysis not computed");
  return *analysis_;
}

void FunctionNode::addBlock(Block block) {
  blocks_.push_back(block);

  // Extend size if block extends past current end
  uint32_t blockEnd = block.base + block.size;
  if (blockEnd > base_ + size_) {
    size_ = blockEnd - base_;
  }
}

bool FunctionNode::containsAddress(uint32_t addr) const {
  // First check overall bounds
  if (addr < base_ || addr >= base_ + size_) {
    return false;
  }

  // If no blocks defined, use linear range
  if (blocks_.empty()) {
    return true;
  }

  // Check individual blocks
  for (const auto& block : blocks_) {
    if (block.contains(addr)) {
      return true;
    }
  }

  // For CONFIG and PDATA functions, trust the declared size even if blocks don't cover it
  // This handles out-of-line switch cases where compiler places code after epilogue
  if (authority_ == FunctionAuthority::CONFIG || authority_ == FunctionAuthority::PDATA) {
    return true;  // Already passed bounds check above
  }

  return false;
}

void FunctionNode::addLabel(uint32_t addr) {
  labels_.insert(addr);
}

void FunctionNode::addCall(uint32_t site, CallTarget target) {
  calls_.push_back({site, std::move(target)});
}

void FunctionNode::addTailCall(uint32_t site, CallTarget target) {
  tailCalls_.push_back({site, std::move(target)});
}

void FunctionNode::addJumpTable(JumpTable jt) {
  // All jump table targets become labels
  for (uint32_t target : jt.targets) {
    labels_.insert(target);
  }
  jumpTables_.push_back(std::move(jt));
}

void FunctionNode::addUnresolvedJump(uint32_t site, uint32_t target, bool isCall,
                                     bool conditional) {
  unresolvedJumps_.push_back({site, target, isCall, conditional});
}

void FunctionNode::removeUnresolvedJump(uint32_t site) {
  auto it = std::remove_if(unresolvedJumps_.begin(), unresolvedJumps_.end(),
                           [site](const UnresolvedJump& j) { return j.site == site; });
  unresolvedJumps_.erase(it, unresolvedJumps_.end());
}

bool FunctionNode::tryResolveAgainst(FunctionNode* newFunction) {
  if (isSealed())
    return false;
  if (!newFunction)
    return false;

  bool anyResolved = false;

  // Check each unresolved jump
  for (auto it = unresolvedJumps_.begin(); it != unresolvedJumps_.end();) {
    if (it->target == newFunction->base()) {
      // Target matches new function's entry point -> external call/tail call
      REXCODEGEN_TRACE(
          "FunctionNode 0x{:08X}: resolved jump 0x{:08X} -> 0x{:08X} as external to {}", base_,
          it->site, it->target, newFunction->name());

      // Add as tail call (unconditional branch to another function)
      addTailCall(it->site, CallTarget::function(newFunction));

      it = unresolvedJumps_.erase(it);
      anyResolved = true;
    } else {
      ++it;
    }
  }

  return anyResolved;
}

bool FunctionNode::tryResolveAgainstImport(uint32_t importAddr, const std::string& importName) {
  if (isSealed())
    return false;

  bool anyResolved = false;

  for (auto it = unresolvedJumps_.begin(); it != unresolvedJumps_.end();) {
    if (it->target == importAddr) {
      REXCODEGEN_TRACE("FunctionNode 0x{:08X}: resolved jump 0x{:08X} -> 0x{:08X} as import {}",
                       base_, it->site, it->target, importName);

      // Add as tail call to import
      addTailCall(it->site, CallTarget::import(importAddr, importName));

      it = unresolvedJumps_.erase(it);
      anyResolved = true;
    } else {
      ++it;
    }
  }

  return anyResolved;
}

bool FunctionNode::tryResolveAsInternalLabel(uint32_t target) {
  // Check if target is within our blocks
  if (!containsAddress(target)) {
    return false;
  }

  // It's internal - add as label
  addLabel(target);
  return true;
}

void FunctionNode::absorbRegion(uint32_t regionBase, uint32_t regionSize) {
  assert(state_ != FunctionState::kSealed && "Cannot absorb into SEALED function");

  // Add as a new block
  addBlock({regionBase, regionSize});

  REXCODEGEN_DEBUG("FunctionNode 0x{:08X}: absorbed region 0x{:08X}-0x{:08X}, new size=0x{:X}",
                   base_, regionBase, regionBase + regionSize, size_);
}

void FunctionNode::seal() {
  assert(canSeal() && "Cannot seal: invariants not met");

  // Sort blocks by address for correct emission order
  std::sort(blocks_.begin(), blocks_.end(),
            [](const Block& a, const Block& b) { return a.base < b.base; });

  // Merge overlapping blocks - can happen when multiple paths reach the same code
  // (e.g., jump table targets and unconditional branches both targeting an epilogue)
  if (blocks_.size() > 1) {
    std::vector<Block> merged;
    merged.reserve(blocks_.size());
    merged.push_back(blocks_[0]);

    for (size_t i = 1; i < blocks_.size(); i++) {
      Block& last = merged.back();
      const Block& curr = blocks_[i];

      // Check for overlap or adjacency
      if (curr.base <= last.end()) {
        // Extend last block to cover both
        uint32_t newEnd = std::max(last.end(), curr.end());
        last.size = newEnd - last.base;
        REXCODEGEN_TRACE(
            "FunctionNode 0x{:08X}: merged overlapping blocks 0x{:08X}-0x{:08X} and "
            "0x{:08X}-0x{:08X}",
            base_, last.base, last.base + last.size - (newEnd - last.end()), curr.base, curr.end());
      } else {
        merged.push_back(curr);
      }
    }

    if (merged.size() < blocks_.size()) {
      REXCODEGEN_DEBUG("FunctionNode 0x{:08X}: reduced {} blocks to {} after merging", base_,
                       blocks_.size(), merged.size());
      blocks_ = std::move(merged);
    }
  }

  // Compute function analysis
  FunctionAnalysis analysis;
  analysis_ = std::move(analysis);
  state_ = FunctionState::kSealed;

  REXCODEGEN_TRACE(
      "FunctionNode 0x{:08X} ({}): SEALED with {} blocks, {} labels, {} calls, {} tail calls",
      base_, name_, blocks_.size(), labels_.size(), calls_.size(), tailCalls_.size());
}

//=============================================================================
// FunctionNode - C++ Code Emission
//=============================================================================

namespace {

// Helper: append formatted text to a raw string (matches Recompiler::println pattern)
template <class... Args>
void emit_println(std::string& out, fmt::format_string<Args...> fmt, Args&&... args) {
  fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
  out += '\n';
}

template <class... Args>
void emit_print(std::string& out, fmt::format_string<Args...> fmt, Args&&... args) {
  fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
}

}  // namespace

std::string FunctionNode::emitCpp(const EmitContext& ctx) const {
  if (authority() == FunctionAuthority::IMPORT) {
    return "";
  }

  std::string out;

  // --- Empty stub for functions with no blocks ---
  if (blocks().empty()) {
    REXCODEGEN_WARN("Function 0x{:08X} has no blocks - generating stub", base());

    std::string name;
    if (base() == ctx.entryPoint) {
      name = "xstart";
    } else if (!name_.empty()) {
      name = name_;
    } else {
      name = fmt::format("sub_{:08X}", base());
    }

    emit_println(out, "// STUB: Function at 0x{:08X} has no discovered code blocks", base());
    emit_println(out, "__attribute__((alias(\"__imp__{}\"))) REX_WEAK_FUNC({});", name, name);
    emit_println(out, "REX_EXTERN(__imp__{}) {{", name);
    emit_println(out, "\tREX_FUNC_PROLOGUE();");
    emit_println(out, "}}\n");
    return out;
  }

  // --- Check for SEH exception info ---
  const SehExceptionInfo* sehInfo = nullptr;
  if (hasExceptionInfo()) {
    sehInfo = exceptionInfo()->asSeh();
    if (sehInfo && !sehInfo->scopes.empty()) {
      REXCODEGEN_TRACE("Function 0x{:08X} has {} SEH scopes", base(), sehInfo->scopes.size());
    }
  }

  // --- First pass: collect labels from all blocks ---
  std::unordered_set<size_t> labels;
  labels.reserve(64);

  for (const auto& block : blocks()) {
    auto* blockData = reinterpret_cast<const uint32_t*>(ctx.binary.translate(block.base));
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

      // Labels from config switch tables
      auto stIt = ctx.config.switchTables.find(static_cast<uint32_t>(addr));
      if (stIt != ctx.config.switchTables.end()) {
        for (auto label : stIt->second.targets)
          labels.emplace(label);
      }

      // Labels and extern declarations from mid-asm hooks
      auto hookIt = ctx.config.midAsmHooks.find(static_cast<uint32_t>(addr));
      if (hookIt != ctx.config.midAsmHooks.end()) {
        if (hookIt->second.returnOnFalse || hookIt->second.returnOnTrue ||
            hookIt->second.jumpAddressOnFalse != 0 || hookIt->second.jumpAddressOnTrue != 0) {
          emit_print(out, "extern bool ");
        } else {
          emit_print(out, "extern void ");
        }

        emit_print(out, "{}(", hookIt->second.name);
        for (auto& reg : hookIt->second.registers) {
          if (out.back() != '(')
            out += ", ";

          switch (reg[0]) {
            case 'c':
              if (reg == "ctr")
                emit_print(out, "PPCRegister& ctr");
              else
                emit_print(out, "PPCCRRegister& {}", reg);
              break;
            case 'x':
              emit_print(out, "PPCXERRegister& xer");
              break;
            case 'r':
              emit_print(out, "PPCRegister& {}", reg);
              break;
            case 'f':
              if (reg == "fpscr")
                emit_print(out, "PPCFPSCRRegister& fpscr");
              else
                emit_print(out, "PPCRegister& {}", reg);
              break;
            case 'v':
              emit_print(out, "PPCVRegister& {}", reg);
              break;
          }
        }

        emit_println(out, ");\n");

        if (hookIt->second.jumpAddress != 0)
          labels.emplace(hookIt->second.jumpAddress);
        if (hookIt->second.jumpAddressOnTrue != 0)
          labels.emplace(hookIt->second.jumpAddressOnTrue);
        if (hookIt->second.jumpAddressOnFalse != 0)
          labels.emplace(hookIt->second.jumpAddressOnFalse);
      }
    }
  }

  // Collect labels from auto-detected jump tables
  for (const auto& jt : jumpTables()) {
    for (auto label : jt.targets) {
      labels.emplace(label);
    }
  }

  // --- Function name ---
  std::string name;
  if (base() == ctx.entryPoint) {
    name = "xstart";
  } else if (!name_.empty()) {
    name = name_;
  } else {
    name = fmt::format("sub_{:08X}", base());
  }

  // Function signature with weak/alias pattern
  emit_println(out, "__attribute__((alias(\"__imp__{}\"))) REX_WEAK_FUNC({});", name, name);
  emit_println(out, "REX_EXTERN(__imp__{}) {{", name);
  emit_println(out, "\tREX_FUNC_PROLOGUE();");

  // --- Second pass: emit instruction code ---
  const JumpTable* activeJt = nullptr;
  bool allRecompiled = true;
  CSRState csrState = CSRState::Unknown;
  RecompilerLocalVariables localVariables;

  // Local map for late-detected jump tables (can't mutate const config)
  std::unordered_map<uint32_t, JumpTable> lateJumpTables;

  std::string body;
  body.reserve(4096);

  ppc_insn insn;
  std::unordered_set<size_t> emittedLabels;

  for (const auto& block : blocks()) {
    auto blockBase = block.base;
    auto blockEnd = block.end();
    auto* data = reinterpret_cast<const uint32_t*>(ctx.binary.translate(block.base));
    if (!data) {
      REXCODEGEN_WARN("Block 0x{:08X} in function 0x{:08X} has no mapped data - skipping",
                      block.base, base());
      continue;
    }

    while (blockBase < blockEnd) {
      // Only emit each label once
      if (labels.find(blockBase) != labels.end() && emittedLabels.insert(blockBase).second) {
        emit_println(body, "loc_{:X}:", blockBase);
        csrState = CSRState::Unknown;
      }

      // Look up switch table for this address
      activeJt = nullptr;
      auto stIt = ctx.config.switchTables.find(blockBase);
      if (stIt != ctx.config.switchTables.end()) {
        activeJt = &stIt->second;
      } else {
        auto lateIt = lateJumpTables.find(blockBase);
        if (lateIt != lateJumpTables.end()) {
          activeJt = &lateIt->second;
        }
      }

      Disassemble(data, 4, blockBase, insn);

      if (insn.opcode == nullptr) {
        emit_println(body, "\t// {}", insn.op_str);
        if (*data != 0)
          REXCODEGEN_WARN("Unable to decode instruction {:X} at {:X}", *data, blockBase);
      } else {
        // Late jump table detection for bctr
        if (insn.opcode->id == PPC_INST_BCTR && !activeJt) {
          bool is_switch_pattern = false;
          constexpr uint32_t MTCTR_MASK = 0xFC1FFFFF;
          constexpr uint32_t MTCTR_OPCODE = 0x7C0003A6;
          constexpr uint32_t NOP = 0x60000000;

          for (int i = 1; i <= 3 && !is_switch_pattern; i++) {
            uint32_t prev_insn = load_and_swap<uint32_t>(data - i);
            if ((prev_insn & MTCTR_MASK) == MTCTR_OPCODE) {
              is_switch_pattern = true;
              for (int j = 1; j < i; j++) {
                if (load_and_swap<uint32_t>(data - j) != NOP) {
                  is_switch_pattern = false;
                  break;
                }
              }
            } else if (prev_insn != NOP) {
              break;
            }
          }

          if (is_switch_pattern) {
            FunctionScanner scanner(ctx.binary);
            auto jt_opt = scanner.detect_jump_table(blockBase);
            if (jt_opt.has_value()) {
              lateJumpTables.emplace(blockBase, std::move(*jt_opt));
              activeJt = &lateJumpTables.at(blockBase);
              for (auto label : activeJt->targets) {
                labels.emplace(label);
              }
              REXCODEGEN_INFO("Late-detected jump table at 0x{:08X} with {} entries", blockBase,
                              activeJt->targets.size());
            }
          }
        }

        // Emit comment with instruction disassembly
        emit_println(body, "\t// {} {}", insn.opcode->name, insn.op_str);

        // Check for mid-asm hook BEFORE instruction
        auto hookIt = ctx.config.midAsmHooks.find(blockBase);
        bool hasHookBefore =
            (hookIt != ctx.config.midAsmHooks.end() && !hookIt->second.afterInstruction);

        // Dispatch instruction to builder
        int id = insn.opcode->id;
        BuilderContext builderCtx{body,           ctx,      *this,   insn, blockBase, data,
                                  localVariables, csrState, activeJt};

        if (hasHookBefore) {
          builderCtx.emit_mid_asm_hook();
        }

        if (!DispatchInstruction(id, builderCtx)) {
          REXCODEGEN_WARN("Unrecognized instruction at 0x{:X}: {}", blockBase, insn.opcode->name);
          allRecompiled = false;
        }

        // Check for mid-asm hook AFTER instruction
        if (hookIt != ctx.config.midAsmHooks.end() && hookIt->second.afterInstruction) {
          builderCtx.emit_mid_asm_hook();
        }
      }

      blockBase += 4;
      ++data;
    }
  }

  // --- Close function body (or SEH try block) ---
  bool generateSeh = sehInfo && !sehInfo->scopes.empty() && ctx.config.generateExceptionHandlers;
  if (generateSeh) {
    emit_println(body, "\t\t}} SEH_CATCH_ALL {{");
    emit_println(body, "\t\t\tREXLOG_WARN(\"SEH exception caught in sub_{:08X}\");", base());

    if (sehInfo->frameSize > 0) {
      emit_println(body, "\t\t\tctx.r12.s64 = ctx.r31.s64 + {};  // Establisher frame pointer",
                   sehInfo->frameSize);
    }

    for (auto it = sehInfo->scopes.rbegin(); it != sehInfo->scopes.rend(); ++it) {
      const auto& scope = *it;
      if (scope.filter == 0 && scope.handler != 0) {
        emit_println(body, "\t\t\tsub_{:08X}(ctx, base);  // __finally handler", scope.handler);
      }
    }

    if (sehInfo->restoreHelper != 0) {
      auto* restoreFn = ctx.graph.getFunction(sehInfo->restoreHelper);
      if (restoreFn && !restoreFn->name().empty()) {
        emit_println(body, "\t\t\t{}(ctx, base);  // Restore caller registers", restoreFn->name());
      }
    }

    emit_println(body, "\t\t\tSEH_RETHROW;");
    emit_println(body, "\t\t}} SEH_END");
    emit_println(body, "\t}}\n");
  } else {
    emit_println(body, "}}\n");
  }

  // --- Emit local variable declarations, then body ---
  if (localVariables.ctr)
    emit_println(out, "\tPPCRegister ctr{{}};");
  if (localVariables.xer)
    emit_println(out, "\tPPCXERRegister xer{{}};");
  if (localVariables.reserved)
    emit_println(out, "\tPPCRegister reserved{{}};");

  for (size_t i = 0; i < 8; i++) {
    if (localVariables.cr[i])
      emit_println(out, "\tPPCCRRegister cr{}{{}};", i);
  }

  for (size_t i = 0; i < 32; i++) {
    if (localVariables.r[i])
      emit_println(out, "\tPPCRegister r{}{{}};", i);
  }

  for (size_t i = 0; i < 32; i++) {
    if (localVariables.f[i])
      emit_println(out, "\tPPCRegister f{}{{}};", i);
  }

  for (size_t i = 0; i < 128; i++) {
    if (localVariables.v[i])
      emit_println(out, "\tPPCVRegister v{}{{}};", i);
  }

  if (localVariables.env)
    emit_println(out, "\tPPCContext env{{}};");
  if (localVariables.temp)
    emit_println(out, "\tPPCRegister temp{{}};");
  if (localVariables.v_temp)
    emit_println(out, "\tPPCVRegister vTemp{{}};");
  if (localVariables.ea)
    emit_println(out, "\tuint32_t ea{{}};");

  // If SEH, emit SEH_TRY and indent body
  if (generateSeh) {
    emit_println(out, "\tSEH_TRY {{");
    std::string indentedBody;
    indentedBody.reserve(body.size() + body.size() / 20);
    for (size_t i = 0; i < body.size(); ++i) {
      indentedBody += body[i];
      if (body[i] == '\n' && i + 1 < body.size() && body[i + 1] == '\t') {
        indentedBody += '\t';
      }
    }
    out += indentedBody;
  } else {
    out += body;
  }

  return out;
}

void FunctionGraph::addCodeBuffer(uint32_t baseAddress, const uint8_t* data, size_t size) {
  assert_always("FunctionGraph::addCodeBuffer not implemented. Use Recompiler with builders");
  return;
  // CodeBuffer buffer;
  // buffer.baseAddress = baseAddress;
  // buffer.data.assign(data, data + size);
  // codeBuffers_.push_back(std::move(buffer));
  // REXCODEGEN_DEBUG("FunctionGraph: added code buffer 0x{:08X}-0x{:08X} ({} bytes)",
  //                 baseAddress, baseAddress + static_cast<uint32_t>(size), size);
}

const uint8_t* FunctionGraph::translateCode(uint32_t addr) const {
  assert_always("FunctionGraph::translateCode not implemented. Use Recompiler with builders");
  return nullptr;
  // for (const auto& buffer : codeBuffers_) {
  //     if (buffer.contains(addr)) {
  //         return buffer.translate(addr);
  //     }
  // }
  // return nullptr;
}

void FunctionGraph::updateFunctionCodePointers() {
  // size_t updated = 0;
  // for (auto& [base, node] : functions_) {
  //     const uint8_t* code = translateCode(base);
  //     if (code) {
  //         node->setCode(code);
  //         updated++;
  //     } else {
  //         REXCODEGEN_WARN("FunctionGraph: no code buffer for function 0x{:08X}", base);
  //     }
  // }
  // REXCODEGEN_DEBUG("FunctionGraph: updated code pointers for {} functions", updated);
}

//=============================================================================
// FunctionGraph - Function Management
//=============================================================================

FunctionNode* FunctionGraph::addFunction(uint32_t base, uint32_t size, FunctionAuthority authority,
                                         bool hasXrefs) {
  // Check for existing function at this address
  auto it = functions_.find(base);
  if (it != functions_.end()) {
    FunctionNode* existing = it->second.get();

    // Higher authority wins
    if (existing->authority() >= authority) {
      REXCODEGEN_TRACE(
          "FunctionGraph: ignoring add of 0x{:08X} ({}) - existing has higher authority ({})", base,
          AuthorityName(authority), AuthorityName(existing->authority()));
      return existing;
    }

    // Replace with higher authority
    REXCODEGEN_DEBUG("FunctionGraph: replacing 0x{:08X} ({}) with ({})", base,
                     AuthorityName(existing->authority()), AuthorityName(authority));
  }

  // Create new node
  auto node = std::make_unique<FunctionNode>(base, size, authority);
  FunctionNode* nodePtr = node.get();
  functions_[base] = std::move(node);
  functionsByBase_[base] = nodePtr;

  // Track xrefs for merge eligibility
  functionHasXrefs_[base] = hasXrefs;

  // Notify all PENDING functions
  notifyFunctionAdded(nodePtr);

  return nodePtr;
}

FunctionNode* FunctionGraph::addFunction(uint32_t base, uint32_t size, FunctionAuthority authority,
                                         std::string_view name, bool hasXrefs) {
  auto* node = addFunction(base, size, authority, hasXrefs);
  if (node && !name.empty()) {
    node->setName(std::string(name));
  }
  return node;
}

FunctionNode* FunctionGraph::addImportFunction(uint32_t address, std::string_view resolvedName) {
  // Add as function with IMPORT authority
  auto* node = addFunction(address, 4, FunctionAuthority::IMPORT, resolvedName, true);
  REXCODEGEN_TRACE("FunctionGraph: added import function 0x{:08X} -> {}", address, resolvedName);
  return node;
}

FunctionNode* FunctionGraph::getFunction(uint32_t entryPoint) {
  auto it = functions_.find(entryPoint);
  return it != functions_.end() ? it->second.get() : nullptr;
}

const FunctionNode* FunctionGraph::getFunction(uint32_t entryPoint) const {
  auto it = functions_.find(entryPoint);
  return it != functions_.end() ? it->second.get() : nullptr;
}

bool FunctionGraph::removeFunction(uint32_t entryPoint) {
  auto it = functions_.find(entryPoint);
  if (it == functions_.end()) {
    return false;
  }
  REXCODEGEN_TRACE("FunctionGraph: removing absorbed function 0x{:08X}", entryPoint);
  functionsByBase_.erase(entryPoint);
  functions_.erase(it);
  functionHasXrefs_.erase(entryPoint);  // Clean up xref tracking
  return true;
}

FunctionNode* FunctionGraph::getFunctionContaining(uint32_t addr) {
  // O(log f) lookup via sorted base index: find last function with base <= addr
  auto it = functionsByBase_.upper_bound(addr);
  if (it != functionsByBase_.begin()) {
    --it;
    if (it->second->containsAddress(addr)) {
      return it->second;
    }
  }
  return nullptr;
}

const FunctionNode* FunctionGraph::getFunctionContaining(uint32_t addr) const {
  auto it = functionsByBase_.upper_bound(addr);
  if (it != functionsByBase_.begin()) {
    --it;
    if (it->second->containsAddress(addr)) {
      return it->second;
    }
  }
  return nullptr;
}

bool FunctionGraph::isEntryPoint(uint32_t addr) const {
  return functions_.contains(addr);
}

bool FunctionGraph::isImport(uint32_t addr) const {
  auto it = functions_.find(addr);
  return it != functions_.end() && it->second->authority() == FunctionAuthority::IMPORT;
}

std::vector<FunctionNode*> FunctionGraph::getPendingFunctions() {
  std::vector<FunctionNode*> result;
  for (auto& [base, node] : functions_) {
    if (node->isPending()) {
      result.push_back(node.get());
    }
  }
  return result;
}

std::vector<FunctionNode*> FunctionGraph::getSealedFunctions() {
  std::vector<FunctionNode*> result;
  for (auto& [base, node] : functions_) {
    if (node->isSealed()) {
      result.push_back(node.get());
    }
  }
  return result;
}

size_t FunctionGraph::pendingCount() const {
  size_t count = 0;
  for (const auto& [base, node] : functions_) {
    if (node->isPending())
      ++count;
  }
  return count;
}

size_t FunctionGraph::sealedCount() const {
  size_t count = 0;
  for (const auto& [base, node] : functions_) {
    if (node->isSealed())
      ++count;
  }
  return count;
}

//=============================================================================
// Function Setup (called during Discover phase)
//=============================================================================

void FunctionGraph::setFunctionName(uint32_t entry, std::string name) {
  if (auto* node = getFunction(entry)) {
    node->setName(std::move(name));
  }
}

void FunctionGraph::setFunctionHasExceptionHandler(uint32_t entry, bool val) {
  if (auto* node = getFunction(entry)) {
    node->setHasExceptionHandler(val);
  }
}

void FunctionGraph::setFunctionExceptionInfo(uint32_t entry, ExceptionInfo info) {
  if (auto* node = getFunction(entry)) {
    node->setExceptionInfo(std::move(info));
  }
}

void FunctionGraph::addBlockToFunction(uint32_t entry, Block block) {
  if (auto* node = getFunction(entry)) {
    node->addBlock(block);
  }
}

void FunctionGraph::addLabelToFunction(uint32_t entry, uint32_t label) {
  if (auto* node = getFunction(entry)) {
    node->addLabel(label);
  }
}

void FunctionGraph::addCallToFunction(uint32_t entry, uint32_t site, CallTarget target) {
  if (auto* node = getFunction(entry)) {
    node->addCall(site, std::move(target));
  }
}

void FunctionGraph::addTailCallToFunction(uint32_t entry, uint32_t site, CallTarget target) {
  if (auto* node = getFunction(entry)) {
    node->addTailCall(site, std::move(target));
  }
}

void FunctionGraph::addJumpTableToFunction(uint32_t entry, JumpTable jt) {
  if (auto* node = getFunction(entry)) {
    node->addJumpTable(std::move(jt));
  }
}

void FunctionGraph::addUnresolvedJumpToFunction(uint32_t entry, uint32_t site, uint32_t target,
                                                bool isCall, bool conditional) {
  auto* node = getFunction(entry);
  if (!node)
    return;

  // Try immediate resolution against existing functions/imports
  if (auto* targetFn = getFunction(target)) {
    // Target is a known function - resolve as call or tail call
    if (isCall) {
      node->addCall(site, CallTarget::function(targetFn));
    } else {
      node->addTailCall(site, CallTarget::function(targetFn));
    }
    REXCODEGEN_TRACE("FunctionGraph: immediately resolved 0x{:08X}->0x{:08X} as {} to {}", site,
                     target, isCall ? "call" : "tail call", targetFn->name());
    return;
  }

  if (isImport(target)) {
    // Target is an import - resolve as call or tail call to import
    auto* importNode = getFunction(target);
    const std::string& importName = importNode->name();
    if (isCall) {
      node->addCall(site, CallTarget::import(target, importName));
    } else {
      node->addTailCall(site, CallTarget::import(target, importName));
    }
    REXCODEGEN_TRACE("FunctionGraph: immediately resolved 0x{:08X}->0x{:08X} as {} to import {}",
                     site, target, isCall ? "call" : "tail call", importName);
    return;
  }

  // Not resolvable yet - add as unresolved
  node->addUnresolvedJump(site, target, isCall, conditional);
  REXCODEGEN_TRACE("FunctionGraph: added unresolved {} 0x{:08X}->0x{:08X} to function 0x{:08X}",
                   isCall ? "call" : "jump", site, target, entry);
}

//=============================================================================
// Resolution and Expansion (called during Merge phase)
//=============================================================================

size_t FunctionGraph::tryResolveFunction(uint32_t entry) {
  auto* node = getFunction(entry);
  if (!node || node->isSealed())
    return 0;

  size_t resolved = 0;

  // Get copy of unresolved jumps (list will be modified)
  auto jumps = node->unresolvedJumps();

  REXCODEGEN_TRACE("FunctionGraph::tryResolveFunction 0x{:08X}: {} unresolved jumps", entry,
                   jumps.size());

  for (const auto& jump : jumps) {
    // Try internal label first
    if (node->tryResolveAsInternalLabel(jump.target)) {
      REXCODEGEN_TRACE("  0x{:08X}->0x{:08X}: resolved as internal label", jump.site, jump.target);
      node->removeUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    // Try function entry
    if (auto* targetFn = getFunction(jump.target)) {
      REXCODEGEN_TRACE("  0x{:08X}->0x{:08X}: resolved as {} to function {}", jump.site,
                       jump.target, jump.isCall ? "call" : "tail call", targetFn->name());
      if (jump.isCall) {
        node->addCall(jump.site, CallTarget::function(targetFn));
      } else {
        node->addTailCall(jump.site, CallTarget::function(targetFn));
      }
      node->removeUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    // Try import
    if (isImport(jump.target)) {
      auto* importNode = getFunction(jump.target);
      const std::string& importName = importNode->name();
      REXCODEGEN_TRACE("  0x{:08X}->0x{:08X}: resolved as {} to import {}", jump.site, jump.target,
                       jump.isCall ? "call" : "tail call", importName);
      if (jump.isCall) {
        node->addCall(jump.site, CallTarget::import(jump.target, importName));
      } else {
        node->addTailCall(jump.site, CallTarget::import(jump.target, importName));
      }
      node->removeUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    REXCODEGEN_TRACE("  0x{:08X}->0x{:08X}: still unresolved", jump.site, jump.target);
  }

  return resolved;
}

void FunctionGraph::absorbRegionIntoFunction(uint32_t entry, uint32_t regionBase,
                                             uint32_t regionSize) {
  if (auto* node = getFunction(entry)) {
    node->absorbRegion(regionBase, regionSize);
  }
}

bool FunctionGraph::trySealFunction(uint32_t entry) {
  auto* node = getFunction(entry);
  if (!node || node->isSealed())
    return false;

  if (node->canSeal()) {
    node->seal();
    return true;
  }
  return false;
}

size_t FunctionGraph::sealAllReady() {
  size_t sealed = 0;
  size_t couldNotSeal = 0;
  for (auto& [base, node] : functions_) {
    if (node->isPending()) {
      if (node->canSeal()) {
        node->seal();
        sealed++;
      } else {
        couldNotSeal++;
        REXCODEGEN_WARN("FunctionGraph::sealAllReady: 0x{:08X} ({}) cannot seal ({} unresolved)",
                        base, node->name(), node->unresolvedJumps().size());
        for (const auto& jump : node->unresolvedJumps()) {
          REXCODEGEN_WARN("  0x{:08X} -> 0x{:08X}", jump.site, jump.target);
        }
      }
    }
  }
  REXCODEGEN_DEBUG("FunctionGraph::sealAllReady: sealed {} functions, {} could not seal", sealed,
                   couldNotSeal);
  return sealed;
}

void FunctionGraph::sealAll() {
  std::vector<std::pair<uint32_t, std::string>> errors;

  for (auto& [base, node] : functions_) {
    if (node->isSealed())
      continue;

    if (!node->canSeal()) {
      std::string reason;
      if (node->isRegistered()) {
        reason = "still in kRegistered state (blocks not discovered)";
      } else if (node->blocks().empty() && !node->isImport()) {
        reason = "has 0 blocks (non-import)";
      } else if (!node->unresolvedJumps().empty()) {
        reason = fmt::format("{} unresolved jumps", node->unresolvedJumps().size());
      } else {
        reason = "unknown reason";
      }
      errors.emplace_back(base, fmt::format("{} (0x{:08X}): {}", node->name(), base, reason));
      continue;
    }

    node->seal();
  }

  if (!errors.empty()) {
    std::string msg = fmt::format("sealAll: {} functions cannot be sealed:\n", errors.size());
    for (const auto& [addr, err] : errors) {
      msg += fmt::format("  - {}\n", err);
    }
    throw std::runtime_error(msg);
  }

  REXCODEGEN_INFO("FunctionGraph::sealAll: all {} functions sealed", functions_.size());
}

//=============================================================================
// Vacancy Checking
//=============================================================================

void FunctionGraph::registerChunk(uint32_t base, uint32_t size) {
  chunks_.emplace_back(base, size);
  REXCODEGEN_TRACE("FunctionGraph: registered chunk 0x{:08X}-0x{:08X}", base, base + size);
}

bool FunctionGraph::isVacant(uint32_t fromAddr, uint32_t targetAddr) const {
  // Rule 1: Check for null dword at boundary
  if (memoryReader_) {
    if (targetAddr > fromAddr) {
      // Check for null at target (typical inter-function padding)
      auto val = memoryReader_(targetAddr);
      if (val && *val == 0x00000000) {
        REXCODEGEN_TRACE("FunctionGraph::isVacant: null dword at 0x{:08X} blocks vacancy",
                         targetAddr);
        return false;
      }
    }
  }

  // Rule 2: Check if any chunk claims the target region
  for (const auto& [chunkBase, chunkSize] : chunks_) {
    if (targetAddr >= chunkBase && targetAddr < chunkBase + chunkSize) {
      REXCODEGEN_TRACE("FunctionGraph::isVacant: chunk 0x{:08X}-0x{:08X} claims 0x{:08X}",
                       chunkBase, chunkBase + chunkSize, targetAddr);
      return false;
    }
  }

  // Rule 3: Check if target falls within any function's range
  for (const auto& [base, node] : functions_) {
    if (node->containsAddress(targetAddr)) {
      // Target is within a function - check if it's mergeable
      if (isMergeableEntryPoint(targetAddr)) {
        // GAP_FILL can be absorbed
        REXCODEGEN_TRACE("FunctionGraph::isVacant: 0x{:08X} is mergeable (GAP_FILL)", targetAddr);
        continue;  // Don't block, it's mergeable
      }
      // Protected function - blocks vacancy
      REXCODEGEN_TRACE("FunctionGraph::isVacant: 0x{:08X} is within protected function 0x{:08X}",
                       targetAddr, base);
      return false;
    }
  }

  // green light for vacancy
  return true;
}

bool FunctionGraph::isMergeableEntryPoint(uint32_t addr) const {
  // Only entry points can be mergeable
  auto it = functions_.find(addr);
  if (it == functions_.end()) {
    return false;  // Not an entry point
  }

  const FunctionNode* node = it->second.get();

  // Only GAP_FILL authority can be absorbed
  // All other authorities represent immutable entry points
  return node->authority() == FunctionAuthority::GAP_FILL;
}

TargetKind FunctionGraph::classifyTarget(uint32_t target, uint32_t callerAddr,
                                         bool isCallInstruction) const {
  // Find the caller's function
  const FunctionNode* callerFn = getFunctionContaining(callerAddr);

  // Case 1: Target is an import - always a call/tail-call
  if (isImport(target)) {
    return TargetKind::Import;
  }

  // Case 2: Target is the caller's own entry point
  if (callerFn && target == callerFn->base()) {
    // bl to own base = recursive call (Function)
    // b to own base = loop back to start (InternalLabel)
    return isCallInstruction ? TargetKind::Function : TargetKind::InternalLabel;
  }

  // Case 3: Target is a DIFFERENT function's entry point - this is a call/tail-call
  // This handles cases where a small thunk function branches to another function
  // whose entry point happens to fall within the thunk's address range
  if (isEntryPoint(target)) {
    return TargetKind::Function;
  }

  // Case 4: Target is inside caller's function -> InternalLabel
  // For bl, this would be a rare PIC code pattern
  if (callerFn && callerFn->containsAddress(target)) {
    return TargetKind::InternalLabel;
  }

  // Case 5: Unknown target
  return TargetKind::Unknown;
}

void FunctionGraph::notifyFunctionAdded(FunctionNode* newFunction) {
  for (auto& [base, node] : functions_) {
    if (node.get() != newFunction && node->isPending()) {
      node->tryResolveAgainst(newFunction);
    }
  }
}

}  // namespace rex::codegen
