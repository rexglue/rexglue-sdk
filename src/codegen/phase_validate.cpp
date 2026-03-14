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

#include <fmt/format.h>

#include <rex/codegen/analysis_errors.h>
#include <rex/logging.h>

#include "codegen_logging.h"
#include <rex/memory/utils.h>

#include <ppc.h>

using rex::memory::load_and_swap;

namespace rex::codegen {

namespace {

//=============================================================================
// Validate all calls resolve
//=============================================================================
const CallEdge* findCallEdgeAt(const FunctionNode* node, uint32_t site) {
  for (const auto& edge : node->calls()) {
    if (edge.site == site)
      return &edge;
  }
  for (const auto& edge : node->tailCalls()) {
    if (edge.site == site)
      return &edge;
  }
  return nullptr;
}

VoidResult validateGraph(CodegenContext& ctx) {
  REXCODEGEN_INFO("Analyze: validating call graph...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  auto& errors = ctx.errors;

  size_t functionsChecked = 0;
  size_t callsChecked = 0;
  size_t edgesVerified = 0;

  for (const auto& [addr, node] : graph.functions()) {
    functionsChecked++;

    for (const auto& block : node->blocks()) {
      const uint8_t* data = binary.translate(block.base);
      if (!data)
        continue;

      for (size_t offset = 0; offset < block.size; offset += 4) {
        uint32_t insn = load_and_swap<uint32_t>(data + offset);

        if (PPC_OP(insn) == PPC_OP_B && !PPC_BA(insn)) {
          uint32_t site = block.base + static_cast<uint32_t>(offset);
          int32_t branchOffset = PPC_BI(insn);
          uint32_t target = site + branchOffset;
          bool isCall = PPC_BL(insn);

          callsChecked++;

          bool targetExists = false;
          bool isInternalJump = false;

          // Check if target is within this function's blocks
          if (node->containsAddress(target)) {
            isInternalJump = true;
            targetExists = true;
          }

          // Check if target is within this function's overall bounds
          // (handles cases where blocks don't cover all owned addresses)
          if (!targetExists && node->isWithinBounds(target)) {
            isInternalJump = true;
            targetExists = true;
          }

          // Check if target is another function's entry point
          if (!targetExists && graph.isEntryPoint(target)) {
            targetExists = true;
          }

          // Check if target is an import
          if (!targetExists && graph.isImport(target)) {
            targetExists = true;
          }

          // Check if target is inside any other function's blocks
          // (handles cross-function internal branches due to gap-fill merging)
          if (!targetExists) {
            const FunctionNode* containingFunc = graph.getFunctionContaining(target);
            if (containingFunc) {
              // Target is inside another function - treat as internal to that function
              // This can happen when gap-fill created overlapping regions
              targetExists = true;
            }
          }

          if (!targetExists) {
            // Target is not in any function - this is an error that must stop the build
            errors.Add(AnalysisErrors::Category::UnresolvedCall, target, site,
                       fmt::format("{} 0x{:08X} from 0x{:08X} - target not in any function",
                                   isCall ? "bl" : "b", target, site));
            continue;
          }

          if (!isInternalJump) {
            const CallEdge* edge = findCallEdgeAt(node.get(), site);
            if (!edge) {
              // Check if target is inside another function - this is a special case
              // where code branches to another function's internal address
              const FunctionNode* containingFunc = graph.getFunctionContaining(target);
              if (!containingFunc) {
                // Target is not in any function - this is an error (call the cops)
                errors.Add(AnalysisErrors::Category::UnresolvedCall, target, site,
                           fmt::format("{} 0x{:08X} from 0x{:08X} in {} - no CallEdge recorded",
                                       isCall ? "bl" : "b", target, site, node->name()));
              }
              // If target is inside another function, it will be handled as a tail call
              // to that function's internal label during code generation
            } else {
              edgesVerified++;
            }
          }
        }
      }
    }
  }

  REXCODEGEN_INFO("Analyze: checked {} branches in {} functions, verified {} edges", callsChecked,
                  functionsChecked, edgesVerified);

  if (errors.HasErrors()) {
    REXCODEGEN_ERROR("Analyze: found {} errors", errors.Count());
    errors.PrintReport();
    return Err(ErrorCategory::Validation,
               fmt::format("Validation failed: {} unresolved calls",
                           errors.Count(AnalysisErrors::Category::UnresolvedCall)));
  }

  REXCODEGEN_INFO("Analyze: all calls resolve");
  return Ok();
}

}  // anonymous namespace

namespace phases {

VoidResult Validate(CodegenContext& ctx) {
  return validateGraph(ctx);
}

}  // namespace phases

}  // namespace rex::codegen
