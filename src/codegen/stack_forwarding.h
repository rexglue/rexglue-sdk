/**
 * @file        codegen/stack_forwarding.h
 * @brief       Tracks the stack slots a basic block has stored, so a reload can reuse the value
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <optional>

#include <rex/types.h>

struct ppc_insn;

namespace rex::codegen {

/** The live stack slots of the instruction being emitted, each held by a numbered local. */
class StackForwarding {
 public:
  /** Number of slots, which is also the number of locals a function can declare. */
  static constexpr size_t kSlots = 8;

  void Invalidate();

  /** Forgets every slot the instruction can change. Stores through r1 are left to HoldSlot. */
  void OnInstruction(const ppc_insn& insn, int id);

  /**
   * Forgets every slot that overlaps the range, then takes the next slot in round robin order.
   *
   * @param offset The offset from r1.
   * @return Index of the slot, which names the local variable for the value.
   */
  u8 HoldSlot(i32 offset, u8 width);

  /**
   * Finds the slot with exactly this offset and width.
   *
   * @param offset The offset from r1.
   * @return The slot index, empty when no slot matches.
   */
  std::optional<u8> FindSlot(i32 offset, u8 width) const;

 private:
  struct Slot {
    i32 offset = 0;
    u8 width = 0;
    bool live = false;
  };

  std::array<Slot, kSlots> slots_{};
  u8 next_ = 0;
};

}  // namespace rex::codegen
