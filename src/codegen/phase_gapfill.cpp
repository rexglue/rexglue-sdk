/**
 * @file        codegen/phase_gapfill.cpp
 * @brief       GapFill phase: find uncovered code regions and register them as functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc/instruction.h"

#include <unordered_set>

#include <rex/codegen/phases.h>
#include "phase_helpers.h"

#include <rex/logging/log.h>

#include <rex/memory/byte_swap.h>

#include <ppc.h>

using rex::codegen::detail::DecodeInstruction;
using rex::codegen::detail::Opcode;

namespace rex::codegen {

namespace {

/**
 * Splits a code region into function segments at terminators such as blr and tail calls.
 *
 * @param region Code region to split.
 * @param binary Binary view.
 * @param known_callables Entry points that mark a branch as a tail call.
 * @return The segments.
 */
std::vector<CodeRegion> SplitRegionOnTerminators(const CodeRegion& region, const BinaryView& binary,
                                                 const std::unordered_set<u32>& known_callables) {
  std::vector<CodeRegion> segments;
  u32 segment_start = region.start;

  for (u32 addr = region.start; addr < region.end; addr += 4) {
    const u8* data = binary.Translate(addr);
    if (!data)
      break;

    u32 raw = memory::load<be_u32>(data);
    auto decoded = DecodeInstruction(addr, raw);
    bool should_split = false;
    const char* reason = nullptr;

    if (decoded.IsReturn()) {
      should_split = true;
      reason = "blr";
    } else if (decoded.opcode == Opcode::b && decoded.branch_target.has_value()) {
      u32 target = decoded.branch_target.value();
      if (target != segment_start && known_callables.contains(target)) {
        should_split = true;
        reason = "tail call";
      }
    }

    if (should_split) {
      u32 segment_end = addr + 4;
      if (segment_end > segment_start) {
        segments.push_back({segment_start, segment_end});
        REXLOG_TRACE(codegen, "GapFill: split segment 0x{:08X}-0x{:08X} ({} at 0x{:08X})",
                     segment_start, segment_end, reason, addr);
      }
      segment_start = segment_end;
    }
  }

  if (segment_start < region.end) {
    segments.push_back({segment_start, region.end});
  }

  return segments;
}

/**
 * Reports whether an address holds exception handler data, a pointer to a known entry point such
 * as __C_specific_handler at +0 followed by a pointer to the scope table in .rdata at +4.
 *
 * @param binary Binary view.
 * @param graph Function graph holding the known entry points.
 * @param addr Address to test.
 * @return True when the data matches.
 */
bool LooksLikeExceptionData(const BinaryView& binary, const FunctionGraph& graph, u32 addr) {
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

void GapFillCodeRegions(CodegenContext& ctx) {
  REXLOG_TRACE(codegen, "Analyze: checking for uncovered code regions...");

  auto& graph = ctx.graph;
  auto& binary = ctx.GetBinary();
  auto& scan = ctx.scan;

  std::unordered_set<u32> known_callables;
  for (const auto& [addr, node] : graph.GetFunctions()) {
    known_callables.insert(addr);
  }

  size_t gaps_found = 0;
  size_t segments_created = 0;

  for (const auto& region : scan.code_regions) {
    auto segments = SplitRegionOnTerminators(region, binary, known_callables);

    for (const auto& segment : segments) {
      if (graph.IsEntryPoint(segment.start))
        continue;

      if (graph.GetFunctionContaining(segment.start)) {
        continue;
      }

      if (LooksLikeExceptionData(binary, graph, segment.start))
        continue;

      u32 segment_size = segment.GetSize();
      graph.AddFunction(segment.start, segment_size, FunctionAuthority::kGapFill, false);

      REXLOG_TRACE(codegen, "GapFill: registered sub_{:08X} (0x{:08X}-0x{:08X}, {} bytes)",
                   segment.start, segment.start, segment.end, segment_size);
      segments_created++;
    }

    gaps_found++;
  }

  if (segments_created > 0) {
    REXLOG_TRACE(codegen, "Analyze: registered {} gap functions from {} regions", segments_created,
                 gaps_found);
  } else {
    REXLOG_TRACE(codegen, "Analyze: no uncovered regions found");
  }
}

/**
 * Removes GAP_FILL functions that lie inside another function.
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

[[nodiscard]] Result<void> GapFill(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  GapFillCodeRegions(ctx);

  auto known = BuildKnownFunctions(ctx.graph, /*exclude_gap_fill=*/true);
  size_t discovered = DiscoverPendingFunctions(ctx, known);
  REXLOG_TRACE(codegen, "Analyze: discovered blocks for {} gap-filled functions", discovered);

  CleanupAbsorbedGapFills(ctx);

  return Ok();
}

}  // namespace detail

}  // namespace rex::codegen
