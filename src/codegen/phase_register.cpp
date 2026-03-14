/**
 * @file        codegen/phase_register.cpp
 * @brief       Register phase: imports, helpers, PDATA, config functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "codegen_flags.h"
#include "ppc/instruction.h"

#include <rex/codegen/phases.h>
#include "phase_helpers.h"

#include <fmt/format.h>

#include <rex/codegen/config.h>
#include <rex/logging.h>

#include "codegen_logging.h"
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

  if (cxxInfo.maxState > REXCVAR_GET(max_eh_states) ||
      nTryBlocks > REXCVAR_GET(max_eh_try_blocks) ||
      nIPMapEntries > REXCVAR_GET(max_eh_ip_map_entries)) {
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
    if (count == 0 || count > REXCVAR_GET(max_seh_scope_entries)) {
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

VoidResult registerEntryPoints(CodegenContext& ctx) {
  REXCODEGEN_INFO("Analyze: registering entry points...");

  auto& graph = ctx.graph;
  auto& config = ctx.Config();
  auto& state = ctx.analysisState();
  auto& binary = ctx.binary();
  auto& ehDiscoveredFuncs = state.ehDiscoveredFuncs;

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

}  // anonymous namespace

namespace phases {

VoidResult Register(CodegenContext& ctx) {
  return registerEntryPoints(ctx);
}

}  // namespace phases

}  // namespace rex::codegen
