/**
 * @file        codegen/phase_scan.cpp
 * @brief       Scan phase: segment binary into code/data regions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <unordered_set>

#include <rex/codegen/phases.h>

#include <rex/codegen/config.h>
#include <rex/logging.h>

#include "codegen_logging.h"
#include <rex/memory/utils.h>

using rex::memory::load_and_swap;

namespace rex::codegen {

namespace {

//=============================================================================
// Scan Phase: segment binary into code/data regions
//=============================================================================

std::vector<CodeRegion> segmentSection(const SectionView& section,
                                       const std::unordered_set<uint32_t>& exceptionHandlerFuncs,
                                       uint32_t exportTable) {
  std::vector<CodeRegion> regions;

  const uint8_t* data = section.data;
  const uint8_t* dataEnd = section.data + section.size;

  if (exportTable && exportTable >= section.baseAddress &&
      exportTable < section.baseAddress + section.size) {
    dataEnd = section.data + (exportTable - section.baseAddress);
  }

  uint32_t regionStart = 0;
  bool inCode = false;

  while (data < dataEnd) {
    uint32_t addr = section.baseAddress + static_cast<uint32_t>(data - section.data);
    uint32_t word = load_and_swap<uint32_t>(data);

    if (word == 0x00000000) {
      if (inCode) {
        regions.push_back({regionStart, addr});
        inCode = false;
      }

      if (data + 12 <= dataEnd) {
        uint32_t nextWord = load_and_swap<uint32_t>(data + 4);
        if (exceptionHandlerFuncs.contains(nextWord)) {
          data += 12;
          continue;
        }
      }
      data += 4;
      continue;
    }

    if (!inCode) {
      regionStart = addr;
      inCode = true;
    }
    data += 4;
  }

  if (inCode) {
    uint32_t endAddr = section.baseAddress + static_cast<uint32_t>(dataEnd - section.data);
    regions.push_back({regionStart, endAddr});
  }

  return regions;
}

void scanBinary(CodegenContext& ctx) {
  REXCODEGEN_INFO("Analyze: scanning binary...");

  auto& binary = ctx.binary();
  auto& config = ctx.Config();
  auto& state = ctx.analysisState();
  auto& scan = ctx.scan;
  auto& graph = ctx.graph;

  uint32_t exportTable = binary.exportTableAddr();

  // Build exception handler function set
  std::unordered_set<uint32_t> exceptionHandlerFuncs;
  for (uint32_t addr : state.exceptionHandlerFuncs) {
    exceptionHandlerFuncs.insert(addr);
  }

  for (const auto& [addr, node] : graph.functions()) {
    if (node->authority() != FunctionAuthority::IMPORT)
      continue;
    if (node->name() == "__imp____C_specific_handler") {
      exceptionHandlerFuncs.insert(addr);
      break;
    }
  }

  // Segment executable sections
  for (const auto& section : binary.sections()) {
    if (!section.executable)
      continue;

    auto regions = segmentSection(section, exceptionHandlerFuncs, exportTable);
    scan.codeRegions.insert(scan.codeRegions.end(), regions.begin(), regions.end());
  }

  REXCODEGEN_INFO("Analyze: segmented into {} code regions", scan.codeRegions.size());

  // Detect data regions
  for (const auto& section : binary.sections()) {
    if (!section.executable)
      continue;

    const uint8_t* data = section.data;
    const uint8_t* dataEnd = section.data + section.size;

    if (exportTable && exportTable >= section.baseAddress &&
        exportTable < section.baseAddress + section.size) {
      dataEnd = section.data + (exportTable - section.baseAddress);
    }

    uint32_t consecutiveInvalid = 0;
    uint32_t dataRegionStart = 0;

    while (data < dataEnd) {
      uint32_t addr = section.baseAddress + static_cast<uint32_t>(data - section.data);
      uint32_t insn = load_and_swap<uint32_t>(data);

      bool isInvalid = (insn == 0x00000000 || insn == 0xFFFFFFFF);
      if (isInvalid) {
        if (consecutiveInvalid == 0)
          dataRegionStart = addr;
        consecutiveInvalid++;
      } else {
        if (consecutiveInvalid >= config.dataRegionThreshold) {
          scan.dataRegions.emplace_back(dataRegionStart, addr);
        }
        consecutiveInvalid = 0;
      }

      data += 4;
    }

    if (consecutiveInvalid >= config.dataRegionThreshold) {
      uint32_t endAddr = section.baseAddress + static_cast<uint32_t>(dataEnd - section.data);
      scan.dataRegions.emplace_back(dataRegionStart, endAddr);
    }
  }

  REXCODEGEN_INFO("Analyze: {} code regions, {} data regions", scan.codeRegions.size(),
                  scan.dataRegions.size());
}

}  // anonymous namespace

namespace phases {

VoidResult Scan(CodegenContext& ctx) {
  scanBinary(ctx);
  return Ok();
}

}  // namespace phases

}  // namespace rex::codegen
