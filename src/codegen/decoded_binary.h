/**
 * @file        rex/codegen/decoded_binary.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "ppc/instruction.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/code_region.h>

namespace rex::codegen {

using DecodedInsn = rex::codegen::ppc::Instruction;

//=============================================================================
// Instruction Range Iterator
//=============================================================================

class InsnRange {
 public:
  class Iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = DecodedInsn*;
    using difference_type = std::ptrdiff_t;
    using pointer = DecodedInsn**;
    using reference = DecodedInsn*&;

    explicit Iterator(DecodedInsn* ptr) : ptr_(ptr) {}

    DecodedInsn* operator*() const { return ptr_; }
    Iterator& operator++() {
      ++ptr_;
      return *this;
    }
    Iterator operator++(int) {
      Iterator tmp = *this;
      ++ptr_;
      return tmp;
    }
    bool operator==(const Iterator& other) const { return ptr_ == other.ptr_; }
    bool operator!=(const Iterator& other) const { return ptr_ != other.ptr_; }

   private:
    DecodedInsn* ptr_;
  };

  InsnRange(DecodedInsn* begin, DecodedInsn* end) : begin_(begin), end_(end) {}

  Iterator begin() const { return Iterator(begin_); }
  Iterator end() const { return Iterator(end_); }
  size_t size() const { return end_ - begin_; }
  bool empty() const { return begin_ == end_; }

  DecodedInsn* data() const { return begin_; }

 private:
  DecodedInsn* begin_;
  DecodedInsn* end_;
};

//=============================================================================
// Decoded Binary - Single-pass instruction decoder
//=============================================================================

class DecodedBinary {
 public:
  explicit DecodedBinary(const BinaryView& binary);

  // Non-copyable, non-movable (holds reference to binary)
  DecodedBinary(DecodedBinary&&) = delete;
  DecodedBinary& operator=(DecodedBinary&&) = delete;
  DecodedBinary(const DecodedBinary&) = delete;
  DecodedBinary& operator=(const DecodedBinary&) = delete;

  /// Decode all executable sections (call once after construction)
  void decode();

  /// Get instruction at address (O(1) lookup)
  /// Returns nullptr if address not in any executable section
  DecodedInsn* get(uint32_t addr);
  const DecodedInsn* get(uint32_t addr) const;

  /// Get range of instructions [start, end)
  /// Returns empty range if addresses not in same section
  InsnRange range(uint32_t start, uint32_t end);

  /// Get raw data at address (for reading non-instruction data like jump tables)
  const uint8_t* rawData(uint32_t addr, size_t len) const;

  /// Read a value at address (with byte-swap for big-endian)
  template <typename T>
  std::optional<T> read(uint32_t addr) const;

  /// Get all code regions (separated by null padding)
  const std::vector<CodeRegion>& codeRegions() const { return codeRegions_; }

  /// Find code region containing address
  const CodeRegion* regionContaining(uint32_t addr) const;

  /// Check if branch from->to crosses a null boundary
  bool crossesNullBoundary(uint32_t from, uint32_t to) const;

  /// Check if address is in any code region
  bool isInCodeRegion(uint32_t addr) const;

  /// Check if instruction at address is null/invalid
  bool isNullPadding(uint32_t addr) const;

  /// Statistics
  size_t instructionCount() const;
  size_t sectionCount() const { return sections_.size(); }

 private:
  struct Section {
    uint32_t base;
    uint32_t size;
    std::vector<DecodedInsn> instructions;
    std::vector<uint8_t> data;  // Copy of raw section data

    bool contains(uint32_t addr) const { return addr >= base && addr < base + size; }

    DecodedInsn* get(uint32_t addr) {
      if (!contains(addr))
        return nullptr;
      uint32_t idx = (addr - base) / 4;
      if (idx >= instructions.size())
        return nullptr;
      return &instructions[idx];
    }

    const DecodedInsn* get(uint32_t addr) const {
      if (!contains(addr))
        return nullptr;
      uint32_t idx = (addr - base) / 4;
      if (idx >= instructions.size())
        return nullptr;
      return &instructions[idx];
    }
  };

  const BinaryView& binary_;
  std::vector<Section> sections_;
  std::vector<CodeRegion> codeRegions_;

  /// Compute code regions by finding null padding boundaries
  void computeCodeRegions();

  /// Find section containing address
  Section* findSection(uint32_t addr);
  const Section* findSection(uint32_t addr) const;
};

//=============================================================================
// Template Implementation
//=============================================================================

template <typename T>
std::optional<T> DecodedBinary::read(uint32_t addr) const {
  const uint8_t* data = rawData(addr, sizeof(T));
  if (!data)
    return std::nullopt;

  T value;
  std::memcpy(&value, data, sizeof(T));

  // Byte-swap for big-endian PPC
  if constexpr (sizeof(T) == 2) {
    value = static_cast<T>((value >> 8) | (value << 8));
  } else if constexpr (sizeof(T) == 4) {
    value = static_cast<T>(((value >> 24) & 0xFF) | ((value >> 8) & 0xFF00) |
                           ((value << 8) & 0xFF0000) | ((value << 24) & 0xFF000000));
  } else if constexpr (sizeof(T) == 8) {
    value = static_cast<T>(((value >> 56) & 0xFF) | ((value >> 40) & 0xFF00) |
                           ((value >> 24) & 0xFF0000) | ((value >> 8) & 0xFF000000) |
                           ((value << 8) & 0xFF00000000ULL) | ((value << 24) & 0xFF0000000000ULL) |
                           ((value << 40) & 0xFF000000000000ULL) |
                           ((value << 56) & 0xFF00000000000000ULL));
  }

  return value;
}

//=============================================================================
// Helper functions for branch analysis (using existing arch::ppc)
//=============================================================================

/// Check if instruction is a branch
inline bool isBranch(const DecodedInsn& insn) {
  return insn.is_branch();
}

/// Check if instruction is a call (branch with link)
inline bool isCall(const DecodedInsn& insn) {
  return insn.is_call();
}

/// Check if instruction is a return (blr)
inline bool isReturn(const DecodedInsn& insn) {
  return insn.is_return();
}

/// Check if instruction is indirect branch (bcctr/bclr)
inline bool isIndirect(const DecodedInsn& insn) {
  return insn.is_indirect_branch();
}

/// Check if instruction is a terminator (unconditional branch, return, bctr)
inline bool isTerminator(const DecodedInsn& insn) {
  return rex::codegen::ppc::is_terminator_instruction(insn.opcode);
}

/// Check if instruction is conditional branch
inline bool isConditional(const DecodedInsn& insn) {
  return insn.is_conditional();
}

/// Check if instruction is lis (addis rD, 0, imm)
inline bool isLis(const DecodedInsn& insn) {
  return insn.opcode == rex::codegen::ppc::Opcode::lis;
}

/// Get branch target address (for direct branches)
inline std::optional<uint32_t> getBranchTarget(const DecodedInsn& insn) {
  return insn.branch_target;
}

/// Check if instruction is null/invalid (null padding)
inline bool isInvalid(const DecodedInsn& insn) {
  uint32_t raw = static_cast<uint32_t>(insn.code);
  return raw == 0x00000000 || raw == 0xFFFFFFFF ||
         insn.opcode == rex::codegen::ppc::Opcode::kUnknown;
}

}  // namespace rex::codegen
