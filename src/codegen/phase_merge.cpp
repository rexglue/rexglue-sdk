/**
 * @file        codegen/phase_merge.cpp
 * @brief       Merge phase: resolve jumps and seal functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/phases.h>

#include <rex/logging.h>

#include <rex/memory/utils.h>

using rex::memory::load_and_swap;

namespace rex::codegen {

namespace {

//=============================================================================
// Merge to resolve jumps then seal functions
//=============================================================================
void mergeAndSeal(CodegenContext& ctx) {
  REXLOG_TRACE(codegen, "Analyze: resolving jumps and sealing functions...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();

  graph.setMemoryReader([&binary](uint32_t addr) -> std::optional<uint32_t> {
    auto* section = binary.findSection(addr);
    if (!section || !section->data) {
      return std::nullopt;
    }
    uint32_t offset = addr - section->baseAddress;
    if (offset + 4 > section->size) {
      return std::nullopt;
    }
    return load_and_swap<uint32_t>(section->data + offset);
  });

  size_t iteration = 0;
  size_t totalResolved = 0;
  const size_t maxResolveIterations = ctx.limits().max_resolve_iterations;

  while (iteration < maxResolveIterations) {
    iteration++;
    size_t changesThisIteration = 0;

    std::vector<uint32_t> pendingAddrs;
    for (const auto* node : graph.getPendingFunctions()) {
      pendingAddrs.push_back(node->base());
    }

    for (uint32_t funcAddr : pendingAddrs) {
      size_t resolved = graph.tryResolveFunction(funcAddr);
      changesThisIteration += resolved;
      totalResolved += resolved;
    }

    if (changesThisIteration == 0)
      break;
  }

  size_t sharedRegs = graph.markFuncletRegisterSharing();
  if (sharedRegs > 0) {
    REXLOG_DEBUG(codegen, "Analyze: {} functions share registers with an SEH funclet", sharedRegs);
  }

  size_t totalSealed = graph.sealAllReady();
  size_t stillPending = graph.pendingCount();

  REXLOG_TRACE(codegen, "Analyze: {} iterations, resolved={}, sealed={}/{}", iteration,
               totalResolved, totalSealed, graph.functionCount());

  if (stillPending > 0) {
    REXLOG_DEBUG(codegen, "Analyze: {} functions still PENDING with unresolved jumps",
                 stillPending);
  }
}

}  // anonymous namespace

namespace phases {

Result<void> Merge(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  mergeAndSeal(ctx);
  return Ok();
}

}  // namespace phases

}  // namespace rex::codegen
