/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay & Rien Gupta, 2026 - Adapted for ReXGlue runtime (POSIX + macOS)
 */

#include <format>
#include <memory>
#include <mutex>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/mapped_memory.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>

// macOS off_t is 64-bit and provides no *64 large-file variants; the base
// names already are the large-file interfaces. Linux still ships legacy 32-bit
// off_t on some distributions, so keep the explicit *64 forms there.
#if defined(__APPLE__)
using rex_stat64_t = struct stat;
using rex_off64_t = off_t;
#define rex_fstat64 fstat
#define rex_ftruncate64 ftruncate
#else
using rex_stat64_t = struct stat64;
using rex_off64_t = off64_t;
#define rex_fstat64 fstat64
#define rex_ftruncate64 ftruncate64
#endif

namespace rex::memory {

class PosixMappedMemory : public MappedMemory {
 public:
  PosixMappedMemory(void* data, size_t size, int file_descriptor)
      : MappedMemory(data, size), file_descriptor_(file_descriptor) {}

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
    int protection = 0;
    switch (mode) {
      case Mode::kRead:
        protection |= PROT_READ;
        break;
      case Mode::kReadWrite:
        protection |= PROT_READ | PROT_WRITE;
        break;
    }

    size_t map_length = length;
    if (!length) {
      rex_stat64_t file_stat;
      if (rex_fstat64(file_descriptor, &file_stat)) {
        close(file_descriptor);
        return nullptr;
      }
      map_length = static_cast<size_t>(file_stat.st_size);
    }

    void* data = mmap(0, map_length, protection, MAP_SHARED, file_descriptor, offset);
    if (!data || data == MAP_FAILED) {
      close(file_descriptor);
      return nullptr;
    }

    return std::make_unique<PosixMappedMemory>(data, map_length, file_descriptor);
  }

  void Close(uint64_t truncate_size) override {
    if (data_) {
      munmap(data_, size());
      data_ = nullptr;
    }
    if (file_descriptor_ >= 0) {
      if (truncate_size) {
        rex_ftruncate64(file_descriptor_, rex_off64_t(truncate_size));
      }
      close(file_descriptor_);
      file_descriptor_ = -1;
    }
  }

  void Flush() override { msync(data(), size(), MS_ASYNC); }

 private:
  int file_descriptor_;
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

class PosixChunkedMappedMemoryWriter : public ChunkedMappedMemoryWriter {
 public:
  PosixChunkedMappedMemoryWriter(const std::filesystem::path& path, size_t chunk_size,
                                 bool low_address_space)
      : ChunkedMappedMemoryWriter(path, chunk_size, low_address_space) {}

  ~PosixChunkedMappedMemoryWriter() override {
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();
  }

  u8* Allocate(size_t length) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!chunks_.empty()) {
      if (u8* result = chunks_.back()->Allocate(length)) {
        return result;
      }
    }
    auto chunk = std::make_unique<Chunk>(chunk_size_);
    auto chunk_path = path_;
    chunk_path.replace_extension(std::format(".{}", chunks_.size()));
    if (!chunk->Open(chunk_path, low_address_space_)) {
      return nullptr;
    }
    u8* result = chunk->Allocate(length);
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
    explicit Chunk(size_t capacity) : capacity_(capacity) {}

    ~Chunk() {
      if (data_) {
        munmap(data_, capacity_);
      }
      if (file_descriptor_ >= 0) {
        close(file_descriptor_);
      }
    }

    bool Open(const std::filesystem::path& path, bool low_address_space) {
      file_descriptor_ = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
      if (file_descriptor_ < 0) {
        return false;
      }
      if (rex_ftruncate64(file_descriptor_, static_cast<rex_off64_t>(capacity_)) != 0) {
        return false;
      }
      if (!low_address_space) {
        void* data =
            mmap(nullptr, capacity_, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor_, 0);
        if (data == MAP_FAILED) {
          return false;
        }
        data_ = static_cast<u8*>(data);
        return true;
      }
      u8* hint = reinterpret_cast<u8*>(kLowAddressBase);
      for (u32 attempt = 0; attempt < kLowAddressAttempts; ++attempt, hint += capacity_) {
        void* data =
            mmap(hint, capacity_, PROT_READ | PROT_WRITE, kLowAddressFlags, file_descriptor_, 0);
        if (data == hint) {
          data_ = hint;
          return true;
        }
        if (data != MAP_FAILED) {
          munmap(data, capacity_);
        }
      }
      REXLOG_ERROR(core, "Unable to find space for mapping");
      return false;
    }

    u8* Allocate(size_t length) {
      if (capacity_ - offset_ < length) {
        return nullptr;
      }
      u8* result = data_ + offset_;
      offset_ += length;
      return result;
    }

    void Flush() { msync(data_, offset_, MS_ASYNC); }

    void FlushNew() {
      size_t start = last_flush_offset_ & ~(page_size() - 1);
      msync(data_ + start, offset_ - start, MS_ASYNC);
      last_flush_offset_ = offset_;
    }

   private:
    static constexpr uintptr_t kLowAddressBase = 0x10000000;
    static constexpr u32 kLowAddressAttempts = 1000;
#if defined(MAP_FIXED_NOREPLACE)
    static constexpr int kLowAddressFlags = MAP_SHARED | MAP_FIXED_NOREPLACE;
#else
    static constexpr int kLowAddressFlags = MAP_SHARED;
#endif

    int file_descriptor_ = -1;
    u8* data_ = nullptr;
    size_t offset_ = 0;
    size_t capacity_;
    size_t last_flush_offset_ = 0;
  };

  std::mutex mutex_;
  std::vector<std::unique_ptr<Chunk>> chunks_;
};

std::unique_ptr<ChunkedMappedMemoryWriter> ChunkedMappedMemoryWriter::Open(
    const std::filesystem::path& path, size_t chunk_size, bool low_address_space) {
  return std::make_unique<PosixChunkedMappedMemoryWriter>(
      path, rex::round_up(chunk_size, allocation_granularity()), low_address_space);
}

}  // namespace rex::memory
