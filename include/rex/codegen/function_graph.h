/**
 * @file        rex/codegen/function_graph.h
 * @brief       Function graph - reactive model for function discovery and resolution
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

// Forward declarations
class FunctionGraph;
class FunctionNode;

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

//=============================================================================
// Function Node
//=============================================================================
// Core object representing a function in the graph.
// Manages its own state transitions and resolution.

class FunctionNode {
  friend class FunctionGraph;  // Graph manages all mutations

 public:
  FunctionNode(uint32_t base, uint32_t size, FunctionAuthority authority);

  //=========================================================================
  // Read-only accessors - external code can inspect but not modify
  //=========================================================================

  // Identity
  uint32_t base() const { return base_; }
  uint32_t size() const { return size_; }
  uint32_t end() const { return base_ + size_; }
  const std::string& name() const { return name_; }

  // Code access - cached pointer to instruction bytes
  const uint8_t* code() const { return code_; }
  bool hasCode() const { return code_ != nullptr; }

  // Authority and state
  FunctionAuthority authority() const { return authority_; }
  FunctionState state() const { return state_; }

  // State queries
  bool isRegistered() const { return state_ == FunctionState::kRegistered; }
  bool isDiscovered() const { return state_ == FunctionState::kDiscovered; }
  bool isSealed() const { return state_ == FunctionState::kSealed; }

  // Legacy aliases (PENDING maps to kRegistered OR kDiscovered - not sealed)
  bool isPending() const { return state_ != FunctionState::kSealed; }

  // Special type checks
  bool isImport() const { return authority_ == FunctionAuthority::IMPORT; }
  bool isHelper() const { return authority_ == FunctionAuthority::HELPER; }

  //=========================================================================
  // State Machine - New 3-state model
  //=========================================================================

  /// Can transition from kRegistered to kDiscovered?
  bool canDiscover() const { return state_ == FunctionState::kRegistered; }

  /// Transition kRegistered -> kDiscovered with blocks and instructions
  /// Precondition: canDiscover() returns true
  /// For non-imports: blocks must not be empty
  void discover(std::vector<Block> blocks,
                std::vector<rex::codegen::ppc::Instruction*> instructions,
                std::set<uint32_t> labels);

  /// Transition kRegistered -> kDiscovered for import functions (no blocks)
  void discoverAsImport();

  /// Can transition from kDiscovered to kSealed?
  /// Returns true if:
  /// - state == kDiscovered
  /// - imports: always OK (no blocks required)
  /// - non-imports: blocks not empty AND no unresolved branches
  bool canSeal() const;

  /// Transition kDiscovered -> kSealed
  /// Computes FunctionAnalysis and sorts blocks
  void seal();

  /// Get analysis result (only valid after seal)
  const FunctionAnalysis& analysis() const;

  //=========================================================================
  // Code Emission (valid after seal)
  //=========================================================================

  /// Emit C++ code for this function
  /// Requires: state() == kSealed
  /// For imports: emits PPC_IMPORT macro
  /// For normal functions: emits PPC_FUNC with blocks and instructions
  void emitCpp(class CodeEmitter& emit) const;

  //=========================================================================
  // Instruction access (valid after discover)
  //=========================================================================

  /// Get owned instructions (pointers into DecodedBinary)
  std::span<rex::codegen::ppc::Instruction* const> instructions() const { return instructions_; }

  // Blocks
  const std::vector<Block>& blocks() const { return blocks_; }
  bool containsAddress(uint32_t addr) const;

  // Check if address is within overall function bounds (ignores blocks)
  // Use this for branch target detection where address may be in a gap between blocks
  bool isWithinBounds(uint32_t addr) const { return addr >= base_ && addr < base_ + size_; }

  // Labels (internal branch targets within this function)
  const std::set<uint32_t>& labels() const { return labels_; }
  bool isLabel(uint32_t addr) const { return labels_.contains(addr); }

  // Resolved calls (bl instructions)
  const std::vector<CallEdge>& calls() const { return calls_; }

  // Resolved tail calls (b instructions to other functions)
  const std::vector<CallEdge>& tailCalls() const { return tailCalls_; }

  // Jump tables
  const std::vector<JumpTable>& jumpTables() const { return jumpTables_; }

  // Unresolved jumps (pending resolution)
  const std::vector<UnresolvedJump>& unresolvedJumps() const { return unresolvedJumps_; }

  // Sealing state (legacy - use canSeal() with new semantics)
  bool hasUnresolvedJumps() const { return !unresolvedJumps_.empty(); }

  // Validation
  bool hasExceptionHandler() const { return hasExceptionHandler_; }

  // Exception info (SEH or C++ EH)
  const std::optional<ExceptionInfo>& exceptionInfo() const { return exceptionInfo_; }
  bool hasExceptionInfo() const { return exceptionInfo_.has_value() && exceptionInfo_->hasInfo(); }

 private:
  //=========================================================================
  // Mutation methods - only FunctionGraph can call these
  //=========================================================================

  void setName(std::string name) { name_ = std::move(name); }
  void setCode(const uint8_t* ptr) { code_ = ptr; }
  void setHasExceptionHandler(bool val) { hasExceptionHandler_ = val; }
  void setExceptionInfo(ExceptionInfo info) { exceptionInfo_ = std::move(info); }

  // Block/label management
  void addBlock(Block block);
  void addLabel(uint32_t addr);

  // Call tracking
  void addCall(uint32_t site, CallTarget target);
  void addTailCall(uint32_t site, CallTarget target);
  void addJumpTable(JumpTable jt);
  void addUnresolvedJump(uint32_t site, uint32_t target, bool isCall, bool conditional);

  // Resolution (reactive - called by graph on events)
  bool tryResolveAgainst(FunctionNode* newFunction);
  bool tryResolveAgainstImport(uint32_t importAddr, const std::string& importName);
  bool tryResolveAsInternalLabel(uint32_t target);

  // Expansion
  void absorbRegion(uint32_t regionBase, uint32_t regionSize);

  // Internal helper
  void removeUnresolvedJump(uint32_t site);

  //=========================================================================
  // State
  //=========================================================================
 private:
  uint32_t base_;
  uint32_t size_;
  std::string name_;
  const uint8_t* code_ = nullptr;  // Cached pointer to instruction bytes
  FunctionAuthority authority_;
  FunctionState state_ = FunctionState::kRegistered;
  bool hasExceptionHandler_ = false;

  // Populated at discover()
  std::vector<Block> blocks_;
  std::vector<rex::codegen::ppc::Instruction*> instructions_;  // Pointers into DecodedBinary
  std::set<uint32_t> labels_;  // Branch targets within this function

  std::vector<CallEdge> calls_;
  std::vector<CallEdge> tailCalls_;
  std::vector<JumpTable> jumpTables_;

  std::vector<UnresolvedJump> unresolvedJumps_;

  std::optional<ExceptionInfo> exceptionInfo_;

  // Computed at seal()
  std::optional<FunctionAnalysis> analysis_;
};

//=============================================================================
// Function Graph
//=============================================================================
// Container for all function nodes. Manages resolution notifications.
// Also handles vacancy checking for merge eligibility.
//
// Vacancy Rules - A region is vacant if ALL are true:
//   1. No null dword at the boundary
//   2. No chunk claims the region
//   3. Target does not fall within a protected function's range:
//      - PDATA/CONFIG/HELPER: always protected (cannot merge into)
//      - DISCOVERED with xrefs: CAN be merged (treated as potential internal label)

class FunctionGraph {
 public:
  using MemoryReader = std::function<std::optional<uint32_t>(uint32_t addr)>;

  //=========================================================================
  // Code Buffer Management
  //=========================================================================

  // Add a code buffer (copies executable section data into graph)
  void addCodeBuffer(uint32_t baseAddress, const uint8_t* data, size_t size);

  // Translate guest address to host pointer (searches all code buffers)
  const uint8_t* translateCode(uint32_t addr) const;

  // Get all code buffers (for iteration/debugging)
  const std::vector<CodeBuffer>& codeBuffers() const { return codeBuffers_; }

  // Update all function code pointers (call after loading code buffers)
  void updateFunctionCodePointers();

  //=========================================================================
  // Function Management
  //=========================================================================

  // Add a function to the graph.
  // Returns the new node, or existing node if already present (higher authority wins).
  // Notifies all PENDING functions to try resolution against the new entry.
  // hasXrefs: true if this is a known call target (bl target, etc.)
  FunctionNode* addFunction(uint32_t base, uint32_t size, FunctionAuthority authority,
                            bool hasXrefs = false);

  // Add a named function to the graph (convenience overload)
  FunctionNode* addFunction(uint32_t base, uint32_t size, FunctionAuthority authority,
                            std::string_view name, bool hasXrefs = false);

  // Add a resolved import as a callable function with __imp__ name
  // Address is the thunk address that bl instructions target
  FunctionNode* addImportFunction(uint32_t address, std::string_view resolvedName);

  // Get function by entry point (O(1))
  FunctionNode* getFunction(uint32_t entryPoint);
  const FunctionNode* getFunction(uint32_t entryPoint) const;

  // Remove function from graph (for cleanup of absorbed GAP_FILLs)
  bool removeFunction(uint32_t entryPoint);

  // Get function containing address (O(log f) via sorted base index)
  FunctionNode* getFunctionContaining(uint32_t addr);
  const FunctionNode* getFunctionContaining(uint32_t addr) const;

  // Check if address is a known entry point
  bool isEntryPoint(uint32_t addr) const;

  // Check if address is an import (FunctionNode with IMPORT authority)
  bool isImport(uint32_t addr) const;

  // Iterate all functions (includes imports with IMPORT authority)
  const std::unordered_map<uint32_t, std::unique_ptr<FunctionNode>>& functions() const {
    return functions_;
  }

  // Get all PENDING functions
  std::vector<FunctionNode*> getPendingFunctions();

  // Get all SEALED functions
  std::vector<FunctionNode*> getSealedFunctions();

  // Statistics
  size_t functionCount() const { return functions_.size(); }
  size_t pendingCount() const;
  size_t sealedCount() const;

  //=========================================================================
  // Function Setup (called during Discover phase)
  //=========================================================================

  // Set function name
  void setFunctionName(uint32_t entry, std::string name);

  // Set exception handler flag
  void setFunctionHasExceptionHandler(uint32_t entry, bool val);

  // Set parsed exception info (SEH or C++ EH)
  void setFunctionExceptionInfo(uint32_t entry, ExceptionInfo info);

  // Add a block to a function
  void addBlockToFunction(uint32_t entry, Block block);

  // Add a label (internal branch target) to a function
  void addLabelToFunction(uint32_t entry, uint32_t label);

  // Add a resolved call to a function
  void addCallToFunction(uint32_t entry, uint32_t site, CallTarget target);

  // Add a resolved tail call to a function
  void addTailCallToFunction(uint32_t entry, uint32_t site, CallTarget target);

  // Add a jump table to a function
  void addJumpTableToFunction(uint32_t entry, JumpTable jt);

  // Add an unresolved jump to a function
  // isCall: true for bl (call), false for b (tail call)
  void addUnresolvedJumpToFunction(uint32_t entry, uint32_t site, uint32_t target, bool isCall,
                                   bool conditional);

  //=========================================================================
  // Resolution and Expansion (called during Merge phase)
  //=========================================================================

  // Try to resolve all unresolved jumps for a function
  // Checks against known functions, imports, and internal labels
  // Returns number of jumps resolved
  size_t tryResolveFunction(uint32_t entry);

  // Absorb a region into a function (for vacancy expansion)
  void absorbRegionIntoFunction(uint32_t entry, uint32_t regionBase, uint32_t regionSize);

  // Seal a function if it can be sealed
  // Returns true if function was sealed
  bool trySealFunction(uint32_t entry);

  // Seal all functions that can be sealed
  // Returns number of functions sealed
  size_t sealAllReady();

  // Seal all functions, throwing if any cannot be sealed
  // Use after discovery is complete to enforce all functions are ready
  void sealAll();

  //=========================================================================
  // Vacancy Checking
  //=========================================================================

  // Set the memory reader for null-dword checking
  void setMemoryReader(MemoryReader reader) { memoryReader_ = std::move(reader); }

  // Register a chunk (address range claimed by config, blocks vacancy)
  void registerChunk(uint32_t base, uint32_t size);

  // Check if a region is vacant for absorption
  // fromAddr: the address we're expanding from (to check for null boundary)
  // targetAddr: the start of the region we want to absorb
  bool isVacant(uint32_t fromAddr, uint32_t targetAddr) const;

  // Check if target is a mergeable entry point (DISCOVERED with xrefs)
  bool isMergeableEntryPoint(uint32_t addr) const;

  //=========================================================================
  // Target Classification (for code generation)
  //=========================================================================

  // Classify a branch target for code generation.
  // target: address being branched to
  // callerAddr: address of the branch instruction
  // isCallInstruction: true for bl (expects return), false for b (no return)
  // Returns how the target should be treated during code generation.
  TargetKind classifyTarget(uint32_t target, uint32_t callerAddr, bool isCallInstruction) const;

 private:
  std::vector<CodeBuffer> codeBuffers_;
  std::unordered_map<uint32_t, std::unique_ptr<FunctionNode>> functions_;
  std::map<uint32_t, FunctionNode*>
      functionsByBase_;  // sorted by base for O(log f) interval lookup
  std::unordered_map<uint32_t, bool> functionHasXrefs_;  // entry -> hasXrefs
  std::vector<std::pair<uint32_t, uint32_t>> chunks_;    // base, size pairs
  MemoryReader memoryReader_;

  // Notify all PENDING functions that a new function was added
  void notifyFunctionAdded(FunctionNode* newFunction);
};

}  // namespace rex::codegen
