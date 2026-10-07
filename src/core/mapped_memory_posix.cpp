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

#include <memory>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/filesystem.h>
#include <rex/memory/mapped_memory.h>
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

std::unique_ptr<ChunkedMappedMemoryWriter> ChunkedMappedMemoryWriter::Open(
    const std::filesystem::path& path, size_t chunk_size, bool low_address_space) {
  // TODO: Implement if needed
  return nullptr;
}

}  // namespace rex::memory
