/**
 * @file        rex/codegen/function_types.h
 * @brief       Types and structures used by FunctionNode and FunctionGraph
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

namespace rex::runtime {
class ExportResolver;
}  // namespace rex::runtime

namespace rex::codegen {

// Forward declarations
class FunctionGraph;
class FunctionNode;
class BinaryView;
struct RecompilerConfig;

/// Lightweight context passed to FunctionNode::emitCpp() and BuilderContext.
/// Non-owning references -- caller must ensure lifetimes.
struct EmitContext {
  const BinaryView& binary;
  const RecompilerConfig& config;
  const FunctionGraph& graph;
  uint32_t entryPoint = 0;                      ///< For "xstart" naming
  runtime::ExportResolver* resolver = nullptr;  ///< For import ordinal resolution (nullable)
};

//=============================================================================
// Authority Levels
//=============================================================================
// Determines boundary mutability and merge eligibility.
// Only GAP_FILL can be absorbed during vacancy merging.
// All others represent immutable entry points.
enum class FunctionAuthority : uint8_t {
  GAP_FILL = 0,    // Speculative - found in unclaimed gap, CAN be absorbed
  DISCOVERED = 1,  // Found via bl/bcl - immutable entry point
  VTABLE = 2,      // Found in vtable - immutable entry point
  HELPER = 3,      // Save/restore helpers - fixed, overlaps allowed
  PDATA = 4,       // From .pdata - entry fixed, can extend
  CONFIG = 5,      // User config - exact boundaries, immutable
  IMPORT = 6,      // Import thunk - external function, immutable
};

//=============================================================================
// Target Classification (for code generation)
//=============================================================================
enum class TargetKind {
  InternalLabel,  // Target inside caller's function (PIC pattern)
  Function,       // Target is a function entry point
  Import,         // Target is an import
  Unknown,        // Target not recognized
};

const char* AuthorityName(FunctionAuthority auth);

//=============================================================================
// Function State (3-state machine)
//=============================================================================
enum class FunctionState : uint8_t {
  kRegistered,  // Entry point known, blocks/instructions not yet assigned
  kDiscovered,  // Blocks and instructions assigned, may have unresolved branches
  kSealed,      // All branches resolved, ready for code generation
};

// Legacy aliases for compatibility during migration
constexpr FunctionState PENDING = FunctionState::kRegistered;  // Will be removed
constexpr FunctionState SEALED = FunctionState::kSealed;       // Will be removed

//=============================================================================
// Exception Handling - SEH (Structured Exception Handling)
//=============================================================================

struct SehScope {
  uint32_t tryStart;  // [+0] Start of __try block
  uint32_t tryEnd;    // [+4] End of __try block
  uint32_t handler;   // [+8] Handler function (__finally or __except body)
  uint32_t filter;    // [+C] Filter expression (0 for __finally, address for __except)
};

struct SehExceptionInfo {
  uint32_t handlerThunk;    // e.g. __C_specific_handler thunk address
  uint32_t scopeTableAddr;  // Pointer to scope table in .rdata
  std::vector<SehScope> scopes;
  uint32_t frameSize = 0;      // Stack frame size for r12 setup during unwind
  uint32_t restoreHelper = 0;  // __restgprlr_N address to call on unwind
};

//=============================================================================
// Exception Handling - C++ EH (FuncInfo with magic 0x19930522)
//=============================================================================

constexpr uint32_t CXX_EH_MAGIC = 0x19930522;

struct CxxUnwindEntry {
  int32_t toState;  // Previous state (-1 = terminal)
  uint32_t action;  // Cleanup/destructor function address
};

struct CxxIPStateEntry {
  uint32_t ip;    // Code address where state changes
  int32_t state;  // State number at this IP
};

struct CxxCatchHandler {
  uint32_t adjectives;           // Catch type flags
  uint32_t typeDescriptor;       // Pointer to type descriptor (RTTI)
  int32_t catchObjDisplacement;  // Displacement of catch object
  uint32_t handlerAddress;       // Catch handler function address
};

struct CxxTryBlock {
  int32_t tryLow;     // Lowest state in try
  int32_t tryHigh;    // Highest state in try
  int32_t catchHigh;  // Highest state in catch
  std::vector<CxxCatchHandler> handlers;
};

struct CxxExceptionInfo {
  uint32_t handlerThunk;  // Frame handler function
  uint32_t funcInfoAddr;  // Address of FuncInfo in .rdata
  uint32_t maxState;      // Number of unwind states
  std::vector<CxxUnwindEntry> unwindMap;
  std::vector<CxxTryBlock> tryBlocks;
  std::vector<CxxIPStateEntry> ipToStateMap;
};

//=============================================================================
// Combined Exception Info (variant of SEH or C++ EH)
//=============================================================================

struct ExceptionInfo {
  std::variant<std::monostate, SehExceptionInfo, CxxExceptionInfo> data;

  bool hasInfo() const { return !std::holds_alternative<std::monostate>(data); }
  bool isSeh() const { return std::holds_alternative<SehExceptionInfo>(data); }
  bool isCxx() const { return std::holds_alternative<CxxExceptionInfo>(data); }

  const SehExceptionInfo* asSeh() const { return std::get_if<SehExceptionInfo>(&data); }
  const CxxExceptionInfo* asCxx() const { return std::get_if<CxxExceptionInfo>(&data); }

  uint32_t handlerThunk() const {
    if (auto* seh = asSeh())
      return seh->handlerThunk;
    if (auto* cxx = asCxx())
      return cxx->handlerThunk;
    return 0;
  }
};

//=============================================================================
// Call Target - Resolved destination of a call/jump
//=============================================================================
struct CallTarget {
  struct ToFunction {
    FunctionNode* node;
  };
  struct ToImport {
    uint32_t address;
    std::string name;
  };
  struct Unresolved {
    uint32_t address;
  };

  std::variant<ToFunction, ToImport, Unresolved> value;

  bool isResolved() const { return !std::holds_alternative<Unresolved>(value); }
  bool isFunction() const { return std::holds_alternative<ToFunction>(value); }
  bool isImport() const { return std::holds_alternative<ToImport>(value); }

  FunctionNode* asFunction() const {
    if (auto* f = std::get_if<ToFunction>(&value))
      return f->node;
    return nullptr;
  }

  static CallTarget function(FunctionNode* fn) { return {ToFunction{fn}}; }
  static CallTarget import(uint32_t addr, std::string name) {
    return {ToImport{addr, std::move(name)}};
  }
  static CallTarget unresolved(uint32_t addr) { return {Unresolved{addr}}; }
};

//=============================================================================
// Call Edge - A call site within a function
//=============================================================================

struct CallEdge {
  uint32_t site;      // Address of the bl/b instruction
  CallTarget target;  // Resolved or unresolved target
};

//=============================================================================
// Basic Block
//=============================================================================

struct Block {
  uint32_t base;
  uint32_t size;

  uint32_t end() const { return base + size; }
  bool contains(uint32_t addr) const { return addr >= base && addr < end(); }
};

//=============================================================================
// Jump Table
//=============================================================================

struct JumpTable {
  uint32_t bctrAddress;           // Address of bctr instruction
  uint32_t tableAddress;          // Address of jump table data
  uint8_t indexRegister;          // Register holding switch index
  std::vector<uint32_t> targets;  // Resolved case targets (internal labels)
};

//=============================================================================
// Function Analysis (computed at seal time)
//=============================================================================

struct FunctionAnalysis {
  // CSR requirements (denormal handling)
  enum class CsrRequirement : uint8_t { None, Fpu, Vmx };

  // Special register usage
  bool usesCtr = false;
  bool usesXer = false;
  bool usesCr = false;
  bool usesFpscr = false;

  // CSR state needed
  CsrRequirement csrRequirement = CsrRequirement::None;
};

//=============================================================================
// Unresolved Jump - Internal jump awaiting resolution
//=============================================================================

struct UnresolvedJump {
  uint32_t site;       // Address of the branch instruction
  uint32_t target;     // Target address
  bool isCall;         // true = bl (call), false = b (tail call)
  bool isConditional;  // true = bc/beq/bne/etc, false = b
};

//=============================================================================
// Code Buffer - Holds executable code for a section
//=============================================================================
// The graph owns code buffers so recompilation doesn't need module access.
// Each buffer corresponds to one executable section.

struct CodeBuffer {
  std::vector<uint8_t> data;
  uint32_t baseAddress = 0;

  uint32_t size() const { return static_cast<uint32_t>(data.size()); }
  uint32_t endAddress() const { return baseAddress + size(); }

  bool contains(uint32_t addr) const { return addr >= baseAddress && addr < endAddress(); }

  const uint8_t* translate(uint32_t addr) const {
    if (!contains(addr))
      return nullptr;
    return data.data() + (addr - baseAddress);
  }
};

}  // namespace rex::codegen
