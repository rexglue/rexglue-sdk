/**
 * @file        codegen/phase_validate.cpp
 * @brief       Validate phase that verifies all call targets resolve
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "pipeline.h"

#include <format>
#include <vector>

#include "analysis_error_log.h"
#include "config.h"
#include "function_types.h"
#include <rex/logging/log_category.h>
#include "codegen_categories.h"

#include <rex/memory/endian.h>

#include <ppc.h>

namespace rex::codegen {

namespace {

const CallEdge* FindCallEdgeAt(const FunctionNode* node, u32 site) {
  for (const auto& edge : node->GetCalls()) {
    if (edge.site == site)
      return &edge;
  }
  for (const auto& edge : node->GetTailCalls()) {
    if (edge.site == site)
      return &edge;
  }
  return nullptr;
}

/**
 * Warns when the graph contradicts a config function boundary. Never fatal. A PDATA entry inside a
 * config extent is normal for a function with an SEH funclet, so only an entry point carved out of
 * a declared range warns.
 */
void ValidateConfigBoundaries(CodegenContext& ctx) {
  auto& graph = ctx.graph;

  for (const auto& [addr, fc] : ctx.GetConfig().functions) {
    if (fc.IsChunk())
      continue;

    const FunctionNode* node = graph.GetFunction(addr);
    if (!node) {
      REXCG_WARN("functions 0x{:08X} is not a function entry point", addr);
      continue;
    }
    if (node->GetAuthority() != FunctionAuthority::kConfig) {
      REXCG_WARN("functions 0x{:08X} outranked by {}", addr, AuthorityName(node->GetAuthority()));
      continue;
    }

    u32 declared = fc.GetSize(addr);
    if (declared == 0)
      continue;

    for (const auto& [otherAddr, other] : graph.GetFunctions()) {
      if (otherAddr <= addr || otherAddr >= addr + declared)
        continue;
      auto authority = other->GetAuthority();
      if (authority == FunctionAuthority::kPData || authority == FunctionAuthority::kHelper) {
        REXCG_DEBUG("functions 0x{:08X} size 0x{:X} contains {} 0x{:08X}", addr, declared,
                    AuthorityName(authority), otherAddr);
        continue;
      }
      REXCG_WARN("functions 0x{:08X} size 0x{:X} split by {} entry point 0x{:08X}", addr, declared,
                 AuthorityName(authority), otherAddr);
    }
  }
}

bool IsInsideMillicodeRun(const FunctionGraph& graph, u32 addr) {
  const FunctionNode* node = graph.GetFunctionContaining(addr);
  return node && node->GetMillicode() && node->GetBase() != addr;
}

}  // anonymous namespace

namespace detail {

/**
 * Validates that every direct b and bl target resolves. A target inside another function counts
 * as resolved and is emitted as a tail call to that function's internal label. A target in no
 * function, or a call with no registered edge, stops the build.
 */
[[nodiscard]] Result<void> ValidateGraph(CodegenContext& ctx) {
  REXCG_TRACE("Analyze: validating call graph...");

  auto& graph = ctx.graph;
  auto& binary = ctx.GetBinary();
  auto& errors = ctx.errors;

  ValidateConfigBoundaries(ctx);

  size_t functions_checked = 0;
  size_t calls_checked = 0;
  size_t edges_verified = 0;

  for (const auto& [addr, node] : graph.GetFunctions()) {
    functions_checked++;

    for (const auto& block : node->GetBlocks()) {
      const u8* data = binary.Translate(block.base);
      if (!data)
        continue;

      for (size_t offset = 0; offset < block.size; offset += 4) {
        u32 insn = memory::load<be_u32>(data + offset);

        if (PPC_OP(insn) == PPC_OP_B && !PPC_BA(insn)) {
          u32 site = block.base + static_cast<u32>(offset);
          i32 branch_offset = PPC_BI(insn);
          u32 target = site + branch_offset;
          bool is_call = PPC_BL(insn);

          calls_checked++;

          if (IsInsideMillicodeRun(graph, target)) {
            errors.Add(AnalysisErrorLog::Category::kMillicode, target, site,
                       std::format("{} 0x{:08X} from 0x{:08X} is inside a millicode run, off "
                                   "its entries",
                                   is_call ? "bl" : "b", target, site));
            continue;
          }

          bool target_exists = false;
          bool is_internal_jump = false;

          if (node->ContainsAddress(target)) {
            is_internal_jump = true;
            target_exists = true;
          }

          if (!target_exists && node->IsWithinBounds(target)) {
            is_internal_jump = true;
            target_exists = true;
          }

          if (!target_exists && graph.IsEntryPoint(target)) {
            target_exists = true;
          }

          if (!target_exists && graph.IsImport(target)) {
            target_exists = true;
          }

          if (!target_exists) {
            const FunctionNode* containing_func = graph.GetFunctionContaining(target);
            if (containing_func) {
              target_exists = true;
            }
          }

          if (!target_exists) {
            errors.Add(AnalysisErrorLog::Category::kUnresolvedCall, target, site,
                       std::format("{} 0x{:08X} from 0x{:08X} has a target in no function",
                                   is_call ? "bl" : "b", target, site));
            continue;
          }

          if (!is_internal_jump) {
            const CallEdge* edge = FindCallEdgeAt(node.get(), site);
            if (!edge) {
              const FunctionNode* containing_func = graph.GetFunctionContaining(target);
              if (!containing_func) {
                errors.Add(AnalysisErrorLog::Category::kUnresolvedCall, target, site,
                           std::format("{} 0x{:08X} from 0x{:08X} in {} has no CallEdge",
                                       is_call ? "bl" : "b", target, site, node->GetName()));
              }
            } else {
              edges_verified++;
            }
          }
        }
      }
    }
  }

  REXCG_TRACE("Analyze: checked {} branches in {} functions, verified {} edges", calls_checked,
              functions_checked, edges_verified);

  if (errors.HasErrors()) {
    REXCG_ERROR("Analyze: found {} errors", errors.Count());
    errors.PrintReport();
    return Err(ErrorCategory::kValidation,
               std::format("Validation failed: {} unresolved calls, {} millicode errors",
                           errors.Count(AnalysisErrorLog::Category::kUnresolvedCall),
                           errors.Count(AnalysisErrorLog::Category::kMillicode)));
  }

  REXCG_TRACE("Analyze: all calls resolve");
  return Ok();
}

}  // namespace detail

}  // namespace rex::codegen
