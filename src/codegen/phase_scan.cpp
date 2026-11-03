/**
 * @file        codegen/phase_scan.cpp
 * @brief       Scan phase that segments the binary into code and data regions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <unordered_set>

#include "pipeline.h"

#include "config.h"
#include <rex/logging/log.h>
#include "codegen_categories.h"

#include <rex/memory/endian.h>

namespace rex::codegen {

namespace {

std::vector<CodeRegion> SegmentSection(const SectionView& section,
                                       const std::unordered_set<u32>& exception_handler_funcs,
                                       u32 export_table) {
  std::vector<CodeRegion> regions;

  const u8* data = section.data;
  const u8* data_end = section.data + section.size;

  if (export_table && export_table >= section.base_address &&
      export_table < section.base_address + section.size) {
    data_end = section.data + (export_table - section.base_address);
  }

  u32 region_start = 0;
  bool in_code = false;

  while (data < data_end) {
    u32 addr = section.base_address + static_cast<u32>(data - section.data);
    u32 word = memory::load<be_u32>(data);

    if (word == 0x00000000) {
      if (in_code) {
        regions.push_back({region_start, addr});
        in_code = false;
      }

      if (data + 12 <= data_end) {
        u32 next_word = memory::load<be_u32>(data + 4);
        if (exception_handler_funcs.contains(next_word)) {
          data += 12;
          continue;
        }
      }
      data += 4;
      continue;
    }

    if (!in_code) {
      region_start = addr;
      in_code = true;
    }
    data += 4;
  }

  if (in_code) {
    u32 end_addr = section.base_address + static_cast<u32>(data_end - section.data);
    regions.push_back({region_start, end_addr});
  }

  return regions;
}

void ScanBinary(CodegenContext& ctx) {
  REXLOG_TRACE(codegen, "Analyze: scanning binary...");

  auto& binary = ctx.GetBinary();
  auto& config = ctx.GetConfig();
  auto& state = ctx.GetAnalysisState();
  auto& scan = ctx.scan;
  auto& graph = ctx.graph;

  u32 export_table = binary.GetExportTableAddress();

  std::unordered_set<u32> exception_handler_funcs;
  for (u32 addr : state.exception_handler_funcs) {
    exception_handler_funcs.insert(addr);
  }

  for (const auto& [addr, node] : graph.GetFunctions()) {
    if (node->GetAuthority() != FunctionAuthority::kImport)
      continue;
    if (node->GetName() == "__imp____C_specific_handler") {
      exception_handler_funcs.insert(addr);
      break;
    }
  }

  for (const auto& section : binary.GetSections()) {
    if (!section.executable)
      continue;

    auto regions = SegmentSection(section, exception_handler_funcs, export_table);
    scan.code_regions.insert(scan.code_regions.end(), regions.begin(), regions.end());
  }

  REXLOG_TRACE(codegen, "Analyze: segmented into {} code regions", scan.code_regions.size());

  for (const auto& section : binary.GetSections()) {
    if (!section.executable)
      continue;

    const u8* data = section.data;
    const u8* data_end = section.data + section.size;

    if (export_table && export_table >= section.base_address &&
        export_table < section.base_address + section.size) {
      data_end = section.data + (export_table - section.base_address);
    }

    u32 consecutive_invalid = 0;
    u32 data_region_start = 0;

    while (data < data_end) {
      u32 addr = section.base_address + static_cast<u32>(data - section.data);
      u32 insn = memory::load<be_u32>(data);

      bool is_invalid = (insn == 0x00000000 || insn == 0xFFFFFFFF);
      if (is_invalid) {
        if (consecutive_invalid == 0)
          data_region_start = addr;
        consecutive_invalid++;
      } else {
        if (consecutive_invalid >= config.data_region_threshold) {
          scan.data_regions.emplace_back(data_region_start, addr);
        }
        consecutive_invalid = 0;
      }

      data += 4;
    }

    if (consecutive_invalid >= config.data_region_threshold) {
      u32 end_addr = section.base_address + static_cast<u32>(data_end - section.data);
      scan.data_regions.emplace_back(data_region_start, end_addr);
    }
  }

  REXLOG_TRACE(codegen, "Analyze: {} code regions, {} data regions", scan.code_regions.size(),
               scan.data_regions.size());
}

}  // anonymous namespace

namespace detail {

[[nodiscard]] Result<void> ScanRegions(CodegenContext& ctx, IProgressReporter* reporter) {
  (void)reporter;
  ScanBinary(ctx);
  return Ok();
}

}  // namespace detail

}  // namespace rex::codegen
