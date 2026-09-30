/**
 * @file        memory/arena.h
 * @brief       Chunked bump allocator
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/arena.h (Ben Vanik, 2021).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <rex/macros.h>
#include <rex/literals.h>
#include <rex/types.h>

namespace rex::memory {

/**
 * Bump allocator that hands out memory from a list of fixed size chunks. Allocations are freed
 * together by Reset or by destroying the arena.
 */
class REX_API Arena {
 public:
  /**
   * Creates an empty arena. The first chunk is allocated on the first Alloc.
   *
   * @param chunk_size Byte size of each chunk.
   */
  explicit Arena(size_t chunk_size = 4_MiB);
  ~Arena();

  /** Discards every allocation and keeps the chunks for reuse. */
  void Reset();
  /** Fills every chunk with the byte 0xCD. */
  void DebugFill();

  /**
   * Allocates memory from the active chunk, moving to the next chunk when it runs short.
   *
   * @param size Byte count, smaller than the chunk size.
   * @param align Alignment, a power of 2 no greater than 16.
   * @return Pointer to the allocation.
   */
  void* Alloc(size_t size, size_t align);

  /**
   * Allocates uninitialized storage for 1 object.
   *
   * @tparam T Object type.
   * @return Pointer to storage aligned for T.
   */
  template <typename T>
  T* Alloc() {
    return reinterpret_cast<T*>(Alloc(sizeof(T), alignof(T)));
  }

  /**
   * Moves the active chunk offset back. Alignment padding added by earlier allocations stays
   * allocated.
   *
   * @param size Byte count to give back.
   */
  void Rewind(size_t size);

  /**
   * Copies every allocation into 1 contiguous block.
   *
   * @return Block from malloc that the caller frees with free.
   */
  void* CloneContents();

  /**
   * Copies every allocation into a vector, resized to hold them.
   *
   * @tparam T Element type of the vector.
   * @param buffer Vector that receives the bytes.
   */
  template <typename T>
  void CloneContents(std::vector<T>* buffer) {
    buffer->resize(CalculateSize() / sizeof(T));
    CloneContents(buffer->data(), buffer->size() * sizeof(T));
  }

 private:
  class REX_API Chunk {
   public:
    /**
     * Allocates the chunk buffer.
     *
     * @param chunk_size Byte size of the buffer.
     */
    explicit Chunk(size_t chunk_size);
    ~Chunk();

    Chunk* next;     /**< Next chunk in the list. */
    size_t capacity; /**< Byte size of buffer. */
    u8* buffer;      /**< Chunk storage, 16 byte aligned. */
    size_t offset;   /**< Bytes in use. */
  };

  size_t CalculateSize();
  void CloneContents(void* buffer, size_t buffer_length);

  size_t chunk_size_;
  Chunk* head_chunk_;
  Chunk* active_chunk_;
};

}  // namespace rex::memory
