/**
 * @file        memory/utils.h
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

#include <bit>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

#include <rex/macros.h>
#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/types.h>

namespace rex {
namespace memory {

/**
 * Restrict qualifier for variable declarations. Not for return values or the this pointer.
 *
 * @code
 * void Copy(u8* REX_RESTRICT_VAR dst, const u8* REX_RESTRICT_VAR src);
 * @endcode
 */
#define REX_RESTRICT_VAR __restrict

/**
 * Copies the bits of 1 trivially copyable object into another of the same size.
 *
 * @tparam Dst Destination type.
 * @tparam Src Source type, the same size as Dst.
 * @param dst Object that receives the bits.
 * @param src Object the bits come from.
 */
template <typename Dst, typename Src>
void Reinterpret(Dst& REX_RESTRICT_VAR dst, const Src& REX_RESTRICT_VAR src) {
  static_assert(sizeof(Dst) == sizeof(Src));
  static_assert(std::is_trivially_copyable_v<Dst>);
  static_assert(std::is_trivially_copyable_v<Src>);
  dst = std::bit_cast<Dst>(src);
}

/**
 * Returns the bits of a trivially copyable object as another type of the same size.
 *
 * @tparam Dst Result type.
 * @tparam Src Source type, the same size as Dst.
 * @param src Object the bits come from.
 * @return A Dst holding the bits of src.
 */
template <typename Dst, typename Src>
Dst Reinterpret(const Src& REX_RESTRICT_VAR src) {
  Dst dst;
  Reinterpret(dst, src);
  return dst;
}

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
enum class PageAccess {
  kNoAccess = 0,                           /**< No access. */
  kReadOnly = 1 << 0,                      /**< Read only. */
  kReadWrite = kReadOnly | 1 << 1,         /**< Read and write. */
  kExecuteReadOnly = kReadOnly | 1 << 2,   /**< Read and execute. */
  kExecuteReadWrite = kReadWrite | 1 << 2, /**< Read, write and execute. */
};

/** Allocation mode AllocFixed uses. */
enum class AllocationType {
  kReserve = 1 << 0,                   /**< Reserve address space without backing it. */
  kCommit = 1 << 1,                    /**< Back reserved address space with memory. */
  kReserveCommit = kReserve | kCommit, /**< Reserve and commit in 1 step. */
};

/** Deallocation mode DeallocFixed uses. */
enum class DeallocationType {
  kRelease = 1 << 0,  /**< Return the address space to the system. */
  kDecommit = 1 << 1, /**< Drop the backing memory and keep the reservation. */
};

/**
 * Tests whether host pages can be allocated or mapped with PageAccess::kExecuteReadWrite.
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

/** Opaque file mapping handle. Holds a HANDLE on Windows and a file descriptor on POSIX. */
using FileMappingHandle = intptr_t;
/** Value CreateFileMappingHandle returns on failure. */
constexpr FileMappingHandle kFileMappingHandleInvalid = -1;

/**
 * Creates a named shared memory object.
 *
 * @param path Name of the object.
 * @param length Size in bytes.
 * @param access Access rights of views of the object.
 * @param commit True to commit the whole object up front. Only Windows reads it.
 * @return Handle to the object, or kFileMappingHandleInvalid on failure.
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
 * Ends the hash_combine recursion.
 *
 * @param hash Hash value so far.
 * @return The hash value unchanged.
 */
inline size_t hash_combine(size_t hash) {
  return hash;
}

/**
 * Mixes the std::hash of each value into a hash value.
 *
 * @tparam T Type of the first value.
 * @tparam Ts Types of the remaining values.
 * @param hash Hash value to mix into.
 * @param v First value.
 * @param vs Remaining values.
 * @return The combined hash value.
 */
template <typename T, typename... Ts>
size_t hash_combine(size_t hash, const T& v, const Ts&... vs) {
  std::hash<T> hasher;
  hash ^= hasher(v) + 0x9E3779B9 + (hash << 6) + (hash >> 2);
  return hash_combine(hash, vs...);
}

/**
 * Truncates a host pointer to its low 32 bits.
 *
 * @param address Pointer to truncate.
 * @return The pointer with the upper 32 bits cleared.
 */
inline void* low_address(void* address) {
  return reinterpret_cast<void*>(reinterpret_cast<u64>(address) & 0xFFFFFFFF);
}

/**
 * Copies 16 byte blocks between 16 byte aligned buffers.
 *
 * @param dest Destination buffer.
 * @param src Source buffer.
 * @param count Number of 16 byte blocks.
 */
REX_API void copy_128_aligned(void* dest, const void* src, size_t count);

/**
 * Copies 16 bit values and swaps the byte order of each. Both buffers are 16 byte aligned.
 *
 * @param dest Destination buffer.
 * @param src Source buffer.
 * @param count Number of 16 bit values.
 */
REX_API void copy_and_swap_16_aligned(void* dest, const void* src, size_t count);

/**
 * Copies 16 bit values and swaps the byte order of each.
 *
 * @param dest Destination buffer.
 * @param src Source buffer.
 * @param count Number of 16 bit values.
 */
REX_API void copy_and_swap_16_unaligned(void* dest, const void* src, size_t count);

/**
 * Copies 32 bit values and swaps the byte order of each. Both buffers are 16 byte aligned.
 *
 * @param dest Destination buffer.
 * @param src Source buffer.
 * @param count Number of 32 bit values.
 */
REX_API void copy_and_swap_32_aligned(void* dest, const void* src, size_t count);

/**
 * Copies 32 bit values and swaps the byte order of each.
 *
 * @param dest Destination buffer.
 * @param src Source buffer.
 * @param count Number of 32 bit values.
 */
REX_API void copy_and_swap_32_unaligned(void* dest, const void* src, size_t count);

/**
 * Copies 64 bit values and swaps the byte order of each. Both buffers are 16 byte aligned.
 *
 * @param dest Destination buffer.
 * @param src Source buffer.
 * @param count Number of 64 bit values.
 */
REX_API void copy_and_swap_64_aligned(void* dest, const void* src, size_t count);

/**
 * Copies 64 bit values and swaps the byte order of each.
 *
 * @param dest Destination buffer.
 * @param src Source buffer.
 * @param count Number of 64 bit values.
 */
REX_API void copy_and_swap_64_unaligned(void* dest, const void* src, size_t count);

/**
 * Copies 32 bit values and swaps the 2 16 bit halves of each. Both buffers are 16 byte aligned.
 *
 * @param dest Destination buffer.
 * @param src Source buffer.
 * @param count Number of 32 bit values.
 */
REX_API void copy_and_swap_16_in_32_aligned(void* dest, const void* src, size_t count);

/**
 * Copies 32 bit values and swaps the 2 16 bit halves of each.
 *
 * @param dest Destination buffer.
 * @param src Source buffer.
 * @param count Number of 32 bit values.
 */
REX_API void copy_and_swap_16_in_32_unaligned(void* dest, const void* src, size_t count);

/**
 * Copies values and swaps the byte order of each. Takes the aligned path when both buffers are
 * 32 byte aligned.
 *
 * @tparam T Element type, 1, 2, 4 or 8 bytes.
 * @param dest Destination buffer.
 * @param src Source buffer.
 * @param count Number of elements.
 */
template <typename T>
void copy_and_swap(T* dest, const T* src, size_t count) {
  bool is_aligned =
      reinterpret_cast<uintptr_t>(dest) % 32 == 0 && reinterpret_cast<uintptr_t>(src) % 32 == 0;
  if (sizeof(T) == 1) {
    std::memcpy(dest, src, count);
  } else if (sizeof(T) == 2) {
    auto ps = reinterpret_cast<const u16*>(src);
    auto pd = reinterpret_cast<u16*>(dest);
    if (is_aligned) {
      copy_and_swap_16_aligned(pd, ps, count);
    } else {
      copy_and_swap_16_unaligned(pd, ps, count);
    }
  } else if (sizeof(T) == 4) {
    auto ps = reinterpret_cast<const u32*>(src);
    auto pd = reinterpret_cast<u32*>(dest);
    if (is_aligned) {
      copy_and_swap_32_aligned(pd, ps, count);
    } else {
      copy_and_swap_32_unaligned(pd, ps, count);
    }
  } else if (sizeof(T) == 8) {
    auto ps = reinterpret_cast<const u64*>(src);
    auto pd = reinterpret_cast<u64*>(dest);
    if (is_aligned) {
      copy_and_swap_64_aligned(pd, ps, count);
    } else {
      copy_and_swap_64_unaligned(pd, ps, count);
    }
  } else {
    REX_ASSERT_ALWAYS("Invalid memory::copy_and_swap size");
  }
}

/**
 * Loads a value from memory of any alignment.
 *
 * @tparam T Trivially copyable value type.
 * @param mem Address to read.
 * @return The value at mem.
 */
template <typename T>
inline T load(const void* mem) {
  static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
  T result;
  std::memcpy(&result, mem, sizeof(T));
  return result;
}

/**
 * Loads a value from memory and swaps its byte order.
 *
 * @tparam T Value type, 1, 2, 4 or 8 bytes.
 * @param mem Address to read.
 * @return The byte swapped value at mem.
 */
template <typename T>
inline T load_and_swap(const void* mem) {
  return byte_swap(load<T>(mem));
}

/**
 * Loads a NUL terminated 8 bit string.
 *
 * @param mem Address of the first character.
 * @return The characters before the terminator.
 */
template <>
inline std::string load_and_swap<std::string>(const void* mem) {
  std::string value;
  for (i32 i = 0;; ++i) {
    auto c = load_and_swap<u8>(reinterpret_cast<const u8*>(mem) + i);
    if (!c) {
      break;
    }
    value.push_back(static_cast<char>(c));
  }
  return value;
}

/**
 * Loads a NUL terminated big endian UTF-16 string.
 *
 * @param mem Address of the first code unit.
 * @return The code units before the terminator in host byte order.
 */
template <>
inline std::u16string load_and_swap<std::u16string>(const void* mem) {
  std::u16string value;
  for (i32 i = 0;; ++i) {
    auto c = load_and_swap<u16>(reinterpret_cast<const u16*>(mem) + i);
    if (!c) {
      break;
    }
    value.push_back(static_cast<wchar_t>(c));
  }
  return value;
}

/**
 * Stores a value to memory of any alignment.
 *
 * @tparam T Trivially copyable value type.
 * @param mem Address to write.
 * @param value Value to write.
 */
template <typename T>
inline void store(void* mem, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
  std::memcpy(mem, &value, sizeof(T));
}

/**
 * Swaps the byte order of a value and stores it to memory.
 *
 * @tparam T Value type, 1, 2, 4 or 8 bytes.
 * @param mem Address to write.
 * @param value Value to write.
 */
template <typename T>
inline void store_and_swap(void* mem, const T& value) {
  store(mem, byte_swap(value));
}

/**
 * Stores the characters of an 8 bit string without a terminator.
 *
 * @param mem Address of the first character.
 * @param value Characters to write.
 */
template <>
inline void store_and_swap<std::string_view>(void* mem, const std::string_view& value) {
  for (size_t i = 0; i < value.size(); ++i) {
    store_and_swap<u8>(reinterpret_cast<u8*>(mem) + i, value[i]);
  }
}

/**
 * Stores the characters of an 8 bit string without a terminator.
 *
 * @param mem Address of the first character.
 * @param value Characters to write.
 */
template <>
inline void store_and_swap<std::string>(void* mem, const std::string& value) {
  return store_and_swap<std::string_view>(mem, value);
}

/**
 * Stores a UTF-16 string as big endian code units without a terminator.
 *
 * @param mem Address of the first code unit.
 * @param value Code units to write.
 */
template <>
inline void store_and_swap<std::u16string_view>(void* mem, const std::u16string_view& value) {
  for (size_t i = 0; i < value.size(); ++i) {
    store_and_swap<u16>(reinterpret_cast<u16*>(mem) + i, value[i]);
  }
}

/**
 * Stores a UTF-16 string as big endian code units without a terminator.
 *
 * @param mem Address of the first code unit.
 * @param value Code units to write.
 */
template <>
inline void store_and_swap<std::u16string>(void* mem, const std::u16string& value) {
  return store_and_swap<std::u16string_view>(mem, value);
}

/** 4 character code packed into 32 bits. */
using fourcc_t = u32;

/**
 * Packs 4 characters into a FourCC in host byte order.
 *
 * @param a First character, the most significant byte.
 * @param b Second character.
 * @param c Third character.
 * @param d Fourth character, the least significant byte.
 * @return The packed code. make_fourcc('a', 'b', 'c', 'd') is 0x61626364.
 */
constexpr inline fourcc_t make_fourcc(char a, char b, char c, char d) {
  return fourcc_t((static_cast<fourcc_t>(a) << 24) | (static_cast<fourcc_t>(b) << 16) |
                  (static_cast<fourcc_t>(c) << 8) | static_cast<fourcc_t>(d));
}

/**
 * Packs a 4 character string into a FourCC in host byte order.
 *
 * @param fourcc String of exactly 4 characters. Any other length throws std::runtime_error.
 * @return The packed code. make_fourcc("abcd") is 0x61626364.
 */
constexpr inline fourcc_t make_fourcc(const std::string_view fourcc) {
  if (fourcc.length() != 4) {
    throw std::runtime_error("Invalid fourcc length");
  }
  return make_fourcc(fourcc[0], fourcc[1], fourcc[2], fourcc[3]);
}

}  // namespace memory
}  // namespace rex
