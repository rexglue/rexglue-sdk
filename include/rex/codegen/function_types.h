/**
 * @file        codegen/function_types.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <bitset>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <rex/types.h>

namespace rex::codegen::detail {
struct Instruction;
}  // namespace rex::codegen::detail

namespace rex::codegen {

class FunctionGraph;
class FunctionNode;
class BinaryView;
struct RecompilerConfig;
struct Limits;
struct MMIOReportSite;

/**
 * Lightweight context passed to FunctionNode::EmitCpp() and BuilderContext. Holds non owning
 * references, so the caller keeps them alive.
 */
struct EmitContext {
  const BinaryView& binary; /**< Binary the function was read from. */
  /** Config consulted for the register and exception handler options. */
  const RecompilerConfig& config;
  const FunctionGraph& graph; /**< Graph that classifies branch targets and resolves calls. */
  const Limits& limits;       /**< Codegen limits such as the maximum output file size. */
  u32 entry_point = 0;        /**< Used for xstart naming */

  /**
   * Every function name emitted as a call. The writer turns this into the per file declaration
   * header, so a missed name is a compile error.
   */
  std::unordered_set<std::string>* referenced = nullptr;

  /** Every classified MMIO site of the emitted functions, for the codegen report. */
  std::vector<MMIOReportSite>* mmio_sites = nullptr;

  /**
   * Adds a function name to the referenced set. Does nothing when referenced is null.
   *
   * @param name Name of the function emitted as a call
   */
  void Reference(std::string_view name) const {
    if (referenced)
      referenced->emplace(name);
  }
};

/**
 * Determines boundary mutability and merge eligibility. Only GAP_FILL can be absorbed during
 * vacancy merging, all others are immutable entry points.
 */
enum class FunctionAuthority : u8 {
  kGapFill = 0,    /**< Found in an unclaimed gap, can be absorbed */
  kDiscovered = 1, /**< Found through bl or bcl */
  kVTable = 2,     /**< Found in a vtable */
  kHelper = 3,     /**< Save and restore helpers, fixed, overlaps allowed */
  kPData = 4,      /**< From .pdata, entry fixed, can extend */
  kConfig = 5,     /**< User config with exact boundaries */
  kImport = 6,     /**< Import thunk for an external function */
};

/** Classification of a branch target for code generation. */
enum class TargetKind : u8 {
  kInternalLabel, /**< Target inside the caller function, a PIC pattern */
  kFunction,      /**< Target is a function in the graph. */
  kImport,        /**< Target is an import thunk, emitted as a call to the import. */
  kUnknown,       /**< Target is in no function or import, emitted as an error and a fatal call. */
};

/**
 * Returns the name of an authority.
 *
 * @param auth Authority to name
 * @return Static name string
 */
const char* AuthorityName(FunctionAuthority auth);

/** What a save or restore millicode run covers. */
enum class MillicodeKind : u8 {
  kGPRSave,    /**< Saves GPRs and LR. */
  kGPRRestore, /**< Restores GPRs and LR and returns. */
  kFPRSave,    /**< Saves FPRs. */
  kFPRRestore, /**< Restores FPRs and returns. */
  kVMXSave,    /**< Saves vector registers. */
  kVMXRestore, /**< Restores vector registers and returns. */
};

static_assert(static_cast<u8>(MillicodeKind::kGPRRestore) ==
              static_cast<u8>(MillicodeKind::kGPRSave) + 1);
static_assert(static_cast<u8>(MillicodeKind::kFPRRestore) ==
              static_cast<u8>(MillicodeKind::kFPRSave) + 1);
static_assert(static_cast<u8>(MillicodeKind::kVMXRestore) ==
              static_cast<u8>(MillicodeKind::kVMXSave) + 1);

/** A validated millicode run. */
struct MillicodeRun {
  MillicodeKind kind; /**< What the run saves or restores. */
  u32 head;           /**< Address of the entry for the first register. */
  u32 first;          /**< Number of the first register in the run. */
  u32 count;          /**< Number of entries in the run. */
  u32 stride;         /**< Distance in bytes between entries. */
};

/** Role of 1 entry of a millicode run. */
struct MillicodeEntry {
  MillicodeKind kind; /**< What the run of the entry saves or restores. */
  u32 reg;            /**< Register the entry starts at. */
};

/** 3 state machine of a function. */
enum class FunctionState : u8 {
  kRegistered, /**< Entry point known, blocks and instructions not assigned */
  kDiscovered, /**< Blocks and instructions assigned, may have unresolved branches */
  kSealed,     /**< All branches resolved, ready for code generation */
};

constexpr FunctionState kSealed = FunctionState::kSealed;

/** 1 scope table entry of an SEH function. */
struct SEHScope {
  u32 try_start; /**< Start of the __try block */
  u32 try_end;   /**< End of the __try block */
  u32 handler;   /**< __finally or __except body */
  u32 filter;    /**< 0 for __finally and the address for __except */
};

/** SEH data of 1 function. */
struct SEHExceptionInfo {
  u32 handler_thunk;                /**< Thunk address such as __C_specific_handler */
  u32 scope_table_addr;             /**< Pointer to the scope table in .rdata */
  std::vector<SEHScope> scopes;     /**< Scope table entries */
  u32 frame_size = 0;               /**< Stack frame size for the r12 setup during unwind */
  std::vector<u32> restore_helpers; /**< Restore millicode addresses to call on unwind */
};

/** FuncInfo magic of C++ EH. */
constexpr u32 kCxxEHMagic = 0x19930522;

/** 1 entry of the C++ EH unwind map. */
struct CxxUnwindEntry {
  i32 to_state; /**< Previous state, -1 for terminal */
  u32 action;   /**< Cleanup or destructor function address */
};

/** Maps a code address to a C++ EH state. */
struct CxxIPStateEntry {
  u32 ip;    /**< Code address where the state changes */
  i32 state; /**< State in effect from ip */
};

/** 1 catch clause of a C++ try block. */
struct CxxCatchHandler {
  u32 adjectives;             /**< Catch type flags */
  u32 type_descriptor;        /**< Pointer to the RTTI type descriptor */
  i32 catch_obj_displacement; /**< Frame displacement of the caught object */
  u32 handler_address;        /**< Address of the catch body */
};

/** 1 C++ try block with its state range and catch clauses. */
struct CxxTryBlock {
  i32 try_low;                           /**< Lowest state inside the try body */
  i32 try_high;                          /**< Highest state inside the try body */
  i32 catch_high;                        /**< Highest state inside the catch bodies */
  std::vector<CxxCatchHandler> handlers; /**< Catch clauses of the block */
};

/** C++ EH data of 1 function. */
struct CxxExceptionInfo {
  u32 handler_thunk;                            /**< Thunk address of the EH handler */
  u32 func_info_addr;                           /**< Address of FuncInfo in .rdata */
  u32 max_state;                                /**< Number of unwind states */
  std::vector<CxxUnwindEntry> unwind_map;       /**< Unwind action per state */
  std::vector<CxxTryBlock> try_blocks;          /**< Try blocks of the function */
  std::vector<CxxIPStateEntry> ip_to_state_map; /**< Code address to state transitions */
};

/** Variant of SEH or C++ EH info. */
struct ExceptionInfo {
  /** Held info, SEH or C++ EH. `std::monostate` when the function has none. */
  std::variant<std::monostate, SEHExceptionInfo, CxxExceptionInfo> data;

  bool HasInfo() const { return !std::holds_alternative<std::monostate>(data); }
  bool IsSEH() const { return std::holds_alternative<SEHExceptionInfo>(data); }
  bool IsCxx() const { return std::holds_alternative<CxxExceptionInfo>(data); }

  const SEHExceptionInfo* AsSEH() const { return std::get_if<SEHExceptionInfo>(&data); }
  const CxxExceptionInfo* AsCxx() const { return std::get_if<CxxExceptionInfo>(&data); }

  /**
   * Returns the handler thunk address of the held info.
   *
   * @return Thunk address, or 0 when no info is held
   */
  u32 GetHandlerThunk() const {
    if (auto* seh = AsSEH())
      return seh->handler_thunk;
    if (auto* cxx = AsCxx())
      return cxx->handler_thunk;
    return 0;
  }
};

/** Resolved destination of a call or jump. */
struct CallTarget {
  /** Destination is a function in the graph. */
  struct ToFunction {
    FunctionNode* node; /**< Destination function */
  };
  /** Destination is an import thunk. */
  struct ToImport {
    u32 address;      /**< Thunk address */
    std::string name; /**< Import name */
  };
  /** Destination not yet resolved. */
  struct Unresolved {
    u32 address; /**< Branch target address */
  };

  /** Resolved destination, a function, an import thunk or an unresolved address. */
  std::variant<ToFunction, ToImport, Unresolved> value;

  bool IsResolved() const { return !std::holds_alternative<Unresolved>(value); }
  bool IsFunction() const { return std::holds_alternative<ToFunction>(value); }
  bool IsImport() const { return std::holds_alternative<ToImport>(value); }

  /**
   * Returns the function destination.
   *
   * @return The node, or null when the target is not a function
   */
  FunctionNode* AsFunction() const {
    if (auto* f = std::get_if<ToFunction>(&value))
      return f->node;
    return nullptr;
  }

  static CallTarget MakeFunction(FunctionNode* fn) { return {ToFunction{fn}}; }
  /**
   * Creates a target for an import.
   *
   * @param addr Thunk address
   * @param name Import name
   * @return The import target
   */
  static CallTarget MakeImport(u32 addr, std::string name) {
    return {ToImport{addr, std::move(name)}};
  }
  static CallTarget MakeUnresolved(u32 addr) { return {Unresolved{addr}}; }
};

/** A call site within a function. */
struct CallEdge {
  u32 site;          /**< Address of the bl or b instruction */
  CallTarget target; /**< Destination of the call. */
};

/** Contiguous run of instructions inside a function. */
struct Block {
  u32 base; /**< Start address */
  u32 size; /**< Size in bytes */

  u32 GetEnd() const { return base + size; }
  bool Contains(u32 addr) const { return addr >= base && addr < GetEnd(); }
};

/** Jump table behind an indirect branch. */
struct JumpTable {
  u32 bctr_address;         /**< Address of the bctr instruction */
  u32 table_address;        /**< Address of the jump table data */
  u8 index_register;        /**< Register holding the switch index */
  std::vector<u32> targets; /**< Resolved case targets, internal labels */
};

/** Analysis computed at seal time. */
struct FunctionAnalysis {
  /** CSR requirement for denormal handling. */
  enum class CSRRequirement : u8 { kNone, kFPU, kVMX };

  bool uses_ctr = false;   /**< No code sets it, always false. */
  bool uses_fpscr = false; /**< No code sets it, always false. */

  CSRRequirement csr_requirement = CSRRequirement::kNone; /**< No code sets it, always None. */
};

/** Internal jump awaiting resolution. */
struct UnresolvedJump {
  u32 site;            /**< Address of the branch instruction */
  u32 target;          /**< Address of the branch destination. */
  bool is_call;        /**< True for bl, false for b */
  bool is_conditional; /**< True for bc, beq, bne and other conditional branches */
};

}  // namespace rex::codegen
