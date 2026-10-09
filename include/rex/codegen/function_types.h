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

namespace rex::codegen::ppc {
struct Instruction;
}  // namespace rex::codegen::ppc

namespace rex::codegen {

class FunctionGraph;
class FunctionNode;
class BinaryView;
struct RecompilerConfig;
struct Limits;
struct MMIOReportSite;

/** Lightweight context passed to FunctionNode::emitCpp() and BuilderContext. Holds non owning
 * references, so the caller keeps them alive. */
struct EmitContext {
  const BinaryView& binary; /**< Binary the function was read from. */
  /** Config consulted for the register and exception handler options. */
  const RecompilerConfig& config;
  const FunctionGraph& graph; /**< Graph that classifies branch targets and resolves calls. */
  const Limits& limits;       /**< Codegen limits such as the maximum output file size. */
  uint32_t entryPoint = 0;    /**< Used for xstart naming */

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
  void reference(std::string_view name) const {
    if (referenced)
      referenced->emplace(name);
  }
};

/**
 * Determines boundary mutability and merge eligibility. Only GAP_FILL can be absorbed during
 * vacancy merging, all others are immutable entry points.
 */
enum class FunctionAuthority : uint8_t {
  GAP_FILL = 0,   /**< Found in an unclaimed gap, can be absorbed */
  DISCOVERED = 1, /**< Found through bl or bcl */
  VTABLE = 2,     /**< Found in a vtable */
  HELPER = 3,     /**< Save and restore helpers, fixed, overlaps allowed */
  PDATA = 4,      /**< From .pdata, entry fixed, can extend */
  CONFIG = 5,     /**< User config with exact boundaries */
  IMPORT = 6,     /**< Import thunk for an external function */
};

/** Classification of a branch target for code generation. */
enum class TargetKind {
  InternalLabel, /**< Target inside the caller function, a PIC pattern */
  Function,      /**< Target is a function in the graph. */
  Import,        /**< Target is an import thunk, emitted as a call to the import. */
  Unknown,       /**< Target is in no function or import, emitted as an error and a fatal call. */
};

/**
 * Returns the name of an authority.
 *
 * @param auth Authority to name
 * @return Static name string
 */
const char* AuthorityName(FunctionAuthority auth);

/** Three state machine of a function. */
enum class FunctionState : uint8_t {
  kRegistered, /**< Entry point known, blocks and instructions not assigned */
  kDiscovered, /**< Blocks and instructions assigned, may have unresolved branches */
  kSealed,     /**< All branches resolved, ready for code generation */
};

constexpr FunctionState PENDING = FunctionState::kRegistered;
constexpr FunctionState SEALED = FunctionState::kSealed;

/** 1 scope table entry of an SEH function. */
struct SehScope {
  uint32_t tryStart; /**< Offset +0, start of the __try block */
  uint32_t tryEnd;   /**< Offset +4, end of the __try block */
  uint32_t handler;  /**< Offset +8, __finally or __except body */
  uint32_t filter;   /**< Offset +C, 0 for __finally and the address for __except */
};

/** SEH data of 1 function. */
struct SehExceptionInfo {
  uint32_t handlerThunk;        /**< Thunk address such as __C_specific_handler */
  uint32_t scopeTableAddr;      /**< Pointer to the scope table in .rdata */
  std::vector<SehScope> scopes; /**< Scope table entries */
  uint32_t frameSize = 0;       /**< Stack frame size for the r12 setup during unwind */
  uint32_t restoreHelper = 0;   /**< __restgprlr_N address to call on unwind */
};

/** FuncInfo magic of C++ EH. */
constexpr uint32_t CXX_EH_MAGIC = 0x19930522;

/** 1 entry of the C++ EH unwind map. */
struct CxxUnwindEntry {
  int32_t toState; /**< Previous state, -1 for terminal */
  uint32_t action; /**< Cleanup or destructor function address */
};

/** Maps a code address to a C++ EH state. */
struct CxxIPStateEntry {
  uint32_t ip;   /**< Code address where the state changes */
  int32_t state; /**< State in effect from ip */
};

/** 1 catch clause of a C++ try block. */
struct CxxCatchHandler {
  uint32_t adjectives;          /**< Catch type flags */
  uint32_t typeDescriptor;      /**< Pointer to the RTTI type descriptor */
  int32_t catchObjDisplacement; /**< Frame displacement of the caught object */
  uint32_t handlerAddress;      /**< Address of the catch body */
};

/** 1 C++ try block with its state range and catch clauses. */
struct CxxTryBlock {
  int32_t tryLow;                        /**< Lowest state inside the try body */
  int32_t tryHigh;                       /**< Highest state inside the try body */
  int32_t catchHigh;                     /**< Highest state inside the catch bodies */
  std::vector<CxxCatchHandler> handlers; /**< Catch clauses of the block */
};

/** C++ EH data of 1 function. */
struct CxxExceptionInfo {
  uint32_t handlerThunk;                     /**< Thunk address of the EH handler */
  uint32_t funcInfoAddr;                     /**< Address of FuncInfo in .rdata */
  uint32_t maxState;                         /**< Number of unwind states */
  std::vector<CxxUnwindEntry> unwindMap;     /**< Unwind action per state */
  std::vector<CxxTryBlock> tryBlocks;        /**< Try blocks of the function */
  std::vector<CxxIPStateEntry> ipToStateMap; /**< Code address to state transitions */
};

/** Variant of SEH or C++ EH info. */
struct ExceptionInfo {
  /** Held info, SEH or C++ EH. `std::monostate` when the function has none. */
  std::variant<std::monostate, SehExceptionInfo, CxxExceptionInfo> data;

  bool hasInfo() const { return !std::holds_alternative<std::monostate>(data); }
  bool isSeh() const { return std::holds_alternative<SehExceptionInfo>(data); }
  bool isCxx() const { return std::holds_alternative<CxxExceptionInfo>(data); }

  const SehExceptionInfo* asSeh() const { return std::get_if<SehExceptionInfo>(&data); }
  const CxxExceptionInfo* asCxx() const { return std::get_if<CxxExceptionInfo>(&data); }

  /**
   * Returns the handler thunk address of the held info.
   *
   * @return Thunk address, or 0 when no info is held
   */
  uint32_t handlerThunk() const {
    if (auto* seh = asSeh())
      return seh->handlerThunk;
    if (auto* cxx = asCxx())
      return cxx->handlerThunk;
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
    uint32_t address; /**< Thunk address */
    std::string name; /**< Import name */
  };
  /** Destination not yet resolved. */
  struct Unresolved {
    uint32_t address; /**< Branch target address */
  };

  /** Resolved destination, a function, an import thunk or an unresolved address. */
  std::variant<ToFunction, ToImport, Unresolved> value;

  bool isResolved() const { return !std::holds_alternative<Unresolved>(value); }
  bool isFunction() const { return std::holds_alternative<ToFunction>(value); }
  bool isImport() const { return std::holds_alternative<ToImport>(value); }

  /**
   * Returns the function destination.
   *
   * @return The node, or null when the target is not a function
   */
  FunctionNode* asFunction() const {
    if (auto* f = std::get_if<ToFunction>(&value))
      return f->node;
    return nullptr;
  }

  static CallTarget function(FunctionNode* fn) { return {ToFunction{fn}}; }
  /**
   * Creates a target for an import.
   *
   * @param addr Thunk address
   * @param name Import name
   * @return The import target
   */
  static CallTarget import(uint32_t addr, std::string name) {
    return {ToImport{addr, std::move(name)}};
  }
  static CallTarget unresolved(uint32_t addr) { return {Unresolved{addr}}; }
};

/** A call site within a function. */
struct CallEdge {
  uint32_t site;     /**< Address of the bl or b instruction */
  CallTarget target; /**< Destination of the call. */
};

/** Contiguous run of instructions inside a function. */
struct Block {
  uint32_t base; /**< Start address */
  uint32_t size; /**< Size in bytes */

  uint32_t end() const { return base + size; }
  bool contains(uint32_t addr) const { return addr >= base && addr < end(); }
};

/** Jump table behind an indirect branch. */
struct JumpTable {
  uint32_t bctrAddress;          /**< Address of the bctr instruction */
  uint32_t tableAddress;         /**< Address of the jump table data */
  uint8_t indexRegister;         /**< Register holding the switch index */
  std::vector<uint32_t> targets; /**< Resolved case targets, internal labels */
};

/** Analysis computed at seal time. */
struct FunctionAnalysis {
  /** CSR requirement for denormal handling. */
  enum class CsrRequirement : uint8_t { None, Fpu, Vmx };

  bool usesCtr = false;   /**< No code sets it, always false. */
  bool usesXer = false;   /**< No code sets it, always false. */
  bool usesCr = false;    /**< No code sets it, always false. */
  bool usesFpscr = false; /**< No code sets it, always false. */

  CsrRequirement csrRequirement = CsrRequirement::None; /**< No code sets it, always None. */
};

/** Internal jump awaiting resolution. */
struct UnresolvedJump {
  uint32_t site;      /**< Address of the branch instruction */
  uint32_t target;    /**< Address of the branch destination. */
  bool isCall;        /**< True for bl, false for b */
  bool isConditional; /**< True for bc, beq, bne and other conditional branches */
};

}  // namespace rex::codegen
