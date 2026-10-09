#include "ppc/instruction.h"
#include "ppc/disasm.h"
#include "builders/builder_context.h"
#include "builders.h"
#include "mmio_analysis.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <unordered_set>

#include <format>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/function_graph.h>
#include <rex/codegen/function_scanner.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include <dis-asm.h>
#include <ppc.h>

using rex::codegen::ppc::Disassemble;

namespace rex::codegen {

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

FunctionNode::FunctionNode(uint32_t base, uint32_t size, FunctionAuthority authority)
    : base_(base), size_(size), authority_(authority), state_(FunctionState::kRegistered) {
  char buf[32];
  snprintf(buf, sizeof(buf), "sub_%08X", base);
  name_ = buf;
}

void FunctionNode::discover(std::vector<Block> blocks,
                            std::vector<rex::codegen::ppc::Instruction*> instructions,
                            std::set<uint32_t> labels) {
  assert(canDiscover() && "Invalid state transition: must be kRegistered");

  if (!isImport()) {
    assert(!blocks.empty() && "Non-import function must have blocks");
  }

  blocks_ = std::move(blocks);
  instructions_ = std::move(instructions);
  labels_ = std::move(labels);

  for (const auto& block : blocks_) {
    uint32_t blockEnd = block.base + block.size;
    if (blockEnd > base_ + size_) {
      size_ = blockEnd - base_;
    }
  }

  state_ = FunctionState::kDiscovered;
  REXLOG_DEBUG(codegen,
               "FunctionNode 0x{:08X} ({}): DISCOVERED with {} blocks, {} instructions, {} labels",
               base_, name_, blocks_.size(), instructions_.size(), labels_.size());
}

void FunctionNode::discoverAsImport() {
  assert(canDiscover() && "Invalid state transition: must be kRegistered");
  assert(isImport() && "Only imports can use discoverAsImport()");

  state_ = FunctionState::kDiscovered;
  REXLOG_DEBUG(codegen, "FunctionNode 0x{:08X} ({}): DISCOVERED as import", base_, name_);
}

bool FunctionNode::canSeal() const {
  if (state_ != FunctionState::kDiscovered)
    return false;
  if (isImport())
    return true;
  if (blocks_.empty())
    return false;
  if (!unresolvedJumps_.empty())
    return false;
  return true;
}

const FunctionAnalysis& FunctionNode::analysis() const {
  assert(state_ == FunctionState::kSealed && "Analysis only valid after seal");
  assert(analysis_.has_value() && "Analysis not computed");
  return *analysis_;
}

void FunctionNode::addBlock(Block block) {
  blocks_.push_back(block);

  uint32_t blockEnd = block.base + block.size;
  if (blockEnd > base_ + size_) {
    size_ = blockEnd - base_;
  }
}

bool FunctionNode::containsAddress(uint32_t addr) const {
  if (addr < base_ || addr >= base_ + size_) {
    return false;
  }

  if (blocks_.empty()) {
    return true;
  }

  for (const auto& block : blocks_) {
    if (block.contains(addr)) {
      return true;
    }
  }

  // CONFIG and PDATA trust the declared size, as out-of-line switch cases sit after the epilogue.
  if (authority_ == FunctionAuthority::CONFIG || authority_ == FunctionAuthority::PDATA) {
    return true;
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

  for (auto it = unresolvedJumps_.begin(); it != unresolvedJumps_.end();) {
    if (it->target == newFunction->base()) {
      REXLOG_TRACE(codegen,
                   "FunctionNode 0x{:08X}: resolved jump 0x{:08X} -> 0x{:08X} as external to {}",
                   base_, it->site, it->target, newFunction->name());

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
      REXLOG_TRACE(codegen,
                   "FunctionNode 0x{:08X}: resolved jump 0x{:08X} -> 0x{:08X} as import {}", base_,
                   it->site, it->target, importName);

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
  if (!containsAddress(target)) {
    return false;
  }

  addLabel(target);
  return true;
}

void FunctionNode::absorbRegion(uint32_t regionBase, uint32_t regionSize) {
  assert(state_ != FunctionState::kSealed && "Cannot absorb into SEALED function");

  addBlock({regionBase, regionSize});

  REXLOG_DEBUG(codegen, "FunctionNode 0x{:08X}: absorbed region 0x{:08X}-0x{:08X}, new size=0x{:X}",
               base_, regionBase, regionBase + regionSize, size_);
}

void FunctionNode::seal() {
  assert(canSeal() && "Cannot seal: invariants not met");

  std::sort(blocks_.begin(), blocks_.end(),
            [](const Block& a, const Block& b) { return a.base < b.base; });

  if (blocks_.size() > 1) {
    std::vector<Block> merged;
    merged.reserve(blocks_.size());
    merged.push_back(blocks_[0]);

    for (size_t i = 1; i < blocks_.size(); i++) {
      Block& last = merged.back();
      const Block& curr = blocks_[i];

      if (curr.base <= last.end()) {
        uint32_t newEnd = std::max(last.end(), curr.end());
        last.size = newEnd - last.base;
        REXLOG_TRACE(codegen,
                     "FunctionNode 0x{:08X}: merged overlapping blocks 0x{:08X}-0x{:08X} and "
                     "0x{:08X}-0x{:08X}",
                     base_, last.base, last.base + last.size - (newEnd - last.end()), curr.base,
                     curr.end());
      } else {
        merged.push_back(curr);
      }
    }

    if (merged.size() < blocks_.size()) {
      REXLOG_DEBUG(codegen, "FunctionNode 0x{:08X}: reduced {} blocks to {} after merging", base_,
                   blocks_.size(), merged.size());
      blocks_ = std::move(merged);
    }
  }

  analysis_ = FunctionAnalysis{};
  state_ = FunctionState::kSealed;

  REXLOG_TRACE(
      codegen,
      "FunctionNode 0x{:08X} ({}): SEALED with {} blocks, {} labels, {} calls, {} tail calls",
      base_, name_, blocks_.size(), labels_.size(), calls_.size(), tailCalls_.size());
}

namespace {

template <class... Args>
void emit_println(std::string& out, std::format_string<Args...> fmt, Args&&... args) {
  std::vformat_to(std::back_inserter(out), fmt.get(), std::make_format_args(args...));
  out += '\n';
}

template <class... Args>
void emit_print(std::string& out, std::format_string<Args...> fmt, Args&&... args) {
  std::vformat_to(std::back_inserter(out), fmt.get(), std::make_format_args(args...));
}

bool IsSwitchPattern(const u32* data) {
  constexpr u32 kMoveToCtrMask = 0xFC1FFFFF;
  constexpr u32 kMoveToCtr = 0x7C0003A6;
  constexpr u32 kNop = 0x60000000;
  for (i32 back = 1; back <= 3; ++back) {
    const u32 previous = memory::load<be_u32>(data - back);
    if ((previous & kMoveToCtrMask) == kMoveToCtr) {
      return true;
    }
    if (previous != kNop) {
      return false;
    }
  }
  return false;
}

std::unordered_map<u32, JumpTable> DetectLateJumpTables(const FunctionNode& node,
                                                        const EmitContext& ctx) {
  std::unordered_map<u32, JumpTable> tables;
  ppc_insn insn;
  for (const auto& block : node.blocks()) {
    const auto* data = reinterpret_cast<const uint32_t*>(ctx.binary.translate(block.base));
    if (!data) {
      continue;
    }
    for (u32 address = block.base; address < block.end(); address += 4, ++data) {
      if (ctx.config.switchTables.contains(address)) {
        continue;
      }
      Disassemble(data, 4, address, insn);
      if (!insn.opcode || insn.opcode->id != PPC_INST_BCTR || !IsSwitchPattern(data)) {
        continue;
      }
      FunctionScanner scanner(ctx.binary, ctx.limits);
      if (auto table = scanner.detect_jump_table(address)) {
        REXLOG_TRACE(codegen, "Late-detected jump table at 0x{:08X} with {} entries", address,
                     table->targets.size());
        tables.emplace(address, std::move(*table));
      }
    }
  }
  return tables;
}

MMIOClassification ClassifyFunctionMMIO(
    const FunctionNode& node, const EmitContext& ctx,
    const std::unordered_map<u32, JumpTable>& late_jump_tables) {
  std::vector<MMIOCodeBlock> blocks;
  std::vector<JumpTable> jump_tables(node.jumpTables().begin(), node.jumpTables().end());
  std::vector<u32> hook_sites;
  std::vector<u32> hook_targets;
  for (const auto& block : node.blocks()) {
    const u8* bytes = ctx.binary.translate(block.base);
    if (!bytes) {
      continue;
    }
    blocks.push_back({block.base, std::span<const u8>(bytes, block.size)});
    for (u32 address = block.base; address < block.end(); address += 4) {
      if (const auto table = ctx.config.switchTables.find(address);
          table != ctx.config.switchTables.end()) {
        jump_tables.push_back(table->second);
        jump_tables.back().bctrAddress = address;
      }
      if (const auto hook = ctx.config.midAsmHooks.find(address);
          hook != ctx.config.midAsmHooks.end()) {
        hook_sites.push_back(address);
        for (u32 target : {hook->second.jumpAddress, hook->second.jumpAddressOnTrue,
                           hook->second.jumpAddressOnFalse}) {
          if (target != 0) {
            hook_targets.push_back(target);
          }
        }
      }
    }
  }
  for (const auto& [address, table] : late_jump_tables) {
    jump_tables.push_back(table);
  }
  return ClassifyMMIO(
      {blocks, jump_tables, hook_sites, hook_targets, &ctx.config.declaredMMIOAccesses});
}

}  // namespace

std::string FunctionNode::emitCpp(const EmitContext& ctx) const {
  if (authority() == FunctionAuthority::IMPORT) {
    return "";
  }

  std::string out;

  if (blocks().empty()) {
    REXLOG_WARN(codegen, "Function 0x{:08X} has no blocks - generating stub", base());

    std::string name;
    if (base() == ctx.entryPoint) {
      name = "xstart";
    } else if (!name_.empty()) {
      name = name_;
    } else {
      name = std::format("sub_{:08X}", base());
    }

    emit_println(out, "// STUB: Function at 0x{:08X} has no discovered code blocks", base());
    emit_println(out, "DEFINE_REX_FUNC({}) {{", name);
    emit_println(out, "\tREX_FUNC_PROLOGUE();");
    emit_println(out, "}}\n");
    return out;
  }

  const SehExceptionInfo* sehInfo = nullptr;
  if (hasExceptionInfo()) {
    sehInfo = exceptionInfo()->asSeh();
    if (sehInfo && !sehInfo->scopes.empty()) {
      REXLOG_TRACE(codegen, "Function 0x{:08X} has {} SEH scopes", base(), sehInfo->scopes.size());
    }
  }

  std::unordered_set<size_t> labels;
  labels.reserve(64);

  for (const auto& block : blocks()) {
    auto* blockData = reinterpret_cast<const uint32_t*>(ctx.binary.translate(block.base));
    if (!blockData)
      continue;

    for (size_t addr = block.base; addr < block.end(); addr += 4) {
      const uint32_t instruction =
          memory::load<be_u32>(reinterpret_cast<const uint8_t*>(blockData) + addr - block.base);
      if (!PPC_BL(instruction)) {
        const size_t op = PPC_OP(instruction);
        if (op == PPC_OP_B)
          labels.emplace(addr + PPC_BI(instruction));
        else if (op == PPC_OP_BC)
          labels.emplace(addr + PPC_BD(instruction));
      }

      auto stIt = ctx.config.switchTables.find(static_cast<uint32_t>(addr));
      if (stIt != ctx.config.switchTables.end()) {
        for (auto label : stIt->second.targets)
          labels.emplace(label);
      }

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
            default:
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

  for (const auto& jt : jumpTables()) {
    for (auto label : jt.targets) {
      labels.emplace(label);
    }
  }

  const auto late_jump_tables = DetectLateJumpTables(*this, ctx);
  for (const auto& [address, table] : late_jump_tables) {
    for (auto label : table.targets) {
      labels.emplace(label);
    }
  }

  const MMIOClassification mmio = ClassifyFunctionMMIO(*this, ctx, late_jump_tables);
  for (const MMIOSite& site : mmio.sites) {
    if (site.reason == MMIOReason::Unsupported) {
      REXLOG_ERROR(codegen,
                   "0x{:08X} reaches the register block at 0x{:08X} through an instruction "
                   "codegen cannot route, it stays a plain access",
                   site.address, site.target);
    }
    if (ctx.mmio_sites) {
      ctx.mmio_sites->push_back({base(), site});
    }
  }

  std::string name;
  if (base() == ctx.entryPoint) {
    name = "xstart";
  } else if (!name_.empty()) {
    name = name_;
  } else {
    name = std::format("sub_{:08X}", base());
  }

  emit_println(out, "DEFINE_REX_FUNC({}) {{", name);
  emit_println(out, "\tREX_FUNC_PROLOGUE();");

  const bool generateSeh =
      sehInfo && !sehInfo->scopes.empty() && ctx.config.generateExceptionHandlers;

  const JumpTable* activeJt = nullptr;
  CSRState csrState = CSRState::Unknown;
  RecompilerLocalVariables localVariables;

  std::string body;
  body.reserve(4096);

  ppc_insn insn;
  std::unordered_set<size_t> emittedLabels;

  for (const auto& block : blocks()) {
    auto blockBase = block.base;
    auto blockEnd = block.end();
    auto* data = reinterpret_cast<const uint32_t*>(ctx.binary.translate(block.base));
    if (!data) {
      REXLOG_WARN(codegen, "Block 0x{:08X} in function 0x{:08X} has no mapped data - skipping",
                  block.base, base());
      continue;
    }

    while (blockBase < blockEnd) {
      if (labels.find(blockBase) != labels.end() && emittedLabels.insert(blockBase).second) {
        emit_println(body, "loc_{:X}:", blockBase);
        csrState = CSRState::Unknown;
      }

      activeJt = nullptr;
      auto stIt = ctx.config.switchTables.find(blockBase);
      if (stIt != ctx.config.switchTables.end()) {
        activeJt = &stIt->second;
      } else {
        auto lateIt = late_jump_tables.find(blockBase);
        if (lateIt != late_jump_tables.end()) {
          activeJt = &lateIt->second;
        }
      }

      Disassemble(data, 4, blockBase, insn);

      if (insn.opcode == nullptr) {
        emit_println(body, "\t// {}", insn.op_str);
        if (*data != 0)
          REXLOG_WARN(codegen, "Unable to decode instruction {:X} at {:X}", *data, blockBase);
      } else {
        emit_println(body, "\t// {} {}", insn.opcode->name, insn.op_str);

        auto hookIt = ctx.config.midAsmHooks.find(blockBase);
        bool hasHookBefore =
            (hookIt != ctx.config.midAsmHooks.end() && !hookIt->second.afterInstruction);

        int id = insn.opcode->id;
        BuilderContext builderCtx{body,           ctx,      *this,    insn, blockBase, data,
                                  localVariables, csrState, activeJt, &mmio};

        if (hasHookBefore) {
          builderCtx.emit_mid_asm_hook();
        }

        if (!DispatchInstruction(id, builderCtx)) {
          REXLOG_WARN(codegen, "Unrecognized instruction at 0x{:X}: {}", blockBase,
                      insn.opcode->name);
        }

        if (hookIt != ctx.config.midAsmHooks.end() && hookIt->second.afterInstruction) {
          builderCtx.emit_mid_asm_hook();
        }
      }

      blockBase += 4;
      ++data;
    }
  }

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
        ctx.reference(std::format("sub_{:08X}", scope.handler));
        emit_println(body, "\t\t\tsub_{:08X}(ctx, base);  // __finally handler", scope.handler);
      }
    }

    if (sehInfo->restoreHelper != 0) {
      auto* restoreFn = ctx.graph.getFunction(sehInfo->restoreHelper);
      if (restoreFn && !restoreFn->name().empty()) {
        ctx.reference(restoreFn->name());
        emit_println(body, "\t\t\t{}(ctx, base);  // Restore caller registers", restoreFn->name());
      }
    }

    emit_println(body, "\t\t\tSEH_RETHROW;");
    emit_println(body, "\t\t}} SEH_END");
    emit_println(body, "\t}}\n");
  } else {
    emit_println(body, "}}\n");
  }

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

  if (generateSeh) {
    emit_println(out, "\tSEH_TRY {{");
    constexpr std::string_view kReturn = "return;";
    constexpr std::string_view kSehReturn = "REX_SEH_RETURN;";
    for (size_t at = body.find(kReturn); at != std::string::npos;
         at = body.find(kReturn, at + kSehReturn.size())) {
      body.replace(at, kReturn.size(), kSehReturn);
    }
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

FunctionNode* FunctionGraph::addFunction(uint32_t base, uint32_t size, FunctionAuthority authority,
                                         bool hasXrefs) {
  auto it = functions_.find(base);
  if (it != functions_.end()) {
    FunctionNode* existing = it->second.get();

    if (existing->authority() >= authority) {
      REXLOG_TRACE(
          codegen,
          "FunctionGraph: ignoring add of 0x{:08X} ({}) - existing has higher authority ({})", base,
          AuthorityName(authority), AuthorityName(existing->authority()));
      return existing;
    }

    REXLOG_DEBUG(codegen, "FunctionGraph: replacing 0x{:08X} ({}) with ({})", base,
                 AuthorityName(existing->authority()), AuthorityName(authority));
  }

  auto node = std::make_unique<FunctionNode>(base, size, authority);
  FunctionNode* nodePtr = node.get();
  functions_[base] = std::move(node);
  functionsByBase_[base] = nodePtr;

  functionHasXrefs_[base] = hasXrefs;

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
  auto* node = addFunction(address, 4, FunctionAuthority::IMPORT, resolvedName, true);
  REXLOG_TRACE(codegen, "FunctionGraph: added import function 0x{:08X} -> {}", address,
               resolvedName);
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
  REXLOG_TRACE(codegen, "FunctionGraph: removing absorbed function 0x{:08X}", entryPoint);
  functionsByBase_.erase(entryPoint);
  functions_.erase(it);
  functionHasXrefs_.erase(entryPoint);
  return true;
}

FunctionNode* FunctionGraph::getFunctionContaining(uint32_t addr) {
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

  if (auto* targetFn = getFunction(target)) {
    if (isCall) {
      node->addCall(site, CallTarget::function(targetFn));
    } else {
      node->addTailCall(site, CallTarget::function(targetFn));
    }
    REXLOG_TRACE(codegen, "FunctionGraph: immediately resolved 0x{:08X}->0x{:08X} as {} to {}",
                 site, target, isCall ? "call" : "tail call", targetFn->name());
    return;
  }

  if (isImport(target)) {
    auto* importNode = getFunction(target);
    const std::string& importName = importNode->name();
    if (isCall) {
      node->addCall(site, CallTarget::import(target, importName));
    } else {
      node->addTailCall(site, CallTarget::import(target, importName));
    }
    REXLOG_TRACE(codegen,
                 "FunctionGraph: immediately resolved 0x{:08X}->0x{:08X} as {} to import {}", site,
                 target, isCall ? "call" : "tail call", importName);
    return;
  }

  node->addUnresolvedJump(site, target, isCall, conditional);
  REXLOG_TRACE(codegen,
               "FunctionGraph: added unresolved {} 0x{:08X}->0x{:08X} to function 0x{:08X}",
               isCall ? "call" : "jump", site, target, entry);
}

size_t FunctionGraph::tryResolveFunction(uint32_t entry) {
  auto* node = getFunction(entry);
  if (!node || node->isSealed())
    return 0;

  size_t resolved = 0;

  // Copy, since resolution modifies the list.
  auto jumps = node->unresolvedJumps();

  REXLOG_TRACE(codegen, "FunctionGraph::tryResolveFunction 0x{:08X}: {} unresolved jumps", entry,
               jumps.size());

  for (const auto& jump : jumps) {
    if (node->tryResolveAsInternalLabel(jump.target)) {
      REXLOG_TRACE(codegen, "  0x{:08X}->0x{:08X}: resolved as internal label", jump.site,
                   jump.target);
      node->removeUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    if (auto* targetFn = getFunction(jump.target)) {
      REXLOG_TRACE(codegen, "  0x{:08X}->0x{:08X}: resolved as {} to function {}", jump.site,
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

    if (isImport(jump.target)) {
      auto* importNode = getFunction(jump.target);
      const std::string& importName = importNode->name();
      REXLOG_TRACE(codegen, "  0x{:08X}->0x{:08X}: resolved as {} to import {}", jump.site,
                   jump.target, jump.isCall ? "call" : "tail call", importName);
      if (jump.isCall) {
        node->addCall(jump.site, CallTarget::import(jump.target, importName));
      } else {
        node->addTailCall(jump.site, CallTarget::import(jump.target, importName));
      }
      node->removeUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    REXLOG_TRACE(codegen, "  0x{:08X}->0x{:08X}: still unresolved", jump.site, jump.target);
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
        REXLOG_DEBUG(codegen,
                     "FunctionGraph::sealAllReady: 0x{:08X} ({}) cannot seal ({} unresolved)", base,
                     node->name(), node->unresolvedJumps().size());
        for (const auto& jump : node->unresolvedJumps()) {
          REXLOG_DEBUG(codegen, "  0x{:08X} -> 0x{:08X}", jump.site, jump.target);
        }
      }
    }
  }
  REXLOG_DEBUG(codegen, "FunctionGraph::sealAllReady: sealed {} functions, {} could not seal",
               sealed, couldNotSeal);
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
        reason = std::format("{} unresolved jumps", node->unresolvedJumps().size());
      } else {
        reason = "unknown reason";
      }
      errors.emplace_back(base, std::format("{} (0x{:08X}): {}", node->name(), base, reason));
      continue;
    }

    node->seal();
  }

  if (!errors.empty()) {
    std::string msg = std::format("sealAll: {} functions cannot be sealed:\n", errors.size());
    for (const auto& [addr, err] : errors) {
      msg += std::format("  - {}\n", err);
    }
    throw std::runtime_error(msg);
  }

  REXLOG_TRACE(codegen, "FunctionGraph::sealAll: all {} functions sealed", functions_.size());
}

void FunctionGraph::registerChunk(uint32_t base, uint32_t size) {
  chunks_.emplace_back(base, size);
  REXLOG_TRACE(codegen, "FunctionGraph: registered chunk 0x{:08X}-0x{:08X}", base, base + size);
}

/**
 * Reports whether a branch may claim a target address. A forward target on a null dword, inside a
 * registered chunk, or inside a function that is not GAP_FILL is not vacant.
 *
 * @param fromAddr Address of the branch.
 * @param targetAddr Address it targets.
 * @return True when the target is vacant.
 */
bool FunctionGraph::isVacant(uint32_t fromAddr, uint32_t targetAddr) const {
  if (memoryReader_) {
    if (targetAddr > fromAddr) {
      auto val = memoryReader_(targetAddr);
      if (val && *val == 0x00000000) {
        REXLOG_TRACE(codegen, "FunctionGraph::isVacant: null dword at 0x{:08X} blocks vacancy",
                     targetAddr);
        return false;
      }
    }
  }

  for (const auto& [chunkBase, chunkSize] : chunks_) {
    if (targetAddr >= chunkBase && targetAddr < chunkBase + chunkSize) {
      REXLOG_TRACE(codegen, "FunctionGraph::isVacant: chunk 0x{:08X}-0x{:08X} claims 0x{:08X}",
                   chunkBase, chunkBase + chunkSize, targetAddr);
      return false;
    }
  }

  for (const auto& [base, node] : functions_) {
    if (node->containsAddress(targetAddr)) {
      if (isMergeableEntryPoint(targetAddr)) {
        REXLOG_TRACE(codegen, "FunctionGraph::isVacant: 0x{:08X} is mergeable (GAP_FILL)",
                     targetAddr);
        continue;
      }
      REXLOG_TRACE(codegen,
                   "FunctionGraph::isVacant: 0x{:08X} is within protected function 0x{:08X}",
                   targetAddr, base);
      return false;
    }
  }

  return true;
}

/**
 * Marks funclets as sharing the registers of their owner. MSVC compiles __finally and __except
 * bodies as funclets that run on the owner's frame with its non volatiles live, reached by bl from
 * the middle of the owner, so localizing their copies would hand them a zero. The scope table names
 * each funclet, the only reliable test since every prologue reads non volatiles to spill them.
 * C++ EH cleanup and catch funclets run the same way and are marked too. The owner stays localized
 * and syncs across the call site. A funclet with both an unwind entry and an inline bl entry is 2
 * nodes sharing 1 tail, so the whole extent is swept. Funclet ranges are [base, end).
 *
 * @return The number of functions marked.
 */
size_t FunctionGraph::markFuncletRegisterSharing() {
  std::vector<std::pair<uint32_t, uint32_t>> ranges;

  for (auto& entry : functions_) {
    auto& node = entry.second;
    const auto& info = node->exceptionInfo();
    if (!info || !info->hasInfo())
      continue;

    auto claim = [&](uint32_t addr) {
      auto it = addr ? functions_.find(addr) : functions_.end();
      if (it == functions_.end())
        return;
      ranges.emplace_back(it->second->base(), it->second->end());
    };

    if (const auto* seh = info->asSeh()) {
      for (const auto& scope : seh->scopes) {
        claim(scope.handler);
        claim(scope.filter);
      }
    } else if (const auto* cxx = info->asCxx()) {
      for (const auto& entry : cxx->unwindMap) {
        claim(entry.action);
      }
      for (const auto& tryBlock : cxx->tryBlocks) {
        for (const auto& handler : tryBlock.handlers) {
          claim(handler.handlerAddress);
        }
      }
    }
  }

  if (ranges.empty())
    return 0;

  std::sort(ranges.begin(), ranges.end());
  size_t out = 0;
  for (size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].first <= ranges[out].second) {
      ranges[out].second = std::max(ranges[out].second, ranges[i].second);
    } else {
      ranges[++out] = ranges[i];
    }
  }
  ranges.resize(out + 1);

  auto insideFunclet = [&](uint32_t addr) {
    auto it = std::upper_bound(
        ranges.begin(), ranges.end(), addr,
        [](uint32_t a, const std::pair<uint32_t, uint32_t>& r) { return a < r.first; });
    if (it == ranges.begin())
      return false;
    return addr < std::prev(it)->second;
  };

  size_t marked = 0;
  for (const auto& [base, node] : functions_) {
    if (insideFunclet(base) && !node->sharesRegisters()) {
      node->setSharesRegisters(true);
      ++marked;
    }
  }

  return marked;
}

bool FunctionGraph::isMergeableEntryPoint(uint32_t addr) const {
  auto it = functions_.find(addr);
  if (it == functions_.end()) {
    return false;
  }

  const FunctionNode* node = it->second.get();

  return node->authority() == FunctionAuthority::GAP_FILL;
}

/**
 * Classifies a branch target. An import is always a call or tail call. A bl to the caller's own
 * base is a recursive call and a b to it loops to the start. An entry point of another function is
 * a call or tail call even inside a thunk's range. A target inside the caller is an internal label,
 * which for a bl is a rare PIC pattern.
 *
 * @param target Target address.
 * @param callerAddr Address of the branch.
 * @param isCallInstruction True for bl.
 * @return The target kind.
 */
TargetKind FunctionGraph::classifyTarget(uint32_t target, uint32_t callerAddr,
                                         bool isCallInstruction) const {
  const FunctionNode* callerFn = getFunctionContaining(callerAddr);

  if (isImport(target)) {
    return TargetKind::Import;
  }

  if (callerFn && target == callerFn->base()) {
    return isCallInstruction ? TargetKind::Function : TargetKind::InternalLabel;
  }

  if (isEntryPoint(target)) {
    return TargetKind::Function;
  }

  if (callerFn && callerFn->containsAddress(target)) {
    return TargetKind::InternalLabel;
  }

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
