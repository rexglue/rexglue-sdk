/**
 * @file        memory/virtual_memory.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/memory.h (Ben Vanik, 2020).
 */

#pragma once

#include <cstdlib>
#include <filesystem>

#include <rex/macros.h>
#include <rex/platform.h>
#include <rex/types.h>

namespace rex::memory {

#if REX_PLATFORM_ANDROID
/** Loads the Android shared memory functions available on API level 26 and later. */
REX_API void AndroidInitialize();
/** Unloads the functions AndroidInitialize loaded. */
REX_API void AndroidShutdown();
#endif

/**
 * Returns the host page size.
 *
 * @return Page size in bytes, usually 4 KiB.
 */
REX_API size_t page_size();

/**
 * Returns the host allocation granularity.
 *
 * @return Granularity in bytes. 64 KiB on Windows, the page size on POSIX.
 */
REX_API size_t allocation_granularity();

/** Access rights of a range of host pages. */
enum class PageAccess : u8 {
  kNoAccess = 0,                           /**< No access. */
  kReadOnly = 1 << 0,                      /**< Read only. */
  kReadWrite = kReadOnly | 1 << 1,         /**< Read and write. */
  kExecuteReadOnly = kReadOnly | 1 << 2,   /**< Read and execute. */
  kExecuteReadWrite = kReadWrite | 1 << 2, /**< Read, write and execute. */
};

/** Allocation mode AllocFixed uses. */
enum class AllocationType : u8 {
  kReserve = 1 << 0,                   /**< Reserve address space without backing it. */
  kCommit = 1 << 1,                    /**< Back reserved address space with memory. */
  kReserveCommit = kReserve | kCommit, /**< Reserve and commit in 1 step. */
};

/** Deallocation mode DeallocFixed uses. */
enum class DeallocationType : u8 {
  kRelease = 1 << 0,  /**< Return the address space to the system. */
  kDecommit = 1 << 1, /**< Drop the backing memory and keep the reservation. */
};

/**
 * Reports whether host pages can be allocated or mapped with PageAccess::kExecuteReadWrite.
 * When they cannot, writes to executable pages go through a separate view of the same file
 * mapping.
 *
 * @return True when writable executable pages are supported.
 */
REX_API bool IsWritableExecutableMemorySupported();

/**
 * Allocates host pages at a page aligned address.
 *
 * @param base_address Requested address, or nullptr to let the system choose.
 * @param length Byte count.
 * @param allocation_type Reserve, commit or both.
 * @param access Access rights of the new pages.
 * @return Address of the allocation, or nullptr when the range is not available.
 */
REX_API void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                         PageAccess access);

/**
 * Decommits or releases host pages.
 *
 * @param base_address Start of the range.
 * @param length Byte count. Windows ignores it for kRelease and releases the whole region.
 * @param deallocation_type Decommit or release.
 * @return True on success.
 */
REX_API bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type);

/**
 * Changes the access rights of host pages.
 *
 * @param base_address Start of the range.
 * @param length Byte count.
 * @param access New access rights.
 * @param out_old_access Receives the previous access rights of the first page, or nullptr.
 * @return True on success.
 */
REX_API bool Protect(void* base_address, size_t length, PageAccess access,
                     PageAccess* out_old_access = nullptr);

/**
 * Returns the access rights of the region that holds an address.
 *
 * @param base_address Address inside the region.
 * @param length Receives the byte count of consecutive pages with the same access rights.
 * @param access_out Receives the access rights.
 * @return True on success.
 */
REX_API bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out);

/**
 * Allocates aligned host memory for 1 object. Free it with AlignedFree.
 *
 * @tparam T Object type.
 * @param alignment Alignment in bytes, a power of 2.
 * @return Uninitialized storage for a T, or nullptr on failure.
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
 * @tparam T Object type.
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
/** File mapping handle, a HANDLE. */
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
 * @param path Name of the object.
 * @param length Size in bytes.
 * @param access Access rights of views of the object.
 * @param commit True to commit the whole object up front. Only Windows reads it.
 * @return Handle to the object, or kFileMappingHandleInvalid on failure or when an object with
 *         that name already exists.
 */
REX_API FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                                  PageAccess access, bool commit);

/**
 * Closes a handle CreateFileMappingHandle returned. POSIX also unlinks the object.
 *
 * @param handle Handle to close.
 * @param path Name the object was created with.
 */
REX_API void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path);

/**
 * Maps a view of a shared memory object.
 *
 * @param handle Handle CreateFileMappingHandle returned.
 * @param base_address Requested address, or nullptr to let the system choose.
 * @param length Byte count of the view.
 * @param access Access rights of the view.
 * @param file_offset Offset of the view in the object, page aligned.
 * @return Address of the view, or nullptr on failure.
 */
REX_API void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                          PageAccess access, size_t file_offset);

/**
 * Unmaps a view MapFileView returned.
 *
 * @param handle Handle the view was mapped from.
 * @param base_address Address of the view.
 * @param length Byte count of the view.
 * @return True on success.
 */
REX_API bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length);

/**
 * Truncates a host pointer to its low 32 bits.
 *
 * @param address Pointer to truncate.
 * @return The pointer with the upper 32 bits cleared.
 */
inline void* low_address(void* address) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr): the contract truncates a host pointer
  return reinterpret_cast<void*>(reinterpret_cast<u64>(address) & 0xFFFFFFFF);
}

}  // namespace rex::memory
