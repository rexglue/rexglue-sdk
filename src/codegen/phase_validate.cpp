/**
 * @file        codegen/phase_validate.cpp
 * @brief       Validate phase: verify all call targets resolve
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/phases.h>

#include <format>
#include <vector>

#include <rex/codegen/analysis_errors.h>
#include <rex/codegen/config.h>
#include <rex/codegen/function_types.h>
#include <rex/logging/log.h>

#include <rex/memory/byte_swap.h>

#include <ppc.h>

namespace rex::codegen {

namespace {

/**
 * Finds the call or tail call edge registered at a site.
 *
 * @param node Function holding the site.
 * @param site Address of the branch.
 * @return The edge, or nullptr when none is registered.
 */
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
 * Warns when the graph contradicts a config function boundary, which the binary cannot assert
 * for itself. Never fatal. A PDATA entry inside a config extent is the normal MSVC layout for a
 * function with an SEH funclet, so only discovery carving an entry point out of a declared range
 * warns.
 *
 * @param ctx Codegen context.
 */
void ValidateConfigBoundaries(CodegenContext& ctx) {
  auto& graph = ctx.graph;

  for (const auto& [addr, fc] : ctx.GetConfig().functions) {
    if (fc.IsChunk())
      continue;

    const FunctionNode* node = graph.GetFunction(addr);
    if (!node) {
      REXLOG_WARN(codegen, "functions 0x{:08X} is not a function entry point", addr);
      continue;
    }
    if (node->GetAuthority() != FunctionAuthority::kConfig) {
      REXLOG_WARN(codegen, "functions 0x{:08X} outranked by {}", addr,
                  AuthorityName(node->GetAuthority()));
      continue;
    }

    // Use the declared size, since Discover() grows node->size() to cover its blocks.
    u32 declared = fc.GetSize(addr);
    if (declared == 0)
      continue;

    for (const auto& [otherAddr, other] : graph.GetFunctions()) {
      if (otherAddr <= addr || otherAddr >= addr + declared)
        continue;
      auto authority = other->GetAuthority();
      if (authority == FunctionAuthority::kPData || authority == FunctionAuthority::kHelper) {
        REXLOG_DEBUG(codegen, "functions 0x{:08X} size 0x{:X} contains {} 0x{:08X}", addr, declared,
                     AuthorityName(authority), otherAddr);
        continue;
      }
      REXLOG_WARN(codegen, "functions 0x{:08X} size 0x{:X} split by {} entry point 0x{:08X}", addr,
                  declared, AuthorityName(authority), otherAddr);
    }
  }
}

/**
 * Reports whether an address lies inside a millicode run without being an entry of it.
 *
 * @param graph Function graph holding the run entries.
 * @param addr Address to test.
 * @return True for an address in the middle of an entry or in the closing words.
 */
bool IsInsideMillicodeRun(const FunctionGraph& graph, u32 addr) {
  const FunctionNode* node = graph.GetFunctionContaining(addr);
  return node && node->GetMillicode() && node->GetBase() != addr;
}

/**
 * Validates that every direct b and bl target resolves. A target inside another function counts
 * as resolved, as gap fill can produce, and is emitted as a tail call to that function's internal
 * label. A target in no function, or a call with no registered edge, stops the build.
 *
 * @param ctx Codegen context.
 * @return Ok, or a validation error.
 */
[[nodiscard]] Result<void> ValidateGraph(CodegenContext& ctx) {
  REXLOG_TRACE(codegen, "Analyze: validating call graph...");

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
            errors.Add(AnalysisErrors::Category::kMillicode, target, site,
                       std::format("{} 0x{:08X} from 0x{:08X} lands inside a millicode run, off "
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
            errors.Add(AnalysisErrors::Category::kUnresolvedCall, target, site,
                       std::format("{} 0x{:08X} from 0x{:08X} - target not in any function",
                                   is_call ? "bl" : "b", target, site));
            continue;
          }

          if (!is_internal_jump) {
            const CallEdge* edge = FindCallEdgeAt(node.get(), site);
            if (!edge) {
              const FunctionNode* containing_func = graph.GetFunctionContaining(target);
              if (!containing_func) {
                errors.Add(AnalysisErrors::Category::kUnresolvedCall, target, site,
                           std::format("{} 0x{:08X} from 0x{:08X} in {} - no CallEdge recorded",
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

  REXLOG_TRACE(codegen, "Analyze: checked {} branches in {} functions, verified {} edges",
               calls_checked, functions_checked, edges_verified);

  if (errors.HasErrors()) {
    REXLOG_ERROR(codegen, "Analyze: found {} errors", errors.Count());
    errors.PrintReport();
    return Err(ErrorCategory::kValidation,
               std::format("Validation failed: {} unresolved calls, {} millicode errors",
                           errors.Count(AnalysisErrors::Category::kUnresolvedCall),
                           errors.Count(AnalysisErrors::Category::kMillicode)));
  }

  REXLOG_TRACE(codegen, "Analyze: all calls resolve");
  return Ok();
}

}  // anonymous namespace

namespace detail {

[[nodiscard]] Result<void> Validate(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  return ValidateGraph(ctx);
}

}  // namespace detail

}  // namespace rex::codegen
