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

#include <rex/logging.h>

#include <rex/memory/utils.h>

#include <ppc.h>

using rex::codegen::ppc::decode_instruction;
using rex::codegen::ppc::Opcode;

namespace rex::codegen {

namespace {

/**
 * Splits a code region into function segments at terminators such as blr and tail calls.
 *
 * @param region Code region to split.
 * @param binary Binary view.
 * @param knownCallables Entry points that mark a branch as a tail call.
 * @return The segments.
 */
std::vector<CodeRegion> splitRegionOnTerminators(const CodeRegion& region, const BinaryView& binary,
                                                 const std::unordered_set<u32>& knownCallables) {
  std::vector<CodeRegion> segments;
  u32 segmentStart = region.start;

  for (u32 addr = region.start; addr < region.end; addr += 4) {
    const u8* data = binary.translate(addr);
    if (!data)
      break;

    u32 raw = memory::load<be_u32>(data);
    auto decoded = decode_instruction(addr, raw);
    bool shouldSplit = false;
    const char* reason = nullptr;

    if (decoded.is_return()) {
      shouldSplit = true;
      reason = "blr";
    } else if (decoded.opcode == Opcode::b && decoded.branch_target.has_value()) {
      u32 target = decoded.branch_target.value();
      if (target != segmentStart && knownCallables.contains(target)) {
        shouldSplit = true;
        reason = "tail call";
      }
    }

    if (shouldSplit) {
      u32 segmentEnd = addr + 4;
      if (segmentEnd > segmentStart) {
        segments.push_back({segmentStart, segmentEnd});
        REXLOG_TRACE(codegen, "GapFill: split segment 0x{:08X}-0x{:08X} ({} at 0x{:08X})",
                     segmentStart, segmentEnd, reason, addr);
      }
      segmentStart = segmentEnd;
    }
  }

  if (segmentStart < region.end) {
    segments.push_back({segmentStart, region.end});
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
bool looksLikeExceptionData(const BinaryView& binary, const FunctionGraph& graph, u32 addr) {
  const u8* data = binary.translate(addr);
  if (!data)
    return false;

  u32 firstDword = memory::load<be_u32>(data);
  u32 secondDword = memory::load<be_u32>(data + 4);

  if (!graph.isEntryPoint(firstDword)) {
    return false;
  }

  auto* rdataSection = binary.findSectionByName(".rdata");
  if (!rdataSection)
    return false;

  u32 rdataStart = rdataSection->baseAddress;
  u32 rdataEnd = rdataStart + rdataSection->size;

  if (secondDword >= rdataStart && secondDword < rdataEnd) {
    REXLOG_TRACE(
        codegen,
        "GapFill: 0x{:08X} looks like exception data (handler=0x{:08X}, scope=0x{:08X}), skipping",
        addr, firstDword, secondDword);
    return true;
  }

  return false;
}

void gapFillCodeRegions(CodegenContext& ctx) {
  REXLOG_TRACE(codegen, "Analyze: checking for uncovered code regions...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  auto& scan = ctx.scan;

  std::unordered_set<u32> knownCallables;
  for (const auto& [addr, node] : graph.functions()) {
    knownCallables.insert(addr);
  }

  size_t gapsFound = 0;
  size_t segmentsCreated = 0;

  for (const auto& region : scan.codeRegions) {
    auto segments = splitRegionOnTerminators(region, binary, knownCallables);

    for (const auto& segment : segments) {
      if (graph.isEntryPoint(segment.start))
        continue;

      if (graph.getFunctionContaining(segment.start)) {
        continue;
      }

      if (looksLikeExceptionData(binary, graph, segment.start))
        continue;

      u32 segmentSize = segment.size();
      graph.addFunction(segment.start, segmentSize, FunctionAuthority::GAP_FILL, false);

      REXLOG_TRACE(codegen, "GapFill: registered sub_{:08X} (0x{:08X}-0x{:08X}, {} bytes)",
                   segment.start, segment.start, segment.end, segmentSize);
      segmentsCreated++;
    }

    gapsFound++;
  }

  if (segmentsCreated > 0) {
    REXLOG_TRACE(codegen, "Analyze: registered {} gap functions from {} regions", segmentsCreated,
                 gapsFound);
  } else {
    REXLOG_TRACE(codegen, "Analyze: no uncovered regions found");
  }
}

/**
 * Removes GAP_FILL functions that lie inside another function.
 *
 * @param ctx Codegen context.
 */
void cleanupAbsorbedGapFills(CodegenContext& ctx) {
  auto& graph = ctx.graph;
  std::vector<u32> toRemove;

  for (const auto& [addr, node] : graph.functions()) {
    if (node->authority() != FunctionAuthority::GAP_FILL)
      continue;

    for (const auto& [otherAddr, otherNode] : graph.functions()) {
      if (otherAddr == addr)
        continue;
      if (!otherNode->containsAddress(addr))
        continue;

      if (otherNode->authority() != FunctionAuthority::GAP_FILL || otherAddr < addr) {
        toRemove.push_back(addr);
        break;
      }
    }
  }

  for (u32 addr : toRemove) {
    graph.removeFunction(addr);
  }

  if (!toRemove.empty()) {
    REXLOG_TRACE(codegen, "Analyze: removed {} absorbed GAP_FILL functions", toRemove.size());
  }
}

}  // anonymous namespace

namespace phases {

[[nodiscard]] Result<void> GapFill(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  gapFillCodeRegions(ctx);

  auto known = buildKnownFunctions(ctx.graph, /*excludeGapFill=*/true);
  size_t discovered = discoverPendingFunctions(ctx, known);
  REXLOG_TRACE(codegen, "Analyze: discovered blocks for {} gap-filled functions", discovered);

  cleanupAbsorbedGapFills(ctx);

  return Ok();
}

}  // namespace phases

}  // namespace rex::codegen
