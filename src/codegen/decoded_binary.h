/**
 * @file        codegen/decoded_binary.h
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
#include <rex/types.h>

namespace rex::codegen {

using DecodedInsn = rex::codegen::detail::Instruction;

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

class DecodedBinary : public rex::NonCopyable {
 public:
  explicit DecodedBinary(const BinaryView& binary);

  /** Decodes all executable sections. */
  void Decode();

  /**
   * Returns the instruction at an address in O(1).
   *
   * @param addr Instruction address.
   * @return The instruction, or nullptr when the address is not in an executable section.
   */
  DecodedInsn* FindInsn(u32 addr);
  const DecodedInsn* FindInsn(u32 addr) const;

  /**
   * Returns the instructions in [start, end).
   *
   * @param start First address.
   * @param end Address after the last instruction.
   * @return The range, empty when the addresses are not in the same section.
   */
  InsnRange GetRange(u32 start, u32 end);

  /**
   * Returns raw section bytes such as jump table data.
   *
   * @param addr Start address.
   * @param len Byte count.
   * @return Pointer to the bytes, or nullptr when the span is not in a section.
   */
  const u8* GetRawData(u32 addr, size_t len) const;

  /**
   * Reads a value at an address, byte swapping from big endian.
   *
   * @tparam T Integer type of 2, 4 or 8 bytes.
   * @param addr Address to read.
   * @return The value, or nullopt when the bytes are not in a section.
   */
  template <typename T>
  std::optional<T> Read(u32 addr) const;

  /** Returns the code regions, separated by null padding. */
  const std::vector<CodeRegion>& GetCodeRegions() const { return code_regions_; }

  /**
   * Returns the code region containing an address.
   *
   * @param addr Address to find.
   * @return The region, or nullptr when none contains the address.
   */
  const CodeRegion* FindRegionContaining(u32 addr) const;

  /**
   * Returns the number of decoded instructions.
   *
   * @return The instruction count.
   */
  size_t GetInstructionCount() const;

 private:
  struct Section {
    u32 base;
    u32 size;
    std::vector<DecodedInsn> instructions;
    std::vector<u8> data;

    bool Contains(u32 addr) const { return addr >= base && addr < base + size; }

    DecodedInsn* FindInsn(u32 addr) {
      if (!Contains(addr))
        return nullptr;
      u32 idx = (addr - base) / 4;
      if (idx >= instructions.size())
        return nullptr;
      return &instructions[idx];
    }

    const DecodedInsn* FindInsn(u32 addr) const {
      if (!Contains(addr))
        return nullptr;
      u32 idx = (addr - base) / 4;
      if (idx >= instructions.size())
        return nullptr;
      return &instructions[idx];
    }
  };

  const BinaryView& binary_;
  std::vector<Section> sections_;
  std::vector<CodeRegion> code_regions_;

  /** Computes code regions by finding null padding boundaries. */
  void ComputeCodeRegions();

  /**
   * Returns the section containing an address.
   *
   * @param addr Address to find.
   * @return The section, or nullptr when none contains the address.
   */
  Section* FindSection(u32 addr);
  const Section* FindSection(u32 addr) const;
};

template <typename T>
std::optional<T> DecodedBinary::Read(u32 addr) const {
  const u8* data = GetRawData(addr, sizeof(T));
  if (!data)
    return std::nullopt;

  T value;
  std::memcpy(&value, data, sizeof(T));

  return rex::byte_swap(value);
}

/**
 * Reports whether the instruction is a branch.
 *
 * @param insn Instruction to test.
 * @return True when it is a branch.
 */
inline bool IsBranch(const DecodedInsn& insn) {
  return insn.IsBranch();
}

/**
 * Reports whether the instruction is a call, a branch with link.
 *
 * @param insn Instruction to test.
 * @return True when it is a call.
 */
inline bool IsCall(const DecodedInsn& insn) {
  return insn.IsCall();
}

/**
 * Reports whether the instruction is a return, blr.
 *
 * @param insn Instruction to test.
 * @return True when it is a return.
 */
inline bool IsReturn(const DecodedInsn& insn) {
  return insn.IsReturn();
}

/**
 * Reports whether the instruction ends a block, as an unconditional branch, return or bctr.
 *
 * @param insn Instruction to test.
 * @return True when it is a terminator.
 */
inline bool IsTerminator(const DecodedInsn& insn) {
  return rex::codegen::detail::IsTerminatorInstruction(insn.opcode);
}

/**
 * Reports whether the instruction is a conditional branch.
 *
 * @param insn Instruction to test.
 * @return True when it is conditional.
 */
inline bool IsConditional(const DecodedInsn& insn) {
  return insn.IsConditional();
}

/**
 * Returns the target of a direct branch.
 *
 * @param insn Instruction to test.
 * @return The target, or nullopt when there is none.
 */
inline std::optional<u32> GetBranchTarget(const DecodedInsn& insn) {
  return insn.branch_target;
}

/**
 * Reports whether the instruction is null padding or invalid.
 *
 * @param insn Instruction to test.
 * @return True when it is invalid.
 */
inline bool IsInvalid(const DecodedInsn& insn) {
  u32 raw = static_cast<u32>(insn.code);
  return raw == 0x00000000 || raw == 0xFFFFFFFF ||
         insn.opcode == rex::codegen::detail::Opcode::kUnknown;
}

}  // namespace rex::codegen
