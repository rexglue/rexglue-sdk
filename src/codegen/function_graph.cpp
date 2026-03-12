/**
 * @file        rexcodegen/function_graph.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc/instruction.h"

#include <algorithm>
#include <cassert>

#include <rex/codegen/code_emitter.h>
#include <rex/codegen/function_graph.h>
#include <rex/logging.h>

#include "codegen_logging.h"

namespace rex::codegen {

//=============================================================================
// Utility
//=============================================================================

const char* AuthorityName(FunctionAuthority auth) {
  switch (auth) {
    case FunctionAuthority::GAP_FILL:
      return "gap_fill";
    case FunctionAuthority::DISCOVERED:
      return "discovered";
    case FunctionAuthority::VTABLE:
      return "vtable";
    case FunctionAuthority::HELPER:
      return "helper";
    case FunctionAuthority::PDATA:
      return "pdata";
    case FunctionAuthority::CONFIG:
      return "config";
    case FunctionAuthority::IMPORT:
      return "import";
    default:
      return "unknown";
  }
}

//=============================================================================
// FunctionNode
//=============================================================================

FunctionNode::FunctionNode(uint32_t base, uint32_t size, FunctionAuthority authority)
    : base_(base), size_(size), authority_(authority), state_(FunctionState::kRegistered) {
  // Generate default name
  char buf[32];
  snprintf(buf, sizeof(buf), "sub_%08X", base);
  name_ = buf;

  // All functions start kRegistered - waiting for discover() to assign blocks.
  // Authority prevents merging via vacancy checks, not via initial state.
}

//=============================================================================
// State Machine Methods
//=============================================================================

void FunctionNode::discover(std::vector<Block> blocks,
                            std::vector<rex::codegen::ppc::Instruction*> instructions,
                            std::set<uint32_t> labels) {
  assert(canDiscover() && "Invalid state transition: must be kRegistered");

  // Non-imports must have blocks
  if (!isImport()) {
    assert(!blocks.empty() && "Non-import function must have blocks");
  }

  blocks_ = std::move(blocks);
  instructions_ = std::move(instructions);
  labels_ = std::move(labels);

  // Update size based on blocks
  for (const auto& block : blocks_) {
    uint32_t blockEnd = block.base + block.size;
    if (blockEnd > base_ + size_) {
      size_ = blockEnd - base_;
    }
  }

  state_ = FunctionState::kDiscovered;
  REXCODEGEN_DEBUG(
      "FunctionNode 0x{:08X} ({}): DISCOVERED with {} blocks, {} instructions, {} labels", base_,
      name_, blocks_.size(), instructions_.size(), labels_.size());
}

void FunctionNode::discoverAsImport() {
  assert(canDiscover() && "Invalid state transition: must be kRegistered");
  assert(isImport() && "Only imports can use discoverAsImport()");

  // blocks_ and instructions_ remain empty for imports
  state_ = FunctionState::kDiscovered;
  REXCODEGEN_DEBUG("FunctionNode 0x{:08X} ({}): DISCOVERED as import", base_, name_);
}

bool FunctionNode::canSeal() const {
  if (state_ != FunctionState::kDiscovered)
    return false;
  if (isImport())
    return true;  // Imports can seal without blocks
  if (blocks_.empty())
    return false;  // Non-imports must have blocks
  if (!unresolvedJumps_.empty())
    return false;  // All branches must be resolved
  return true;
}

const FunctionAnalysis& FunctionNode::analysis() const {
  assert(state_ == FunctionState::kSealed && "Analysis only valid after seal");
  assert(analysis_.has_value() && "Analysis not computed");
  return *analysis_;
}

void FunctionNode::addBlock(Block block) {
  blocks_.push_back(block);

  // Extend size if block extends past current end
  uint32_t blockEnd = block.base + block.size;
  if (blockEnd > base_ + size_) {
    size_ = blockEnd - base_;
  }
}

bool FunctionNode::containsAddress(uint32_t addr) const {
  // First check overall bounds
  if (addr < base_ || addr >= base_ + size_) {
    return false;
  }

  // If no blocks defined, use linear range
  if (blocks_.empty()) {
    return true;
  }

  // Check individual blocks
  for (const auto& block : blocks_) {
    if (block.contains(addr)) {
      return true;
    }
  }

  // For CONFIG and PDATA functions, trust the declared size even if blocks don't cover it
  // This handles out-of-line switch cases where compiler places code after epilogue
  if (authority_ == FunctionAuthority::CONFIG || authority_ == FunctionAuthority::PDATA) {
    return true;  // Already passed bounds check above
  }

  return false;
}

void FunctionNode::addLabel(uint32_t addr) {
  labels_.insert(addr);
}

void FunctionNode::addCall(uint32_t site, CallTarget target) {
  calls_.push_back({site, std::move(target)});
}

void FunctionNode::addTailCall(uint32_t site, CallTarget target) {
  tailCalls_.push_back({site, std::move(target)});
}

void FunctionNode::addJumpTable(JumpTable jt) {
  // All jump table targets become labels
  for (uint32_t target : jt.targets) {
    labels_.insert(target);
  }
  jumpTables_.push_back(std::move(jt));
}

void FunctionNode::addUnresolvedJump(uint32_t site, uint32_t target, bool isCall,
                                     bool conditional) {
  unresolvedJumps_.push_back({site, target, isCall, conditional});
}

void FunctionNode::removeUnresolvedJump(uint32_t site) {
  auto it = std::remove_if(unresolvedJumps_.begin(), unresolvedJumps_.end(),
                           [site](const UnresolvedJump& j) { return j.site == site; });
  unresolvedJumps_.erase(it, unresolvedJumps_.end());
}

bool FunctionNode::tryResolveAgainst(FunctionNode* newFunction) {
  if (isSealed())
    return false;
  if (!newFunction)
    return false;

  bool anyResolved = false;

  // Check each unresolved jump
  for (auto it = unresolvedJumps_.begin(); it != unresolvedJumps_.end();) {
    if (it->target == newFunction->base()) {
      // Target matches new function's entry point -> external call/tail call
      REXCODEGEN_TRACE(
          "FunctionNode 0x{:08X}: resolved jump 0x{:08X} -> 0x{:08X} as external to {}", base_,
          it->site, it->target, newFunction->name());

      // Add as tail call (unconditional branch to another function)
      addTailCall(it->site, CallTarget::function(newFunction));

      it = unresolvedJumps_.erase(it);
      anyResolved = true;
    } else {
      ++it;
    }
  }

  return anyResolved;
}

bool FunctionNode::tryResolveAgainstImport(uint32_t importAddr, const std::string& importName) {
  if (isSealed())
    return false;

  bool anyResolved = false;

  for (auto it = unresolvedJumps_.begin(); it != unresolvedJumps_.end();) {
    if (it->target == importAddr) {
      REXCODEGEN_TRACE("FunctionNode 0x{:08X}: resolved jump 0x{:08X} -> 0x{:08X} as import {}",
                       base_, it->site, it->target, importName);

      // Add as tail call to import
      addTailCall(it->site, CallTarget::import(importAddr, importName));

      it = unresolvedJumps_.erase(it);
      anyResolved = true;
    } else {
      ++it;
    }
  }

  return anyResolved;
}

bool FunctionNode::tryResolveAsInternalLabel(uint32_t target) {
  // Check if target is within our blocks
  if (!containsAddress(target)) {
    return false;
  }

  // It's internal - add as label
  addLabel(target);
  return true;
}

void FunctionNode::absorbRegion(uint32_t regionBase, uint32_t regionSize) {
  assert(state_ != FunctionState::kSealed && "Cannot absorb into SEALED function");

  // Add as a new block
  addBlock({regionBase, regionSize});

  REXCODEGEN_DEBUG("FunctionNode 0x{:08X}: absorbed region 0x{:08X}-0x{:08X}, new size=0x{:X}",
                   base_, regionBase, regionBase + regionSize, size_);
}

void FunctionNode::seal() {
  assert(canSeal() && "Cannot seal: invariants not met");

  // Sort blocks by address for correct emission order
  std::sort(blocks_.begin(), blocks_.end(),
            [](const Block& a, const Block& b) { return a.base < b.base; });

  // Merge overlapping blocks - can happen when multiple paths reach the same code
  // (e.g., jump table targets and unconditional branches both targeting an epilogue)
  if (blocks_.size() > 1) {
    std::vector<Block> merged;
    merged.reserve(blocks_.size());
    merged.push_back(blocks_[0]);

    for (size_t i = 1; i < blocks_.size(); i++) {
      Block& last = merged.back();
      const Block& curr = blocks_[i];

      // Check for overlap or adjacency
      if (curr.base <= last.end()) {
        // Extend last block to cover both
        uint32_t newEnd = std::max(last.end(), curr.end());
        last.size = newEnd - last.base;
        REXCODEGEN_TRACE(
            "FunctionNode 0x{:08X}: merged overlapping blocks 0x{:08X}-0x{:08X} and "
            "0x{:08X}-0x{:08X}",
            base_, last.base, last.base + last.size - (newEnd - last.end()), curr.base, curr.end());
      } else {
        merged.push_back(curr);
      }
    }

    if (merged.size() < blocks_.size()) {
      REXCODEGEN_DEBUG("FunctionNode 0x{:08X}: reduced {} blocks to {} after merging", base_,
                       blocks_.size(), merged.size());
      blocks_ = std::move(merged);
    }
  }

  // Compute function analysis
  FunctionAnalysis analysis;
  analysis_ = std::move(analysis);
  state_ = FunctionState::kSealed;

  REXCODEGEN_TRACE(
      "FunctionNode 0x{:08X} ({}): SEALED with {} blocks, {} labels, {} calls, {} tail calls",
      base_, name_, blocks_.size(), labels_.size(), calls_.size(), tailCalls_.size());
}

//=============================================================================
// FunctionGraph - Code Buffer Management (WIP)
//=============================================================================
void FunctionNode::emitCpp(CodeEmitter& emit) const {
  // WIP(tomc) - needs a big refactor for this to actually work.
  assert_always("FunctionNode::emitCpp not implemented. Recompiler with builders");
  return;
  // assert(state_ == FunctionState::kSealed && "emitCpp requires sealed function");

  // if (isImport()) {
  //     // Imports emit a simple declaration/macro
  //     emit.line("// Import: {}", name_);
  //     emit.line("PPC_IMPORT_FUNC({});", name_);
  //     return;
  // }

  //// Function header
  // emit.line("PPC_FUNC({}) {{", name_);
  // emit.indent();

  //// Emit block labels placeholder
  //// Full instruction emission requires BuilderContext/Recompiler integration
  //// which will be added incrementally
  // for (const auto& block : blocks_) {
  //     emit.line("// Block 0x{:08X} - 0x{:08X}", block.base, block.end());
  // }

  // if (blocks_.empty()) {
  //     emit.comment("WARNING: No blocks in sealed function");
  // }

  //// TODO: Full instruction emission via instruction builders
  //// For now, emit placeholder that existing recompiler can fill
  // emit.comment("Instruction emission delegated to Recompiler");

  // emit.dedent();
  // emit.line("}}");
}

void FunctionGraph::addCodeBuffer(uint32_t baseAddress, const uint8_t* data, size_t size) {
  assert_always("FunctionGraph::addCodeBuffer not implemented. Use Recompiler with builders");
  return;
  // CodeBuffer buffer;
  // buffer.baseAddress = baseAddress;
  // buffer.data.assign(data, data + size);
  // codeBuffers_.push_back(std::move(buffer));
  // REXCODEGEN_DEBUG("FunctionGraph: added code buffer 0x{:08X}-0x{:08X} ({} bytes)",
  //                 baseAddress, baseAddress + static_cast<uint32_t>(size), size);
}

const uint8_t* FunctionGraph::translateCode(uint32_t addr) const {
  assert_always("FunctionGraph::translateCode not implemented. Use Recompiler with builders");
  return nullptr;
  // for (const auto& buffer : codeBuffers_) {
  //     if (buffer.contains(addr)) {
  //         return buffer.translate(addr);
  //     }
  // }
  // return nullptr;
}

void FunctionGraph::updateFunctionCodePointers() {
  // size_t updated = 0;
  // for (auto& [base, node] : functions_) {
  //     const uint8_t* code = translateCode(base);
  //     if (code) {
  //         node->setCode(code);
  //         updated++;
  //     } else {
  //         REXCODEGEN_WARN("FunctionGraph: no code buffer for function 0x{:08X}", base);
  //     }
  // }
  // REXCODEGEN_DEBUG("FunctionGraph: updated code pointers for {} functions", updated);
}

//=============================================================================
// FunctionGraph - Function Management
//=============================================================================

FunctionNode* FunctionGraph::addFunction(uint32_t base, uint32_t size, FunctionAuthority authority,
                                         bool hasXrefs) {
  // Check for existing function at this address
  auto it = functions_.find(base);
  if (it != functions_.end()) {
    FunctionNode* existing = it->second.get();

    // Higher authority wins
    if (existing->authority() >= authority) {
      REXCODEGEN_TRACE(
          "FunctionGraph: ignoring add of 0x{:08X} ({}) - existing has higher authority ({})", base,
          AuthorityName(authority), AuthorityName(existing->authority()));
      return existing;
    }

    // Replace with higher authority
    REXCODEGEN_DEBUG("FunctionGraph: replacing 0x{:08X} ({}) with ({})", base,
                     AuthorityName(existing->authority()), AuthorityName(authority));
  }

  // Create new node
  auto node = std::make_unique<FunctionNode>(base, size, authority);
  FunctionNode* nodePtr = node.get();
  functions_[base] = std::move(node);
  functionsByBase_[base] = nodePtr;

  // Track xrefs for merge eligibility
  functionHasXrefs_[base] = hasXrefs;

  // Notify all PENDING functions
  notifyFunctionAdded(nodePtr);

  return nodePtr;
}

FunctionNode* FunctionGraph::addFunction(uint32_t base, uint32_t size, FunctionAuthority authority,
                                         std::string_view name, bool hasXrefs) {
  auto* node = addFunction(base, size, authority, hasXrefs);
  if (node && !name.empty()) {
    node->setName(std::string(name));
  }
  return node;
}

FunctionNode* FunctionGraph::addImportFunction(uint32_t address, std::string_view resolvedName) {
  // Add as function with IMPORT authority
  auto* node = addFunction(address, 4, FunctionAuthority::IMPORT, resolvedName, true);
  REXCODEGEN_TRACE("FunctionGraph: added import function 0x{:08X} -> {}", address, resolvedName);
  return node;
}

FunctionNode* FunctionGraph::getFunction(uint32_t entryPoint) {
  auto it = functions_.find(entryPoint);
  return it != functions_.end() ? it->second.get() : nullptr;
}

const FunctionNode* FunctionGraph::getFunction(uint32_t entryPoint) const {
  auto it = functions_.find(entryPoint);
  return it != functions_.end() ? it->second.get() : nullptr;
}

bool FunctionGraph::removeFunction(uint32_t entryPoint) {
  auto it = functions_.find(entryPoint);
  if (it == functions_.end()) {
    return false;
  }
  REXCODEGEN_TRACE("FunctionGraph: removing absorbed function 0x{:08X}", entryPoint);
  functionsByBase_.erase(entryPoint);
  functions_.erase(it);
  functionHasXrefs_.erase(entryPoint);  // Clean up xref tracking
  return true;
}

FunctionNode* FunctionGraph::getFunctionContaining(uint32_t addr) {
  // O(log f) lookup via sorted base index: find last function with base <= addr
  auto it = functionsByBase_.upper_bound(addr);
  if (it != functionsByBase_.begin()) {
    --it;
    if (it->second->containsAddress(addr)) {
      return it->second;
    }
  }
  return nullptr;
}

const FunctionNode* FunctionGraph::getFunctionContaining(uint32_t addr) const {
  auto it = functionsByBase_.upper_bound(addr);
  if (it != functionsByBase_.begin()) {
    --it;
    if (it->second->containsAddress(addr)) {
      return it->second;
    }
  }
  return nullptr;
}

bool FunctionGraph::isEntryPoint(uint32_t addr) const {
  return functions_.contains(addr);
}

bool FunctionGraph::isImport(uint32_t addr) const {
  auto it = functions_.find(addr);
  return it != functions_.end() && it->second->authority() == FunctionAuthority::IMPORT;
}

std::vector<FunctionNode*> FunctionGraph::getPendingFunctions() {
  std::vector<FunctionNode*> result;
  for (auto& [base, node] : functions_) {
    if (node->isPending()) {
      result.push_back(node.get());
    }
  }
  return result;
}

std::vector<FunctionNode*> FunctionGraph::getSealedFunctions() {
  std::vector<FunctionNode*> result;
  for (auto& [base, node] : functions_) {
    if (node->isSealed()) {
      result.push_back(node.get());
    }
  }
  return result;
}

size_t FunctionGraph::pendingCount() const {
  size_t count = 0;
  for (const auto& [base, node] : functions_) {
    if (node->isPending())
      ++count;
  }
  return count;
}

size_t FunctionGraph::sealedCount() const {
  size_t count = 0;
  for (const auto& [base, node] : functions_) {
    if (node->isSealed())
      ++count;
  }
  return count;
}

//=============================================================================
// Function Setup (called during Discover phase)
//=============================================================================

void FunctionGraph::setFunctionName(uint32_t entry, std::string name) {
  if (auto* node = getFunction(entry)) {
    node->setName(std::move(name));
  }
}

void FunctionGraph::setFunctionHasExceptionHandler(uint32_t entry, bool val) {
  if (auto* node = getFunction(entry)) {
    node->setHasExceptionHandler(val);
  }
}

void FunctionGraph::setFunctionExceptionInfo(uint32_t entry, ExceptionInfo info) {
  if (auto* node = getFunction(entry)) {
    node->setExceptionInfo(std::move(info));
  }
}

void FunctionGraph::addBlockToFunction(uint32_t entry, Block block) {
  if (auto* node = getFunction(entry)) {
    node->addBlock(block);
  }
}

void FunctionGraph::addLabelToFunction(uint32_t entry, uint32_t label) {
  if (auto* node = getFunction(entry)) {
    node->addLabel(label);
  }
}

void FunctionGraph::addCallToFunction(uint32_t entry, uint32_t site, CallTarget target) {
  if (auto* node = getFunction(entry)) {
    node->addCall(site, std::move(target));
  }
}

void FunctionGraph::addTailCallToFunction(uint32_t entry, uint32_t site, CallTarget target) {
  if (auto* node = getFunction(entry)) {
    node->addTailCall(site, std::move(target));
  }
}

void FunctionGraph::addJumpTableToFunction(uint32_t entry, JumpTable jt) {
  if (auto* node = getFunction(entry)) {
    node->addJumpTable(std::move(jt));
  }
}

void FunctionGraph::addUnresolvedJumpToFunction(uint32_t entry, uint32_t site, uint32_t target,
                                                bool isCall, bool conditional) {
  auto* node = getFunction(entry);
  if (!node)
    return;

  // Try immediate resolution against existing functions/imports
  if (auto* targetFn = getFunction(target)) {
    // Target is a known function - resolve as call or tail call
    if (isCall) {
      node->addCall(site, CallTarget::function(targetFn));
    } else {
      node->addTailCall(site, CallTarget::function(targetFn));
    }
    REXCODEGEN_TRACE("FunctionGraph: immediately resolved 0x{:08X}->0x{:08X} as {} to {}", site,
                     target, isCall ? "call" : "tail call", targetFn->name());
    return;
  }

  if (isImport(target)) {
    // Target is an import - resolve as call or tail call to import
    auto* importNode = getFunction(target);
    const std::string& importName = importNode->name();
    if (isCall) {
      node->addCall(site, CallTarget::import(target, importName));
    } else {
      node->addTailCall(site, CallTarget::import(target, importName));
    }
    REXCODEGEN_TRACE("FunctionGraph: immediately resolved 0x{:08X}->0x{:08X} as {} to import {}",
                     site, target, isCall ? "call" : "tail call", importName);
    return;
  }

  // Not resolvable yet - add as unresolved
  node->addUnresolvedJump(site, target, isCall, conditional);
  REXCODEGEN_TRACE("FunctionGraph: added unresolved {} 0x{:08X}->0x{:08X} to function 0x{:08X}",
                   isCall ? "call" : "jump", site, target, entry);
}

//=============================================================================
// Resolution and Expansion (called during Merge phase)
//=============================================================================

size_t FunctionGraph::tryResolveFunction(uint32_t entry) {
  auto* node = getFunction(entry);
  if (!node || node->isSealed())
    return 0;

  size_t resolved = 0;

  // Get copy of unresolved jumps (list will be modified)
  auto jumps = node->unresolvedJumps();

  REXCODEGEN_TRACE("FunctionGraph::tryResolveFunction 0x{:08X}: {} unresolved jumps", entry,
                   jumps.size());

  for (const auto& jump : jumps) {
    // Try internal label first
    if (node->tryResolveAsInternalLabel(jump.target)) {
      REXCODEGEN_TRACE("  0x{:08X}->0x{:08X}: resolved as internal label", jump.site, jump.target);
      node->removeUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    // Try function entry
    if (auto* targetFn = getFunction(jump.target)) {
      REXCODEGEN_TRACE("  0x{:08X}->0x{:08X}: resolved as {} to function {}", jump.site,
                       jump.target, jump.isCall ? "call" : "tail call", targetFn->name());
      if (jump.isCall) {
        node->addCall(jump.site, CallTarget::function(targetFn));
      } else {
        node->addTailCall(jump.site, CallTarget::function(targetFn));
      }
      node->removeUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    // Try import
    if (isImport(jump.target)) {
      auto* importNode = getFunction(jump.target);
      const std::string& importName = importNode->name();
      REXCODEGEN_TRACE("  0x{:08X}->0x{:08X}: resolved as {} to import {}", jump.site, jump.target,
                       jump.isCall ? "call" : "tail call", importName);
      if (jump.isCall) {
        node->addCall(jump.site, CallTarget::import(jump.target, importName));
      } else {
        node->addTailCall(jump.site, CallTarget::import(jump.target, importName));
      }
      node->removeUnresolvedJump(jump.site);
      resolved++;
      continue;
    }

    REXCODEGEN_TRACE("  0x{:08X}->0x{:08X}: still unresolved", jump.site, jump.target);
  }

  return resolved;
}

void FunctionGraph::absorbRegionIntoFunction(uint32_t entry, uint32_t regionBase,
                                             uint32_t regionSize) {
  if (auto* node = getFunction(entry)) {
    node->absorbRegion(regionBase, regionSize);
  }
}

bool FunctionGraph::trySealFunction(uint32_t entry) {
  auto* node = getFunction(entry);
  if (!node || node->isSealed())
    return false;

  if (node->canSeal()) {
    node->seal();
    return true;
  }
  return false;
}

size_t FunctionGraph::sealAllReady() {
  size_t sealed = 0;
  size_t couldNotSeal = 0;
  for (auto& [base, node] : functions_) {
    if (node->isPending()) {
      if (node->canSeal()) {
        node->seal();
        sealed++;
      } else {
        couldNotSeal++;
        REXCODEGEN_WARN("FunctionGraph::sealAllReady: 0x{:08X} ({}) cannot seal ({} unresolved)",
                        base, node->name(), node->unresolvedJumps().size());
        for (const auto& jump : node->unresolvedJumps()) {
          REXCODEGEN_WARN("  0x{:08X} -> 0x{:08X}", jump.site, jump.target);
        }
      }
    }
  }
  REXCODEGEN_DEBUG("FunctionGraph::sealAllReady: sealed {} functions, {} could not seal", sealed,
                   couldNotSeal);
  return sealed;
}

void FunctionGraph::sealAll() {
  std::vector<std::pair<uint32_t, std::string>> errors;

  for (auto& [base, node] : functions_) {
    if (node->isSealed())
      continue;

    if (!node->canSeal()) {
      std::string reason;
      if (node->isRegistered()) {
        reason = "still in kRegistered state (blocks not discovered)";
      } else if (node->blocks().empty() && !node->isImport()) {
        reason = "has 0 blocks (non-import)";
      } else if (!node->unresolvedJumps().empty()) {
        reason = fmt::format("{} unresolved jumps", node->unresolvedJumps().size());
      } else {
        reason = "unknown reason";
      }
      errors.emplace_back(base, fmt::format("{} (0x{:08X}): {}", node->name(), base, reason));
      continue;
    }

    node->seal();
  }

  if (!errors.empty()) {
    std::string msg = fmt::format("sealAll: {} functions cannot be sealed:\n", errors.size());
    for (const auto& [addr, err] : errors) {
      msg += fmt::format("  - {}\n", err);
    }
    throw std::runtime_error(msg);
  }

  REXCODEGEN_INFO("FunctionGraph::sealAll: all {} functions sealed", functions_.size());
}

//=============================================================================
// Vacancy Checking
//=============================================================================

void FunctionGraph::registerChunk(uint32_t base, uint32_t size) {
  chunks_.emplace_back(base, size);
  REXCODEGEN_TRACE("FunctionGraph: registered chunk 0x{:08X}-0x{:08X}", base, base + size);
}

bool FunctionGraph::isVacant(uint32_t fromAddr, uint32_t targetAddr) const {
  // Rule 1: Check for null dword at boundary
  if (memoryReader_) {
    if (targetAddr > fromAddr) {
      // Check for null at target (typical inter-function padding)
      auto val = memoryReader_(targetAddr);
      if (val && *val == 0x00000000) {
        REXCODEGEN_TRACE("FunctionGraph::isVacant: null dword at 0x{:08X} blocks vacancy",
                         targetAddr);
        return false;
      }
    }
  }

  // Rule 2: Check if any chunk claims the target region
  for (const auto& [chunkBase, chunkSize] : chunks_) {
    if (targetAddr >= chunkBase && targetAddr < chunkBase + chunkSize) {
      REXCODEGEN_TRACE("FunctionGraph::isVacant: chunk 0x{:08X}-0x{:08X} claims 0x{:08X}",
                       chunkBase, chunkBase + chunkSize, targetAddr);
      return false;
    }
  }

  // Rule 3: Check if target falls within any function's range
  for (const auto& [base, node] : functions_) {
    if (node->containsAddress(targetAddr)) {
      // Target is within a function - check if it's mergeable
      if (isMergeableEntryPoint(targetAddr)) {
        // GAP_FILL can be absorbed
        REXCODEGEN_TRACE("FunctionGraph::isVacant: 0x{:08X} is mergeable (GAP_FILL)", targetAddr);
        continue;  // Don't block, it's mergeable
      }
      // Protected function - blocks vacancy
      REXCODEGEN_TRACE("FunctionGraph::isVacant: 0x{:08X} is within protected function 0x{:08X}",
                       targetAddr, base);
      return false;
    }
  }

  // green light for vacancy
  return true;
}

bool FunctionGraph::isMergeableEntryPoint(uint32_t addr) const {
  // Only entry points can be mergeable
  auto it = functions_.find(addr);
  if (it == functions_.end()) {
    return false;  // Not an entry point
  }

  const FunctionNode* node = it->second.get();

  // Only GAP_FILL authority can be absorbed
  // All other authorities represent immutable entry points
  return node->authority() == FunctionAuthority::GAP_FILL;
}

TargetKind FunctionGraph::classifyTarget(uint32_t target, uint32_t callerAddr,
                                         bool isCallInstruction) const {
  // Find the caller's function
  const FunctionNode* callerFn = getFunctionContaining(callerAddr);

  // Case 1: Target is an import - always a call/tail-call
  if (isImport(target)) {
    return TargetKind::Import;
  }

  // Case 2: Target is the caller's own entry point
  if (callerFn && target == callerFn->base()) {
    // bl to own base = recursive call (Function)
    // b to own base = loop back to start (InternalLabel)
    return isCallInstruction ? TargetKind::Function : TargetKind::InternalLabel;
  }

  // Case 3: Target is a DIFFERENT function's entry point - this is a call/tail-call
  // This handles cases where a small thunk function branches to another function
  // whose entry point happens to fall within the thunk's address range
  if (isEntryPoint(target)) {
    return TargetKind::Function;
  }

  // Case 4: Target is inside caller's function -> InternalLabel
  // For bl, this would be a rare PIC code pattern
  if (callerFn && callerFn->containsAddress(target)) {
    return TargetKind::InternalLabel;
  }

  // Case 5: Unknown target
  return TargetKind::Unknown;
}

void FunctionGraph::notifyFunctionAdded(FunctionNode* newFunction) {
  for (auto& [base, node] : functions_) {
    if (node.get() != newFunction && node->isPending()) {
      node->tryResolveAgainst(newFunction);
    }
  }
}

}  // namespace rex::codegen
