/**
 * @file        gpu/xenos/host/deferred_stream.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <rex/types.h>

namespace rex::gpu {

/**
 * A stream of commands, each a header and its arguments in whole uintmax_t elements.
 *
 * @tparam CommandType The command enum of the backend.
 */
template <typename CommandType>
class DeferredStream {
 public:
  /** Start of every command in the stream. */
  struct Header {
    CommandType command;         /**< The command. */
    u32 arguments_size_elements; /**< Elements of arguments that follow the header. */
  };

  /** Elements a header takes in the stream. */
  static constexpr size_t kHeaderSizeElements =
      (sizeof(Header) + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);

  /**
   * Reserves room for bytes of commands.
   *
   * @param bytes The size in bytes.
   */
  void Reserve(size_t bytes) { stream_.reserve(bytes / sizeof(uintmax_t)); }

  /** Drops every command and keeps the capacity. */
  void Clear() { stream_.clear(); }

  /**
   * Returns the first element.
   *
   * @return The first element.
   */
  const uintmax_t* Data() const { return stream_.data(); }

  /**
   * Returns the element count.
   *
   * @return The element count.
   */
  size_t Size() const { return stream_.size(); }

  /**
   * Appends a header and room for its arguments.
   *
   * @param command The command.
   * @param arguments_size_bytes The size of the arguments in bytes.
   * @return The start of the argument elements, valid until the next write.
   */
  void* Write(CommandType command, size_t arguments_size_bytes) {
    size_t arguments_size_elements =
        (arguments_size_bytes + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);
    size_t offset = stream_.size();
    stream_.resize(offset + kHeaderSizeElements + arguments_size_elements);
    Header& header = *reinterpret_cast<Header*>(stream_.data() + offset);
    header.command = command;
    header.arguments_size_elements = static_cast<u32>(arguments_size_elements);
    return stream_.data() + (offset + kHeaderSizeElements);
  }

 private:
  std::vector<uintmax_t> stream_;
};

}  // namespace rex::gpu
