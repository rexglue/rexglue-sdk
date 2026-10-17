#include "ppc/instruction.h"
#include "ppc/disasm.h"
#include "ppc/opcode.h"
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

#include <rex/assert.h>
#include <rex/codegen/binary_view.h>
#include <rex/codegen/function_graph.h>
#include <rex/logging/log.h>
#include <rex/memory/byte_swap.h>

#include <dis-asm.h>
#include <ppc.h>

using rex::codegen::detail::Disassemble;
using rex::codegen::detail::ResolveID;

namespace rex::codegen {

const char* AuthorityName(FunctionAuthority auth) {
  switch (auth) {
    case FunctionAuthority::kGapFill:
      return "gap_fill";
    case FunctionAuthority::kDiscovered:
      return "discovered";
    case FunctionAuthority::kVTable:
      return "vtable";
    case FunctionAuthority::kHelper:
      return "helper";
    case FunctionAuthority::kPData:
      return "pdata";
    case FunctionAuthority::kConfig:
      return "config";
    case FunctionAuthority::kImport:
      return "import";
    default:
      return "unknown";
  }
}

FunctionNode::FunctionNode(u32 base, u32 size, FunctionAuthority authority)
    : base_(base), size_(size), authority_(authority), state_(FunctionState::kRegistered) {
  char buf[32];
  snprintf(buf, sizeof(buf), "sub_%08X", base);
  name_ = buf;
}

void FunctionNode::Discover(std::vector<Block> blocks,
                            std::vector<rex::codegen::detail::Instruction*> instructions,
                            std::set<u32> labels) {
  assert(CanDiscover() && "Invalid state transition: must be kRegistered");

  if (!IsImport()) {
    assert(!blocks.empty() && "Non-import function must have blocks");
  }

  blocks_ = std::move(blocks);
  instructions_ = std::move(instructions);
  labels_ = std::move(labels);

  for (const auto& block : blocks_) {
    u32 block_end = block.base + block.size;
    if (block_end > base_ + size_) {
      size_ = block_end - base_;
    }
  }

  state_ = FunctionState::kDiscovered;
  REXLOG_DEBUG(codegen,
               "FunctionNode 0x{:08X} ({}): DISCOVERED with {} blocks, {} instructions, {} labels",
               base_, name_, blocks_.size(), instructions_.size(), labels_.size());
}

void FunctionNode::DiscoverAsImport() {
  assert(CanDiscover() && "Invalid state transition: must be kRegistered");
  assert(IsImport() && "Only imports can use discoverAsImport()");

  state_ = FunctionState::kDiscovered;
  REXLOG_DEBUG(codegen, "FunctionNode 0x{:08X} ({}): DISCOVERED as import", base_, name_);
}

bool FunctionNode::CanSeal() const {
  if (state_ != FunctionState::kDiscovered)
    return false;
  if (IsImport())
    return true;
  if (blocks_.empty())
    return false;
  if (!unresolved_jumps_.empty())
    return false;
  return true;
}

const FunctionAnalysis& FunctionNode::GetAnalysis() const {
  REX_ASSERT(state_ == FunctionState::kSealed, "Analysis only valid after seal");
  return analysis_;
}

void FunctionNode::AddBlock(Block block) {
  blocks_.push_back(block);

  u32 block_end = block.base + block.size;
  if (block_end > base_ + size_) {
    size_ = block_end - base_;
  }
}

bool FunctionNode::ContainsAddress(u32 addr) const {
  if (addr < base_ || addr >= base_ + size_) {
    return false;
  }

  if (blocks_.empty()) {
    return true;
  }

  for (const auto& block : blocks_) {
    if (block.Contains(addr)) {
      return true;
    }
  }

  // CONFIG and PDATA trust the declared size, as out-of-line switch cases sit after the epilogue.
  if (authority_ == FunctionAuthority::kConfig || authority_ == FunctionAuthority::kPData) {
    return true;
  }

  return false;
}

void FunctionNode::AddLabel(u32 addr) {
  labels_.insert(addr);
}

void FunctionNode::AddCall(u32 site, CallTarget target) {
  calls_.push_back({site, std::move(target)});
}

void FunctionNode::AddTailCall(u32 site, CallTarget target) {
  tail_calls_.push_back({site, std::move(target)});
}

void FunctionNode::AddJumpTable(JumpTable jt) {
  for (u32 target : jt.targets) {
    labels_.insert(target);
  }
  jump_tables_.push_back(std::move(jt));
}

void FunctionNode::AddUnresolvedJump(u32 site, u32 target, bool is_call, bool conditional) {
  unresolved_jumps_.push_back({site, target, is_call, conditional});
}

void FunctionNode::RemoveUnresolvedJump(u32 site) {
  auto it = std::remove_if(unresolved_jumps_.begin(), unresolved_jumps_.end(),
                           [site](const UnresolvedJump& j) { return j.site == site; });
  unresolved_jumps_.erase(it, unresolved_jumps_.end());
}

bool FunctionNode::TryResolveAgainst(FunctionNode* new_function) {
  if (IsSealed())
    return false;
  if (!new_function)
    return false;

  bool any_resolved = false;

  for (auto it = unresolved_jumps_.begin(); it != unresolved_jumps_.end();) {
    if (it->target == new_function->GetBase()) {
      REXLOG_TRACE(codegen,
                   "FunctionNode 0x{:08X}: resolved jump 0x{:08X} -> 0x{:08X} as external to {}",
                   base_, it->site, it->target, new_function->GetName());

      AddTailCall(it->site, CallTarget::MakeFunction(new_function));

      it = unresolved_jumps_.erase(it);
      any_resolved = true;
    } else {
      ++it;
    }
  }

  return any_resolved;
}

bool FunctionNode::TryResolveAgainstImport(u32 import_addr, const std::string& import_name) {
  if (IsSealed())
    return false;

  bool any_resolved = false;

  for (auto it = unresolved_jumps_.begin(); it != unresolved_jumps_.end();) {
    if (it->target == import_addr) {
      REXLOG_TRACE(codegen,
                   "FunctionNode 0x{:08X}: resolved jump 0x{:08X} -> 0x{:08X} as import {}", base_,
                   it->site, it->target, import_name);

      AddTailCall(it->site, CallTarget::MakeImport(import_addr, import_name));

      it = unresolved_jumps_.erase(it);
      any_resolved = true;
    } else {
      ++it;
    }
  }

  return any_resolved;
}

bool FunctionNode::TryResolveAsInternalLabel(u32 target) {
  if (!ContainsAddress(target)) {
    return false;
  }

  AddLabel(target);
  return true;
}

void FunctionNode::AbsorbRegion(u32 region_base, u32 region_size) {
  assert(state_ != FunctionState::kSealed && "Cannot absorb into SEALED function");

  AddBlock({region_base, region_size});

  REXLOG_DEBUG(codegen, "FunctionNode 0x{:08X}: absorbed region 0x{:08X}-0x{:08X}, new size=0x{:X}",
               base_, region_base, region_base + region_size, size_);
}

void FunctionNode::Seal() {
  assert(CanSeal() && "Cannot seal: invariants not met");

  std::sort(blocks_.begin(), blocks_.end(),
            [](const Block& a, const Block& b) { return a.base < b.base; });

  if (blocks_.size() > 1) {
    std::vector<Block> merged;
    merged.reserve(blocks_.size());
    merged.push_back(blocks_[0]);

    for (size_t i = 1; i < blocks_.size(); i++) {
      Block& last = merged.back();
      const Block& curr = blocks_[i];

      if (curr.base <= last.GetEnd()) {
        u32 new_end = std::max(last.GetEnd(), curr.GetEnd());
        last.size = new_end - last.base;
        REXLOG_TRACE(codegen,
                     "FunctionNode 0x{:08X}: merged overlapping blocks 0x{:08X}-0x{:08X} and "
                     "0x{:08X}-0x{:08X}",
                     base_, last.base, last.base + last.size - (new_end - last.GetEnd()), curr.base,
                     curr.GetEnd());
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
      base_, name_, blocks_.size(), labels_.size(), calls_.size(), tail_calls_.size());
}

namespace {

template <class... Args>
void EmitPrintln(std::string& out, std::format_string<Args...> fmt, Args&&... args) {
  std::vformat_to(std::back_inserter(out), fmt.get(), std::make_format_args(args...));
  out += '\n';
}

template <class... Args>
void EmitPrint(std::string& out, std::format_string<Args...> fmt, Args&&... args) {
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
    const auto* data = reinterpret_cast<const u32*>(ctx.binary.translate(block.base));
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
  std::vector<JumpTable> jump_tables(node.GetJumpTables().begin(), node.GetJumpTables().end());
  std::vector<u32> hook_sites;
  std::vector<u32> hook_targets;
  for (const auto& block : node.GetBlocks()) {
    const u8* bytes = ctx.binary.Translate(block.base);
    if (!bytes) {
      continue;
    }
    blocks.push_back({block.base, std::span<const u8>(bytes, block.size)});
    for (u32 address = block.base; address < block.GetEnd(); address += 4) {
      if (const auto table = ctx.config.switch_tables.find(address);
          table != ctx.config.switch_tables.end()) {
        jump_tables.push_back(table->second);
        jump_tables.back().bctr_address = address;
      }
      if (const auto hook = ctx.config.mid_asm_hooks.find(address);
          hook != ctx.config.mid_asm_hooks.end()) {
        hook_sites.push_back(address);
        for (u32 target : {hook->second.jump_address, hook->second.jump_address_on_true,
                           hook->second.jump_address_on_false}) {
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
      {blocks, jump_tables, hook_sites, hook_targets, &ctx.config.declared_mmio_accesses});
}

}  // namespace

std::string FunctionNode::EmitCpp(const EmitContext& ctx) const {
  if (GetAuthority() == FunctionAuthority::kImport) {
    return "";
  }

  std::string out;

  if (GetBlocks().empty()) {
    REXLOG_WARN(codegen, "Function 0x{:08X} has no blocks - generating stub", GetBase());

    std::string name;
    if (GetBase() == ctx.entry_point) {
      name = "xstart";
    } else if (!name_.empty()) {
      name = name_;
    } else {
      name = std::format("sub_{:08X}", GetBase());
    }

    EmitPrintln(out, "// STUB: Function at 0x{:08X} has no discovered code blocks", GetBase());
    EmitPrintln(out, "DEFINE_REX_FUNC({}) {{", name);
    EmitPrintln(out, "\tREX_FUNC_PROLOGUE();");
    EmitPrintln(out, "}}\n");
    return out;
  }

  const auto& info = GetExceptionData();
  const SEHExceptionInfo* seh_info = info ? info->AsSEH() : nullptr;
  if (seh_info && !seh_info->scopes.empty()) {
    REXLOG_TRACE(codegen, "Function 0x{:08X} has {} SEH scopes", GetBase(),
                 seh_info->scopes.size());
  }

  std::unordered_set<size_t> labels;
  labels.reserve(64);

  for (const auto& block : GetBlocks()) {
    auto* block_data = reinterpret_cast<const u32*>(ctx.binary.Translate(block.base));
    if (!block_data)
      continue;

    for (size_t addr = block.base; addr < block.GetEnd(); addr += 4) {
      const u32 instruction =
          memory::load<be_u32>(reinterpret_cast<const u8*>(block_data) + addr - block.base);
      if (!PPC_BL(instruction)) {
        const size_t op = PPC_OP(instruction);
        if (op == PPC_OP_B)
          labels.emplace(addr + PPC_BI(instruction));
        else if (op == PPC_OP_BC)
          labels.emplace(addr + PPC_BD(instruction));
      }

      auto st_it = ctx.config.switch_tables.find(static_cast<u32>(addr));
      if (st_it != ctx.config.switch_tables.end()) {
        for (auto label : st_it->second.targets)
          labels.emplace(label);
      }

      auto hook_it = ctx.config.mid_asm_hooks.find(static_cast<u32>(addr));
      if (hook_it != ctx.config.mid_asm_hooks.end()) {
        if (hook_it->second.return_on_false || hook_it->second.return_on_true ||
            hook_it->second.jump_address_on_false != 0 ||
            hook_it->second.jump_address_on_true != 0) {
          EmitPrint(out, "extern bool ");
        } else {
          EmitPrint(out, "extern void ");
        }

        EmitPrint(out, "{}(", hook_it->second.name);
        for (auto& reg : hook_it->second.registers) {
          if (out.back() != '(')
            out += ", ";

          switch (reg[0]) {
            case 'c':
              if (reg == "ctr")
                EmitPrint(out, "PPCRegister& ctr");
              else
                EmitPrint(out, "PPCCRRegister& {}", reg);
              break;
            case 'x':
              EmitPrint(out, "PPCXERRegister& xer");
              break;
            case 'r':
              EmitPrint(out, "PPCRegister& {}", reg);
              break;
            case 'f':
              if (reg == "fpscr")
                EmitPrint(out, "PPCFPSCRRegister& fpscr");
              else
                EmitPrint(out, "PPCRegister& {}", reg);
              break;
            case 'v':
              EmitPrint(out, "PPCVRegister& {}", reg);
              break;
            default:
              break;
          }
        }

        EmitPrintln(out, ");\n");

        if (hook_it->second.jump_address != 0)
          labels.emplace(hook_it->second.jump_address);
        if (hook_it->second.jump_address_on_true != 0)
          labels.emplace(hook_it->second.jump_address_on_true);
        if (hook_it->second.jump_address_on_false != 0)
          labels.emplace(hook_it->second.jump_address_on_false);
      }
    }
  }

  for (const auto& jt : GetJumpTables()) {
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
    if (site.reason == MMIOReason::kUnsupported) {
      REXLOG_ERROR(codegen,
                   "0x{:08X} reaches the register block at 0x{:08X} through an instruction "
                   "codegen cannot route, it stays a plain access",
                   site.address, site.target);
    }
    if (ctx.mmio_sites) {
      ctx.mmio_sites->push_back({GetBase(), site});
    }
  }

  std::string name;
  if (GetBase() == ctx.entry_point) {
    name = "xstart";
  } else if (!name_.empty()) {
    name = name_;
  } else {
    name = std::format("sub_{:08X}", GetBase());
  }

  EmitPrintln(out, "DEFINE_REX_FUNC({}) {{", name);
  EmitPrintln(out, "\tREX_FUNC_PROLOGUE();");

  const bool generate_seh =
      seh_info && !seh_info->scopes.empty() && ctx.config.generate_exception_handlers;

  const JumpTable* active_jt = nullptr;
  CSRState csr_state = CSRState::kUnknown;
  RecompilerLocalVariables local_variables;

  std::string body;
  body.reserve(4096);

  ppc_insn insn;
  std::unordered_set<size_t> emitted_labels;

  for (const auto& block : GetBlocks()) {
    auto block_base = block.base;
    auto block_end = block.GetEnd();
    auto* data = reinterpret_cast<const u32*>(ctx.binary.Translate(block.base));
    if (!data) {
      REXLOG_WARN(codegen, "Block 0x{:08X} in function 0x{:08X} has no mapped data - skipping",
                  block.base, GetBase());
      continue;
    }

    while (block_base < block_end) {
      if (labels.find(block_base) != labels.end() && emitted_labels.insert(block_base).second) {
        EmitPrintln(body, "loc_{:X}:", block_base);
        csr_state = CSRState::kUnknown;
      }

      active_jt = nullptr;
      auto st_it = ctx.config.switch_tables.find(block_base);
      if (st_it != ctx.config.switch_tables.end()) {
        active_jt = &st_it->second;
      }

      Disassemble(data, 4, block_base, insn);

      if (insn.opcode == nullptr) {
        EmitPrintln(body, "\t// {}", insn.op_str);
        if (*data != 0)
          REXLOG_WARN(codegen, "Unable to decode instruction {:X} at {:X}", *data, block_base);
      } else {
        EmitPrintln(body, "\t// {} {}", insn.opcode->name, insn.op_str);

        auto hook_it = ctx.config.mid_asm_hooks.find(block_base);
        bool has_hook_before =
            (hook_it != ctx.config.mid_asm_hooks.end() && !hook_it->second.after_instruction);

        int id = ResolveID(insn);
        BuilderContext builder_ctx{
            body, ctx, *this, insn, block_base, data, local_variables, csr_state, active_jt, &mmio};

        if (has_hook_before) {
          builder_ctx.EmitMidAsmHook();
        }

        if (!DispatchInstruction(id, builder_ctx)) {
          REXLOG_WARN(codegen, "Unrecognized instruction at 0x{:X}: {}", block_base,
                      insn.opcode->name);
        }

        if (hook_it != ctx.config.mid_asm_hooks.end() && hook_it->second.after_instruction) {
          builder_ctx.EmitMidAsmHook();
        }
      }

      block_base += 4;
      ++data;
    }

    if (GetFallThroughs().contains(block_end)) {
      BuilderContext builder_ctx{body,      ctx,     *this, insn, block_end, data, local_variables,
                                 csr_state, nullptr, &mmio};
      builder_ctx.EmitFunctionCall(block_end);
      EmitPrintln(body, "	return;");
    }
  }

  if (generate_seh) {
    EmitPrintln(body, "\t\t}} REX_SEH_CATCH_ALL {{");
    EmitPrintln(body, "\t\t\tREXLOG_WARN(\"SEH exception caught in sub_{:08X}\");", GetBase());

    if (seh_info->frame_size > 0) {
      EmitPrintln(body, "\t\t\tctx.r12.s64 = ctx.r31.s64 + {};  // Establisher frame pointer",
                  seh_info->frame_size);
    }

    for (auto it = seh_info->scopes.rbegin(); it != seh_info->scopes.rend(); ++it) {
      const auto& scope = *it;
      if (scope.filter == 0 && scope.handler != 0) {
        ctx.Reference(std::format("sub_{:08X}", scope.handler));
        EmitPrintln(body, "\t\t\tsub_{:08X}(ctx, base);  // __finally handler", scope.handler);
      }
    }

    for (u32 restore_helper : seh_info->restore_helpers) {
      auto* restore_fn = ctx.graph.GetFunction(restore_helper);
      if (restore_fn && !restore_fn->GetName().empty()) {
        ctx.Reference(restore_fn->GetName());
        EmitPrintln(body, "\t\t\t{}(ctx, base);  // Restore caller registers",
                    restore_fn->GetName());
      }
    }

    EmitPrintln(body, "\t\t\tREX_SEH_RETHROW;");
    EmitPrintln(body, "\t\t}} REX_SEH_END");
    EmitPrintln(body, "\t}}\n");
  } else {
    EmitPrintln(body, "}}\n");
  }

  if (local_variables.ctr)
    EmitPrintln(out, "\tPPCRegister ctr{{}};");
  if (local_variables.xer)
    EmitPrintln(out, "\tPPCXERRegister xer{{}};");
  if (local_variables.reserved)
    EmitPrintln(out, "\tPPCRegister reserved{{}};");

  for (size_t i = 0; i < 8; i++) {
    if (local_variables.cr[i])
      EmitPrintln(out, "\tPPCCRRegister cr{}{{}};", i);
  }

  for (size_t i = 0; i < 32; i++) {
    if (local_variables.r[i])
      EmitPrintln(out, "\tPPCRegister r{}{{}};", i);
  }

  for (size_t i = 0; i < 32; i++) {
    if (local_variables.f[i])
      EmitPrintln(out, "\tPPCRegister f{}{{}};", i);
  }

  for (size_t i = 0; i < 128; i++) {
    if (local_variables.v[i])
      EmitPrintln(out, "\tPPCVRegister v{}{{}};", i);
  }

  if (local_variables.env)
    EmitPrintln(out, "\tPPCContext env{{}};");
  if (local_variables.temp)
    EmitPrintln(out, "\tPPCRegister temp{{}};");
  if (local_variables.v_temp)
    EmitPrintln(out, "\tPPCVRegister vTemp{{}};");
  if (local_variables.ea)
    EmitPrintln(out, "\tu32 ea{{}};");

  if (generate_seh) {
    EmitPrintln(out, "\tREX_SEH_TRY {{");
    constexpr std::string_view kReturn = "return;";
    constexpr std::string_view kSEHReturn = "REX_SEH_RETURN;";
    for (size_t at = body.find(kReturn); at != std::string::npos;
         at = body.find(kReturn, at + kSEHReturn.size())) {
      body.replace(at, kReturn.size(), kSEHReturn);
    }
    std::string indented_body;
    indented_body.reserve(body.size() + body.size() / 20);
    for (size_t i = 0; i < body.size(); ++i) {
      indented_body += body[i];
      if (body[i] == '\n' && i + 1 < body.size() && body[i + 1] == '\t') {
        indented_body += '\t';
      }
    }
    out += indented_body;
  } else {
    out += body;
  }

  return out;
}

FunctionNode* FunctionGraph::AddFunction(u32 base, u32 size, FunctionAuthority authority,
                                         bool has_xrefs) {
  auto it = functions_.find(base);
  if (it != functions_.end()) {
    FunctionNode* existing = it->second.get();

    if (existing->GetAuthority() >= authority) {
      REXLOG_TRACE(
          codegen,
          "FunctionGraph: ignoring add of 0x{:08X} ({}) - existing has higher authority ({})", base,
          AuthorityName(authority), AuthorityName(existing->GetAuthority()));
      return existing;
    }

    REXLOG_DEBUG(codegen, "FunctionGraph: replacing 0x{:08X} ({}) with ({})", base,
                 AuthorityName(existing->GetAuthority()), AuthorityName(authority));
  }

  auto node = std::make_unique<FunctionNode>(base, size, authority);
  FunctionNode* node_ptr = node.get();
  functions_[base] = std::move(node);
  functions_by_base_[base] = node_ptr;

  function_has_xrefs_[base] = has_xrefs;

  NotifyFunctionAdded(node_ptr);

  return node_ptr;
}

FunctionNode* FunctionGraph::AddFunction(u32 base, u32 size, FunctionAuthority authority,
                                         std::string_view name, bool has_xrefs) {
  auto* node = AddFunction(base, size, authority, has_xrefs);
  if (node && !name.empty()) {
    node->SetName(std::string(name));
  }
  return node;
}

FunctionNode* FunctionGraph::AddImportFunction(u32 address, std::string_view resolved_name,
                                               u32 size) {
  auto* node = AddFunction(address, size, FunctionAuthority::kImport, resolved_name, true);
  REXLOG_TRACE(codegen, "FunctionGraph: added import function 0x{:08X} -> {}", address,
               resolved_name);
  return node;
}

FunctionNode* FunctionGraph::GetFunction(u32 entry_point) {
  auto it = functions_.find(entry_point);
  return it != functions_.end() ? it->second.get() : nullptr;
}

const FunctionNode* FunctionGraph::GetFunction(u32 entry_point) const {
  auto it = functions_.find(entry_point);
  return it != functions_.end() ? it->second.get() : nullptr;
}

bool FunctionGraph::RemoveFunction(u32 entry_point) {
  auto it = functions_.find(entry_point);
  if (it == functions_.end()) {
    return false;
  }
  REXLOG_TRACE(codegen, "FunctionGraph: removing absorbed function 0x{:08X}", entry_point);
  functions_by_base_.erase(entry_point);
  functions_.erase(it);
  function_has_xrefs_.erase(entry_point);
  return true;
}

FunctionNode* FunctionGraph::GetFunctionContaining(u32 addr) {
  auto it = functions_by_base_.upper_bound(addr);
  if (it != functions_by_base_.begin()) {
    --it;
    if (it->second->ContainsAddress(addr)) {
      return it->second;
    }
  }
  return nullptr;
}

const FunctionNode* FunctionGraph::GetFunctionContaining(u32 addr) const {
  auto it = functions_by_base_.upper_bound(addr);
  if (it != functions_by_base_.begin()) {
    --it;
    if (it->second->ContainsAddress(addr)) {
      return it->second;
    }
  }
  return nullptr;
}

bool FunctionGraph::IsEntryPoint(u32 addr) const {
  return functions_.contains(addr);
}

bool FunctionGraph::IsImport(u32 addr) const {
  auto it = functions_.find(addr);
  return it != functions_.end() && it->second->GetAuthority() == FunctionAuthority::kImport;
}

std::vector<FunctionNode*> FunctionGraph::GetPendingFunctions() {
  std::vector<FunctionNode*> result;
  for (auto& [base, node] : functions_) {
    if (node->IsPending()) {
      result.push_back(node.get());
    }
  }
  return result;
}

std::vector<FunctionNode*> FunctionGraph::GetSealedFunctions() {
  std::vector<FunctionNode*> result;
  for (auto& [base, node] : functions_) {
    if (node->IsSealed()) {
      result.push_back(node.get());
    }
  }
  return result;
}

size_t FunctionGraph::GetPendingCount() const {
  size_t count = 0;
  for (const auto& [base, node] : functions_) {
    if (node->IsPending())
      ++count;
  }
  return count;
}

size_t FunctionGraph::GetSealedCount() const {
  size_t count = 0;
  for (const auto& [base, node] : functions_) {
    if (node->IsSealed())
      ++count;
  }
  return count;
}

void FunctionGraph::SetFunctionName(u32 entry, std::string name) {
  if (auto* node = GetFunction(entry)) {
    node->SetName(std::move(name));
  }
}

void FunctionGraph::SetFunctionMillicode(u32 entry, MillicodeEntry millicode) {
  if (auto* node = GetFunction(entry)) {
    node->SetMillicode(millicode);
  }
}

void FunctionGraph::SetFunctionHasExceptionHandler(u32 entry, bool val) {
  if (auto* node = GetFunction(entry)) {
    node->SetHasExceptionHandler(val);
  }
}

void FunctionGraph::SetFunctionExceptionInfo(u32 entry, ExceptionInfo info) {
  if (auto* node = GetFunction(entry)) {
    node->SetExceptionData(std::move(info));
  }
}

void FunctionGraph::AddBlockToFunction(u32 entry, Block block) {
  if (auto* node = GetFunction(entry)) {
    node->AddBlock(block);
  }
}

void FunctionGraph::AddLabelToFunction(u32 entry, u32 label) {
  if (auto* node = GetFunction(entry)) {
    node->AddLabel(label);
  }
}

void FunctionGraph::AddCallToFunction(u32 entry, u32 site, CallTarget target) {
  if (auto* node = GetFunction(entry)) {
    node->AddCall(site, std::move(target));
  }
}

void FunctionGraph::AddTailCallToFunction(u32 entry, u32 site, CallTarget target) {
  if (auto* node = GetFunction(entry)) {
    node->AddTailCall(site, std::move(target));
  }
}

void FunctionGraph::AddJumpTableToFunction(u32 entry, JumpTable jt) {
  if (auto* node = GetFunction(entry)) {
    node->AddJumpTable(std::move(jt));
  }
}

void FunctionGraph::AddUnresolvedJumpToFunction(u32 entry, u32 site, u32 target, bool is_call,
                                                bool conditional) {
  auto* node = GetFunction(entry);
  if (!node)
    return;

  if (auto* target_fn = GetFunction(target)) {
    if (is_call) {
      node->AddCall(site, CallTarget::MakeFunction(target_fn));
    } else {
      node->AddTailCall(site, CallTarget::MakeFunction(target_fn));
    }
    REXLOG_TRACE(codegen, "FunctionGraph: immediately resolved 0x{:08X}->0x{:08X} as {} to {}",
                 site, target, is_call ? "call" : "tail call", target_fn->GetName());
    return;
  }

  if (IsImport(target)) {
    auto* import_node = GetFunction(target);
    const std::string& import_name = import_node->GetName();
    if (is_call) {
      node->AddCall(site, CallTarget::MakeImport(target, import_name));
    } else {
      node->AddTailCall(site, CallTarget::MakeImport(target, import_name));
    }
    REXLOG_TRACE(codegen,
                 "FunctionGraph: immediately resolved 0x{:08X}->0x{:08X} as {} to import {}", site,
                 target, is_call ? "call" : "tail call", import_name);
    return;
  }

  node->AddUnresolvedJump(site, target, is_call, conditional);
  REXLOG_TRACE(codegen,
               "FunctionGraph: added unresolved {} 0x{:08X}->0x{:08X} to function 0x{:08X}",
               is_call ? "call" : "jump", site, target, entry);
}

size_t FunctionGraph::TryResolveFunction(u32 entry) {
  auto* node = GetFunction(entry);
  if (!node || node->IsSealed())
    return 0;

  size_t resolved = 0;

  // Copy, since resolution modifies the list.
  auto jumps = node->GetUnresolvedJumps();

  REXLOG_TRACE(codegen, "FunctionGraph::tryResolveFunction 0x{:08X}: {} unresolved jumps", entry,
               jumps.size());

  for (const auto& jump : jumps) {
    if (node->TryResolveAsInternalLabel(jump.target)) {
      REXLOG_TRACE(codegen, "  0x{:08X}->0x{:08X}: resolved as internal label", jump.site,
                   jump.target);
      node->RemoveUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    if (auto* target_fn = GetFunction(jump.target)) {
      REXLOG_TRACE(codegen, "  0x{:08X}->0x{:08X}: resolved as {} to function {}", jump.site,
                   jump.target, jump.is_call ? "call" : "tail call", target_fn->GetName());
      if (jump.is_call) {
        node->AddCall(jump.site, CallTarget::MakeFunction(target_fn));
      } else {
        node->AddTailCall(jump.site, CallTarget::MakeFunction(target_fn));
      }
      node->RemoveUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    if (IsImport(jump.target)) {
      auto* import_node = GetFunction(jump.target);
      const std::string& import_name = import_node->GetName();
      REXLOG_TRACE(codegen, "  0x{:08X}->0x{:08X}: resolved as {} to import {}", jump.site,
                   jump.target, jump.is_call ? "call" : "tail call", import_name);
      if (jump.is_call) {
        node->AddCall(jump.site, CallTarget::MakeImport(jump.target, import_name));
      } else {
        node->AddTailCall(jump.site, CallTarget::MakeImport(jump.target, import_name));
      }
      node->RemoveUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    REXLOG_TRACE(codegen, "  0x{:08X}->0x{:08X}: still unresolved", jump.site, jump.target);
  }

  return resolved;
}

void FunctionGraph::AbsorbRegionIntoFunction(u32 entry, u32 region_base, u32 region_size) {
  if (auto* node = GetFunction(entry)) {
    node->AbsorbRegion(region_base, region_size);
  }
}

bool FunctionGraph::TrySealFunction(u32 entry) {
  auto* node = GetFunction(entry);
  if (!node || node->IsSealed())
    return false;

  if (node->CanSeal()) {
    node->Seal();
    return true;
  }
  return false;
}

size_t FunctionGraph::SealAllReady() {
  size_t sealed = 0;
  size_t could_not_seal = 0;
  for (auto& [base, node] : functions_) {
    if (node->IsPending()) {
      if (node->CanSeal()) {
        node->Seal();
        sealed++;
      } else {
        could_not_seal++;
        REXLOG_DEBUG(codegen,
                     "FunctionGraph::sealAllReady: 0x{:08X} ({}) cannot seal ({} unresolved)", base,
                     node->GetName(), node->GetUnresolvedJumps().size());
        for (const auto& jump : node->GetUnresolvedJumps()) {
          REXLOG_DEBUG(codegen, "  0x{:08X} -> 0x{:08X}", jump.site, jump.target);
        }
      }
    }
  }
  REXLOG_DEBUG(codegen, "FunctionGraph::sealAllReady: sealed {} functions, {} could not seal",
               sealed, could_not_seal);
  return sealed;
}

void FunctionGraph::SealAll() {
  std::vector<std::pair<u32, std::string>> errors;

  for (auto& [base, node] : functions_) {
    if (node->IsSealed())
      continue;

    if (!node->CanSeal()) {
      std::string reason;
      if (node->IsRegistered()) {
        reason = "still in kRegistered state (blocks not discovered)";
      } else if (node->GetBlocks().empty() && !node->IsImport()) {
        reason = "has 0 blocks (non-import)";
      } else if (!node->GetUnresolvedJumps().empty()) {
        reason = std::format("{} unresolved jumps", node->GetUnresolvedJumps().size());
      } else {
        reason = "unknown reason";
      }
      errors.emplace_back(base, std::format("{} (0x{:08X}): {}", node->GetName(), base, reason));
      continue;
    }

    node->Seal();
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

void FunctionGraph::RegisterChunk(u32 base, u32 size) {
  chunks_.emplace_back(base, size);
  REXLOG_TRACE(codegen, "FunctionGraph: registered chunk 0x{:08X}-0x{:08X}", base, base + size);
}

/**
 * Reports whether a branch may claim a target address. A forward target on a null dword, inside a
 * registered chunk, or inside a function that is not GAP_FILL is not vacant.
 *
 * @param from_addr Address of the branch.
 * @param target_addr Address it targets.
 * @return True when the target is vacant.
 */
bool FunctionGraph::IsVacant(u32 from_addr, u32 target_addr) const {
  if (memory_reader_) {
    if (target_addr > from_addr) {
      auto val = memory_reader_(target_addr);
      if (val && *val == 0x00000000) {
        REXLOG_TRACE(codegen, "FunctionGraph::isVacant: null dword at 0x{:08X} blocks vacancy",
                     target_addr);
        return false;
      }
    }
  }

  for (const auto& [chunkBase, chunkSize] : chunks_) {
    if (target_addr >= chunkBase && target_addr < chunkBase + chunkSize) {
      REXLOG_TRACE(codegen, "FunctionGraph::isVacant: chunk 0x{:08X}-0x{:08X} claims 0x{:08X}",
                   chunkBase, chunkBase + chunkSize, target_addr);
      return false;
    }
  }

  for (const auto& [base, node] : functions_) {
    if (node->ContainsAddress(target_addr)) {
      if (IsMergeableEntryPoint(target_addr)) {
        REXLOG_TRACE(codegen, "FunctionGraph::isVacant: 0x{:08X} is mergeable (GAP_FILL)",
                     target_addr);
        continue;
      }
      REXLOG_TRACE(codegen,
                   "FunctionGraph::isVacant: 0x{:08X} is within protected function 0x{:08X}",
                   target_addr, base);
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
size_t FunctionGraph::MarkFuncletRegisterSharing() {
  std::vector<std::pair<u32, u32>> ranges;

  for (auto& entry : functions_) {
    auto& node = entry.second;
    const auto& info = node->GetExceptionData();
    if (!info || !info->HasInfo())
      continue;

    auto claim = [&](u32 addr) {
      auto it = addr ? functions_.find(addr) : functions_.end();
      if (it == functions_.end())
        return;
      ranges.emplace_back(it->second->GetBase(), it->second->GetEnd());
    };

    if (const auto* seh = info->AsSEH()) {
      for (const auto& scope : seh->scopes) {
        claim(scope.handler);
        claim(scope.filter);
      }
    } else if (const auto* cxx = info->AsCxx()) {
      for (const auto& unwind : cxx->unwind_map) {
        claim(unwind.action);
      }
      for (const auto& try_block : cxx->try_blocks) {
        for (const auto& handler : try_block.handlers) {
          claim(handler.handler_address);
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

  auto inside_funclet = [&](u32 addr) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), addr,
                               [](u32 a, const std::pair<u32, u32>& r) { return a < r.first; });
    if (it == ranges.begin())
      return false;
    return addr < std::prev(it)->second;
  };

  size_t marked = 0;
  for (const auto& [base, node] : functions_) {
    if (inside_funclet(base) && !node->SharesRegisters()) {
      node->SetSharesRegisters(true);
      ++marked;
    }
  }

  return marked;
}

bool FunctionGraph::IsMergeableEntryPoint(u32 addr) const {
  auto it = functions_.find(addr);
  if (it == functions_.end()) {
    return false;
  }

  const FunctionNode* node = it->second.get();

  return node->GetAuthority() == FunctionAuthority::kGapFill;
}

/**
 * Classifies a branch target. An import is always a call or tail call. A bl to the caller's own
 * base is a recursive call and a b to it loops to the start. An entry point of another function is
 * a call or tail call even inside a thunk's range. A target inside the caller is an internal label,
 * which for a bl is a rare PIC pattern.
 *
 * @param target Target address.
 * @param caller_addr Address of the branch.
 * @param is_call_instruction True for bl.
 * @return The target kind.
 */
TargetKind FunctionGraph::ClassifyTarget(u32 target, u32 caller_addr,
                                         bool is_call_instruction) const {
  const FunctionNode* caller_fn = GetFunctionContaining(caller_addr);

  if (IsImport(target)) {
    return TargetKind::kImport;
  }

  if (caller_fn && target == caller_fn->GetBase()) {
    return is_call_instruction ? TargetKind::kFunction : TargetKind::kInternalLabel;
  }

  if (IsEntryPoint(target)) {
    return TargetKind::kFunction;
  }

  if (caller_fn && caller_fn->ContainsAddress(target)) {
    return TargetKind::kInternalLabel;
  }

  return TargetKind::kUnknown;
}

void FunctionGraph::NotifyFunctionAdded(FunctionNode* new_function) {
  for (auto& [base, node] : functions_) {
    if (node.get() != new_function && node->IsPending()) {
      node->TryResolveAgainst(new_function);
    }
  }
}

}  // namespace rex::codegen
