/**
 * @file        xenos/packet.h
 * @brief       Command packet opcodes, events and builders
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/gpu/xenos.h (Ben Vanik, 2020).
 */

#pragma once

#include <rex/assert.h>
#include <rex/memory/endian.h>
#include <rex/types.h>

namespace rex::xenos {

/** Signature that VdSwap writes as the first payload dword of its swap packet. */
constexpr memory::FourCCT kSwapSignature = memory::make_fourcc("SWAP");

/** Event values for VGT_EVENT_INITIATOR. */
enum Event : u8 {
  VS_DEALLOC = 0,
  PS_DEALLOC = 1,
  VS_DONE_TS = 2,
  PS_DONE_TS = 3,
  CACHE_FLUSH_TS = 4,
  CONTEXT_DONE = 5,
  CACHE_FLUSH = 6,
  VIZQUERY_START = 7,
  VIZQUERY_END = 8,
  SC_WAIT_WC = 9,
  MPASS_PS_CP_REFETCH = 10,
  MPASS_PS_RST_START = 11,
  MPASS_PS_INCR_START = 12,
  RST_PIX_CNT = 13,
  RST_VTX_CNT = 14,
  TILE_FLUSH = 15,
  CACHE_FLUSH_AND_INV_TS_EVENT = 20,
  ZPASS_DONE = 21,
  CACHE_FLUSH_AND_INV_EVENT = 22,
  PERFCOUNTER_START = 23,
  PERFCOUNTER_STOP = 24,
  SCREEN_EXT_INIT = 25,
  SCREEN_EXT_RPT = 26,
  VS_FETCH_DONE_TS = 27,
};

// clang-format off
/** Type 3 packet opcodes (IT_OPCODE) in the ring buffer. */
enum Type3Opcode : u8 {
  PM4_ME_INIT               = 0x48, /**< Initialize the CP micro engine. */
  PM4_NOP                   = 0x10, /**< Skip N 32 bit words. */
  PM4_INDIRECT_BUFFER       = 0x3f, /**< Indirect buffer dispatch, prefetched. */
  PM4_INDIRECT_BUFFER_PFD   = 0x37, /**< Indirect buffer dispatch, pipelined init. */
  PM4_WAIT_FOR_IDLE         = 0x26,
  PM4_WAIT_REG_MEM          = 0x3c, /**< Wait for a register or memory value. */
  PM4_WAIT_REG_EQ           = 0x52,
  PM4_WAIT_REG_GTE          = 0x53,
  PM4_WAIT_UNTIL_READ       = 0x5c,
  PM4_WAIT_IB_PFD_COMPLETE  = 0x5d, /**< Wait for IB_PFD base and size writes. */
  PM4_REG_RMW               = 0x21, /**< Register read, modify and write. */
  PM4_REG_TO_MEM            = 0x3e, /**< Copy a register to memory. */
  PM4_MEM_WRITE             = 0x3d, /**< Write N 32 bit words to memory. */
  PM4_MEM_WRITE_CNTR        = 0x4f, /**< Write CP_PROG_COUNTER to memory. */
  PM4_COND_EXEC             = 0x44, /**< Conditionally execute packets. */
  PM4_COND_WRITE            = 0x45, /**< Conditionally write memory or a register. */
  PM4_EVENT_WRITE           = 0x46, /**< Event that writes memory on completion. */
  PM4_EVENT_WRITE_SHD       = 0x58, /**< Vertex or pixel shader done event. */
  PM4_EVENT_WRITE_CFL       = 0x59, /**< Cache flush done event. */
  PM4_EVENT_WRITE_EXT       = 0x5a, /**< Screen extent event. */
  PM4_EVENT_WRITE_ZPD       = 0x5b, /**< Z pass done event. */
  PM4_DRAW_INDX             = 0x22, /**< Fetch the index buffer and draw. */
  PM4_DRAW_INDX_2           = 0x36, /**< Draw with indices in the packet. */
  PM4_DRAW_INDX_BIN         = 0x34, /**< Fetch the index buffer and bin IDs and draw. */
  PM4_DRAW_INDX_2_BIN       = 0x35, /**< Fetch bin IDs and draw with packet indices. */
  PM4_VIZ_QUERY             = 0x23, /**< Visibility query begin or end. */
  PM4_SET_STATE             = 0x25, /**< Fetch state and start shader code DMA. */
  PM4_SET_CONSTANT          = 0x2d, /**< Load a constant into the chip and memory. */
  PM4_SET_CONSTANT2         = 0x55, /**< Incremental state update. */
  PM4_SET_SHADER_CONSTANTS  = 0x56, /**< Incremental constant update. */
  PM4_LOAD_ALU_CONSTANT     = 0x2f, /**< Load constants from memory. */
  PM4_IM_LOAD               = 0x27, /**< Load instruction memory from a pointer. */
  PM4_IM_LOAD_IMMEDIATE     = 0x2b, /**< Load instruction memory from the packet. */
  PM4_LOAD_CONSTANT_CONTEXT = 0x2e, /**< Load constants from a memory location. */
  PM4_INVALIDATE_STATE      = 0x3b, /**< Invalidate selected state pointers. */
  PM4_SET_SHADER_BASES      = 0x4A, /**< Change the instruction memory partition. */
  PM4_SET_BIN_BASE_OFFSET   = 0x4B, /**< Offset added to BIN_BASE of binned draws. */
  PM4_SET_BIN_MASK          = 0x50, /**< Set the 64 bit BIN_MASK. */
  PM4_SET_BIN_SELECT        = 0x51, /**< Set the 64 bit BIN_SELECT. */
  PM4_CONTEXT_UPDATE        = 0x5e, /**< Update the current context if needed. */
  PM4_INTERRUPT             = 0x54,
  PM4_XE_SWAP               = 0x64, /**< Opcode that VdSwap writes to present. */
  PM4_IM_STORE              = 0x2c, /**< Copy instruction memory to system memory. */
  PM4_SET_BIN_MASK_LO       = 0x60, /**< Set the low BIN_MASK dword for tiled rendering. */
  PM4_SET_BIN_MASK_HI       = 0x61,
  PM4_SET_BIN_SELECT_LO     = 0x62,
  PM4_SET_BIN_SELECT_HI     = 0x63,
};
// clang-format on

/**
 * Builds a type 0 packet header that writes consecutive registers.
 *
 * Layout is ttcccccc cccccccc oiiiiiii iiiiiiii.
 *
 * @param index First register index, up to 0x7FFF.
 * @param count Register count, 1 to 0x4000.
 * @param one_reg True to write every value to the register at index.
 */
inline u32 BuildPacketType0(u16 index, u16 count, bool one_reg = false) {
  REX_ASSERT(index <= 0x7FFF);
  REX_ASSERT(count >= 1 && count <= 0x4000);
  return (0u << 30) | (((count - 1) & 0x3FFF) << 16) | (one_reg ? 1u << 15 : 0u) | (index & 0x7FFF);
}

/**
 * Builds a type 1 packet header that writes two registers.
 *
 * Layout tt?????? ??222222 22222111 11111111.
 *
 * @param index_1 First register index, up to 0x7FF.
 * @param index_2 Second register index, up to 0x7FF.
 */
inline u32 BuildPacketType1(u16 index_1, u16 index_2) {
  REX_ASSERT(index_1 <= 0x7FF);
  REX_ASSERT(index_2 <= 0x7FF);
  return (1u << 30) | ((index_2 & 0x7FF) << 11) | (index_1 & 0x7FF);
}

/** Builds a type 2 packet, a single dword no op. */
constexpr u32 BuildPacketType2() {
  return (2u << 30);
}

/**
 * Builds a type 3 packet header.
 *
 * Layout is ttcccccc cccccccc ?ooooooo ???????p.
 *
 * @param count Payload dword count, 1 to 0x4000.
 * @param predicate Whether the packet is predicated.
 */
inline u32 BuildPacketType3(Type3Opcode opcode, u16 count, bool predicate = false) {
  REX_ASSERT(opcode <= 0x7F);
  REX_ASSERT(count >= 1 && count <= 0x4000);
  return (3u << 30) | (((count - 1) & 0x3FFF) << 16) | ((opcode & 0x7F) << 8) | (predicate ? 1 : 0);
}

}  // namespace rex::xenos
