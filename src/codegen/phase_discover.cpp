/**
 * @file        codegen/phase_discover.cpp
 * @brief       Discover phase: iterative function block discovery
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "decoded_binary.h"
#include <rex/codegen/function_scanner.h>

#include <array>
#include <bitset>
#include <unordered_set>

#include <rex/codegen/phases.h>
#include "phase_helpers.h"

#include <rex/codegen/vtable_scanner.h>
#include <rex/logging.h>

#include <rex/memory/utils.h>

#include <ppc.h>

using rex::codegen::ppc::Opcode;

namespace rex::codegen {

namespace {

void discoverFunction(CodegenContext& ctx, u32 funcAddr,
                      const std::unordered_set<u32>& knownFunctions) {
  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  auto& decoded = ctx.decoded();

  auto* node = graph.getFunction(funcAddr);
  if (!node)
    return;

  if (!node->canDiscover()) {
    REXLOG_TRACE(codegen, "Analyze: function 0x{:08X} already discovered, skipping", funcAddr);
    return;
  }

  if (node->isImport()) {
    node->discoverAsImport();
    return;
  }

  REXLOG_TRACE(codegen, "Analyze: discovering function 0x{:08X} ({})", funcAddr, node->name());

  u32 pdataSize = 0;

  // CONFIG sizes are entry point hints, so only a declared size applies and PDATA is ignored.
  if (node->authority() == FunctionAuthority::CONFIG) {
    pdataSize = node->size();
    REXLOG_TRACE(codegen, "Analyze: 0x{:08X} is CONFIG, using declared size={}", funcAddr,
                 pdataSize);
  } else {
    auto pdataIt = ctx.scan.pdataSizes.find(funcAddr);
    if (pdataIt != ctx.scan.pdataSizes.end()) {
      pdataSize = pdataIt->second;
      REXLOG_TRACE(codegen, "Analyze: 0x{:08X} using PDATA size={}", funcAddr, pdataSize);
    }
  }

  const CodeRegion* region = nullptr;
  for (const auto& r : ctx.scan.codeRegions) {
    if (r.contains(funcAddr)) {
      region = &r;
      break;
    }
  }
  if (!region) {
    REXLOG_WARN(codegen, "Analyze: function 0x{:08X} not in any code region", funcAddr);
    return;
  }

  auto result = discoverBlocks(decoded, ctx.limits(), funcAddr, *region, knownFunctions, pdataSize,
                               &ctx.Config().switchTables);

  if (result.blocks.empty()) {
    REXLOG_WARN(codegen, "Analyze: no blocks found for function 0x{:08X}", funcAddr);
    return;
  }

  node->discover(std::move(result.blocks), std::move(result.instructions),
                 std::move(result.labels));

  for (const auto& jt : result.jumpTables) {
    graph.addJumpTableToFunction(funcAddr, jt);
  }

  for (u32 target : result.externalCalls) {
    if (!graph.isEntryPoint(target) && !graph.isImport(target)) {
      if (binary.isInImportExportRange(target)) {
        continue;
      }
      graph.addFunction(target, 4, FunctionAuthority::DISCOVERED, true);
    }
  }

  for (const auto& branch : result.unresolvedBranches) {
    graph.addUnresolvedJumpToFunction(funcAddr, branch.site, branch.target, branch.isCall,
                                      branch.isConditional);
  }

  if (pdataSize > 0) {
    std::unordered_set<u32> discoveredAddrs;
    for (const auto& block : result.blocks) {
      for (u32 addr = block.base; addr < block.base + block.size; addr += 4) {
        discoveredAddrs.insert(addr);
      }
    }

    u32 pdataEnd = funcAddr + pdataSize;
    const u8* funcData = binary.translate(funcAddr);
    if (funcData) {
      for (u32 offset = 0; offset < pdataSize; offset += 4) {
        u32 site = funcAddr + offset;

        if (discoveredAddrs.count(site))
          continue;

        auto invalidIt = ctx.analysisState().invalidInstructions.find(site);
        if (invalidIt != ctx.analysisState().invalidInstructions.end()) {
          continue;
        }

        u32 insn = memory::load<be_u32>(funcData + offset);
        u32 opcode = PPC_OP(insn);

        if (opcode != PPC_OP_B && opcode != PPC_OP_BC)
          continue;

        u32 target = 0;
        bool isCall = PPC_BL(insn);
        bool isAbsolute = PPC_BA(insn);

        if (opcode == PPC_OP_B) {
          i32 branchOffset = PPC_BI(insn);
          target = isAbsolute ? static_cast<u32>(branchOffset) : site + branchOffset;
        } else {
          i32 branchOffset = PPC_BD(insn);
          target = isAbsolute ? static_cast<u32>(branchOffset) : site + branchOffset;
        }

        if (!isCall && target >= funcAddr && target < pdataEnd) {
          continue;
        }

        graph.addUnresolvedJumpToFunction(funcAddr, site, target, isCall, false);

        if (isCall && !graph.isEntryPoint(target) && !graph.isImport(target)) {
          if (binary.isInImportExportRange(target)) {
            continue;
          }
          graph.addFunction(target, 4, FunctionAuthority::DISCOVERED, true);
        }
      }
    }
  }
}

void discoverAllFunctions(CodegenContext& ctx) {
  REXLOG_TRACE(codegen, "Analyze: starting iterative discovery...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();

  size_t iteration = 0;
  size_t lastFunctionCount = 0;
  const size_t maxIterations = ctx.limits().max_discovery_iterations;

  while (iteration < maxIterations) {
    iteration++;

    size_t currentFunctionCount = graph.functionCount();
    if (currentFunctionCount == lastFunctionCount && iteration > 1) {
      REXLOG_DEBUG(codegen, "Analyze: fixed point at iteration {} ({} functions)", iteration,
                   currentFunctionCount);
      break;
    }

    lastFunctionCount = currentFunctionCount;

    auto knownFunctions = buildKnownFunctions(graph);
    if (discoverPendingFunctions(ctx, knownFunctions) == 0) {
      break;
    }
  }

  REXLOG_TRACE(codegen, "Analyze: {} functions after call graph expansion", graph.functionCount());

  {
    VTableScanner vtScanner(binary);
    auto vtables = vtScanner.scan();

    size_t newFunctions = 0;

    for (const auto& vt : vtables) {
      for (u32 funcAddr : vt.slots) {
        if (graph.isEntryPoint(funcAddr))
          continue;
        if (binary.isInImportExportRange(funcAddr))
          continue;

        graph.addFunction(funcAddr, 4, FunctionAuthority::VTABLE, true);
        newFunctions++;
      }
    }

    REXLOG_TRACE(codegen, "Analyze: VTable scan found {} vtables, {} new functions", vtables.size(),
                 newFunctions);

    if (newFunctions > 0) {
      size_t vtableIteration = 0;
      const size_t maxVtableIterations = ctx.limits().max_vtable_iterations;

      while (vtableIteration < maxVtableIterations) {
        vtableIteration++;

        auto knownFunctions = buildKnownFunctions(graph);
        if (discoverPendingFunctions(ctx, knownFunctions) == 0)
          break;

        if (graph.functionCount() == lastFunctionCount)
          break;
        lastFunctionCount = graph.functionCount();
      }
    }
  }

  REXLOG_TRACE(codegen, "Analyze: {} total functions after vtable scan", graph.functionCount());
}

}  // anonymous namespace

size_t discoverPendingFunctions(CodegenContext& ctx,
                                const std::unordered_set<u32>& knownFunctions) {
  std::vector<u32> pending;
  for (const auto& [addr, node] : ctx.graph.functions()) {
    if (node->canDiscover()) {
      pending.push_back(addr);
    }
  }
  for (u32 funcAddr : pending) {
    discoverFunction(ctx, funcAddr, knownFunctions);
  }
  return pending.size();
}

namespace phases {

[[nodiscard]] Result<void> Discover(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  discoverAllFunctions(ctx);
  return Ok();
}

}  // namespace phases

}  // namespace rex::codegen
