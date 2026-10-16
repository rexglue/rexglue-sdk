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
#include <rex/logging/log.h>

#include <rex/memory/byte_swap.h>
#include <rex/runtime/exports.h>
#include <rex/types.h>

#include <ppc.h>

using rex::codegen::detail::DecodeInstruction;
using rex::codegen::detail::Opcode;

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

struct SEHHeader {
  be_u32 handler_thunk;
  be_u32 table_addr;
};
REX_STATIC_ASSERT_SIZE(SEHHeader, 8);

struct SEHScopeEntry {
  be_u32 try_start;
  be_u32 try_end;
  be_u32 filter;
  be_u32 handler;
};
REX_STATIC_ASSERT_SIZE(SEHScopeEntry, 16);

struct ParsedExceptionInfo {
  ExceptionInfo info;
  u32 max_address;
  std::vector<u32> discovered_funcs;
};

/** Frame size and save helper read from a function prologue for SEH unwinding. */
struct PrologInfo {
  /** Frame size from addi r31, r1, -N. */
  u32 frame_size = 0;
  /** Target of the bl to the save helper. */
  u32 save_helper = 0;
  bool valid = false;
};

/**
 * Scans the prologue of a function with ExceptionFlag set for its frame size and save helper.
 * Scanning is skipped when the PDATA prolog length is 0. valid may be false for handler
 * functions that receive the frame in r12, so the caller warns only when SEH scopes need it.
 *
 * @param binary Binary view.
 * @param func_addr Function address.
 * @param prolog_length Prolog length in instructions from PDATA.
 * @return The prologue info.
 */
PrologInfo ScanProlog(const BinaryView& binary, u32 func_addr, u32 prolog_length) {
  PrologInfo info;

  auto* section = binary.FindSection(func_addr);
  if (!section || !section->data) {
    return info;
  }

  u32 offset = func_addr - section->base_address;
  if (offset + prolog_length * 4 > section->size) {
    return info;
  }

  const u8* code = section->data + offset;

  if (prolog_length == 0) {
    REXLOG_WARN(codegen, "SEH function 0x{:08X}: PrologLength=0, cannot determine frame size",
                func_addr);
    return info;
  }

  for (u32 i = 0; i < prolog_length; i++) {
    u32 raw = memory::load<be_u32>(code + size_t{i} * 4);
    u32 addr = func_addr + i * 4;
    auto decoded = DecodeInstruction(addr, raw);

    if (decoded.opcode == Opcode::addi && decoded.D.RT == 31 && decoded.D.RA == 1) {
      i32 simm = decoded.D.SIMM();
      if (simm < 0) {
        info.frame_size = static_cast<u32>(-simm);
        info.valid = true;
      }
    }

    if (decoded.IsCall() && decoded.branch_target.has_value()) {
      info.save_helper = decoded.branch_target.value();
    }
  }

  return info;
}

/**
 * Parses an SEH scope table. A __finally scope holds the handler in word 2 and 0 in word 3. An
 * __except scope holds the filter in word 2 and the handler in word 3. A __finally handler and
 * an __except filter are separate functions and an __except handler is inline.
 *
 * @param handler_thunk Address of the handler thunk.
 * @param scope_table_addr Address of the scope table.
 * @param count Scope count.
 * @param rdata_base Host pointer to the start of .rdata.
 * @param rdata_start Address of the start of .rdata.
 * @param rdata_size Size of .rdata in bytes.
 * @param function_begin_addr Address of the function.
 * @param discovered_funcs Receives the functions the table names.
 * @return The parsed info, or nullopt when the table is outside .rdata.
 */
std::optional<ParsedExceptionInfo> ParseSEHScopeTable(u32 handler_thunk, u32 scope_table_addr,
                                                      u32 count, const u8* rdata_base,
                                                      u32 rdata_start, u32 rdata_size,
                                                      u32 function_begin_addr,
                                                      std::vector<u32>& discovered_funcs) {
  u32 table_offset = scope_table_addr - rdata_start;
  if (table_offset + 4 + count * 16 > rdata_size) {
    return std::nullopt;
  }

  SEHExceptionInfo seh_info;
  seh_info.handler_thunk = handler_thunk;
  seh_info.scope_table_addr = scope_table_addr;

  u32 max_addr = function_begin_addr;

  const auto* entries = reinterpret_cast<const SEHScopeEntry*>(rdata_base + table_offset + 4);
  for (u32 i = 0; i < count; i++) {
    SEHScope scope;
    scope.try_start = entries[i].try_start;
    scope.try_end = entries[i].try_end;
    scope.filter = entries[i].filter;
    scope.handler = entries[i].handler;

    if (scope.handler == 0 && scope.filter != 0) {
      scope.handler = scope.filter;
      scope.filter = 0;
    }

    seh_info.scopes.push_back(scope);

    if (scope.try_start != 0 && scope.try_start > max_addr)
      max_addr = scope.try_start;
    if (scope.try_end != 0 && scope.try_end > max_addr)
      max_addr = scope.try_end;
    if (scope.filter != 0 && scope.filter > max_addr)
      max_addr = scope.filter;
    if (scope.handler != 0 && scope.handler > max_addr)
      max_addr = scope.handler;

    if (scope.filter == 0 && scope.handler != 0) {
      discovered_funcs.push_back(scope.handler);
    }
    if (scope.filter != 0) {
      discovered_funcs.push_back(scope.filter);
    }
  }

  discovered_funcs.push_back(handler_thunk);

  ParsedExceptionInfo result;
  result.info.data = std::move(seh_info);
  result.max_address = max_addr;
  result.discovered_funcs = std::move(discovered_funcs);
  return result;
}

std::optional<ParsedExceptionInfo> ParseCxxFuncInfo(const Limits& limits, u32 handler_thunk,
                                                    u32 func_info_addr, const u8* rdata_base,
                                                    u32 rdata_start, u32 rdata_size,
                                                    u32 function_begin_addr,
                                                    std::vector<u32>& discovered_funcs) {
  u32 table_offset = func_info_addr - rdata_start;
  if (table_offset + 28 > rdata_size) {
    return std::nullopt;
  }

  auto read_u32 = [&](u32 addr) { return memory::load<be_u32>(rdata_base + (addr - rdata_start)); };
  auto read_i32 = [&](u32 addr) {
    return i32{memory::load<be_i32>(rdata_base + (addr - rdata_start))};
  };

  CxxExceptionInfo cxx_info;
  cxx_info.handler_thunk = handler_thunk;
  cxx_info.func_info_addr = func_info_addr;

  u32 magic = read_u32(func_info_addr + 0);
  cxx_info.max_state = read_u32(func_info_addr + 4);
  u32 unwind_map = read_u32(func_info_addr + 8);
  u32 try_block_count = read_u32(func_info_addr + 12);
  u32 try_block_map = read_u32(func_info_addr + 16);
  u32 ip_map_entry_count = read_u32(func_info_addr + 20);
  u32 ip_to_state_map = read_u32(func_info_addr + 24);

  if (magic != kCXXEHMagic) {
    return std::nullopt;
  }

  if (cxx_info.max_state > limits.max_eh_states || try_block_count > limits.max_eh_try_blocks ||
      ip_map_entry_count > limits.max_eh_ip_map_entries) {
    return std::nullopt;
  }

  u32 max_addr = function_begin_addr;
  discovered_funcs.push_back(handler_thunk);

  if (unwind_map != 0 && cxx_info.max_state > 0) {
    if (unwind_map >= rdata_start &&
        unwind_map + cxx_info.max_state * 8 <= rdata_start + rdata_size) {
      for (u32 i = 0; i < cxx_info.max_state; i++) {
        CxxUnwindEntry entry;
        entry.to_state = read_i32(unwind_map + i * 8);
        entry.action = read_u32(unwind_map + i * 8 + 4);
        cxx_info.unwind_map.push_back(entry);

        if (entry.action != 0) {
          discovered_funcs.push_back(entry.action);
        }
      }
    }
  }

  if (try_block_map != 0 && try_block_count > 0) {
    if (try_block_map >= rdata_start &&
        try_block_map + try_block_count * 20 <= rdata_start + rdata_size) {
      for (u32 i = 0; i < try_block_count; i++) {
        u32 entry_addr = try_block_map + i * 20;
        CxxTryBlock try_block;
        try_block.try_low = read_i32(entry_addr + 0);
        try_block.try_high = read_i32(entry_addr + 4);
        try_block.catch_high = read_i32(entry_addr + 8);
        u32 catch_count = read_u32(entry_addr + 12);
        u32 handlers = read_u32(entry_addr + 16);

        if (handlers != 0 && catch_count > 0 && catch_count <= 20) {
          if (handlers >= rdata_start && handlers + catch_count * 16 <= rdata_start + rdata_size) {
            for (u32 j = 0; j < catch_count; j++) {
              u32 handler_entry_addr = handlers + j * 16;
              CxxCatchHandler handler;
              handler.adjectives = read_u32(handler_entry_addr + 0);
              handler.type_descriptor = read_u32(handler_entry_addr + 4);
              handler.catch_obj_displacement = read_i32(handler_entry_addr + 8);
              handler.handler_address = read_u32(handler_entry_addr + 12);
              try_block.handlers.push_back(handler);

              if (handler.handler_address != 0) {
                discovered_funcs.push_back(handler.handler_address);
              }
            }
          }
        }

        cxx_info.try_blocks.push_back(std::move(try_block));
      }
    }
  }

  if (ip_to_state_map != 0 && ip_map_entry_count > 0) {
    if (ip_to_state_map >= rdata_start &&
        ip_to_state_map + ip_map_entry_count * 8 <= rdata_start + rdata_size) {
      for (u32 i = 0; i < ip_map_entry_count; i++) {
        CxxIPStateEntry entry;
        entry.ip = read_u32(ip_to_state_map + i * 8);
        entry.state = read_i32(ip_to_state_map + i * 8 + 4);
        cxx_info.ip_to_state_map.push_back(entry);

        if (entry.ip != 0 && entry.ip > max_addr) {
          max_addr = entry.ip;
        }
      }
    }
  }

  ParsedExceptionInfo result;
  result.info.data = std::move(cxx_info);
  result.max_address = max_addr;
  result.discovered_funcs = std::move(discovered_funcs);
  return result;
}

std::optional<ParsedExceptionInfo> ParseExceptionInfo(const BinaryView& binary,
                                                      const Limits& limits,
                                                      u32 function_begin_addr) {
  u32 header_addr = function_begin_addr - 8;

  auto* text_section = binary.FindSection(header_addr);
  if (!text_section || !text_section->data) {
    return std::nullopt;
  }

  u32 header_offset = header_addr - text_section->base_address;
  if (header_offset + 8 > text_section->size) {
    return std::nullopt;
  }

  const auto* header = reinterpret_cast<const SEHHeader*>(text_section->data + header_offset);
  u32 handler_thunk = header->handler_thunk;
  u32 table_addr = header->table_addr;

  auto* rdata_section = binary.FindSectionByName(".rdata");
  if (!rdata_section || !rdata_section->data) {
    return std::nullopt;
  }

  u32 rdata_start = rdata_section->base_address;
  u32 rdata_size = rdata_section->size;
  u32 rdata_end = rdata_start + rdata_size;
  const u8* rdata_base = rdata_section->data;

  if (table_addr < rdata_start || table_addr >= rdata_end) {
    return std::nullopt;
  }

  u32 table_offset = table_addr - rdata_start;
  if (table_offset + 4 > rdata_size) {
    return std::nullopt;
  }

  u32 first_word = memory::load<be_u32>(rdata_base + table_offset);
  std::vector<u32> discovered_funcs;

  if (first_word == kCXXEHMagic) {
    return ParseCxxFuncInfo(limits, handler_thunk, table_addr, rdata_base, rdata_start, rdata_size,
                            function_begin_addr, discovered_funcs);
  } else {
    u32 count = first_word;
    if (count == 0 || count > limits.max_seh_scope_entries) {
      return std::nullopt;
    }
    return ParseSEHScopeTable(handler_thunk, table_addr, count, rdata_base, rdata_start, rdata_size,
                              function_begin_addr, discovered_funcs);
  }
}

/**
 * Finds the first save and restore helper of each kind in the executable sections.
 *
 * @param binary Binary view.
 * @param state Receives the helper addresses.
 */
void DetectSaveRestoreHelpers(const BinaryView& binary, AnalysisState& state) {
  struct HelperPattern {
    u32 pattern;
    u32* state_addr;
    const char* name;
  };

  HelperPattern patterns[] = {
      {0xe9c1ff68, &state.rest_gpr14_address, "__restgprlr_14"},
      {0xf9c1ff68, &state.save_gpr14_address, "__savegprlr_14"},
      {0xc9ccff70, &state.rest_fpr14_address, "__restfpr_14"},
      {0xd9ccff70, &state.save_fpr14_address, "__savefpr_14"},
  };

  for (const auto& section : binary.GetSections()) {
    if (!section.executable)
      continue;

    const u8* data = section.data;
    const size_t size = section.size;
    const size_t base = section.base_address;

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
          if (next == 0x7dcb60ce && state.rest_vmx14_address == 0) {
            state.rest_vmx14_address = addr;
            REXLOG_DEBUG(codegen, "Found __restvmx_14 at 0x{:08X}", addr);
          } else if (next == 0x7dcb61ce && state.save_vmx14_address == 0) {
            state.save_vmx14_address = addr;
            REXLOG_DEBUG(codegen, "Found __savevmx_14 at 0x{:08X}", addr);
          }
        }

        if (instr == 0x3960fc00) {
          if (next == 0x100b60cb && state.rest_vmx64_address == 0) {
            state.rest_vmx64_address = addr;
            REXLOG_DEBUG(codegen, "Found __restvmx_64 at 0x{:08X}", addr);
          } else if (next == 0x100b61cb && state.save_vmx64_address == 0) {
            state.save_vmx64_address = addr;
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
[[nodiscard]] Result<void> RegisterEntryPoints(CodegenContext& ctx) {
  REXLOG_TRACE(codegen, "Analyze: registering entry points...");

  auto& graph = ctx.graph;
  auto& config = ctx.GetConfig();
  auto& state = ctx.GetAnalysisState();
  auto& binary = ctx.GetBinary();
  auto& eh_discovered_funcs = state.eh_discovered_funcs;

  for (const auto& [addr, size] : config.invalid_instruction_hints) {
    state.invalid_instructions[addr] = size;
  }
  for (u32 addr : config.known_indirect_call_hints) {
    state.known_indirect_calls.insert(addr);
  }
  for (u32 addr : config.exception_handler_func_hints) {
    state.exception_handler_funcs.push_back(addr);
  }

  for (const auto& [addr, cfg] : config.functions) {
    if (cfg.IsChunk()) {
      state.chunks_by_parent[cfg.parent].push_back(addr);
    }
  }

  DetectSaveRestoreHelpers(binary, state);

  {
    size_t import_count = 0;
    size_t resolved_count = 0;
    size_t variable_count = 0;
    size_t unresolved_count = 0;
    size_t module_import_count = 0;

    for (const auto& sym : binary.GetImportSymbols()) {
      auto at_pos = sym.name.find('@');
      if (at_pos == std::string::npos) {
        REXLOG_ERROR(codegen, "Invalid import format (missing @): {}", sym.name);
        continue;
      }

      auto lib_name = sym.name.substr(0, at_pos);
      auto ordinal_str = sym.name.substr(at_pos + 1);
      u16 ordinal = static_cast<u16>(std::stoul(ordinal_str));

      std::string resolved_name;
      const auto export_module = runtime::ModuleFromName(lib_name);
      const auto* exp = export_module ? runtime::FindByOrdinal(*export_module, ordinal) : nullptr;
      if (exp) {
        if (exp->kind == runtime::ExportKind::kFunction) {
          resolved_name = "__imp__" + std::string(exp->name);
          resolved_count++;
        } else {
          variable_count++;
          continue;
        }
      }

      if (resolved_name.empty() && !export_module) {
        if (sym.address == 0) {
          continue;
        }
        resolved_name = std::format("sub_{:X}", sym.address);
        state.module_imports[sym.address] = {lib_name, ordinal};
        module_import_count++;
      } else if (resolved_name.empty()) {
        REXLOG_ERROR(codegen, "Cannot resolve ordinal {} from {}", ordinal, lib_name);
        resolved_name = std::format("sub_{:X}", sym.address);
        unresolved_count++;
      }

      auto* node = graph.AddImportFunction(sym.address, resolved_name);
      if (node && node->CanDiscover()) {
        node->DiscoverAsImport();
        if (node->CanSeal()) {
          node->Seal();
        }
      }
      import_count++;
    }

    REXLOG_TRACE(codegen,
                 "Analyze: loaded {} imports ({} resolved, {} from other modules, {} unresolved, "
                 "{} variables skipped)",
                 import_count, resolved_count, module_import_count, unresolved_count,
                 variable_count);
  }

  auto register_helpers = [&](u32 base14, const char* prefix, size_t stride, size_t end_reg,
                              size_t extra_size) {
    if (base14 == 0)
      return;
    for (size_t i = 14; i <= end_reg; i++) {
      u32 addr = base14 + static_cast<u32>((i - 14) * stride);
      u32 size = static_cast<u32>((end_reg + 1 - i) * stride + extra_size);
      graph.AddFunction(addr, size, FunctionAuthority::kHelper, true);
      graph.SetFunctionName(addr, std::format("{}{}", prefix, i));
    }
  };

  register_helpers(state.rest_gpr14_address, "__restgprlr_", 4, 31, 12);
  register_helpers(state.save_gpr14_address, "__savegprlr_", 4, 31, 8);
  register_helpers(state.rest_fpr14_address, "__restfpr_", 4, 31, 4);
  register_helpers(state.save_fpr14_address, "__savefpr_", 4, 31, 4);
  register_helpers(state.rest_vmx14_address, "__restvmx_", 8, 31, 4);
  register_helpers(state.save_vmx14_address, "__savevmx_", 8, 31, 4);

  if (state.rest_vmx64_address != 0) {
    for (size_t i = 64; i < 128; i++) {
      u32 addr = state.rest_vmx64_address + static_cast<u32>((i - 64) * 8);
      u32 size = static_cast<u32>((128 - i) * 8 + 4);
      graph.AddFunction(addr, size, FunctionAuthority::kHelper, true);
      graph.SetFunctionName(addr, std::format("__restvmx_{}", i));
    }
  }
  if (state.save_vmx64_address != 0) {
    for (size_t i = 64; i < 128; i++) {
      u32 addr = state.save_vmx64_address + static_cast<u32>((i - 64) * 8);
      u32 size = static_cast<u32>((128 - i) * 8 + 4);
      graph.AddFunction(addr, size, FunctionAuthority::kHelper, true);
      graph.SetFunctionName(addr, std::format("__savevmx_{}", i));
    }
  }

  size_t config_funcs = 0, config_chunks = 0;
  for (const auto& [address, cfg] : config.functions) {
    u32 size = cfg.GetSize(address);
    std::string name = cfg.name.empty() ? std::format("sub_{:08X}", address) : cfg.name;
    auto* node = graph.AddFunction(address, size, FunctionAuthority::kConfig, true);
    graph.SetFunctionName(address, name);
    if (node && cfg.share_registers) {
      node->SetSharesRegisters(true);
    }
    config_funcs++;

    if (cfg.IsChunk()) {
      graph.RegisterChunk(address, size);
      config_chunks++;
    }
  }
  if (config_funcs > 0) {
    REXLOG_DEBUG(codegen, "Analyze: {} CONFIG functions, {} chunks", config_funcs, config_chunks);
  }

  u32 pdata_addr = binary.GetExceptionDirectoryAddr();
  u32 pdata_size = binary.GetExceptionDirectorySize();

  if (pdata_size == 0) {
    return Err(ErrorCategory::kFormat, "Exception DataDirectory not found (size=0)");
  }

  auto* pdata_section = binary.FindSection(pdata_addr);
  if (!pdata_section) {
    return Err(ErrorCategory::kFormat,
               std::format("Cannot find section containing PDATA at 0x{:08X}", pdata_addr));
  }

  u32 offset_in_section = pdata_addr - pdata_section->base_address;
  const u8* pdata_data = pdata_section->data + offset_in_section;

  REXLOG_TRACE(codegen, "Analyze: PDATA base=0x{:08X}, size={}", pdata_addr, pdata_size);

  size_t count = pdata_size / sizeof(IMAGE_CE_RUNTIME_FUNCTION);
  auto* entries = reinterpret_cast<const IMAGE_CE_RUNTIME_FUNCTION*>(pdata_data);

  size_t pdata_added = 0;
  for (size_t i = 0; i < count; i++) {
    u32 begin_addr = memory::load<be_u32>(&entries[i].BeginAddress);
    u32 data = memory::load<be_u32>(&entries[i].Data);

    IMAGE_CE_RUNTIME_FUNCTION fn;
    fn.BeginAddress = begin_addr;
    fn.Data = data;

    u32 size = fn.FunctionLength * 4;
    if (size == 0)
      size = 4;

    if (graph.GetFunction(begin_addr) != nullptr) {
      continue;
    }

    std::optional<ParsedExceptionInfo> ex_info;
    PrologInfo prolog_info;
    if (fn.ExceptionFlag) {
      prolog_info = ScanProlog(binary, begin_addr, fn.PrologLength);

      ex_info = ParseExceptionInfo(binary, ctx.GetLimits(), begin_addr);
      if (ex_info) {
        if (ex_info->max_address > begin_addr + size) {
          size = ex_info->max_address - begin_addr + 4;
        }
        state.invalid_instructions[begin_addr - 8] = 8;

        for (u32 addr : ex_info->discovered_funcs) {
          eh_discovered_funcs.push_back(addr);
        }

        if (ex_info->info.IsSEH()) {
          auto& seh_info = std::get<SEHExceptionInfo>(ex_info->info.data);
          seh_info.frame_size = prolog_info.frame_size;

          if (!prolog_info.valid && !seh_info.scopes.empty()) {
            REXLOG_WARN(codegen,
                        "SEH function 0x{:08X}: could not determine frame size from prolog",
                        begin_addr);
          }

          if (prolog_info.save_helper != 0 && state.rest_gpr14_address != 0 &&
              state.save_gpr14_address != 0) {
            seh_info.restore_helper =
                prolog_info.save_helper + (state.rest_gpr14_address - state.save_gpr14_address);
          }
        }
      }
    }

    graph.AddFunction(begin_addr, size, FunctionAuthority::kPData, true);
    graph.SetFunctionHasExceptionHandler(begin_addr, fn.ExceptionFlag);

    if (ex_info && ex_info->info.HasInfo()) {
      if (auto* seh = ex_info->info.AsSEH()) {
        for (const auto& scope : seh->scopes) {
          if (scope.try_start != 0)
            graph.AddLabelToFunction(begin_addr, scope.try_start);
          if (scope.try_end != 0)
            graph.AddLabelToFunction(begin_addr, scope.try_end);
          if (scope.filter != 0 && scope.handler != 0) {
            graph.AddLabelToFunction(begin_addr, scope.handler);
          }
        }
      } else if (auto* cxx = ex_info->info.AsCxx()) {
        for (const auto& entry : cxx->ip_to_state_map) {
          if (entry.ip != 0)
            graph.AddLabelToFunction(begin_addr, entry.ip);
        }
      }
      graph.SetFunctionExceptionInfo(begin_addr, std::move(ex_info->info));
    }

    ctx.scan.pdata_sizes[begin_addr] = size;
    pdata_added++;
  }

  REXLOG_TRACE(codegen, "Analyze: added {} functions from PDATA", pdata_added);

  size_t eh_funcs_queued = 0;
  for (u32 addr : eh_discovered_funcs) {
    if (graph.GetFunction(addr) != nullptr)
      continue;
    if (graph.IsImport(addr))
      continue;

    graph.AddFunction(addr, 0, FunctionAuthority::kDiscovered, true);
    eh_funcs_queued++;
  }

  if (eh_funcs_queued > 0) {
    REXLOG_TRACE(codegen, "Analyze: queued {} functions from exception handling", eh_funcs_queued);
  }

  return Ok();
}

}  // anonymous namespace

namespace detail {

[[nodiscard]] Result<void> Register(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  return RegisterEntryPoints(ctx);
}

}  // namespace detail

}  // namespace rex::codegen
