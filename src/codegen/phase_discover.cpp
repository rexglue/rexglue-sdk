/**
 * @file        codegen/phase_discover.cpp
 * @brief       Discover phase that iterates function block discovery
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "decoded_binary.h"
#include "function_scanner.h"

#include <array>
#include <bitset>
#include <unordered_set>

#include "pipeline.h"
#include "known_function.h"

#include "vtable_scanner.h"
#include <rex/logging/log_category.h>
#include "codegen_categories.h"

#include <rex/memory/endian.h>

#include <ppc.h>

namespace rex::codegen {

namespace {

void DiscoverFunction(CodegenContext& ctx, u32 func_addr,
                      const std::unordered_set<u32>& known_functions) {
  auto& graph = ctx.graph;
  auto& binary = ctx.GetBinary();
  auto& decoded = ctx.GetDecoded();

  auto* node = graph.GetFunction(func_addr);
  if (!node)
    return;

  if (!node->CanDiscover()) {
    REXCG_TRACE("Analyze: function 0x{:08X} already discovered, skipping", func_addr);
    return;
  }

  if (node->IsImport()) {
    node->DiscoverAsImport();
    return;
  }

  REXCG_TRACE("Analyze: discovering function 0x{:08X} ({})", func_addr, node->GetName());

  u32 pdata_size = 0;

  // CONFIG sizes are entry point hints, so only a declared size applies and PDATA is ignored.
  if (node->GetAuthority() == FunctionAuthority::kConfig) {
    pdata_size = node->GetSize();
    REXCG_TRACE("Analyze: 0x{:08X} is CONFIG, using declared size={}", func_addr, pdata_size);
  } else {
    auto pdata_it = ctx.scan.pdata_sizes.find(func_addr);
    if (pdata_it != ctx.scan.pdata_sizes.end()) {
      pdata_size = pdata_it->second;
      REXCG_TRACE("Analyze: 0x{:08X} using PDATA size={}", func_addr, pdata_size);
    }
  }

  const CodeRegion* region = nullptr;
  for (const auto& r : ctx.scan.code_regions) {
    if (r.Contains(func_addr)) {
      region = &r;
      break;
    }
  }
  if (!region) {
    REXCG_WARN("Analyze: function 0x{:08X} not in any code region", func_addr);
    return;
  }

  auto result = DiscoverBlocks(decoded, ctx.GetLimits(), func_addr, *region, known_functions,
                               pdata_size, &ctx.GetConfig().switch_tables);

  if (node->GetAuthority() == FunctionAuthority::kGapFill && pdata_size == 0) {
    if (const auto cut = FindReturnCut(result, func_addr)) {
      REXCG_TRACE("GapFill: cut sub_{:08X} after the return at 0x{:08X}", func_addr, *cut - 4);
      result = DiscoverBlocks(decoded, ctx.GetLimits(), func_addr, *region, known_functions,
                              *cut - func_addr, &ctx.GetConfig().switch_tables);
    }
  }

  if (result.blocks.empty()) {
    REXCG_WARN("Analyze: no blocks found for function 0x{:08X}", func_addr);
    return;
  }

  node->Discover(std::move(result.blocks), std::move(result.instructions),
                 std::move(result.labels));
  node->SetFallThroughs(std::move(result.fall_throughs));

  for (const auto& jt : result.jump_tables) {
    graph.AddJumpTableToFunction(func_addr, jt);
  }

  for (u32 target : result.external_calls) {
    if (!graph.IsEntryPoint(target) && !graph.IsImport(target)) {
      if (binary.IsInImportExportRange(target)) {
        continue;
      }
      graph.AddFunction(target, 4, FunctionAuthority::kDiscovered, true);
    }
  }

  for (const auto& branch : result.unresolved_jumps) {
    graph.AddUnresolvedJumpToFunction(func_addr, branch.site, branch.target, branch.is_call,
                                      branch.is_conditional);
  }

  if (pdata_size > 0) {
    std::unordered_set<u32> discovered_addrs;
    for (const auto& block : result.blocks) {
      for (u32 addr = block.base; addr < block.base + block.size; addr += 4) {
        discovered_addrs.insert(addr);
      }
    }

    u32 pdata_end = func_addr + pdata_size;
    const u8* func_data = binary.Translate(func_addr);
    if (func_data) {
      for (u32 offset = 0; offset < pdata_size; offset += 4) {
        u32 site = func_addr + offset;

        if (discovered_addrs.count(site))
          continue;

        auto invalid_it = ctx.GetAnalysisState().invalid_instructions.find(site);
        if (invalid_it != ctx.GetAnalysisState().invalid_instructions.end()) {
          continue;
        }

        u32 insn = memory::load<be_u32>(func_data + offset);
        u32 opcode = PPC_OP(insn);

        if (opcode != PPC_OP_B && opcode != PPC_OP_BC)
          continue;

        u32 target = 0;
        bool is_call = PPC_BL(insn);
        bool is_absolute = PPC_BA(insn);

        if (opcode == PPC_OP_B) {
          i32 branch_offset = PPC_BI(insn);
          target = is_absolute ? static_cast<u32>(branch_offset) : site + branch_offset;
        } else {
          i32 branch_offset = PPC_BD(insn);
          target = is_absolute ? static_cast<u32>(branch_offset) : site + branch_offset;
        }

        if (!is_call && target >= func_addr && target < pdata_end) {
          continue;
        }

        graph.AddUnresolvedJumpToFunction(func_addr, site, target, is_call, false);

        if (is_call && !graph.IsEntryPoint(target) && !graph.IsImport(target)) {
          if (binary.IsInImportExportRange(target)) {
            continue;
          }
          graph.AddFunction(target, 4, FunctionAuthority::kDiscovered, true);
        }
      }
    }
  }
}

void DiscoverAllFunctions(CodegenContext& ctx) {
  REXCG_TRACE("Analyze: starting iterative discovery...");

  auto& graph = ctx.graph;
  auto& binary = ctx.GetBinary();

  size_t iteration = 0;
  size_t last_function_count = 0;
  const size_t max_iterations = ctx.GetLimits().max_discovery_iterations;

  while (iteration < max_iterations) {
    iteration++;

    size_t current_function_count = graph.GetFunctionCount();
    if (current_function_count == last_function_count && iteration > 1) {
      REXCG_DEBUG("Analyze: fixed point at iteration {} ({} functions)", iteration,
                  current_function_count);
      break;
    }

    last_function_count = current_function_count;

    auto known_functions = BuildKnownFunctions(graph);
    if (DiscoverPendingFunctions(ctx, known_functions) == 0) {
      break;
    }
  }

  REXCG_TRACE("Analyze: {} functions after call graph expansion", graph.GetFunctionCount());

  {
    VTableScanner vt_scanner(binary);
    auto vtables = vt_scanner.Scan();

    size_t new_functions = 0;

    for (const auto& vt : vtables) {
      for (u32 func_addr : vt.slots) {
        if (graph.IsEntryPoint(func_addr))
          continue;
        if (binary.IsInImportExportRange(func_addr))
          continue;

        graph.AddFunction(func_addr, 4, FunctionAuthority::kVTable, true);
        new_functions++;
      }
    }

    REXCG_TRACE("Analyze: VTable scan found {} vtables, {} new functions", vtables.size(),
                new_functions);

    if (new_functions > 0) {
      size_t vtable_iteration = 0;
      const size_t max_vtable_iterations = ctx.GetLimits().max_vtable_iterations;

      while (vtable_iteration < max_vtable_iterations) {
        vtable_iteration++;

        auto known_functions = BuildKnownFunctions(graph);
        if (DiscoverPendingFunctions(ctx, known_functions) == 0)
          break;

        if (graph.GetFunctionCount() == last_function_count)
          break;
        last_function_count = graph.GetFunctionCount();
      }
    }
  }

  REXCG_TRACE("Analyze: {} total functions after vtable scan", graph.GetFunctionCount());
}

}  // anonymous namespace

size_t DiscoverPendingFunctions(CodegenContext& ctx,
                                const std::unordered_set<u32>& known_functions) {
  std::vector<u32> pending;
  for (const auto& [addr, node] : ctx.graph.GetFunctions()) {
    if (node->CanDiscover()) {
      pending.push_back(addr);
    }
  }
  for (u32 func_addr : pending) {
    DiscoverFunction(ctx, func_addr, known_functions);
  }
  return pending.size();
}

namespace detail {

[[nodiscard]] Result<void> DiscoverFunctions(CodegenContext& ctx, IProgressReporter* reporter) {
  (void)reporter;
  DiscoverAllFunctions(ctx);
  return Ok();
}

}  // namespace detail

}  // namespace rex::codegen
