/**
 * @file        codegen/analyze.cpp
 * @brief       Analysis pipeline and code emission
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "decoded_binary.h"
#include "discovery.h"
#include "ppc/instruction.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/codegen/analysis_errors.h>
#include <rex/codegen/analyze.h>
#include <rex/codegen/config.h>
#include <rex/codegen/vtable_scanner.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/export_resolver.h>
#include <rex/types.h>

#include <ppc.h>

using rex::codegen::ppc::decode_instruction;
using rex::codegen::ppc::Opcode;
using rex::memory::load_and_swap;

namespace rex::codegen {

namespace {

//=============================================================================
// Helpers
//=============================================================================

/// Build set of non-helper function entry points for boundary detection.
/// @param graph The function graph to scan
/// @param excludeGapFill If true, also exclude GAP_FILL authority functions
std::unordered_set<uint32_t> buildKnownFunctions(const FunctionGraph& graph,
                                                 bool excludeGapFill = false) {
  std::unordered_set<uint32_t> result;
  for (const auto& [addr, node] : graph.functions()) {
    auto auth = node->authority();
    if (auth == FunctionAuthority::HELPER)
      continue;
    if (excludeGapFill && auth == FunctionAuthority::GAP_FILL)
      continue;
    result.insert(addr);
  }
  return result;
}

// Forward declaration (defined later in the file)
void discoverFunction(CodegenContext& ctx, uint32_t funcAddr,
                      const std::unordered_set<uint32_t>& knownFunctions);

/// Collect all functions awaiting discovery and discover their blocks.
/// @return Number of functions discovered
size_t discoverPendingFunctions(CodegenContext& ctx,
                                const std::unordered_set<uint32_t>& knownFunctions) {
  std::vector<uint32_t> pending;
  for (const auto& [addr, node] : ctx.graph.functions()) {
    if (node->canDiscover()) {
      pending.push_back(addr);
    }
  }
  for (uint32_t funcAddr : pending) {
    discoverFunction(ctx, funcAddr, knownFunctions);
  }
  return pending.size();
}

//=============================================================================
// PE Structures
//=============================================================================

#pragma pack(push, 1)
struct IMAGE_CE_RUNTIME_FUNCTION {
  uint32_t BeginAddress;
  union {
    uint32_t Data;
    struct {
      uint32_t PrologLength : 8;
      uint32_t FunctionLength : 22;
      uint32_t ThirtyTwoBit : 1;
      uint32_t ExceptionFlag : 1;
    };
  };
};
#pragma pack(pop)

//=============================================================================
// Exception Info Parsing
//=============================================================================

struct ParsedExceptionInfo {
  ExceptionInfo info;
  uint32_t maxAddress;
  std::vector<uint32_t> discoveredFuncs;
};

// Prolog info extracted from function prologue for SEH unwinding
struct PrologInfo {
  uint32_t frameSize = 0;   // From addi r31, r1, -N
  uint32_t saveHelper = 0;  // From bl __savegprlr_N
  bool valid = false;       // True if frame size was successfully detected
};

// Scan function prolog to extract frame size and save helper
// Only called for functions with ExceptionFlag set
PrologInfo scanProlog(const BinaryView& binary, uint32_t funcAddr, uint32_t prologLength) {
  PrologInfo info;

  auto* section = binary.findSection(funcAddr);
  if (!section || !section->data) {
    return info;
  }

  uint32_t offset = funcAddr - section->baseAddress;
  if (offset + prologLength * 4 > section->size) {
    return info;
  }

  const uint8_t* code = section->data + offset;

  // Use pdata prolog length - if 0, we can't safely scan
  if (prologLength == 0) {
    REXCODEGEN_WARN("SEH function 0x{:08X}: PrologLength=0, cannot determine frame size", funcAddr);
    return info;
  }

  for (uint32_t i = 0; i < prologLength; i++) {
    uint32_t raw = load_and_swap<uint32_t>(code + i * 4);
    uint32_t addr = funcAddr + i * 4;
    auto decoded = decode_instruction(addr, raw);

    // Check for addi r31, r1, -N (frame pointer setup)
    if (decoded.opcode == Opcode::addi && decoded.D.RT == 31 && decoded.D.RA == 1) {
      int16_t simm = decoded.D.SIMM();
      if (simm < 0) {
        info.frameSize = static_cast<uint32_t>(-simm);
        info.valid = true;
      }
    }

    // Check for bl - save helper call
    if (decoded.is_call() && decoded.branch_target.has_value()) {
      info.saveHelper = decoded.branch_target.value();
    }
  }

  // NOTE(tomc): info.valid may be false for handler functions that receive frame in r12
  // The caller should warn only if frameSize is actually needed (i.e., function has SEH scopes)
  return info;
}

std::optional<ParsedExceptionInfo> parseSehScopeTable(uint32_t handlerThunk,
                                                      uint32_t scopeTableAddr, uint32_t count,
                                                      const uint8_t* rdataBase, uint32_t rdataStart,
                                                      uint32_t rdataSize,
                                                      uint32_t functionBeginAddr,
                                                      std::vector<uint32_t>& discoveredFuncs) {
  uint32_t tableOffset = scopeTableAddr - rdataStart;
  if (tableOffset + 4 + count * 16 > rdataSize) {
    return std::nullopt;
  }

  SehExceptionInfo sehInfo;
  sehInfo.handlerThunk = handlerThunk;
  sehInfo.scopeTableAddr = scopeTableAddr;

  uint32_t maxAddr = functionBeginAddr;

  const uint32_t* entriesData = reinterpret_cast<const uint32_t*>(rdataBase + tableOffset + 4);
  for (uint32_t i = 0; i < count; i++) {
    SehScope scope;
    scope.tryStart = byte_swap(entriesData[i * 4 + 0]);
    scope.tryEnd = byte_swap(entriesData[i * 4 + 1]);
    scope.filter = byte_swap(entriesData[i * 4 + 2]);
    scope.handler = byte_swap(entriesData[i * 4 + 3]);

    // __finally has layout [2]=handler, [3]=0; __except has [2]=filter, [3]=handler
    if (scope.handler == 0 && scope.filter != 0) {
      scope.handler = scope.filter;
      scope.filter = 0;
    }

    sehInfo.scopes.push_back(scope);

    if (scope.tryStart != 0 && scope.tryStart > maxAddr)
      maxAddr = scope.tryStart;
    if (scope.tryEnd != 0 && scope.tryEnd > maxAddr)
      maxAddr = scope.tryEnd;
    if (scope.filter != 0 && scope.filter > maxAddr)
      maxAddr = scope.filter;
    if (scope.handler != 0 && scope.handler > maxAddr)
      maxAddr = scope.handler;

    // For __finally (filter=0), handler is a separate function
    // For __except (filter!=0), only filter is a separate function (handler is inline)
    if (scope.filter == 0 && scope.handler != 0) {
      discoveredFuncs.push_back(scope.handler);  // __finally handler
    }
    if (scope.filter != 0) {
      discoveredFuncs.push_back(scope.filter);  // __except filter
    }
  }

  discoveredFuncs.push_back(handlerThunk);

  ParsedExceptionInfo result;
  result.info.data = std::move(sehInfo);
  result.maxAddress = maxAddr;
  result.discoveredFuncs = std::move(discoveredFuncs);
  return result;
}

std::optional<ParsedExceptionInfo> parseCxxFuncInfo(uint32_t handlerThunk, uint32_t funcInfoAddr,
                                                    const uint8_t* rdataBase, uint32_t rdataStart,
                                                    uint32_t rdataSize, uint32_t functionBeginAddr,
                                                    std::vector<uint32_t>& discoveredFuncs) {
  uint32_t tableOffset = funcInfoAddr - rdataStart;
  if (tableOffset + 28 > rdataSize) {
    return std::nullopt;
  }

  auto readU32 = [&](uint32_t addr) {
    return load_and_swap<uint32_t>(rdataBase + (addr - rdataStart));
  };
  auto readI32 = [&](uint32_t addr) {
    return static_cast<int32_t>(load_and_swap<uint32_t>(rdataBase + (addr - rdataStart)));
  };

  CxxExceptionInfo cxxInfo;
  cxxInfo.handlerThunk = handlerThunk;
  cxxInfo.funcInfoAddr = funcInfoAddr;

  uint32_t magic = readU32(funcInfoAddr + 0);
  cxxInfo.maxState = readU32(funcInfoAddr + 4);
  uint32_t pUnwindMap = readU32(funcInfoAddr + 8);
  uint32_t nTryBlocks = readU32(funcInfoAddr + 12);
  uint32_t pTryBlockMap = readU32(funcInfoAddr + 16);
  uint32_t nIPMapEntries = readU32(funcInfoAddr + 20);
  uint32_t pIPtoStateMap = readU32(funcInfoAddr + 24);

  if (magic != CXX_EH_MAGIC) {
    return std::nullopt;
  }

  if (cxxInfo.maxState > 100 || nTryBlocks > 50 || nIPMapEntries > 200) {
    return std::nullopt;
  }

  uint32_t maxAddr = functionBeginAddr;
  discoveredFuncs.push_back(handlerThunk);

  // Parse UnwindMap
  if (pUnwindMap != 0 && cxxInfo.maxState > 0) {
    if (pUnwindMap >= rdataStart && pUnwindMap + cxxInfo.maxState * 8 <= rdataStart + rdataSize) {
      for (uint32_t i = 0; i < cxxInfo.maxState; i++) {
        CxxUnwindEntry entry;
        entry.toState = readI32(pUnwindMap + i * 8);
        entry.action = readU32(pUnwindMap + i * 8 + 4);
        cxxInfo.unwindMap.push_back(entry);

        if (entry.action != 0) {
          discoveredFuncs.push_back(entry.action);
        }
      }
    }
  }

  // Parse TryBlockMap
  if (pTryBlockMap != 0 && nTryBlocks > 0) {
    if (pTryBlockMap >= rdataStart && pTryBlockMap + nTryBlocks * 20 <= rdataStart + rdataSize) {
      for (uint32_t i = 0; i < nTryBlocks; i++) {
        uint32_t entryAddr = pTryBlockMap + i * 20;
        CxxTryBlock tryBlock;
        tryBlock.tryLow = readI32(entryAddr + 0);
        tryBlock.tryHigh = readI32(entryAddr + 4);
        tryBlock.catchHigh = readI32(entryAddr + 8);
        uint32_t nCatches = readU32(entryAddr + 12);
        uint32_t pHandlers = readU32(entryAddr + 16);

        if (pHandlers != 0 && nCatches > 0 && nCatches <= 20) {
          if (pHandlers >= rdataStart && pHandlers + nCatches * 16 <= rdataStart + rdataSize) {
            for (uint32_t j = 0; j < nCatches; j++) {
              uint32_t hAddr = pHandlers + j * 16;
              CxxCatchHandler handler;
              handler.adjectives = readU32(hAddr + 0);
              handler.typeDescriptor = readU32(hAddr + 4);
              handler.catchObjDisplacement = readI32(hAddr + 8);
              handler.handlerAddress = readU32(hAddr + 12);
              tryBlock.handlers.push_back(handler);

              if (handler.handlerAddress != 0) {
                discoveredFuncs.push_back(handler.handlerAddress);
              }
            }
          }
        }

        cxxInfo.tryBlocks.push_back(std::move(tryBlock));
      }
    }
  }

  // Parse IPtoStateMap
  if (pIPtoStateMap != 0 && nIPMapEntries > 0) {
    if (pIPtoStateMap >= rdataStart &&
        pIPtoStateMap + nIPMapEntries * 8 <= rdataStart + rdataSize) {
      for (uint32_t i = 0; i < nIPMapEntries; i++) {
        CxxIPStateEntry entry;
        entry.ip = readU32(pIPtoStateMap + i * 8);
        entry.state = readI32(pIPtoStateMap + i * 8 + 4);
        cxxInfo.ipToStateMap.push_back(entry);

        if (entry.ip != 0 && entry.ip > maxAddr) {
          maxAddr = entry.ip;
        }
      }
    }
  }

  ParsedExceptionInfo result;
  result.info.data = std::move(cxxInfo);
  result.maxAddress = maxAddr;
  result.discoveredFuncs = std::move(discoveredFuncs);
  return result;
}

std::optional<ParsedExceptionInfo> parseExceptionInfo(const BinaryView& binary,
                                                      uint32_t functionBeginAddr) {
  uint32_t headerAddr = functionBeginAddr - 8;

  auto* textSection = binary.findSection(headerAddr);
  if (!textSection || !textSection->data) {
    return std::nullopt;
  }

  uint32_t headerOffset = headerAddr - textSection->baseAddress;
  if (headerOffset + 8 > textSection->size) {
    return std::nullopt;
  }

  const uint8_t* headerData = textSection->data + headerOffset;
  uint32_t handlerThunk = load_and_swap<uint32_t>(headerData);
  uint32_t tableAddr = load_and_swap<uint32_t>(headerData + 4);

  auto* rdataSection = binary.findSectionByName(".rdata");
  if (!rdataSection || !rdataSection->data) {
    return std::nullopt;
  }

  uint32_t rdataStart = rdataSection->baseAddress;
  uint32_t rdataSize = rdataSection->size;
  uint32_t rdataEnd = rdataStart + rdataSize;
  const uint8_t* rdataBase = rdataSection->data;

  if (tableAddr < rdataStart || tableAddr >= rdataEnd) {
    return std::nullopt;
  }

  uint32_t tableOffset = tableAddr - rdataStart;
  if (tableOffset + 4 > rdataSize) {
    return std::nullopt;
  }

  uint32_t firstWord = load_and_swap<uint32_t>(rdataBase + tableOffset);
  std::vector<uint32_t> discoveredFuncs;

  if (firstWord == CXX_EH_MAGIC) {
    return parseCxxFuncInfo(handlerThunk, tableAddr, rdataBase, rdataStart, rdataSize,
                            functionBeginAddr, discoveredFuncs);
  } else {
    uint32_t count = firstWord;
    if (count == 0 || count > 100) {
      return std::nullopt;
    }
    return parseSehScopeTable(handlerThunk, tableAddr, count, rdataBase, rdataStart, rdataSize,
                              functionBeginAddr, discoveredFuncs);
  }
}

//=============================================================================
// Helper Detection
//=============================================================================

void detectSaveRestoreHelpers(const BinaryView& binary, AnalysisState& state) {
  struct HelperPattern {
    uint32_t pattern;
    uint32_t* state_addr;
    const char* name;
  };

  HelperPattern patterns[] = {
      {0xe9c1ff68, &state.restGpr14Address, "__restgprlr_14"},
      {0xf9c1ff68, &state.saveGpr14Address, "__savegprlr_14"},
      {0xc9ccff70, &state.restFpr14Address, "__restfpr_14"},
      {0xd9ccff70, &state.saveFpr14Address, "__savefpr_14"},
  };

  for (const auto& section : binary.sections()) {
    if (!section.executable)
      continue;

    const uint8_t* data = section.data;
    const size_t size = section.size;
    const size_t base = section.baseAddress;

    for (size_t offset = 0; offset + 4 <= size; offset += 4) {
      uint32_t instr = load_and_swap<uint32_t>(data + offset);
      uint32_t addr = static_cast<uint32_t>(base + offset);

      for (auto& p : patterns) {
        if (instr == p.pattern && *p.state_addr == 0) {
          *p.state_addr = addr;
          REXCODEGEN_DEBUG("Found {} at 0x{:08X}", p.name, addr);
        }
      }

      if (offset + 8 <= size) {
        uint32_t next = load_and_swap<uint32_t>(data + offset + 4);

        if (instr == 0x3960fee0) {
          if (next == 0x7dcb60ce && state.restVmx14Address == 0) {
            state.restVmx14Address = addr;
            REXCODEGEN_DEBUG("Found __restvmx_14 at 0x{:08X}", addr);
          } else if (next == 0x7dcb61ce && state.saveVmx14Address == 0) {
            state.saveVmx14Address = addr;
            REXCODEGEN_DEBUG("Found __savevmx_14 at 0x{:08X}", addr);
          }
        }

        if (instr == 0x3960fc00) {
          if (next == 0x100b60cb && state.restVmx64Address == 0) {
            state.restVmx64Address = addr;
            REXCODEGEN_DEBUG("Found __restvmx_64 at 0x{:08X}", addr);
          } else if (next == 0x100b61cb && state.saveVmx64Address == 0) {
            state.saveVmx64Address = addr;
            REXCODEGEN_DEBUG("Found __savevmx_64 at 0x{:08X}", addr);
          }
        }
      }
    }
  }
}

//=============================================================================
// Register Phase: imports, helpers, PDATA, config functions
//=============================================================================

VoidResult registerEntryPoints(CodegenContext& ctx, std::vector<uint32_t>& ehDiscoveredFuncs) {
  REXCODEGEN_INFO("Analyze: registering entry points...");

  auto& graph = ctx.graph;
  auto& config = ctx.Config();
  auto& state = ctx.analysisState();
  auto& binary = ctx.binary();

  // Merge user hints into analysis state
  for (const auto& [addr, size] : config.invalidInstructionHints) {
    state.invalidInstructions[addr] = size;
  }
  for (uint32_t addr : config.knownIndirectCallHints) {
    state.knownIndirectCalls.insert(addr);
  }
  for (uint32_t addr : config.exceptionHandlerFuncHints) {
    state.exceptionHandlerFuncs.push_back(addr);
  }

  // Build chunksByParent from config.functions
  for (const auto& [addr, cfg] : config.functions) {
    if (cfg.isChunk()) {
      state.chunksByParent[cfg.parent].push_back(addr);
    }
  }

  detectSaveRestoreHelpers(binary, state);

  // Register imports
  {
    auto* resolver = ctx.resolver();
    if (!resolver) {
      REXCODEGEN_WARN("No export resolver available - imports won't be resolved");
    }

    size_t importCount = 0;
    size_t resolvedCount = 0;
    size_t variableCount = 0;
    size_t unresolvedCount = 0;

    for (const auto& sym : binary.importSymbols()) {
      auto at_pos = sym.name.find('@');
      if (at_pos == std::string::npos) {
        REXCODEGEN_ERROR("Invalid import format (missing @): {}", sym.name);
        continue;
      }

      auto lib_name = sym.name.substr(0, at_pos);
      auto ordinal_str = sym.name.substr(at_pos + 1);
      uint16_t ordinal = static_cast<uint16_t>(std::stoul(ordinal_str));

      std::string resolvedName;
      if (resolver) {
        auto* exp = resolver->GetExportByOrdinal(lib_name + ".xex", ordinal);
        if (!exp)
          exp = resolver->GetExportByOrdinal(lib_name, ordinal);

        if (exp) {
          if (exp->type == runtime::Export::Type::kFunction) {
            resolvedName = "__imp__" + std::string(exp->name);
            resolvedCount++;
          } else {
            variableCount++;
            continue;
          }
        }
      }

      if (resolvedName.empty()) {
        REXCODEGEN_ERROR("Cannot resolve ordinal {} from {}", ordinal, lib_name);
        resolvedName = fmt::format("sub_{:X}", sym.address);
        unresolvedCount++;
      }

      auto* node = graph.addImportFunction(sym.address, resolvedName);
      if (node && node->canDiscover()) {
        node->discoverAsImport();
        if (node->canSeal()) {
          node->seal();
        }
      }
      importCount++;
    }

    REXCODEGEN_INFO("Analyze: loaded {} imports ({} resolved, {} unresolved, {} variables skipped)",
                    importCount, resolvedCount, unresolvedCount, variableCount);
  }

  // Register save/restore helpers
  auto registerHelpers = [&](uint32_t base14, const char* prefix, size_t stride, size_t endReg,
                             size_t extraSize) {
    if (base14 == 0)
      return;
    for (size_t i = 14; i <= endReg; i++) {
      uint32_t addr = base14 + static_cast<uint32_t>((i - 14) * stride);
      uint32_t size = static_cast<uint32_t>((endReg + 1 - i) * stride + extraSize);
      graph.addFunction(addr, size, FunctionAuthority::HELPER, true);
      graph.setFunctionName(addr, fmt::format("{}{}", prefix, i));
    }
  };

  registerHelpers(state.restGpr14Address, "__restgprlr_", 4, 31, 12);
  registerHelpers(state.saveGpr14Address, "__savegprlr_", 4, 31, 8);
  registerHelpers(state.restFpr14Address, "__restfpr_", 4, 31, 4);
  registerHelpers(state.saveFpr14Address, "__savefpr_", 4, 31, 4);
  registerHelpers(state.restVmx14Address, "__restvmx_", 8, 31, 4);
  registerHelpers(state.saveVmx14Address, "__savevmx_", 8, 31, 4);

  // VMX 64-127
  if (state.restVmx64Address != 0) {
    for (size_t i = 64; i < 128; i++) {
      uint32_t addr = state.restVmx64Address + static_cast<uint32_t>((i - 64) * 8);
      uint32_t size = static_cast<uint32_t>((128 - i) * 8 + 4);
      graph.addFunction(addr, size, FunctionAuthority::HELPER, true);
      graph.setFunctionName(addr, fmt::format("__restvmx_{}", i));
    }
  }
  if (state.saveVmx64Address != 0) {
    for (size_t i = 64; i < 128; i++) {
      uint32_t addr = state.saveVmx64Address + static_cast<uint32_t>((i - 64) * 8);
      uint32_t size = static_cast<uint32_t>((128 - i) * 8 + 4);
      graph.addFunction(addr, size, FunctionAuthority::HELPER, true);
      graph.setFunctionName(addr, fmt::format("__savevmx_{}", i));
    }
  }

  // Register CONFIG functions
  size_t configFuncs = 0, configChunks = 0;
  for (const auto& [address, cfg] : config.functions) {
    uint32_t size = cfg.getSize(address);
    std::string name = cfg.name.empty() ? fmt::format("sub_{:08X}", address) : cfg.name;
    graph.addFunction(address, size, FunctionAuthority::CONFIG, true);
    graph.setFunctionName(address, name);
    configFuncs++;

    if (cfg.isChunk()) {
      graph.registerChunk(address, size);
      configChunks++;
    }
  }
  if (configFuncs > 0) {
    REXCODEGEN_DEBUG("Analyze: {} CONFIG functions, {} chunks", configFuncs, configChunks);
  }

  // Register PDATA functions
  uint32_t pdataAddr = binary.exceptionDirectoryAddr();
  uint32_t pdataSize = binary.exceptionDirectorySize();

  if (pdataSize == 0) {
    return Err(ErrorCategory::Format, "Exception DataDirectory not found (size=0)");
  }

  auto* pdataSection = binary.findSection(pdataAddr);
  if (!pdataSection) {
    return Err(ErrorCategory::Format,
               fmt::format("Cannot find section containing PDATA at 0x{:08X}", pdataAddr));
  }

  uint32_t offsetInSection = pdataAddr - pdataSection->baseAddress;
  const uint8_t* pdataData = pdataSection->data + offsetInSection;

  REXCODEGEN_INFO("Analyze: PDATA base=0x{:08X}, size={}", pdataAddr, pdataSize);

  size_t count = pdataSize / sizeof(IMAGE_CE_RUNTIME_FUNCTION);
  auto* entries = reinterpret_cast<const IMAGE_CE_RUNTIME_FUNCTION*>(pdataData);

  size_t pdataAdded = 0;
  for (size_t i = 0; i < count; i++) {
    uint32_t beginAddr = byte_swap(entries[i].BeginAddress);
    uint32_t data = byte_swap(entries[i].Data);

    IMAGE_CE_RUNTIME_FUNCTION fn;
    fn.BeginAddress = beginAddr;
    fn.Data = data;

    uint32_t size = fn.FunctionLength * 4;
    if (size == 0)
      size = 4;

    if (graph.getFunction(beginAddr) != nullptr) {
      continue;
    }

    std::optional<ParsedExceptionInfo> exInfo;
    PrologInfo prologInfo;
    if (fn.ExceptionFlag) {
      // Scan prolog to get frame size for SEH unwinding
      prologInfo = scanProlog(binary, beginAddr, fn.PrologLength);

      exInfo = parseExceptionInfo(binary, beginAddr);
      if (exInfo) {
        if (exInfo->maxAddress > beginAddr + size) {
          size = exInfo->maxAddress - beginAddr + 4;
        }
        state.invalidInstructions[beginAddr - 8] = 8;

        for (uint32_t addr : exInfo->discoveredFuncs) {
          ehDiscoveredFuncs.push_back(addr);
        }

        // Populate SEH frame info for unwinding
        if (exInfo->info.isSeh()) {
          // Need non-const access to set frame info
          auto& sehInfo = std::get<SehExceptionInfo>(exInfo->info.data);
          sehInfo.frameSize = prologInfo.frameSize;

          // Warn if we have SEH scopes but couldn't determine frame size
          if (!prologInfo.valid && !sehInfo.scopes.empty()) {
            REXCODEGEN_WARN("SEH function 0x{:08X}: could not determine frame size from prolog",
                            beginAddr);
          }

          // Compute restore helper from save helper
          // Save helpers and restore helpers are at matching offsets from their base addresses
          if (prologInfo.saveHelper != 0 && state.restGpr14Address != 0 &&
              state.saveGpr14Address != 0) {
            sehInfo.restoreHelper =
                prologInfo.saveHelper + (state.restGpr14Address - state.saveGpr14Address);
          }
        }
      }
    }

    graph.addFunction(beginAddr, size, FunctionAuthority::PDATA, true);
    graph.setFunctionHasExceptionHandler(beginAddr, fn.ExceptionFlag);

    if (exInfo && exInfo->info.hasInfo()) {
      if (auto* seh = exInfo->info.asSeh()) {
        for (const auto& scope : seh->scopes) {
          if (scope.tryStart != 0)
            graph.addLabelToFunction(beginAddr, scope.tryStart);
          if (scope.tryEnd != 0)
            graph.addLabelToFunction(beginAddr, scope.tryEnd);
          // For __except (filter!=0), handler is inline code - add as label
          if (scope.filter != 0 && scope.handler != 0) {
            graph.addLabelToFunction(beginAddr, scope.handler);
          }
        }
      } else if (auto* cxx = exInfo->info.asCxx()) {
        for (const auto& entry : cxx->ipToStateMap) {
          if (entry.ip != 0)
            graph.addLabelToFunction(beginAddr, entry.ip);
        }
      }
      graph.setFunctionExceptionInfo(beginAddr, std::move(exInfo->info));
    }

    ctx.scan.pdataSizes[beginAddr] = size;
    pdataAdded++;
  }

  REXCODEGEN_INFO("Analyze: added {} functions from PDATA", pdataAdded);

  // Queue EH-discovered functions
  size_t ehFuncsQueued = 0;
  for (uint32_t addr : ehDiscoveredFuncs) {
    if (graph.getFunction(addr) != nullptr)
      continue;
    if (graph.isImport(addr))
      continue;

    graph.addFunction(addr, 0, FunctionAuthority::DISCOVERED, true);
    ehFuncsQueued++;
  }

  if (ehFuncsQueued > 0) {
    REXCODEGEN_INFO("Analyze: queued {} functions from exception handling", ehFuncsQueued);
  }

  return Ok();
}

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

//=============================================================================
// Discover Phase: iterative function block discovery
//=============================================================================

void discoverFunction(CodegenContext& ctx, uint32_t funcAddr,
                      const std::unordered_set<uint32_t>& knownFunctions) {
  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  auto& decoded = ctx.decoded();

  auto* node = graph.getFunction(funcAddr);
  if (!node)
    return;

  // Skip if already discovered
  if (!node->canDiscover()) {
    REXCODEGEN_TRACE("Analyze: function 0x{:08X} already discovered, skipping", funcAddr);
    return;
  }

  // Imports don't need block discovery
  if (node->isImport()) {
    node->discoverAsImport();
    return;
  }

  REXCODEGEN_TRACE("Analyze: discovering function 0x{:08X} ({})", funcAddr, node->name());

  // Lookup pdataSize for exception handler boundary
  uint32_t pdataSize = 0;

  // For CONFIG functions: use only the explicitly declared size (if any)
  // If no size specified (size=0), let discovery find natural boundaries via region
  // Don't inherit PDATA sizes for CONFIG functions - they're user hints for entry points
  if (node->authority() == FunctionAuthority::CONFIG) {
    pdataSize = node->size();  // 0 if not specified, which is correct
    REXCODEGEN_TRACE("Analyze: 0x{:08X} is CONFIG, using declared size={}", funcAddr, pdataSize);
  } else {
    // For non-CONFIG functions, use PDATA size if available
    auto pdataIt = ctx.scan.pdataSizes.find(funcAddr);
    if (pdataIt != ctx.scan.pdataSizes.end()) {
      pdataSize = pdataIt->second;
      REXCODEGEN_TRACE("Analyze: 0x{:08X} using PDATA size={}", funcAddr, pdataSize);
    }
  }

  // Find the code region containing this function
  const CodeRegion* region = nullptr;
  for (const auto& r : ctx.scan.codeRegions) {
    if (r.contains(funcAddr)) {
      region = &r;
      break;
    }
  }
  if (!region) {
    REXCODEGEN_WARN("Analyze: function 0x{:08X} not in any code region", funcAddr);
    return;
  }

  // Pass pdataSize so forward branches within function extent are correctly identified
  auto result = discoverBlocks(decoded, funcAddr, *region, knownFunctions, pdataSize);

  if (result.blocks.empty()) {
    REXCODEGEN_WARN("Analyze: no blocks found for function 0x{:08X}", funcAddr);
    return;
  }

  // snooper the function with the discovered blocks and instructions
  node->discover(std::move(result.blocks), std::move(result.instructions),
                 std::move(result.labels));

  // Add jump tables (targets become labels in the function)
  for (const auto& jt : result.jumpTables) {
    graph.addJumpTableToFunction(funcAddr, jt);
  }

  // Register external call targets as new functions (bl only, not b)
  for (uint32_t target : result.externalCalls) {
    if (!graph.isEntryPoint(target) && !graph.isImport(target)) {
      if (binary.isInImportExportRange(target)) {
        continue;
      }
      graph.addFunction(target, 4, FunctionAuthority::DISCOVERED, true);
    }
  }

  // Add unresolved branches for later resolution
  for (const auto& branch : result.unresolvedBranches) {
    graph.addUnresolvedJumpToFunction(funcAddr, branch.site, branch.target, branch.isCall,
                                      branch.isConditional);
  }

  // Scan exception handler regions for branches not in discovered blocks
  if (pdataSize > 0) {
    std::unordered_set<uint32_t> discoveredAddrs;
    for (const auto& block : result.blocks) {
      for (uint32_t addr = block.base; addr < block.base + block.size; addr += 4) {
        discoveredAddrs.insert(addr);
      }
    }

    uint32_t pdataEnd = funcAddr + pdataSize;
    const uint8_t* funcData = binary.translate(funcAddr);
    if (funcData) {
      for (uint32_t offset = 0; offset < pdataSize; offset += 4) {
        uint32_t site = funcAddr + offset;

        // Skip if already discovered by normal control flow
        if (discoveredAddrs.count(site))
          continue;

        // Skip if marked invalid
        auto invalidIt = ctx.analysisState().invalidInstructions.find(site);
        if (invalidIt != ctx.analysisState().invalidInstructions.end()) {
          continue;
        }

        uint32_t insn = load_and_swap<uint32_t>(funcData + offset);
        uint32_t opcode = PPC_OP(insn);

        if (opcode != PPC_OP_B && opcode != PPC_OP_BC)
          continue;

        uint32_t target = 0;
        bool isCall = PPC_BL(insn);
        bool isAbsolute = PPC_BA(insn);

        if (opcode == PPC_OP_B) {
          int32_t branchOffset = PPC_BI(insn);
          target = isAbsolute ? static_cast<uint32_t>(branchOffset) : site + branchOffset;
        } else {
          int32_t branchOffset = PPC_BD(insn);
          target = isAbsolute ? static_cast<uint32_t>(branchOffset) : site + branchOffset;
        }

        // Skip internal jumps within pdata region
        if (!isCall && target >= funcAddr && target < pdataEnd) {
          continue;
        }

        graph.addUnresolvedJumpToFunction(funcAddr, site, target, isCall, false);

        // Register call targets as new functions
        if (isCall && !graph.isEntryPoint(target) && !graph.isImport(target)) {
          if (binary.isInImportExportRange(target)) {
            continue;
          }
          graph.addFunction(target, 4, FunctionAuthority::DISCOVERED, true);
        }
      }
    }
  }
}

void discoverAllFunctions(CodegenContext& ctx) {
  REXCODEGEN_INFO("Analyze: starting iterative discovery...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();

  // Iterative discovery
  size_t iteration = 0;
  size_t lastFunctionCount = 0;
  constexpr size_t MAX_ITERATIONS = 1000;

  while (iteration < MAX_ITERATIONS) {
    iteration++;

    size_t currentFunctionCount = graph.functionCount();
    if (currentFunctionCount == lastFunctionCount && iteration > 1) {
      REXCODEGEN_DEBUG("Analyze: fixed point at iteration {} ({} functions)", iteration,
                       currentFunctionCount);
      break;
    }

    lastFunctionCount = currentFunctionCount;

    auto knownFunctions = buildKnownFunctions(graph);
    if (discoverPendingFunctions(ctx, knownFunctions) == 0) {
      break;
    }
  }

  REXCODEGEN_INFO("Analyze: {} functions after call graph expansion", graph.functionCount());

  // VTable scanning
  {
    VTableScanner vtScanner(binary);
    auto vtables = vtScanner.scan();

    size_t newFunctions = 0;

    for (const auto& vt : vtables) {
      for (size_t i = 0; i < vt.slots.size(); i++) {
        uint32_t funcAddr = vt.slots[i];

        if (graph.isEntryPoint(funcAddr))
          continue;
        if (binary.isInImportExportRange(funcAddr))
          continue;

        graph.addFunction(funcAddr, 4, FunctionAuthority::VTABLE, true);
        newFunctions++;
      }
    }

    REXCODEGEN_INFO("Analyze: VTable scan found {} vtables, {} new functions", vtables.size(),
                    newFunctions);

    // Continue discovery for vtable functions
    if (newFunctions > 0) {
      size_t vtableIteration = 0;
      constexpr size_t MAX_VTABLE_ITERATIONS = 100;

      while (vtableIteration < MAX_VTABLE_ITERATIONS) {
        vtableIteration++;

        auto knownFunctions = buildKnownFunctions(graph);
        if (discoverPendingFunctions(ctx, knownFunctions) == 0)
          break;

        if (graph.functionCount() == lastFunctionCount)
          break;
        lastFunctionCount = graph.functionCount();
      }
    }
  }

  REXCODEGEN_INFO("Analyze: {} total functions after vtable scan", graph.functionCount());
}

//=============================================================================
// Function Pointer Scan: find lis/addi pairs loading code addresses
// TODO(tomc): THIS IS WIP AND PROB A BAD IDEA LOL LETS SEE
//=============================================================================
void functionPointerScan(CodegenContext& ctx) {
  if (!ctx.hasDecoded()) {
    REXCODEGEN_WARN("functionPointerScan: DecodedBinary not initialized, skipping");
    return;
  }

  auto& graph = ctx.graph;
  auto& decoded = ctx.decoded();
  const auto& codeRegions = decoded.codeRegions();

  if (codeRegions.empty()) {
    REXCODEGEN_WARN("functionPointerScan: no code regions, skipping");
    return;
  }

  // Build set of existing functions to avoid duplicates
  std::unordered_set<uint32_t> existingFunctions;
  for (const auto& [addr, node] : graph.functions()) {
    existingFunctions.insert(addr);
  }

  // Track lis values: register -> (high_value, lis_address)
  // We scan linearly and track the most recent lis for each register
  // PPC has exactly 32 GPRs, so a fixed-size array is more efficient than a map
  std::array<std::pair<uint32_t, uint32_t>, 32> lisValues{};
  std::bitset<32> lisValid;

  size_t foundCount = 0;

  for (const auto& region : codeRegions) {
    lisValid.reset();  // Reset tracking at region boundaries

    for (uint32_t addr = region.start; addr < region.end; addr += 4) {
      auto* insn = decoded.get(addr);
      if (!insn)
        continue;

      // Track lis rD, IMM
      if (isLis(*insn)) {
        uint8_t rd = static_cast<uint8_t>(insn->D.RT);
        uint32_t hi = static_cast<uint32_t>(static_cast<int16_t>(insn->D.d)) << 16;
        lisValues[rd] = {hi, addr};
        lisValid.set(rd);
        continue;
      }

      // Check for addi rD, rA, IMM where rA was set by lis
      if (insn->opcode == rex::codegen::ppc::Opcode::addi) {
        uint8_t ra = static_cast<uint8_t>(insn->D.RA);
        if (ra == 0)
          continue;  // li pseudo-op, not addi

        if (!lisValid.test(ra))
          continue;

        uint32_t hi = lisValues[ra].first;
        int16_t lo = static_cast<int16_t>(insn->D.d);
        uint32_t fullAddr = hi + lo;  // Sign-extended add

        // PPC instructions are 4-byte aligned
        if (fullAddr & 0x3)
          continue;

        // Check if this address is in a code region
        const CodeRegion* targetRegion = decoded.regionContaining(fullAddr);
        if (!targetRegion)
          continue;

        // Skip if already a known function
        if (existingFunctions.contains(fullAddr))
          continue;

        // Skip if it's an internal address (within same function's likely range)
        // Heuristic: if target is very close to current address, probably internal label
        int32_t distance = static_cast<int32_t>(fullAddr) - static_cast<int32_t>(addr);
        if (distance > -0x1000 && distance < 0x1000) {
          // Could be local label, skip for now
          continue;
        }

        // Register as function with DISCOVERED authority and hasXrefs=true
        graph.addFunction(fullAddr, 4, FunctionAuthority::DISCOVERED, true);
        existingFunctions.insert(fullAddr);
        foundCount++;

        REXCODEGEN_TRACE("functionPointerScan: found 0x{:08X} via lis/addi at 0x{:08X}", fullAddr,
                         addr);
      }

      // Also check ori rD, rA, IMM (alternative to addi for unsigned)
      if (insn->opcode == rex::codegen::ppc::Opcode::ori) {
        uint8_t ra = static_cast<uint8_t>(insn->D.RA);
        if (!lisValid.test(ra))
          continue;

        uint32_t hi = lisValues[ra].first;
        uint16_t lo = static_cast<uint16_t>(insn->D.d);
        uint32_t fullAddr = hi | lo;  // Unsigned OR

        // PPC instructions are 4-byte aligned
        if (fullAddr & 0x3)
          continue;

        const CodeRegion* targetRegion = decoded.regionContaining(fullAddr);
        if (!targetRegion)
          continue;

        if (existingFunctions.contains(fullAddr))
          continue;

        int32_t distance = static_cast<int32_t>(fullAddr) - static_cast<int32_t>(addr);
        if (distance > -0x1000 && distance < 0x1000)
          continue;

        graph.addFunction(fullAddr, 4, FunctionAuthority::DISCOVERED, true);
        existingFunctions.insert(fullAddr);
        foundCount++;

        REXCODEGEN_TRACE("functionPointerScan: found 0x{:08X} via lis/ori at 0x{:08X}", fullAddr,
                         addr);
      }

      // Clear lis tracking if register is overwritten by other instruction
      // (Simplified: we clear on any write to the register)
      // This is conservative - could miss some patterns but avoids false positives
    }
  }

  REXCODEGEN_INFO("functionPointerScan: found {} new function pointer targets", foundCount);
}

//=============================================================================
// GapFill to register uncovered code regions
//=============================================================================

// Split a code region into function segments based on terminators (blr, tail calls).
std::vector<CodeRegion> splitRegionOnTerminators(
    const CodeRegion& region, const BinaryView& binary,
    const std::unordered_set<uint32_t>& knownCallables) {
  std::vector<CodeRegion> segments;
  uint32_t segmentStart = region.start;

  for (uint32_t addr = region.start; addr < region.end; addr += 4) {
    const uint8_t* data = binary.translate(addr);
    if (!data)
      break;

    uint32_t raw = load_and_swap<uint32_t>(data);
    auto decoded = decode_instruction(addr, raw);
    bool shouldSplit = false;
    const char* reason = nullptr;

    // Check for terminators
    if (decoded.is_return()) {
      shouldSplit = true;
      reason = "blr";
    } else if (decoded.opcode == Opcode::b && decoded.branch_target.has_value()) {
      uint32_t target = decoded.branch_target.value();
      // Don't split on tail recursion (branch to own segment start)
      if (target != segmentStart && knownCallables.contains(target)) {
        shouldSplit = true;
        reason = "tail call";
      }
    }

    if (shouldSplit) {
      uint32_t segmentEnd = addr + 4;
      if (segmentEnd > segmentStart) {
        segments.push_back({segmentStart, segmentEnd});
        REXCODEGEN_TRACE("GapFill: split segment 0x{:08X}-0x{:08X} ({} at 0x{:08X})", segmentStart,
                         segmentEnd, reason, addr);
      }
      segmentStart = segmentEnd;
    }
  }

  // Handle remaining code after last terminator
  if (segmentStart < region.end) {
    segments.push_back({segmentStart, region.end});
  }

  return segments;
}

// Check if address looks like exception handler data (handler ptr + rdata ptr)
bool looksLikeExceptionData(const BinaryView& binary, const FunctionGraph& graph, uint32_t addr) {
  const uint8_t* data = binary.translate(addr);
  if (!data)
    return false;

  // Exception handler data pattern:
  // [addr+0]: pointer to __C_specific_handler (entry point)
  // [addr+4]: pointer to scope table in .rdata
  uint32_t firstDword = load_and_swap<uint32_t>(data);
  uint32_t secondDword = load_and_swap<uint32_t>(data + 4);

  // Check if first dword is a known entry point (like __C_specific_handler)
  if (!graph.isEntryPoint(firstDword)) {
    return false;
  }

  // Check if second dword points to .rdata section
  auto* rdataSection = binary.findSectionByName(".rdata");
  if (!rdataSection)
    return false;

  uint32_t rdataStart = rdataSection->baseAddress;
  uint32_t rdataEnd = rdataStart + rdataSection->size;

  if (secondDword >= rdataStart && secondDword < rdataEnd) {
    REXCODEGEN_TRACE(
        "GapFill: 0x{:08X} looks like exception data (handler=0x{:08X}, scope=0x{:08X}), skipping",
        addr, firstDword, secondDword);
    return true;
  }

  return false;
}

void gapFillCodeRegions(CodegenContext& ctx) {
  REXCODEGEN_INFO("Analyze: checking for uncovered code regions...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  auto& scan = ctx.scan;

  // Build set of known callables for tail call detection
  std::unordered_set<uint32_t> knownCallables;
  for (const auto& [addr, node] : graph.functions()) {
    knownCallables.insert(addr);
  }

  size_t gapsFound = 0;
  size_t segmentsCreated = 0;

  for (const auto& region : scan.codeRegions) {
    // Split region on terminators (blr, tail calls), then check each segment
    auto segments = splitRegionOnTerminators(region, binary, knownCallables);

    for (const auto& segment : segments) {
      // Skip if this segment's start is already a registered function entry
      if (graph.isEntryPoint(segment.start))
        continue;

      // Skip if this segment's start is inside another function
      if (auto* containingFunc = graph.getFunctionContaining(segment.start)) {
        continue;
      }

      // Skip if this looks like exception handler data (handler ptr + rdata ptr)
      if (looksLikeExceptionData(binary, graph, segment.start))
        continue;

      uint32_t segmentSize = segment.size();
      graph.addFunction(segment.start, segmentSize, FunctionAuthority::GAP_FILL, false);

      REXCODEGEN_TRACE("GapFill: registered sub_{:08X} (0x{:08X}-0x{:08X}, {} bytes)",
                       segment.start, segment.start, segment.end, segmentSize);
      segmentsCreated++;
    }

    gapsFound++;
  }

  if (segmentsCreated > 0) {
    REXCODEGEN_INFO("Analyze: registered {} gap functions from {} regions", segmentsCreated,
                    gapsFound);
  } else {
    REXCODEGEN_INFO("Analyze: no uncovered regions found");
  }
}

//=============================================================================
// Cleanup absorbed GAP_FILL functions
//=============================================================================

void cleanupAbsorbedGapFills(CodegenContext& ctx) {
  auto& graph = ctx.graph;
  std::vector<uint32_t> toRemove;

  for (const auto& [addr, node] : graph.functions()) {
    if (node->authority() != FunctionAuthority::GAP_FILL)
      continue;

    for (const auto& [otherAddr, otherNode] : graph.functions()) {
      if (otherAddr == addr)
        continue;
      if (!otherNode->containsAddress(addr))
        continue;

      // This GAP_FILL is inside another function's blocks
      if (otherNode->authority() != FunctionAuthority::GAP_FILL) {
        // Absorbed by higher authority - remove
        toRemove.push_back(addr);
        break;
      } else if (otherAddr < addr) {
        // Both GAP_FILL, other has lower address - it survives
        toRemove.push_back(addr);
        break;
      }
    }
  }

  for (uint32_t addr : toRemove) {
    graph.removeFunction(addr);
  }

  if (!toRemove.empty()) {
    REXCODEGEN_INFO("Analyze: removed {} absorbed GAP_FILL functions", toRemove.size());
  }
}

//=============================================================================
// Merge to resolve jumps then seal functions
//=============================================================================
void mergeAndSeal(CodegenContext& ctx) {
  REXCODEGEN_INFO("Analyze: resolving jumps and sealing functions...");

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
  constexpr size_t MAX_ITERATIONS = 100;

  while (iteration < MAX_ITERATIONS) {
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

  size_t totalSealed = graph.sealAllReady();
  size_t stillPending = graph.pendingCount();

  REXCODEGEN_INFO("Analyze: {} iterations, resolved={}, sealed={}/{}", iteration, totalResolved,
                  totalSealed, graph.functionCount());

  if (stillPending > 0) {
    REXCODEGEN_WARN("Analyze: {} functions still PENDING with unresolved jumps", stillPending);
  }
}

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

Result<void> Analyze(CodegenContext& ctx) {
  REXCODEGEN_INFO("Analyze: starting analysis...");

  ctx.initDecoded();
  REXCODEGEN_INFO("Analyze: decoded {} instructions across {} code regions",
                  ctx.decoded().instructionCount(), ctx.decoded().codeRegions().size());

  std::vector<uint32_t> ehDiscoveredFuncs;

  // 1. Register entry points (imports, helpers, config, pdata)
  auto regResult = registerEntryPoints(ctx, ehDiscoveredFuncs);
  if (!regResult) {
    return regResult;
  }

  // 2. Scan binary into code/data regions
  scanBinary(ctx);

  // 3. Discover function blocks iteratively (includes vtable scan)
  discoverAllFunctions(ctx);

  // 3.5. Function pointer scan: find lis/addi pairs loading code addresses
  // TODO(tomc): disabled for now, causes too many false positives
  // functionPointerScan(ctx);

  // 4. Gap fill uncovered regions
  gapFillCodeRegions(ctx);

  // 5. Discover blocks for gap-filled functions
  {
    auto knownFunctions = buildKnownFunctions(ctx.graph, /*excludeGapFill=*/true);
    size_t discovered = discoverPendingFunctions(ctx, knownFunctions);
    REXCODEGEN_INFO("Analyze: discovered blocks for {} gap-filled functions", discovered);
  }

  // 5.5. Remove absorbed GAP_FILL functions
  cleanupAbsorbedGapFills(ctx);

  // 6. Merge: resolve jumps and seal functions
  mergeAndSeal(ctx);

  // 7. Validate
  auto validateResult = validateGraph(ctx);
  if (!validateResult) {
    return validateResult;
  }

  REXCODEGEN_INFO("Analyze: complete - {} functions ready for code generation",
                  ctx.graph.functionCount());

  return Ok();
}

//=============================================================================
// AnalysisErrors implementation (merged from analysis_errors.cpp)
//=============================================================================

const char* AnalysisErrors::CategoryName(Category cat) {
  switch (cat) {
    case Category::UnresolvedCall:
      return "UnresolvedCall";
    case Category::MissingJumpTable:
      return "MissingJumpTable";
    case Category::JumpTargetOutOfBounds:
      return "JumpTargetOutOfBounds";
    case Category::DiscontinuousFunction:
      return "DiscontinuousFunction";
    case Category::UnimplementedInsn:
      return "UnimplementedInsn";
    default:
      return "Unknown";
  }
}

void AnalysisErrors::Add(Category cat, uint32_t addr, const std::string& msg) {
  Add(cat, addr, 0, msg);
}

void AnalysisErrors::Add(Category cat, uint32_t addr, uint32_t secondary, const std::string& msg) {
  entries_.push_back({cat, addr, secondary, msg});
}

size_t AnalysisErrors::Count(Category cat) const {
  return std::count_if(entries_.begin(), entries_.end(),
                       [cat](const Entry& e) { return e.category == cat; });
}

void AnalysisErrors::PrintReport() const {
  if (entries_.empty()) {
    return;
  }

  REXCODEGEN_ERROR("=== ANALYSIS ERRORS ===");

  // Group by category
  std::map<Category, std::vector<const Entry*>> byCategory;
  for (const auto& entry : entries_) {
    byCategory[entry.category].push_back(&entry);
  }

  for (const auto& [cat, entries] : byCategory) {
    REXCODEGEN_ERROR("{} ({}):", CategoryName(cat), entries.size());

    for (const auto* entry : entries) {
      if (entry->secondaryAddress != 0) {
        REXCODEGEN_ERROR("  0x{:08X} from 0x{:08X}: {}", entry->address, entry->secondaryAddress,
                         entry->message);
      } else {
        REXCODEGEN_ERROR("  0x{:08X}: {}", entry->address, entry->message);
      }
    }
  }

  REXCODEGEN_ERROR("Total: {} errors", entries_.size());
}

}  // namespace rex::codegen
