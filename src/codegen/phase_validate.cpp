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

#include <rex/codegen/analysis_errors.h>
#include <rex/codegen/config.h>
#include <rex/codegen/function_types.h>
#include <rex/logging.h>

#include <rex/memory/utils.h>

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
const CallEdge* findCallEdgeAt(const FunctionNode* node, u32 site) {
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

/**
 * Warns when the graph contradicts a config function boundary, which the binary cannot assert
 * for itself. Never fatal. A PDATA entry inside a config extent is the normal MSVC layout for a
 * function with an SEH funclet, so only discovery carving an entry point out of a declared range
 * warns.
 *
 * @param ctx Codegen context.
 */
void validateConfigBoundaries(CodegenContext& ctx) {
  auto& graph = ctx.graph;

  for (const auto& [addr, fc] : ctx.Config().functions) {
    if (fc.isChunk())
      continue;

    const FunctionNode* node = graph.getFunction(addr);
    if (!node) {
      REXLOG_WARN(codegen, "functions 0x{:08X} is not a function entry point", addr);
      continue;
    }
    if (node->authority() != FunctionAuthority::CONFIG) {
      REXLOG_WARN(codegen, "functions 0x{:08X} outranked by {}", addr,
                  AuthorityName(node->authority()));
      continue;
    }

    // Use the declared size, since discover() grows node->size() to cover its blocks.
    u32 declared = fc.getSize(addr);
    if (declared == 0)
      continue;

    for (const auto& [otherAddr, other] : graph.functions()) {
      if (otherAddr <= addr || otherAddr >= addr + declared)
        continue;
      auto authority = other->authority();
      if (authority == FunctionAuthority::PDATA || authority == FunctionAuthority::HELPER) {
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
 * Validates that every direct b and bl target resolves. A target inside another function counts
 * as resolved, as gap fill can produce, and is emitted as a tail call to that function's internal
 * label. A target in no function, or a call with no registered edge, stops the build.
 *
 * @param ctx Codegen context.
 * @return Ok, or a validation error.
 */
[[nodiscard]] Result<void> validateGraph(CodegenContext& ctx) {
  REXLOG_TRACE(codegen, "Analyze: validating call graph...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  auto& errors = ctx.errors;

  validateConfigBoundaries(ctx);

  size_t functionsChecked = 0;
  size_t callsChecked = 0;
  size_t edgesVerified = 0;

  for (const auto& [addr, node] : graph.functions()) {
    functionsChecked++;

    for (const auto& block : node->blocks()) {
      const u8* data = binary.translate(block.base);
      if (!data)
        continue;

      for (size_t offset = 0; offset < block.size; offset += 4) {
        u32 insn = memory::load<be_u32>(data + offset);

        if (PPC_OP(insn) == PPC_OP_B && !PPC_BA(insn)) {
          u32 site = block.base + static_cast<u32>(offset);
          i32 branchOffset = PPC_BI(insn);
          u32 target = site + branchOffset;
          bool isCall = PPC_BL(insn);

          callsChecked++;

          bool targetExists = false;
          bool isInternalJump = false;

          if (node->containsAddress(target)) {
            isInternalJump = true;
            targetExists = true;
          }

          if (!targetExists && node->isWithinBounds(target)) {
            isInternalJump = true;
            targetExists = true;
          }

          if (!targetExists && graph.isEntryPoint(target)) {
            targetExists = true;
          }

          if (!targetExists && graph.isImport(target)) {
            targetExists = true;
          }

          if (!targetExists) {
            const FunctionNode* containingFunc = graph.getFunctionContaining(target);
            if (containingFunc) {
              targetExists = true;
            }
          }

          if (!targetExists) {
            errors.Add(AnalysisErrors::Category::UnresolvedCall, target, site,
                       std::format("{} 0x{:08X} from 0x{:08X} - target not in any function",
                                   isCall ? "bl" : "b", target, site));
            continue;
          }

          if (!isInternalJump) {
            const CallEdge* edge = findCallEdgeAt(node.get(), site);
            if (!edge) {
              const FunctionNode* containingFunc = graph.getFunctionContaining(target);
              if (!containingFunc) {
                errors.Add(AnalysisErrors::Category::UnresolvedCall, target, site,
                           std::format("{} 0x{:08X} from 0x{:08X} in {} - no CallEdge recorded",
                                       isCall ? "bl" : "b", target, site, node->name()));
              }
            } else {
              edgesVerified++;
            }
          }
        }
      }
    }
  }

  REXLOG_TRACE(codegen, "Analyze: checked {} branches in {} functions, verified {} edges",
               callsChecked, functionsChecked, edgesVerified);

  if (errors.HasErrors()) {
    REXLOG_ERROR(codegen, "Analyze: found {} errors", errors.Count());
    errors.PrintReport();
    return Err(ErrorCategory::Validation,
               std::format("Validation failed: {} unresolved calls",
                           errors.Count(AnalysisErrors::Category::UnresolvedCall)));
  }

  REXLOG_TRACE(codegen, "Analyze: all calls resolve");
  return Ok();
}

}  // anonymous namespace

namespace phases {

[[nodiscard]] Result<void> Validate(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  return validateGraph(ctx);
}

}  // namespace phases

}  // namespace rex::codegen
