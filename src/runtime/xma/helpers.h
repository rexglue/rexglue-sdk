/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <stdint.h>

#include <rex/types.h>

namespace rex::runtime::xma {

static constexpr u32 kMaxFrameLength = 0x7FFF;

/**
 * Returns the number of frames that begin in a packet, valid only for XMA2 packets.
 *
 * @param packet Packet bytes.
 * @return Frame count.
 */
inline u8 GetPacketFrameCount(const u8* packet) {
  return packet[0] >> 2;
}

/**
 * Returns the offset of the first frame in a packet.
 *
 * @param packet Packet bytes.
 * @return Offset in bits.
 */
inline u32 GetPacketFrameOffset(const u8* packet) {
  u32 val = static_cast<u16>(((packet[0] & 0x3) << 13) | (packet[1] << 5) | (packet[2] >> 3));
  return val + 32;
}

inline u8 GetPacketMetadata(const u8* packet) {
  return packet[2] & 0x7;
}

inline bool IsPacketXMA2Type(const u8* packet) {
  return GetPacketMetadata(packet) == 1;
}

inline u8 GetPacketSkipCount(const u8* packet) {
  return packet[3];
}

}  // namespace rex::runtime::xma
