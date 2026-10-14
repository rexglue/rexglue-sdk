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
#include <rex/memory/byte_swap.h>
#include <rex/stream.h>
#include <rex/types.h>

namespace rex {

BitStream::BitStream(u8* buffer, size_t size_in_bits) : buffer_(buffer), size_bits_(size_in_bits) {}

void BitStream::SetOffset(size_t offset_bits) {
  REX_ASSERT(!(offset_bits > size_bits_));
  offset_bits_ = std::min(offset_bits, size_bits_);
}

size_t BitStream::GetBitsRemaining() const {
  return size_bits_ - offset_bits_;
}

u64 BitStream::Peek(size_t num_bits) {
  // At most 57 bits fit since 57 = 7 * 8 + 1 spans at most 8 bytes, the widest load.
  REX_ASSERT(!(num_bits > 57));
  REX_ASSERT(!(offset_bits_ + num_bits > size_bits_));

  size_t offset_bytes = offset_bits_ >> 3;
  size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);

  u64 bits = memory::load<be_u64>(buffer_ + offset_bytes);

  bits >>= 64 - (rel_offset_bits + num_bits);

  bits &= (1ULL << num_bits) - 1;

  return bits;
}

u64 BitStream::Read(size_t num_bits) {
  u64 val = Peek(num_bits);
  Advance(num_bits);

  return val;
}

bool BitStream::Write(u64 val, size_t num_bits) {
  REX_ASSERT(!(num_bits > 57));
  REX_ASSERT(!(offset_bits_ + num_bits > size_bits_));

  size_t offset_bytes = offset_bits_ >> 3;
  size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);

  u64 mask = (1ULL << num_bits) - 1;
  mask <<= 64 - (rel_offset_bits + num_bits);
  mask = ~mask;

  val <<= 64 - (rel_offset_bits + num_bits);

  u64 bits = memory::load<be_u64>(buffer_ + offset_bytes);

  bits &= mask;

  bits |= val;

  memory::store(buffer_ + offset_bytes, be_u64(bits));

  Advance(num_bits);

  return true;
}

size_t BitStream::Copy(u8* dest_buffer, size_t num_bits) {
  size_t offset_bytes = offset_bits_ >> 3;
  size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);
  size_t bits_left = num_bits;
  size_t out_offset_bytes = 0;

  if (rel_offset_bits) {
    u64 bits = Peek(8 - rel_offset_bits);
    u8 clear_mask = ~((static_cast<u8>(1) << rel_offset_bits) - 1);
    dest_buffer[out_offset_bytes] &= clear_mask;
    dest_buffer[out_offset_bytes] |= static_cast<u8>(bits);

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
    u64 bits = Peek(bits_left);
    bits <<= 8 - bits_left;

    u8 clear_mask = ((static_cast<u8>(1) << bits_left) - 1);
    dest_buffer[out_offset_bytes] &= clear_mask;
    dest_buffer[out_offset_bytes] |= static_cast<u8>(bits);
    Advance(bits_left);
  }

  return rel_offset_bits;
}

void BitStream::Advance(size_t num_bits) {
  SetOffset(offset_bits_ + num_bits);
}

}  // namespace rex
