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

namespace rex::runtime::xma {

static constexpr uint32_t kMaxFrameLength = 0x7FFF;

/**
 * Returns the number of frames that begin in a packet, valid only for XMA2 packets.
 *
 * @param packet Packet bytes.
 * @return Frame count.
 */
inline uint8_t GetPacketFrameCount(const uint8_t* packet) {
  return packet[0] >> 2;
}

/**
 * Returns the offset of the first frame in a packet.
 *
 * @param packet Packet bytes.
 * @return Offset in bits.
 */
inline uint32_t GetPacketFrameOffset(const uint8_t* packet) {
  uint32_t val =
      static_cast<uint16_t>(((packet[0] & 0x3) << 13) | (packet[1] << 5) | (packet[2] >> 3));
  return val + 32;
}

inline uint8_t GetPacketMetadata(const uint8_t* packet) {
  return packet[2] & 0x7;
}

inline bool IsPacketXma2Type(const uint8_t* packet) {
  return GetPacketMetadata(packet) == 1;
}

inline uint8_t GetPacketSkipCount(const uint8_t* packet) {
  return packet[3];
}

}  // namespace rex::runtime::xma
