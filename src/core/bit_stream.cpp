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

#include <algorithm>
#include <cstring>

#include <rex/assert.h>
#include <rex/memory/utils.h>
#include <rex/stream.h>
#include <rex/types.h>

namespace rex {

BitStream::BitStream(uint8_t* buffer, size_t size_in_bits)
    : buffer_(buffer), size_bits_(size_in_bits) {}

BitStream::~BitStream() = default;

void BitStream::SetOffset(size_t offset_bits) {
  REX_ASSERT(!(offset_bits > size_bits_));
  offset_bits_ = std::min(offset_bits, size_bits_);
}

size_t BitStream::BitsRemaining() {
  return size_bits_ - offset_bits_;
}

uint64_t BitStream::Peek(size_t num_bits) {
  // At most 57 bits fit since 57 = 7 * 8 + 1 spans at most 8 bytes, the widest load.
  REX_ASSERT(!(num_bits > 57));
  REX_ASSERT(!(offset_bits_ + num_bits > size_bits_));

  size_t offset_bytes = offset_bits_ >> 3;
  size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);

  uint64_t bits = memory::load<be_u64>(buffer_ + offset_bytes);

  bits >>= 64 - (rel_offset_bits + num_bits);

  bits &= (1ULL << num_bits) - 1;

  return bits;
}

uint64_t BitStream::Read(size_t num_bits) {
  uint64_t val = Peek(num_bits);
  Advance(num_bits);

  return val;
}

bool BitStream::Write(uint64_t val, size_t num_bits) {
  REX_ASSERT(!(num_bits > 57));
  REX_ASSERT(!(offset_bits_ + num_bits > size_bits_));

  size_t offset_bytes = offset_bits_ >> 3;
  size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);

  uint64_t mask = (1ULL << num_bits) - 1;
  mask <<= 64 - (rel_offset_bits + num_bits);
  mask = ~mask;

  val <<= 64 - (rel_offset_bits + num_bits);

  uint64_t bits = memory::load<be_u64>(buffer_ + offset_bytes);

  bits &= mask;

  bits |= val;

  memory::store(buffer_ + offset_bytes, be_u64(bits));

  Advance(num_bits);

  return true;
}

size_t BitStream::Copy(uint8_t* dest_buffer, size_t num_bits) {
  size_t offset_bytes = offset_bits_ >> 3;
  size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);
  size_t bits_left = num_bits;
  size_t out_offset_bytes = 0;

  if (rel_offset_bits) {
    uint64_t bits = Peek(8 - rel_offset_bits);
    uint8_t clear_mask = ~((static_cast<uint8_t>(1) << rel_offset_bits) - 1);
    dest_buffer[out_offset_bytes] &= clear_mask;
    dest_buffer[out_offset_bytes] |= static_cast<uint8_t>(bits);

    bits_left -= 8 - rel_offset_bits;
    Advance(8 - rel_offset_bits);
    out_offset_bytes++;
  }

  if (bits_left >= 8) {
    std::memcpy(dest_buffer + out_offset_bytes, buffer_ + offset_bytes + out_offset_bytes,
                bits_left / 8);
    out_offset_bytes += (bits_left / 8);
    Advance((bits_left / 8) * 8);
    bits_left -= (bits_left / 8) * 8;
  }

  if (bits_left) {
    uint64_t bits = Peek(bits_left);
    bits <<= 8 - bits_left;

    uint8_t clear_mask = ((static_cast<uint8_t>(1) << bits_left) - 1);
    dest_buffer[out_offset_bytes] &= clear_mask;
    dest_buffer[out_offset_bytes] |= static_cast<uint8_t>(bits);
    Advance(bits_left);
  }

  return rel_offset_bits;
}

void BitStream::Advance(size_t num_bits) {
  SetOffset(offset_bits_ + num_bits);
}

}  // namespace rex
