/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>

#include <rex/memory/ring_buffer.h>

namespace rex::memory {

RingBuffer::RingBuffer(uint8_t* buffer, size_t capacity)
    : buffer_(buffer), capacity_(static_cast<ring_size_t>(capacity)) {}

void RingBuffer::AdvanceRead(size_t count) {
  ring_size_t cnt = static_cast<ring_size_t>(count);
  if (read_offset_ + cnt < capacity_) {
    read_offset_ += cnt;
  } else {
    ring_size_t left_half = capacity_ - read_offset_;
    ring_size_t right_half = cnt - left_half;
    read_offset_ = right_half;
  }
}

void RingBuffer::AdvanceWrite(size_t count) {
  ring_size_t cnt = static_cast<ring_size_t>(count);
  if (write_offset_ + cnt < capacity_) {
    write_offset_ += cnt;
  } else {
    ring_size_t left_half = capacity_ - write_offset_;
    ring_size_t right_half = cnt - left_half;
    write_offset_ = right_half;
  }
}

RingBuffer::ReadRange RingBuffer::BeginRead(size_t count) {
  ring_size_t cnt = static_cast<ring_size_t>(std::min(count, static_cast<size_t>(capacity_)));
  if (!cnt) {
    return {nullptr, nullptr, 0, 0};
  }
  if (read_offset_ + cnt < capacity_) {
    return {buffer_ + read_offset_, nullptr, cnt, 0};
  } else {
    ring_size_t left_half = capacity_ - read_offset_;
    ring_size_t right_half = cnt - left_half;
    return {buffer_ + read_offset_, buffer_, left_half, right_half};
  }
}

void RingBuffer::EndRead(ReadRange read_range) {
  if (read_range.second) {
    read_offset_ = read_range.second_length;
  } else {
    read_offset_ += read_range.first_length;
  }
}

size_t RingBuffer::Read(uint8_t* buffer, size_t count) {
  count = std::min(count, static_cast<size_t>(capacity_));
  if (!count) {
    return 0;
  }

  ring_size_t cnt = static_cast<ring_size_t>(count);

  // Sanity check: Make sure we don't read over the write offset.
  if (read_offset_ < write_offset_) {
    assert_true(read_offset_ + cnt <= write_offset_);
  } else if (read_offset_ + cnt >= capacity_) {
    ring_size_t left_half = capacity_ - read_offset_;
    assert_true(cnt - left_half <= write_offset_);
  }

  if (read_offset_ + cnt < capacity_) {
    std::memcpy(buffer, buffer_ + read_offset_, cnt);
    read_offset_ += cnt;
  } else {
    ring_size_t left_half = capacity_ - read_offset_;
    ring_size_t right_half = cnt - left_half;
    std::memcpy(buffer, buffer_ + read_offset_, left_half);
    std::memcpy(buffer + left_half, buffer_, right_half);
    read_offset_ = right_half;
  }

  return count;
}

size_t RingBuffer::Write(const uint8_t* buffer, size_t count) {
  count = std::min(count, static_cast<size_t>(capacity_));
  if (!count) {
    return 0;
  }

  ring_size_t cnt = static_cast<ring_size_t>(count);

  // Sanity check: Make sure we don't write over the read offset.
  if (write_offset_ < read_offset_) {
    assert_true(write_offset_ + cnt <= read_offset_);
  } else if (write_offset_ + cnt >= capacity_) {
    ring_size_t left_half = capacity_ - write_offset_;
    assert_true(cnt - left_half <= read_offset_);
  }

  if (write_offset_ + cnt < capacity_) {
    std::memcpy(buffer_ + write_offset_, buffer, cnt);
    write_offset_ += cnt;
  } else {
    ring_size_t left_half = capacity_ - write_offset_;
    ring_size_t right_half = cnt - left_half;
    std::memcpy(buffer_ + write_offset_, buffer, left_half);
    std::memcpy(buffer_, buffer + left_half, right_half);
    write_offset_ = right_half;
  }

  return count;
}

}  // namespace rex::memory
