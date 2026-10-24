/**
 * @file        codegen/phase_register.cpp
 * @brief       Register phase for imports, helpers, PDATA and config functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc/disasm.h"
#include "ppc/instruction.h"
#include "ppc/opcode.h"

#include <rex/codegen/phases.h>
#include "known_functions.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/assert.h>
#include <rex/codegen/config.h>
#include <rex/logging/log.h>

#include <rex/memory/byte_swap.h>
#include <rex/ppc/pdata.h>
#include <rex/runtime/exports.h>
#include <rex/types.h>

#include <ppc.h>

using rex::codegen::detail::DecodeInstruction;
using rex::codegen::detail::Disassemble;
using rex::codegen::detail::LookupID;
using rex::codegen::detail::Opcode;

namespace rex::codegen {

namespace {

struct RuntimeFunctionWords {
  be_u32 begin;
  be_u32 word;
};
REX_STATIC_ASSERT_SIZE(RuntimeFunctionWords, 8);

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

struct ParsedException {
  ExceptionInfo info;
  u32 max_address;
  std::vector<u32> discovered_funcs;
};

/** Frame size and calls read from a function prologue for SEH unwinding. */
struct Prolog {
  /** Frame size from addi r31, r1, -N. */
  u32 frame_size = 0;
  /** Targets of the calls in the prologue, in order. */
  std::vector<u32> call_targets;
  bool valid = false;
};

/**
 * Scans the prologue of a function with handler data for its frame size and calls.
 * Scanning is skipped when the PDATA prolog length is 0. valid may be false for handler
 * functions that receive the frame in r12.
 *
 * @param binary Binary view.
 * @param func_addr Function address.
 * @param prolog_length Prolog length in instructions from PDATA.
 * @return The prologue info.
 */
Prolog ScanProlog(const BinaryView& binary, u32 func_addr, u32 prolog_length) {
  Prolog info;

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
      info.call_targets.push_back(decoded.branch_target.value());
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
std::optional<ParsedException> ParseSEHScopeTable(u32 handler_thunk, u32 scope_table_addr,
                                                  u32 count, const u8* rdata_base, u32 rdata_start,
                                                  u32 rdata_size, u32 function_begin_addr,
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

  ParsedException result;
  result.info.data = std::move(seh_info);
  result.max_address = max_addr;
  result.discovered_funcs = std::move(discovered_funcs);
  return result;
}

std::optional<ParsedException> ParseCxxFuncInfo(const Limits& limits, u32 handler_thunk,
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

  if (magic != kCxxEHMagic) {
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

  ParsedException result;
  result.info.data = std::move(cxx_info);
  result.max_address = max_addr;
  result.discovered_funcs = std::move(discovered_funcs);
  return result;
}

std::optional<ParsedException> ParseExceptionInfo(const BinaryView& binary, const Limits& limits,
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

  if (first_word == kCxxEHMagic) {
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
 * Measures an import thunk, which runs from its address through the bctr that follows an mtctr.
 * Any words before the mtctr are whatever the loader leaves there. A thunk that does not end that
 * way within a few words counts as 1 word.
 *
 * @param binary Binary view.
 * @param address Thunk address.
 * @return The size of the thunk in bytes.
 */
u32 GetImportThunkSize(const BinaryView& binary, u32 address) {
  constexpr u32 kMaxThunkWords = 8;
  constexpr u32 kFallbackSize = 4;
  Opcode previous = Opcode::kUnknown;
  for (u32 i = 0; i < kMaxThunkWords; ++i) {
    const u32 word_addr = address + i * 4;
    const u8* code = binary.Translate(word_addr);
    if (!code)
      break;
    const auto decoded = DecodeInstruction(word_addr, memory::load<be_u32>(code));
    if (decoded.opcode == Opcode::bcctr && !decoded.IsConditional() && previous == Opcode::mtctr)
      return (i + 1) * 4;
    if (IsTerminatorInstruction(decoded.opcode))
      break;
    previous = decoded.opcode;
  }
  REXLOG_DEBUG(codegen, "Import thunk at 0x{:08X} does not end in mtctr and bctr, sized as 1 word",
               address);
  return kFallbackSize;
}

/** Instruction of a millicode entry or tail as the disassembler prints it. */
struct MillicodeWord {
  std::string_view name;
  std::string operands;
};

constexpr u32 kGPRFirst = 14;
constexpr u32 kGPRWithR13 = 13;
constexpr u32 kVMXExtendedFirst = 64;
constexpr u32 kSavedRegisterCount = 18;
constexpr u32 kVMXExtendedCount = 64;
constexpr u32 kGPRSlot = 8;
constexpr u32 kFPRSlot = 8;
constexpr u32 kVMXSlot = 16;
constexpr u32 kGPRSlotBase = 33;
constexpr u32 kFPRSlotBase = 32;
constexpr u32 kVMXSlotBase = 32;
constexpr u32 kVMXExtendedSlotBase = 128;
constexpr u32 kWordBytes = 4;
constexpr size_t kRunClassCount = 8;

bool IsSaveKind(MillicodeKind kind) {
  return kind == MillicodeKind::kGPRSave || kind == MillicodeKind::kFPRSave ||
         kind == MillicodeKind::kVMXSave;
}

bool IsGPRKind(MillicodeKind kind) {
  return kind == MillicodeKind::kGPRSave || kind == MillicodeKind::kGPRRestore;
}

bool IsFPRKind(MillicodeKind kind) {
  return kind == MillicodeKind::kFPRSave || kind == MillicodeKind::kFPRRestore;
}

u32 GetEntryWordCount(MillicodeKind kind) {
  return IsGPRKind(kind) || IsFPRKind(kind) ? 1 : 2;
}

std::string_view GetMillicodePrefix(MillicodeKind kind) {
  switch (kind) {
    case MillicodeKind::kGPRSave:
      return "__savegprlr_";
    case MillicodeKind::kGPRRestore:
      return "__restgprlr_";
    case MillicodeKind::kFPRSave:
      return "__savefpr_";
    case MillicodeKind::kFPRRestore:
      return "__restfpr_";
    case MillicodeKind::kVMXSave:
      return "__savevmx_";
    case MillicodeKind::kVMXRestore:
      return "__restvmx_";
  }
  return {};
}

/**
 * Lists the instructions of the entry for a register.
 *
 * @param kind The run kind.
 * @param reg Register the entry starts at.
 * @return The instructions the entry must hold.
 */
std::vector<MillicodeWord> GetEntryWords(MillicodeKind kind, u32 reg) {
  const bool save = IsSaveKind(kind);
  if (IsGPRKind(kind)) {
    return {{save ? "std" : "ld",
             std::format("r{},{}(r1)", reg, -static_cast<i32>(kGPRSlot * (kGPRSlotBase - reg)))}};
  }
  if (IsFPRKind(kind)) {
    return {{save ? "stfd" : "lfd",
             std::format("f{},{}(r12)", reg, -static_cast<i32>(kFPRSlot * (kFPRSlotBase - reg)))}};
  }
  const bool extended = reg >= kVMXExtendedFirst;
  const u32 slot_base = extended ? kVMXExtendedSlotBase : kVMXSlotBase;
  const std::string_view name = extended ? (save ? "stvx128" : "lvx128") : (save ? "stvx" : "lvx");
  return {{"li", std::format("r11,{}", -static_cast<i32>(kVMXSlot * (slot_base - reg)))},
          {name, std::format("v{},r11,r12", reg)}};
}

/**
 * Lists the instructions after the last entry of a run.
 *
 * @param kind The run kind.
 * @return The instructions that close the run.
 */
std::vector<MillicodeWord> GetTailWords(MillicodeKind kind) {
  if (kind == MillicodeKind::kGPRSave)
    return {{"stw", "r12,-8(r1)"}, {"blr", ""}};
  if (kind == MillicodeKind::kGPRRestore)
    return {{"lwz", "r12,-8(r1)"}, {"mtlr", "r12"}, {"blr", ""}};
  return {{"blr", ""}};
}

bool WordMatches(const BinaryView& binary, u32 addr, const MillicodeWord& expected) {
  const u8* code = binary.Translate(addr);
  if (!code)
    return false;
  ppc_insn insn;
  if (Disassemble(code, addr, insn) <= 0 || insn.opcode == nullptr)
    return false;
  return expected.name == insn.opcode->name && expected.operands == insn.op_str;
}

bool WordsMatch(const BinaryView& binary, u32 addr, const std::vector<MillicodeWord>& expected) {
  for (const auto& word : expected) {
    if (!WordMatches(binary, addr, word))
      return false;
    addr += kWordBytes;
  }
  return true;
}

/**
 * Validates a run from the address of its first entry.
 *
 * @param binary The binary view the words are read from.
 * @param head The address of the first store or load word.
 * @param kind The kind to validate against.
 * @param first Number of the first register of the run.
 * @param count Number of entries the run must have.
 * @return The run, or no value when any entry or the closing return is wrong.
 */
std::optional<MillicodeRun> ValidateRunFrom(const BinaryView& binary, u32 head, MillicodeKind kind,
                                            u32 first, u32 count) {
  const u32 stride = GetEntryWordCount(kind) * kWordBytes;
  for (u32 i = 0; i < count; ++i) {
    if (!WordsMatch(binary, head + i * stride, GetEntryWords(kind, first + i)))
      return std::nullopt;
  }
  if (!WordsMatch(binary, head + count * stride, GetTailWords(kind)))
    return std::nullopt;
  return MillicodeRun{kind, head, first, count, stride};
}

/**
 * Validates a candidate run from its first word.
 *
 * @param binary The binary view the words are read from.
 * @param head The address of the first store or load word.
 * @param kind The kind to validate against.
 * @return The run, or no value when any entry or the closing return is wrong.
 */
std::optional<MillicodeRun> ValidateMillicodeRun(const BinaryView& binary, u32 head,
                                                 MillicodeKind kind) {
  if (auto run = ValidateRunFrom(binary, head, kind, kGPRFirst, kSavedRegisterCount)) {
    if (IsGPRKind(kind) &&
        WordsMatch(binary, head - kWordBytes, GetEntryWords(kind, kGPRWithR13))) {
      return MillicodeRun{kind, head - kWordBytes, kGPRWithR13, run->count + 1, run->stride};
    }
    return run;
  }
  if (GetEntryWordCount(kind) == 2)
    return ValidateRunFrom(binary, head, kind, kVMXExtendedFirst, kVMXExtendedCount);
  return std::nullopt;
}

/**
 * Reports whether 2 runs are the same run class, which is a kind and either the standard or the
 * extended vector range.
 *
 * @param run A run.
 * @param kind The other run kind.
 * @param first First register of the other run.
 * @return True when both belong to the same class.
 */
bool IsSameRunClass(const MillicodeRun& run, MillicodeKind kind, u32 first) {
  return run.kind == kind && (run.first >= kVMXExtendedFirst) == (first >= kVMXExtendedFirst);
}

/**
 * Finds the first valid run of each class in the executable sections.
 *
 * @param binary Binary view.
 * @param state Receives the runs.
 */
void DetectMillicodeRuns(const BinaryView& binary, AnalysisState& state) {
  auto& runs = state.millicode_runs;
  for (const auto& section : binary.GetSections()) {
    if (!section.executable)
      continue;

    for (u32 offset = 0; offset + kWordBytes <= section.size; offset += kWordBytes) {
      const u32 addr = section.base_address + offset;
      std::vector<MillicodeKind> kinds;
      switch (LookupID(memory::load<be_u32>(section.data + offset))) {
        case PPC_INST_STD:
          kinds = {MillicodeKind::kGPRSave};
          break;
        case PPC_INST_LD:
          kinds = {MillicodeKind::kGPRRestore};
          break;
        case PPC_INST_STFD:
          kinds = {MillicodeKind::kFPRSave};
          break;
        case PPC_INST_LFD:
          kinds = {MillicodeKind::kFPRRestore};
          break;
        case PPC_INST_LI:
        case PPC_INST_LIL:
          kinds = {MillicodeKind::kVMXSave, MillicodeKind::kVMXRestore};
          break;
        default:
          continue;
      }

      for (MillicodeKind kind : kinds) {
        auto run = ValidateMillicodeRun(binary, addr, kind);
        if (!run)
          continue;
        const bool known = std::any_of(runs.begin(), runs.end(), [&](const MillicodeRun& other) {
          return IsSameRunClass(other, kind, run->first);
        });
        if (known)
          continue;
        REXLOG_DEBUG(codegen, "Found {}{} at 0x{:08X}", GetMillicodePrefix(kind), run->first,
                     run->head);
        runs.push_back(*run);
      }
      if (runs.size() == kRunClassCount)
        return;
    }
  }
}

/**
 * Finds the restore helper that undoes a save helper call.
 *
 * @param state Analysis state holding the runs.
 * @param entry Role of the save helper that was called.
 * @return The restore entry address, or 0 when no restore run covers the register.
 */
u32 FindRestoreHelper(const AnalysisState& state, const MillicodeEntry& entry) {
  const auto restore_kind = static_cast<MillicodeKind>(static_cast<u8>(entry.kind) + 1);
  for (const auto& run : state.millicode_runs) {
    if (IsSameRunClass(run, restore_kind, entry.reg) && entry.reg >= run.first &&
        entry.reg < run.first + run.count) {
      return run.head + (entry.reg - run.first) * run.stride;
    }
  }
  return 0;
}

/**
 * Registers imports, save and restore helpers, config functions and PDATA functions. A restore
 * helper of the matching kind and register gives the restore helper of an SEH function. An __except
 * handler is inline and becomes a label.
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
  for (u32 addr : config.exception_handler_func_hints) {
    state.exception_handler_funcs.push_back(addr);
  }

  for (const auto& [addr, cfg] : config.functions) {
    if (cfg.IsChunk()) {
      state.chunks_by_parent[cfg.parent].push_back(addr);
    }
  }

  DetectMillicodeRuns(binary, state);

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

      auto* node = graph.AddImportFunction(sym.address, resolved_name,
                                           GetImportThunkSize(binary, sym.address));
      if (exp && runtime::IsNoReturnExport(*export_module, ordinal)) {
        graph.DeclareNoReturn(sym.address);
      }
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

  for (const auto& run : state.millicode_runs) {
    const u32 tail_bytes = static_cast<u32>(GetTailWords(run.kind).size()) * kWordBytes;
    for (u32 i = 0; i < run.count; ++i) {
      const u32 addr = run.head + i * run.stride;
      const u32 reg = run.first + i;
      graph.AddFunction(addr, (run.count - i) * run.stride + tail_bytes, FunctionAuthority::kHelper,
                        true);
      graph.SetFunctionName(addr, std::format("{}{}", GetMillicodePrefix(run.kind), reg));
      graph.SetFunctionMillicode(addr, MillicodeEntry{run.kind, reg});
    }
  }

  if (config.longjmp_address != 0) {
    graph.DeclareNoReturn(config.longjmp_address);
  }

  size_t config_funcs = 0, config_chunks = 0;
  for (const auto& [address, cfg] : config.functions) {
    if (const auto* existing = graph.GetFunction(address); existing && existing->GetMillicode()) {
      return Err(ErrorCategory::kValidation,
                 std::format("function 0x{:08X} is millicode, which no config entry may replace",
                             address));
    }
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

  u32 pdata_addr = binary.GetExceptionDirectoryAddress();
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

  size_t count = pdata_size / sizeof(RuntimeFunctionWords);
  auto* entries = reinterpret_cast<const RuntimeFunctionWords*>(pdata_data);

  size_t pdata_added = 0;
  for (size_t i = 0; i < count; i++) {
    const u32 begin_addr = entries[i].begin;
    const auto fn = rex::ppc::DecodeRuntimeFunction(begin_addr, entries[i].word);

    if (fn.type == rex::ppc::RuntimeFunctionType::kSaveMillicode ||
        fn.type == rex::ppc::RuntimeFunctionType::kRestoreMillicode) {
      const auto* helper = graph.GetFunction(begin_addr);
      const bool restore = fn.type == rex::ppc::RuntimeFunctionType::kRestoreMillicode;
      if (!helper || !helper->GetMillicode() ||
          IsSaveKind(helper->GetMillicode()->kind) == restore) {
        return Err(ErrorCategory::kValidation,
                   std::format("pdata entry 0x{:08X} is a {} millicode helper but no such helper "
                               "run has an entry there",
                               begin_addr, restore ? "restore" : "save"));
      }
      continue;
    }

    u32 size = fn.length * 4;
    if (size == 0)
      size = 4;

    if (graph.GetFunction(begin_addr) != nullptr) {
      continue;
    }

    const bool has_handler = fn.type == rex::ppc::RuntimeFunctionType::kHandler;
    std::optional<ParsedException> ex_info;
    Prolog prolog_info;
    if (has_handler) {
      prolog_info = ScanProlog(binary, begin_addr, fn.prologue);

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

          for (u32 target : prolog_info.call_targets) {
            const auto* callee = graph.GetFunction(target);
            if (!callee || !callee->GetMillicode() || !IsSaveKind(callee->GetMillicode()->kind))
              continue;
            if (u32 restore = FindRestoreHelper(state, *callee->GetMillicode()))
              seh_info.restore_helpers.push_back(restore);
          }
        }
      }
    }

    graph.AddFunction(begin_addr, size, FunctionAuthority::kPData, true);
    graph.SetFunctionHasExceptionHandler(begin_addr, has_handler);

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

[[nodiscard]] Result<void> Register(CodegenContext& ctx, IProgressReporter* reporter) {
  (void)reporter;
  return RegisterEntryPoints(ctx);
}

}  // namespace detail

}  // namespace rex::codegen
