#include "mmio_analysis.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

#include <rex/memory/utils.h>
#include <rex/mmio.h>

namespace rex::codegen {

namespace {

enum class ValueKind : u8 { Unknown, Known, Offset };

struct Value {
  ValueKind kind = ValueKind::Unknown;
  u32 constant = 0;
  u32 shift = 0;

  bool operator==(const Value&) const = default;
};

using Registers = std::array<Value, 32>;

constexpr Value kUnknown{};
constexpr u32 kEieio = 0x7C0006AC;

constexpr Value KnownValue(u32 value) {
  return {ValueKind::Known, value, 0};
}

constexpr Value OffsetValue(u32 constant, u32 shift) {
  return {ValueKind::Offset, constant, shift};
}

constexpr Value Add(Value a, Value b) {
  if (a.kind == ValueKind::Known) {
    std::swap(a, b);
  }
  if (b.kind != ValueKind::Known) {
    return kUnknown;
  }
  switch (a.kind) {
    case ValueKind::Known:
      return KnownValue(a.constant + b.constant);
    case ValueKind::Offset:
      return OffsetValue(a.constant + b.constant, a.shift);
    case ValueKind::Unknown:
      return OffsetValue(b.constant, 0);
  }
  return kUnknown;
}

constexpr Value ShiftLeft(Value value, u32 shift) {
  switch (value.kind) {
    case ValueKind::Known:
      return KnownValue(value.constant << shift);
    case ValueKind::Offset:
      return value.shift + shift < 32 ? OffsetValue(value.constant << shift, value.shift + shift)
                                      : kUnknown;
    case ValueKind::Unknown:
      return kUnknown;
  }
  return kUnknown;
}

constexpr Value Or(Value source, Value other, bool same_register) {
  if (same_register) {
    return source;
  }
  if (source.kind == ValueKind::Known && other.kind == ValueKind::Known) {
    return KnownValue(source.constant | other.constant);
  }
  return kUnknown;
}

constexpr u32 PrimaryOpcode(u32 word) {
  return word >> 26;
}

constexpr u32 FieldD(u32 word) {
  return (word >> 21) & 31;
}

constexpr u32 FieldA(u32 word) {
  return (word >> 16) & 31;
}

constexpr u32 FieldB(u32 word) {
  return (word >> 11) & 31;
}

constexpr u32 ExtendedOpcode(u32 word) {
  return (word >> 1) & 0x3FF;
}

constexpr u32 Immediate(u32 word) {
  return static_cast<u32>(static_cast<i32>(static_cast<i16>(word & 0xFFFF)));
}

constexpr bool LinkBit(u32 word) {
  return (word & 1) != 0;
}

constexpr bool AbsoluteBit(u32 word) {
  return (word & 2) != 0;
}

constexpr bool BranchAlways(u32 word) {
  return (FieldD(word) & 0x14) == 0x14;
}

constexpr bool IsShiftLeftImmediate(u32 word) {
  const u32 mask_begin = (word >> 6) & 31;
  const u32 mask_end = (word >> 1) & 31;
  return mask_begin == 0 && mask_end == 31 - FieldB(word);
}

struct AccessForm {
  u32 code;
  bool classified;
  bool update;
  bool loads_gpr;
};

constexpr AccessForm kDForms[] = {
    {32, true, false, true},  {33, true, true, true},    {34, true, false, true},
    {35, true, true, true},   {36, true, false, false},  {37, true, true, false},
    {38, true, false, false}, {39, true, true, false},   {40, true, false, true},
    {41, true, true, true},   {42, true, false, true},   {43, true, true, true},
    {44, true, false, false}, {45, true, true, false},   {48, false, false, false},
    {49, false, true, false}, {50, false, false, false}, {51, false, true, false},
    {52, true, false, false}, {53, true, true, false},   {54, true, false, false},
    {55, true, true, false},
};

constexpr AccessForm kXForms[] = {
    {7, false, false, false},   {20, false, false, true},    {21, true, false, true},
    {23, true, false, true},    {39, false, false, false},   {53, true, true, true},
    {55, true, true, true},     {71, false, false, false},   {84, false, false, true},
    {87, true, false, true},    {103, false, false, false},  {119, true, true, true},
    {135, false, false, false}, {149, true, false, false},   {150, false, false, false},
    {151, true, false, false},  {167, false, false, false},  {181, true, true, false},
    {183, true, true, false},   {199, false, false, false},  {214, false, false, false},
    {215, true, false, false},  {231, false, false, false},  {247, true, true, false},
    {279, true, false, true},   {311, true, true, true},     {341, true, false, true},
    {343, true, false, true},   {359, false, false, false},  {373, true, true, true},
    {375, true, true, true},    {407, true, false, false},   {439, true, true, false},
    {487, false, false, false}, {519, false, false, false},  {534, true, false, true},
    {535, false, false, false}, {551, false, false, false},  {567, false, true, false},
    {599, false, false, false}, {631, false, true, false},   {647, false, false, false},
    {662, true, false, false},  {663, true, false, false},   {679, false, false, false},
    {695, true, true, false},   {727, true, false, false},   {759, true, true, false},
    {775, false, false, false}, {790, true, false, true},    {807, false, false, false},
    {903, false, false, false}, {918, true, false, false},   {935, false, false, false},
    {983, true, false, false},  {1014, false, false, false},
};

static_assert(std::ranges::is_sorted(kDForms, {}, &AccessForm::code));
static_assert(std::ranges::is_sorted(kXForms, {}, &AccessForm::code));

constexpr u32 kXOFormWritesD[] = {8,   9,   10,  11,  40,  73,  75,  104, 136, 138, 200,
                                  202, 232, 233, 234, 235, 266, 457, 459, 489, 491};
constexpr u32 kXFormWritesD[] = {19, 83, 339, 371, 595, 659};
constexpr u32 kXFormWritesA[] = {24,  26,  27,  28,  58,  60,  124, 284, 316, 412, 444,
                                 476, 536, 539, 792, 794, 824, 826, 827, 922, 954, 986};
constexpr u32 kXFormWritesNone[] = {0,   4,   6,   32,  38,  54,  68,  86,  144,
                                    146, 178, 246, 278, 467, 470, 598, 854, 982};
constexpr u32 kXFormWritesAll[] = {533, 597};

const AccessForm* FindForm(std::span<const AccessForm> forms, u32 code) {
  const auto it = std::ranges::lower_bound(forms, code, {}, &AccessForm::code);
  return it != forms.end() && it->code == code ? &*it : nullptr;
}

bool Contains(std::span<const u32> list, u32 value) {
  return std::ranges::find(list, value) != list.end();
}

struct Access {
  bool classified = false;
  bool writes = false;
  bool indexed = false;
  bool update = false;
  bool loads_gpr = false;
  bool loads_multiple = false;
  u32 displacement = 0;
};

std::optional<Access> DecodeAccess(u32 word) {
  const u32 opcode = PrimaryOpcode(word);
  if (opcode == 31) {
    const AccessForm* form = FindForm(kXForms, ExtendedOpcode(word));
    if (!form) {
      return std::nullopt;
    }
    return Access{.classified = form->classified,
                  .writes = (ExtendedOpcode(word) & 0x80) != 0,
                  .indexed = true,
                  .update = form->update,
                  .loads_gpr = form->loads_gpr};
  }
  if (opcode == 46 || opcode == 47) {
    return Access{
        .writes = opcode == 47, .loads_multiple = opcode == 46, .displacement = Immediate(word)};
  }
  if (opcode == 58 || opcode == 62) {
    const u32 form = word & 3;
    if (form > (opcode == 58 ? 2u : 1u)) {
      return std::nullopt;
    }
    return Access{.classified = true,
                  .writes = opcode == 62,
                  .update = form == 1,
                  .loads_gpr = opcode == 58,
                  .displacement = Immediate(word & ~3u)};
  }
  const AccessForm* form = FindForm(kDForms, opcode);
  if (!form) {
    return std::nullopt;
  }
  return Access{.classified = form->classified,
                .writes = (opcode & 4) != 0,
                .update = form->update,
                .loads_gpr = form->loads_gpr,
                .displacement = Immediate(word)};
}

Value EffectiveAddress(const Access& access, u32 word, const Registers& registers) {
  const u32 base_register = FieldA(word);
  const Value base = base_register == 0 ? KnownValue(0) : registers[base_register];
  return Add(base, access.indexed ? registers[FieldB(word)] : KnownValue(access.displacement));
}

void ApplyAccess(const Access& access, u32 word, Value address, Registers& registers) {
  if (access.loads_gpr) {
    registers[FieldD(word)] = kUnknown;
  }
  if (access.loads_multiple) {
    std::fill(registers.begin() + FieldD(word), registers.end(), kUnknown);
  }
  if (access.update) {
    registers[FieldA(word)] = address;
  }
}

void ClobberCall(Registers& registers) {
  registers[0] = kUnknown;
  std::fill(registers.begin() + 3, registers.begin() + 13, kUnknown);
}

void TransferExtended(u32 word, Registers& registers) {
  const u32 extended = ExtendedOpcode(word);
  const u32 rd = FieldD(word);
  const u32 ra = FieldA(word);
  const u32 rb = FieldB(word);
  if (extended == 444) {
    registers[ra] = Or(registers[rd], registers[rb], rd == rb);
    return;
  }
  if ((extended & 0x1FF) == 266) {
    registers[rd] = Add(registers[ra], registers[rb]);
    return;
  }
  if (Contains(kXFormWritesNone, extended)) {
    return;
  }
  if (Contains(kXFormWritesA, extended)) {
    registers[ra] = kUnknown;
    return;
  }
  if (Contains(kXFormWritesD, extended) || Contains(kXOFormWritesD, extended & 0x1FF)) {
    registers[rd] = kUnknown;
    return;
  }
  if (Contains(kXFormWritesAll, extended)) {
    registers.fill(kUnknown);
    return;
  }
  registers[rd] = kUnknown;
  registers[ra] = kUnknown;
}

void Transfer(u32 word, Registers& registers) {
  const u32 opcode = PrimaryOpcode(word);
  const u32 rd = FieldD(word);
  const u32 ra = FieldA(word);
  switch (opcode) {
    case 14:
    case 15: {
      const u32 immediate = opcode == 14 ? Immediate(word) : Immediate(word) << 16;
      registers[rd] = ra == 0 ? KnownValue(immediate) : Add(registers[ra], KnownValue(immediate));
      return;
    }
    case 24:
    case 25: {
      const u32 bits = opcode == 24 ? (word & 0xFFFF) : (word & 0xFFFF) << 16;
      registers[ra] = registers[rd].kind == ValueKind::Known
                          ? KnownValue(registers[rd].constant | bits)
                          : kUnknown;
      return;
    }
    case 21:
      registers[ra] =
          IsShiftLeftImmediate(word) ? ShiftLeft(registers[rd], FieldB(word)) : kUnknown;
      return;
    case 31:
      TransferExtended(word, registers);
      return;
    case 16:
    case 18:
      if (LinkBit(word)) {
        ClobberCall(registers);
      }
      return;
    case 19: {
      const u32 extended = ExtendedOpcode(word);
      if ((extended == 16 || extended == 528) && LinkBit(word)) {
        ClobberCall(registers);
      }
      return;
    }
    case 17:
      ClobberCall(registers);
      return;
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 10:
    case 11:
    case 59:
    case 63:
      return;
    case 7:
    case 8:
    case 12:
    case 13:
      registers[rd] = kUnknown;
      return;
    case 20:
    case 23:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
      registers[ra] = kUnknown;
      return;
    default:
      registers[rd] = kUnknown;
      registers[ra] = kUnknown;
      return;
  }
}

struct Flow {
  bool ends = false;
  bool falls_through = true;
  bool through_ctr = false;
  std::optional<u32> target = std::nullopt;
};

Flow DecodeFlow(u32 address, u32 word) {
  switch (PrimaryOpcode(word)) {
    case 18: {
      if (LinkBit(word)) {
        return {.ends = true};
      }
      const u32 offset = static_cast<u32>(static_cast<i32>(word << 6) >> 6) & ~3u;
      return {.ends = true,
              .falls_through = false,
              .target = (AbsoluteBit(word) ? 0 : address) + offset};
    }
    case 16:
      if (LinkBit(word)) {
        return {.ends = true};
      }
      return {.ends = true, .target = (AbsoluteBit(word) ? 0 : address) + Immediate(word & ~3u)};
    case 19: {
      const u32 extended = ExtendedOpcode(word);
      if (extended != 16 && extended != 528) {
        return {};
      }
      if (LinkBit(word)) {
        return {.ends = true};
      }
      return {.ends = true, .falls_through = !BranchAlways(word), .through_ctr = extended == 528};
    }
    case 17:
      return {.ends = true};
    default:
      return {};
  }
}

struct Instruction {
  u32 address;
  u32 word;
  std::optional<Access> access;
};

struct Code {
  std::vector<Instruction> instructions;

  std::optional<size_t> IndexOf(u32 address) const {
    const auto it = std::ranges::lower_bound(instructions, address, {}, &Instruction::address);
    if (it == instructions.end() || it->address != address) {
      return std::nullopt;
    }
    return static_cast<size_t>(it - instructions.begin());
  }
};

struct BasicBlock {
  size_t first = 0;
  size_t last = 0;
  bool starts_unknown = false;
  std::vector<size_t> successors;
};

Code CollectCode(std::span<const MMIOCodeBlock> blocks) {
  Code code;
  for (const MMIOCodeBlock& block : blocks) {
    for (size_t offset = 0; offset + 4 <= block.bytes.size(); offset += 4) {
      const u32 word = rex::memory::load_and_swap<u32>(block.bytes.data() + offset);
      code.instructions.push_back(
          {block.base + static_cast<u32>(offset), word, DecodeAccess(word)});
    }
  }
  std::ranges::sort(code.instructions, {}, &Instruction::address);
  const auto duplicates = std::ranges::unique(code.instructions, {}, &Instruction::address);
  code.instructions.erase(duplicates.begin(), duplicates.end());
  return code;
}

std::vector<bool> FindLeaders(const Code& code, const MMIOFunctionInput& input) {
  std::vector<bool> leaders(code.instructions.size(), false);
  leaders[0] = true;
  const auto mark = [&](u32 address) {
    if (const auto index = code.IndexOf(address)) {
      leaders[*index] = true;
    }
  };
  for (const MMIOCodeBlock& block : input.blocks) {
    mark(block.base);
  }
  for (const JumpTable& table : input.jump_tables) {
    std::ranges::for_each(table.targets, mark);
  }
  std::ranges::for_each(input.hook_targets, mark);
  for (size_t index = 0; index < code.instructions.size(); ++index) {
    const Instruction& instruction = code.instructions[index];
    if (index > 0 && code.instructions[index - 1].address + 4 != instruction.address) {
      leaders[index] = true;
    }
    const Flow flow = DecodeFlow(instruction.address, instruction.word);
    if (!flow.ends) {
      continue;
    }
    mark(instruction.address + 4);
    if (flow.target) {
      mark(*flow.target);
    }
  }
  return leaders;
}

std::vector<BasicBlock> BuildBlocks(const Code& code, const MMIOFunctionInput& input) {
  const std::vector<bool> leaders = FindLeaders(code, input);
  std::vector<BasicBlock> blocks;
  std::vector<std::optional<size_t>> block_at(code.instructions.size());
  for (size_t index = 0; index < code.instructions.size(); ++index) {
    if (leaders[index]) {
      block_at[index] = blocks.size();
      BasicBlock& block = blocks.emplace_back();
      block.first = index;
      block.last = index;
    } else {
      blocks.back().last = index;
    }
  }

  for (BasicBlock& block : blocks) {
    const Instruction& last = code.instructions[block.last];
    const Flow flow = DecodeFlow(last.address, last.word);
    const auto link = [&](u32 address) {
      if (const auto index = code.IndexOf(address); index && block_at[*index]) {
        block.successors.push_back(*block_at[*index]);
      }
    };
    if (flow.falls_through) {
      link(last.address + 4);
    }
    if (flow.target) {
      link(*flow.target);
    }
    if (flow.through_ctr) {
      for (const JumpTable& table : input.jump_tables) {
        if (table.bctrAddress == last.address) {
          std::ranges::for_each(table.targets, link);
        }
      }
    }
  }

  std::vector<size_t> predecessors(blocks.size(), 0);
  for (const BasicBlock& block : blocks) {
    for (size_t successor : block.successors) {
      ++predecessors[successor];
    }
  }
  const u32 entry = input.blocks.front().base;
  for (size_t index = 0; index < blocks.size(); ++index) {
    const u32 start = code.instructions[blocks[index].first].address;
    blocks[index].starts_unknown =
        predecessors[index] == 0 || start == entry || Contains(input.hook_targets, start);
  }
  return blocks;
}

template <typename Visit>
Registers RunBlock(const BasicBlock& block, Registers registers, const Code& code,
                   std::span<const u32> hook_sites, Visit&& visit) {
  for (size_t index = block.first; index <= block.last; ++index) {
    const Instruction& instruction = code.instructions[index];
    const bool hooked = Contains(hook_sites, instruction.address);
    if (hooked) {
      registers.fill(kUnknown);
    }
    if (instruction.access) {
      const Value address = EffectiveAddress(*instruction.access, instruction.word, registers);
      visit(index, *instruction.access, address);
      ApplyAccess(*instruction.access, instruction.word, address, registers);
    } else {
      Transfer(instruction.word, registers);
    }
    if (hooked) {
      registers.fill(kUnknown);
    }
  }
  return registers;
}

bool Meet(Registers& into, const Registers& from) {
  bool changed = false;
  for (size_t index = 0; index < into.size(); ++index) {
    if (into[index] != from[index] && into[index] != kUnknown) {
      into[index] = kUnknown;
      changed = true;
    }
  }
  return changed;
}

std::vector<Registers> Solve(const std::vector<BasicBlock>& blocks, const Code& code,
                             std::span<const u32> hook_sites) {
  std::vector<Registers> entries(blocks.size());
  std::vector<bool> reached(blocks.size());
  std::vector<size_t> pending;
  for (size_t index = 0; index < blocks.size(); ++index) {
    if (blocks[index].starts_unknown) {
      reached[index] = true;
      pending.push_back(index);
    }
  }
  const auto ignore = [](size_t, const Access&, Value) {};
  while (!pending.empty()) {
    const size_t index = pending.back();
    pending.pop_back();
    const Registers exit = RunBlock(blocks[index], entries[index], code, hook_sites, ignore);
    for (size_t successor : blocks[index].successors) {
      if (blocks[successor].starts_unknown) {
        continue;
      }
      if (!reached[successor]) {
        reached[successor] = true;
        entries[successor] = exit;
        pending.push_back(successor);
      } else if (Meet(entries[successor], exit)) {
        pending.push_back(successor);
      }
    }
  }
  return entries;
}

constexpr MMIOAccess AccessOf(MMIOBlock block) {
  return block == MMIOBlock::GPU ? MMIOAccess::GPU : MMIOAccess::APU;
}

std::optional<MMIOSite> Classify(u32 address, const Access& access, Value target, bool barrier,
                                 const MMIODeclaredAccess* declared) {
  const bool known = target.kind == ValueKind::Known;
  const bool write = access.writes;
  if (known) {
    if (const auto block = MMIOBlockOf(target.constant)) {
      if (!access.classified) {
        return MMIOSite{address, MMIOAccess::Plain, MMIOReason::Unsupported, target.constant,
                        write};
      }
      return MMIOSite{address, AccessOf(*block), MMIOReason::Proven, target.constant, write};
    }
  }
  if (!access.classified) {
    return std::nullopt;
  }
  if (declared) {
    return MMIOSite{address, AccessOf(declared->block), MMIOReason::Declared,
                    known ? target.constant : 0, write};
  }
  if (known && (target.constant >> 24) == 0x7F) {
    return MMIOSite{address, MMIOAccess::Plain, MMIOReason::Unserved, target.constant, write};
  }
  if (target.kind == ValueKind::Offset && IsMMIOBlockAddress(target.constant)) {
    return MMIOSite{address, MMIOAccess::Guarded, MMIOReason::Offset, 0, write};
  }
  if (!known && barrier) {
    return MMIOSite{address, MMIOAccess::Guarded, MMIOReason::Barrier, 0, write};
  }
  return std::nullopt;
}

}  // namespace

MMIOAccess MMIOClassification::At(u32 address) const {
  const auto it = std::ranges::lower_bound(sites, address, {}, &MMIOSite::address);
  return it != sites.end() && it->address == address ? it->access : MMIOAccess::Plain;
}

MMIOClassification ClassifyMMIO(const MMIOFunctionInput& input) {
  MMIOClassification result;
  if (input.blocks.empty()) {
    return result;
  }
  const Code code = CollectCode(input.blocks);
  if (code.instructions.empty()) {
    return result;
  }
  const std::vector<BasicBlock> blocks = BuildBlocks(code, input);
  const std::vector<Registers> entries = Solve(blocks, code, input.hook_sites);

  const auto barrier_after = [&](size_t index) {
    return index + 1 < code.instructions.size() &&
           code.instructions[index + 1].address == code.instructions[index].address + 4 &&
           code.instructions[index + 1].word == kEieio;
  };
  const auto visit = [&](size_t index, const Access& access, Value target) {
    const u32 address = code.instructions[index].address;
    const MMIODeclaredAccess* declared = nullptr;
    if (input.declared) {
      if (const auto it = input.declared->find(address); it != input.declared->end()) {
        declared = &it->second;
      }
    }
    if (auto site = Classify(address, access, target, barrier_after(index), declared)) {
      result.sites.push_back(*site);
    }
  };
  for (size_t index = 0; index < blocks.size(); ++index) {
    RunBlock(blocks[index], entries[index], code, input.hook_sites, visit);
  }
  std::ranges::sort(result.sites, {}, &MMIOSite::address);
  return result;
}

}  // namespace rex::codegen
