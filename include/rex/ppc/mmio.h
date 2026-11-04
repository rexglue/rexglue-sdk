/**
 * @file        ppc/mmio.h
 * @brief       Handlers for GPU and APU register block access
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::ppc {

enum class MMIOBlock : u8 {
  /** 0x7FC80000 to 0x7FC8FFFF. */
  kGPU,
  /** 0x7FEA0000 to 0x7FEAFFFF. */
  kAPU,
};

inline constexpr u32 kMMIOBlockSize = 0x10000;

constexpr u32 MMIOBlockBase(MMIOBlock block) {
  return block == MMIOBlock::kGPU ? 0x7FC80000u : 0x7FEA0000u;
}

/**
 * Finds the register block that holds an address.
 *
 * @return The block, or std::nullopt when the address is outside both blocks.
 */
constexpr std::optional<MMIOBlock> MMIOBlockOf(u32 address) {
  if (address - MMIOBlockBase(MMIOBlock::kGPU) < kMMIOBlockSize) {
    return MMIOBlock::kGPU;
  }
  if (address - MMIOBlockBase(MMIOBlock::kAPU) < kMMIOBlockSize) {
    return MMIOBlock::kAPU;
  }
  return std::nullopt;
}

/**
 * Reports whether an address is in a register block.
 *
 * @return True when the address is in the GPU or APU block.
 */
constexpr bool IsMMIOBlockAddress(u32 address) {
  return MMIOBlockOf(address).has_value();
}

struct MMIORecipient {
  /** Passed back to read and write. */
  void* context;
  /** Returns the 32 bit register value at a block address. */
  u32 (*read)(void* context, u32 address);
  /** Writes the 32 bit register value at a block address. */
  void (*write)(void* context, u32 address, u32 value);
};

/**
 * Routes the loads and stores of a block to a recipient. A recipient with no read or write handler
 * is refused. Call it only while no guest thread runs.
 *
 * @param block Block whose accesses are routed.
 * @param recipient The handler and its context.
 */
REX_PPC_API void AttachMMIO(MMIOBlock block, const MMIORecipient& recipient);

/**
 * Restores the null recipient when the block routes to the context.
 *
 * @param block Block to restore.
 * @param context The context the owner attached.
 */
REX_PPC_API void DetachMMIO(MMIOBlock block, const void* context);

namespace detail {

/** Recipient of each block, indexed by MMIOBlock. */
REX_PPC_API extern MMIORecipient mmio_slots[2];

}  // namespace detail

/**
 * Loads a register value.
 *
 * @return The register value, or 0 when the address is outside both blocks.
 */
REX_PPC_API u32 MMIOLoad32(u32 address);

/** Stores a register value. A store outside both blocks is ignored. */
REX_PPC_API void MMIOStore32(u32 address, u32 value);

/**
 * Loads one byte of a register, addressed as in big endian order.
 *
 * @param address The guest address of the byte.
 * @return The byte, or 0 when the address is outside both blocks.
 */
REX_PPC_API u8 MMIOLoad8(u32 address);

/**
 * Loads two bytes of a register, addressed as in big endian order.
 *
 * @return The halfword, or 0 when the address is outside both blocks.
 */
REX_PPC_API u16 MMIOLoad16(u32 address);

/** Ends the process with an error naming a store narrower than a register. */
[[noreturn]] REX_PPC_API void MMIONarrowStore(u32 address, u32 size);

}  // namespace rex::ppc
