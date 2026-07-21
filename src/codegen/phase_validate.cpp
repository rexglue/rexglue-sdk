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
#include "phase_helpers.h"
#include <rex/memory/utils.h>

#include <set>

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

struct BranchInfo {
  uint32_t target;
  bool isCall;
};

// Decode b/bl (I-form) and bc-family (B-form) relative branches.
std::optional<BranchInfo> decodeDirectBranch(uint32_t insn, uint32_t site) {
  uint32_t op = PPC_OP(insn);
  if (op != PPC_OP_B && op != PPC_OP_BC)
    return std::nullopt;
  if (PPC_BA(insn))
    return std::nullopt;
  int32_t branchOffset = (op == PPC_OP_B) ? PPC_BI(insn) : PPC_BD(insn);
  return BranchInfo{site + static_cast<uint32_t>(branchOffset),
                    static_cast<bool>(PPC_BL(insn))};
}

// Resolve unresolved jumps and seal whatever became sealable. Mirrors the
// Merge phase for functions promoted during validation.
void resolveAndSealPending(CodegenContext& ctx) {
  auto& graph = ctx.graph;
  while (true) {
    size_t resolved = 0;
    std::vector<uint32_t> pendingAddrs;
    for (const auto* node : graph.getPendingFunctions()) {
      pendingAddrs.push_back(node->base());
    }
    for (uint32_t funcAddr : pendingAddrs) {
      resolved += graph.tryResolveFunction(funcAddr);
    }
    if (resolved == 0)
      break;
  }
  graph.sealAllReady();
}

VoidResult validateGraph(CodegenContext& ctx) {
  REXCODEGEN_TRACE("Analyze: validating call graph...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  auto& errors = ctx.errors;

  // A branch into another function's interior (a shared tail) cannot be
  // emitted as a goto. Promote such targets to standalone functions so the
  // shared code becomes directly callable (it ends in blr and returns), then
  // re-validate. Bounded because each pass only re-runs when it promoted
  // something new.
  constexpr int kMaxPromotePasses = 8;

  struct PendingError {
    uint32_t target;
    uint32_t site;
    std::string message;
  };

  for (int pass = 0;; ++pass) {
    size_t functionsChecked = 0;
    size_t callsChecked = 0;
    size_t edgesVerified = 0;
    std::vector<PendingError> pendingErrors;
    std::set<uint32_t> promoteTargets;

    for (const auto& [addr, node] : graph.functions()) {
      functionsChecked++;

      for (const auto& block : node->blocks()) {
        const uint8_t* data = binary.translate(block.base);
        if (!data)
          continue;

        for (size_t offset = 0; offset < block.size; offset += 4) {
          uint32_t insn = load_and_swap<uint32_t>(data + offset);
          uint32_t site = block.base + static_cast<uint32_t>(offset);
          auto branch = decodeDirectBranch(insn, site);
          if (!branch.has_value())
            continue;

          uint32_t target = branch->target;
          bool isCall = branch->isCall;

          callsChecked++;

          // A distinct entry point (including imports and promoted shared
          // functions) wins over containment: codegen classifies these as
          // direct calls, so the site must carry a call edge pointing at the
          // node currently registered at the target. A branch to the
          // function's own entry stays internal (loop back / recursion, which
          // discovery already resolved).
          FunctionNode* targetFn = graph.getFunction(target);
          if (targetFn && target != node->base()) {
            const CallEdge* edge = findCallEdgeAt(node.get(), site);
            if (!edge) {
              if (isCall) {
                graph.addCallToFunction(node->base(), site, CallTarget::function(targetFn));
              } else {
                graph.addTailCallToFunction(node->base(), site, CallTarget::function(targetFn));
              }
            } else {
              // The edge may be stale: it can point at a node that was
              // removed (absorbed GAP_FILL) or replaced after the edge was
              // recorded, which makes codegen call the wrong function. Check
              // identity against the live node and re-point if needed.
              bool edgeValid = false;
              if (const auto* fn = std::get_if<CallTarget::ToFunction>(&edge->target.value)) {
                edgeValid = fn->node == targetFn;
              } else if (const auto* imp = std::get_if<CallTarget::ToImport>(&edge->target.value)) {
                edgeValid = imp->address == target;
              }
              if (!edgeValid) {
                REXCODEGEN_DEBUG("Analyze: repairing stale call edge at 0x{:08X} in {} -> {}",
                                 site, node->name(), targetFn->name());
                graph.retargetCallEdgesAt(node->base(), site, CallTarget::function(targetFn));
              }
            }
            edgesVerified++;
            continue;
          }

          // Internal jump within this function's blocks or bounds
          if (node->containsAddress(target) || node->isWithinBounds(target))
            continue;

          // Target is inside another function (shared tail) or not covered by
          // any function: promote it to a standalone function and re-validate.
          if (!binary.isInImportExportRange(target)) {
            promoteTargets.insert(target);
          }

          const FunctionNode* containingFunc = graph.getFunctionContaining(target);
          if (containingFunc) {
            pendingErrors.push_back(
                {target, site,
                 fmt::format("{} 0x{:08X} from 0x{:08X} enters interior of {} at 0x{:08X}",
                             isCall ? "bl" : "b", target, site, containingFunc->name(),
                             containingFunc->base())});
          } else {
            pendingErrors.push_back(
                {target, site,
                 fmt::format("{} 0x{:08X} from 0x{:08X} - target not in any function",
                             isCall ? "bl" : "b", target, site)});
          }
        }
      }
    }

    REXCODEGEN_TRACE("Analyze: checked {} branches in {} functions, verified {} edges (pass {})",
                     callsChecked, functionsChecked, edgesVerified, pass + 1);

    if (!promoteTargets.empty() && pass < kMaxPromotePasses) {
      REXCODEGEN_INFO("Analyze: promoting {} shared branch targets to functions and re-validating",
                      promoteTargets.size());
      for (uint32_t target : promoteTargets) {
        graph.addFunction(target, 4, FunctionAuthority::DISCOVERED, true);
      }

      auto known = buildKnownFunctions(graph);
      discoverPendingFunctions(ctx, known);
      resolveAndSealPending(ctx);
      continue;
    }

    for (const auto& pending : pendingErrors) {
      errors.Add(AnalysisErrors::Category::UnresolvedCall, pending.target, pending.site,
                 pending.message);
    }

    if (errors.HasErrors()) {
      REXCODEGEN_ERROR("Analyze: found {} errors", errors.Count());
      errors.PrintReport();
      return Err(ErrorCategory::Validation,
                 fmt::format("Validation failed: {} unresolved calls",
                             errors.Count(AnalysisErrors::Category::UnresolvedCall)));
    }

    REXCODEGEN_TRACE("Analyze: all calls resolve");
    return Ok();
  }
}

}  // anonymous namespace

namespace phases {

VoidResult Validate(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  return validateGraph(ctx);
}

}  // namespace phases

}  // namespace rex::codegen
