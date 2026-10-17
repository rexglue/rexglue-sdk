/**
 * @file        mmio.h
 * @brief       Recipients of title loads and stores to the GPU and APU register blocks
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
#include <rex/export.h>
#include <rex/types.h>

namespace rex {

/** Guest register blocks that MMIO accesses reach. */
enum class MMIOBlock : u8 {
  /** 0x7FC80000 to 0x7FC8FFFF. */
  kGPU,
  /** 0x7FEA0000 to 0x7FEAFFFF. */
  kAPU,
};

/** Size of each register block in bytes. */
inline constexpr u32 kMMIOBlockSize = 0x10000;

/**
 * Returns the first guest address of a register block.
 *
 * @param block The register block.
 * @return The block base address.
 */
constexpr u32 MMIOBlockBase(MMIOBlock block) {
  return block == MMIOBlock::kGPU ? 0x7FC80000u : 0x7FEA0000u;
}

/**
 * Finds the register block that holds an address.
 *
 * @param address The guest address.
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
 * @param address The guest address.
 * @return True when the address is in the GPU or APU block.
 */
constexpr bool IsMMIOBlockAddress(u32 address) {
  return MMIOBlockOf(address).has_value();
}

/** A register block handler and its context. */
struct MMIORecipient {
  /** Passed back to read and write. */
  void* context;
  /** Returns the 32 bit register value at a block address. */
  u32 (*read)(void* context, u32 address);
  /** Writes the 32 bit register value at a block address. */
  void (*write)(void* context, u32 address, u32 value);
};

/**
 * Routes the loads and stores of a block to a recipient. A recipient with no read or no write
 * handler is refused with an error.
 *
 * @param block The register block.
 * @param recipient The handler and its context.
 */
REX_API void AttachMMIO(MMIOBlock block, const MMIORecipient& recipient);

/**
 * Restores the null recipient when the block still routes to the context.
 *
 * @param block The register block.
 * @param context The context the owner attached.
 */
REX_API void DetachMMIO(MMIOBlock block, const void* context);

namespace detail {

/** Recipient of each block, indexed by MMIOBlock. */
REX_API extern MMIORecipient mmio_slots[2];

}  // namespace detail

namespace ppc {

/**
 * Loads a register for a guarded access.
 *
 * @param address The guest address.
 * @return The register value, or 0 when the address is outside both blocks.
 */
REX_API u32 MMIOLoad32(u32 address);

/**
 * Stores a register for a guarded access. A store outside both blocks is dropped.
 *
 * @param address The guest address.
 * @param value The register value.
 */
REX_API void MMIOStore32(u32 address, u32 value);

/**
 * Loads 1 byte of a register for a guarded access, from the big endian lane the address names.
 *
 * @param address The guest address of the byte.
 * @return The byte, or 0 when the address is outside both blocks.
 */
REX_API u8 MMIOLoad8(u32 address);

/**
 * Loads 2 bytes of a register for a guarded access, from the big endian half the address names.
 *
 * @param address The guest address of the halfword.
 * @return The halfword, or 0 when the address is outside both blocks.
 */
REX_API u16 MMIOLoad16(u32 address);

/**
 * Ends the process with an error naming a store narrower than a register.
 *
 * @param address The guest address of the store.
 * @param size The store size in bytes.
 */
[[noreturn]] REX_API void MMIONarrowWrite(u32 address, u32 size);

}  // namespace ppc

}  // namespace rex
