/**
 * @file        codegen/phase_gapfill.cpp
 * @brief       GapFill phase that finds uncovered code regions and registers them as functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc/disasm.h"
#include "ppc/instruction.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <vector>
#include <iterator>
#include <map>
#include <memory>
#include <unordered_set>

#include <rex/codegen/phases.h>
#include "known_functions.h"

#include <rex/logging/log.h>

#include <rex/memory/byte_swap.h>

#include <ppc.h>

using rex::codegen::detail::DecodeInstruction;
using rex::codegen::detail::Disassemble;
using rex::codegen::detail::Opcode;

namespace rex::codegen {

namespace {

/**
 * Reports whether an address holds exception handler data, a pointer to a known entry point such
 * as __C_specific_handler at +0 followed by a pointer to the scope table in .rdata at +4.
 *
 * @param binary Binary view.
 * @param graph Function graph holding the known entry points.
 * @param addr Address to test.
 * @return True when the data matches.
 */
bool IsExceptionData(const BinaryView& binary, const FunctionGraph& graph, u32 addr) {
  const u8* data = binary.Translate(addr);
  if (!data)
    return false;

  u32 first_dword = memory::load<be_u32>(data);
  u32 second_dword = memory::load<be_u32>(data + 4);

  if (!graph.IsEntryPoint(first_dword)) {
    return false;
  }

  auto* rdata_section = binary.FindSectionByName(".rdata");
  if (!rdata_section)
    return false;

  u32 rdata_start = rdata_section->base_address;
  u32 rdata_end = rdata_start + rdata_section->size;

  if (second_dword >= rdata_start && second_dword < rdata_end) {
    REXLOG_TRACE(
        codegen,
        "GapFill: 0x{:08X} looks like exception data (handler=0x{:08X}, scope=0x{:08X}), skipping",
        addr, first_dword, second_dword);
    return true;
  }

  return false;
}

/**
 * Disjoint address ranges covered by functions.
 */
class Coverage {
 public:
  /**
   * Adds a range.
   *
   * @param start First address of the range.
   * @param end Address 1 past the range.
   */
  void Add(u32 start, u32 end) {
    if (end <= start)
      return;
    auto it = ranges_->upper_bound(start);
    if (it != ranges_->begin()) {
      auto prev = std::prev(it);
      if (prev->second >= start) {
        start = prev->first;
        end = std::max(end, prev->second);
        it = ranges_->erase(prev);
      }
    }
    while (it != ranges_->end() && it->first <= end) {
      end = std::max(end, it->second);
      it = ranges_->erase(it);
    }
    (*ranges_)[start] = end;
  }

  /**
   * Finds the end of the range that holds an address.
   *
   * @param addr Address to look up.
   * @return The address 1 past the range, or 0 when no range holds the address.
   */
  u32 GetCoveredEnd(u32 addr) const {
    auto it = ranges_->upper_bound(addr);
    if (it == ranges_->begin())
      return 0;
    --it;
    return addr < it->second ? it->second : 0;
  }

 private:
  std::unique_ptr<std::map<u32, u32>> ranges_ = std::make_unique<std::map<u32, u32>>();
};

/**
 * Lists the general registers an instruction names, in operand order.
 *
 * @param operands The operand text of the disassembled instruction.
 * @return The register numbers.
 */
std::vector<u32> GetRegisterOperands(std::string_view operands) {
  std::vector<u32> regs;
  for (size_t i = 0; i + 1 < operands.size(); ++i) {
    const bool starts_token = i == 0 || !std::isalnum(static_cast<unsigned char>(operands[i - 1]));
    if (operands[i] != 'r' || !starts_token ||
        !std::isdigit(static_cast<unsigned char>(operands[i + 1])))
      continue;
    u32 reg = 0;
    for (++i; i < operands.size() && std::isdigit(static_cast<unsigned char>(operands[i])); ++i)
      reg = reg * 10 + static_cast<u32>(operands[i] - '0');
    regs.push_back(reg);
    --i;
  }
  return regs;
}

/**
 * Reports whether the first instruction of a function reads a nonvolatile register as an address
 * base or an operand before anything wrote it, or writes one before saving it. A store through r1
 * only saves, so it does not count.
 *
 * @param code Pointer to the big endian instruction word.
 * @return True when the word cannot start a function.
 */
bool HasNonvolatileFirst(const u8* code) {
  constexpr u32 kFirstNonvolatile = 14;
  constexpr u32 kStackPointer = 1;
  constexpr std::array<std::string_view, 6> kSourceFirst = {"st", "cmp", "mt", "tw", "td", "dcb"};

  ppc_insn insn;
  if (Disassemble(code, 0, insn) <= 0 || insn.opcode == nullptr)
    return false;

  const std::string_view name = insn.opcode->name;
  auto regs = GetRegisterOperands(insn.op_str);
  const bool is_store = name.starts_with("st");
  const bool writes_first =
      std::none_of(kSourceFirst.begin(), kSourceFirst.end(),
                   [&](std::string_view prefix) { return name.starts_with(prefix); });
  if (writes_first && !regs.empty()) {
    if (regs.front() >= kFirstNonvolatile)
      return true;
    regs.erase(regs.begin());
  }
  if (is_store && regs.size() > 1 && regs[1] == kStackPointer)
    regs.erase(regs.begin(), regs.begin() + 2);

  return std::any_of(regs.begin(), regs.end(), [&](u32 reg) { return reg >= kFirstNonvolatile; });
}

/**
 * Reports whether an uncovered word is data rather than the start of a function.
 *
 * @param ctx Codegen context holding the function graph after discovery.
 * @param binary Binary view the words are read from.
 * @param region Executable region that holds the word.
 * @param addr Address of the candidate word.
 * @return True when the word does not decode, uses a nonvolatile register before writing it,
 *         is exception handler data, or never reaches a terminator inside the region.
 */
bool IsDataWord(const CodegenContext& ctx, const BinaryView& binary, const CodeRegion& region,
                u32 addr) {
  const u8* first = binary.Translate(addr);
  if (!first)
    return true;
  if (HasNonvolatileFirst(first))
    return true;
  if (IsExceptionData(binary, ctx.graph, addr))
    return true;

  for (u32 cursor = addr; cursor < region.end; cursor += 4) {
    const u8* data = binary.Translate(cursor);
    if (!data)
      return true;
    auto decoded = DecodeInstruction(cursor, memory::load<be_u32>(data));
    if (decoded.opcode == Opcode::kUnknown)
      return true;
    if (decoded.IsReturn() || decoded.opcode == Opcode::bcctr || decoded.opcode == Opcode::b ||
        decoded.opcode == Opcode::ba || decoded.IsTrapAlways()) {
      return false;
    }
  }
  return true;
}

Coverage BuildCoverage(const FunctionGraph& graph) {
  Coverage coverage;
  for (const auto& [addr, node] : graph.GetFunctions()) {
    coverage.Add(addr, addr + node->GetSize());
  }
  return coverage;
}

void DiscoverRegisteredFunctions(CodegenContext& ctx, std::unordered_set<u32>& known) {
  const size_t max_iterations = ctx.GetLimits().max_discovery_iterations;
  for (size_t iteration = 0; iteration < max_iterations; ++iteration) {
    const size_t count = ctx.graph.GetFunctionCount();
    if (DiscoverPendingFunctions(ctx, known) == 0 || ctx.graph.GetFunctionCount() == count)
      break;
    known = BuildKnownFunctions(ctx.graph);
  }
}

void GapFillCodeRegions(CodegenContext& ctx) {
  REXLOG_TRACE(codegen, "Analyze: checking for uncovered code regions...");

  auto& graph = ctx.graph;
  auto& binary = ctx.GetBinary();

  Coverage coverage = BuildCoverage(graph);
  auto known = BuildKnownFunctions(graph);
  size_t registered = 0;

  for (const auto& region : ctx.scan.code_regions) {
    u32 addr = region.start;
    while (addr < region.end) {
      if (u32 end = coverage.GetCoveredEnd(addr)) {
        addr = end;
        continue;
      }
      if (IsDataWord(ctx, binary, region, addr)) {
        addr += 4;
        continue;
      }

      const size_t before = graph.GetFunctionCount();
      graph.AddFunction(addr, 4, FunctionAuthority::kGapFill, false);
      known.insert(addr);
      DiscoverRegisteredFunctions(ctx, known);
      REXLOG_TRACE(codegen, "GapFill: registered sub_{:08X}", addr);
      ++registered;

      if (graph.GetFunctionCount() > before + 1) {
        coverage = BuildCoverage(graph);
      } else if (const auto* node = graph.GetFunction(addr)) {
        coverage.Add(addr, addr + node->GetSize());
      }
      addr = std::max(addr + 4, coverage.GetCoveredEnd(addr));
    }
  }

  REXLOG_TRACE(codegen, "Analyze: registered {} GAP_FILL functions", registered);
}

/**
 * Removes GAP_FILL functions that another function contains.
 *
 * @param ctx Codegen context.
 */
void CleanupAbsorbedGapFills(CodegenContext& ctx) {
  auto& graph = ctx.graph;
  std::vector<u32> to_remove;

  for (const auto& [addr, node] : graph.GetFunctions()) {
    if (node->GetAuthority() != FunctionAuthority::kGapFill)
      continue;

    for (const auto& [otherAddr, otherNode] : graph.GetFunctions()) {
      if (otherAddr == addr)
        continue;
      if (!otherNode->ContainsAddress(addr))
        continue;

      if (otherNode->GetAuthority() != FunctionAuthority::kGapFill || otherAddr < addr) {
        to_remove.push_back(addr);
        break;
      }
    }
  }

  for (u32 addr : to_remove) {
    graph.RemoveFunction(addr);
  }

  if (!to_remove.empty()) {
    REXLOG_TRACE(codegen, "Analyze: removed {} absorbed GAP_FILL functions", to_remove.size());
  }
}

}  // anonymous namespace

namespace detail {

[[nodiscard]] Result<void> GapFill(CodegenContext& ctx, IProgressReporter* reporter) {
  (void)reporter;
  GapFillCodeRegions(ctx);
  CleanupAbsorbedGapFills(ctx);

  return Ok();
}

}  // namespace detail

}  // namespace rex::codegen
