/**
 * @file        memory/virtual_memory.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/memory.h and xenia/base/mapped_memory.h (Ben
 *              Vanik, 2020).
 */

#pragma once

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string_view>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::memory {

#if REX_PLATFORM_ANDROID
/** Loads the Android shared memory functions available on API level 26 and later. */
REX_API void AndroidInitialize();
/** Unloads the functions AndroidInitialize loaded. */
REX_API void AndroidShutdown();
#endif

/**
 * @return Page size in bytes, usually 4 KiB.
 */
REX_API size_t GetPageSize();

/**
 * @return Granularity in bytes. 64 KiB on Windows, the page size on POSIX.
 */
REX_API size_t GetAllocationGranularity();

enum class PageAccess : u8 {
  kNoAccess = 0,
  kReadOnly = 1 << 0,
  kReadWrite = kReadOnly | 1 << 1,
  kExecuteReadOnly = kReadOnly | 1 << 2,
  kExecuteReadWrite = kReadWrite | 1 << 2,
};

/** Allocation mode AllocFixed uses. */
enum class AllocationType : u8 {
  kReserve = 1 << 0, /**< Reserve address space without backing it. */
  kCommit = 1 << 1,  /**< Back reserved address space with memory. */
  kReserveCommit = kReserve | kCommit,
};

/** Deallocation mode DeallocFixed uses. */
enum class DeallocationType : u8 {
  kRelease = 1 << 0,  /**< Return the address space to the system. */
  kDecommit = 1 << 1, /**< Release the backing memory and keep the reservation. */
};

/**
 * Reports whether host pages can use PageAccess::kExecuteReadWrite.
 *
 * @return True when writable executable pages are supported.
 */
REX_API bool IsWritableExecutableMemorySupported();

/**
 * Allocates host pages at a page aligned address.
 *
 * @param base_address Requested address, or null to let the system choose.
 * @param allocation_type Reserve, commit or both.
 * @param access Access rights of the new pages.
 * @return Address of the allocation, or null when the range is not available.
 */
REX_API void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                         PageAccess access);

/**
 * Decommits or releases host pages.
 *
 * @param length Byte count. Windows ignores it for kRelease and releases the whole region.
 * @param deallocation_type Decommit or release.
 */
REX_API bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type);

/**
 * Changes the access rights of host pages.
 *
 * @param out_old_access Receives the previous access rights of the first page, or null.
 */
REX_API bool Protect(void* base_address, size_t length, PageAccess access,
                     PageAccess* out_old_access = nullptr);

/**
 * Returns the access rights of the region that holds an address.
 *
 * @param base_address Address inside the region.
 * @param length Receives the byte count of consecutive pages with the same access rights.
 * @param out_access Receives the access rights.
 */
REX_API bool QueryProtect(void* base_address, size_t& length, PageAccess& out_access);

/**
 * Allocates aligned host memory for one object.
 *
 * @param alignment Alignment in bytes, a power of 2.
 * @return Uninitialized storage for a T, or null on failure.
 */
template <typename T>
inline T* AlignedAlloc(size_t alignment) {
#if REX_PLATFORM_WIN32
  return reinterpret_cast<T*>(_aligned_malloc(sizeof(T), alignment));
#else
  void* ptr = nullptr;
  if (posix_memalign(&ptr, alignment, sizeof(T))) {
    return nullptr;
  }
  return reinterpret_cast<T*>(ptr);
#endif
}

/**
 * Frees memory AlignedAlloc returned.
 *
 * @param ptr Pointer AlignedAlloc returned.
 */
template <typename T>
void AlignedFree(T* ptr) {
#if REX_PLATFORM_WIN32
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

#if REX_PLATFORM_WIN32
/** Stands for a Windows HANDLE. */
using FileMappingHandle = void*;
/** Value CreateFileMappingHandle returns on failure. */
constexpr FileMappingHandle kFileMappingHandleInvalid = nullptr;
#else
/** File mapping handle, a file descriptor. */
using FileMappingHandle = int;
/** Value CreateFileMappingHandle returns on failure. */
constexpr FileMappingHandle kFileMappingHandleInvalid = -1;
#endif

/**
 * Creates a named shared memory object.
 *
 * @param access Access rights of views of the object.
 * @param commit True to commit the whole object up front. Only Windows reads it.
 * @return Handle to the object, or kFileMappingHandleInvalid on failure or when an object with
 *         that name already exists.
 */
REX_API FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                                  PageAccess access, bool commit);

/** Builds a unique name from the prefix, process ID, underscore and random hex. */
REX_API std::filesystem::path CreateFileMappingName(std::string_view prefix);

/**
 * Removes shared memory objects whose name starts with the prefix, a dead process ID and an
 * underscore. Only Linux removes any.
 *
 * @param prefix Leading part of the names to inspect.
 */
REX_API void RemoveStaleFileMappings(std::string_view prefix);

/**
 * Closes a handle CreateFileMappingHandle returned. POSIX unlinks the object.
 *
 * @param path Name the object was created with.
 */
REX_API void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path);

/**
 * Maps a view of a shared memory object.
 *
 * @param handle Handle CreateFileMappingHandle returned.
 * @param base_address Requested address, or null to let the system choose.
 * @param file_offset Offset of the view in the object, page aligned.
 * @return Address of the view, or null on failure.
 */
REX_API void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                          PageAccess access, size_t file_offset);

/** Unmaps a view MapFileView returned. */
REX_API bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length);

/**
 * Truncates a host pointer to its low 32 bits.
 *
 * @return The pointer with the upper 32 bits cleared.
 */
inline void* LowAddress(void* address) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr): LowAddress truncates a host pointer
  return reinterpret_cast<void*>(reinterpret_cast<u64>(address) & 0xFFFFFFFF);
}

}  // namespace rex::memory

namespace rex::memory {

/** A file, or a range of a file, mapped into host memory. */
class REX_API MappedMemory : public rex::NonCopyable {
 public:
  enum class Mode : u8 {
    kRead,
    kReadWrite,
  };

  /**
   * Maps an existing file.
   *
   * @param offset Byte offset of the mapped range in the file.
   * @param length Byte count of the range, or 0 to map to the end of the file.
   * @return The mapping, or null on failure.
   */
  static std::unique_ptr<MappedMemory> Open(const std::filesystem::path& path, Mode mode,
                                            size_t offset = 0, size_t length = 0);

  MappedMemory() = default;

  /** Wraps memory the object does not own. */
  MappedMemory(void* data, size_t size) : data_(data), size_(size) {}
  virtual ~MappedMemory() = default;

  /**
   * Returns a view of part of this mapping, valid while it stays open.
   *
   * @param offset Byte offset of the view in this mapping.
   */
  std::unique_ptr<MappedMemory> Range(size_t offset, size_t length) const {
    return std::make_unique<MappedMemory>(GetData() + offset, length);
  }

  /** Returns the start of the mapped memory. It is null once closed. */
  u8* GetData() const { return reinterpret_cast<u8*>(data_); }

  size_t GetSize() const { return size_; }

  /**
   * Unmaps the memory and closes the file.
   *
   * @param truncate_size Size to truncate the file to, or 0 to keep its size.
   */
  virtual void Close(u64 truncate_size = 0) { (void)truncate_size; }

  virtual void Flush() {}

  /**
   * Maps a different range of the file.
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
  void* data_ = nullptr;
  size_t size_ = 0;
};

/** Append only writer over fixed size mapped files named path.N. */
class REX_API ChunkedMappedMemoryWriter {
 public:
  virtual ~ChunkedMappedMemoryWriter() = default;

  /**
   * Creates a writer.
   *
   * @param chunk_size Byte size of each chunk, rounded up to the allocation granularity.
   * @param low_address_space True to map chunks below 4 GiB, which Allocate refuses on macOS.
   */
  static std::unique_ptr<ChunkedMappedMemoryWriter> Open(const std::filesystem::path& path,
                                                         size_t chunk_size,
                                                         bool low_address_space = false);

  /**
   * Reserves space in the current chunk, opening a new chunk when it is full.
   *
   * @param length Byte count, no larger than the chunk size.
   * @return Pointer to the reserved space, or null on failure.
   */
  virtual u8* Allocate(size_t length) = 0;

  virtual void Flush() = 0;

  /** Writes the data added since the last FlushNew back to the files. */
  virtual void FlushNew() = 0;

 protected:
  /**
   * Stores the writer settings.
   *
   * @param low_address_space True to map chunks below 4 GiB.
   */
  ChunkedMappedMemoryWriter(const std::filesystem::path& path, size_t chunk_size,
                            bool low_address_space)
      : path_(path), chunk_size_(chunk_size), low_address_space_(low_address_space) {}

  std::filesystem::path path_;
  size_t chunk_size_;
  bool low_address_space_;
};

}  // namespace rex::memory
