/**
 * @file        codegen/phase_live_ins.cpp
 * @brief       Functions that read non volatile registers their caller set
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "pipeline.h"

#include <algorithm>
#include <format>
#include <string>
#include <unordered_set>
#include <vector>

#include "builder/builder_context.h"
#include "call_usage.h"
#include "codegen_categories.h"
#include "config.h"
#include <rex/logging/log_category.h>

namespace rex::codegen::detail {

namespace {

/** Appends the set registers of one class as "r14-r16 r31". */
template <typename IsSet>
void AppendRegisters(std::string& out, char prefix, u32 count, IsSet is_set) {
  for (u32 first = 0; first < count; ++first) {
    if (!is_set(first)) {
      continue;
    }
    u32 last = first;
    while (last + 1 < count && is_set(last + 1)) {
      ++last;
    }
    if (!out.empty()) {
      out += ' ';
    }
    out += last == first ? std::format("{}{}", prefix, first)
                         : std::format("{}{}-{}{}", prefix, first, prefix, last);
    first = last;
  }
}

std::string RegisterList(const NonVolatileLiveIns& live) {
  std::string out;
  AppendRegisters(out, 'r', 32, [&](u32 n) { return (live.gpr >> n) & 1; });
  AppendRegisters(out, 'f', 32, [&](u32 n) { return (live.fpr >> n) & 1; });
  AppendRegisters(out, 'v', 128, [&](u32 n) { return live.vr.test(n); });
  return out;
}

}  // namespace

void ShareNonVolatileLiveIns(CodegenContext& ctx) {
  const RecompilerConfig& config = ctx.GetConfig();
  if (!config.non_volatile_registers_as_local_variables) {
    return;
  }

  // setjmp and longjmp read every non volatile register on purpose, to save and restore it, and
  // codegen writes calls to them as ppc_setjmp and ppc_longjmp: like the millicode helpers, a
  // call to them reads nothing here.
  std::vector<const FunctionNode*> functions;
  std::unordered_set<u32> opaque;
  for (const u32 address : {config.setjmp_address, config.longjmp_address}) {
    if (address != 0) {
      opaque.insert(address);
    }
  }
  for (const auto& [address, node] : ctx.graph.GetFunctions()) {
    if (node->GetMillicode()) {
      opaque.insert(address);
    }
    if (node->GetAuthority() != FunctionAuthority::kImport) {
      functions.push_back(node.get());
    }
  }
  std::ranges::sort(functions, {}, &FunctionNode::GetBase);

  const auto live_ins = ComputeNonVolatileLiveIns(functions, opaque);
  std::vector<std::pair<u32, NonVolatileLiveIns>> found;
  for (const FunctionNode* fn : functions) {
    const auto live = live_ins.find(fn->GetBase());
    if (live != live_ins.end() && !opaque.contains(fn->GetBase()) &&
        LocalizesNonVolatiles(*fn, config)) {
      found.emplace_back(fn->GetBase(), live->second);
    }
  }
  if (found.empty()) {
    return;
  }

  if (config.share_non_volatile_live_ins) {
    for (const auto& [address, live] : found) {
      ctx.graph.GetFunction(address)->SetSharesRegisters(true);
      REXCG_DEBUG("0x{:08X} reads {} from its caller, given share_registers", address,
                  RegisterList(live));
    }
    REXCG_INFO(
        "{} functions read non volatile registers their caller set and share its registers "
        "(share_non_volatile_live_ins)",
        found.size());
    return;
  }

  for (const auto& [address, live] : found) {
    REXCG_WARN("0x{:08X} reads {} before it writes them", address, RegisterList(live));
  }
  REXCG_WARN(
      "{} functions read non volatile registers before they write them. These are values their "
      "caller set, but with non_volatile_as_local each function has its own copy, so they read "
      "zero. Give them share_registers = true in [functions], or set share_non_volatile_live_ins "
      "= true",
      found.size());
}

}  // namespace rex::codegen::detail
