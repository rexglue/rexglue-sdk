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

#include <rex/memory/byte_swap.h>

namespace rex::codegen {

namespace {

/**
 * Resolves jumps then seals functions.
 *
 * @param ctx Codegen context.
 */
void MergeAndSeal(CodegenContext& ctx) {
  REXLOG_TRACE(codegen, "Analyze: resolving jumps and sealing functions...");

  auto& graph = ctx.graph;
  auto& binary = ctx.GetBinary();

  graph.SetMemoryReader([&binary](u32 addr) -> std::optional<u32> {
    auto* section = binary.FindSection(addr);
    if (!section || !section->data) {
      return std::nullopt;
    }
    u32 offset = addr - section->base_address;
    if (offset + 4 > section->size) {
      return std::nullopt;
    }
    return memory::load<be_u32>(section->data + offset);
  });

  size_t iteration = 0;
  size_t total_resolved = 0;
  const size_t max_resolve_iterations = ctx.GetLimits().max_resolve_iterations;

  while (iteration < max_resolve_iterations) {
    iteration++;
    size_t changes_this_iteration = 0;

    std::vector<u32> pending_addrs;
    for (const auto* node : graph.GetPendingFunctions()) {
      pending_addrs.push_back(node->GetBase());
    }

    for (u32 func_addr : pending_addrs) {
      size_t resolved = graph.TryResolveFunction(func_addr);
      changes_this_iteration += resolved;
      total_resolved += resolved;
    }

    if (changes_this_iteration == 0)
      break;
  }

  size_t shared_regs = graph.MarkFuncletRegisterSharing();
  if (shared_regs > 0) {
    REXLOG_DEBUG(codegen, "Analyze: {} functions share registers with an SEH funclet", shared_regs);
  }

  size_t total_sealed = graph.SealAllReady();
  size_t still_pending = graph.GetPendingCount();

  REXLOG_TRACE(codegen, "Analyze: {} iterations, resolved={}, sealed={}/{}", iteration,
               total_resolved, total_sealed, graph.GetFunctionCount());

  if (still_pending > 0) {
    REXLOG_DEBUG(codegen, "Analyze: {} functions still PENDING with unresolved jumps",
                 still_pending);
  }
}

}  // anonymous namespace

namespace detail {

[[nodiscard]] Result<void> Merge(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  MergeAndSeal(ctx);
  return Ok();
}

}  // namespace detail

}  // namespace rex::codegen
