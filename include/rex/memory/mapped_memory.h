/**
 * @file        memory/mapped_memory.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/mapped_memory.h (Ben Vanik, 2020).
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include <rex/macros.h>
#include <rex/platform.h>
#include <rex/types.h>

namespace rex::memory {

/** A file, or a range of a file, mapped into host memory. */
class REX_API MappedMemory : public rex::NonCopyable {
 public:
  /** Access mode of the mapping. */
  enum class Mode {
    kRead,      /**< Read only. */
    kReadWrite, /**< Read and write. */
  };

  /**
   * Maps an existing file.
   *
   * @param path File to map.
   * @param mode Access mode.
   * @param offset Byte offset of the mapped range in the file.
   * @param length Byte count of the range, or 0 to map to the end of the file.
   * @return The mapping, or nullptr on failure.
   */
  static std::unique_ptr<MappedMemory> Open(const std::filesystem::path& path, Mode mode,
                                            size_t offset = 0, size_t length = 0);

  /** Creates an empty mapping. */
  MappedMemory() = default;

  /**
   * Wraps memory the object does not own.
   *
   * @param data Start of the memory.
   * @param size Byte count.
   */
  MappedMemory(void* data, size_t size) : data_(data), size_(size) {}
  virtual ~MappedMemory() = default;

  /**
   * Returns a view of part of this mapping. The view does not own the memory and is valid only
   * while this mapping stays open.
   *
   * @param offset Byte offset of the view in this mapping.
   * @param length Byte count of the view.
   * @return The view.
   */
  std::unique_ptr<MappedMemory> Slice(size_t offset, size_t length) {
    return std::make_unique<MappedMemory>(data() + offset, length);
  }

  /**
   * Returns the start of the mapped memory.
   *
   * @return Pointer to the first byte, or nullptr once closed.
   */
  u8* data() const { return reinterpret_cast<u8*>(data_); }

  /**
   * Returns the size of the mapped memory.
   *
   * @return Byte count.
   */
  size_t size() const { return size_; }

  /**
   * Unmaps the memory and closes the file.
   *
   * @param truncate_size Size to truncate the file to, or 0 to keep its size.
   */
  virtual void Close(u64 truncate_size = 0) { (void)truncate_size; }

  /** Writes modified pages back to the file. */
  virtual void Flush() {}

  /**
   * Maps a different range of the file. data() and size() change to match.
   *
   * @param offset Byte offset of the new range in the file.
   * @param length Byte count of the new range, or 0 to map to the end of the file.
   * @return True on success. Always false on POSIX.
   */
  virtual bool Remap(size_t offset, size_t length) {
    (void)offset;
    (void)length;
    return false;
  }

 protected:
  void* data_ = nullptr; /**< Start of the mapped memory. */
  size_t size_ = 0;      /**< Byte count of the mapped memory. */
};

/**
 * Append only writer that spreads its data over a series of memory mapped files of a fixed
 * size. Chunk N is written to the file path with the extension .N.
 */
class REX_API ChunkedMappedMemoryWriter {
 public:
  virtual ~ChunkedMappedMemoryWriter() = default;

  /**
   * Creates a writer. POSIX has no implementation and returns nullptr.
   *
   * @param path Base path of the chunk files.
   * @param chunk_size Byte size of each chunk, rounded up to the allocation granularity.
   * @param low_address_space True to map chunks below 4 GiB.
   * @return The writer, or nullptr when unsupported.
   */
  static std::unique_ptr<ChunkedMappedMemoryWriter> Open(const std::filesystem::path& path,
                                                         size_t chunk_size,
                                                         bool low_address_space = false);

  /**
   * Reserves space in the current chunk, opening a new chunk when it is full.
   *
   * @param length Byte count, no larger than the chunk size.
   * @return Pointer to the reserved space, or nullptr on failure.
   */
  virtual u8* Allocate(size_t length) = 0;

  /** Writes every chunk back to its file. */
  virtual void Flush() = 0;

  /** Writes the data added since the last FlushNew back to the files. */
  virtual void FlushNew() = 0;

 protected:
  /**
   * Stores the writer settings.
   *
   * @param path Base path of the chunk files.
   * @param chunk_size Byte size of each chunk.
   * @param low_address_space True to map chunks below 4 GiB.
   */
  ChunkedMappedMemoryWriter(const std::filesystem::path& path, size_t chunk_size,
                            bool low_address_space)
      : path_(path), chunk_size_(chunk_size), low_address_space_(low_address_space) {}

  std::filesystem::path path_; /**< Base path of the chunk files. */
  size_t chunk_size_;          /**< Byte size of each chunk. */
  bool low_address_space_;     /**< True to map chunks below 4 GiB. */
};

}  // namespace rex::memory
