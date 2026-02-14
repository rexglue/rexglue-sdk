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

// This file contains some functions used to help parse XMA data.

#pragma once

#include <stdint.h>

namespace rex::audio::xma {

static const uint32_t kMaxFrameLength = 0x7FFF;

// Get number of frames that /begin/ in this packet.
inline uint32_t GetPacketFrameCount(uint8_t* packet) {
  return (uint8_t)(packet[0] >> 2);
}

// Get the first frame offset in bits
inline uint32_t GetPacketFrameOffset(uint8_t* packet) {
  uint32_t val = (uint16_t)(((packet[0] & 0x3) << 13) | (packet[1] << 5) | (packet[2] >> 3));
  // if (val > kBitsPerPacket - kBitsPerHeader) {
  //   // There is no data in this packet
  //   return -1;
  // } else {
  return val + 32;
  // }
}

inline uint32_t GetPacketMetadata(uint8_t* packet) {
  return (uint8_t)(packet[2] & 0x7);
}

inline uint32_t GetPacketSkipCount(uint8_t* packet) {
  return (uint8_t)(packet[3]);
}

}  // namespace rex::audio::xma
