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

/** r14 to r31, and f14 to f31. */
constexpr u32 kNonVolatileMask = 0xFFFFC000u;

/** v14 to v31 and v64 to v127. */
std::bitset<128> NonVolatileVectorMask() {
  std::bitset<128> mask;
  for (u32 n = 14; n < 32; ++n) {
    mask.set(n);
  }
  for (u32 n = 64; n < 128; ++n) {
    mask.set(n);
  }
  return mask;
}

struct Live {
  u32 gpr = 0;
  u32 fpr = 0;

  bool operator==(const Live&) const = default;
};

Live operator|(const Live& a, const Live& b) {
  return {a.gpr | b.gpr, a.fpr | b.fpr};
}

Live operator&(const Live& a, const Live& b) {
  return {a.gpr & b.gpr, a.fpr & b.fpr};
}

Live operator~(const Live& a) {
  return {~a.gpr, ~a.fpr};
}

struct Node {
  const Instruction* instruction = nullptr;
  RegisterEffects effects;
  /** The function a direct call or tail branch out of the function reaches, if any. */
  std::optional<u32> callee;
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

std::vector<Node> BuildNodes(const FunctionNode& fn, const std::unordered_set<u32>& millicode) {
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

  for (size_t i = 0; i < nodes.size(); ++i) {
    const Instruction& instruction = *nodes[i].instruction;
    Node& node = nodes[i];
    const bool internal_call = instruction.IsCall() && !IsCallOutOfFunction(fn, instruction);
    const bool millicode_call = instruction.IsCall() && instruction.branch_target.has_value() &&
                                millicode.contains(*instruction.branch_target);
    if (!internal_call && !millicode_call) {
      node.effects = detail::GetRegisterEffects(instruction);
    }
    node.callee = GetDirectTarget(fn, instruction, millicode);

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

/**
 * Runs a backward liveness analysis over the nodes to a fixpoint and returns the registers live at
 * the entry of the function. uses[i] and defs[i] are the registers node i reads and writes.
 */
template <typename T>
T LiveAtEntry(const FunctionNode& fn, const std::vector<Node>& nodes, const std::vector<T>& uses,
              const std::vector<T>& defs) {
  std::vector<T> live_in(nodes.size());
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = nodes.size(); i-- > 0;) {
      T out{};
      for (const size_t successor : nodes[i].successors) {
        if (successor != kExit) {
          out = out | live_in[successor];
        }
      }
      const T in = uses[i] | (out & ~defs[i]);
      if (!(in == live_in[i])) {
        live_in[i] = in;
        changed = true;
      }
    }
  }
  const auto entry = std::ranges::find(nodes, fn.GetBase(),
                                       [](const Node& node) { return node.instruction->address; });
  return entry == nodes.end() ? T{} : live_in[static_cast<size_t>(entry - nodes.begin())];
}

/**
 * Computes a value for every function, again for its callers whenever it changes, until no value
 * changes. compute(fn, values) gives the value of fn from the values known so far. Values equal to
 * T{} are left out of the result.
 */
template <typename T, typename Compute>
std::unordered_map<u32, T> ComputeOverCalls(std::span<const FunctionNode* const> functions,
                                            const std::unordered_set<u32>& millicode,
                                            Compute compute) {
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

  std::unordered_map<u32, T> values;
  std::deque<size_t> pending;
  std::vector<bool> queued(functions.size(), true);
  for (size_t i = 0; i < functions.size(); ++i) {
    pending.push_back(i);
  }
  while (!pending.empty()) {
    const size_t i = pending.front();
    pending.pop_front();
    queued[i] = false;
    const T value = compute(*functions[i], values);
    const auto known = values.find(functions[i]->GetBase());
    const T before = known == values.end() ? T{} : known->second;
    if (value == before) {
      continue;
    }
    values[functions[i]->GetBase()] = value;
    for (const size_t caller : callers[i]) {
      if (!queued[caller]) {
        queued[caller] = true;
        pending.push_back(caller);
      }
    }
  }
  std::erase_if(values, [](const auto& entry) { return entry.second == T{}; });
  return values;
}

}  // namespace

CallUsage ComputeCallUsage(const FunctionNode& fn, const std::unordered_set<u32>& millicode,
                           const std::unordered_map<u32, CallUsage>& callees) {
  if (fn.GetInstructions().empty()) {
    return {};
  }
  const std::vector<Node> nodes = BuildNodes(fn, millicode);

  u32 frame_address = ~0u;
  for (const Node& node : nodes) {
    if (detail::IsFrameAllocation(*node.instruction)) {
      frame_address = node.instruction->address;
      break;
    }
  }

  std::vector<Live> uses(nodes.size());
  std::vector<Live> defs(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) {
    const Node& node = nodes[i];
    if (node.instruction->address < frame_address &&
        detail::IsCallerFrameSpill(*node.instruction)) {
      continue;
    }
    uses[i] = {node.effects.gpr_use, node.effects.fpr_use};
    defs[i] = {node.effects.gpr_def, node.effects.fpr_def};
    if (node.callee) {
      if (const auto callee = callees.find(*node.callee); callee != callees.end()) {
        uses[i].gpr |= static_cast<u32>(callee->second.gpr_mask) << kFirstArgumentGPR;
        uses[i].fpr |= static_cast<u32>(callee->second.fpr_mask) << kFirstArgumentFPR;
      }
    }
  }

  const Live at_entry = LiveAtEntry(fn, nodes, uses, defs);
  CallUsage usage;
  usage.gpr_mask =
      static_cast<u8>((at_entry.gpr >> kFirstArgumentGPR) & ((1u << kGPRArgumentCount) - 1));
  usage.fpr_mask =
      static_cast<u16>((at_entry.fpr >> kFirstArgumentFPR) & ((1u << kFPRArgumentCount) - 1));
  return usage;
}

std::unordered_map<u32, CallUsage> ComputeCallUsages(std::span<const FunctionNode* const> functions,
                                                     const std::unordered_set<u32>& millicode) {
  return ComputeOverCalls<CallUsage>(
      functions, millicode,
      [&](const FunctionNode& fn, const std::unordered_map<u32, CallUsage>& callees) {
        return ComputeCallUsage(fn, millicode, callees);
      });
}

NonVolatileLiveIns ComputeNonVolatileLiveIn(
    const FunctionNode& fn, const std::unordered_set<u32>& opaque,
    const std::unordered_map<u32, NonVolatileLiveIns>& callees) {
  if (fn.GetInstructions().empty()) {
    return {};
  }
  const std::vector<Node> nodes = BuildNodes(fn, opaque);
  const NonVolatileLiveIns mask{kNonVolatileMask, kNonVolatileMask, NonVolatileVectorMask()};

  std::vector<NonVolatileLiveIns> uses(nodes.size());
  std::vector<NonVolatileLiveIns> defs(nodes.size());
  NonVolatileLiveIns written;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const RegisterEffects& effects = nodes[i].effects;
    defs[i] = {effects.gpr_def, effects.fpr_def, effects.vr_def};
    if (!effects.frame_load) {
      written = written | defs[i];
    }
    // A store through r1 saves a register in the frame, as a prologue does before the function
    // uses the register, and does not read a value the function takes from its caller.
    if (!effects.frame_store) {
      uses[i] = NonVolatileLiveIns{effects.gpr_use, effects.fpr_use, effects.vr_use} & mask;
    }
    if (nodes[i].callee) {
      if (const auto callee = callees.find(*nodes[i].callee); callee != callees.end()) {
        uses[i] = uses[i] | callee->second;
      }
    }
  }
  // A register the function writes itself, other than reloading it from its frame as an epilogue
  // does, is one it sets: a path that reads it first is one the code does not take (a value that
  // may be unset, which compilers allow when its use is guarded the same way), not a value the
  // caller passes.
  return LiveAtEntry(fn, nodes, uses, defs) & mask & ~written;
}

std::unordered_map<u32, NonVolatileLiveIns> ComputeNonVolatileLiveIns(
    std::span<const FunctionNode* const> functions, const std::unordered_set<u32>& opaque) {
  return ComputeOverCalls<NonVolatileLiveIns>(
      functions, opaque,
      [&](const FunctionNode& fn, const std::unordered_map<u32, NonVolatileLiveIns>& callees) {
        return ComputeNonVolatileLiveIn(fn, opaque, callees);
      });
}

}  // namespace rex::codegen
