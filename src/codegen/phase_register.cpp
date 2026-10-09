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

#include "ppc/instruction.h"

#include <rex/codegen/phases.h>
#include "phase_helpers.h"

#include <format>

#include <rex/assert.h>
#include <rex/codegen/config.h>
#include <rex/logging.h>

#include <rex/memory/utils.h>
#include <rex/runtime/exports.h>
#include <rex/types.h>

#include <ppc.h>

using rex::codegen::ppc::decode_instruction;
using rex::codegen::ppc::Opcode;

namespace rex::codegen {

namespace {

#pragma pack(push, 1)
struct IMAGE_CE_RUNTIME_FUNCTION {
  u32 BeginAddress;
  union {
    u32 Data;
    struct {
      u32 PrologLength : 8;
      u32 FunctionLength : 22;
      u32 ThirtyTwoBit : 1;
      u32 ExceptionFlag : 1;
    };
  };
};
#pragma pack(pop)

struct SehHeader {
  be_u32 handlerThunk;
  be_u32 tableAddr;
};
REX_STATIC_ASSERT_SIZE(SehHeader, 8);

struct SehScopeEntry {
  be_u32 tryStart;
  be_u32 tryEnd;
  be_u32 filter;
  be_u32 handler;
};
REX_STATIC_ASSERT_SIZE(SehScopeEntry, 16);

struct ParsedExceptionInfo {
  ExceptionInfo info;
  u32 maxAddress;
  std::vector<u32> discoveredFuncs;
};

/** Frame size and save helper read from a function prologue for SEH unwinding. */
struct PrologInfo {
  /** Frame size from addi r31, r1, -N. */
  u32 frameSize = 0;
  /** Target of the bl to the save helper. */
  u32 saveHelper = 0;
  bool valid = false;
};

/**
 * Scans the prologue of a function with ExceptionFlag set for its frame size and save helper.
 * Scanning is skipped when the PDATA prolog length is 0. valid may be false for handler
 * functions that receive the frame in r12, so the caller warns only when SEH scopes need it.
 *
 * @param binary Binary view.
 * @param funcAddr Function address.
 * @param prologLength Prolog length in instructions from PDATA.
 * @return The prologue info.
 */
PrologInfo scanProlog(const BinaryView& binary, u32 funcAddr, u32 prologLength) {
  PrologInfo info;

  auto* section = binary.findSection(funcAddr);
  if (!section || !section->data) {
    return info;
  }

  u32 offset = funcAddr - section->baseAddress;
  if (offset + prologLength * 4 > section->size) {
    return info;
  }

  const u8* code = section->data + offset;

  if (prologLength == 0) {
    REXLOG_WARN(codegen, "SEH function 0x{:08X}: PrologLength=0, cannot determine frame size",
                funcAddr);
    return info;
  }

  for (u32 i = 0; i < prologLength; i++) {
    u32 raw = memory::load<be_u32>(code + i * 4);
    u32 addr = funcAddr + i * 4;
    auto decoded = decode_instruction(addr, raw);

    if (decoded.opcode == Opcode::addi && decoded.D.RT == 31 && decoded.D.RA == 1) {
      i16 simm = decoded.D.SIMM();
      if (simm < 0) {
        info.frameSize = static_cast<u32>(-simm);
        info.valid = true;
      }
    }

    if (decoded.is_call() && decoded.branch_target.has_value()) {
      info.saveHelper = decoded.branch_target.value();
    }
  }

  return info;
}

/**
 * Parses an SEH scope table. A __finally scope holds the handler in word 2 and 0 in word 3. An
 * __except scope holds the filter in word 2 and the handler in word 3. A __finally handler and
 * an __except filter are separate functions and an __except handler is inline.
 *
 * @param handlerThunk Address of the handler thunk.
 * @param scopeTableAddr Address of the scope table.
 * @param count Scope count.
 * @param rdataBase Host pointer to the start of .rdata.
 * @param rdataStart Address of the start of .rdata.
 * @param rdataSize Size of .rdata in bytes.
 * @param functionBeginAddr Address of the function.
 * @param discoveredFuncs Receives the functions the table names.
 * @return The parsed info, or nullopt when the table is outside .rdata.
 */
std::optional<ParsedExceptionInfo> parseSehScopeTable(u32 handlerThunk, u32 scopeTableAddr,
                                                      u32 count, const u8* rdataBase,
                                                      u32 rdataStart, u32 rdataSize,
                                                      u32 functionBeginAddr,
                                                      std::vector<u32>& discoveredFuncs) {
  u32 tableOffset = scopeTableAddr - rdataStart;
  if (tableOffset + 4 + count * 16 > rdataSize) {
    return std::nullopt;
  }

  SehExceptionInfo sehInfo;
  sehInfo.handlerThunk = handlerThunk;
  sehInfo.scopeTableAddr = scopeTableAddr;

  u32 maxAddr = functionBeginAddr;

  const auto* entries = reinterpret_cast<const SehScopeEntry*>(rdataBase + tableOffset + 4);
  for (u32 i = 0; i < count; i++) {
    SehScope scope;
    scope.tryStart = entries[i].tryStart;
    scope.tryEnd = entries[i].tryEnd;
    scope.filter = entries[i].filter;
    scope.handler = entries[i].handler;

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

    if (scope.filter == 0 && scope.handler != 0) {
      discoveredFuncs.push_back(scope.handler);
    }
    if (scope.filter != 0) {
      discoveredFuncs.push_back(scope.filter);
    }
  }

  discoveredFuncs.push_back(handlerThunk);

  ParsedExceptionInfo result;
  result.info.data = std::move(sehInfo);
  result.maxAddress = maxAddr;
  result.discoveredFuncs = std::move(discoveredFuncs);
  return result;
}

std::optional<ParsedExceptionInfo> parseCxxFuncInfo(const Limits& limits, u32 handlerThunk,
                                                    u32 funcInfoAddr, const u8* rdataBase,
                                                    u32 rdataStart, u32 rdataSize,
                                                    u32 functionBeginAddr,
                                                    std::vector<u32>& discoveredFuncs) {
  u32 tableOffset = funcInfoAddr - rdataStart;
  if (tableOffset + 28 > rdataSize) {
    return std::nullopt;
  }

  auto readU32 = [&](u32 addr) { return memory::load<be_u32>(rdataBase + (addr - rdataStart)); };
  auto readI32 = [&](u32 addr) {
    return i32{memory::load<be_i32>(rdataBase + (addr - rdataStart))};
  };

  CxxExceptionInfo cxxInfo;
  cxxInfo.handlerThunk = handlerThunk;
  cxxInfo.funcInfoAddr = funcInfoAddr;

  u32 magic = readU32(funcInfoAddr + 0);
  cxxInfo.maxState = readU32(funcInfoAddr + 4);
  u32 pUnwindMap = readU32(funcInfoAddr + 8);
  u32 nTryBlocks = readU32(funcInfoAddr + 12);
  u32 pTryBlockMap = readU32(funcInfoAddr + 16);
  u32 nIPMapEntries = readU32(funcInfoAddr + 20);
  u32 pIPtoStateMap = readU32(funcInfoAddr + 24);

  if (magic != CXX_EH_MAGIC) {
    return std::nullopt;
  }

  if (cxxInfo.maxState > limits.max_eh_states || nTryBlocks > limits.max_eh_try_blocks ||
      nIPMapEntries > limits.max_eh_ip_map_entries) {
    return std::nullopt;
  }

  u32 maxAddr = functionBeginAddr;
  discoveredFuncs.push_back(handlerThunk);

  if (pUnwindMap != 0 && cxxInfo.maxState > 0) {
    if (pUnwindMap >= rdataStart && pUnwindMap + cxxInfo.maxState * 8 <= rdataStart + rdataSize) {
      for (u32 i = 0; i < cxxInfo.maxState; i++) {
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

  if (pTryBlockMap != 0 && nTryBlocks > 0) {
    if (pTryBlockMap >= rdataStart && pTryBlockMap + nTryBlocks * 20 <= rdataStart + rdataSize) {
      for (u32 i = 0; i < nTryBlocks; i++) {
        u32 entryAddr = pTryBlockMap + i * 20;
        CxxTryBlock tryBlock;
        tryBlock.tryLow = readI32(entryAddr + 0);
        tryBlock.tryHigh = readI32(entryAddr + 4);
        tryBlock.catchHigh = readI32(entryAddr + 8);
        u32 nCatches = readU32(entryAddr + 12);
        u32 pHandlers = readU32(entryAddr + 16);

        if (pHandlers != 0 && nCatches > 0 && nCatches <= 20) {
          if (pHandlers >= rdataStart && pHandlers + nCatches * 16 <= rdataStart + rdataSize) {
            for (u32 j = 0; j < nCatches; j++) {
              u32 hAddr = pHandlers + j * 16;
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

  if (pIPtoStateMap != 0 && nIPMapEntries > 0) {
    if (pIPtoStateMap >= rdataStart &&
        pIPtoStateMap + nIPMapEntries * 8 <= rdataStart + rdataSize) {
      for (u32 i = 0; i < nIPMapEntries; i++) {
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
                                                      const Limits& limits, u32 functionBeginAddr) {
  u32 headerAddr = functionBeginAddr - 8;

  auto* textSection = binary.findSection(headerAddr);
  if (!textSection || !textSection->data) {
    return std::nullopt;
  }

  u32 headerOffset = headerAddr - textSection->baseAddress;
  if (headerOffset + 8 > textSection->size) {
    return std::nullopt;
  }

  const auto* header = reinterpret_cast<const SehHeader*>(textSection->data + headerOffset);
  u32 handlerThunk = header->handlerThunk;
  u32 tableAddr = header->tableAddr;

  auto* rdataSection = binary.findSectionByName(".rdata");
  if (!rdataSection || !rdataSection->data) {
    return std::nullopt;
  }

  u32 rdataStart = rdataSection->baseAddress;
  u32 rdataSize = rdataSection->size;
  u32 rdataEnd = rdataStart + rdataSize;
  const u8* rdataBase = rdataSection->data;

  if (tableAddr < rdataStart || tableAddr >= rdataEnd) {
    return std::nullopt;
  }

  u32 tableOffset = tableAddr - rdataStart;
  if (tableOffset + 4 > rdataSize) {
    return std::nullopt;
  }

  u32 firstWord = memory::load<be_u32>(rdataBase + tableOffset);
  std::vector<u32> discoveredFuncs;

  if (firstWord == CXX_EH_MAGIC) {
    return parseCxxFuncInfo(limits, handlerThunk, tableAddr, rdataBase, rdataStart, rdataSize,
                            functionBeginAddr, discoveredFuncs);
  } else {
    u32 count = firstWord;
    if (count == 0 || count > limits.max_seh_scope_entries) {
      return std::nullopt;
    }
    return parseSehScopeTable(handlerThunk, tableAddr, count, rdataBase, rdataStart, rdataSize,
                              functionBeginAddr, discoveredFuncs);
  }
}

/**
 * Finds the first save and restore helper of each kind in the executable sections.
 *
 * @param binary Binary view.
 * @param state Receives the helper addresses.
 */
void detectSaveRestoreHelpers(const BinaryView& binary, AnalysisState& state) {
  struct HelperPattern {
    u32 pattern;
    u32* state_addr;
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

    const u8* data = section.data;
    const size_t size = section.size;
    const size_t base = section.baseAddress;

    for (size_t offset = 0; offset + 4 <= size; offset += 4) {
      u32 instr = memory::load<be_u32>(data + offset);
      u32 addr = static_cast<u32>(base + offset);

      for (auto& p : patterns) {
        if (instr == p.pattern && *p.state_addr == 0) {
          *p.state_addr = addr;
          REXLOG_DEBUG(codegen, "Found {} at 0x{:08X}", p.name, addr);
        }
      }

      if (offset + 8 <= size) {
        u32 next = memory::load<be_u32>(data + offset + 4);

        if (instr == 0x3960fee0) {
          if (next == 0x7dcb60ce && state.restVmx14Address == 0) {
            state.restVmx14Address = addr;
            REXLOG_DEBUG(codegen, "Found __restvmx_14 at 0x{:08X}", addr);
          } else if (next == 0x7dcb61ce && state.saveVmx14Address == 0) {
            state.saveVmx14Address = addr;
            REXLOG_DEBUG(codegen, "Found __savevmx_14 at 0x{:08X}", addr);
          }
        }

        if (instr == 0x3960fc00) {
          if (next == 0x100b60cb && state.restVmx64Address == 0) {
            state.restVmx64Address = addr;
            REXLOG_DEBUG(codegen, "Found __restvmx_64 at 0x{:08X}", addr);
          } else if (next == 0x100b61cb && state.saveVmx64Address == 0) {
            state.saveVmx64Address = addr;
            REXLOG_DEBUG(codegen, "Found __savevmx_64 at 0x{:08X}", addr);
          }
        }
      }
    }
  }
}

/**
 * Registers imports, save and restore helpers, config functions and PDATA functions. A restore
 * helper sits at the same offset from its base as the matching save helper, which gives the
 * restore helper of an SEH function. An __except handler is inline and becomes a label.
 *
 * @param ctx Codegen context.
 * @return Ok, or the failure.
 */
[[nodiscard]] Result<void> registerEntryPoints(CodegenContext& ctx) {
  REXLOG_TRACE(codegen, "Analyze: registering entry points...");

  auto& graph = ctx.graph;
  auto& config = ctx.Config();
  auto& state = ctx.analysisState();
  auto& binary = ctx.binary();
  auto& ehDiscoveredFuncs = state.ehDiscoveredFuncs;

  for (const auto& [addr, size] : config.invalidInstructionHints) {
    state.invalidInstructions[addr] = size;
  }
  for (u32 addr : config.knownIndirectCallHints) {
    state.knownIndirectCalls.insert(addr);
  }
  for (u32 addr : config.exceptionHandlerFuncHints) {
    state.exceptionHandlerFuncs.push_back(addr);
  }

  for (const auto& [addr, cfg] : config.functions) {
    if (cfg.isChunk()) {
      state.chunksByParent[cfg.parent].push_back(addr);
    }
  }

  detectSaveRestoreHelpers(binary, state);

  {
    size_t importCount = 0;
    size_t resolvedCount = 0;
    size_t variableCount = 0;
    size_t unresolvedCount = 0;

    for (const auto& sym : binary.importSymbols()) {
      auto at_pos = sym.name.find('@');
      if (at_pos == std::string::npos) {
        REXLOG_ERROR(codegen, "Invalid import format (missing @): {}", sym.name);
        continue;
      }

      auto lib_name = sym.name.substr(0, at_pos);
      auto ordinal_str = sym.name.substr(at_pos + 1);
      u16 ordinal = static_cast<u16>(std::stoul(ordinal_str));

      std::string resolvedName;
      const auto exportModule = runtime::ModuleFromName(lib_name);
      const auto* exp = exportModule ? runtime::FindByOrdinal(*exportModule, ordinal) : nullptr;
      if (exp) {
        if (exp->kind == runtime::ExportKind::Function) {
          resolvedName = "__imp__" + std::string(exp->name);
          resolvedCount++;
        } else {
          variableCount++;
          continue;
        }
      }

      if (resolvedName.empty()) {
        REXLOG_ERROR(codegen, "Cannot resolve ordinal {} from {}", ordinal, lib_name);
        resolvedName = std::format("sub_{:X}", sym.address);
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

    REXLOG_TRACE(codegen,
                 "Analyze: loaded {} imports ({} resolved, {} unresolved, {} variables skipped)",
                 importCount, resolvedCount, unresolvedCount, variableCount);
  }

  auto registerHelpers = [&](u32 base14, const char* prefix, size_t stride, size_t endReg,
                             size_t extraSize) {
    if (base14 == 0)
      return;
    for (size_t i = 14; i <= endReg; i++) {
      u32 addr = base14 + static_cast<u32>((i - 14) * stride);
      u32 size = static_cast<u32>((endReg + 1 - i) * stride + extraSize);
      graph.addFunction(addr, size, FunctionAuthority::HELPER, true);
      graph.setFunctionName(addr, std::format("{}{}", prefix, i));
    }
  };

  registerHelpers(state.restGpr14Address, "__restgprlr_", 4, 31, 12);
  registerHelpers(state.saveGpr14Address, "__savegprlr_", 4, 31, 8);
  registerHelpers(state.restFpr14Address, "__restfpr_", 4, 31, 4);
  registerHelpers(state.saveFpr14Address, "__savefpr_", 4, 31, 4);
  registerHelpers(state.restVmx14Address, "__restvmx_", 8, 31, 4);
  registerHelpers(state.saveVmx14Address, "__savevmx_", 8, 31, 4);

  if (state.restVmx64Address != 0) {
    for (size_t i = 64; i < 128; i++) {
      u32 addr = state.restVmx64Address + static_cast<u32>((i - 64) * 8);
      u32 size = static_cast<u32>((128 - i) * 8 + 4);
      graph.addFunction(addr, size, FunctionAuthority::HELPER, true);
      graph.setFunctionName(addr, std::format("__restvmx_{}", i));
    }
  }
  if (state.saveVmx64Address != 0) {
    for (size_t i = 64; i < 128; i++) {
      u32 addr = state.saveVmx64Address + static_cast<u32>((i - 64) * 8);
      u32 size = static_cast<u32>((128 - i) * 8 + 4);
      graph.addFunction(addr, size, FunctionAuthority::HELPER, true);
      graph.setFunctionName(addr, std::format("__savevmx_{}", i));
    }
  }

  size_t configFuncs = 0, configChunks = 0;
  for (const auto& [address, cfg] : config.functions) {
    u32 size = cfg.getSize(address);
    std::string name = cfg.name.empty() ? std::format("sub_{:08X}", address) : cfg.name;
    auto* node = graph.addFunction(address, size, FunctionAuthority::CONFIG, true);
    graph.setFunctionName(address, name);
    if (node && cfg.shareRegisters) {
      node->setSharesRegisters(true);
    }
    configFuncs++;

    if (cfg.isChunk()) {
      graph.registerChunk(address, size);
      configChunks++;
    }
  }
  if (configFuncs > 0) {
    REXLOG_DEBUG(codegen, "Analyze: {} CONFIG functions, {} chunks", configFuncs, configChunks);
  }

  u32 pdataAddr = binary.exceptionDirectoryAddr();
  u32 pdataSize = binary.exceptionDirectorySize();

  if (pdataSize == 0) {
    return Err(ErrorCategory::Format, "Exception DataDirectory not found (size=0)");
  }

  auto* pdataSection = binary.findSection(pdataAddr);
  if (!pdataSection) {
    return Err(ErrorCategory::Format,
               std::format("Cannot find section containing PDATA at 0x{:08X}", pdataAddr));
  }

  u32 offsetInSection = pdataAddr - pdataSection->baseAddress;
  const u8* pdataData = pdataSection->data + offsetInSection;

  REXLOG_TRACE(codegen, "Analyze: PDATA base=0x{:08X}, size={}", pdataAddr, pdataSize);

  size_t count = pdataSize / sizeof(IMAGE_CE_RUNTIME_FUNCTION);
  auto* entries = reinterpret_cast<const IMAGE_CE_RUNTIME_FUNCTION*>(pdataData);

  size_t pdataAdded = 0;
  for (size_t i = 0; i < count; i++) {
    u32 beginAddr = memory::load<be_u32>(&entries[i].BeginAddress);
    u32 data = memory::load<be_u32>(&entries[i].Data);

    IMAGE_CE_RUNTIME_FUNCTION fn;
    fn.BeginAddress = beginAddr;
    fn.Data = data;

    u32 size = fn.FunctionLength * 4;
    if (size == 0)
      size = 4;

    if (graph.getFunction(beginAddr) != nullptr) {
      continue;
    }

    std::optional<ParsedExceptionInfo> exInfo;
    PrologInfo prologInfo;
    if (fn.ExceptionFlag) {
      prologInfo = scanProlog(binary, beginAddr, fn.PrologLength);

      exInfo = parseExceptionInfo(binary, ctx.limits(), beginAddr);
      if (exInfo) {
        if (exInfo->maxAddress > beginAddr + size) {
          size = exInfo->maxAddress - beginAddr + 4;
        }
        state.invalidInstructions[beginAddr - 8] = 8;

        for (u32 addr : exInfo->discoveredFuncs) {
          ehDiscoveredFuncs.push_back(addr);
        }

        if (exInfo->info.isSeh()) {
          auto& sehInfo = std::get<SehExceptionInfo>(exInfo->info.data);
          sehInfo.frameSize = prologInfo.frameSize;

          if (!prologInfo.valid && !sehInfo.scopes.empty()) {
            REXLOG_WARN(codegen,
                        "SEH function 0x{:08X}: could not determine frame size from prolog",
                        beginAddr);
          }

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

  REXLOG_TRACE(codegen, "Analyze: added {} functions from PDATA", pdataAdded);

  size_t ehFuncsQueued = 0;
  for (u32 addr : ehDiscoveredFuncs) {
    if (graph.getFunction(addr) != nullptr)
      continue;
    if (graph.isImport(addr))
      continue;

    graph.addFunction(addr, 0, FunctionAuthority::DISCOVERED, true);
    ehFuncsQueued++;
  }

  if (ehFuncsQueued > 0) {
    REXLOG_TRACE(codegen, "Analyze: queued {} functions from exception handling", ehFuncsQueued);
  }

  return Ok();
}

}  // anonymous namespace

namespace phases {

[[nodiscard]] Result<void> Register(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  return registerEntryPoints(ctx);
}

}  // namespace phases

}  // namespace rex::codegen
