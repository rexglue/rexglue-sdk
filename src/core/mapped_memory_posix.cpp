/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/filesystem.h>
#include <rex/math.h>
#include <rex/memory/mapped_memory.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>

namespace rex::memory {

class PosixMappedMemory : public MappedMemory {
 public:
  PosixMappedMemory(void* data, size_t size, int file_descriptor, int protection)
      : MappedMemory(data, size), file_descriptor_(file_descriptor), protection_(protection) {}

  ~PosixMappedMemory() override {
    if (data_) {
      munmap(data_, size());
    }
    if (file_descriptor_ >= 0) {
      close(file_descriptor_);
    }
  }

  static std::unique_ptr<PosixMappedMemory> WrapFileDescriptor(int file_descriptor, Mode mode,
                                                               size_t offset = 0,
                                                               size_t length = 0) {
    const size_t aligned_offset = offset & ~(memory::allocation_granularity() - 1);
    const size_t offset_in_mapping = offset - aligned_offset;
    int protection = 0;
    switch (mode) {
      case Mode::kRead:
        protection |= PROT_READ;
        break;
      case Mode::kReadWrite:
        protection |= PROT_READ | PROT_WRITE;
        break;
    }

    struct stat64 file_stat;
    if (fstat64(file_descriptor, &file_stat)) {
      close(file_descriptor);
      return nullptr;
    }
    const size_t file_size = size_t(file_stat.st_size);
    if (aligned_offset > file_size) {
      close(file_descriptor);
      return nullptr;
    }

    size_t map_length = length ? (length + offset_in_mapping) : (file_size - aligned_offset);
    if (!map_length) {
      close(file_descriptor);
      return nullptr;
    }

    void* data = mmap(0, map_length, protection, MAP_SHARED, file_descriptor, aligned_offset);
    if (!data || data == MAP_FAILED) {
      close(file_descriptor);
      return nullptr;
    }

    return std::make_unique<PosixMappedMemory>(data, map_length, file_descriptor, protection);
  }

  void Close(uint64_t truncate_size) override {
    if (data_) {
      munmap(data_, size());
      data_ = nullptr;
    }
    if (file_descriptor_ >= 0) {
      if (truncate_size) {
        ftruncate64(file_descriptor_, off64_t(truncate_size));
      }
      close(file_descriptor_);
      file_descriptor_ = -1;
    }
  }

  void Flush() override { msync(data(), size(), MS_ASYNC); }

  bool Remap(size_t offset, size_t length) override {
    if (file_descriptor_ < 0) {
      return false;
    }

    const size_t aligned_offset = offset & ~(memory::allocation_granularity() - 1);
    const size_t offset_in_mapping = offset - aligned_offset;

    struct stat64 file_stat;
    if (fstat64(file_descriptor_, &file_stat)) {
      return false;
    }
    const size_t file_size = size_t(file_stat.st_size);
    if (aligned_offset > file_size) {
      return false;
    }

    size_t map_length = length ? (length + offset_in_mapping) : (file_size - aligned_offset);
    if (!map_length) {
      return false;
    }

    if (data_) {
      munmap(data_, size());
      data_ = nullptr;
      size_ = 0;
    }

    void* remapped =
        mmap(nullptr, map_length, protection_, MAP_SHARED, file_descriptor_, aligned_offset);
    if (!remapped || remapped == MAP_FAILED) {
      return false;
    }

    data_ = remapped;
    size_ = map_length;
    return true;
  }

 private:
  int file_descriptor_;
  int protection_;
};

std::unique_ptr<MappedMemory> MappedMemory::Open(const std::filesystem::path& path, Mode mode,
                                                 size_t offset, size_t length) {
  int open_flags = 0;
  switch (mode) {
    case Mode::kRead:
      open_flags |= O_RDONLY;
      break;
    case Mode::kReadWrite:
      open_flags |= O_RDWR;
      break;
  }
  int file_descriptor = open(path.c_str(), open_flags);
  if (file_descriptor < 0) {
    return nullptr;
  }
  return PosixMappedMemory::WrapFileDescriptor(file_descriptor, mode, offset, length);
}

#if REX_PLATFORM_ANDROID
std::unique_ptr<MappedMemory> MappedMemory::OpenForAndroidContentUri(const std::string_view uri,
                                                                     Mode mode, size_t offset,
                                                                     size_t length) {
  const char* open_mode = nullptr;
  switch (mode) {
    case Mode::kRead:
      open_mode = "r";
      break;
    case Mode::kReadWrite:
      open_mode = "rw";
      break;
  }
  int file_descriptor = rex::filesystem::OpenAndroidContentFileDescriptor(uri, open_mode);
  if (file_descriptor < 0) {
    return nullptr;
  }
  return PosixMappedMemory::WrapFileDescriptor(file_descriptor, mode, offset, length);
}
#endif  // REX_PLATFORM_ANDROID

class PosixChunkedMappedMemoryWriter : public ChunkedMappedMemoryWriter {
 public:
  PosixChunkedMappedMemoryWriter(const std::filesystem::path& path, size_t chunk_size,
                                 bool low_address_space)
      : ChunkedMappedMemoryWriter(path, chunk_size, low_address_space) {}

  ~PosixChunkedMappedMemoryWriter() override {
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();
  }

  uint8_t* Allocate(size_t length) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!chunks_.empty()) {
      uint8_t* result = chunks_.back()->Allocate(length);
      if (result) {
        return result;
      }
    }
    auto chunk = std::make_unique<Chunk>(chunk_size_);
    std::filesystem::path chunk_path = path_;
    chunk_path.replace_extension(std::string(".") + std::to_string(chunks_.size()));
    if (!chunk->Open(chunk_path, low_address_space_)) {
      return nullptr;
    }
    uint8_t* result = chunk->Allocate(length);
    chunks_.push_back(std::move(chunk));
    return result;
  }

  void Flush() override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& chunk : chunks_) {
      chunk->Flush();
    }
  }

  void FlushNew() override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& chunk : chunks_) {
      chunk->FlushNew();
    }
  }

 private:
  class Chunk {
   public:
    explicit Chunk(size_t capacity)
        : file_descriptor_(-1),
          data_(nullptr),
          offset_(0),
          capacity_(capacity),
          last_flush_offset_(0) {}

    ~Chunk() {
      if (data_) {
        munmap(data_, capacity_);
      }
      if (file_descriptor_ >= 0) {
        close(file_descriptor_);
      }
    }

    bool Open(const std::filesystem::path& path, bool low_address_space) {
      file_descriptor_ = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0666);
      if (file_descriptor_ < 0) {
        return false;
      }
      if (ftruncate64(file_descriptor_, static_cast<off64_t>(capacity_)) != 0) {
        return false;
      }

      int flags = MAP_SHARED;
#if REX_PLATFORM_LINUX && defined(__x86_64__) && defined(MAP_32BIT)
      if (low_address_space) {
        flags |= MAP_32BIT;
      }
#else
      (void)low_address_space;
#endif

      data_ = static_cast<uint8_t*>(
          mmap(nullptr, capacity_, PROT_READ | PROT_WRITE, flags, file_descriptor_, 0));
      if (!data_ || data_ == MAP_FAILED) {
        data_ = nullptr;
        return false;
      }

      return true;
    }

    uint8_t* Allocate(size_t length) {
      if (capacity_ - offset_ < length) {
        return nullptr;
      }
      uint8_t* result = data_ + offset_;
      offset_ += length;
      return result;
    }

    void Flush() {
      if (offset_) {
        msync(data_, offset_, MS_ASYNC);
      }
    }

    void FlushNew() {
      if (offset_ > last_flush_offset_) {
        msync(data_ + last_flush_offset_, offset_ - last_flush_offset_, MS_ASYNC);
        last_flush_offset_ = offset_;
      }
    }

   private:
    int file_descriptor_;
    uint8_t* data_;
    size_t offset_;
    size_t capacity_;
    size_t last_flush_offset_;
  };

  std::mutex mutex_;
  std::vector<std::unique_ptr<Chunk>> chunks_;
};

std::unique_ptr<ChunkedMappedMemoryWriter> ChunkedMappedMemoryWriter::Open(
    const std::filesystem::path& path, size_t chunk_size, bool low_address_space) {
  size_t aligned_chunk_size = rex::round_up(chunk_size, memory::allocation_granularity());
  return std::make_unique<PosixChunkedMappedMemoryWriter>(path, aligned_chunk_size,
                                                          low_address_space);
}

}  // namespace rex::memory
