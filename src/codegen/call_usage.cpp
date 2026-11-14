#include "call_usage.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ppc/instruction.h"

namespace rex::codegen {

namespace {

using rex::codegen::detail::Instruction;
using rex::codegen::detail::Opcode;
using rex::codegen::detail::RegisterEffects;

constexpr u32 kFirstArgumentGPR = 3;
constexpr u32 kGPRArgumentCount = 8;
constexpr u32 kFirstArgumentFPR = 1;
constexpr u32 kFPRArgumentCount = 13;
constexpr size_t kExit = ~static_cast<size_t>(0);

struct Live {
  u32 gpr = 0;
  u32 fpr = 0;

  bool operator==(const Live&) const = default;
};

struct Node {
  const Instruction* instruction = nullptr;
  RegisterEffects effects;
  std::vector<size_t> successors;
};

bool IsCallOutOfFunction(const FunctionNode& fn, const Instruction& instruction) {
  if (!instruction.IsCall()) {
    return false;
  }
  return !instruction.branch_target.has_value() || !fn.IsWithinBounds(*instruction.branch_target);
}

std::optional<u32> GetDirectTarget(const FunctionNode& fn, const Instruction& instruction,
                                   const std::unordered_set<u32>& millicode) {
  switch (instruction.opcode) {
    case Opcode::b:
    case Opcode::ba:
    case Opcode::bl:
    case Opcode::bla:
    case Opcode::bc:
    case Opcode::bca:
    case Opcode::bcl:
    case Opcode::bcla:
      break;
    default:
      return std::nullopt;
  }
  if (!instruction.branch_target || fn.IsWithinBounds(*instruction.branch_target) ||
      millicode.contains(*instruction.branch_target)) {
    return std::nullopt;
  }
  return instruction.branch_target;
}

std::vector<Node> BuildNodes(const FunctionNode& fn, const std::unordered_set<u32>& millicode,
                             const std::unordered_map<u32, CallUsage>& callees) {
  std::vector<const Instruction*> sorted(fn.GetInstructions().begin(), fn.GetInstructions().end());
  std::ranges::sort(sorted, {}, &Instruction::address);

  std::vector<Node> nodes(sorted.size());
  std::unordered_map<u32, size_t> index_of;
  for (size_t i = 0; i < sorted.size(); ++i) {
    nodes[i].instruction = sorted[i];
    index_of.emplace(sorted[i]->address, i);
  }
  const auto find = [&](u32 address) {
    const auto it = index_of.find(address);
    return it == index_of.end() ? kExit : it->second;
  };

  u32 frame_address = ~0u;
  for (const Node& node : nodes) {
    if (detail::IsFrameAllocation(*node.instruction)) {
      frame_address = node.instruction->address;
      break;
    }
  }

  for (size_t i = 0; i < nodes.size(); ++i) {
    const Instruction& instruction = *nodes[i].instruction;
    Node& node = nodes[i];
    const bool internal_call = instruction.IsCall() && !IsCallOutOfFunction(fn, instruction);
    const bool millicode_call = instruction.IsCall() && instruction.branch_target.has_value() &&
                                millicode.contains(*instruction.branch_target);
    if (!internal_call && !millicode_call) {
      node.effects = detail::GetRegisterEffects(instruction);
    }
    if (instruction.address < frame_address && detail::IsCallerFrameSpill(instruction)) {
      node.effects = {};
    }
    if (const auto target = GetDirectTarget(fn, instruction, millicode)) {
      if (const auto callee = callees.find(*target); callee != callees.end()) {
        node.effects.gpr_use |= static_cast<u32>(callee->second.gpr_mask) << kFirstArgumentGPR;
        node.effects.fpr_use |= static_cast<u32>(callee->second.fpr_mask) << kFirstArgumentFPR;
      }
    }

    const size_t next = find(instruction.address + 4);
    switch (instruction.opcode) {
      case Opcode::b:
      case Opcode::ba:
        node.successors.push_back(instruction.branch_target ? find(*instruction.branch_target)
                                                            : kExit);
        break;
      case Opcode::bc:
      case Opcode::bca:
        node.successors.push_back(instruction.branch_target ? find(*instruction.branch_target)
                                                            : kExit);
        if (!instruction.IsBranchAlways()) {
          node.successors.push_back(next);
        }
        break;
      case Opcode::bclr:
        node.successors.push_back(kExit);
        if (instruction.IsConditional()) {
          node.successors.push_back(next);
        }
        break;
      case Opcode::bcctr: {
        const auto& tables = fn.GetJumpTables();
        const auto table = std::ranges::find(tables, instruction.address, &JumpTable::bctr_address);
        if (table != tables.end() && !table->targets.empty()) {
          for (const u32 target : table->targets) {
            node.successors.push_back(find(target));
          }
        } else {
          node.successors.push_back(kExit);
        }
        if (instruction.IsConditional()) {
          node.successors.push_back(next);
        }
        break;
      }
      default:
        node.successors.push_back(next);
        if (internal_call && instruction.branch_target) {
          node.successors.push_back(find(*instruction.branch_target));
        }
        break;
    }
  }
  return nodes;
}

}  // namespace

CallUsage ComputeCallUsage(const FunctionNode& fn, const std::unordered_set<u32>& millicode,
                           const std::unordered_map<u32, CallUsage>& callees) {
  if (fn.GetInstructions().empty()) {
    return {};
  }
  const std::vector<Node> nodes = BuildNodes(fn, millicode, callees);
  std::vector<Live> live_in(nodes.size());

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = nodes.size(); i-- > 0;) {
      Live out;
      for (const size_t successor : nodes[i].successors) {
        if (successor != kExit) {
          out.gpr |= live_in[successor].gpr;
          out.fpr |= live_in[successor].fpr;
        }
      }
      const RegisterEffects& effects = nodes[i].effects;
      const Live in{effects.gpr_use | (out.gpr & ~effects.gpr_def),
                    effects.fpr_use | (out.fpr & ~effects.fpr_def)};
      if (in != live_in[i]) {
        live_in[i] = in;
        changed = true;
      }
    }
  }

  const auto entry = std::ranges::find(nodes, fn.GetBase(),
                                       [](const Node& node) { return node.instruction->address; });
  if (entry == nodes.end()) {
    return {};
  }
  const Live& at_entry = live_in[static_cast<size_t>(entry - nodes.begin())];
  CallUsage usage;
  usage.gpr_mask =
      static_cast<u8>((at_entry.gpr >> kFirstArgumentGPR) & ((1u << kGPRArgumentCount) - 1));
  usage.fpr_mask =
      static_cast<u16>((at_entry.fpr >> kFirstArgumentFPR) & ((1u << kFPRArgumentCount) - 1));
  return usage;
}

std::unordered_map<u32, CallUsage> ComputeCallUsages(std::span<const FunctionNode* const> functions,
                                                     const std::unordered_set<u32>& millicode) {
  std::unordered_map<u32, size_t> index_of;
  for (size_t i = 0; i < functions.size(); ++i) {
    index_of.emplace(functions[i]->GetBase(), i);
  }
  std::vector<std::vector<size_t>> callers(functions.size());
  for (size_t i = 0; i < functions.size(); ++i) {
    std::unordered_set<size_t> seen;
    for (const Instruction* instruction : functions[i]->GetInstructions()) {
      const auto target = GetDirectTarget(*functions[i], *instruction, millicode);
      if (!target) {
        continue;
      }
      const auto callee = index_of.find(*target);
      if (callee != index_of.end() && seen.insert(callee->second).second) {
        callers[callee->second].push_back(i);
      }
    }
  }

  std::unordered_map<u32, CallUsage> usages;
  std::deque<size_t> pending;
  std::vector<bool> queued(functions.size(), true);
  for (size_t i = 0; i < functions.size(); ++i) {
    pending.push_back(i);
  }
  while (!pending.empty()) {
    const size_t i = pending.front();
    pending.pop_front();
    queued[i] = false;
    const CallUsage usage = ComputeCallUsage(*functions[i], millicode, usages);
    const auto known = usages.find(functions[i]->GetBase());
    const CallUsage before = known == usages.end() ? CallUsage{} : known->second;
    if (usage.gpr_mask == before.gpr_mask && usage.fpr_mask == before.fpr_mask) {
      continue;
    }
    usages[functions[i]->GetBase()] = usage;
    for (const size_t caller : callers[i]) {
      if (!queued[caller]) {
        queued[caller] = true;
        pending.push_back(caller);
      }
    }
  }
  std::erase_if(usages, [](const auto& entry) {
    return entry.second.gpr_mask == 0 && entry.second.fpr_mask == 0;
  });
  return usages;
}

}  // namespace rex::codegen
