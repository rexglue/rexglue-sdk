/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "lzx.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cstdlib>
#include <cstring>

#include <rex/filesystem/xex_types.h>

#include <lzx.h>
#include <mspack.h>

namespace rex::filesystem::detail {

namespace {

struct MemoryFile {
  mspack_system sys;
  u8* buffer;
  off_t buffer_size;
  off_t offset;
};

MemoryFile* OpenMemoryFile(void* buffer, size_t buffer_size) {
  if (buffer_size >= INT_MAX) {
    return nullptr;
  }
  auto* file = static_cast<MemoryFile*>(std::calloc(1, sizeof(MemoryFile)));
  if (!file) {
    return nullptr;
  }
  file->buffer = static_cast<u8*>(buffer);
  file->buffer_size = static_cast<off_t>(buffer_size);
  return file;
}

int ReadMemoryFile(mspack_file* handle, void* buffer, int bytes) {
  auto* file = reinterpret_cast<MemoryFile*>(handle);
  const off_t total = std::min(static_cast<off_t>(bytes), file->buffer_size - file->offset);
  std::memcpy(buffer, file->buffer + file->offset, total);
  file->offset += total;
  return static_cast<int>(total);
}

int WriteMemoryFile(mspack_file* handle, void* buffer, int bytes) {
  auto* file = reinterpret_cast<MemoryFile*>(handle);
  const off_t total = std::min(static_cast<off_t>(bytes), file->buffer_size - file->offset);
  std::memcpy(file->buffer + file->offset, buffer, total);
  file->offset += total;
  return static_cast<int>(total);
}

void* AllocMemory(mspack_system*, size_t bytes) {
  return std::calloc(bytes, 1);
}

void FreeMemory(void* ptr) {
  std::free(ptr);
}

void CopyBytes(void* src, void* dest, size_t chars) {
  std::memcpy(dest, src, chars);
}

}  // namespace

int LZXDecompress(std::span<const u8> input, std::span<u8> output, u32 window_size,
                  std::span<const u8> window_data) {
  if (!std::has_single_bit(window_size) || window_data.size() > window_size) {
    return 1;
  }
  const int window_bits = std::countr_zero(window_size);

  mspack_system sys{};
  sys.read = ReadMemoryFile;
  sys.write = WriteMemoryFile;
  sys.alloc = AllocMemory;
  sys.free = FreeMemory;
  sys.copy = CopyBytes;

  MemoryFile* source = OpenMemoryFile(const_cast<u8*>(input.data()), input.size());
  MemoryFile* dest = OpenMemoryFile(output.data(), output.size());
  int result = 1;
  if (source && dest) {
    lzxd_stream* lzxd = lzxd_init(&sys, reinterpret_cast<mspack_file*>(source),
                                  reinterpret_cast<mspack_file*>(dest), window_bits, 0, 0x8000,
                                  static_cast<off_t>(output.size()), 0);
    if (lzxd) {
      if (!window_data.empty()) {
        const size_t padding = window_size - window_data.size();
        std::memset(&lzxd->window[0], 0, padding);
        std::memcpy(&lzxd->window[padding], window_data.data(), window_data.size());
        lzxd->ref_data_size = window_size;
      }
      result = lzxd_decompress(lzxd, static_cast<off_t>(output.size()));
      lzxd_free(lzxd);
    }
  }
  std::free(source);
  std::free(dest);
  return result;
}

int LZXApplyDelta(std::span<const u8> patch, u32 window_size, std::span<u8> image) {
  constexpr size_t kPatchHeaderSize = 12;
  size_t offset = 0;
  while (offset + kPatchHeaderSize <= patch.size()) {
    const auto* entry = reinterpret_cast<const XEXDeltaPatch*>(patch.data() + offset);
    const u32 old_addr = entry->old_addr;
    const u32 new_addr = entry->new_addr;
    const u32 length = entry->uncompressed_len;
    const u32 compressed = entry->compressed_len;
    if (!old_addr && !new_addr && !length && !compressed) {
      break;
    }
    if (u64{new_addr} + length > image.size()) {
      return 1;
    }
    size_t entry_size = kPatchHeaderSize;
    switch (compressed) {
      case 0:
        std::memset(image.data() + new_addr, 0, length);
        break;
      case 1:
        if (u64{old_addr} + length > image.size()) {
          return 1;
        }
        std::memmove(image.data() + new_addr, image.data() + old_addr, length);
        break;
      default: {
        if (offset + kPatchHeaderSize + compressed > patch.size() ||
            u64{old_addr} + length > image.size()) {
          return 1;
        }
        const int result = LZXDecompress(patch.subspan(offset + kPatchHeaderSize, compressed),
                                         image.subspan(new_addr, length), window_size,
                                         image.subspan(old_addr, length));
        if (result) {
          return result;
        }
        entry_size += compressed;
      } break;
    }
    offset += entry_size;
  }
  return 0;
}

}  // namespace rex::filesystem::detail
